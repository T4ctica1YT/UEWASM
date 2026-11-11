"""x87 floating point (32-bit code): the register stack lives in the CPU struct as doubles (MSVC sets 53-bit precision)."""
import iced_x86 as I

from .lift import MN, OK, RN, Func, Unsupported

ST_IDX = {getattr(I.Register, f"ST{i}"): i for i in range(8)}


def st(i):
    return f"FST_({i})"


def _sti(f, k):
    r = f.ins.op_register(k)
    if r not in ST_IDX:
        raise Unsupported("x87 operand")
    return ST_IDX[r]


def _mem_float(f, k=0):
    n = I.MemorySizeInfo(f.ins.memory_size).size
    a = f.addr()
    if n == 4:
        return f"F32B(LD32({a}))"
    if n == 8:
        return f"F64B(LD64({a}))"
    if n == 10:
        return f"F80B(LD64({a}), LD16({a} + 8))"
    raise Unsupported("x87 size")


def _mem_int(f):
    n = I.MemorySizeInfo(f.ins.memory_size).size
    a = f.addr()
    return {2: f"(double)(int16_t)LD16({a})", 4: f"(double)(int32_t)LD32({a})", 8: f"(double)(int64_t)LD64({a})"}[n]


def _store_float(f, v):
    n = I.MemorySizeInfo(f.ins.memory_size).size
    a = f.addr()
    if n == 4:
        f.emit(f"ST32({a}, B32F({v}));")
    elif n == 8:
        f.emit(f"ST64({a}, B64F({v}));")
    elif n == 10:
        f.emit(f"{{ uint64_t m_; uint16_t e_; F80S({v}, &m_, &e_); ST64({a}, m_); ST16({a} + 8, e_); }}")
    else:
        raise Unsupported("x87 size")


def op_FLD(f, ins):
    if ins.op_kind(0) == OK.MEMORY:
        f.emit(f"fpush(c, {_mem_float(f)});")
    else:
        f.emit(f"{{ double v_ = {st(_sti(f, 0))}; fpush(c, v_); }}")


def op_FILD(f, ins):
    f.emit(f"fpush(c, {_mem_int(f)});")


def _fst(f, ins, pop):
    if ins.op_kind(0) == OK.MEMORY:
        _store_float(f, st(0))
    else:
        f.emit(f"{st(_sti(f, 0))} = {st(0)};")
    if pop:
        f.emit("fpop(c);")


def op_FST(f, ins):
    _fst(f, ins, False)


def op_FSTP(f, ins):
    _fst(f, ins, True)


def _fist(f, ins, pop, trunc=False):
    n = I.MemorySizeInfo(ins.memory_size).size
    v = f"fist(c, {st(0)}, {n * 8}, {1 if trunc else 0})"
    f.emit(f"ST{n * 8}({f.addr()}, {v});")
    if pop:
        f.emit("fpop(c);")


def op_FIST(f, ins):
    _fist(f, ins, False)


def op_FISTP(f, ins):
    _fist(f, ins, True)


def op_FISTTP(f, ins):
    _fist(f, ins, True, True)


CONST = {"FLDZ": "0.0", "FLD1": "1.0", "FLDPI": "3.14159265358979323846", "FLDL2E": "1.44269504088896340736",
         "FLDL2T": "3.32192809488736234787", "FLDLG2": "0.30102999566398119521", "FLDLN2": "0.69314718055994530942"}


def op_const(f, ins):
    f.emit(f"fpush(c, {CONST[MN[ins.mnemonic]]});")


ARITH = {"ADD": "{d} + {s}", "SUB": "{d} - {s}", "SUBR": "{s} - {d}", "MUL": "{d} * {s}", "DIV": "{d} / {s}", "DIVR": "{s} / {d}"}


def _arith(f, ins, op, pop, integer=False):
    if ins.op_kind(0) == OK.MEMORY:
        d, s = st(0), (_mem_int(f) if integer else _mem_float(f))
    else:
        d, s = st(_sti(f, 0)), st(_sti(f, 1))
    f.emit(f"{{ double s_ = {s}, d_ = {d}; {d} = {ARITH[op].format(d='d_', s='s_')}; }}")
    if pop:
        f.emit("fpop(c);")


def _make_arith(op, pop, integer):
    return lambda f, ins: _arith(f, ins, op, pop, integer)


def op_FXCH(f, ins):
    i = _sti(f, 1) if ins.op_count > 1 else 1
    f.emit(f"{{ double t_ = {st(0)}; {st(0)} = {st(i)}; {st(i)} = t_; }}")


def _cmp_src(f, ins):
    if ins.op_count == 0:
        return st(1)
    k = ins.op_count - 1
    if ins.op_kind(k) == OK.MEMORY:
        return _mem_float(f)
    return st(_sti(f, k))


def _fcom(f, ins, pops, integer=False):
    src = _mem_int(f) if integer else _cmp_src(f, ins)
    f.emit(f"fcom(c, {st(0)}, {src});")
    for _ in range(pops):
        f.emit("fpop(c);")


def _fcomi(f, ins, pop):
    f.emit(f"rt_ucomi(&F, {st(0)}, {st(_sti(f, 1))});")
    if pop:
        f.emit("fpop(c);")


def op_FTST(f, ins):
    f.emit(f"fcom(c, {st(0)}, 0.0);")


def op_FXAM(f, ins):
    f.emit("fxam(c);")


