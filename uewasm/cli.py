import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


CHOICES = {
    "renderer": {"webgpu": "WebGPU (faster, compute shaders; Chrome/Edge/Safari 26+; falls back to WebGL 2)", "webgl2": "WebGL 2 only (smaller, works in every modern browser)"},
    "threads": {"coop": "Cooperative: all game threads share one core, no special hosting needed",
                "real": "Real threads: game threads on several cores (WebGPU only; needs https or localhost, isolation set up automatically)"},
    "opt": {"-O2": "Balanced build time and speed", "-O3": "Fastest code, slower build", "-Os": "Smallest download"},
}


def ask(name, default):
    opts = list(CHOICES[name])
    print(f"\n{name}:")
    for i, k in enumerate(opts, 1):
        print(f"  {'*' if k == default else ' '}{i}. {k:7} {CHOICES[name][k]}")
    while True:
        v = input(f"pick 1-{len(opts)} [{opts.index(default) + 1}]: ").strip()
        if not v:
            return default
        if v.isdigit() and 1 <= int(v) <= len(opts):
            return opts[int(v) - 1]
        if v in opts:
            return v


def find_exe(path):
    p = Path(path)
    if p.is_file():
        return p
    found = sorted(p.rglob("*-Shipping.exe")) or sorted(x for x in p.rglob("*.exe") if "Redist" not in x.parts)
    if not found:
        sys.exit(f"no .exe in {p}")
    return max(found, key=lambda x: x.stat().st_size)


def cmd_recomp(a):
    from .recomp import build
    from .recomp.link import game_dlls
    # --progress: machine-readable lines for front ends (the desktop app): @@progress stage done total, @@error, @@done
    prog = build.nop_progress
    if a.progress:
        sys.stdout.reconfigure(line_buffering=True)

        def prog(stage, done, total):
            print(f"@@progress\t{stage}\t{done}\t{total}", flush=True)
    exe = find_exe(a.exe)
    if not a.yes and sys.stdin.isatty():
        for k in ("renderer", "threads", "opt"):
            setattr(a, k, ask(k, getattr(a, k)))
    out = Path(a.output or Path.cwd() / "builds" / exe.stem)
    extra, search = [], []
    root, found = game_dlls(exe)
    if a.game_dlls == "auto":
        extra = found
        search = sorted({str(d.parent) for d in extra})
    root = Path(root)
    exe_rel = exe.relative_to(root).as_posix()
    aes = None
    if a.aes_key:
        from .recomp import upak
        aes = upak.parse_key(a.aes_key)
        if not aes:
            sys.exit("--aes-key should be 64 hex digits (0x optional) or base64 of 32 bytes")
        bad = [p.name for p in upak.encrypted_paks(root) if not upak.check_key(p, aes)]
        if bad and a.progress:
            print(f"@@error\twrongkey\tThe AES key is wrong for {', '.join(bad[:5])}.", flush=True)
        if bad:
            sys.exit(f"the AES key is wrong for {', '.join(bad[:5])}; give the game's own correct key")
    from .recomp import resources
    resources.set_mode(a.priority)
    resources.lower_this_process()
    print(resources.describe())
    print(f"{exe.name}: {len(extra)} game DLLs, {(exe.stat().st_size + sum(p.stat().st_size for p in extra)) / 1048576:.0f} MB "
          "of machine code to translate")
    r = build.recompile(exe, out, mem_mb=a.mem_mb, comments=a.comments, extra=extra, search=search, system_dlls=not a.no_system_dlls,
                        lifter=a.lifter, progress=prog)
    print(f"{r['functions']} functions in {len(r['modules'])} modules, {r['unsupported_total']} unsupported instructions, "
          f"{len(r['missing_imports'])} imports not implemented yet (details in {out / 'coverage.json'})")
    (out / "options.json").write_text(json.dumps({k: getattr(a, k) for k in ("renderer", "threads", "opt", "mem_mb")}, indent=2))
    if a.native:
        native = build.build_native(out, debug=a.debug, progress=prog)
        if a.run:
            subprocess.run([str(native), "--image", str(out / "image.bin"), "--root", str(root), "--save", str(out / "save"), "--exe", exe_rel])
    if not a.native_only:
        w, h = (int(x) for x in a.resolution.lower().split("x"))
        web = build.build_wasm(out, opt=a.opt, threads=a.threads, renderer=a.renderer, game_root=root, exe_rel=exe_rel,
                               size=(w, h), part_mb=a.part_mb, compress=a.compress, video_height=a.video_height, video_mode=a.video_mode,
                               wasm_opt=a.wasm_opt, show_fps=a.fps, async_shaders=not a.sync_shaders, debug_panel=not a.no_debug_panel,
                               backend=a.backend, placeholders=a.placeholders, aes_key=aes, mem_limit_gb=a.mem_limit_gb,
                               single_file=a.single_file, progress=prog, quality=a.quality, names=a.names,
                               game_args=[*build.default_args(root / exe_rel), *a.game_arg] if a.game_arg else None)
        print(f"web build ready in {web} (upload the whole folder; test it with: uewasm test {web})")
        if a.progress:
            print(f"@@done\t{web}", flush=True)
        if a.single_file:
            print(f"single-file build in {out / 'single'} (open the .html directly)")


