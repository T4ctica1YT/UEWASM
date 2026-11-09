import functools
import os
import re
import threading
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MIME = {
    ".wasm": "application/wasm", ".js": "text/javascript", ".html": "text/html", ".css": "text/css",
    ".json": "application/json", ".txt": "text/plain", ".png": "image/png", ".ico": "image/x-icon",
}


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, **MIME}

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    ranges = True

    def send_head(self):
        m = self.ranges and re.fullmatch(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        path = self.translate_path(self.path)
        if not m or not os.path.isfile(path):
            return super().send_head()
        size = os.path.getsize(path)
        start = int(m.group(1))
        end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
        if start > end:
            self.send_error(416)
            return None
        f = open(path, "rb")
        f.seek(start)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(path))
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        self._left = end - start + 1
        return f

    def copyfile(self, src, dst):
        left = getattr(self, "_left", None)
        if left is None:
            return super().copyfile(src, dst)
        self._left = None
        while left:
            b = src.read(min(left, 1 << 20))
            if not b:
                break
            dst.write(b)
            left -= len(b)

    def log_message(self, fmt, *args):
        if self.server.verbose:
            super().log_message(fmt, *args)


def make_server(root, port=8080, verbose=False):
    handler = functools.partial(Handler, directory=str(Path(root).resolve()))
    srv = ThreadingHTTPServer(("127.0.0.1", port), handler)
    srv.verbose = verbose
    srv.daemon_threads = True
    return srv


def serve(root, port=8080):
    srv = make_server(root, port, verbose=True)
    print(f"Serving {root} at http://localhost:{srv.server_port}/  (Ctrl+C to stop)")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()


def background(root, port=0):
    srv = make_server(root, port)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv
