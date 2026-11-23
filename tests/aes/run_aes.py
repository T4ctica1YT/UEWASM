"""OpenSSL's assembly AES recompiled: builds tests/aes/aes.c against the libcrypto-3.dll of the Python install, runs it
natively and recompiled (native host build) on the SSSE3 path and the integer-table path (no AES-NI, as in the runtime),
and passes when every line matches.
    python tests/aes/run_aes.py"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from uewasm.recomp import build  # noqa: E402

# OPENSSL_ia32cap masks: no AES-NI and no PCLMULQDQ (vpaes, 4-bit GHASH); also no SSSE3 (table AES)
PATHS = {"SSSE3 (vpaes)": "~0x200000200000000:0", "integer tables": "~0x200020200000000:0"}


def main():
    out = HERE.parents[0] / "bin" / "aes"
    out.mkdir(parents=True, exist_ok=True)
    dll = Path(sys.base_prefix) / "DLLs" / "libcrypto-3.dll"
    if not dll.exists():
        sys.exit(f"SKIP: no {dll}")
    shutil.copy2(dll, out / dll.name)
    env = build.msvc_env("x64")
    exe = out / "aes.exe"
    r = subprocess.run([build.tool(env, "cl"), "/nologo", "/O2", "/MD", f"/Fe:{exe}", f"/Fo:{out}\\", str(HERE / "aes.c")],
                       env=env, capture_output=True, text=True, cwd=out)
    if r.returncode:
        sys.exit(r.stdout[-2000:])
    rec = out / "recomp"
    build.recompile(exe, rec, mem_mb=256, log=lambda *_: None, extra=[out / dll.name])
    native = build.build_native(rec, log=lambda *_: None)
    ok = True
    for name, cap in PATHS.items():
        want = subprocess.run([str(exe), cap], capture_output=True, text=True, cwd=out, env=dict(os.environ)).stdout.split("\n")
        got = subprocess.run([str(native), "--image", str(rec / "image.bin"), "--root", str(out), "--exe", exe.name, cap],
                             capture_output=True, text=True).stdout.split("\n")
        want = [l.strip() for l in want if l.strip()]
        got = [l.strip() for l in got if l.strip() and not l.startswith("[")]
        same = got == want
        ok &= same
        print(f"{name}: {'MATCH' if same else 'DIFF'}")
        if not same:
            print("  want", want, "\n  got ", got)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
