# Fake Dispatcharr live proxy: /proxy/ts/stream/<uuid> -> endless real-time MPEG-TS from ffmpeg lavfi.
import subprocess
import sys
import threading
import collections
import json
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

COUNTS = collections.Counter()
L = threading.Lock()
MODE = {"refuse": False}


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/__stats":
            b = json.dumps(dict(COUNTS)).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
            return
        if self.path.startswith("/__refuse="):
            MODE["refuse"] = self.path.endswith("1")
            self.send_response(200)
            self.end_headers()
            return
        with L:
            COUNTS["connect " + self.path] += 1
            COUNTS["active"] += 1
        try:
            if MODE["refuse"]:
                self.send_response(503)
                self.end_headers()
                return
            p = subprocess.Popen(
                [
                    "ffmpeg",
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-re",
                    "-f",
                    "lavfi",
                    "-i",
                    "testsrc=size=320x180:rate=25",
                    "-f",
                    "lavfi",
                    "-i",
                    "sine=frequency=440",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "ultrafast",
                    "-g",
                    "25",
                    "-c:a",
                    "aac",
                    "-f",
                    "mpegts",
                    "pipe:1",
                ],
                stdout=subprocess.PIPE,
            )
            self.send_response(200)
            self.send_header("Content-Type", "video/mp2t")
            self.end_headers()
            try:
                while True:
                    d = p.stdout.read(65536)
                    if not d:
                        break
                    self.wfile.write(d)
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                p.kill()
                p.wait()
        finally:
            with L:
                COUNTS["active"] -= 1


class S(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128


S(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
