"""SteamStub (variant 3.x, x64) unwrapping, so a wrapped game exe can be recompiled.

The stub keeps an XOR-chained header just before its entry point in the .bind section, with the real entry point
and, unless the "no encryption" flag is set, the AES key/IV that decrypt the code section."""
import struct

SIG = 0xC0DEC0DF
HEADER = 0xF0
NO_ENCRYPTION = 0x04


def _xor(data, key=0):
    out = bytearray(data)
    off = 0
    if not key:
        key = struct.unpack_from("<I", data, 0)[0]
        off = 4
    for i in range(off, len(data) - 3, 4):
        v = struct.unpack_from("<I", data, i)[0]
        struct.pack_into("<I", out, i, v ^ key)
        key = v
    return bytes(out)


def _sections(d):
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    osz = struct.unpack_from("<H", d, pe + 20)[0]
    first = pe + 24 + osz
    out = []
    for i in range(nsec):
        o = first + 40 * i
        name, vsz, va, rsz, raw = struct.unpack_from("<8sIIII", d, o)
        out.append({"off": o, "name": name.rstrip(b"\0").decode("latin1"), "vsz": vsz, "va": va, "rsz": rsz, "raw": raw})
    return pe, out


def _file_off(secs, rva):
    for s in secs:
        if s["va"] <= rva < s["va"] + max(s["vsz"], s["rsz"]):
            return s["raw"] + rva - s["va"]
    return None


def header(d):
    """the decoded stub header, or None when the exe isn't wrapped"""
    pe, secs = _sections(d)
    if not any(s["name"] == ".bind" for s in secs):
        return None
    ep = struct.unpack_from("<I", d, pe + 24 + 16)[0]
    off = _file_off(secs, ep)
    if off is None or off < HEADER:
        return None
    h = _xor(d[off - HEADER:off])
    if struct.unpack_from("<I", h, 4)[0] != SIG:
        return None
    f = {
        "image_base": struct.unpack_from("<Q", h, 0x08)[0],
        "oep": struct.unpack_from("<Q", h, 0x20)[0],
        "app_id": struct.unpack_from("<I", h, 0x38)[0],
        "flags": struct.unpack_from("<I", h, 0x3C)[0],
        "code_va": struct.unpack_from("<Q", h, 0x48)[0],
        "code_size": struct.unpack_from("<Q", h, 0x50)[0],
        "key": h[0x58:0x78], "iv": h[0x78:0x88], "stolen": h[0x88:0x98],
    }
    return f


def unwrap(data):
    """returns (unwrapped exe bytes, info) or (data, None) if it isn't a SteamStub 3.x x64 exe"""
    h = header(data)
    if not h:
        return data, None
    from Crypto.Cipher import AES
    d = bytearray(data)
    pe, secs = _sections(d)
    if not h["flags"] & NO_ENCRYPTION:
        code = next(s for s in secs if s["va"] <= h["code_va"] < s["va"] + s["vsz"])
        raw = bytes(d[code["raw"]:code["raw"] + code["rsz"]])
        iv = AES.new(h["key"], AES.MODE_ECB).decrypt(h["iv"])
        buf = h["stolen"] + raw
        buf = buf[:len(buf) // 16 * 16]
        plain = AES.new(h["key"], AES.MODE_CBC, iv).decrypt(buf)
        d[code["raw"]:code["raw"] + len(plain)] = plain[:code["rsz"]]
    oep = h["oep"]
    if oep >= h["image_base"] and h["image_base"]:
        oep -= h["image_base"]
    struct.pack_into("<I", d, pe + 24 + 16, oep)
    return bytes(d), h
