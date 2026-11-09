import struct
import sys
from pathlib import Path

import iced_x86 as I

from .link import Linker


def main():
    exe, addr = sys.argv[1], int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 12
    link = Linker(exe)
    for m in link.order:
        if m.base <= addr < m.base + m.pe.image_size:
            rva = addr - m.base
            print(f"{m.name} rva 0x{rva:x}")
            fn = [(b, e) for b, e in m.pe.functions if b <= rva < e]
            print("pdata:", [(hex(b), hex(e)) for b, e in fn] or "none")
            img = m.pe.image(m.base)
            d = I.Decoder(64, img[rva:rva + 16 * count], ip=addr)
            fmt = I.Formatter(I.FormatterSyntax.INTEL)
            for _ in range(count):
                ins = d.decode()
                print(f"  {ins.ip:x}: {fmt.format(ins)}")
            needle4 = struct.pack("<I", rva)
            needle8 = struct.pack("<Q", addr)
            for name, pat in (("rva32", needle4), ("abs64", needle8)):
                hits = []
                p = img.find(pat)
                while p >= 0 and len(hits) < 5:
                    hits.append(hex(m.base + p))
                    p = img.find(pat, p + 1)
                print(f"refs as {name}: {hits}")
            return
    print("not in any module")


if __name__ == "__main__":
    main()
