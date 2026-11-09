"""Game files over HTTP: builds tests/web/assets.c, checks the recompiled native build matches the original, then runs
the web build in headless Edge against a server with and without Range support."""
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from uewasm import serve  # noqa: E402
from uewasm.recomp import build  # noqa: E402

OUT = ROOT / "tests" / "bin" / "web"


def make_game(game):
    (game / "Content" / "Sub").mkdir(parents=True, exist_ok=True)
    (game / "Binaries").mkdir(exist_ok=True)
    rng = random.Random(1)
    (game / "Content" / "big.bin").write_bytes(bytes(rng.getrandbits(8) for _ in range(3_100_000)))
    (game / "Content" / "Sub" / "small.txt").write_text("hello from the game folder\n")
    (game / "Content" / "notes.txt").write_text("x")
    words = ["neighbor", "basement", "key", "door", "window", "crow", "toy", "glue"]
    (game / "Content" / "words.txt").write_text(" ".join(rng.choice(words) for _ in range(400_000)))
    (game / "Binaries" / "helper.dll").write_bytes(b"MZ" + bytes(1000))


def run_browser(web, ranges):
    from playwright.sync_api import sync_playwright
    serve.Handler.ranges = ranges
    srv = serve.background(web)
    out = []
    try:
        with sync_playwright() as p:
            b = p.chromium.launch(headless=True, channel="msedge", args=["--mute-audio"])
            page = b.new_page()
            page.on("console", lambda m: out.append(m.text))
            page.on("pageerror", lambda e: out.append("PAGEERROR " + str(e)))
            for _ in range(2):
                n = len(out)
                page.goto(f"http://localhost:{srv.server_port}/index.html")
                t0 = time.time()
                while time.time() - t0 < 60 and not any(l.strip() == "done" or l.startswith("PAGEERROR") for l in out[n:]):
                    page.wait_for_timeout(250)
                page.wait_for_timeout(6000)
            b.close()
    finally:
        srv.shutdown()
        srv.server_close()
        serve.Handler.ranges = True
    return out


def main():
    game = OUT / "game"
    make_game(game)
    env = build.msvc_env()
    exe = game / "assets.exe"
    r = subprocess.run([build.tool(env, "cl"), "/nologo", "/O2", "/MD", f"/Fe:{exe}", f"/Fo:{OUT}\\", str(HERE / "assets.c")],
                       env=env, capture_output=True, text=True, cwd=OUT)
    if r.returncode:
        sys.exit(r.stdout[-2000:])
    save = OUT / "save"
    shutil.rmtree(save, ignore_errors=True)
    shutil.rmtree(game / "Saved", ignore_errors=True)
    ref = []
    for _ in range(2):
        ref += subprocess.run([str(exe)], capture_output=True, text=True, cwd=game).stdout.replace("\r", "").strip().splitlines()
    shutil.rmtree(game / "Saved")
    out = OUT / "recomp"
    build.recompile(exe, out, mem_mb=256, log=lambda *_: None)
    native = build.build_native(out, log=lambda *_: None)
    got = []
    for _ in range(2):
        got += subprocess.run([str(native), "--image", str(out / "image.bin"), "--root", str(game), "--save", str(save), "--exe", "assets.exe"],
                              capture_output=True, text=True).stdout.replace("\r", "").strip().splitlines()
    ok = True
    print("native:", "MATCH" if got == ref else "DIFF")
    ok &= got == ref
    if got != ref:
        print("  want", ref, "\n  got ", got)
    ref = [l for l in ref if l.strip()]
    for compress in (False, True):
        sizes = []
        web = build.build_wasm(out, log=sizes.append, game_root=game, exe_rel="assets.exe", part_mb=1, compress=compress)
        print(sizes[0])
        for ranges in (True, False):
            lines = [l for l in run_browser(web, ranges) if l.strip() and not l.startswith("[")]
            same = lines[:len(ref)] == ref
            print(f"web ({'deflated' if compress else 'raw'}, {'range requests' if ranges else 'whole parts'}):", "MATCH" if same else "DIFF")
            if not same:
                print("  want", ref, "\n  got ", lines[:40])
            ok &= same
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
