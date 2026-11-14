"""Desktop app entry point (also what the packaged .exe runs)."""
import multiprocessing
import os
import sys
from pathlib import Path


def run_py(argv):
    """UEWASM.exe --run-py script.py args: the packaged app's Python runs helper scripts (emsdk.py) on PCs without one"""
    import runpy
    if sys.stdout is None:
        sys.stdout = sys.stderr = open(1, "w", encoding="utf-8", errors="replace", closefd=False)
    sys.argv = argv
    sys.path.insert(0, os.path.dirname(os.path.abspath(argv[0])))
    try:
        runpy.run_path(argv[0], run_name="__main__")
    except SystemExit as e:
        sys.stdout.flush()
        os._exit(e.code if isinstance(e.code, int) else 1)
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    multiprocessing.freeze_support()
    if len(sys.argv) > 2 and sys.argv[1] == "--run-py":
        run_py(sys.argv[2:])
    if not getattr(sys, "frozen", False):
        sys.path.insert(0, str(Path(__file__).resolve().parent))
    from uewasm.gui import main
    main()
