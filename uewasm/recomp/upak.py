"""Unreal Engine .pak files (UE4 4.25+ format, version 11): index, directory listing and entries.

Encrypted indexes need the game's 32-byte AES key (find_aes_key looks for it in the executable)."""
import os
import re
from pathlib import Path
import struct
from dataclasses import dataclass, field

MAGIC = 0x5A6F12E1


def _aes(key):
    from Crypto.Cipher import AES
    return AES.new(key, AES.MODE_ECB)


class Reader:
    def __init__(self, b, pos=0):
        self.b, self.p = b, pos

    def u32(self):
        v = struct.unpack_from("<I", self.b, self.p)[0]
        self.p += 4
        return v

    def i32(self):
        v = struct.unpack_from("<i", self.b, self.p)[0]
        self.p += 4
        return v

    def u64(self):
        v = struct.unpack_from("<Q", self.b, self.p)[0]
        self.p += 8
        return v

    def raw(self, n):
        v = self.b[self.p:self.p + n]
        self.p += n
        return v

    def fstring(self):
        n = self.i32()
        if n == 0:
            return ""
        if n < 0:
            s = self.raw(-n * 2).decode("utf-16-le")
        else:
            s = self.raw(n).decode("latin-1")
        return s.rstrip("\0")


@dataclass
class Entry:
    name: str
    offset: int
    size: int            # stored bytes (compressed if compressed)
    usize: int           # uncompressed bytes
    method: str          # "" for none
    encrypted: bool
    blocks: list = field(default_factory=list)   # (start, end) after the entry record, stored bytes
    block_size: int = 0
    method_index: int = 0
    dir: str = ""
    file: str = ""


@dataclass
class Pak:
    path: str
    version: int
    mount: str
    methods: list
    entries: list
    encrypted_index: bool
    seed: int = 0
    footer_names: bytes = b""
    dirs: list = None   # every directory in the index, in order, including ones that only hold subdirectories


