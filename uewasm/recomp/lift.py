import bisect
from collections import Counter, defaultdict

import iced_x86 as I

MN = {v: k for k, v in vars(I.Mnemonic).items() if isinstance(v, int)}
RN = {v: k for k, v in vars(I.Register).items() if isinstance(v, int)}
CCN = {v: k for k, v in vars(I.ConditionCode).items() if isinstance(v, int)}
OK = I.OpKind
FC = I.FlowControl

_G64 = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI"] + [f"R{i}" for i in range(8, 16)]
_G32 = ["EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"] + [f"R{i}D" for i in range(8, 16)]
_G16 = ["AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"] + [f"R{i}W" for i in range(8, 16)]
_G8 = ["AL", "CL", "DL", "BL", "SPL", "BPL", "SIL", "DIL"] + [f"R{i}L" for i in range(8, 16)]
REG = {}
for i, n in enumerate(_G64):
    REG[getattr(I.Register, n)] = ("g", i, 8)
for i, n in enumerate(_G32):
    REG[getattr(I.Register, n)] = ("g", i, 4)
for i, n in enumerate(_G16):
    REG[getattr(I.Register, n)] = ("g", i, 2)
for i, n in enumerate(_G8):
    REG[getattr(I.Register, n)] = ("g", i, 1)
for i, n in enumerate(["AH", "CH", "DH", "BH"]):
    REG[getattr(I.Register, n)] = ("h", i, 1)
for i in range(16):
    REG[getattr(I.Register, f"XMM{i}")] = ("x", i, 16)
    REG[getattr(I.Register, f"YMM{i}")] = ("y", i, 32)

IMM = {OK.IMMEDIATE8, OK.IMMEDIATE8_2ND, OK.IMMEDIATE16, OK.IMMEDIATE32, OK.IMMEDIATE64,
       OK.IMMEDIATE8TO16, OK.IMMEDIATE8TO32, OK.IMMEDIATE8TO64, OK.IMMEDIATE32TO64}
MASK = {1: "0xFFull", 2: "0xFFFFull", 4: "0xFFFFFFFFull", 8: "~0ull"}
UT = {1: "uint8_t", 2: "uint16_t", 4: "uint32_t", 8: "uint64_t"}
ST = {1: "int8_t", 2: "int16_t", 4: "int32_t", 8: "int64_t"}


class Unsupported(Exception):
    pass


def h(v):
    return f"0x{v & 0xFFFFFFFFFFFFFFFF:x}ull"


SETJMP = {"_setjmp", "_setjmpex", "setjmp", "_setjmp3", "__intrinsic_setjmp", "__intrinsic_setjmpex"}


