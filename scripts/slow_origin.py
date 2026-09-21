# 원격 원본 서버를 흉내 내는 로컬 서버: 응답마다 300ms 지연
import sys, time
from http.server import BaseHTTPRequestHandler, HTTPServer
class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    def do_GET(self):
        time.sleep(0.3); body = b"hello from origin\n"
        print(f"[origin] GET {self.path} (300ms)", flush=True)
        self.send_response(200); self.send_header("Content-Type","text/plain")
        self.send_header("Cache-Control","public, max-age=60"); self.send_header("Content-Length",str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self,*a): pass
HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
