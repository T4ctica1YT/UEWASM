"""Lays out game files for the web runtime (data/index.txt plus parts small enough for static hosts). The work is done by
uewpack (Rust, rustlift/src/bin/uewpack.rs); this runs it."""
from pathlib import Path

from . import resources


def package(game_root, web, extra=None, part_mb=16, compress=False, log=print, progress=lambda s, d, t: None, replace=None):
    """game_root is mounted at /game; extra maps more virtual paths (e.g. /image.bin) to local files;
    replace maps paths relative to game_root to files served in their place (re-encoded videos, rewritten paks)"""
    from .build import run_tool
    args = [game_root, web, "--part-mb", part_mb, "--workers", resources.plan()["package_workers"]]
    if compress:
        args.append("--compress")
    for virt, p in (extra or {}).items():
        args += ["--extra", f"{virt}={p}"]
    for rel, p in (replace or {}).items():
        args += ["--replace", f"{rel}={p}"]
    run_tool("uewpack", args, log, progress)
    return Path(web) / "data"
