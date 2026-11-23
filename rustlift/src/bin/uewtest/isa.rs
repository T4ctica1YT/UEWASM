//! Instruction-level differential test for the lifter: one small function per instruction form (ml64), each run over
//! random register/flag/memory/xmm states, a hash printed per form. The original exe, the native recompile and
//! (with --wasm) the wasm build must print the same hashes; mismatching forms are listed by name.
use crate::*;
use std::process::Command;
use std::time::Duration;

const GPR: &[&str] = &["rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11"];
const CF: u64 = 1;
const PF: u64 = 4;
const ZF: u64 = 0x40;
const SF: u64 = 0x80;
const OF: u64 = 0x800;
const ALL: u64 = CF | PF | ZF | SF | OF;
const CC: &[&str] = &["o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"];

fn r(n: &str, s: u32) -> String {
    if s == 64 {
        return n.into();
    }
    let i = GPR.iter().position(|g| *g == n).unwrap();
    let t: [&[&str]; 3] = [
        &["eax", "ebx", "ecx", "edx", "esi", "edi", "r8d", "r9d", "r10d", "r11d"],
        &["ax", "bx", "cx", "dx", "si", "di", "r8w", "r9w", "r10w", "r11w"],
        &["al", "bl", "cl", "dl", "sil", "dil", "r8b", "r9b", "r10b", "r11b"],
    ];
    t[match s { 32 => 0, 16 => 1, _ => 2 }][i].into()
}

fn ptr(s: u32) -> &'static str {
    match s { 8 => "byte", 16 => "word", 32 => "dword", _ => "qword" }
}

fn msize(op: &str) -> &'static str {
    let q = ["cvtps2pd", "cvtdq2pd", "movddup", "pmovzxbw", "pmovsxbw", "pmovzxwd", "pmovsxwd", "pmovzxdq", "pmovsxdq", "cvtsd2ss"];
    if q.contains(&op) {
        return "qword";
    }
    if ["pmovzxbd", "pmovsxbd", "pmovzxwq", "pmovsxwq", "cvtss2sd"].contains(&op) {
        return "dword";
    }
    if op == "pmovzxbq" {
        return "word";
    }
    let pc = op.starts_with('p') || op.starts_with("cvt");
    if op.ends_with("ss") && !pc || op == "rcpss" || op == "rsqrtss" {
        return "dword";
    }
    if op.ends_with("sd") && !pc {
        return "qword";
    }
    "xmmword"
}

type Form = (String, Vec<String>, u64);

fn f(name: impl Into<String>, body: &[&str], mask: u64) -> Form {
    (name.into(), body.iter().map(|s| s.to_string()).collect(), mask)
}

fn fv(name: impl Into<String>, body: Vec<String>, mask: u64) -> Form {
    (name.into(), body, mask)
}

