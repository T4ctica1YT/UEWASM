"""Smaller game videos for the web build.

Videos are re-encoded at most `height` lines tall, in the codec family they came in (VP8/VP9 WebM stays WebM,
H.264 MP4 stays MP4) so the game's own player still decodes them. Unreal .pak files are rewritten with the new
videos; everything else in them is copied as stored. Results are cached, so a rebuild only encodes what changed."""
import concurrent.futures
import hashlib
import os
import re
import subprocess
from pathlib import Path

VIDEO = re.compile(r"\.(webm|mp4|mkv|mov|m4v)$", re.I)
MODES = {
    # name: encoder arguments; "fast" uses the real-time encoder (about 10x faster, about 1.5x larger)
    "fast": {"vp9": ["-deadline", "realtime", "-cpu-used", "8", "-crf", "32"], "vp8": ["-deadline", "realtime", "-cpu-used", "8", "-crf", "12"],
             "h264": ["-preset", "veryfast", "-crf", "24"]},
    "small": {"vp9": ["-deadline", "good", "-cpu-used", "5", "-crf", "32"], "vp8": ["-deadline", "good", "-cpu-used", "2", "-crf", "12"],
              "h264": ["-preset", "slow", "-crf", "24"]},
}


def ffmpeg():
    import imageio_ffmpeg
    return imageio_ffmpeg.get_ffmpeg_exe()


def probe(path):
    """(codec, height) of the first video stream"""
    r = subprocess.run([ffmpeg(), "-hide_banner", "-i", str(path)], capture_output=True, text=True, errors="replace")
    m = re.search(r"Video: (\w+).*?, (\d+)x(\d+)", r.stderr)
    return (m.group(1), int(m.group(3))) if m else (None, 0)


def encode(src, dst, height, mode, threads=4):
    """re-encodes src into dst if it is taller than height; returns False when the original should be kept"""
    codec, h = probe(src)
    if not codec or h <= height:
        return False
    family = "vp9" if codec == "vp9" else "vp8" if codec == "vp8" else "h264"
    enc = {"vp9": ["-c:v", "libvpx-vp9", "-b:v", "0", "-row-mt", "1", "-tile-columns", "2"], "vp8": ["-c:v", "libvpx", "-b:v", "8M"],
           "h264": ["-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart"]}[family]
    fmt = "webm" if family != "h264" else "mp4"
    tmp = str(dst) + ".part"
    cmd = [ffmpeg(), "-hide_banner", "-loglevel", "error", "-y", "-i", str(src), "-vf", f"scale=-2:{height}", *enc, *MODES[mode][family],
           "-threads", str(threads), "-c:a", "copy", "-f", fmt, tmp]
    r = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if r.returncode or not os.path.exists(tmp):
        raise RuntimeError(f"ffmpeg failed on {src}:\n{r.stderr[-1500:]}")
    if os.path.getsize(tmp) >= os.path.getsize(src):
        os.unlink(tmp)
        return False
    os.replace(tmp, dst)
    return True


def _key(*parts):
    return hashlib.sha1("|".join(map(str, parts)).encode()).hexdigest()[:20]


def shrink(game_root, exe, out, height=720, mode="fast", log=print, progress=lambda s, d, t: None):
    """Returns {path relative to game_root: replacement file} for files that got smaller."""
    from . import upak
    game_root, out = Path(game_root), Path(out)
    cache = out / "media"
    cache.mkdir(parents=True, exist_ok=True)
    jobs = []           # (source description, read source -> path, cached output path)
    paks = {}           # pak path -> (Pak, key, {entry name: cached path})
    loose = {}
    key = None
    for p in sorted(game_root.rglob("*")):
        rel = p.relative_to(game_root).as_posix()
        if p.suffix.lower() == ".pak":
            try:
                ver, *_ = upak.footer(p)
                if ver < 11:
                    continue
                if key is None and upak.footer(p)[3]:
                    key = upak.find_aes_key(exe, p) or b""
                    log("pak AES key found in the executable" if key else "pak index is encrypted and no key was found; paks are left as they are")
                pk = upak.open_pak(str(p), key or None)
            except (ValueError, OSError) as e:
                log(f"  {rel}: {e}")
                continue
            vids = [e for e in pk.entries if VIDEO.search(e.name)]
            if vids:
                paks[rel] = (pk, key, {})
                for e in vids:
                    c = cache / f"{_key(rel, e.name, e.offset, e.size, height, mode)}{Path(e.name).suffix.lower()}"
                    jobs.append(("pak", rel, e, c))
        elif VIDEO.search(p.name):
            c = cache / f"{_key(rel, p.stat().st_size, p.stat().st_mtime_ns, height, mode)}{p.suffix.lower()}"
            jobs.append(("file", rel, p, c))
    if not jobs:
        return {}
    total = sum(j[2].size if j[0] == "pak" else j[2].stat().st_size for j in jobs)
    log(f"{len(jobs)} videos ({total / 2**30:.1f} GiB): re-encoding to {height}p ({mode})")
    done_bytes = 0
    workers = max(1, (os.cpu_count() or 4) // 4)

    def run(job):
        kind, rel, src, c = job
        keep = c.with_suffix(c.suffix + ".keep")
        if c.exists() or keep.exists():
            return job, c.exists()
        tmp_src = None
        if kind == "pak":
            pk, k, _ = paks[rel]
            tmp_src = c.with_suffix(".src" + c.suffix)
            with open(pk.path, "rb") as f:
                tmp_src.write_bytes(upak.read_entry(f, src, k or None))
            path = tmp_src
        else:
            path = src
        try:
            ok = encode(path, c, height, mode)
        finally:
            if tmp_src and tmp_src.exists():
                tmp_src.unlink()
        if not ok:
            keep.write_bytes(b"")
        return job, ok

    with concurrent.futures.ThreadPoolExecutor(workers) as ex:
        for job, ok in ex.map(run, jobs):
            kind, rel, src, c = job
            done_bytes += src.size if kind == "pak" else src.stat().st_size
            progress("media", done_bytes, total)
            if ok:
                if kind == "pak":
                    paks[rel][2][src.name] = c
                else:
                    loose[rel] = c
    result = dict(loose)
    for rel, (pk, k, repl) in paks.items():
        if not repl:
            continue
        dst = out / "paks" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        sig = _key("pak-writer-2", *sorted((n, str(c)) for n, c in repl.items()), os.path.getsize(pk.path))
        stamp = dst.with_suffix(".stamp")
        if not dst.exists() or not stamp.exists() or stamp.read_text() != sig:
            upak.write_pak(pk, str(dst), {n: str(c) for n, c in repl.items()})
            stamp.write_text(sig)
        result[rel] = dst
    before = sum((game_root / r).stat().st_size for r in result)
    after = sum(Path(p).stat().st_size for p in result.values())
    log(f"videos: {len(result)} files went from {before / 2**30:.2f} GiB to {after / 2**30:.2f} GiB")
    return result
