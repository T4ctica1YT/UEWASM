import struct
from pathlib import Path
from dataclasses import dataclass, field


@dataclass
class Section:
    name: str
    rva: int
    vsize: int
    raw: int
    rsize: int
    flags: int

    @property
    def executable(self):
        return bool(self.flags & 0x20000000)


@dataclass
class Import:
    dll: str
    name: str
    iat_rva: int


@dataclass
class PE:
    path: str
    data: bytes
    image_base: int
    entry_rva: int
    image_size: int
    sections: list = field(default_factory=list)
    imports: list = field(default_factory=list)
    relocs: list = field(default_factory=list)
    functions: list = field(default_factory=list)
    unwind: dict = field(default_factory=dict)
    exports: dict = field(default_factory=dict)
    exports_ord: dict = field(default_factory=dict)
    ord_names: dict = field(default_factory=dict)
    family: dict = field(default_factory=dict)
    tls_callbacks: list = field(default_factory=list)
    tls: dict = field(default_factory=dict)
    dirs: list = field(default_factory=list)

    def off(self, rva):
        for s in self.sections:
            if s.rva <= rva < s.rva + max(s.vsize, s.rsize):
                d = rva - s.rva
                return s.raw + d if d < s.rsize else None
        return rva if rva < self.sections[0].rva else None

    def u32(self, rva):
        o = self.off(rva)
        return struct.unpack_from("<I", self.data, o)[0]

    def u64(self, rva):
        o = self.off(rva)
        return struct.unpack_from("<Q", self.data, o)[0]

    def cstr(self, rva):
        o = self.off(rva)
        return self.data[o:self.data.index(b"\0", o)].decode("latin-1")

    def section_of(self, rva):
        for s in self.sections:
            if s.rva <= rva < s.rva + max(s.vsize, s.rsize):
                return s
        return None

    def is_code(self, rva):
        s = self.section_of(rva)
        return s is not None and s.executable

    def image(self, base):
        img = bytearray(self.image_size)
        hdr = self.sections[0].raw if self.sections else 0x400
        img[:hdr] = self.data[:hdr]
        for s in self.sections:
            n = min(s.rsize, s.vsize or s.rsize)
            img[s.rva:s.rva + n] = self.data[s.raw:s.raw + n]
        delta = base - self.image_base
        for rva in self.relocs:
            v = struct.unpack_from("<Q", img, rva)[0]
            struct.pack_into("<Q", img, rva, (v + delta) & 0xFFFFFFFFFFFFFFFF)
        return bytes(img)


def load(path):
    data = open(path, "rb").read()
    if data[:2] != b"MZ":
        raise ValueError("not a PE file")
    from . import steamstub
    data, stub = steamstub.unwrap(data)
    if stub:
        print(f"{Path(path).name}: SteamStub wrapper removed (app {stub['app_id']}, entry {stub['oep']:#x})", flush=True)
    pe_off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe_off:pe_off + 4] != b"PE\0\0":
        raise ValueError("bad PE signature")
    machine, nsec, _, _, _, optsize, _ = struct.unpack_from("<HHIIIHH", data, pe_off + 4)
    if machine != 0x8664:
        raise ValueError("only x86-64 executables are supported")
    opt = pe_off + 24
    if struct.unpack_from("<H", data, opt)[0] != 0x20B:
        raise ValueError("not a PE32+ image")
    entry = struct.unpack_from("<I", data, opt + 16)[0]
    image_base = struct.unpack_from("<Q", data, opt + 24)[0]
    image_size = struct.unpack_from("<I", data, opt + 56)[0]
    ndirs = struct.unpack_from("<I", data, opt + 108)[0]
    dirs = [struct.unpack_from("<II", data, opt + 112 + 8 * i) for i in range(ndirs)]
    pe = PE(path, data, image_base, entry, image_size, dirs=dirs)
    so = opt + optsize
    for i in range(nsec):
        name = data[so:so + 8].rstrip(b"\0").decode("latin-1")
        vsize, rva, rsize, raw = struct.unpack_from("<IIII", data, so + 8)
        flags = struct.unpack_from("<I", data, so + 36)[0]
        pe.sections.append(Section(name, rva, vsize, raw, rsize, flags))
        so += 40

    def d(i):
        return dirs[i] if i < len(dirs) else (0, 0)

    rva, size = d(1)
    if rva:
        p = rva
        while True:
            ilt, _, _, name_rva, iat = struct.unpack_from("<IIIII", data, pe.off(p))
            if not name_rva:
                break
            dll = pe.cstr(name_rva)
            q, slot = ilt or iat, iat
            while True:
                v = pe.u64(q)
                if not v:
                    break
                name = f"#{v & 0xFFFF}" if v >> 63 else pe.cstr((v & 0x7FFFFFFF) + 2)
                pe.imports.append(Import(dll, name, slot))
                q += 8
                slot += 8
            p += 20

    rva, size = d(0)
    if rva:
        o = pe.off(rva)
        _, _, _, _, name_rva, ordbase, nfun, nnames, afun, anames, aords = struct.unpack_from("<IIHHIIIIIII", data, o)
        funcs = [pe.u32(afun + 4 * i) for i in range(nfun)]

        def target(f):
            return pe.cstr(f) if rva <= f < rva + size else f

        for i, f in enumerate(funcs):
            if f:
                pe.exports_ord[ordbase + i] = target(f)
        for i in range(nnames):
            nm = pe.cstr(pe.u32(anames + 4 * i))
            ordi = struct.unpack_from("<H", data, pe.off(aords + 2 * i))[0]
            pe.ord_names[ordbase + ordi] = nm
            if funcs[ordi]:
                pe.exports[nm] = target(funcs[ordi])

    rva, size = d(5)
    p, end = rva, rva + size
    while rva and p < end:
        page, bsize = struct.unpack_from("<II", data, pe.off(p))
        if bsize < 8:
            break
        for i in range((bsize - 8) // 2):
            e = struct.unpack_from("<H", data, pe.off(p + 8 + 2 * i))[0]
            if e >> 12 == 10:
                pe.relocs.append(page + (e & 0xFFF))
        p += bsize

    rva, size = d(3)
    for i in range(size // 12 if rva else 0):
        b, e, u = struct.unpack_from("<III", data, pe.off(rva + 12 * i))
        if b and e > b:
            pe.functions.append((b, e))
            pe.unwind[b] = u

    parent = {}
    for b, u in pe.unwind.items():
        o = pe.off(u)
        if o is None or o + 4 > len(data):
            continue
        flags, ncodes = data[o] >> 3, data[o + 2]
        if flags & 4:
            co = o + 4 + 2 * ((ncodes + 1) & ~1)
            if co + 12 <= len(data):
                parent[b] = struct.unpack_from("<I", data, co)[0]
    for b in pe.unwind:
        r, n = b, 0
        while r in parent and n < 32:
            r, n = parent[r], n + 1
        pe.family[b] = r

    rva, size = d(9)
    if rva:
        start, end, idx, cb = struct.unpack_from("<QQQQ", data, pe.off(rva))
        zero, chars = struct.unpack_from("<II", data, pe.off(rva) + 32)
        pe.tls = {"start": start - image_base, "end": end - image_base, "index": idx - image_base, "zero": zero}
        if cb:
            p = cb - image_base
            while pe.off(p) is not None and pe.u64(p):
                pe.tls_callbacks.append(pe.u64(p) - image_base)
                p += 8
    return pe