fn forms() -> Vec<Form> {
    let mut out: Vec<Form> = vec![];
    for s in [8u32, 16, 32, 64] {
        let (a, b) = (r("rax", s), r("rbx", s));
        let m = format!("{} ptr [r14+8]", ptr(s));
        for op in ["add", "sub", "adc", "sbb", "and", "or", "xor", "cmp", "test"] {
            let fl = ALL;
            out.push(fv(format!("{op}{s}_rr"), vec![format!("{op} {a}, {b}")], fl));
            out.push(fv(format!("{op}{s}_rm"), vec![format!("{op} {a}, {m}")], fl));
            out.push(fv(format!("{op}{s}_mr"), vec![format!("{op} {m}, {a}")], fl));
            let imms: Vec<i64> = if s == 8 { vec![1, 0x7F, -1, -128] } else { vec![1, 0x7F, -1, if s > 16 { 0x12345 } else { 0x1234 }] };
            for imm in imms {
                if op == "test" && imm < 0 && s == 64 {
                    continue;
                }
                out.push(fv(format!("{op}{s}_ri{:x}", imm as u64 & 0xFFFF_FFFF), vec![format!("{op} {a}, {imm}")], fl));
            }
        }
        for op in ["inc", "dec"] {
            out.push(fv(format!("{op}{s}"), vec![format!("{op} {a}")], ALL & !CF));
            out.push(fv(format!("{op}{s}_m"), vec![format!("{op} {m}")], ALL & !CF));
        }
        out.push(fv(format!("neg{s}"), vec![format!("neg {a}")], ALL));
        out.push(fv(format!("not{s}"), vec![format!("not {a}")], 0));
        for op in ["shl", "shr", "sar", "rol", "ror", "rcl", "rcr"] {
            let rot = ["rol", "ror", "rcl", "rcr"].contains(&op);
            let one = if rot { CF | OF } else { ALL };
            let many = if rot { CF } else { CF | PF | ZF | SF };
            out.push(fv(format!("{op}{s}_1"), vec![format!("{op} {a}, 1")], one));
            for k in [3, s - 1, if s < 32 { s + 3 } else if s == 64 { 33 } else { 31 }] {
                out.push(fv(format!("{op}{s}_i{k}"), vec![format!("{op} {a}, {k}")], if k % s != 1 { many } else { 0 }));
            }
            out.push(fv(format!("{op}{s}_cl"), vec![format!("{op} {a}, cl")], 0));
        }
        if s > 8 {
            for op in ["shld", "shrd"] {
                out.push(fv(format!("{op}{s}_i"), vec![format!("{op} {a}, {b}, 5")], CF | ZF | SF | PF));
                out.push(fv(format!("{op}{s}_cl"), vec![format!("{op} {a}, {b}, cl")], 0));
            }
            for op in ["bt", "bts", "btr", "btc"] {
                out.push(fv(format!("{op}{s}_rr"), vec![format!("{op} {a}, {b}")], CF));
                out.push(fv(format!("{op}{s}_ri"), vec![format!("{op} {a}, {}", s - 3)], CF));
            }
            out.push(fv(format!("bsf{s}"), vec![format!("bsf {a}, {b}")], ZF));
            out.push(fv(format!("bsr{s}"), vec![format!("bsr {a}, {b}")], ZF));
            for op in ["lzcnt", "tzcnt", "popcnt"] {
                out.push(fv(format!("{op}{s}"), vec![format!("{op} {a}, {b}")], if op != "popcnt" { CF | ZF } else { ALL }));
            }
            out.push(fv(format!("imul{s}_rr"), vec![format!("imul {a}, {b}")], CF | OF));
            out.push(fv(format!("imul{s}_rri"), vec![format!("imul {a}, {b}, 1234")], CF | OF));
            out.push(fv(format!("imul{s}_rri8"), vec![format!("imul {a}, {b}, -3")], CF | OF));
            for cc in CC {
                out.push(fv(format!("cmov{cc}{s}"), vec![format!("cmov{cc} {a}, {b}")], 0));
            }
            out.push(fv(format!("movzx{s}_8"), vec![format!("movzx {a}, bl")], 0));
            out.push(fv(format!("movsx{s}_8"), vec![format!("movsx {a}, bl")], 0));
            if s > 16 {
                out.push(fv(format!("movzx{s}_16"), vec![format!("movzx {a}, bx")], 0));
                out.push(fv(format!("movsx{s}_16"), vec![format!("movsx {a}, bx")], 0));
            }
        }
        out.push(fv(format!("mul{s}"), vec![format!("mul {b}")], CF | OF));
        out.push(fv(format!("imul{s}_1"), vec![format!("imul {b}")], CF | OF));
        out.push(fv(format!("div{s}"), vec!["xor edx, edx".into(), format!("or {b}, 1"), format!("mov {}, 0", if s > 8 { r("rdx", s) } else { "ah".into() }), format!("div {b}")], 0));
        out.push(fv(format!("idiv{s}"), vec![format!("or {b}, 1"), if s > 8 { format!("and {}, 0Fh", r("rax", s)) } else { "and ax, 0Fh".into() },
            match s { 64 => "cqo", 32 => "cdq", 16 => "cwd", _ => "cbw" }.into(), format!("idiv {b}")], 0));
        out.push(fv(format!("xadd{s}"), vec![format!("xadd {a}, {b}")], ALL));
        out.push(fv(format!("cmpxchg{s}"), vec![format!("cmpxchg {m}, {b}")], ALL));
        out.push(fv(format!("cmpxchg{s}_eq"), vec![format!("mov {m}, {a}"), format!("cmpxchg {m}, {b}")], ALL));
        out.push(fv(format!("xchg{s}"), vec![format!("xchg {a}, {b}")], 0));
    }
    for cc in CC {
        out.push(fv(format!("set{cc}"), vec![format!("set{cc} al")], 0));
        for (suffix, first) in [("cmp", "cmp rax, rbx"), ("sub32", "sub eax, ebx"), ("inc8", "inc bl"), ("test", "test rax, rbx"), ("shl", "shl eax, 3"),
                                ("ucomiss", "ucomiss xmm0, xmm1"), ("ucomisd", "ucomisd xmm0, xmm1"), ("bt", "bt rax, 7"), ("neg", "neg rbx"),
                                ("adc", "adc rax, rbx"), ("sbb16", "sbb ax, bx")] {
            out.push(fv(format!("set{cc}_{suffix}"), vec![first.into(), format!("set{cc} cl")], 0));
        }
    }
    out.extend([f("cbw", &["cbw"], 0), f("cwde", &["cwde"], 0), f("cdqe", &["cdqe"], 0), f("cwd", &["cwd"], 0), f("cdq", &["cdq"], 0), f("cqo", &["cqo"], 0),
        f("bswap32", &["bswap eax"], 0), f("bswap64", &["bswap rax"], 0), f("movsxd", &["movsxd rax, ebx"], 0),
        f("lea1", &["lea rax, [rbx+rcx*4+12345h]"], 0), f("lea32", &["lea eax, [rbx+rcx*8-7]"], 0), f("lea16", &["lea ax, [rbx+rcx]"], 0),
        f("andn", &["andn rax, rbx, rcx"], ZF | SF | CF | OF), f("bextr", &["bextr rax, rbx, rcx"], ZF),
        f("shlx", &["shlx rax, rbx, rcx"], 0), f("shrx", &["shrx rax, rbx, rcx"], 0), f("sarx", &["sarx eax, ebx, ecx"], 0),
        f("rorx", &["rorx rax, rbx, 13"], 0), f("blsr", &["blsr rax, rbx"], ZF | SF | CF), f("blsi", &["blsi rax, rbx"], ZF | SF | CF),
        f("blsmsk", &["blsmsk rax, rbx"], SF | CF), f("bzhi", &["bzhi rax, rbx, rcx"], ZF | SF | CF), f("pdep", &["pdep rax, rbx, rcx"], 0),
        f("pext", &["pext rax, rbx, rcx"], 0), f("mulx", &["mulx rax, rbx, rcx"], 0),
        f("stc", &["stc"], CF), f("clc", &["clc"], CF), f("cmc", &["cmc"], CF), f("lahf", &["lahf"], 0), f("sahf", &["sahf"], ALL & !OF),
        f("movbe", &["movbe eax, dword ptr [r14+4]"], 0),
        // rcx-counted branches (the count is kept to 1-8 so the loops end)
        f("loop", &["and ecx, 7", "inc ecx", "@@:", "add rax, rbx", "loop @B"], 0),
        f("loope", &["and ecx, 7", "inc ecx", "@@:", "inc rax", "test al, 3", "loope @B"], 0),
        f("loopne", &["and ecx, 7", "inc ecx", "@@:", "inc rax", "test al, 3", "loopne @B"], 0),
        f("jrcxz", &["and ecx, 1", "jrcxz @F", "add rax, 5", "@@:"], 0),
        f("jecxz", &["and ecx, 1", "jecxz @F", "add rax, 5", "@@:"], 0)]);
    let ss = ["addss", "subss", "mulss", "divss", "minss", "maxss", "sqrtss", "addsd", "subsd", "mulsd", "divsd", "minsd", "maxsd", "sqrtsd",
        "addps", "subps", "mulps", "divps", "minps", "maxps", "sqrtps", "addpd", "subpd", "mulpd", "divpd", "minpd", "maxpd", "sqrtpd",
        "andps", "andnps", "orps", "xorps", "unpcklps", "unpckhps", "unpcklpd", "unpckhpd", "movss", "movsd", "movhlps", "movlhps",
        "haddps", "hsubps", "haddpd", "addsubps", "rcpps", "rsqrtps", "rcpss", "rsqrtss",
        "paddb", "paddw", "paddd", "paddq", "psubb", "psubw", "psubd", "psubq", "paddsb", "paddsw", "paddusb", "paddusw", "psubsb",
        "psubsw", "psubusb", "psubusw", "pmullw", "pmulhw", "pmulhuw", "pmulld", "pmuludq", "pmuldq", "pmaddwd", "pavgb", "pavgw",
        "pminub", "pmaxub", "pminsw", "pmaxsw", "pminsb", "pmaxsb", "pminuw", "pmaxuw", "pminud", "pmaxud", "pminsd", "pmaxsd",
        "pand", "pandn", "por", "pxor", "pcmpeqb", "pcmpeqw", "pcmpeqd", "pcmpeqq", "pcmpgtb", "pcmpgtw", "pcmpgtd", "pcmpgtq",
        "punpcklbw", "punpcklwd", "punpckldq", "punpcklqdq", "punpckhbw", "punpckhwd", "punpckhdq", "punpckhqdq",
        "packsswb", "packssdw", "packuswb", "packusdw", "psadbw", "pshufb", "pabsb", "pabsw", "pabsd", "psignb", "psignw", "psignd",
        "psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad", "phaddw", "phaddd", "phaddsw", "phsubsw", "phminposuw", "pmaddubsw", "pmulhrsw",
        "cvtps2pd", "cvtpd2ps", "cvtdq2ps", "cvtps2dq", "cvttps2dq", "cvtdq2pd", "cvtpd2dq", "cvttpd2dq", "cvtss2sd", "cvtsd2ss",
        "pmovzxbw", "pmovzxbd", "pmovzxwd", "pmovzxdq", "pmovsxbw", "pmovsxbd", "pmovsxwd", "pmovsxdq", "pmovzxbq", "pmovsxwq",
        "movshdup", "movsldup", "movddup", "pminsd"];
    for op in ss {
        out.push(fv(format!("x_{op}"), vec![format!("{op} xmm0, xmm1")], 0));
        if op != "movhlps" && op != "movlhps" {
            out.push(fv(format!("x_{op}_m"), vec![format!("{op} xmm0, {} ptr [r14+16]", msize(op))], 0));
        }
    }
    for (op, imm) in [("shufps", 0x1B), ("shufps", 0xE4), ("shufpd", 1), ("pshufd", 0x4E), ("pshuflw", 0x1B), ("pshufhw", 0xB1),
                      ("cmpps", 1), ("cmpps", 4), ("cmpss", 2), ("cmppd", 3), ("cmpsd", 0), ("roundps", 1), ("roundss", 0), ("roundsd", 3),
                      ("roundpd", 2), ("blendps", 5), ("pblendw", 0x5A), ("insertps", 0x1D), ("dpps", 0xF1), ("dppd", 0x31), ("dppd", 0xF2), ("palignr", 5),
                      ("pinsrd", 1), ("pinsrq", 1), ("pinsrw", 3), ("pinsrb", 9), ("mpsadbw", 0)] {
        let src = match op { "pinsrd" | "pinsrw" | "pinsrb" => "ebx", "pinsrq" => "rbx", _ => "xmm1" };
        out.push(fv(format!("x_{op}_{imm}"), vec![format!("{op} xmm0, {src}, {imm}")], 0));
    }
    for k in [1, 7, 15, 31, 33, 63, 70] {
        for op in ["psllw", "pslld", "psllq", "psrlw", "psrld", "psrlq", "psraw", "psrad"] {
            out.push(fv(format!("x_{op}_i{k}"), vec![format!("{op} xmm0, {k}")], 0));
        }
    }
    for k in [1, 3, 8, 15, 16] {
        out.push(fv(format!("x_pslldq{k}"), vec![format!("pslldq xmm0, {k}")], 0));
        out.push(fv(format!("x_psrldq{k}"), vec![format!("psrldq xmm0, {k}")], 0));
    }
    for op in ["blendvps", "pblendvb", "blendvpd"] {
        out.push(fv(format!("x_{op}"), vec![format!("{op} xmm1, xmm2, xmm0")], 0));
    }
    out.extend([f("x_ucomiss", &["ucomiss xmm0, xmm1"], ALL), f("x_comiss", &["comiss xmm0, xmm1"], ALL), f("x_ucomisd", &["ucomisd xmm0, xmm1"], ALL),
        f("x_ptest", &["ptest xmm0, xmm1"], ALL), f("x_movmskps", &["movmskps eax, xmm0"], 0), f("x_movmskpd", &["movmskpd eax, xmm0"], 0),
        f("x_pmovmskb", &["pmovmskb eax, xmm0"], 0), f("x_cvttss2si", &["cvttss2si eax, xmm0"], 0), f("x_cvtss2si", &["cvtss2si rax, xmm0"], 0),
        f("x_cvttsd2si", &["cvttsd2si rax, xmm0"], 0), f("x_cvtsd2si", &["cvtsd2si eax, xmm0"], 0), f("x_cvtsi2ss", &["cvtsi2ss xmm0, rbx"], 0),
        f("x_cvtsi2ss32", &["cvtsi2ss xmm0, ebx"], 0), f("x_cvtsi2sd", &["cvtsi2sd xmm0, rbx"], 0), f("x_cvtsi2sd32", &["cvtsi2sd xmm0, ebx"], 0),
        f("x_movd", &["movd xmm0, ebx"], 0), f("x_movq", &["movq xmm0, rbx"], 0), f("x_movd_out", &["movd eax, xmm1"], 0),
        f("x_movq_out", &["movq rax, xmm1"], 0), f("x_pextrw", &["pextrw eax, xmm1, 5"], 0), f("x_pextrd", &["pextrd eax, xmm1, 3"], 0),
        f("x_pextrq", &["pextrq rax, xmm1, 1"], 0), f("x_pextrb", &["pextrb eax, xmm1, 13"], 0), f("x_extractps", &["extractps eax, xmm1, 2"], 0),
        f("x_movq_xx", &["movq xmm0, xmm1"], 0), f("x_movhps", &["movhps xmm0, qword ptr [r14+16]"], 0),
        f("x_movlps", &["movlps xmm0, qword ptr [r14+16]"], 0), f("x_movhps_st", &["movhps qword ptr [r14+8], xmm1"], 0),
        f("x_crc32_8", &["crc32 eax, bl"], 0), f("x_crc32_32", &["crc32 eax, ebx"], 0), f("x_crc32_64", &["crc32 rax, rbx"], 0)]);
    out
}

