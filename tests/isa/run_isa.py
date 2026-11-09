"""Instruction-level differential test for the lifter.

Generates one small function per instruction form (ml64), runs each over random register/flag/memory/xmm
states, and prints a hash per form. The original exe, the native recompile and (optionally) the wasm build
must print the same hashes; mismatching forms are listed by name.

    python tests/isa/run_isa.py            # native only
    python tests/isa/run_isa.py --wasm     # also wasm (Node)
    python tests/isa/run_isa.py --filter shl
"""
import argparse
import random
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from uewasm.recomp import build  # noqa: E402

GPR = ["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"]
SUB = {
    32: {"rax": "eax", "rbx": "ebx", "rcx": "ecx", "rdx": "edx", "rsi": "esi", "rdi": "edi", "r8": "r8d", "r9": "r9d", "r10": "r10d", "r11": "r11d"},
    16: {"rax": "ax", "rbx": "bx", "rcx": "cx", "rdx": "dx", "rsi": "si", "rdi": "di", "r8": "r8w", "r9": "r9w", "r10": "r10w", "r11": "r11w"},
    8: {"rax": "al", "rbx": "bl", "rcx": "cl", "rdx": "dl", "rsi": "sil", "rdi": "dil", "r8": "r8b", "r9": "r9b", "r10": "r10b", "r11": "r11b"},
}
PTR = {8: "byte", 16: "word", 32: "dword", 64: "qword"}
CF, PF, AF, ZF, SF, OF = 1, 4, 0x10, 0x40, 0x80, 0x800
ALL = CF | PF | ZF | SF | OF
CC = ["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"]


def r(n, s):
    return n if s == 64 else SUB[s][n]


def msize(op):
    q = ("cvtps2pd", "cvtdq2pd", "movddup", "pmovzxbw", "pmovsxbw", "pmovzxwd", "pmovsxwd", "pmovzxdq", "pmovsxdq", "cvtsd2ss")
    if op in q:
        return "qword"
    if op in ("pmovzxbd", "pmovsxbd", "pmovzxwq", "pmovsxwq", "cvtss2sd"):
        return "dword"
    if op == "pmovzxbq":
        return "word"
    if op.endswith("ss") and not op.startswith(("p", "cvt")) or op in ("rcpss", "rsqrtss"):
        return "dword"
    if op.endswith("sd") and not op.startswith(("p", "cvt")):
        return "qword"
    return "xmmword"


