"""Finding (and where possible installing) the tools that builds and particular settings need."""
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

EMSDK_ZIP = "https://github.com/emscripten-core/emsdk/archive/refs/heads/main.zip"
RUSTUP_URL = "https://rustup.rs"
VS_URL = "https://visualstudio.microsoft.com/visual-cpp-build-tools/"
FROZEN = getattr(sys, "frozen", False)
EXE = ".exe" if os.name == "nt" else ""


def settings_file():
    return Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "UEWASM" / "settings.json"


def saved_emsdk():
    try:
        return json.loads(settings_file().read_text(encoding="utf-8")).get("emsdk") or None
    except (OSError, ValueError):
        return None


def default_emsdk_dir():
    return Path.home() / "emsdk"


def emsdk_at(p):
    """the SDK root for a folder the user picked: the root itself, a folder holding 'emsdk', or a folder inside the SDK"""
    if not p:
        return None
    p = Path(str(p).strip().strip('"')).expanduser()
    for c in [p, p / "emsdk", *list(p.parents)[:3]]:
        if (c / "upstream" / "emscripten").is_dir() and (c / "emsdk.py").exists():
            return c
    return None


def emsdk_partial(p):
    """an SDK checkout whose compilers were never installed"""
    p = Path(str(p).strip().strip('"')).expanduser() if p else None
    return bool(p and (p / "emsdk.py").exists() and not (p / "upstream" / "emscripten").is_dir())


def find_emsdk():
    """UEWASM_EMSDK (the app's or --emsdk's choice), the app's saved choice, EMSDK, then the usual places"""
    here = Path(__file__).resolve()
    cands = [os.environ.get("UEWASM_EMSDK"), saved_emsdk(), os.environ.get("EMSDK"), default_emsdk_dir(),
             here.parents[1] / ".toolchain/emsdk", here.parents[2] / ".toolchain/emsdk"]
    for c in cands:
        r = emsdk_at(c)
        if r:
            return r
    return None


def binaryen(root):
    return root and (Path(root) / "upstream" / "bin" / f"wasm-opt{EXE}").exists()


def vswhere():
    return Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"


def msvc():
    vw = vswhere()
    if not vw.exists():
        return None
    r = subprocess.run([str(vw), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                        "-property", "installationPath"], capture_output=True, text=True, creationflags=no_window())
    p = r.stdout.strip().splitlines()
    return p[0] if p else None


def cargo():
    c = shutil.which("cargo") or str(Path.home() / ".cargo" / "bin" / f"cargo{EXE}")
    return c if Path(c).exists() else None


def rust_lifter_ready():
    from .recomp.build import RUST_DIR
    return (RUST_DIR / "target" / "release" / f"uewlift{EXE}").exists() or cargo() is not None


def module(name):
    try:
        return importlib.util.find_spec(name) is not None
    except (ImportError, ValueError):
        return False


def winget():
    return shutil.which("winget")


def no_window():
    return subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def python_cmd():
    """a Python to run helper scripts with; the packaged app runs them itself (uewasm_gui.pyw --run-py)"""
    return [sys.executable, "--run-py"] if FROZEN else [sys.executable]


def stream(cmd, log, cancel=None, **kw):
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace",
                         creationflags=no_window(), **kw)
    for line in p.stdout:
        line = line.rstrip()
        if line:
            log(line)
        if cancel and cancel.is_set():
            p.kill()
            raise InterruptedError("cancelled")
    if p.wait():
        raise RuntimeError(f"{Path(cmd[0]).name} {' '.join(cmd[1:3])} failed (exit code {p.returncode})")


def install_emsdk(dest, log=print, cancel=None, progress=None):
    """emsdk from GitHub into dest, then its latest compilers (about 2.5 GB); returns the SDK root"""
    dest = Path(dest)
    if not (dest / "emsdk.py").exists():
        if dest.exists() and any(dest.iterdir()):
            dest = dest / "emsdk"
        dest.mkdir(parents=True, exist_ok=True)
        log(f"downloading emsdk to {dest}")
        with tempfile.TemporaryDirectory() as tmp:
            z = Path(tmp) / "emsdk.zip"
            with urllib.request.urlopen(EMSDK_ZIP, timeout=60) as r, open(z, "wb") as f:
                shutil.copyfileobj(r, f)
            with zipfile.ZipFile(z) as zf:
                for m in zf.infolist():
                    rel = m.filename.split("/", 1)[1] if "/" in m.filename else ""
                    if not rel:
                        continue
                    t = dest / rel
                    if m.is_dir():
                        t.mkdir(parents=True, exist_ok=True)
                    else:
                        t.parent.mkdir(parents=True, exist_ok=True)
                        t.write_bytes(zf.read(m))
    env = {k: v for k, v in os.environ.items() if k not in ("PYTHONHOME", "PYTHONPATH", "EMSDK", "EM_CONFIG")}
    for step in ("install", "activate"):
        log(f"emsdk {step} latest (the download is large; this takes a while)")
        stream([*python_cmd(), str(dest / "emsdk.py"), step, "latest"], log, cancel, cwd=str(dest), env=env)
    if not emsdk_at(dest):
        raise RuntimeError(f"emsdk finished but {dest / 'upstream' / 'emscripten'} is missing")
    return dest