fn make_asm(fs: &[Form]) -> String {
    let mut l: Vec<String> = vec!["option casemap:none".into(), ".code".into()];
    for (i, (_, body, _)) in fs.iter().enumerate() {
        l.push(format!("snip{i} PROC FRAME"));
        for g in ["rbx", "rbp", "rsi", "rdi", "r12", "r13", "r14", "r15"] {
            l.push(format!("  push {g}"));
            l.push(format!("  .pushreg {g}"));
        }
        l.extend(["  sub rsp, 8", "  .allocstack 8", "  .endprolog", "  mov r15, rcx", "  lea r14, [r15+208]"].iter().map(|s| s.to_string()));
        for (k, g) in GPR.iter().enumerate() {
            if *g != "rcx" {
                l.push(format!("  mov {g}, [r15+{}]", k * 8));
            }
        }
        for x in 0..4 {
            l.push(format!("  movdqu xmm{x}, [r15+144+{}]", x * 16));
        }
        l.extend(["  push qword ptr [r15+128]", "  popfq", "  mov rcx, [r15+16]"].iter().map(|s| s.to_string()));
        l.extend(body.iter().map(|b| format!("  {b}")));
        l.extend(["  pushfq", "  pop qword ptr [r15+136]"].iter().map(|s| s.to_string()));
        for (k, g) in GPR.iter().enumerate() {
            l.push(format!("  mov [r15+{}], {g}", k * 8));
        }
        for x in 0..4 {
            l.push(format!("  movdqu [r15+144+{}], xmm{x}", x * 16));
        }
        l.extend(["  add rsp, 8", "  pop r15", "  pop r14", "  pop r13", "  pop r12", "  pop rdi", "  pop rsi", "  pop rbp", "  pop rbx", "  ret"].iter().map(|s| s.to_string()));
        l.push(format!("snip{i} ENDP"));
    }
    l.push("END".into());
    l.join("\n") + "\n"
}