def forms():
    """(name, asm lines, defined flag mask)"""
    out = []
    for s in (8, 16, 32, 64):
        a, b = r("rax", s), r("rbx", s)
        m = f"{PTR[s]} ptr [r14+8]"
        for op in ("add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test"):
            fl = ALL if op not in ("and", "or", "xor", "test") else ALL
            out += [(f"{op}{s}_rr", [f"{op} {a}, {b}"], fl), (f"{op}{s}_rm", [f"{op} {a}, {m}"], fl),
                    (f"{op}{s}_mr", [f"{op} {m}, {a}"], fl)]
            for imm in (1, 0x7F, -1, -128) if s == 8 else (1, 0x7F, -1, 0x12345 & ((1 << min(s, 32)) - 1) if s > 16 else 0x1234):
                if op == "test" and imm < 0 and s == 64:
                    continue
                out.append((f"{op}{s}_ri{imm & 0xFFFFFFFF:x}", [f"{op} {a}, {imm}"], fl))
        for op in ("inc", "dec"):
            out += [(f"{op}{s}", [f"{op} {a}"], ALL & ~CF), (f"{op}{s}_m", [f"{op} {m}"], ALL & ~CF)]
        out += [(f"neg{s}", [f"neg {a}"], ALL), (f"not{s}", [f"not {a}"], 0)]
        for op in ("shl", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            rot = op in ("rol", "ror", "rcl", "rcr")
            one = CF | OF if rot else ALL
            many = CF if rot else CF | PF | ZF | SF
            out.append((f"{op}{s}_1", [f"{op} {a}, 1"], one))
            for k in (3, s - 1, s + 3 if s < 32 else 33 if s == 64 else 31):
                out.append((f"{op}{s}_i{k}", [f"{op} {a}, {k}"], many if k % s != 1 else 0))
            out.append((f"{op}{s}_cl", [f"{op} {a}, cl"], 0))
        if s > 8:
            for op in ("shld", "shrd"):
                out.append((f"{op}{s}_i", [f"{op} {a}, {b}, 5"], CF | ZF | SF | PF))
                out.append((f"{op}{s}_cl", [f"{op} {a}, {b}, cl"], 0))
            for op in ("bt", "bts", "btr", "btc"):
                out.append((f"{op}{s}_rr", [f"{op} {a}, {b}"], CF))
                out.append((f"{op}{s}_ri", [f"{op} {a}, {s - 3}"], CF))
            out.append((f"bsf{s}", [f"bsf {a}, {b}"], ZF))
            out.append((f"bsr{s}", [f"bsr {a}, {b}"], ZF))
            for op in ("lzcnt", "tzcnt", "popcnt"):
                out.append((f"{op}{s}", [f"{op} {a}, {b}"], CF | ZF if op != "popcnt" else ALL))
            out.append((f"imul{s}_rr", [f"imul {a}, {b}"], CF | OF))
            out.append((f"imul{s}_rri", [f"imul {a}, {b}, 1234"], CF | OF))
            out.append((f"imul{s}_rri8", [f"imul {a}, {b}, -3"], CF | OF))
            for cc in CC:
                out.append((f"cmov{cc}{s}", [f"cmov{cc} {a}, {b}"], 0))
            out.append((f"movzx{s}_8", [f"movzx {a}, bl"], 0))
            out.append((f"movsx{s}_8", [f"movsx {a}, bl"], 0))
            if s > 16:
                out.append((f"movzx{s}_16", [f"movzx {a}, bx"], 0))
                out.append((f"movsx{s}_16", [f"movsx {a}, bx"], 0))
        out.append((f"mul{s}", [f"mul {b}"], CF | OF))
        out.append((f"imul{s}_1", [f"imul {b}"], CF | OF))
        out.append((f"div{s}", [f"xor edx, edx", f"or {b}, 1", f"mov {r('rdx', s) if s > 8 else 'ah'}, 0", f"div {b}"], 0))
        out.append((f"idiv{s}", [f"or {b}, 1", f"and {r('rax', s)}, 0Fh" if s > 8 else "and ax, 0Fh",
                                 "cqo" if s == 64 else "cdq" if s == 32 else "cwd" if s == 16 else "cbw", f"idiv {b}"], 0))
        out.append((f"xadd{s}", [f"xadd {a}, {b}"], ALL))
        out.append((f"cmpxchg{s}", [f"cmpxchg {m}, {b}"], ALL))
        out.append((f"cmpxchg{s}_eq", [f"mov {m}, {a}", f"cmpxchg {m}, {b}"], ALL))
        out.append((f"xchg{s}", [f"xchg {a}, {b}"], 0))
    for cc in CC:
        out.append((f"set{cc}", [f"set{cc} al"], 0))
        out.append((f"set{cc}_cmp", ["cmp rax, rbx", f"set{cc} cl"], 0))
        out.append((f"set{cc}_sub32", ["sub eax, ebx", f"set{cc} cl"], 0))
        out.append((f"set{cc}_inc8", ["inc bl", f"set{cc} cl"], 0))
        out.append((f"set{cc}_test", ["test rax, rbx", f"set{cc} cl"], 0))
        out.append((f"set{cc}_shl", ["shl eax, 3", f"set{cc} cl"], 0))
        out.append((f"set{cc}_ucomiss", ["ucomiss xmm0, xmm1", f"set{cc} cl"], 0))
        out.append((f"set{cc}_ucomisd", ["ucomisd xmm0, xmm1", f"set{cc} cl"], 0))
        out.append((f"set{cc}_bt", ["bt rax, 7", f"set{cc} cl"], 0))
        out.append((f"set{cc}_neg", ["neg rbx", f"set{cc} cl"], 0))
        out.append((f"set{cc}_adc", ["adc rax, rbx", f"set{cc} cl"], 0))
        out.append((f"set{cc}_sbb16", ["sbb ax, bx", f"set{cc} cl"], 0))
    out += [("cbw", ["cbw"], 0), ("cwde", ["cwde"], 0), ("cdqe", ["cdqe"], 0), ("cwd", ["cwd"], 0), ("cdq", ["cdq"], 0), ("cqo", ["cqo"], 0),
            ("bswap32", ["bswap eax"], 0), ("bswap64", ["bswap rax"], 0), ("movsxd", ["movsxd rax, ebx"], 0),
            ("lea1", ["lea rax, [rbx+rcx*4+12345h]"], 0), ("lea32", ["lea eax, [rbx+rcx*8-7]"], 0), ("lea16", ["lea ax, [rbx+rcx]"], 0),
            ("andn", ["andn rax, rbx, rcx"], ZF | SF | CF | OF), ("bextr", ["bextr rax, rbx, rcx"], ZF),
            ("shlx", ["shlx rax, rbx, rcx"], 0), ("shrx", ["shrx rax, rbx, rcx"], 0), ("sarx", ["sarx eax, ebx, ecx"], 0),
            ("rorx", ["rorx rax, rbx, 13"], 0), ("blsr", ["blsr rax, rbx"], ZF | SF | CF), ("blsi", ["blsi rax, rbx"], ZF | SF | CF),
            ("blsmsk", ["blsmsk rax, rbx"], SF | CF), ("bzhi", ["bzhi rax, rbx, rcx"], ZF | SF | CF), ("pdep", ["pdep rax, rbx, rcx"], 0),
            ("pext", ["pext rax, rbx, rcx"], 0), ("mulx", ["mulx rax, rbx, rcx"], 0),
            ("stc", ["stc"], CF), ("clc", ["clc"], CF), ("cmc", ["cmc"], CF), ("lahf", ["lahf"], 0), ("sahf", ["sahf"], ALL & ~OF),
            ("movbe", ["movbe eax, dword ptr [r14+4]"], 0)]
    ss = ["addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss", "addsd", "subsd", "mulsd", "divsd", "minsd", "maxsd", "sqrtsd",
          "addps", "subps", "mulps", "divps", "minps", "maxps", "sqrtps", "addpd", "subpd", "mulpd", "divpd", "minpd", "maxpd", "sqrtpd",
          "andps", "andnps", "orps", "xorps", "unpcklps", "unpckhps", "unpcklpd", "unpckhpd", "movss", "movsd", "movhlps", "movlhps",
          "haddps", "hsubps", "haddpd", "addsubps", "rcpps", "rsqrtps", "rcpss", "rsqrtss",
          "paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "paddsb", "paddsw", "paddusb", "paddusw", "psubsb",
          "psubsw", "psubusb", "psubusw", "pmullw", "pmulhw", "pmulhuw", "pmulld", "pmuludq", "pmuldq", "pmaddwd", "pavgb", "pavgw",
          "pminub", "pmaxub", "pminsw", "pmaxsw", "pminsb", "pmaxsb", "pminuw", "pmaxuw", "pminud", "pmaxud", "pminsd", "pmaxsd",
          "pand", "pandn", "por", "pxor", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpeqq", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pcmpgtq",
          "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
          "packsswb", "packssdw", "packuswb", "packusdw", "psadbw", "pshufb", "pabsb", "pabsw", "pabsd", "psignb", "psignw", "psignd",
          "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad", "phaddw", "phaddd", "pmaddubsw", "pmulhrsw",
          "cvtps2pd", "cvtpd2ps", "cvtdq2ps", "cvtps2dq", "cvttps2dq", "cvtdq2pd", "cvtpd2dq", "cvttpd2dq", "cvtss2sd", "cvtsd2ss",
          "pmovzxbw", "pmovzxbd", "pmovzxwd", "pmovzxdq", "pmovsxbw", "pmovsxbd", "pmovsxwd", "pmovsxdq", "pmovzxbq", "pmovsxwq",
          "movshdup", "movsldup", "movddup", "pminsd", "ptest_dummy"]
    for op in ss:
        if op == "ptest_dummy":
            continue
        out.append((f"x_{op}", [f"{op} xmm0, xmm1"], 0))
        if op not in ("movhlps", "movlhps"):
            out.append((f"x_{op}_m", [f"{op} xmm0, {msize(op)} ptr [r14+16]"], 0))
    for op, imm in (("shufps", 0x1B), ("shufps", 0xE4), ("shufpd", 1), ("pshufd", 0x4E), ("pshuflw", 0x1B), ("pshufhw", 0xB1),
                    ("cmpps", 1), ("cmpps", 4), ("cmpss", 2), ("cmppd", 3), ("cmpsd", 0), ("roundps", 1), ("roundss", 0), ("roundsd", 3),
                    ("roundpd", 2), ("blendps", 5), ("pblendw", 0x5A), ("insertps", 0x1D), ("dpps", 0xF1), ("palignr", 5),
                    ("pinsrd", 1), ("pinsrq", 1), ("pinsrw", 3), ("pinsrb", 9), ("mpsadbw", 0)):
        if op in ("pinsrd", "pinsrw", "pinsrb"):
            out.append((f"x_{op}_{imm}", [f"{op} xmm0, ebx, {imm}"], 0))
        elif op == "pinsrq":
            out.append((f"x_{op}_{imm}", [f"{op} xmm0, rbx, {imm}"], 0))
        else:
            out.append((f"x_{op}_{imm}", [f"{op} xmm0, xmm1, {imm}"], 0))
    for k in (1, 7, 15, 31, 33, 63, 70):
        for op in ("psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"):
            out.append((f"x_{op}_i{k}", [f"{op} xmm0, {k}"], 0))
    for k in (1, 3, 8, 15, 16):
        out += [(f"x_pslldq{k}", [f"pslldq xmm0, {k}"], 0), (f"x_psrldq{k}", [f"psrldq xmm0, {k}"], 0)]
    for op in ("blendvps", "pblendvb", "blendvpd"):
        out.append((f"x_{op}", [f"{op} xmm1, xmm2, xmm0"], 0))
    out += [("x_ucomiss", ["ucomiss xmm0, xmm1"], ALL), ("x_comiss", ["comiss xmm0, xmm1"], ALL), ("x_ucomisd", ["ucomisd xmm0, xmm1"], ALL),
            ("x_ptest", ["ptest xmm0, xmm1"], ALL), ("x_movmskps", ["movmskps eax, xmm0"], 0), ("x_movmskpd", ["movmskpd eax, xmm0"], 0),
            ("x_pmovmskb", ["pmovmskb eax, xmm0"], 0), ("x_cvttss2si", ["cvttss2si eax, xmm0"], 0), ("x_cvtss2si", ["cvtss2si rax, xmm0"], 0),
            ("x_cvttsd2si", ["cvttsd2si rax, xmm0"], 0), ("x_cvtsd2si", ["cvtsd2si eax, xmm0"], 0), ("x_cvtsi2ss", ["cvtsi2ss xmm0, rbx"], 0),
            ("x_cvtsi2ss32", ["cvtsi2ss xmm0, ebx"], 0), ("x_cvtsi2sd", ["cvtsi2sd xmm0, rbx"], 0), ("x_cvtsi2sd32", ["cvtsi2sd xmm0, ebx"], 0),
            ("x_movd", ["movd xmm0, ebx"], 0), ("x_movq", ["movq xmm0, rbx"], 0), ("x_movd_out", ["movd eax, xmm1"], 0),
            ("x_movq_out", ["movq rax, xmm1"], 0), ("x_pextrw", ["pextrw eax, xmm1, 5"], 0), ("x_pextrd", ["pextrd eax, xmm1, 3"], 0),
            ("x_pextrq", ["pextrq rax, xmm1, 1"], 0), ("x_pextrb", ["pextrb eax, xmm1, 13"], 0), ("x_extractps", ["extractps eax, xmm1, 2"], 0),
            ("x_movq_xx", ["movq xmm0, xmm1"], 0), ("x_movhps", ["movhps xmm0, qword ptr [r14+16]"], 0),
            ("x_movlps", ["movlps xmm0, qword ptr [r14+16]"], 0), ("x_movhps_st", ["movhps qword ptr [r14+8], xmm1"], 0),
            ("x_crc32_8", ["crc32 eax, bl"], 0), ("x_crc32_32", ["crc32 eax, ebx"], 0), ("x_crc32_64", ["crc32 rax, rbx"], 0)]
    return out


def make_asm(fs):
    lines = ["option casemap:none", ".code"]
    for i, (name, body, _) in enumerate(fs):
        lines += [f"snip{i} PROC FRAME", "  push rbx", "  .pushreg rbx", "  push rbp", "  .pushreg rbp", "  push rsi", "  .pushreg rsi",
                  "  push rdi", "  .pushreg rdi", "  push r12", "  .pushreg r12", "  push r13", "  .pushreg r13", "  push r14", "  .pushreg r14",
                  "  push r15", "  .pushreg r15", "  sub rsp, 8", "  .allocstack 8", "  .endprolog", "  mov r15, rcx", "  lea r14, [r15+208]"]
        for k, g in enumerate(GPR):
            if g != "rcx":
                lines.append(f"  mov {g}, [r15+{k * 8}]")
        for x in range(4):
            lines.append(f"  movdqu xmm{x}, [r15+144+{x * 16}]")
        lines += ["  push qword ptr [r15+128]", "  popfq", "  mov rcx, [r15+16]"]
        lines += ["  " + l for l in body]
        lines += ["  pushfq", "  pop qword ptr [r15+136]"]
        for k, g in enumerate(GPR):
            lines.append(f"  mov [r15+{k * 8}], {g}")
        for x in range(4):
            lines.append(f"  movdqu [r15+144+{x * 16}], xmm{x}")
        lines += ["  add rsp, 8", "  pop r15", "  pop r14", "  pop r13", "  pop r12", "  pop rdi", "  pop rsi", "  pop rbp", "  pop rbx", "  ret",
                  f"snip{i} ENDP"]
    lines.append("END")
    return "\n".join(lines) + "\n"


def make_c(fs, trials):
    decl = "".join(f"void snip{i}(St*);\n" for i in range(len(fs)))
    table = ",\n".join(f'    {{snip{i}, "{n}", 0x{m:x}}}' for i, (n, _, m) in enumerate(fs))
    return f"""#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef struct {{ uint64_t r[16]; uint64_t fl_in, fl_out; uint8_t x[4][16]; uint8_t mem[64]; }} St;
{decl}
static const struct {{ void (*f)(St*); const char* name; uint64_t mask; }} T[] = {{
{table}
}};
static uint64_t s = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {{ s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }}
static uint64_t val(void)
{{
    static const uint64_t edge[] = {{ 0, 1, 2, 0x7F, 0x80, 0xFF, 0x7FFF, 0x8000, 0xFFFF, 0x7FFFFFFF, 0x80000000ull, 0xFFFFFFFFull,
        0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, ~0ull, 31, 32, 63, 64 }};
    uint64_t k = rnd() % 8;
    if (k == 0) return edge[rnd() % (sizeof edge / 8)];
    if (k == 1) return rnd() & 0xFF;
    if (k == 2) return (uint64_t)(int64_t)(int8_t)rnd();
    return rnd();
}}
static void fval(uint8_t* p)
{{
    static const float fe[] = {{ 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 1.5f, 2.5f, -2.5f, 1e30f, -1e-30f, 3.0e9f, 65535.75f, 1.0f / 0.0f }};
    for (int i = 0; i < 4; i++) {{
        uint32_t u = (uint32_t)rnd();
        float f;
        switch (rnd() % 4) {{
        case 0: f = fe[rnd() % 13]; memcpy(&u, &f, 4); break;
        case 1: f = (float)((int32_t)(rnd() % 2001) - 1000) / 8.0f; memcpy(&u, &f, 4); break;
        case 2: {{ double d = (double)((int64_t)(rnd() % 20001) - 10000) / 16.0; memcpy(p + (i & 2) * 4, &d, 8); i |= 1; continue; }}
        }}
        memcpy(p + i * 4, &u, 4);
    }}
}}
int main(void)
{{
    St st;
    for (size_t t = 0; t < sizeof T / sizeof T[0]; t++) {{
        uint64_t h = 1469598103934665603ull;
        for (int n = 0; n < {trials}; n++) {{
            memset(&st, 0, sizeof st);
            for (int i = 0; i < 10; i++) st.r[i] = val();
            st.fl_in = 0x202 | (rnd() & 0x8D5);
            for (int i = 0; i < 4; i++) fval(st.x[i]);
            for (int i = 0; i < 64; i += 8) {{ uint64_t v = val(); memcpy(st.mem + i, &v, 8); }}
            if (rnd() % 3 == 0) memcpy(st.mem + 16, st.x[1], 16);
            T[t].f(&st);
            st.fl_out &= T[t].mask;
            const uint8_t* b = (const uint8_t*)&st;
            for (size_t i = 0; i < sizeof st; i++) {{
                if (i >= 128 && i < 136) continue;
                h = (h ^ b[i]) * 1099511628211ull;
            }}
        }}
        printf("%s %016llx\\n", T[t].name, (unsigned long long)h);
    }}
    return 0;
}}
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm", action="store_true")
    ap.add_argument("--filter", default="")
    ap.add_argument("--trials", type=int, default=200)
    a = ap.parse_args()
    # approximations and architecturally undefined results differ between CPUs, so they aren't compared
    undefined = ("rcpps", "rsqrtps", "rcpss", "rsqrtss", "shld16_cl", "shrd16_cl", "x_haddps_m")
    fs = [f for f in forms() if a.filter in f[0] and not any(u in f[0] for u in undefined)]
    out = HERE.parents[0] / "bin" / "isa"
    out.mkdir(parents=True, exist_ok=True)
    exe, rec = compile_and_lift(fs, out, a.trials)
    import json
    unsup = {k.lower() for k in json.loads((rec / "coverage.json").read_text())["unsupported"]}
    skipped = [f[0] for f in fs if any(l.split()[0] in unsup for l in f[1])]
    if skipped:
        print(f"not lifted yet ({len(skipped)} forms): " + " ".join(skipped))
        fs = [f for f in fs if f[0] not in skipped]
        exe, rec = compile_and_lift(fs, out, a.trials)
    run_and_compare(exe, rec, out, a.wasm)


def compile_and_lift(fs, out, trials):
    (out / "isa.asm").write_text(make_asm(fs))
    (out / "isa.c").write_text(make_c(fs, trials))
    env = build.msvc_env()
    r = subprocess.run([build.tool(env, "ml64"), "/nologo", "/c", "/Fo", str(out / "isa_asm.obj"), str(out / "isa.asm")],
                       env=env, capture_output=True, text=True, cwd=out)
    if r.returncode:
        bad = [l for l in r.stdout.splitlines() if "error" in l][:20]
        sys.exit("ml64 failed:\n" + "\n".join(bad))
    exe = out / "isa.exe"
    r = subprocess.run([build.tool(env, "cl"), "/nologo", "/O1", "/MD", f"/Fe:{exe}", f"/Fo:{out}\\", str(out / "isa.c"), str(out / "isa_asm.obj")],
                       env=env, capture_output=True, text=True, cwd=out)
    if r.returncode:
        sys.exit("cl failed:\n" + r.stdout[-2000:])
    rec = out / "recomp"
    build.recompile(exe, rec, mem_mb=256, log=lambda *_: None)
    return exe, rec


def run_and_compare(exe, rec, out, wasm):
    ref = subprocess.run([str(exe)], capture_output=True, text=True).stdout.splitlines()
    native = build.build_native(rec, log=lambda *_: None)
    args = ["--image", str(rec / "image.bin"), "--root", str(out), "--exe", exe.name]
    results = {"native": subprocess.run([str(native), *args], capture_output=True, text=True).stdout.splitlines()}
    if wasm:
        build.build_wasm(rec, log=lambda *_: None, node_fs=True)
        node = build.emsdk_env()[2]
        results["wasm"] = subprocess.run([node, *build.jspi_flag(node), "game.js", *args], capture_output=True, text=True,
                                         cwd=rec / "web").stdout.splitlines()
    for k, got in results.items():
        bad = [x.split()[0] for x, y in zip(ref, got) if x != y]
        if len(got) < len(ref):
            bad.append(f"(stopped after {len(got)} of {len(ref)}: next is {ref[len(got)].split()[0]})")
        print(f"{k}: {len(ref) - len(bad)} / {len(ref)} forms match")
        for b in bad[:80]:
            print("   ", b)


if __name__ == "__main__":
    main()