class Program:
    def __init__(self, pe, base):
        self.pe = pe
        self.base = base
        self.img = pe.image(base)
        self.text = [s for s in pe.sections if s.executable]
        self.roots = set()
        self.ends = {}
        self.cache = {}
        self.jt = {}
        self.comments = False
        self.unsupported = Counter()
        self.unsupported_at = defaultdict(list)
        self.sj_slots = {base + i.iat_rva for i in pe.imports if i.name in SETJMP}

    def is_setjmp(self, t):
        """t is the IAT slot of _setjmp, or a 'jmp [slot]' import stub for it"""
        if t in self.sj_slots:
            return True
        o = t - self.base
        if 0 <= o < len(self.img) - 6 and self.img[o] == 0xFF and self.img[o + 1] == 0x25:
            return t + 6 + int.from_bytes(self.img[o + 2:o + 6], "little", signed=True) in self.sj_slots
        return False

    def fam(self, a):
        if not hasattr(self, "_fstarts"):
            self._funcs = sorted(self.pe.functions)
            self._fstarts = [b for b, _ in self._funcs]
        rva = a - self.base
        i = bisect.bisect_right(self._fstarts, rva) - 1
        if i < 0 or not (self._funcs[i][0] <= rva < self._funcs[i][1]):
            return None
        return self.pe.family.get(self._funcs[i][0], self._funcs[i][0])

    def flags_live(self, a):
        """code at a may read the status flags before setting them (so a jump there has to hand them over)"""
        cache = self.__dict__.setdefault("_flags_live", {})
        if a in cache:
            return cache[a]
        live, written, p = True, 0, a
        for _ in range(64):
            try:
                ins = self.decode(p)
            except Exception:
                break
            if ins.code == I.Code.INVALID:
                break
            if ins.rflags_read & 0x3F & ~written:
                break
            written |= ins.rflags_modified
            fc = ins.flow_control
            if (written & 0x3F) == 0x3F or fc in (FC.CALL, FC.RETURN):
                live = False
                break
            if fc not in (FC.NEXT,):
                break
            p = ins.next_ip
        cache[a] = live
        return live

    def func_start(self, a):
        """a begins a function of its own (not a chained fragment or a bare jump target)"""
        rva = a - self.base
        return rva in self.pe.unwind and self.pe.family.get(rva, rva) == rva

    def in_code(self, a):
        rva = a - self.base
        return any(s.rva <= rva < s.rva + s.vsize for s in self.text)

    def decode(self, a):
        ins = self.cache.get(a)
        if ins is None:
            rva = a - self.base
            d = I.Decoder(64, self.img[rva:rva + 16], ip=a)
            ins = d.decode()
            self.cache[a] = ins
        return ins

    def discover(self):
        pe = self.pe
        for b, e in pe.functions:
            self.roots.add(self.base + b)
            self.ends[self.base + b] = self.base + e
        self.roots.add(self.base + pe.entry_rva)
        for cb in pe.tls_callbacks:
            self.roots.add(self.base + cb)
        for rva in list(pe.exports.values()) + list(pe.exports_ord.values()):
            if isinstance(rva, int) and self.in_code(self.base + rva):
                self.roots.add(self.base + rva)
        for rva in pe.relocs:
            v = int.from_bytes(self.img[rva:rva + 8], "little")
            if self.in_code(v) and not self.in_code(self.base + rva):
                self.roots.add(v)
        self.starts = sorted(self.roots)
        work = list(self.roots)
        seen = set()
        self.blocks = {}
        while work:
            r = work.pop()
            if r in seen:
                continue
            seen.add(r)
            labels, extra = self.scan(r)
            self.blocks[r] = labels
            for t in extra:
                if t not in self.roots:
                    self.roots.add(t)
                    bisect.insort(self.starts, t)
                    work.append(t)
        # roots found late may sit inside regions scanned earlier: rescan those so no root's code is copied into another,
        # until no rescan turns up a new root
        for _ in range(100):
            stale = [r for r in self.blocks if any(l != r and l in self.roots for l in self.blocks[r])]
            if not stale:
                break
            for r in stale:
                self.blocks[r], extra = self.scan(r)
                work += [t for t in extra if t not in self.roots]
            while work:
                r = work.pop()
                if r in self.roots and r in self.blocks:
                    continue
                self.roots.add(r)
                bisect.insort(self.starts, r)
                self.blocks[r], extra = self.scan(r)
                work += [t for t in extra if t not in self.roots]
        return self

    def region_end(self, r):
        if r in self.ends:
            return self.ends[r]
        i = bisect.bisect_right(self.starts, r)
        nxt = self.starts[i] if i < len(self.starts) else r + 0x100000
        for s in self.text:
            if s.rva <= r - self.base < s.rva + s.vsize:
                nxt = min(nxt, self.base + s.rva + s.vsize)
        return nxt

    def jump_table(self, r, end, insns):
        targets = set()
        bound = None
        for ins in insns:
            if MN[ins.mnemonic] == "CMP" and ins.op_count == 2 and ins.op_kind(0) == OK.REGISTER and ins.op_kind(1) in IMM:
                v = ins.immediate(1)
                if 0 <= v < 4096:
                    bound = v + 1
        for ins in insns:
            if MN[ins.mnemonic] in ("MOV", "MOVSXD") and ins.op_count == 2 and ins.op_kind(1) == OK.MEMORY \
                    and ins.memory_index != I.Register.NONE and ins.memory_index_scale == 4 \
                    and I.MemorySizeInfo(ins.memory_size).size == 4:
                tbl = ins.memory_displacement & 0xFFFFFFFF
                if not (0 < tbl < self.pe.image_size):
                    continue
                for k in range(bound or 4096):
                    p = tbl + 4 * k
                    if p + 4 > len(self.img):
                        break
                    t = self.base + int.from_bytes(self.img[p:p + 4], "little")
                    if bound:
                        if not self.in_code(t):
                            break
                    elif not (r <= t < end) and (self.fam(t) is None or self.fam(t) != self.fam(r)):
                        break
                    targets.add(t)
        # clang style: lea rB, [table]; movsxd rX, [rB + idx*4]; add rX, rB -- entries are relative to the table
        lea = {}
        for ins in insns:
            m = MN[ins.mnemonic]
            if m == "LEA" and ins.is_ip_rel_memory_operand:
                lea[ins.op_register(0)] = ins.ip_rel_memory_address
            elif m == "MOVSXD" and ins.op_kind(1) == OK.MEMORY and ins.memory_base in lea and ins.memory_index_scale == 4 \
                    and ins.memory_displacement == 0:
                tbl = lea[ins.memory_base]
                for k in range(bound or 4096):
                    p = tbl - self.base + 4 * k
                    if not (0 <= p and p + 4 <= len(self.img)):
                        break
                    t = tbl + int.from_bytes(self.img[p:p + 4], "little", signed=True)
                    if not self.in_code(t) or (not bound and not (r <= t < end) and (self.fam(t) is None or self.fam(t) != self.fam(r))):
                        break
                    targets.add(t)
        return targets

    def own(self, r, t, end):
        """t is lifted inside root r (else it is reached by a jump to another root)"""
        return r <= t < end and (t == r or t not in self.roots)

    def scan(self, r):
        end = self.region_end(r)
        labels = {r}
        work = [r]
        done = set()
        extra = set()
        insns = []
        while work:
            a = work.pop()
            while a not in done:
                if a != r and a in self.roots:
                    extra.add(a)
                    break
                done.add(a)
                try:
                    ins = self.decode(a)
                except Exception:
                    break
                if ins.code == I.Code.INVALID:
                    break
                insns.append(ins)
                fc = ins.flow_control
                nxt = ins.next_ip
                if fc == FC.CONDITIONAL_BRANCH:
                    t = ins.near_branch_target
                    if self.own(r, t, end):
                        labels.add(t)
                        work.append(t)
                    else:
                        extra.add(t)
                    if nxt == r or nxt not in self.roots:
                        labels.add(nxt)
                elif fc == FC.UNCONDITIONAL_BRANCH:
                    t = ins.near_branch_target
                    if self.own(r, t, end):
                        labels.add(t)
                        work.append(t)
                    elif self.in_code(t):
                        extra.add(t)
                    break
                elif fc == FC.INDIRECT_BRANCH:
                    tbl = self.jump_table(r, end, insns[-12:])
                    if tbl:
                        self.jt[ins.ip] = sorted(tbl)
                    for t in tbl:
                        if self.own(r, t, end):
                            labels.add(t)
                            work.append(t)
                        else:
                            extra.add(t)
                    break
                elif fc == FC.CALL:
                    t = ins.near_branch_target
                    if self.in_code(t):
                        extra.add(t)
                elif fc in (FC.RETURN, FC.EXCEPTION) or MN[ins.mnemonic] in ("INT3", "UD2", "HLT"):
                    break
                elif fc == FC.INTERRUPT:
                    break
                if MN[ins.mnemonic] == "LEA" and ins.is_ip_rel_memory_operand and self.in_code(ins.ip_rel_memory_address):
                    extra.add(ins.ip_rel_memory_address)
                a = nxt
        self.ends.setdefault(r, end)
        return labels, extra