def cmd_coverage(a):
    r = json.loads((Path(a.build) / "coverage.json").read_text(encoding="utf-8"))
    print(f"{r['exe']}\n  functions {r['functions']}, unsupported instructions {r['unsupported_total']}")
    for k, v in list(r["unsupported"].items())[:a.top]:
        print(f"  {v:8} {k}  e.g. {', '.join(r['unsupported_examples'].get(k, [])[:2])}")
    print(f"  missing imports: {len(r['missing_imports'])}")
    for m in r["missing_imports"][:a.top]:
        print(f"    {m}")


def cmd_doctor(a):
    from .deps import status
    print(f"UEWASM python {sys.version.split()[0]}")
    try:
        import iced_x86  # noqa: F401
        print("  iced-x86: ok")
    except ImportError:
        print("  iced-x86: missing (pip install iced-x86)")
    st = status()
    print(f"  emscripten (web builds): {st['emsdk'] or 'not found (uewasm install-emsdk, or --emsdk PATH)'}")
    print(f"  binaryen (--wasm-opt): {'ok' if st['binaryen'] else 'not found'}")
    print(f"  MSVC (--native test builds): {st['msvc'] or 'not found'}")
    print(f"  Rust (--lifter rust): {st['rust'] or 'not found (https://rustup.rs)'}")
    print(f"  imageio-ffmpeg (--videos): {'ok' if st['ffmpeg'] else 'missing (pip install imageio-ffmpeg)'}")
    print(f"  pycryptodome (--aes-key): {'ok' if st['crypto'] else 'missing (pip install pycryptodome)'}")


def cmd_install_emsdk(a):
    from .deps import default_emsdk_dir, install_emsdk
    root = install_emsdk(Path(a.dir) if a.dir else default_emsdk_dir())
    print(f"Emscripten installed in {root}")


