import concurrent.futures
from collections import Counter
from contextlib import contextmanager
import http.server
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
COUNTS = Counter()
LOCK = threading.Lock()
WORKERS = 4
# NUL bytes and an embedded blank line: the cache stores one NUL-terminated
# blob and re-splits it on the first CRLFCRLF, so a body that looks like a
# header terminator is the case that breaks a naive implementation.
BINARY = b"\r\n\r\n" + bytes(range(256)) * 4 + b"\x00\x00\r\n\r\n"


class Origin(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        with LOCK:
            COUNTS[self.path] += 1
        if self.path == "/slow":
            time.sleep(0.8)
        if self.path == "/herd":
            # Long enough that every worker is still in flight when the next
            # client arrives, so the test observes uncoalesced misses.
            time.sleep(0.15)
        if self.path == "/chunked":
            self.connection.sendall(
                b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"
            )
            return
        body = (
            BINARY
            if self.path == "/binary"
            else b"x" * 150000
            if self.path == "/large"
            else b"x" * 100000
            if self.path.startswith("/big-cache")
            else self.path.encode() + b":payload"
        )
        self.send_response(200)
        self.send_header(
            "Content-Length", str(len(body) + (10 if self.path == "/broken" else 0))
        )
        if self.path != "/unmarked":
            self.send_header(
                "Cache-Control",
                (
                    "public, max-age=60"
                    if self.path.startswith("/big-cache")
                    else "public, max-age=2"
                )
                if self.path != "/private"
                else "private, max-age=100",
            )
        if self.path == "/stale":
            self.send_header("Age", "999")
        if self.path == "/cookie":
            self.send_header("Set-Cookie", "a=b")
        self.end_headers()
        try:
            for i in range(0, len(body), 1024):
                self.wfile.write(body[i : i + 1024])
        except (BrokenPipeError, ConnectionResetError):
            pass


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_ready(proc, port, diagnostics):
    # Startup can be delayed on a busy machine; readiness has its own deadline.
    # This is only startup readiness; request timeout tests still use 300 ms.
    deadline = time.monotonic() + 10
    while True:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                return
        except OSError as error:
            status = proc.poll()
            if status is not None or time.monotonic() > deadline:
                diagnostics.seek(0)
                raise RuntimeError(
                    f"Server not ready: exit={status}, stderr={diagnostics.read()!r}"
                ) from error
            time.sleep(0.01)


@contextmanager
def servers():
    COUNTS.clear()
    origin = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Origin)
    thread = threading.Thread(target=origin.serve_forever, daemon=True)
    thread.start()
    port = free_port()
    diagnostics = tempfile.TemporaryFile(mode="w+")
    proc = subprocess.Popen(
        [str(ROOT / "webproxy-lab/proxy"), str(port)],
        env={**os.environ, "PROXY_TIMEOUT_MS": "300", "PROXY_WORKERS": str(WORKERS)},
        stdout=subprocess.DEVNULL,
        stderr=diagnostics,
    )
    try:
        wait_ready(proc, port, diagnostics)
        yield port, origin.server_port
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        diagnostics.close()
        origin.shutdown()
        origin.server_close()
        thread.join()


def request(proxy, origin, path, headers="", raw=None, fragmented=False):
    data = (
        raw
        or f"GET http://127.0.0.1:{origin}{path} HTTP/1.1\r\nHost: 127.0.0.1:{origin}\r\n{headers}\r\n".encode()
    )
    with socket.create_connection(("127.0.0.1", proxy), timeout=3) as s:
        if fragmented:
            for i in range(0, len(data), 7):
                s.sendall(data[i : i + 7])
        else:
            s.sendall(data)
        chunks = []
        while True:
            try:
                b = s.recv(65536)
            except ConnectionResetError:
                break
            if not b:
                break
            chunks.append(b)
    return b"".join(chunks)


