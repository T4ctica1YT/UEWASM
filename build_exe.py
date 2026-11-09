"""Packages the desktop app as a single .exe (needs: pip install pyinstaller).
python build_exe.py  ->  dist/UEWASM.exe"""
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = "UEWASM"


def main():
    work = HERE / "build"
    work.mkdir(exist_ok=True)
    ico = HERE / "uewasm" / "assets" / "uewasm.ico"
    # only what the recompiler needs at run time: the runtime's C sources and web glue
    rt = work / "runtime"
    shutil.rmtree(rt, ignore_errors=True)
    shutil.copytree(HERE / "runtime", rt, ignore=shutil.ignore_patterns("*.o", "*.obj", "__pycache__"))
    sep = ";" if sys.platform == "win32" else ":"
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed", "--name", NAME,
           "--icon", str(ico), "--add-data", f"{rt}{sep}runtime", "--add-data", f"{HERE / 'uewasm' / 'assets'}{sep}uewasm/assets",
           "--exclude-module", "playwright", "--exclude-module", "numpy", "--exclude-module", "greenlet",
           "--collect-all", "iced_x86", "--hidden-import", "psutil",
           # video re-encoding and Unreal pak handling
           "--collect-all", "imageio_ffmpeg", "--hidden-import", "ooz", "--collect-submodules", "Crypto",
           "--distpath", str(HERE / "dist"), "--workpath", str(work / "pyi"), "--specpath", str(work),
           str(HERE / "uewasm_gui.pyw")]
    subprocess.run(cmd, check=True)
    exe = HERE / "dist" / f"{NAME}.exe"
    print(f"built {exe} ({exe.stat().st_size / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()
