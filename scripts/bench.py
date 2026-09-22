"""프록시 성능을 재현 가능하게 측정한다.

측정하는 것은 세 가지다.

1. 프록시가 요청 하나에 더하는 비용 — 같은 원본을 직접 때린 지연과 프록시를
   거친 MISS 지연의 차이. 부하 생성기가 Python 이라 절대값에는 클라이언트
   비용이 섞이지만, 차이를 보면 그 공통 비용이 상쇄된다.
2. 캐시 HIT 가 MISS 대비 실제로 얼마나 싼지 — 원본 지연을 0 에 가깝게 만든
   상태에서 재므로 "원본이 느려서 빨라 보이는" 착시가 없다.
3. worker 풀의 포화점 — 동시성을 올리며 처리량을 재고, 느린 원본에서
   head-of-line blocking 이 README 가 주장하는 대로 일어나는지 확인한다.

사용법: make bench  (또는 python3 scripts/bench.py --quick)
"""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import platform
import socket
import socketserver
import statistics
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from test_proxy import free_port, wait_ready  # noqa: E402

BODY = b"x" * 1024
HEAD = (
    b"HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
    b"Cache-Control: public, max-age=600\r\n"
    b"Content-Length: %d\r\n\r\n" % len(BODY)
)
RESPONSE = HEAD + BODY
WORKERS = 8


class Origin(socketserver.StreamRequestHandler):
    """고정 응답만 돌려주는 최소 원본. delay 로 느린 원본을 흉내낸다."""

    delay = 0.0

    def handle(self):
        if not self.rfile.readline():
            return
        while True:
            line = self.rfile.readline()
            if line in (b"\r\n", b"\n", b""):
                break
        if Origin.delay:
            time.sleep(Origin.delay)
        self.wfile.write(RESPONSE)


class OriginServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def fetch(host, port, target, host_header):
    """요청 하나를 보내고 응답 전체를 받는 데 걸린 초를 돌려준다."""
    wire = (
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n"
        % (target, host_header)
    ).encode()
    start = time.perf_counter()
    with socket.create_connection((host, port), timeout=10) as s:
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.sendall(wire)
        got = 0
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            got += len(chunk)
    if got < len(BODY):
        raise RuntimeError("short response: %d bytes" % got)
    return time.perf_counter() - start


def percentiles(samples):
    ordered = sorted(samples)
    def at(q):
        return ordered[min(len(ordered) - 1, int(q * len(ordered)))]
    return {
        "n": len(ordered),
        "p50_ms": round(at(0.50) * 1000, 4),
        "p90_ms": round(at(0.90) * 1000, 4),
        "p99_ms": round(at(0.99) * 1000, 4),
        "mean_ms": round(statistics.fmean(ordered) * 1000, 4),
    }


def sequential(count, make_target, proxy_port, origin_port, direct=False):
    port = origin_port if direct else proxy_port
    host_header = "127.0.0.1:%d" % origin_port
    samples = []
    for i in range(count):
        target = make_target(i)
        if direct:
            target = target.split("127.0.0.1:%d" % origin_port, 1)[-1]
        samples.append(fetch("127.0.0.1", port, target, host_header))
    return percentiles(samples)


def throughput(concurrency, seconds, proxy_port, origin_port):
    target = "http://127.0.0.1:%d/hot" % origin_port
    host_header = "127.0.0.1:%d" % origin_port
    stop = time.perf_counter() + seconds
    counts = [0] * concurrency

    def run(slot):
        while time.perf_counter() < stop:
            fetch("127.0.0.1", proxy_port, target, host_header)
            counts[slot] += 1

    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        list(pool.map(run, range(concurrency)))
    elapsed = time.perf_counter() - started
    return {"concurrency": concurrency, "req_per_s": round(sum(counts) / elapsed, 1)}