fn make_c(fs: &[Form], trials: u32) -> String {
    let decl: String = (0..fs.len()).map(|i| format!("void snip{i}(St*);\n")).collect();
    let table = fs.iter().enumerate().map(|(i, (n, _, m))| format!("    {{snip{i}, \"{n}\", 0x{m:x}}}")).collect::<Vec<_>>().join(",\n");
    format!(r#"#include <stdio.h>
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
        printf("%s %016llx\n", T[t].name, (unsigned long long)h);
    }}
    return 0;
}}
"#)
}

fn compile_and_lift(fs: &[Form], out: &std::path::Path, trials: u32) -> (std::path::PathBuf, std::path::PathBuf) {
    std::fs::write(out.join("isa.asm"), make_asm(fs)).unwrap();
    std::fs::write(out.join("isa.c"), make_c(fs, trials)).unwrap();
    let env = msvc("x64");
    let o = run(Command::new(tool(&env, "ml64")).args(["/nologo", "/c", "/Fo"]).arg(out.join("isa_asm.obj")).arg(out.join("isa.asm")).env_clear().envs(&env).current_dir(out),
                Duration::from_secs(600));
    if o.code != 0 {
        let bad: Vec<&str> = o.stdout.lines().filter(|l| l.contains("error")).take(20).collect();
        die(format!("ml64 failed:\n{}", bad.join("\n")));
    }
    let exe = out.join("isa.exe");
    cl(&env, &[s("/nologo"), s("/O1"), s("/MD"), format!("/Fe:{}", exe.display()), format!("/Fo:{}\\", out.display()), util::ps(&out.join("isa.c")),
               util::ps(&out.join("isa_asm.obj"))], out);
    let rec = out.join("recomp");
    recompile(&exe, &rec, 256, &[]);
    (exe, rec)
}

