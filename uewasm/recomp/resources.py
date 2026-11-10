"""How much of the PC a recompile may use. Two modes, same output:

- high: tuned to this machine for speed: compile jobs on all but two logical cores, a RAM budget of about a third of
  the PC's memory (less when little is free), helpers running below normal priority so the PC stays usable.
- low: for working or gaming alongside a build: about half the physical cores, a fifth of the memory, idle priority.

Every tool process starts without a console window (Windows opens one per compiler otherwise)."""
import os
import subprocess
import sys

MODES = ("high", "low")
_mode = "high"


def set_mode(mode):
    global _mode
    if mode not in MODES:
        raise ValueError(f"priority must be one of {MODES}")
    _mode = mode


def mode():
    return _mode


def _machine():
    logical = os.cpu_count() or 4
    physical, total, avail = None, None, None
    try:
        import psutil
        physical = psutil.cpu_count(logical=False)
        vm = psutil.virtual_memory()
        total, avail = vm.total >> 20, vm.available >> 20
    except ImportError:
        pass
    physical = physical or max(1, logical // 2)
    total = total or 8192
    avail = avail if avail is not None else total // 2
    return logical, physical, total, avail


def plan():
    """worker counts and the RAM budget (MB) for the current mode on this machine"""
    logical, physical, total, avail = _machine()
    if _mode == "low":
        budget = max(1024, min(int(total * 0.20), int(avail * 0.45)))
        compile_jobs = max(1, physical // 2)
        lift = max(1, min(2, physical // 2))
        media = 1
        media_threads = max(1, physical // 2)
        package = max(1, min(2, physical // 2))
    else:
        budget = max(1536, min(int(total * 0.35), int(avail * 0.65)))
        compile_jobs = max(1, logical - 2)
        # a lift worker holds the whole program's function list: ~1.2 GB each on large games
        lift = max(1, min(6, physical, budget // 1200))
        media = max(1, physical // 4)
        media_threads = 4
        package = max(1, min(4, physical // 2))
    if os.environ.get("UEW_BUILD_MB"):
        budget = int(os.environ["UEW_BUILD_MB"])
    return {"budget_mb": budget, "compile_jobs": compile_jobs, "lift_workers": lift, "media_workers": media,
            "media_threads": media_threads, "package_workers": package, "logical": logical, "physical": physical, "ram_mb": total}


def describe():
    p = plan()
    return (f"{_mode} priority: {p['compile_jobs']} compile jobs, {p['budget_mb'] / 1024:.1f} GB RAM budget "
            f"({p['physical']} cores / {p['logical']} threads, {p['ram_mb'] / 1024:.0f} GB RAM)")


_BELOW_NORMAL, _IDLE, _NO_WINDOW = 0x4000, 0x40, 0x08000000


def proc_flags():
    """keyword arguments for subprocess calls: no console window, lowered priority"""
    if sys.platform != "win32":
        return {}
    return {"creationflags": _NO_WINDOW | (_IDLE if _mode == "low" else _BELOW_NORMAL)}


def lower_this_process():
    """worker processes (lifting, packaging) run at the same lowered priority as the tool processes"""
    try:
        import psutil
        p = psutil.Process()
        if sys.platform == "win32":
            p.nice(psutil.IDLE_PRIORITY_CLASS if _mode == "low" else psutil.BELOW_NORMAL_PRIORITY_CLASS)
        else:
            p.nice(19 if _mode == "low" else 10)
    except Exception:
        pass


def run(cmd, **kw):
    """subprocess.run without a console window and at the mode's priority"""
    return subprocess.run(cmd, **proc_flags(), **kw)
