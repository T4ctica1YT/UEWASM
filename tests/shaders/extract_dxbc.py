"""Collects every DXBC shader blob from a game install (shader caches and cooked assets) for translator testing."""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
seen, kinds = set(), {}
for f in src.rglob("*"):
    if not f.is_file() or f.suffix.lower() not in (".bin", ".uasset", ".uexp", ".ushaderbytecode", ".pak", ".ubulk"):
        continue
    raw = f.read_bytes()
    parts = [raw]
    i = raw.find(b"\x78\x9c")
    while i >= 0:
        try:
            parts.append(zlib.decompressobj().decompress(raw[i:i + (8 << 20)]))
        except zlib.error:
            pass
        i = raw.find(b"\x78\x9c", i + 2)
    data = b"".join(parts)
    p = data.find(b"DXBC")
    while p >= 0:
        if p + 32 <= len(data):
            size = struct.unpack_from("<I", data, p + 24)[0]
            if 32 < size < 4 << 20 and p + size <= len(data):
                blob = data[p:p + size]
                h = hashlib.sha1(blob).hexdigest()[:16]
                if h not in seen:
                    seen.add(h)
                    shex = blob.find(b"SHEX")
                    if shex < 0:
                        shex = blob.find(b"SHDR")
                    stage = struct.unpack_from("<I", blob, shex + 8)[0] >> 16 if shex >= 0 else 9
                    name = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs"}.get(stage, "xx")
                    kinds[name] = kinds.get(name, 0) + 1
                    (out / f"{name}_{h}.dxbc").write_bytes(blob)
        p = data.find(b"DXBC", p + 4)
print(f"{len(seen)} unique shaders:", kinds)
