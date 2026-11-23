"""Real connections from a recompiled program through WISP: builds tests/net/nettest.c, runs it natively, recompiles it to
the web with --wisp and runs that in headless Edge against `uewasm serve`'s WISP server. Passes when the page prints the
same OK lines as the native run. Needs an internet connection.
    python tests/net/run_net.py"""
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from playwright.sync_api import sync_playwright  # noqa: E402

from uewasm import serve  # noqa: E402
from uewasm.recomp import build  # noqa: E402


def main():
    out = HERE.parents[0] / "bin" / "net"
    out.mkdir(parents=True, exist_ok=True)
    env = build.msvc_env()
    exe = out / "nettest.exe"
    r = subprocess.run([build.tool(env, "cl"), "/nologo", "/O1", "/MD", f"/Fe:{exe}", f"/Fo:{out}\\", str(HERE / "nettest.c")],
                       env=env, capture_output=True, text=True)
    if r.returncode:
        sys.exit("cl failed:\n" + r.stdout[-2000:])
    native = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60).stdout
    print("native:\n" + native)
    rec = out / "recomp"
    build.recompile(exe, rec, mem_mb=256, log=lambda *_: None)
    web = build.build_wasm(rec, game_root=out, exe_rel=exe.name, wisp=True, log=lambda *_: None)
    srv = serve.background(str(web))
    lines = []
    with sync_playwright() as p:
        b = p.chromium.launch(headless=True, channel="msedge", args=["--mute-audio", "--enable-unsafe-webgpu"])
        pg = b.new_page()
        pg.on("console", lambda m: lines.append(m.text))
        pg.goto(f"http://localhost:{srv.server_port}/index.html?netdebug=1", wait_until="commit")
        t0 = time.time()
        while time.time() - t0 < 90 and not any(l.strip() == "DONE" for l in lines):
            pg.wait_for_timeout(250)
        b.close()
    srv.shutdown()
    page = [l for l in lines if l.startswith(("OK", "FAIL", "[wisp]"))]
    print("web:\n" + "\n".join(page))
    want = [l.strip() for l in native.splitlines() if l.startswith("OK")]
    got = [l.strip() for l in lines if l.startswith("OK")]
    for w, g in zip(want, got):
        if w != g:
            print(f"differs:\n  native: {w}\n  web:    {g}")
    ok = want and want == got
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