pub fn main(args: &[String]) -> bool {
    let (mut wasm_on, mut filter, mut trials) = (false, String::new(), 200u32);
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--wasm" => wasm_on = true,
            "--filter" => filter = it.next().cloned().unwrap_or_default(),
            "--trials" => trials = it.next().and_then(|x| x.parse().ok()).unwrap_or(200),
            x => die(format!("unknown argument {x}")),
        }
    }
    // approximations and architecturally undefined results differ between CPUs, so they aren't compared
    let undefined = ["rcpps", "rsqrtps", "rcpss", "rsqrtss", "shld16_cl", "shrd16_cl", "x_haddps_m"];
    let mut fs: Vec<Form> = forms().into_iter().filter(|f| f.0.contains(&filter) && !undefined.iter().any(|u| f.0.contains(u))).collect();
    let out = bin_dir("isa");
    let (mut exe, mut rec) = compile_and_lift(&fs, &out, trials);
    let cov: serde_json::Value = serde_json::from_str(&std::fs::read_to_string(rec.join("coverage.json")).unwrap()).unwrap();
    let unsup: Vec<String> = cov["unsupported"].as_object().map(|m| m.keys().map(|k| k.to_lowercase()).collect()).unwrap_or_default();
    let skipped: Vec<String> = fs.iter().filter(|f| f.1.iter().any(|l| unsup.iter().any(|u| l.split_whitespace().next() == Some(u)))).map(|f| f.0.clone()).collect();
    if !skipped.is_empty() {
        println!("not lifted yet ({} forms): {}", skipped.len(), skipped.join(" "));
        fs.retain(|f| !skipped.contains(&f.0));
        (exe, rec) = compile_and_lift(&fs, &out, trials);
    }
    let reference: Vec<String> = run(&mut Command::new(&exe), Duration::from_secs(600)).stdout.lines().map(String::from).collect();
    let nat = native(&rec, false);
    let args = [s("--image"), util::ps(&rec.join("image.bin")), s("--root"), util::ps(&out), s("--exe"), s("isa.exe")];
    let mut results = vec![("native", run(Command::new(&nat).args(&args), Duration::from_secs(600)).stdout)];
    if wasm_on {
        wasm(&rec, &build::Wasm { node_fs: true, ..Default::default() });
        let (n, f) = node();
        results.push(("wasm", run(Command::new(n).args(f).arg("game.js").args(&args).current_dir(rec.join("web")), Duration::from_secs(600)).stdout));
    }
    let mut ok = true;
    for (k, got) in results {
        let got: Vec<&str> = got.lines().collect();
        let mut bad: Vec<String> = reference.iter().zip(&got).filter(|(x, y)| x.as_str() != **y).map(|(x, _)| x.split_whitespace().next().unwrap_or("").to_string()).collect();
        if got.len() < reference.len() {
            bad.push(format!("(stopped after {} of {}: next is {})", got.len(), reference.len(), reference[got.len()].split_whitespace().next().unwrap_or("")));
        }
        ok &= bad.is_empty();
        println!("{k}: {} / {} forms match", reference.len() - bad.len(), reference.len());
        for b in bad.iter().take(80) {
            println!("    {b}");
        }
    }
    ok
}
