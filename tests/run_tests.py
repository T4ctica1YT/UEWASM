"""Builds every tests/*.c(pp) with MSVC, recompiles it and checks that the original exe, the native
recompile and the WebAssembly build all print exactly the same thing."""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))

from uewasm.recomp import build  # noqa: E402

FLAGS = {".c": ["/O2", "/MD", "/GS"], ".cpp": ["/O2", "/MD", "/GS", "/EHsc", "/std:c++17"]}


def compile_test(src, env):
    out = HERE / "bin" / src.stem
    out.mkdir(parents=True, exist_ok=True)
    exe = out / f"{src.stem}.exe"
    if not exe.exists() or exe.stat().st_mtime < src.stat().st_mtime:
        r = subprocess.run([build.tool(env, "cl"), "/nologo", *FLAGS[src.suffix], f"/Fe:{exe}", f"/Fo:{out}\\", str(src)],
                           env=env, capture_output=True, text=True, cwd=out)
        if r.returncode:
            raise RuntimeError(r.stdout[-2000:])
    return exe


def clean_saved(d):
    shutil.rmtree(d / "Saved", ignore_errors=True)


def run(cmd, cwd=None):
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=cwd, timeout=600)
    return r.returncode, r.stdout, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("names", nargs="*")
    ap.add_argument("--no-wasm", action="store_true")
    a = ap.parse_args()
    env = build.msvc_env()
    srcs = sorted(p for p in HERE.iterdir() if p.suffix in FLAGS and (not a.names or p.stem in a.names))
    failed = 0
    for src in srcs:
        exe = compile_test(src, env)
        out = exe.parent / "recomp"
        build.recompile(exe, out, mem_mb=256, log=lambda *_: None)
        native = build.build_native(out, log=lambda *_: None)
        clean_saved(exe.parent)
        ref = run([str(exe)], cwd=exe.parent)
        clean_saved(exe.parent)
        save = out / "save"
        shutil.rmtree(save, ignore_errors=True)
        save.mkdir(parents=True)
        clean_saved(exe.parent)
        args = ["--image", str(out / "image.bin"), "--root", str(exe.parent), "--exe", exe.name, "--save", str(save)]
        got = {"native": run([str(native), *args])}
        if not a.no_wasm:
            build.build_wasm(out, log=lambda *_: None, node_fs=True)
            node = build.emsdk_env()[2]
            got["wasm"] = run([node, *build.jspi_flag(node), "game.js", *args], cwd=out / "web")
        ok = True
        if (exe.parent / "Saved").exists():
            ok = False
            print(f"  {src.name}: recompiled build wrote into the install folder")
        for name, (code, so, se) in got.items():
            if so.replace("\r", "") != ref[1].replace("\r", "") or code != ref[0]:
                ok = False
                ol, rl = so.splitlines(), ref[1].splitlines()
                diff = next((i for i in range(max(len(ol), len(rl))) if i >= len(ol) or i >= len(rl) or ol[i] != rl[i]), None)
                print(f"  {src.name} [{name}] exit {code} vs {ref[0]}; first diff at line {diff}:")
                if diff is not None:
                    print(f"    want: {rl[diff] if diff < len(rl) else '<eof>'}")
                    print(f"    got:  {ol[diff] if diff < len(ol) else '<eof>'}")
                if se.strip():
                    print("    stderr: " + se.strip().replace("\n", "\n            ")[:1500])
        print(f"{'PASS' if ok else 'FAIL'} {src.name} ({', '.join(got)})")
        failed += not ok
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
