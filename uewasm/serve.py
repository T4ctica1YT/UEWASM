import functools
import io
import os
import re
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

MIME = {
    ".wasm": "application/wasm", ".js": "text/javascript", ".html": "text/html", ".css": "text/css",
    ".json": "application/json", ".txt": "text/plain", ".png": "image/png", ".ico": "image/x-icon",
}


# with --log, put into the page's <head> before the game's scripts: every page console line (and uncaught errors) is
# sent back in batches and appended to the log file, so a run can be read while it happens without "Save log". It
# sees what the page's console sees (the same lines the debug panel saves).
LOG_SCRIPT = b"""<script>
(() => {
  let buf = [], busy = false;
  const t0 = performance.now();
  const keep = (k, args) => {
    const s = Array.from(args).map(a => typeof a === "string" ? a : (a && a.stack) || String(a)).join(" ");
    buf.push(((performance.now() - t0) / 1000).toFixed(1) + (k === "log" ? " " : " [" + k + "] ") + s);
  };
  for (const k of ["log", "info", "warn", "error"]) {
    const orig = console[k].bind(console);
    console[k] = (...a) => { keep(k, a); orig(...a); };
  }
  addEventListener("error", e => keep("page error", [e.message + " at " + e.filename + ":" + e.lineno]));
  addEventListener("unhandledrejection", e => keep("unhandled rejection", [e.reason]));
  const flush = () => {
    if (busy || !buf.length) return;
    const body = buf.join("\\n") + "\\n";
    buf = [];
    busy = true;
    fetch("/__uewlog", { method: "POST", body, keepalive: body.length < 60000 }).catch(() => {}).finally(() => { busy = false; });
  };
  setInterval(flush, 500);
  addEventListener("pagehide", () => { if (buf.length) navigator.sendBeacon("/__uewlog", buf.join("\\n") + "\\n"); buf = []; });
  keep("log", ["[uewlog] page " + location.href + " " + navigator.userAgent]);
})();
</script>
"""


class Handler(SimpleHTTPRequestHandler):
    extensions_map = {**SimpleHTTPRequestHandler.extensions_map, **MIME}

    def do_POST(self):
        logfile = getattr(self.server, "logfile", None)
        if self.path != "/__uewlog" or not logfile:
            self.send_error(404)
            return
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0))
        with self.server.log_lock, open(logfile, "ab") as f:
            f.write(body)
        self.send_response(204)
        self.end_headers()

    def _page_with_log(self):
        """index.html with LOG_SCRIPT inserted after <head>, when the server writes a log"""
        if not getattr(self.server, "logfile", None) or self.path.split("?")[0] not in ("/", "/index.html"):
            return None
        path = Path(self.translate_path("/index.html"))
        if not path.is_file():
            return None
        html = path.read_bytes()
        i = html.find(b"<head>")
        html = html[:i + 6] + b"\n" + LOG_SCRIPT + html[i + 6:] if i >= 0 else LOG_SCRIPT + html
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(html)))
        self.end_headers()
        return io.BytesIO(html)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        # lets the page sample itself with the JS Self-Profiling API (new Profiler(...)), wasm frames included: how
        # the page's own thread (rendering, audio) spends a frame, which the browser's devtools can't be scripted for
        self.send_header("Document-Policy", "js-profiling")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    ranges = True

    def send_head(self):
        page = self._page_with_log()
        if page:
            return page
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


def make_server(root, port=8080, verbose=False, logfile=None):
    handler = functools.partial(Handler, directory=str(Path(root).resolve()))
    srv = ThreadingHTTPServer(("127.0.0.1", port), handler)
    srv.verbose = verbose
    srv.daemon_threads = True
    srv.logfile = Path(logfile).resolve() if logfile else None
    srv.log_lock = threading.Lock()
    if srv.logfile:
        srv.logfile.parent.mkdir(parents=True, exist_ok=True)
        with open(srv.logfile, "a", encoding="utf-8") as f:
            f.write(f"=== uewasm serve {Path(root).resolve()} on port {srv.server_port}, {time.strftime('%Y-%m-%d %H:%M:%S')}\n")
    return srv


def serve(root, port=8080, logfile=None):
    srv = make_server(root, port, verbose=True, logfile=logfile)
    print(f"Serving {root} at http://localhost:{srv.server_port}/  (Ctrl+C to stop)")
    if srv.logfile:
        print(f"The page's log is written to {srv.logfile}")
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
