"""Collects the Direct3D 9 shader blobs (vs/ps 2.0-3.0) from a UE3 game's shader caches into a folder, one .dxbc file
each (the name is historical: wgsl_dump and wgsl_check take D3D9 bytecode too).

    python tests/shaders/extract_d3d9.py "C:/Games/Dishonored" tests/bin/shaders9
"""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

VERSIONS = {v | k for v in (0xFFFE0000, 0xFFFF0000) for k in (0x0200, 0x0201, 0x0300)}


def walk(data, off):
    """the end of a well-formed shader starting at off (its version token), else None"""
    n = len(data)
    i = off + 4
    major = (struct.unpack_from("<I", data, off)[0] >> 8) & 0xFF
    count = 0
    while i + 4 <= n and count < 200000:
        tok = struct.unpack_from("<I", data, i)[0]
        op = tok & 0xFFFF
        if op == 0xFFFF:
            return i + 4 if tok == 0x0000FFFF else None
        if op == 0xFFFE:
            i += 4 + 4 * ((tok >> 16) & 0x7FFF)
            continue
        if tok & 0x80000000 or (op > 96 and op != 0xFFFD):
            return None
        i += 4 + 4 * ((tok >> 24) & 0xF if major >= 2 else 0)
        count += 1
    return None


def blobs(data):
    # UE3 serializes shader code inside packages at any byte offset, so search for the version tokens themselves
    for v in sorted(VERSIONS):
        pat = struct.pack("<I", v)
        off = data.find(pat)
        while off >= 0:
            end = walk(data, off)
            if end and end - off >= 16:
                yield data[off:end]
            off = data.find(pat, off + 4)


def lzo1x(src, size):
    """LZO1X decompression (UE3's default package compression on PC)"""
    out = bytearray()
    ip, n = 0, len(src)

    def copy_match(dist, length):
        start = len(out) - dist
        if start < 0:
            raise ValueError("lzo: bad distance")
        if dist >= length:
            out.extend(out[start:start + length])
        else:
            for k in range(length):
                out.append(out[start + k])

    state = 0      # literals copied by the previous instruction (0, 1-3, or 4 = a long run)
    if src[0] > 17:
        t = src[0] - 17
        ip = 1
        out.extend(src[ip:ip + t])
        ip += t
        state = 4 if t >= 4 else t
    while ip < n:
        t = src[ip]
        ip += 1
        if t < 16:
            if state == 0:
                # literal run
                if t == 0:
                    while src[ip] == 0:
                        t += 255
                        ip += 1
                    t += 15 + src[ip]
                    ip += 1
                t += 3
                out.extend(src[ip:ip + t])
                ip += t
                state = 4
                continue
            if state == 4:
                dist = 1 + 0x0800 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 3)
            else:
                dist = 1 + (t >> 2) + (src[ip] << 2)
                ip += 1
                copy_match(dist, 2)
        elif t >= 64:
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
            ip += 1
            copy_match(dist, (t >> 5) + 1)
        elif t >= 32:
            length = t & 31
            if length == 0:
                while src[ip] == 0:
                    length += 255
                    ip += 1
                length += 31 + src[ip]
                ip += 1
            v = src[ip] | src[ip + 1] << 8
            ip += 2
            copy_match(1 + (v >> 2), length + 2)
        else:
            dist = (t & 8) << 11
            length = t & 7
            if length == 0:
                while src[ip] == 0:
                    length += 255
                    ip += 1
                length += 7 + src[ip]
                ip += 1
            v = src[ip] | src[ip + 1] << 8
            ip += 2
            dist += v >> 2
            if dist == 0:
                break
            copy_match(dist + 0x4000, length + 2)
        state = src[ip - 2] & 3
        if state:
            out.extend(src[ip:ip + state])
            ip += state
    return bytes(out[:size])


def inflate(buf, usize):
    try:
        return zlib.decompress(buf)
    except zlib.error:
        return lzo1x(buf, usize)


def unpack_ue3(data):
    """UE3 packages store compressed chunks (zlib here); returns every chunk it can inflate, else the raw data"""
    out, pos = [], 0
    while True:
        j = data.find(b"\xc1\x83\x2a\x9e", pos)
        if j < 0:
            break
        pos = j + 4
        try:
            _, block, csize, usize = struct.unpack_from("<IIII", data, j)
            nblocks = (usize + block - 1) // block if block else 0
            if not (0 < nblocks < 4096 and 0 < csize < len(data)):
                continue
            sizes = [struct.unpack_from("<II", data, j + 16 + 8 * b) for b in range(nblocks)]
            p = j + 16 + 8 * nblocks
            buf = b""
            for cs, us in sizes:
                buf += inflate(data[p:p + cs], us)
                p += cs
            out.append(buf)
        except Exception:
            continue
    return out or [data]


def main():
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    dst.mkdir(parents=True, exist_ok=True)
    seen, kinds = set(), {}
    files = [p for p in src.rglob("*") if p.is_file() and "ShaderCache" in p.name]
    for f in files:
        data = f.read_bytes()
        for part in ([data] if f.suffix == ".bin" else [data] + unpack_ue3(data)):
            for b in blobs(part):
                h = hashlib.sha1(b).hexdigest()[:16]
                if h in seen:
                    continue
                seen.add(h)
                v = struct.unpack_from("<I", b, 0)[0]
                kind = f"{'ps' if v >> 16 == 0xFFFF else 'vs'}_{(v >> 8) & 0xFF}_{v & 0xFF}"
                kinds[kind] = kinds.get(kind, 0) + 1
                (dst / f"{kind}_{h}.dxbc").write_bytes(b)
    print(f"{len(seen)} shaders from {len(files)} files:", ", ".join(f"{k} {n}" for k, n in sorted(kinds.items())))


if __name__ == "__main__":
    main()
