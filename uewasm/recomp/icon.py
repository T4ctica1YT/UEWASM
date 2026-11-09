"""The game's own icon as a favicon: the main icon group in the exe's resources, rebuilt as an .ico file."""
import struct
from pathlib import Path

RT_ICON, RT_GROUP_ICON = 3, 14


def _sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", d, opt)[0]
    ddir = opt + (112 if magic == 0x20B else 96)
    rsrc_rva, rsrc_size = struct.unpack_from("<II", d, ddir + 2 * 8)
    sec = opt + struct.unpack_from("<H", d, pe + 20)[0]
    secs = [struct.unpack_from("<IIII", d, sec + i * 40 + 8) for i in range(nsec)]
    return rsrc_rva, rsrc_size, secs


def _off(secs, rva):
    for vsize, va, rsize, raw in secs:
        if va <= rva < va + max(vsize, rsize):
            return rva - va + raw
    return None


def _resources(d):
    """{type: {id: bytes}} for numbered resources (first language of each)"""
    rva, size, secs = _sections(d)
    base = _off(secs, rva) if rva else None
    if base is None:
        return {}

    def entries(o):
        named, ids = struct.unpack_from("<HH", d, base + o + 12)
        for i in range(named + ids):
            name, target = struct.unpack_from("<II", d, base + o + 16 + 8 * i)
            yield (None if name & 0x80000000 else name), target

    out = {}
    for tid, t in entries(0):
        if tid not in (RT_ICON, RT_GROUP_ICON) or not t & 0x80000000:
            continue
        for rid, r in entries(t & 0x7FFFFFFF):
            if rid is None or not r & 0x80000000:
                continue
            for _, leaf in entries(r & 0x7FFFFFFF):
                if leaf & 0x80000000:
                    continue
                drva, dsize = struct.unpack_from("<II", d, base + leaf)
                o = _off(secs, drva)
                if o is not None:
                    out.setdefault(tid, {})[rid] = d[o:o + dsize]
                break
    return out


def exe_icon(exe):
    """.ico bytes of the exe's first icon group, or None"""
    try:
        d = Path(exe).read_bytes()
        res = _resources(d)
    except (OSError, struct.error):
        return None
    groups, icons = res.get(RT_GROUP_ICON), res.get(RT_ICON, {})
    if not groups:
        return None
    g = groups[min(groups)]
    count = struct.unpack_from("<H", g, 4)[0]
    items = []
    for i in range(count):
        w, h, colors, _, planes, bits, size, rid = struct.unpack_from("<BBBBHHIH", g, 6 + 14 * i)
        if rid in icons:
            items.append((w, h, colors, planes, bits, icons[rid]))
    if not items:
        return None
    head = struct.pack("<HHH", 0, 1, len(items))
    off = 6 + 16 * len(items)
    dirs, blobs = b"", b""
    for w, h, colors, planes, bits, data in items:
        dirs += struct.pack("<BBBBHHII", w, h, colors, 0, planes, bits, len(data), off + len(blobs))
        blobs += data
    return head + dirs + blobs


def write_favicon(web, exe):
    """favicon.ico in the web folder: the game's icon, else the UEWASM one. Returns the file name."""
    data = exe_icon(exe) if exe else None
    if not data:
        fallback = Path(__file__).resolve().parents[1] / "assets" / "uewasm.ico"
        data = fallback.read_bytes() if fallback.exists() else None
    if not data:
        return None
    (Path(web) / "favicon.ico").write_bytes(data)
    return "favicon.ico"