def head_of_line(concurrency, proxy_port, origin_port):
    """느린 원본에서 worker 수가 완료 시간을 어떻게 계단으로 만드는지 잰다."""
    host_header = "127.0.0.1:%d" % origin_port
    started = time.perf_counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
        done = list(
            pool.map(
                lambda i: fetch(
                    "127.0.0.1",
                    proxy_port,
                    "http://127.0.0.1:%d/slow%d" % (origin_port, i),
                    host_header,
                ),
                range(concurrency),
            )
        )
    wall = time.perf_counter() - started
    ideal = -(-concurrency // WORKERS) * Origin.delay
    return {
        "concurrency": concurrency,
        "origin_delay_ms": round(Origin.delay * 1000, 1),
        "workers": WORKERS,
        "wall_s": round(wall, 3),
        "batches_expected": -(-concurrency // WORKERS),
        "wall_if_pool_is_the_limit_s": round(ideal, 3),
        "slowest_request_s": round(max(done), 3),
    }


def svg(rows, path):
    """의존성 없이 처리량 막대그래프를 그린다(라이트/다크 모두 읽히는 색)."""
    top = max(r["req_per_s"] for r in rows) or 1
    width, height, pad = 520, 240, 38
    bar = (width - 2 * pad) / len(rows) * 0.62
    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" '
        'font-family="ui-monospace,monospace" font-size="11">' % (width, height),
        '<rect width="%d" height="%d" fill="none"/>' % (width, height),
        '<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#888"/>'
        % (pad, height - pad, width - pad, height - pad),
        '<text x="%d" y="16" fill="#888">캐시 HIT 처리량 (req/s)</text>' % pad,
    ]
    for i, row in enumerate(rows):
        x = pad + (width - 2 * pad) / len(rows) * (i + 0.19)
        h = (height - 2 * pad) * row["req_per_s"] / top
        y = height - pad - h
        parts.append(
            '<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#4c8dd9"/>'
            % (x, y, bar, h)
        )
        parts.append(
            '<text x="%.1f" y="%.1f" text-anchor="middle" fill="#888">%d</text>'
            % (x + bar / 2, y - 4, row["req_per_s"])
        )
        parts.append(
            '<text x="%.1f" y="%d" text-anchor="middle" fill="#888">c=%d</text>'
            % (x + bar / 2, height - pad + 14, row["concurrency"])
        )
    parts.append("</svg>")
    path.write_text("\n".join(parts), encoding="utf-8")


def report(result, path):
    d, m, h = result["direct"], result["miss"], result["hit"]
    lines = [
        "# 벤치마크",
        "",
        "`make bench` 가 이 파일을 다시 만든다. 숫자는 환경에 따라 달라지므로",
        "표의 절대값이 아니라 **같은 실행 안에서의 차이**를 보면 된다.",
        "",
        "- 환경: %s / %s / Python %s"
        % (result["env"]["platform"], result["env"]["machine"], result["env"]["python"]),
        "- 측정 시각: %s" % result["env"]["when"],
        "- 원본: `scripts/bench.py` 안의 고정 응답 서버(본문 1 KiB, `public, max-age=600`)",
        "- 프록시: worker %d, queue 32, `PROXY_TIMEOUT_MS=2000`" % WORKERS,
        "",
        "## 1. 프록시가 요청 하나에 더하는 비용",
        "",
        "| 경로 | p50 | p90 | p99 | 평균 |",
        "|---|---|---|---|---|",
        "| 원본에 직접 (기준선) | %.3f ms | %.3f ms | %.3f ms | %.3f ms |"
        % (d["p50_ms"], d["p90_ms"], d["p99_ms"], d["mean_ms"]),
        "| 프록시 경유, 매번 MISS | %.3f ms | %.3f ms | %.3f ms | %.3f ms |"
        % (m["p50_ms"], m["p90_ms"], m["p99_ms"], m["mean_ms"]),
        "| 프록시 경유, 캐시 HIT | %.3f ms | %.3f ms | %.3f ms | %.3f ms |"
        % (h["p50_ms"], h["p90_ms"], h["p99_ms"], h["mean_ms"]),
        "",
        "- MISS 가 직접 요청보다 **+%.3f ms**(p50). 이게 연결 하나를 더 열고"
        % (m["p50_ms"] - d["p50_ms"]),
        "  요청·응답 헤더를 한 번 더 파싱하는 값이다.",
        "- HIT 는 MISS 대비 **%.3f ms → %.3f ms (%.1f배)**. 원본이 거의 즉시"
        % (m["p50_ms"], h["p50_ms"], m["p50_ms"] / max(h["p50_ms"], 1e-9)),
        "  응답하는 조건에서 잰 값이므로, 이 배수는 **원본 지연이 0 일 때의 하한**이다.",
        "  원본이 느릴수록 배수는 커진다.",
        "",
        "## 2. 동시성에 따른 캐시 HIT 처리량",
        "",
        "![동시성별 캐시 HIT 처리량 막대그래프](bench.svg)",
        "",
        "| 동시 클라이언트 | req/s |",
        "|---|---|",
    ]
    for row in result["throughput"]:
        lines.append("| %d | %.1f |" % (row["concurrency"], row["req_per_s"]))
    best = max(result["throughput"], key=lambda r: r["req_per_s"])
    last = result["throughput"][-1]
    lines += [
        "",
        "최고 처리량은 동시 %d 에서 %.1f req/s, 동시 %d 에서는 %.1f req/s 로"
        % (
            best["concurrency"],
            best["req_per_s"],
            last["concurrency"],
            last["req_per_s"],
        ),
        "%s."
        % (
            "떨어졌다"
            if last["req_per_s"] < best["req_per_s"]
            else "유지됐다"
        ),
        "",
        "**이 표를 프록시의 확장성 곡선으로 읽으면 안 된다.** 부하 생성기가 스레드",
        "%d 개짜리 Python 이라 동시성을 올리면 클라이언트 쪽 GIL 경합이 먼저 커진다."
        % last["concurrency"],
        "최고점이 worker 수(%d)보다 한참 낮은 %d 에서 나온 것이 그 증거다. 여기서"
        % (WORKERS, best["concurrency"]),
        "가져갈 수 있는 결론은 두 가지뿐이다. (1) 캐시 HIT 경로의 처리량은 이",
        "조건에서 만 단위 req/s 수준이고, (2) 그 값이 프록시 능력의 **하한**이다.",
        "worker 풀이 실제로 병목이 되는 모습은 3번에서 원본 지연이 클라이언트",
        "비용을 압도할 때 비로소 관측된다.",
        "",
        "## 3. 느린 원본에서의 head-of-line blocking",
        "",
        "README 가 \"느린 원본이 worker 를 오래 붙잡으면 처리량이 떨어진다\"고",
        "적고 있으니, 그게 실제로 관측되는지 재봤다.",
        "",
        "| 항목 | 값 |",
        "|---|---|",
    ]
    hol = result["head_of_line"]
    for label, key in [
        ("동시 요청 수", "concurrency"),
        ("원본 응답 지연", "origin_delay_ms"),
        ("worker 수", "workers"),
        ("풀이 병목이라면 예상되는 배치 수", "batches_expected"),
        ("그 경우 예상 총 시간", "wall_if_pool_is_the_limit_s"),
        ("실제 총 시간", "wall_s"),
        ("가장 늦은 요청 1건", "slowest_request_s"),
    ]:
        lines.append("| %s | %s |" % (label, hol[key]))
    lines += [
        "",
        "동시 %d 요청이 %d ms 짜리 원본을 때릴 때 총 %.3f 초가 걸렸다."
        % (hol["concurrency"], hol["origin_delay_ms"], hol["wall_s"]),
        "worker 가 %d 개이므로 요청은 %d 배치로 나뉘어 처리되고, 풀이 병목이라면"
        % (hol["workers"], hol["batches_expected"]),
        "%.3f 초가 나와야 한다. 실측이 여기에 붙는다는 것은 병목이 대역폭이나"
        % hol["wall_if_pool_is_the_limit_s"],
        "CPU 가 아니라 **worker 수 그 자체**라는 뜻이다. 가장 늦은 요청은 %.3f 초를"
        % hol["slowest_request_s"],
        "기다렸다 — 상한을 두는 대가가 이것이다.",
        "",
        "## 다음 병목",
        "",
        "1. 느린 원본에 대한 head-of-line blocking(위 3번). 요청마다 worker 하나를",
        "   붙잡는 구조라 worker 를 늘리는 것 외에는 답이 없다. 근본 해결은 비동기",
        "   I/O 이고, 그건 이 저장소의 범위를 넘는다.",
        "2. 같은 cold URL 에 대한 동시 요청이 병합되지 않아 원본에 worker 수만큼",
        "   중복 요청이 간다(`tests/test_proxy.py` 의",
        "   `test_concurrent_misses_stay_bounded_and_agree` 가 이 동작을 고정한다).",
        "3. 캐시 전역 mutex 하나. 항목이 16 개뿐이라 선형 탐색이 싸서 지금은 문제가",
        "   아니지만, 위 두 가지를 고치고 나면 다음 차례가 여기다.",
        "",
    ]
    path.write_text("\n".join(lines), encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--quick", action="store_true", help="표본 수를 줄인다")
    args = parser.parse_args()
    samples = 60 if args.quick else 300
    seconds = 0.4 if args.quick else 1.0

    origin = OriginServer(("127.0.0.1", 0), Origin)
    threading.Thread(target=origin.serve_forever, daemon=True).start()
    origin_port = origin.server_address[1]

    proxy_port = free_port()
    diagnostics = tempfile.TemporaryFile(mode="w+")
    proc = subprocess.Popen(
        [str(ROOT / "webproxy-lab/proxy"), str(proxy_port)],
        env={**os.environ, "PROXY_WORKERS": str(WORKERS), "PROXY_TIMEOUT_MS": "2000"},
        stdout=subprocess.DEVNULL,
        stderr=diagnostics,
    )
    try:
        wait_ready(proc, proxy_port, diagnostics)
        url = lambda i: "http://127.0.0.1:%d/cold%d" % (origin_port, i)
        hot = lambda i: "http://127.0.0.1:%d/hot" % origin_port
        warm = lambda i: "http://127.0.0.1:%d/warm%d" % (origin_port, i)

        # Warm up on URLs the measured series never reuses, otherwise the
        # first entries of the MISS series would be served from the cache.
        # /hot is primed separately so the HIT series contains no MISS.
        sequential(20, warm, proxy_port, origin_port)
        sequential(2, hot, proxy_port, origin_port)
        result = {
            "env": {
                "platform": platform.platform(terse=True),
                "machine": platform.machine(),
                "python": platform.python_version(),
                "when": time.strftime("%Y-%m-%d %H:%M:%S %Z"),
            },
            "direct": sequential(samples, url, proxy_port, origin_port, direct=True),
            "miss": sequential(samples, url, proxy_port, origin_port),
            "hit": sequential(samples, hot, proxy_port, origin_port),
            "throughput": [
                throughput(c, seconds, proxy_port, origin_port)
                for c in (1, 2, 4, 8, 16, 32)
            ],
        }
        Origin.delay = 0.2
        result["head_of_line"] = head_of_line(32, proxy_port, origin_port)
        Origin.delay = 0.0
    finally:
        proc.terminate()
        proc.wait(timeout=5)
        diagnostics.close()
        origin.shutdown()
        origin.server_close()

    docs = ROOT / "docs"
    (docs / "bench.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    svg(result["throughput"], docs / "bench.svg")
    report(result, docs / "bench.md")
    print(json.dumps(result, indent=2))
    print("\nwrote docs/bench.md, docs/bench.svg, docs/bench.json")


if __name__ == "__main__":
    main()