def main(argv=None):
    ap = argparse.ArgumentParser(prog="uewasm", description="Windows x64 game recompiler for the web")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("recomp", help="recompile a game for the web")
    r.add_argument("exe", help="the game's .exe, or its install folder")
    r.add_argument("-o", "--output")
    r.add_argument("--renderer", choices=list(CHOICES["renderer"]), default="webgpu")
    r.add_argument("--threads", choices=list(CHOICES["threads"]), default="coop")
    r.add_argument("--emsdk", help="where the Emscripten SDK is (default: the app's choice, EMSDK, or %%USERPROFILE%%\\emsdk)")
    r.add_argument("--opt", choices=list(CHOICES["opt"]), default="-O2")
    r.add_argument("--lifter", choices=["python", "rust"], default="python",
                   help="python: the original lifter; rust: the same output many times faster (needs a Rust toolchain, built on first use)")
    r.add_argument("--mem-mb", type=int, default=3072, help="guest memory (the browser limit is 4096 total)")
    r.add_argument("--mem-limit-gb", type=float, default=8, help="most memory the running game may use in all (game, file cache, video memory)")
    r.add_argument("--resolution", default="1280x720", help="render size; the page scales it to the window")
    r.add_argument("--part-mb", type=int, default=16, help="largest file written for the web host (Cloudflare Pages allows 25)")
    r.add_argument("--compress", action="store_true", help="compress game files (not audio) for a smaller download")
    r.add_argument("--single-file", action="store_true",
                   help="also write one .html holding the whole game, playable straight from disk with no server (cooperative threads only; best for small games)")
    r.add_argument("--sync-shaders", action="store_true", help="compile shaders when first drawn (exact first frames, but freezes while compiling)")
    r.add_argument("--quality", choices=["game", "low", "medium", "high", "epic"], default="game",
                   help="Unreal games: graphics quality (game: the game's own setting); lower settings run faster here")
    r.add_argument("--game-arg", action="append", default=[], help="extra launch argument for the game (repeat for more)")
    r.add_argument("--fps", action="store_true", help="show an FPS counter in the page (Ctrl+Shift+F toggles it)")
    r.add_argument("--names", action="store_true", help="keep function names in the module, so browser profiles name the game's functions")
    r.add_argument("--priority", choices=["high", "low"], default="high",
                   help="high: fastest, tuned to this PC; low: far less CPU and memory so the PC stays usable (same result)")
    r.add_argument("--backend", choices=["epic"], default=None,
                   help="offline stand-in for a game's online services (epic: Epic accounts, profiles and menus)")
    r.add_argument("--aes-key", help="the game's AES-256 pak key (hex or base64): checked against its encrypted paks, which are packaged decrypted")
    r.add_argument("--placeholders", action="store_true", help="with --backend: stand-in images and data for files the servers would send")
    r.add_argument("--no-debug-panel", action="store_true", help="leave out the UEWASM debug panel toggle (\\ or Insert in game)")
    r.add_argument("--wasm-opt", action="store_true", help="run Binaryen on the linked module: ~10%% smaller, needs ~20x its size in RAM")
    r.add_argument("--video-height", type=int, default=0, help="re-encode game videos taller than this (e.g. 720); 0 keeps them")
    r.add_argument("--video-mode", choices=["fast", "small"], default="fast", help="fast encodes ~10x quicker; small makes ~30%% smaller files")
    r.add_argument("--game-dlls", choices=["auto", "none"], default="auto", help="also recompile DLLs shipped with the game")
    r.add_argument("--no-system-dlls", action="store_true", help="use built-in CRT instead of recompiling the Microsoft runtime DLLs")
    r.add_argument("--native", action="store_true", help="also build a native verification binary")
    r.add_argument("--native-only", action="store_true", help="skip the web build")
    r.add_argument("--run", action="store_true", help="run the native build afterwards")
    r.add_argument("--debug", action="store_true")
    r.add_argument("--comments", action="store_true", help="annotate generated C with the original instructions")
    r.add_argument("-y", "--yes", action="store_true", help="don't ask, use the given or default options")
    r.add_argument("--progress", action="store_true", help="print machine-readable progress lines (for front ends)")
    c = sub.add_parser("coverage", help="show what's still missing for a build")
    c.add_argument("build")
    c.add_argument("--top", type=int, default=30)
    s = sub.add_parser("serve", help="serve a web build locally")
    s.add_argument("build")
    s.add_argument("--port", type=int, default=8080)
    t = sub.add_parser("test", help="run a web build in a headless browser")
    t.add_argument("build")
    t.add_argument("--seconds", type=int, default=90)
    t.add_argument("--report")
    d = sub.add_parser("doctor", help="check the toolchain")
    d.add_argument("--emsdk", help="where the Emscripten SDK is")
    e = sub.add_parser("install-emsdk", help="download and install the Emscripten SDK (about 2.5 GB)")
    e.add_argument("dir", nargs="?", help="where to install it (default %%USERPROFILE%%\\emsdk)")
    sub.add_parser("gui", help="open the desktop app (the default with no arguments)")
    if argv is None and len(sys.argv) == 1:
        argv = ["gui"]
    a = ap.parse_args(argv)
    if getattr(a, "emsdk", None):
        os.environ["UEWASM_EMSDK"] = a.emsdk
    if a.cmd == "gui":
        from .gui import main as gui_main
        gui_main()
        return
    if a.cmd == "recomp":
        cmd_recomp(a)
    elif a.cmd == "coverage":
        cmd_coverage(a)
    elif a.cmd == "serve":
        from .serve import serve
        serve(a.build, a.port)
    elif a.cmd == "test":
        from . import webtest
        res = webtest.run(a.build, a.seconds, a.report)
        print(webtest.summary(res))
        sys.exit(0 if res["passed"] else 1)
    elif a.cmd == "doctor":
        cmd_doctor(a)
    elif a.cmd == "install-emsdk":
        cmd_install_emsdk(a)