UNARY = {"FCHS": "-{0}", "FABS": "fabs({0})", "FSQRT": "sqrt({0})", "FSIN": "sin({0})", "FCOS": "cos({0})",
         "FRNDINT": "frnd(c, {0})", "F2XM1": "(exp2({0}) - 1.0)"}


def op_unary(f, ins):
    f.emit(f"{st(0)} = {UNARY[MN[ins.mnemonic]].format(st(0))}; c->fsw &= ~0x400u;")


def op_FPTAN(f, ins):
    f.emit(f"{st(0)} = tan({st(0)}); fpush(c, 1.0); c->fsw &= ~0x400u;")


def op_FSINCOS(f, ins):
    f.emit(f"{{ double v_ = {st(0)}; {st(0)} = sin(v_); fpush(c, cos(v_)); c->fsw &= ~0x400u; }}")


def op_FPATAN(f, ins):
    f.emit(f"{st(1)} = atan2({st(1)}, {st(0)}); fpop(c);")


def op_FYL2X(f, ins):
    f.emit(f"{st(1)} = {st(1)} * log2({st(0)}); fpop(c);")


def op_FYL2XP1(f, ins):
    f.emit(f"{st(1)} = {st(1)} * log2({st(0)} + 1.0); fpop(c);")


def op_FSCALE(f, ins):
    f.emit(f"{st(0)} = ldexp({st(0)}, (int)trunc({st(1)}));")


def op_FPREM(f, ins):
    f.emit(f"{st(0)} = fmod({st(0)}, {st(1)}); c->fsw &= ~0x400u;")


def op_FPREM1(f, ins):
    f.emit(f"{st(0)} = remainder({st(0)}, {st(1)}); c->fsw &= ~0x400u;")


def op_FXTRACT(f, ins):
    f.emit(f"{{ int e_; double m_ = frexp({st(0)}, &e_); {st(0)} = (double)(e_ - 1); fpush(c, m_ * 2.0); }}")


def op_FINCSTP(f, ins):
    f.emit("fpop(c);")


def op_FDECSTP(f, ins):
    f.emit("c->ftop = (c->ftop - 1) & 7;")


def op_FFREE(f, ins):
    pass


def op_FLDCW(f, ins):
    f.emit(f"c->fcw = (uint32_t)LD16({f.addr()});")


def op_FNSTCW(f, ins):
    f.wr(0, "c->fcw")


def op_FNSTSW(f, ins):
    f.wr(0, "fstsw(c)")


def op_FNINIT(f, ins):
    f.emit("c->ftop = 0; c->fsw = 0; c->fcw = 0x27F;")


def op_FNCLEX(f, ins):
    f.emit("c->fsw &= 0x7F00u;")


def op_FNSTENV(f, ins):
    f.emit(f"rt_fnstenv(c, {f.addr()});")


def op_FLDENV(f, ins):
    f.emit(f"rt_fldenv(c, {f.addr()});")


def op_FNSAVE(f, ins):
    f.emit(f"rt_fnsave(c, {f.addr()});")


def op_FRSTOR(f, ins):
    f.emit(f"rt_frstor(c, {f.addr()});")


FCMOV = {"FCMOVB": "CC_B", "FCMOVE": "CC_E", "FCMOVBE": "CC_BE", "FCMOVU": "CC_P",
         "FCMOVNB": "CC_AE", "FCMOVNE": "CC_NE", "FCMOVNBE": "CC_A", "FCMOVNU": "CC_NP"}


def op_fcmov(f, ins):
    f.emit(f"if ({FCMOV[MN[ins.mnemonic]]}(&F)) {st(0)} = {st(_sti(f, 1))};")


def _bind(name, fn):
    setattr(Func, "op_" + name, lambda self, ins, fn=fn: fn(self, ins))


for _n, _fn in list(globals().items()):
    if _n.startswith("op_F") and callable(_fn):
        _bind(_n[3:], _fn)
for _n in CONST:
    _bind(_n, op_const)
for _n in UNARY:
    _bind(_n, op_unary)
for _n in FCMOV:
    _bind(_n, op_fcmov)
for _op in ARITH:
    _bind("F" + _op, _make_arith(_op, False, False))
    _bind("F" + _op + "P", _make_arith(_op, True, False))
    _bind("FI" + _op, _make_arith(_op, False, True))
for _n, _p in (("FCOM", 1 - 1), ("FCOMP", 1), ("FCOMPP", 2), ("FUCOM", 0), ("FUCOMP", 1), ("FUCOMPP", 2)):
    _bind(_n, lambda f, ins, p=_p: _fcom(f, ins, p))
for _n, _p in (("FICOM", 0), ("FICOMP", 1)):
    _bind(_n, lambda f, ins, p=_p: _fcom(f, ins, p, True))
for _n, _p in (("FCOMI", 0), ("FCOMIP", 1), ("FUCOMI", 0), ("FUCOMIP", 1)):
    _bind(_n, lambda f, ins, p=_p: _fcomi(f, ins, p))
for _a, _b in (("FSTSW", "FNSTSW"), ("FSTCW", "FNSTCW"), ("FINIT", "FNINIT"), ("FCLEX", "FNCLEX"), ("FSTENV", "FNSTENV"),
               ("FSAVE", "FNSAVE")):
    setattr(Func, "op_" + _a, getattr(Func, "op_" + _b))