class Func:
    def __init__(self, prog, root):
        self.p = prog
        self.root = root
        self.labels = prog.blocks[root]
        self.out = []
        self.ins = None

    def emit(self, s):
        self.out.append("    " + s)

    def reg(self, r):
        info = REG.get(r)
        if info is None:
            raise Unsupported(f"reg {RN.get(r, r)}")
        return info

    def size(self, i):
        k = self.ins.op_kind(i)
        if k == OK.REGISTER:
            return self.reg(self.ins.op_register(i))[2]
        if k == OK.MEMORY:
            return I.MemorySizeInfo(self.ins.memory_size).size
        return {OK.IMMEDIATE8: 1, OK.IMMEDIATE16: 2, OK.IMMEDIATE32: 4, OK.IMMEDIATE64: 8}.get(k, 8)

    def addr(self):
        ins = self.ins
        if ins.is_ip_rel_memory_operand:
            a = h(ins.ip_rel_memory_address)
        else:
            parts = []
            if ins.memory_base != I.Register.NONE:
                k, n, sz = self.reg(ins.memory_base)
                parts.append(f"R({n})" if sz == 8 else f"(uint32_t)R({n})")
            if ins.memory_index != I.Register.NONE:
                k, n, sz = self.reg(ins.memory_index)
                v = f"R({n})" if sz == 8 else f"(uint32_t)R({n})"
                parts.append(v if ins.memory_index_scale == 1 else f"{v}*{ins.memory_index_scale}")
            if ins.memory_displacement:
                parts.append(h(ins.memory_displacement))
            a = "(" + "+".join(parts or ["0"]) + ")"
        seg = ins.memory_segment
        if seg in (I.Register.GS, I.Register.FS):
            a = f"(c->gsbase+{a})"
        return a

    def rd(self, i, sz=None):
        ins = self.ins
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            kind, n, rsz = self.reg(ins.op_register(i))
            if kind == "g":
                return f"R({n})" if rsz == 8 else f"(uint64_t)({UT[rsz]})R({n})"
            if kind == "h":
                return f"((R({n})>>8)&0xFF)"
            raise Unsupported("xmm as gpr")
        if k == OK.MEMORY:
            s = sz or self.size(i)
            return f"LD{s * 8}({self.addr()})"
        if k in IMM:
            s = sz or self.size(0)
            return h(ins.immediate(i) & int(MASK[s][:-3] if s < 8 else "0xFFFFFFFFFFFFFFFF", 16))
        raise Unsupported(f"opkind {k}")

    def wr(self, i, v):
        ins = self.ins
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            kind, n, rsz = self.reg(ins.op_register(i))
            if kind == "h":
                self.emit(f"R({n}) = (R({n}) & ~0xFF00ull) | ((uint64_t)(uint8_t)({v}) << 8);")
            elif rsz == 8:
                self.emit(f"R({n}) = {v};")
            elif rsz == 4:
                self.emit(f"R({n}) = (uint32_t)({v});")
            else:
                self.emit(f"R({n}) = (R({n}) & ~{MASK[rsz]}) | ({UT[rsz]})({v});")
        elif k == OK.MEMORY:
            s = self.size(i)
            self.emit(f"ST{s * 8}({self.addr()}, {v});")
        else:
            raise Unsupported("write to non-lvalue")

    def sext(self, v, s):
        return f"(uint64_t)(int64_t)({ST[s]})({v})"

    def xmm(self, i):
        k, n, _ = self.reg(self.ins.op_register(i))
        if k != "x":
            raise Unsupported("ymm")
        return n

    def xsrc(self, i, sz=None):
        ins = self.ins
        if ins.op_kind(i) == OK.REGISTER:
            return f"c->x[{self.xmm(i)}]"
        if ins.op_kind(i) == OK.MEMORY:
            return f"XLD({self.addr()}, {sz or self.size(i)})"
        raise Unsupported("xmm src")

    def target(self, t):
        if t in self.labels:
            return f"goto L_{t:x};"
        # flags can be live across a jump into a fragment, so hand them over
        fl = "SPILL(); " if self.p.func_start(t) or not self.p.flags_live(t) else "rt_xfl = rt_flags(&F); SPILL(); "
        if t in self.p.roots:
            return f"{fl}TAIL F_{t:x}(c); return;"
        return f"{fl}TAIL rt_jump_fn(c, {h(t)})(c); return;"

    def lift(self):
        self.out = []
        order = sorted(self.labels)
        bad = 0
        for lab in order:
            self.out.append(f"L_{lab:x}:")
            a = lab
            while True:
                try:
                    ins = self.p.decode(a)
                except Exception:
                    self.emit(f"SPILL(); rt_trap(c, {h(a)}, \"decode\"); return;")
                    break
                self.ins = ins
                if ins.code == I.Code.INVALID:
                    self.emit(f"SPILL(); rt_trap(c, {h(a)}, \"invalid\"); return;")
                    break
                self.emit(f"/* {a:x}: {ins} */") if self.p.comments else None
                try:
                    stop = self.one(ins)
                except Unsupported as e:
                    m = MN[ins.mnemonic]
                    self.p.unsupported[m] += 1
                    if len(self.p.unsupported_at[m]) < 5:
                        self.p.unsupported_at[m].append(a)
                    self.emit(f"SPILL(); rt_trap(c, {h(a)}, \"{m}: {str(e)[:40]}\"); return;")
                    bad += 1
                    stop = True
                if stop:
                    break
                a = ins.next_ip
                if a in self.labels:
                    self.emit(f"goto L_{a:x};")
                    break
                if a in self.p.roots:
                    # runs on into another root's code
                    self.emit(self.target(a))
                    break
        return bad

    def labels_switch(self, v):
        self.emit(f"switch ({v}) {{")
        for t in sorted(self.labels):
            self.emit(f"case {h(t)}: goto L_{t:x};")
        self.emit(f"default: SPILL(); TAIL rt_jump_fn(c, {v})(c); return;")
        self.emit("}")

    def flags(self, op, s, a, b, r):
        self.emit(f"FL({op}, {s * 8}, {a}, {b}, {r});")

    def one(self, ins):
        m = MN[ins.mnemonic]
        fn = getattr(self, "op_" + m, None)
        if fn is None:
            fn = SSE.get(m)
            if fn is None:
                raise Unsupported("no handler")
            return fn(self, ins, m)
        return fn(ins)

    # ---- data movement
    def op_MOV(self, ins):
        if ins.op_kind(0) == OK.REGISTER and self.reg(ins.op_register(0))[0] == "x":
            raise Unsupported("mov xmm")
        self.wr(0, self.rd(1, self.size(0)))

    def op_MOVZX(self, ins):
        self.wr(0, self.rd(1))

    def op_MOVSX(self, ins):
        self.wr(0, self.sext(self.rd(1), self.size(1)))

    op_MOVSXD = op_MOVSX

    def op_LEA(self, ins):
        self.wr(0, self.addr())

    def op_XCHG(self, ins):
        self.emit(f"{{ uint64_t a_ = {self.rd(0)}, b_ = {self.rd(1)};")
        self.wr(0, "b_")
        self.wr(1, "a_")
        self.emit("}")

    def op_PUSH(self, ins):
        v = self.rd(0, 8) if ins.op_kind(0) not in IMM else h(ins.immediate(0))
        self.emit(f"{{ uint64_t v_ = {v}; R(4) -= 8; ST64(R(4), v_); }}")

    def op_POP(self, ins):
        self.emit("{ uint64_t v_ = LD64(R(4)); R(4) += 8;")
        self.wr(0, "v_")
        self.emit("}")

    def op_PUSHFQ(self, ins):
        self.emit("R(4) -= 8; ST64(R(4), rt_flags(&F));")

    def op_POPFQ(self, ins):
        self.emit("rt_setflags(&F, LD64(R(4))); R(4) += 8;")

    def op_LAHF(self, ins):
        self.emit("R(0) = (R(0) & ~0xFF00ull) | ((rt_flags(&F) & 0xD5) << 8) | 0x200;")

    def op_SAHF(self, ins):
        self.emit("rt_setflags(&F, (rt_flags(&F) & ~0xD5ull) | ((R(0) >> 8) & 0xD5));")

    def op_CLC(self, ins):
        self.emit("rt_setflags(&F, rt_flags(&F) & ~1ull);")

    def op_STC(self, ins):
        self.emit("rt_setflags(&F, rt_flags(&F) | 1);")

    def op_CMC(self, ins):
        self.emit("rt_setflags(&F, rt_flags(&F) ^ 1);")

    def op_CLD(self, ins):
        pass

    def op_NOP(self, ins):
        pass

    def op_PAUSE(self, ins):
        self.emit("SPILL(); rt_pause(c); FILL();")

    op_LFENCE = op_MFENCE = op_SFENCE = op_PREFETCHT0 = op_PREFETCHT1 = op_PREFETCHT2 = op_NOP
    # x87 is only touched for control/status words on x64 (CRT float setup); keep the defaults
    op_FLDCW = op_FNINIT = op_FNCLEX = op_FWAIT = op_FNOP = op_NOP

    def op_FNSTCW(self, ins):
        self.wr(0, "0x27F")

    def op_FNSTSW(self, ins):
        self.wr(0, "0")

    op_FSTCW, op_FSTSW, op_FINIT, op_FCLEX = op_FNSTCW, op_FNSTSW, op_NOP, op_NOP
    op_PREFETCHNTA = op_PREFETCHW = op_ENDBR64 = op_NOP
    # CET shadow stack: without CET these leave the register untouched
    op_RDSSPQ = op_RDSSPD = op_INCSSPQ = op_INCSSPD = op_SETSSBSY = op_SAVEPREVSSP = op_RSTORSSP = op_NOP

    def op_CBW(self, ins):
        self.emit("R(0) = (R(0) & ~0xFFFFull) | (uint16_t)(int16_t)(int8_t)R(0);")

    def op_CWDE(self, ins):
        self.emit("R(0) = (uint32_t)(int32_t)(int16_t)R(0);")

    def op_CDQE(self, ins):
        self.emit("R(0) = (uint64_t)(int64_t)(int32_t)R(0);")

    def op_CWD(self, ins):
        self.emit("R(2) = (R(2) & ~0xFFFFull) | (((int16_t)R(0) < 0) ? 0xFFFF : 0);")

    def op_CDQ(self, ins):
        self.emit("R(2) = ((int32_t)R(0) < 0) ? 0xFFFFFFFFull : 0;")

    def op_CQO(self, ins):
        self.emit("R(2) = ((int64_t)R(0) < 0) ? ~0ull : 0;")

    def op_BSWAP(self, ins):
        s = self.size(0)
        self.wr(0, f"BSWAP{s * 8}({self.rd(0)})")

    # ---- arithmetic
    def _alu(self, ins, op, expr, flag, store=True):
        s = self.size(0)
        a, b = self.rd(0), self.rd(1, s)
        if ins.op_kind(1) in IMM:
            b = h(ins.immediate(1) & ((1 << (8 * s)) - 1))
        self.emit(f"{{ uint64_t a_ = {a}, b_ = {b}, r_ = ({expr}) & {MASK[s]};")
        self.flags(flag, s, "a_", "b_", "r_")
        if store:
            self.wr(0, "r_")
        self.emit("}")

    def op_ADD(self, ins):
        self._alu(ins, "+", "a_ + b_", "F_ADD")

    def op_SUB(self, ins):
        self._alu(ins, "-", "a_ - b_", "F_SUB")

    def op_CMP(self, ins):
        self._alu(ins, "-", "a_ - b_", "F_SUB", False)

    def op_AND(self, ins):
        self._alu(ins, "&", "a_ & b_", "F_LOGIC")

    def op_TEST(self, ins):
        self._alu(ins, "&", "a_ & b_", "F_LOGIC", False)

    def op_OR(self, ins):
        self._alu(ins, "|", "a_ | b_", "F_LOGIC")

    def op_XOR(self, ins):
        if ins.op_kind(0) == OK.REGISTER and ins.op_kind(1) == OK.REGISTER and ins.op_register(0) == ins.op_register(1):
            s = self.size(0)
            self.wr(0, "0")
            self.flags("F_LOGIC", s, "0", "0", "0")
            return
        self._alu(ins, "^", "a_ ^ b_", "F_LOGIC")

    def op_ADC(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t ci_ = CF(&F), a_ = {self.rd(0)}, b_ = {self.rd(1, s)}, r_ = (a_ + b_ + ci_) & {MASK[s]};")
        self.emit(f"F.fcin = ci_; FL(F_ADC, {s * 8}, a_, b_, r_);")
        self.wr(0, "r_")
        self.emit("}")

    def op_SBB(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t ci_ = CF(&F), a_ = {self.rd(0)}, b_ = {self.rd(1, s)}, r_ = (a_ - b_ - ci_) & {MASK[s]};")
        self.emit(f"F.fcin = ci_; FL(F_SBB, {s * 8}, a_, b_, r_);")
        self.wr(0, "r_")
        self.emit("}")

    def op_INC(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t a_ = {self.rd(0)}, r_ = (a_ + 1) & {MASK[s]}; F.fcin = CF(&F); FL(F_INC, {s * 8}, a_, 1, r_);")
        self.wr(0, "r_")
        self.emit("}")

    def op_DEC(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t a_ = {self.rd(0)}, r_ = (a_ - 1) & {MASK[s]}; F.fcin = CF(&F); FL(F_DEC, {s * 8}, a_, 1, r_);")
        self.wr(0, "r_")
        self.emit("}")

    def op_NEG(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t b_ = {self.rd(0)}, r_ = (0 - b_) & {MASK[s]}; FL(F_SUB, {s * 8}, 0, b_, r_);")
        self.wr(0, "r_")
        self.emit("}")

    def op_NOT(self, ins):
        s = self.size(0)
        self.wr(0, f"(~{self.rd(0)}) & {MASK[s]}")

    def _shift(self, ins, kind):
        s = self.size(0)
        cnt = "R(1)" if ins.op_kind(1) == OK.REGISTER else h(ins.immediate(1))
        cm = 63 if s == 8 else 31
        self.emit(f"{{ unsigned n_ = (unsigned)({cnt}) & {cm}; if (n_) {{ uint64_t a_ = {self.rd(0)}, r_;")
        if kind in ("RCL", "RCR"):
            w = s * 8
            mod = f" % {w + 1}" if w < 32 else ""
            self.emit(f"unsigned k_ = n_{mod}; uint64_t fl_ = rt_flags(&F), cf_ = fl_ & 1, of_; r_ = a_;")
            if kind == "RCL":
                self.emit(f"for (unsigned i_ = 0; i_ < k_; i_++) {{ uint64_t o_ = (r_ >> {w - 1}) & 1; r_ = ((r_ << 1) | cf_) & {MASK[s]}; cf_ = o_; }}")
                self.emit(f"of_ = ((r_ >> {w - 1}) & 1) ^ cf_;")
            else:
                self.emit(f"of_ = ((a_ >> {w - 1}) & 1) ^ cf_;")
                self.emit(f"for (unsigned i_ = 0; i_ < k_; i_++) {{ uint64_t o_ = r_ & 1; r_ = (r_ >> 1) | (cf_ << {w - 1}); cf_ = o_; }}")
            self.emit("rt_setflags(&F, (fl_ & ~0x801ull) | cf_ | (of_ << 11));")
        if kind == "SHL":
            self.emit(f"r_ = (a_ << n_) & {MASK[s]}; FL(F_SHL, {s * 8}, a_, n_, r_);")
        elif kind == "SHR":
            self.emit(f"r_ = a_ >> n_; FL(F_SHR, {s * 8}, a_, n_, r_);")
        elif kind == "SAR":
            self.emit(f"r_ = (uint64_t)(({ST[s]})a_ >> (n_ > {s * 8 - 1} ? {s * 8 - 1} : n_)) & {MASK[s]}; FL(F_SAR, {s * 8}, a_, n_, r_);")
        elif kind in ("ROL", "ROR"):
            self.emit(f"unsigned k_ = n_ % {s * 8}; r_ = {kind}{s * 8}(a_, k_);")
            self.emit(f"rt_rotflags(&F, r_, {s * 8}, {1 if kind == 'ROL' else 0});")
        self.wr(0, "r_")
        self.emit("}" + self._zext_on_zero(ins) + " }")

    def _zext_on_zero(self, ins):
        if self.size(0) == 4 and ins.op_kind(0) == OK.REGISTER:
            n = self.reg(ins.op_register(0))[1]
            return f" else R({n}) = (uint32_t)R({n});"
        return ""

    def op_RCL(self, ins):
        self._shift(ins, "RCL")

    def op_RCR(self, ins):
        self._shift(ins, "RCR")

    def op_SHL(self, ins):
        self._shift(ins, "SHL")

    op_SAL = op_SHL

    def op_SHR(self, ins):
        self._shift(ins, "SHR")

    def op_SAR(self, ins):
        self._shift(ins, "SAR")

    def op_ROL(self, ins):
        self._shift(ins, "ROL")

    def op_ROR(self, ins):
        self._shift(ins, "ROR")

    def _shxd(self, ins, left):
        s = self.size(0)
        cnt = "R(1)" if ins.op_kind(2) == OK.REGISTER else h(ins.immediate(2))
        cm = 63 if s == 8 else 31
        w = s * 8
        self.emit(f"{{ unsigned n_ = (unsigned)({cnt}) & {cm}; if (n_) {{ uint64_t a_ = {self.rd(0)}, b_ = {self.rd(1)}, r_;")
        if left:
            self.emit(f"r_ = ((a_ << n_) | (b_ >> ({w} - n_))) & {MASK[s]}; FL(F_SHL, {w}, a_, n_, r_);")
        else:
            self.emit(f"r_ = ((a_ >> n_) | (b_ << ({w} - n_))) & {MASK[s]}; FL(F_SHR, {w}, a_, n_, r_);")
        self.wr(0, "r_")
        self.emit("}" + self._zext_on_zero(ins) + " }")

    def op_SHLD(self, ins):
        self._shxd(ins, True)

    def op_SHRD(self, ins):
        self._shxd(ins, False)

    def op_IMUL(self, ins):
        if ins.op_count == 1:
            s = self.size(0)
            src = self.rd(0)
            if s == 8:
                self.emit(f"{{ int64_t hi_; uint64_t lo_ = MULS64((int64_t)R(0), (int64_t){src}, &hi_); R(0) = lo_; R(2) = (uint64_t)hi_;")
                self.emit("F.fcin = (hi_ != ((int64_t)lo_ >> 63)); FL(F_MUL, 64, 0, 0, lo_); }")
            else:
                w = s * 8
                self.emit(f"{{ int64_t p_ = (int64_t)({ST[s]})R(0) * (int64_t)({ST[s]}){src};")
                if s == 1:
                    self.emit("R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_;")
                elif s == 2:
                    self.emit("R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_; R(2) = (R(2) & ~0xFFFFull) | (uint16_t)(p_ >> 16);")
                else:
                    self.emit("R(0) = (uint32_t)p_; R(2) = (uint32_t)(p_ >> 32);")
                self.emit(f"F.fcin = (p_ != ({ST[s]})p_); FL(F_MUL, {w}, 0, 0, (uint64_t)p_ & {MASK[s]}); }}")
            return
        s = self.size(0)
        a = self.rd(1, s) if ins.op_count == 3 else self.rd(0)
        b = h(ins.immediate(2)) if ins.op_count == 3 else self.rd(1, s)
        w = s * 8
        if s == 8:
            self.emit(f"{{ int64_t hi_; uint64_t r_ = MULS64((int64_t){a}, (int64_t){b}, &hi_); F.fcin = (hi_ != ((int64_t)r_ >> 63));")
        else:
            self.emit(f"{{ int64_t p_ = (int64_t)({ST[s]}){a} * (int64_t)({ST[s]}){b}; uint64_t r_ = (uint64_t)p_ & {MASK[s]}; F.fcin = (p_ != ({ST[s]})p_);")
        self.emit(f"FL(F_MUL, {w}, 0, 0, r_);")
        self.wr(0, "r_")
        self.emit("}")

    def op_MUL(self, ins):
        s = self.size(0)
        src = self.rd(0)
        if s == 8:
            self.emit(f"{{ uint64_t hi_, lo_ = MULU64(R(0), {src}, &hi_); R(0) = lo_; R(2) = hi_; F.fcin = hi_ != 0; FL(F_MUL, 64, 0, 0, lo_); }}")
        elif s == 4:
            self.emit(f"{{ uint64_t p_ = (uint64_t)(uint32_t)R(0) * (uint32_t){src}; R(0) = (uint32_t)p_; R(2) = p_ >> 32; F.fcin = (p_ >> 32) != 0; FL(F_MUL, 32, 0, 0, (uint32_t)p_); }}")
        elif s == 2:
            self.emit(f"{{ uint32_t p_ = (uint32_t)(uint16_t)R(0) * (uint16_t){src}; R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_; R(2) = (R(2) & ~0xFFFFull) | (p_ >> 16); F.fcin = (p_ >> 16) != 0; FL(F_MUL, 16, 0, 0, (uint16_t)p_); }}")
        else:
            self.emit(f"{{ uint32_t p_ = (uint32_t)(uint8_t)R(0) * (uint8_t){src}; R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_; F.fcin = (p_ >> 8) != 0; FL(F_MUL, 8, 0, 0, (uint8_t)p_); }}")

    def _div(self, ins, signed):
        s = self.size(0)
        src = self.rd(0)
        fn = "IDIV" if signed else "DIV"
        self.emit(f"SPILL(); rt_{fn.lower()}{s * 8}(c, {src}); FILL();")

    def op_DIV(self, ins):
        self._div(ins, False)

    def op_IDIV(self, ins):
        self._div(ins, True)

    def op_CMPXCHG(self, ins):
        s = self.size(0)
        acc = f"(R(0) & {MASK[s]})"
        self.emit(f"{{ uint64_t d_ = {self.rd(0)}, r_ = ({acc} - d_) & {MASK[s]}; FL(F_SUB, {s * 8}, {acc}, d_, r_);")
        self.emit(f"if (d_ == {acc}) {{")
        self.wr(0, self.rd(1))
        self.emit("} else {")
        if s == 4:
            self.emit("R(0) = (uint32_t)d_;")
        elif s == 8:
            self.emit("R(0) = d_;")
        else:
            self.emit(f"R(0) = (R(0) & ~{MASK[s]}) | d_;")
        self.emit("} }")

    def op_CMPXCHG16B(self, ins):
        self.emit(f"SPILL(); rt_cmpxchg16b(c, &F, {self.addr()}); FILL();")

    def op_XADD(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t a_ = {self.rd(0)}, b_ = {self.rd(1)}, r_ = (a_ + b_) & {MASK[s]}; FL(F_ADD, {s * 8}, a_, b_, r_);")
        self.wr(1, "a_")
        self.wr(0, "r_")
        self.emit("}")

    def _bt(self, ins, op):
        s = self.size(0)
        if ins.op_kind(0) == OK.MEMORY and ins.op_kind(1) == OK.REGISTER:
            k, n, rs = self.reg(ins.op_register(1))
            off = f"(int64_t)({ST[rs]})R({n})"
            self.emit(f"{{ uint64_t ea_ = {self.addr()} + (({off}) >> 3); unsigned n_ = ({off}) & 7; uint64_t v_ = LD8(ea_);")
            self.emit("rt_setcf(&F, (v_ >> n_) & 1);")
            if op:
                self.emit(f"ST8(ea_, {op.replace('V', 'v_').replace('B', '(1ull << n_)')});")
            self.emit("}")
            return
        bits = s * 8
        cnt = self.rd(1) if ins.op_kind(1) != OK.REGISTER else self.rd(1)
        self.emit(f"{{ uint64_t v_ = {self.rd(0)}; unsigned n_ = (unsigned)({cnt}) & {bits - 1};")
        self.emit("rt_setcf(&F, (v_ >> n_) & 1);")
        if op:
            self.wr(0, op.replace("V", "v_").replace("B", "(1ull << n_)"))
        self.emit("}")

    def op_BT(self, ins):
        self._bt(ins, None)

    def op_BTS(self, ins):
        self._bt(ins, "V | B")

    def op_BTR(self, ins):
        self._bt(ins, "V & ~B")

    def op_BTC(self, ins):
        self._bt(ins, "V ^ B")

    def op_BSF(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t v_ = {self.rd(1)}; rt_setzf(&F, v_ == 0); if (v_) {{")
        self.wr(0, "CTZ64(v_)")
        self.emit("} }")

    def op_BSR(self, ins):
        self.emit(f"{{ uint64_t v_ = {self.rd(1)}; rt_setzf(&F, v_ == 0); if (v_) {{")
        self.wr(0, "63 - CLZ64(v_)")
        self.emit("} }")

    def op_TZCNT(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t v_ = {self.rd(1)}; uint64_t r_ = v_ ? CTZ64(v_) : {s * 8}; rt_setcfzf(&F, v_ == 0, r_ == 0);")
        self.wr(0, "r_")
        self.emit("}")

    def op_LZCNT(self, ins):
        s = self.size(0)
        self.emit(f"{{ uint64_t v_ = {self.rd(1)}; uint64_t r_ = v_ ? CLZ64(v_) - {64 - s * 8} : {s * 8}; rt_setcfzf(&F, v_ == 0, r_ == 0);")
        self.wr(0, "r_")
        self.emit("}")

    def op_POPCNT(self, ins):
        self.emit(f"{{ uint64_t r_ = POPCNT64({self.rd(1)}); rt_setflags(&F, r_ ? 0 : 0x40);")
        self.wr(0, "r_")
        self.emit("}")

    # ---- conditionals
    def cc(self, ins):
        return f"CC_{CCN[ins.condition_code]}(&F)"

    def op_SETcc(self, ins):
        self.wr(0, self.cc(ins))

    def op_CMOVcc(self, ins):
        s = self.size(0)
        self.emit(f"if ({self.cc(ins)}) {{")
        self.wr(0, self.rd(1))
        self.emit("}")
        if s == 4:
            self.emit(f"else {{ {self._zext32(ins)} }}")

    def _zext32(self, ins):
        k, n, _ = self.reg(ins.op_register(0))
        return f"R({n}) = (uint32_t)R({n});"

    # ---- control flow
    def op_Jcc(self, ins):
        t = ins.near_branch_target
        self.emit(f"if ({self.cc(ins)}) {{ {self.target(t)} }}")

    def op_JMP(self, ins):
        if ins.op_kind(0) == OK.NEAR_BRANCH64:
            self.emit(self.target(ins.near_branch_target))
            return True
        v = self.rd(0, 8)
        targets = [t for t in self.p.jt.get(ins.ip, []) if t in self.labels or t in self.p.roots]
        if not targets:
            self.emit(f"{{ static IC ic_; uint64_t t_ = {v}; SPILL(); GuestFn f_ = t_ == ic_.a ? ic_.f : rt_ic_fn(c, t_, &ic_); TAIL f_(c); return; }}")
            return True
        self.emit(f"switch ({v}) {{")
        for t in targets:
            self.emit(f"case {h(t)}: {self.target(t)}")
        self.emit(f"default: SPILL(); TAIL rt_jump_fn(c, {v})(c); return;")
        self.emit("}")
        return True

    def op_CALL(self, ins):
        nxt = h(ins.next_ip)
        sj = ins.near_branch_target if ins.op_kind(0) == OK.NEAR_BRANCH64 else (ins.ip_rel_memory_address if ins.is_ip_rel_memory_operand else 0)
        if sj and self.p.is_setjmp(sj):
            # returns twice: the host setjmp has to live in this frame
            # after a longjmp lands here the runtime has put the restored registers in the CPU struct
            self.emit(f"{{ SPILL(); jmp_buf* sj_ = rt_sj_slot(c, R(1), R(2), {nxt}); if (!setjmp(*sj_)) R(0) = 0; else FILL(); }}")
            return
        if ins.op_kind(0) == OK.NEAR_BRANCH64:
            t = ins.near_branch_target
            call = f"F_{t:x}(c);" if t in self.p.roots else f"rt_call(c, {h(t)});"
            self.emit(f"R(4) -= 8; ST64(R(4), {nxt}); SPILL(); {call} FILLV();")
        else:
            self.emit(f"{{ static IC ic_; uint64_t t_ = {self.rd(0, 8)}; R(4) -= 8; ST64(R(4), {nxt}); SPILL(); if (t_ == ic_.a) ic_.f(c); else rt_call_ic(c, t_, &ic_); FILLV(); }}")

    def op_RET(self, ins):
        extra = ins.immediate(0) if ins.op_count else 0
        self.emit(f"R(4) += {8 + extra}; SPILL(); return;")
        return True

    def op_INT3(self, ins):
        self.emit(f"SPILL(); rt_trap(c, {h(ins.ip)}, \"int3\"); return;")
        return True

    def op_UD2(self, ins):
        self.emit(f"SPILL(); rt_trap(c, {h(ins.ip)}, \"ud2\"); return;")
        return True

    def op_INT(self, ins):
        self.emit(f"SPILL(); rt_trap(c, {h(ins.ip)}, \"int\"); return;")
        return True

    op_HLT = op_INT

    def op_LEAVE(self, ins):
        self.emit("R(4) = R(5); R(5) = LD64(R(4)); R(4) += 8;")

    def op_FXSAVE(self, ins):
        self.emit(f"rt_fxsave(c, {self.addr()});")

    def op_FXSAVE64(self, ins):
        self.op_FXSAVE(ins)

    def op_FXRSTOR(self, ins):
        self.emit(f"rt_fxrstor(c, {self.addr()});")

    def op_FXRSTOR64(self, ins):
        self.op_FXRSTOR(ins)

    def op_CPUID(self, ins):
        self.emit("SPILL(); rt_cpuid(c); FILL();")

    def op_XGETBV(self, ins):
        self.emit("SPILL(); rt_xgetbv(c); FILL();")

    def op_RDTSC(self, ins):
        self.emit("SPILL(); rt_rdtsc(c); FILL();")

    def op_RDTSCP(self, ins):
        self.emit("SPILL(); rt_rdtsc(c); FILL(); R(1) = 0;")

    def _string(self, ins, name):
        s = I.MemorySizeInfo(ins.memory_size).size
        rep = "1" if ins.has_rep_prefix else ("2" if ins.has_repne_prefix else "0")
        fl = "&F, " if name in ("scas", "cmps") else ""
        self.emit(f"SPILL(); rt_{name}(c, {fl}{s}, {rep}); FILL();")

    def op_MOVSB(self, ins):
        self._string(ins, "movs")

    op_MOVSW = op_MOVSQ = op_MOVSB

    def op_STOSB(self, ins):
        self._string(ins, "stos")

    op_STOSW = op_STOSD = op_STOSQ = op_STOSB

    def op_LODSB(self, ins):
        self._string(ins, "lods")

    op_LODSW = op_LODSD = op_LODSQ = op_LODSB

    def op_SCASB(self, ins):
        self._string(ins, "scas")

    op_SCASW = op_SCASD = op_SCASQ = op_SCASB

    def op_CMPSB(self, ins):
        self._string(ins, "cmps")

    op_CMPSW = op_CMPSQ = op_CMPSB

    def op_MOVSD(self, ins):
        if ins.op_count == 0 or ins.op_kind(0) in (OK.MEMORY_ESRDI,):
            return self._string(ins, "movs")
        return SSE["MOVSD"](self, ins, "MOVSD")

    def op_CMPSD(self, ins):
        if ins.op_count == 0 or ins.op_kind(0) != OK.REGISTER:
            return self._string(ins, "cmps")
        return SSE.get("CMPSD")(self, ins, "CMPSD")


for _cc in ("O", "NO", "B", "AE", "E", "NE", "BE", "A", "S", "NS", "P", "NP", "L", "GE", "LE", "G"):
    setattr(Func, "op_SET" + _cc, Func.op_SETcc)
    setattr(Func, "op_CMOV" + _cc, Func.op_CMOVcc)
    setattr(Func, "op_J" + _cc, Func.op_Jcc)
for _a, _b in (("JZ", "E"), ("JNZ", "NE"), ("JC", "B"), ("JNC", "AE")):
    setattr(Func, "op_" + _a, Func.op_Jcc)

from .sse import SSE  # noqa: E402