def install_winget(pkg, log=print, cancel=None, override=None):
    cmd = [winget(), "install", "--id", pkg, "-e", "--accept-package-agreements", "--accept-source-agreements"]
    if override:
        cmd += ["--override", override]
    stream(cmd, log, cancel)


def install_rust(log=print, cancel=None):
    install_winget("Rustlang.Rustup", log, cancel)


def install_msvc(log=print, cancel=None):
    install_winget("Microsoft.VisualStudio.2022.BuildTools", log, cancel,
                   override="--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended")


def install_pip(pkg, log=print, cancel=None):
    stream([sys.executable, "-m", "pip", "install", pkg], log, cancel)


class Need:
    """a missing tool and what can be done about it: install (label, fn), browse for a folder, or turn the setting off"""

    def __init__(self, key, name, title, text, install=None, browse=False, off=None, url=None):
        self.key, self.name, self.title, self.text = key, name, title, text
        self.install, self.browse, self.off, self.url = install, browse, off, url


def missing(o):
    """what the chosen options need that this PC lacks; o has the build options' keys (native, lifter, wasm_opt, videos, aes)"""
    out = []
    root = find_emsdk()
    if not root:
        out.append(Need("emsdk", "Emscripten", "Emscripten isn't installed",
                        "UEWASM compiles games to WebAssembly with the Emscripten SDK, and it wasn't found"
                        + (f" (looked in {os.environ.get('UEWASM_EMSDK') or saved_emsdk()})" if os.environ.get("UEWASM_EMSDK") or saved_emsdk() else "")
                        + ".\n\nUEWASM can download and install it for you (about 2.5 GB), or you can point it at an existing install.",
                        install=("Install Emscripten for me", None), browse=True, url="https://emscripten.org/docs/getting_started/downloads.html"))
    elif o.get("wasm_opt") and not binaryen(root):
        out.append(Need("binaryen", "Binaryen", "Binaryen isn't in this Emscripten install",
                        f"'Shrink code with Binaryen' runs wasm-opt, which wasn't found in {root / 'upstream' / 'bin'}.",
                        off="Build without Binaryen"))
    if o.get("native") and not msvc():
        out.append(Need("msvc", "Visual Studio Build Tools", "Visual Studio C++ tools aren't installed",
                        "The native test .exe is built with Microsoft's C++ compiler (Visual Studio or its Build Tools, "
                        "with 'Desktop development with C++')." + ("\n\nUEWASM can install the Build Tools for you with winget "
                                                                   "(about 6 GB; Windows asks for administrator rights)." if winget() else ""),
                        install=("Install Build Tools for me", install_msvc) if winget() else None, off="Skip the native test .exe", url=VS_URL))
    if o.get("lifter") == "rust" and not rust_lifter_ready():
        out.append(Need("rust", "Rust", "Rust isn't installed",
                        "The Rust recompiler is built from source the first time it's used, which needs the Rust toolchain. "
                        "The Python recompiler gives the same result, only slower." + ("\n\nUEWASM can install Rust for you with winget." if winget() else ""),
                        install=("Install Rust for me", install_rust) if winget() else None, off="Use the Python recompiler", url=RUSTUP_URL))
    if o.get("videos") and not module("imageio_ffmpeg"):
        out.append(Need("ffmpeg", "imageio-ffmpeg", "The video encoder isn't installed",
                        "Shrinking videos uses ffmpeg from the imageio-ffmpeg Python package, which isn't installed.",
                        install=("Install it for me (pip)", lambda log, cancel: install_pip("imageio-ffmpeg", log, cancel)) if not FROZEN else None,
                        off="Keep the original videos"))
    if o.get("aes") and not module("Crypto"):
        out.append(Need("crypto", "pycryptodome", "The AES decryptor isn't installed",
                        "Decrypting paks uses the pycryptodome Python package, which isn't installed.",
                        install=("Install it for me (pip)", lambda log, cancel: install_pip("pycryptodome", log, cancel)) if not FROZEN else None,
                        off="Don't decrypt"))
    return out


def status():
    """one line per tool for the app's Tools tab and 'uewasm doctor'"""
    root = find_emsdk()
    return {
        "emsdk": str(root) if root else None,
        "binaryen": bool(root and binaryen(root)),
        "msvc": msvc(),
        "rust": cargo() or ("built" if rust_lifter_ready() else None),
        "ffmpeg": module("imageio_ffmpeg"),
        "crypto": module("Crypto"),
    }
