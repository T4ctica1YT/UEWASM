import re
from pathlib import Path

import iced_x86 as I

from .lift import IMM, MASK, OK, UT, Unsupported, h

RUNTIME = Path(__file__).resolve().parents[2] / "runtime"
GENERIC = set(re.findall(r"^XFN\((\w+)\)", (RUNTIME / "sse.h").read_text(encoding="utf-8"), re.M))
SSE = {}
SHIFTS = {"PSLLW", "PSLLD", "PSLLQ", "PSRLW", "PSRLD", "PSRLQ", "PSRAW", "PSRAD"}


def sse(*names):
    def wrap(fn):
        for n in names:
            SSE[n] = fn
        return fn
    return wrap


def is_x(f, i):
    return f.ins.op_kind(i) == OK.REGISTER and f.reg(f.ins.op_register(i))[0] == "x"


def is_g(f, i):
    return f.ins.op_kind(i) == OK.REGISTER and f.reg(f.ins.op_register(i))[0] in ("g", "h")


def memsize(f):
    return I.MemorySizeInfo(f.ins.memory_size).size


def store_x(f, i, val, n):
    f.emit(f"XST({f.addr()}, {val}, {n});")


@sse("MOVAPS", "MOVUPS", "MOVAPD", "MOVUPD", "MOVDQA", "MOVDQU", "LDDQU", "MOVNTPS", "MOVNTPD", "MOVNTDQ", "MOVNTDQA",
     "VMOVAPS", "VMOVUPS", "VMOVAPD", "VMOVUPD", "VMOVDQA", "VMOVDQU", "VLDDQU", "VMOVNTPS", "VMOVNTDQ")
def mov128(f, ins, m):
    if is_x(f, 0):
        f.emit(f"c->x[{f.xmm(0)}] = {f.xsrc(1, 16)};")
    else:
        store_x(f, 0, f"c->x[{f.xmm(1)}]", 16)


@sse("MOVSS", "VMOVSS")
def movss(f, ins, m):
    _movs(f, ins, 4)


def _movs(f, ins, n):
    if ins.op_count == 3:
        d, a, b = f.xmm(0), f.xmm(1), f.xmm(2)
        f.emit(f"{{ XMM t_ = c->x[{a}]; memcpy(&t_, &c->x[{b}], {n}); c->x[{d}] = t_; }}")
    elif is_x(f, 0) and is_x(f, 1):
        f.emit(f"memcpy(&c->x[{f.xmm(0)}], &c->x[{f.xmm(1)}], {n});")
    elif is_x(f, 0):
        f.emit(f"c->x[{f.xmm(0)}] = XLD({f.addr()}, {n});")
    else:
        store_x(f, 0, f"c->x[{f.xmm(1)}]", n)


@sse("MOVSD", "VMOVSD")
def movsd(f, ins, m):
    _movs(f, ins, 8)


@sse("MOVD", "MOVQ", "VMOVD", "VMOVQ")
def movdq(f, ins, m):
    n = 4 if m.endswith("MOVD") else 8
    if is_x(f, 0):
        if is_x(f, 1):
            f.emit(f"{{ XMM t_ = {{0}}; t_.q[0] = c->x[{f.xmm(1)}].q[0]; c->x[{f.xmm(0)}] = t_; }}")
        elif ins.op_kind(1) == OK.MEMORY:
            f.emit(f"c->x[{f.xmm(0)}] = XLD({f.addr()}, {n});")
        else:
            f.emit(f"{{ XMM t_ = {{0}}; t_.q[0] = {f.rd(1)} & {MASK[n]}; c->x[{f.xmm(0)}] = t_; }}")
    else:
        v = f"c->x[{f.xmm(1)}].q[0]"
        if ins.op_kind(0) == OK.MEMORY:
            f.emit(f"ST{n * 8}({f.addr()}, {v});")
        else:
            f.wr(0, v if n == 8 else f"(uint32_t){v}")


@sse("MOVLPS", "MOVLPD", "MOVHPS", "MOVHPD")
def movlh(f, ins, m):
    lane = 1 if "H" in m[3] else 0
    if is_x(f, 0):
        f.emit(f"c->x[{f.xmm(0)}].q[{lane}] = LD64({f.addr()});")
    else:
        f.emit(f"ST64({f.addr()}, c->x[{f.xmm(1)}].q[{lane}]);")