class ProxyTests(unittest.TestCase):
    def setUp(self):
        self.ctx = servers()
        self.proxy, self.origin = self.ctx.__enter__()

    def tearDown(self):
        self.ctx.__exit__(None, None, None)

    def req(self, path, **kw):
        return request(self.proxy, self.origin, path, **kw)

    def test_hit_miss_expiry(self):
        a = self.req("/fresh", fragmented=True)
        b = self.req("/fresh")
        self.assertEqual(a.split(b"\r\n\r\n", 1)[1], b"/fresh:payload")
        self.assertEqual(b.split(b"\r\n\r\n", 1)[1], b"/fresh:payload")
        self.assertEqual(COUNTS["/fresh"], 1)
        time.sleep(2.1)  # max-age is the behavior under test
        self.req("/fresh")
        self.assertEqual(COUNTS["/fresh"], 2)

    def test_private_cookie_unmarked_large_are_not_cached(self):
        for path in ["/private", "/cookie", "/unmarked", "/large", "/stale"]:
            with self.subTest(path=path):
                a = self.req(path)
                b = self.req(path)
                self.assertEqual(COUNTS[path], 2)
                self.assertEqual(a.split(b"\r\n\r\n", 1)[1], b.split(b"\r\n\r\n", 1)[1])

    def test_auth_and_cookie_requests_bypass_existing_cache(self):
        self.req("/fresh")
        self.req("/fresh", headers="Authorization: Bearer test\r\n")
        self.req("/fresh", headers="Cookie: a=b\r\n")
        self.assertEqual(COUNTS["/fresh"], 3)

    def test_broken_origin_is_not_cached(self):
        self.req("/broken")
        self.req("/broken")
        self.assertEqual(COUNTS["/broken"], 2)

    def test_slow_and_chunked_origins_are_refused(self):
        # Both failures belong to the origin side: no answer inside the
        # timeout, and framing the proxy cannot forward without buffering.
        self.assertIn(b"504", self.req("/slow").split(b"\r\n", 1)[0])
        self.assertIn(b"502", self.req("/chunked").split(b"\r\n", 1)[0])

    def test_malformed_requests_are_refused_before_any_origin_contact(self):
        # One row per rejection branch in parse_uri()/the header loop. Every
        # case must be decided by the proxy alone, so COUNTS stays empty.
        cases = [
            ("origin-form target", b"GET /x HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("non-http scheme", b"GET https://a/ HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("userinfo in authority", b"GET http://u@a/ HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("ipv6 literal", b"GET http://[::1]:80/ HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("fragment", b"GET http://a/p#f HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("port 0", b"GET http://a:0/ HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("port above 65535", b"GET http://a:99999/ HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("empty host", b"GET http:/// HTTP/1.1\r\nHost: a\r\n\r\n", b"400"),
            ("unknown version", b"GET http://a/ HTTP/2.0\r\nHost: a\r\n\r\n", b"400"),
            ("trailing token", b"GET http://a/ HTTP/1.1 x\r\nHost: a\r\n\r\n", b"400"),
            ("missing host header", b"GET http://a/ HTTP/1.1\r\n\r\n", b"400"),
            ("duplicate host header", b"GET http://a/ HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", b"400"),
            ("obs-fold continuation", b"GET http://a/ HTTP/1.1\r\nHost: a\r\nX: 1\r\n c\r\n\r\n", b"400"),
            ("space before colon", b"GET http://a/ HTTP/1.1\r\nHost: a\r\nX : 1\r\n\r\n", b"400"),
            ("chunked request", b"GET http://a/ HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", b"400"),
            ("request body", b"GET http://a/ HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n", b"400"),
            ("connection option", b"GET http://a/ HTTP/1.1\r\nHost: a\r\nConnection: foo\r\n\r\n", b"400"),
            ("method other than GET", b"CONNECT a:443 HTTP/1.1\r\n\r\n", b"501"),
        ]
        for name, wire, status in cases:
            with self.subTest(case=name):
                reply = self.req("/", raw=wire)
                self.assertIn(status, reply.split(b"\r\n", 1)[0])
        self.assertEqual(sum(COUNTS.values()), 0)

    def test_cached_body_is_returned_byte_for_byte(self):
        # The cache keeps one NUL-terminated blob and re-inserts an Age header
        # at the CRLFCRLF boundary; a binary body that contains NUL and a blank
        # line would survive a byte-count bug but not a string-handling bug.
        miss, hit = self.req("/binary"), self.req("/binary")
        self.assertEqual(COUNTS["/binary"], 1)
        for reply in (miss, hit):
            head, body = reply.split(b"\r\n\r\n", 1)
            self.assertEqual(body, BINARY)
            # The proxy normalises forwarded header names to lower case.
            self.assertIn(b"content-length: %d" % len(BINARY), head)
        self.assertIn(b"Age: 0", hit.split(b"\r\n\r\n", 1)[0])

    def test_concurrent_misses_stay_bounded_and_agree(self):
        # No request coalescing: a cold URL fetched by many clients at once can
        # reach the origin once per worker, never more, and every client must
        # still get the same bytes and leave one consistent entry behind.
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            replies = list(pool.map(lambda _: self.req("/herd"), range(16)))
        bodies = {r.split(b"\r\n\r\n", 1)[1] for r in replies}
        self.assertEqual(bodies, {b"/herd:payload"})
        self.assertGreaterEqual(COUNTS["/herd"], 1)
        self.assertLessEqual(COUNTS["/herd"], WORKERS)
        settled = COUNTS["/herd"]
        self.req("/herd")
        self.assertEqual(COUNTS["/herd"], settled)

    def test_cache_capacity_and_recently_used_entry(self):
        for i in range(10):
            self.req(f"/big-cache{i}")
        self.req("/big-cache0")
        self.assertEqual(COUNTS["/big-cache0"], 1)
        self.req("/big-cache10")
        self.req("/big-cache0")
        self.assertEqual(COUNTS["/big-cache0"], 1)
        self.req("/big-cache1")
        self.assertEqual(COUNTS["/big-cache1"], 2)

    def test_tiny_server_static_file(self):
        port = free_port()
        diagnostics = tempfile.TemporaryFile(mode="w+")
        proc = subprocess.Popen(
            [str(ROOT / "webproxy-lab/tiny/tiny"), str(port)],
            cwd=ROOT / "webproxy-lab/tiny",
            stdout=subprocess.DEVNULL,
            stderr=diagnostics,
        )
        try:
            wait_ready(proc, port, diagnostics)
            expected = (ROOT / "webproxy-lab/tiny/home.html").read_bytes()
            for _ in range(2):
                response = request(self.proxy, port, "/home.html")
                self.assertEqual(response.split(b"\r\n\r\n", 1)[1], expected)
        finally:
            proc.terminate()
            proc.wait(timeout=3)
            diagnostics.close()

    def test_parallel_responses_and_repeated_disconnects(self):
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            replies = list(pool.map(lambda i: self.req(f"/p{i % 4}"), range(48)))
        for i, r in enumerate(replies):
            self.assertEqual(r.split(b"\r\n\r\n", 1)[1], f"/p{i % 4}:payload".encode())
        for _ in range(40):
            with socket.create_connection(("127.0.0.1", self.proxy), timeout=1):
                pass
        self.assertIn(b"200", self.req("/alive").split(b"\r\n", 1)[0])


if __name__ == "__main__":
    unittest.main()