def footer(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(max(0, size - 1024))
        tail = f.read()
    i = tail.rfind(struct.pack("<I", MAGIC))
    if i < 0:
        raise ValueError(f"{path}: not a pak")
    _, version, off, isize = struct.unpack_from("<IIqq", tail, i)
    enc = tail[i - 1] if i > 0 else 0
    methods = [tail[j:j + 32].split(b"\0")[0].decode("latin-1") for j in range(i + 44, len(tail) - 31, 32)]
    return version, off, isize, bool(enc), [m for m in methods if m] or []


def _read(f, off, size, key):
    f.seek(off)
    b = f.read(size)
    if key:
        b = _aes(key).decrypt(b[:len(b) // 16 * 16])
    return b


def _encoded(r, name, methods):
    flags = r.u32()
    method = (flags >> 23) & 0x3F
    enc = bool(flags >> 22 & 1)
    nblocks = (flags >> 6) & 0xFFFF
    # all six bits set: the real block size follows as its own field
    bsize = r.u32() if flags & 0x3F == 0x3F else (flags & 0x3F) << 11
    off = r.u32() if flags >> 31 & 1 else r.u64()
    usize = r.u32() if flags >> 30 & 1 else r.u64()
    size = (r.u32() if flags >> 29 & 1 else r.u64()) if method else usize
    blocks = []
    if nblocks == 1 and not enc:
        blocks = [(0, size)]
    elif nblocks:
        # block starts are relative to the entry; the entry header (serialized FPakEntry) precedes the data
        pos = 0
        for _ in range(nblocks):
            n = r.u32()
            blocks.append((pos, pos + n))
            pos += (n + 15) // 16 * 16 if enc else n
    return Entry(name, off, size, usize, methods[method - 1] if method else "", enc, blocks, bsize, method)


def open_pak(path, key=None):
    version, off, isize, enc, methods = footer(path)
    if version < 11:
        raise ValueError(f"{path}: pak version {version} is not supported yet")
    with open(path, "rb") as f:
        idx = Reader(_read(f, off, isize, key if enc else None))
        mount = idx.fstring()
        if not mount.startswith("../") and not mount.startswith("/"):
            raise ValueError(f"{path}: index didn't decrypt (wrong or missing AES key)")
        idx.i32()
        seed = idx.u64()
        has_hash = idx.u32()
        if has_hash:
            idx.u64(); idx.u64(); idx.raw(20)
        has_full = idx.u32()
        if not has_full:
            raise ValueError(f"{path}: no full directory index")
        fd_off, fd_size = idx.u64(), idx.u64()
        idx.raw(20)
        enc_size = idx.i32()
        encoded = idx.raw(enc_size)
        fd = Reader(_read(f, fd_off, fd_size, key if enc else None))
    entries = []
    dirs = []
    for _ in range(fd.i32()):
        d = fd.fstring()
        dirs.append(d)
        for _ in range(fd.i32()):
            n = fd.fstring()
            eo = fd.i32()
            if eo < 0:
                continue
            e = _encoded(Reader(encoded, eo), (d + n).lstrip("/"), methods)
            e.dir, e.file = d, n
            entries.append(e)
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(max(0, size - 1024))
        tail = f.read()
    i = tail.rfind(struct.pack("<I", MAGIC))
    return Pak(path, version, mount, methods, entries, enc, seed, tail[i + 44:i + 44 + 160], dirs)


def find_aes_key(exe, pak):
    """The key appears as eight 4-byte immediate stores (or four 8-byte ones, or plain data) in shipping builds."""
    version, off, isize, enc, _ = footer(pak)
    if not enc:
        return None
    with open(pak, "rb") as f:
        f.seek(off)
        block = f.read(16)
    data = open(exe, "rb").read()

    def ok(k):
        d = _aes(k).decrypt(block)
        n = struct.unpack_from("<i", d)[0]
        return 4 < n < 1024 and d[4:7] == b"../"
    for pat, at, n, width in ((rb"\xc7[\x40-\x47]", 3, 8, 4), (rb"[\x48\x49][\xb8-\xbf]", 2, 4, 8)):
        hits = [(m.start(), data[m.start() + at:m.start() + at + width]) for m in re.finditer(pat, data)]
        for k in range(len(hits) - n + 1):
            w = hits[k:k + n]
            if w[-1][0] - w[0][0] <= 96:
                key = b"".join(x[1] for x in w)
                if ok(key):
                    return key
    for p in range(0, len(data) - 32, 4):
        k = data[p:p + 32]
        if k.count(0) <= 6 and ok(k):
            return k
    return None


def header_size(e):
    """the serialized entry record that precedes each entry's data (version 11)"""
    return 53 if not e.method else 57 + 16 * len(e.blocks)


def read_entry(f, e, key=None):
    """the entry's original bytes: decrypted and decompressed"""
    import zlib
    aes = _aes(key) if e.encrypted and key else None
    base = e.offset + header_size(e)

    def chunk(start, n):
        f.seek(base + start)
        b = f.read((n + 15) // 16 * 16 if aes else n)
        return (aes.decrypt(b) if aes else b)[:n]
    if not e.method:
        return chunk(0, e.size)
    out = bytearray()
    for s, t in e.blocks:
        raw = chunk(s, t - s)
        want = min(e.block_size or e.usize, e.usize - len(out))
        if e.method == "Oodle":
            import ooz
            out += ooz.decompress(raw, want)
        elif e.method == "Zlib":
            out += zlib.decompress(raw)
        elif e.method == "Gzip":
            out += zlib.decompress(raw, 31)
        else:
            raise ValueError(f"{e.name}: compression {e.method} not supported")
    return bytes(out)


def path_hash(path, seed):
    """FNV-1a 64 of the lowercase UTF-16 path relative to the mount point, offset basis plus the pak's seed"""
    h = (0xcbf29ce484222325 + seed) & 0xFFFFFFFFFFFFFFFF
    for b in path.lower().encode("utf-16-le"):
        h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def _stored_len(e):
    pad = (lambda n: (n + 15) // 16 * 16) if e.encrypted else (lambda n: n)
    if not e.method:
        return pad(e.size)
    s, t = e.blocks[-1]
    return s + pad(t - s)


def _fstr(s):
    if not s:
        return struct.pack("<i", 0)
    try:
        b = s.encode("latin-1") + b"\0"
        return struct.pack("<i", len(b)) + b
    except UnicodeEncodeError:
        b = s.encode("utf-16-le") + b"\0\0"
        return struct.pack("<i", -(len(b) // 2)) + b


def _encode(e, offset, usize, size, method_index, encrypted, blocks, bsize):
    flags = (method_index & 0x3F) << 23 | (encrypted & 1) << 22 | (len(blocks) & 0xFFFF) << 6
    small = bsize and bsize % 2048 == 0 and bsize >> 11 < 0x3F
    flags |= (bsize >> 11) if small else (0x3F if blocks else 0)
    flags |= (offset < 1 << 32) << 31 | (usize < 1 << 32) << 30 | (size < 1 << 32) << 29
    out = struct.pack("<I", flags)
    if blocks and not small:
        out += struct.pack("<I", bsize)
    out += struct.pack("<I", offset) if offset < 1 << 32 else struct.pack("<Q", offset)
    out += struct.pack("<I", usize) if usize < 1 << 32 else struct.pack("<Q", usize)
    if method_index:
        out += struct.pack("<I", size) if size < 1 << 32 else struct.pack("<Q", size)
    if blocks and not (len(blocks) == 1 and not encrypted):
        out += b"".join(struct.pack("<I", t - s) for s, t in blocks)
    return out


def write_pak(pak, dst, replace, log=print):
    """Rewrites a pak with some entries replaced (name -> bytes). Untouched entries are copied as stored (still
    compressed and encrypted with the game's key); replacements are stored plain. The index is written unencrypted."""
    import hashlib
    entries = sorted(pak.entries, key=lambda e: e.offset)
    out_entries = []
    with open(pak.path, "rb") as src, open(dst + ".tmp", "wb") as out:
        for e in entries:
            pos = out.tell()
            if e.name in replace:
                # bytes, or a file path streamed in pieces (videos can be large)
                r = replace[e.name]
                n = len(r) if isinstance(r, (bytes, bytearray)) else os.path.getsize(r)
                h = hashlib.sha1()
                rec_at = out.tell()
                out.write(b"\0" * 53)
                if isinstance(r, (bytes, bytearray)):
                    h.update(r)
                    out.write(r)
                else:
                    with open(r, "rb") as rf:
                        while b := rf.read(8 << 20):
                            h.update(b)
                            out.write(b)
                end = out.tell()
                out.seek(rec_at)
                out.write(struct.pack("<qqqI", 0, n, n, 0) + h.digest() + struct.pack("<BI", 0, 0))
                out.seek(end)
                out_entries.append((e, _encode(e, pos, n, n, 0, False, [], 0)))
            else:
                n = header_size(e) + _stored_len(e)
                src.seek(e.offset)
                left = n
                while left:
                    b = src.read(min(left, 8 << 20))
                    if not b:
                        raise IOError(f"{pak.path}: entry {e.name} runs past the end")
                    out.write(b)
                    left -= len(b)
                out_entries.append((e, _encode(e, pos, e.usize, e.size, e.method_index, e.encrypted, e.blocks, e.block_size)))
        encoded, where = b"", {}
        for e, enc in out_entries:
            where[e.name] = len(encoded)
            encoded += enc
        mount_rel = pak.mount
        # keep empty directories too: the engine asks whether they exist (e.g. the ICU data folder)
        dirs = {d: [] for d in (pak.dirs or [])}
        for e, _ in out_entries:
            dirs.setdefault(e.dir, []).append((e.file, where[e.name]))
        dir_index = struct.pack("<i", len(dirs)) + b"".join(
            _fstr(d) + struct.pack("<i", len(fs)) + b"".join(_fstr(f) + struct.pack("<i", o) for f, o in fs) for d, fs in dirs.items())
        hashes = sorted((path_hash(e.name, pak.seed), where[e.name]) for e, _ in out_entries)
        hash_index = struct.pack("<i", len(hashes)) + b"".join(struct.pack("<Qi", h, o) for h, o in hashes) + dir_index
        index_off = out.tell()
        ph_off = index_off  # filled below once the primary index size is known
        primary = lambda ph, fd: (_fstr(pak.mount) + struct.pack("<iQ", len(out_entries), pak.seed)
                                  + struct.pack("<IQQ", 1, ph, len(hash_index)) + hashlib.sha1(hash_index).digest()
                                  + struct.pack("<IQQ", 1, fd, len(dir_index)) + hashlib.sha1(dir_index).digest()
                                  + struct.pack("<i", len(encoded)) + encoded + struct.pack("<i", 0))
        size0 = len(primary(0, 0))
        ph_off = index_off + size0
        fd_off = ph_off + len(hash_index)
        prim = primary(ph_off, fd_off)
        out.write(prim)
        out.write(hash_index)
        out.write(dir_index)
        out.write(b"\0" * 16 + b"\0" + struct.pack("<IIqq", MAGIC, pak.version, index_off, len(prim)) + hashlib.sha1(prim).digest() + pak.footer_names)
    os.replace(dst + ".tmp", dst)
    return dst


def parse_key(text):
    """an AES-256 key as 64 hex digits (0x optional) or base64; None when it isn't one"""
    import base64
    t = (text or "").strip().replace(" ", "")
    if t[:2].lower() == "0x":
        t = t[2:]
    if re.fullmatch(r"[0-9a-fA-F]{64}", t):
        return bytes.fromhex(t)
    try:
        b = base64.b64decode(t, validate=True)
    except ValueError:
        return None
    return b if len(b) == 32 else None


def encrypted_paks(root):
    """paks under root whose index is encrypted"""
    out = []
    for p in sorted(Path(root).rglob("*.pak")):
        try:
            if footer(p)[3]:
                out.append(p)
        except (ValueError, OSError):
            pass
    return out


def check_key(pak, key):
    """the key decrypts this pak's index into something that starts like one (a mount point FString)"""
    _, off, _, enc, _ = footer(pak)
    if not enc:
        return True
    with open(pak, "rb") as f:
        f.seek(off)
        d = _aes(key).decrypt(f.read(16))
    n = struct.unpack_from("<i", d)[0]
    return 0 < n < 1024 and (d[4:7] == b"../" or d[4:5] == b"/")


def _legacy_index(idx, version):
    """entries of a pre-path-hash index (versions 3-10): (name, entry offset, size, compressed blocks, flags byte position)"""
    out = []
    for _ in range(idx.i32()):
        name = idx.fstring()
        off, size, usize = idx.u64(), idx.u64(), idx.u64()
        method = idx.u32()
        if version <= 1:
            idx.u64()
        idx.raw(20)
        blocks = []
        if method:
            blocks = [(idx.u64(), idx.u64()) for _ in range(idx.i32())]
        flag_at = idx.p
        flags = idx.raw(1)[0]
        idx.u32()
        out.append((name, off, size, method, blocks, flags, flag_at))
    return out


def encrypted_entries(path, key):
    """how many entries of an older pak have encrypted data (0: only the index is encrypted)"""
    version, off, isize, enc, _ = footer(path)
    with open(path, "rb") as f:
        f.seek(off)
        b = f.read(isize)
    r = Reader(_aes(key).decrypt(b) if enc else b)
    r.fstring()
    return sum(1 for e in _legacy_index(r, version) if e[5] & 1)


def decrypt_pak(path, key, dst, log=print):
    """A copy of an older pak (versions 4-10) with the index and every entry decrypted in place: same layout, flags
    cleared, so the engine reads it as a plain pak (and the data compresses)."""
    import hashlib
    import shutil
    version, off, isize, enc, _ = footer(path)
    if not 4 <= version <= 10:
        raise ValueError(f"{path}: pak version {version} can't be decrypted here (versions 4-10 are)")
    if not check_key(path, key):
        raise ValueError(f"{path}: the AES key doesn't decrypt this pak")
    aes = _aes(key)
    tmp = str(dst) + ".tmp"
    shutil.copyfile(path, tmp)
    with open(tmp, "r+b") as f:
        f.seek(off)
        index = bytearray(aes.decrypt(f.read(isize)) if enc else f.read(isize))
        r = Reader(index)
        r.fstring()
        entries = _legacy_index(r, version)

        def dec(a, n):
            n = (n + 15) // 16 * 16
            f.seek(a)
            b = f.read(n)
            f.seek(a)
            f.write(aes.decrypt(b[:len(b) // 16 * 16]) + b[len(b) // 16 * 16:])
        for name, eo, size, method, blocks, flags, flag_at in entries:
            if not flags & 1:
                continue
            hdr = 53 + (4 + 16 * len(blocks) if method else 0)
            if method:
                for s, t in blocks:
                    dec(s if version < 5 else eo + s, t - s)
            else:
                dec(eo + hdr, size)
            f.seek(eo + hdr - 5)
            f.write(bytes([flags & ~1]))
            index[flag_at] = flags & ~1
        f.seek(off)
        f.write(index)
        size = os.path.getsize(tmp)
        f.seek(max(0, size - 1024))
        tail = f.read()
        i = tail.rfind(struct.pack("<I", MAGIC))
        base = size - len(tail)
        f.seek(base + i - 1)
        f.write(b"\0")
        f.seek(base + i + 24)
        f.write(hashlib.sha1(index).digest())
    os.replace(tmp, dst)
    log(f"  {Path(path).name}: {sum(1 for e in entries if e[5] & 1)} of {len(entries)} entries decrypted")
    return dst


def decrypt_paks(game_root, key, out, log=print, progress=lambda s, d, t: None):
    """{path relative to game_root: decrypted copy} for every encrypted pak the key opens; a wrong key raises"""
    game_root, out = Path(game_root), Path(out)
    paks = encrypted_paks(game_root)
    bad = [p for p in paks if not check_key(p, key)]
    if bad:
        raise ValueError("The AES key is wrong for " + ", ".join(p.name for p in bad[:5]) + ". Enter the game's own correct AES key.")
    result, total, done = {}, sum(p.stat().st_size for p in paks) or 1, 0
    for p in paks:
        rel = p.relative_to(game_root).as_posix()
        version = footer(p)[0]
        if version >= 11:
            log(f"  {rel}: pak version {version}, left encrypted (the game decrypts it as it plays)")
            continue
        if not encrypted_entries(p, key):
            log(f"  {rel}: only the index is encrypted; the game reads it with its own key, so it is served as it is")
            continue
        dst = out / "paks_decrypted" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        stamp = dst.with_suffix(".stamp")
        sig = f"{p.stat().st_size}:{p.stat().st_mtime_ns}:{key.hex()[:8]}"
        if not (dst.exists() and stamp.exists() and stamp.read_text() == sig):
            decrypt_pak(p, key, dst, log)
            stamp.write_text(sig)
        result[rel] = dst
        done += p.stat().st_size
        progress("decrypt", done, total)
    return result