@sse("MOVHLPS")
def movhlps(f, ins, m):
    f.emit(f"c->x[{f.xmm(0)}].q[0] = c->x[{f.xmm(1)}].q[1];")


@sse("MOVLHPS")
def movlhps(f, ins, m):
    f.emit(f"c->x[{f.xmm(0)}].q[1] = c->x[{f.xmm(1)}].q[0];")


@sse("MOVMSKPS", "MOVMSKPD", "PMOVMSKB", "VMOVMSKPS", "VMOVMSKPD", "VPMOVMSKB")
def movmsk(f, ins, m):
    f.wr(0, f"x_{m.lstrip('V')}(c->x[{f.xmm(1)}])")


@sse("CVTSI2SS", "CVTSI2SD", "VCVTSI2SS", "VCVTSI2SD")
def cvtsi2(f, ins, m):
    src = 1 if ins.op_count == 2 else 2
    n = f.size(src)
    v = f"(int64_t)(int{n * 8}_t){f.rd(src)}"
    d = f.xmm(0)
    if ins.op_count == 3:
        f.emit(f"c->x[{d}] = c->x[{f.xmm(1)}];")
    if m.endswith("SS"):
        f.emit(f"c->x[{d}].f[0] = (float)({v});")
    else:
        f.emit(f"c->x[{d}].g[0] = (double)({v});")


@sse("CVTTSS2SI", "CVTTSD2SI", "CVTSS2SI", "CVTSD2SI", "VCVTTSS2SI", "VCVTTSD2SI", "VCVTSS2SI", "VCVTSD2SI")
def cvt2si(f, ins, m):
    n = f.size(0)
    sd = "SD2" in m
    src = f.xsrc(1, 8 if sd else 4)
    lane = f"({src}).g[0]" if sd else f"({src}).f[0]"
    fn = "CVTT" if "CVTT" in m else "CVTR"
    f.wr(0, f"{fn}{n * 8}((double){lane})")


@sse("UCOMISS", "COMISS", "VUCOMISS", "VCOMISS")
def ucomiss(f, ins, m):
    f.emit(f"rt_ucomi(&F, c->x[{f.xmm(0)}].f[0], ({f.xsrc(1, 4)}).f[0]);")


@sse("UCOMISD", "COMISD", "VUCOMISD", "VCOMISD")
def ucomisd(f, ins, m):
    f.emit(f"rt_ucomi(&F, c->x[{f.xmm(0)}].g[0], ({f.xsrc(1, 8)}).g[0]);")


@sse("PEXTRB", "PEXTRW", "PEXTRD", "PEXTRQ", "EXTRACTPS", "VPEXTRB", "VPEXTRW", "VPEXTRD", "VPEXTRQ", "VEXTRACTPS")
def pextr(f, ins, m):
    n = {"B": 1, "W": 2, "D": 4, "Q": 8, "S": 4}[m[-1]]
    imm = ins.immediate(2)
    lanes = 16 // n
    v = f"XLANE{n * 8}(c->x[{f.xmm(1)}], {imm % lanes})"
    if ins.op_kind(0) == OK.MEMORY:
        f.emit(f"ST{n * 8}({f.addr()}, {v});")
    else:
        f.wr(0, v)


