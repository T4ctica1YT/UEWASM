"""Builds the D3D11 triangle test, checks the recompiled build matches the original, then renders it in a
headless browser and checks pixels."""
import struct
import subprocess
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT))
from uewasm.recomp import build  # noqa: E402
from uewasm.serve import background  # noqa: E402

FXC = next(Path(r"C:\Program Files (x86)\Windows Kits\10\bin").glob("*/x64/fxc.exe"))
OUT = ROOT / "tests" / "bin" / "gfx"


def png_pixels(data):
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h = struct.unpack(">II", body[:8])
            bpp = 4 if body[9] == 6 else 3
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return lambda x, y: tuple(rows[y][x * bpp:x * bpp + 3])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    env = build.msvc_env()
    for entry, prof, name in (("VSMain", "vs_5_0", "triangle_vs.h"), ("PSMain", "ps_5_0", "triangle_ps.h")):
        subprocess.run([str(FXC), "/nologo", f"/T{prof}", f"/E{entry}", f"/Vn", f"g_{entry}", f"/Fh{OUT / name}", str(HERE / "triangle.hlsl")],
                       check=True, capture_output=True)
    exe = OUT / "triangle.exe"
    r = subprocess.run([build.tool(env, "cl"), "/nologo", "/O2", "/MD", "/EHsc", f"/I{OUT}", f"/Fe:{exe}", f"/Fo:{OUT}\\", str(HERE / "triangle.cpp")],
                       env=env, capture_output=True, text=True, cwd=OUT)
    if r.returncode:
        sys.exit(r.stdout[-2000:])
    ref = subprocess.run([str(exe)], capture_output=True, text=True, cwd=OUT).stdout
    out = OUT / "recomp"
    build.recompile(exe, out, mem_mb=256, log=lambda *_: None)
    native = build.build_native(out, log=lambda *_: None)
    got = subprocess.run([str(native), "--image", str(out / "image.bin"), "--root", str(OUT), "--exe", exe.name], capture_output=True, text=True)
    print("original:", ref.strip().replace("\n", " | "))
    print("native  :", got.stdout.strip().replace("\n", " | "), "MATCH" if got.stdout.replace("\r", "") == ref.replace("\r", "") else "DIFF")
    if got.stderr.strip():
        print("   ", got.stderr.strip()[:800])

    renderer = next((x for x in sys.argv[1:] if x in ("webgpu", "webgl2")), "webgpu")
    print("renderer:", renderer)
    web = build.build_wasm(out, log=lambda *_: None, renderer=renderer)
    (web / "index.html").write_text('<!doctype html><body style="margin:0;background:#000"><canvas id="canvas" width="640" height="360"></canvas>'
                                    '<script>var Module={canvas:document.getElementById("canvas"),arguments:["--exe","triangle.exe"],'
                                    'uewgDebug:location.search.includes("debug"),print:t=>console.log(t),printErr:t=>console.log(t)};</script><script src="game.js"></script></body>')
    from playwright.sync_api import sync_playwright
    srv = background(web)
    logs = []
    with sync_playwright() as p:
        b = p.chromium.launch(headless=True, channel="msedge", args=["--mute-audio", "--ignore-gpu-blocklist", "--use-angle=d3d11", "--enable-unsafe-webgpu"])
        page = b.new_page(viewport={"width": 640, "height": 360})
        page.on("console", lambda m: logs.append(m.text))
        page.on("pageerror", lambda e: logs.append("PAGEERROR " + str(e)))
        page.goto(f"http://localhost:{srv.server_port}/index.html" + ("?debug" if "debug" in sys.argv else ""))
        page.wait_for_timeout(2500)
        shot = page.locator("#canvas").screenshot()
        b.close()
    srv.shutdown()
    (OUT / "web_frame.png").write_bytes(shot)
    px = png_pixels(shot)
    center, corner = px(320, 200), px(10, 10)
    print("browser log:", " | ".join(l for l in logs if l.strip())[:900 if "debug" not in sys.argv else 20000])
    print("corner pixel", corner, "(expect about (13, 25, 76) clear colour)")
    print("center pixel", center, "(expect a mix of red/green/blue from the triangle)")
    ok_corner = all(abs(a - b) < 12 for a, b in zip(corner, (13, 25, 76)))
    ok_center = center != corner and max(center) > 40
    print("RENDER", "PASS" if ok_corner and ok_center else "FAIL")


if __name__ == "__main__":
    main()
