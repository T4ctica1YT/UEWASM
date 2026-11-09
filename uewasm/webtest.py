import json
import struct
import time
import zlib
from pathlib import Path

from .serve import background

PROBE = """
(() => {
  const s = window.__uewasm = { contexts: [], frames: 0, start: performance.now() };
  const get = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (t, a) {
    const c = get.call(this, t, a);
    if (c && !s.contexts.includes(t)) s.contexts.push(t);
    return c;
  };
  if (navigator.gpu) {
    const req = navigator.gpu.requestAdapter.bind(navigator.gpu);
    navigator.gpu.requestAdapter = (...x) => { s.contexts.push("webgpu"); return req(...x); };
  }
  const tick = () => { s.frames++; requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
})();
"""

FATAL = ("abort(", "out of memory", "runtimeerror", "uncaught", "failed to compile", "webgl context lost", "exception thrown")


def _png_variance(data):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return 0.0
    pos, idat, w, h, ctype = 8, b"", 0, 0, 2
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, _, ctype = struct.unpack(">IIBB", body[:10])
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(idat)
    stride = w * bpp
    prev = bytearray(stride)
    samples = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        if y % 8 == 0:
            samples.extend(tuple(line[x:x + 3]) for x in range(0, stride, bpp * 8))
    if not samples:
        return 0.0
    total = 0.0
    for ch in range(3):
        vals = [s[ch] for s in samples]
        mean = sum(vals) / len(vals)
        total += sum((v - mean) ** 2 for v in vals) / len(vals)
    return total


def _launch(p, renderer):
    args = ["--mute-audio", "--autoplay-policy=user-gesture-required", "--ignore-gpu-blocklist",
            "--enable-gpu-rasterization", "--use-angle=default", "--disable-renderer-backgrounding",
            "--disable-background-timer-throttling"]
    if renderer == "webgpu":
        args += ["--enable-unsafe-webgpu", "--enable-features=Vulkan,WebGPU"]
    errors = []
    for channel in (None, "msedge", "chrome"):
        try:
            return p.chromium.launch(headless=True, channel=channel, args=args)
        except Exception as e:
            errors.append(f"{channel or 'bundled'}: {str(e).splitlines()[0]}")
    raise RuntimeError("No usable Chromium found. Run: python -m playwright install chromium\n" + "\n".join(errors))


def run(build_dir, seconds=90, report_dir=None):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        raise SystemExit("Headless testing needs Playwright: python -m pip install playwright")

    build_dir = Path(build_dir).resolve()
    info = {}
    if (build_dir / "uewasm.json").is_file():
        info = json.loads((build_dir / "uewasm.json").read_text(encoding="utf-8"))
    srv = background(build_dir)
    url = f"http://localhost:{srv.server_port}/index.html"
    console, page_errors, failed = [], [], []
    result = {"url": url, "build": info.get("build"), "passed": False}
    try:
        with sync_playwright() as p:
            browser = _launch(p, info.get("renderer"))
            ctx = browser.new_context(viewport={"width": 1280, "height": 720}, service_workers="allow")
            ctx.add_init_script(PROBE)
            page = ctx.new_page()
            page.on("console", lambda m: console.append(f"[{m.type}] {m.text}"))
            page.on("pageerror", lambda e: page_errors.append(str(e)))
            page.on("requestfailed", lambda r: failed.append(f"{r.url} {r.failure}"))
            page.on("response", lambda r: failed.append(f"{r.url} HTTP {r.status}") if r.status >= 400 else None)
            t0 = time.time()
            page.goto(url, wait_until="load", timeout=120000)
            fatal = None
            last = (0, t0)
            fps = []
            while time.time() - t0 < seconds:
                page.wait_for_timeout(2000)
                if page.is_closed():
                    break
                frames = page.evaluate("window.__uewasm ? window.__uewasm.frames : 0")
                now = time.time()
                fps.append((frames - last[0]) / (now - last[1]))
                last = (frames, now)
                bad = [l for l in console + page_errors if any(k in l.lower() for k in FATAL)]
                if bad:
                    fatal = bad[0]
                    break
            state = page.evaluate("window.__uewasm || {}")
            shot = page.screenshot()
            variance = _png_variance(shot)
            result.update({
                "seconds": round(time.time() - t0, 1),
                "contexts": state.get("contexts", []),
                "fps_avg": round(sum(fps[-10:]) / max(1, len(fps[-10:])), 1),
                "screen_variance": round(variance, 1),
                "fatal": fatal,
                "page_errors": page_errors[:20],
                "failed_requests": failed[:20],
                "console_tail": console[-40:],
            })
            result["passed"] = bool(result["contexts"]) and not fatal and not page_errors and variance > 20
            if report_dir:
                rd = Path(report_dir)
                rd.mkdir(parents=True, exist_ok=True)
                (rd / "screenshot.png").write_bytes(shot)
                (rd / "report.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
            browser.close()
    finally:
        srv.shutdown()
        srv.server_close()
    return result


def summary(r):
    lines = [f"Headless test: {'PASS' if r['passed'] else 'FAIL'}",
             f"  graphics contexts: {', '.join(r.get('contexts') or ['none'])}",
             f"  frame rate (browser rAF): {r.get('fps_avg')}",
             f"  screen variance: {r.get('screen_variance')} (near 0 means a blank screen)"]
    if r.get("fatal"):
        lines.append(f"  fatal: {r['fatal'][:300]}")
    for e in r.get("page_errors", [])[:5]:
        lines.append(f"  page error: {e[:300]}")
    for f in r.get("failed_requests", [])[:5]:
        lines.append(f"  failed request: {f[:300]}")
    return "\n".join(lines)