@sse("PINSRB", "PINSRW", "PINSRD", "PINSRQ")
def pinsr(f, ins, m):
    n = {"B": 1, "W": 2, "D": 4, "Q": 8}[m[-1]]
    imm = ins.immediate(2) % (16 // n)
    v = f"LD{n * 8}({f.addr()})" if ins.op_kind(1) == OK.MEMORY else f.rd(1)
    f.emit(f"XSETLANE{n * 8}(c->x[{f.xmm(0)}], {imm}, {v});")


@sse("PTEST", "VPTEST")
def ptest(f, ins, m):
    f.emit(f"rt_ptest(&F, c->x[{f.xmm(0)}], {f.xsrc(1, 16)});")


@sse("PCMPESTRI", "PCMPISTRI", "PCMPESTRM", "PCMPISTRM", "PCMPESTRI64", "PCMPISTRI64", "PCMPESTRM64", "PCMPISTRM64", "VPCMPESTRI64", "VPCMPESTRM64", "VPCMPESTRI", "VPCMPISTRI", "VPCMPESTRM", "VPCMPISTRM")
def pcmpstr(f, ins, m):
    expl = int("ESTR" in m)
    mask = int(m.endswith("M"))
    w = ins.op_code().operand_size == 64
    la, lb = ("R(0)", "R(2)") if w else ("(int64_t)(int32_t)R(0)", "(int64_t)(int32_t)R(2)")
    call = f"rt_pcmpstr(&F, &c->x[0], c->x[{f.xmm(0)}], {f.xsrc(1, 16)}, {ins.immediate(2) & 0xFF}, {la}, {lb}, {expl}, {mask})"
    if mask:
        f.emit(f"{call};")
    else:
        f.emit(f"R(1) = {call};")


@sse("CRC32")
def crc32(f, ins, m):
    n = f.size(1)
    f.wr(0, f"rt_crc32({f.rd(0)}, {f.rd(1)}, {n})")


@sse("LDMXCSR", "VLDMXCSR")
def ldmxcsr(f, ins, m):
    f.emit(f"c->mxcsr = (uint32_t)LD32({f.addr()});")


@sse("STMXCSR", "VSTMXCSR")
def stmxcsr(f, ins, m):
    f.emit(f"ST32({f.addr()}, c->mxcsr);")


@sse("MOVNTI")
def movnti(f, ins, m):
    f.wr(0, f.rd(1))


@sse("BLENDVPS", "BLENDVPD", "PBLENDVB")
def blendv(f, ins, m):
    src = f"XLD({f.addr()}, 16)" if ins.op_kind(1) == OK.MEMORY else f"c->x[{f.xmm(1)}]"
    f.emit(f"x_{m}(&c->x[{f.xmm(0)}], {src}, c->x[0]);")


@sse("VZEROUPPER", "VZEROALL", "EMMS")
def vzu(f, ins, m):
    pass


def generic(f, ins, m):
    name = m[1:] if m.startswith("V") and m[1:] in GENERIC else m
    if name not in GENERIC:
        raise Unsupported("no helper")
    if not is_x(f, 0):
        raise Unsupported("non-xmm dst")
    imm = ins.op_kind(ins.op_count - 1) in IMM
    nops = ins.op_count - (1 if imm else 0)
    immarg = f", {ins.immediate(ins.op_count - 1) & 0xFF}" if imm else ""
    src_i = nops - 1
    if ins.op_kind(src_i) == OK.MEMORY:
        src = f"XLD({f.addr()}, {memsize(f)})"
    elif ins.op_kind(src_i) == OK.REGISTER and is_x(f, src_i):
        src = f"c->x[{f.xmm(src_i)}]"
    elif ins.op_kind(src_i) in IMM or nops == 1:
        src = "c->x[0]"
    else:
        raise Unsupported("src")
    d = f.xmm(0)
    if imm and name in SHIFTS:
        srcx = f"c->x[{f.xmm(1)}]" if nops == 2 else f"c->x[{d}]"
        f.emit(f"{{ XMM t_ = {srcx}; x_{name}(&t_, immx({ins.immediate(ins.op_count - 1) & 0xFF})); c->x[{d}] = t_; }}")
        return
    if name in ("PSLLDQ", "PSRLDQ"):
        srcx = f"c->x[{f.xmm(1)}]" if nops == 2 else f"c->x[{d}]"
        f.emit(f"x_{name}(&c->x[{d}], {srcx}{immarg});")
        return
    if m.startswith("V") and nops == 3:
        f.emit(f"{{ XMM t_ = c->x[{f.xmm(1)}]; x_{name}(&t_, {src}{immarg}); c->x[{d}] = t_; }}")
    elif m.startswith("V") and nops == 4:
        f.emit(f"{{ XMM t_ = c->x[{f.xmm(1)}]; XMM s_ = {src}; x_{name}(&t_, s_, c->x[{f.xmm(3)}]); c->x[{d}] = t_; }}")
    else:
        if nops == 1:
            f.emit(f"x_{name}(&c->x[{d}], c->x[{d}]{immarg});")
        else:
            f.emit(f"x_{name}(&c->x[{d}], {src}{immarg});")


class _Fallback(dict):
    def get(self, k, default=None):
        v = super().get(k)
        if v is not None:
            return v
        base = k[1:] if k.startswith("V") else k
        if base in GENERIC or k in GENERIC:
            return generic
        return default


SSE = _Fallback(SSE)
