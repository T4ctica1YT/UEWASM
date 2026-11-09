"""Derives DXBC opcode numbers by aligning the token stream with fxc's disassembly."""
import re
import struct
import subprocess
import sys
from collections import Counter, defaultdict
from pathlib import Path

FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\fxc.exe"
votes = defaultdict(Counter)
files = sorted(Path(sys.argv[1]).glob("*.dxbc"))[: int(sys.argv[2]) if len(sys.argv) > 2 else 400]
for f in files:
    blob = f.read_bytes()
    p = blob.find(b"SHEX")
    if p < 0:
        p = blob.find(b"SHDR")
    toks = struct.unpack_from(f"<{struct.unpack_from('<I', blob, p + 4)[0] // 4}I", blob, p + 8)
    ops, i = [], 2
    while i < min(toks[1], len(toks)):
        t = toks[i]
        op = t & 0x7FF
        n = toks[i + 1] if op == 53 else (t >> 24) & 0x7F
        if op != 53:
            ops.append(op)
        if not n:
            break
        i += n
    asm = subprocess.run([FXC, "/nologo", "/dumpbin", str(f)], capture_output=True, text=True).stdout.splitlines()
    names = []
    for line in asm:
        s = line.strip()
        if not s or s.startswith("//") or re.match(r"^(ps|vs|gs|hs|ds|cs)_\d", s) or s.startswith("{") or s.startswith("}") or s.startswith("dcl_immediateConstantBuffer"):
            continue
        if re.match(r"^[0-9a-fx.\-+, ]+[,}]?$", s):
            continue
        names.append(s.split()[0].split("_")[0] if not s.startswith("dcl_") else "_".join(s.split()[0].split("_")[:2]))
    if len(names) != len(ops):
        continue
    for o, nme in zip(ops, names):
        votes[o][nme] += 1
for o in sorted(votes):
    print(o, votes[o].most_common(2))
