"""Lays out game files for the web runtime: data/index.txt plus each file cut into parts small enough for static hosts
(Cloudflare Pages allows 25 MiB per file). Executables and debug files are listed but not uploaded.

With compress=True every 1 MiB block is deflated on its own so reads stay random access; audio and anything that
doesn't shrink is stored as is."""
import os
import re
import shutil
import zlib
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

BLOCK = 1 << 20
STUB = re.compile(r"\.(exe|dll|pdb|sym|lib|exp|bat|cmd|sh|command)$", re.I)
SKIP = re.compile(r"(^|/)(Saved|Intermediate|DerivedDataCache)/|(^|/)steam_appid\.txt$|\.(log|dmp)$", re.I)
AUDIO = re.compile(r"\.(ogg|wav|mp3|wem|bnk|opus|flac|xwb|xsb|fsb|bank|m4a|aac|wma)$", re.I)


def _create(path):
    """open for writing without writing through a hard link into the game install"""
    if path.exists() or path.is_symlink():
        path.unlink()
    return path.open("xb")


def _place(src, dst, offset, size):
    """copy a slice of src to dst, or hard link the whole file when that is the slice"""
    if dst.exists():
        dst.unlink()
    if offset == 0 and size == src.stat().st_size:
        try:
            os.link(src, dst)
            return 0
        except OSError:
            shutil.copyfile(src, dst)
            return size
    with src.open("rb") as f, _create(dst) as o:
        f.seek(offset)
        left = size
        while left:
            b = f.read(min(left, 8 << 20))
            if not b:
                break
            o.write(b)
            left -= len(b)
    return size


def _raw(job):
    i, src, data, part = job
    size = src.stat().st_size
    names, written = [], 0
    for k in range(max(1, -(-size // part))):
        name = f"{i}.{k}" if k else f"{i}"
        written += _place(src, data / name, k * part, min(part, size - k * part))
        names.append(name)
    return names, "", written


def _deflated(job):
    """deflate each block; parts end on block boundaries. Returns the block sizes for the index (raw blocks keep their size)"""
    i, src, data, part = job
    sizes, names, out, used = [], [], None, 0

    def start():
        nonlocal out, used
        if out:
            out.close()
        name = f"{i}.{len(names)}" if names else f"{i}"
        names.append(name)
        out, used = _create(data / name), 0
    start()
    written = 0
    with src.open("rb") as f:
        while True:
            raw = f.read(BLOCK)
            if not raw:
                break
            z = zlib.compressobj(6, zlib.DEFLATED, -15)
            c = z.compress(raw) + z.flush()
            blk = c if len(c) < len(raw) * 0.95 else raw
            if used and used + len(blk) > part:
                start()
            out.write(blk)
            used += len(blk)
            written += len(blk)
            sizes.append(len(blk))
    out.close()
    return names, ",".join(f"{s:x}" for s in sizes), written


def package(game_root, web, extra=None, part_mb=16, compress=False, log=print, progress=lambda s, d, t: None, replace=None):
    """game_root is mounted at /game; extra maps more virtual paths (e.g. /image.bin) to local files;
    replace maps paths relative to game_root to files served in their place (re-encoded videos, rewritten paks)"""
    game_root, web = Path(game_root), Path(web)
    data = web / "data"
    data.mkdir(parents=True, exist_ok=True)
    part = part_mb << 20
    entries = []
    for p in sorted(game_root.rglob("*")):
        rel = p.relative_to(game_root).as_posix()
        if p.is_file() and not SKIP.search(rel):
            entries.append((f"/game/{rel}", Path((replace or {}).get(rel, p)), bool(STUB.search(p.name))))
    for virt, p in (extra or {}).items():
        entries.append((virt, Path(p), False))

    # what each file id held last time, kept beside (not inside) the published folder
    stamp = web.parent / f"{web.name}.files"
    before = {}
    if stamp.exists():
        for line in stamp.read_text(encoding="utf-8").splitlines():
            k, sig, names, blocks = (line.split("\t") + ["", "", ""])[:4]
            before[k] = (sig, names.split(",") if names else [], blocks)
    lines = [f"uewasm-files {2 if compress else 1} {part}"]
    after, jobs, rows = {}, [], []
    total = 0
    for i, (virt, src, stub) in enumerate(entries):
        st = src.stat()
        mtime = (int(st.st_mtime) + 11644473600) * 10000000
        z = compress and not stub and not AUDIO.search(src.name) and st.st_size > 0
        flag = 1 if stub else 2 if z else 0
        rows.append([st.st_size, mtime, flag, virt, ""])
        if stub:
            continue
        total += st.st_size
        sig = f"{'z' if z else 'r'}|{src}|{st.st_size}|{st.st_mtime_ns}|{part}"
        old = before.get(str(i))
        if old and old[0] == sig and all((data / n).exists() for n in old[1]):
            after[str(i)] = old
            rows[i][4] = old[2]
        else:
            jobs.append((i, sig, z, (i, src, data, part)))
    written = 0
    work = sum(entries[j[0]][1].stat().st_size for j in jobs) or 1
    moved = 0
    progress("package", 0, work)
    def done(i, sig, res):
        nonlocal written, moved
        names, blocks, w = res
        moved += entries[i][1].stat().st_size
        progress("package", moved, work)
        after[str(i)] = (sig, names, blocks)
        rows[i][4] = blocks
        written += w
    if any(z for _, _, z, _ in jobs):
        with ProcessPoolExecutor(min(4, os.cpu_count() or 4)) as ex:
            futs = [(i, sig, ex.submit(_deflated if z else _raw, job)) for i, sig, z, job in jobs]
            for i, sig, fut in futs:
                done(i, sig, fut.result())
    else:
        for i, sig, _, job in jobs:
            done(i, sig, _raw(job))
    keep = {n for _, names, _ in after.values() for n in names}
    for old in data.iterdir():
        if old.name != "index.txt" and old.name not in keep:
            old.unlink()
    for size, mtime, flag, virt, blocks in rows:
        lines.append(f"{size}\t{mtime}\t{flag}\t{virt}" + (f"\t{blocks}" if blocks else ""))
    with _create(data / "index.txt") as f:
        f.write(("\n".join(lines) + "\n").encode("utf-8"))
    stamp.write_text("\n".join(f"{k}\t{s}\t{','.join(n)}\t{b}" for k, (s, n, b) in after.items()), encoding="utf-8")
    served = sum(f.stat().st_size for f in data.iterdir())
    log(f"game files: {len(entries)} listed, {total / 1048576:.0f} MB of game data served as {served / 1048576:.0f} MB "
        f"({len(jobs)} files updated, {written / 1048576:.0f} MB written, the rest linked or unchanged)")
    return data
