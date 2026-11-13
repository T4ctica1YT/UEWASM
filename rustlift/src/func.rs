// One function at a time to C: the Rust form of lift.py's Func (the text it writes is the same, line for line).
use iced_x86::{Code, ConditionCode, Instruction, OpKind, Register};
use std::collections::{BTreeSet, HashMap, HashSet};

use crate::prog::Program;

pub struct Unsup(pub String);
pub type R<T> = Result<T, Unsup>;

fn un<T>(s: impl Into<String>) -> R<T> {
    Err(Unsup(s.into()))
}

/// the mnemonic's name as the Python lifter spells it (upper case), from a table made once
pub fn mn(ins: &Instruction) -> &'static str {
    static NAMES: std::sync::OnceLock<Vec<String>> = std::sync::OnceLock::new();
    let t = NAMES.get_or_init(|| iced_x86::Mnemonic::values().map(|m| format!("{:?}", m).to_uppercase()).collect());
    &t[ins.mnemonic() as usize]
}

const HANDLED: &[&str] = &["JZ", "JNZ", "JC", "JNC", "MOV", "MOVZX", "MOVSX", "MOVSXD", "LEA", "XCHG", "PUSH", "POP", "PUSHAD", "POPAD", "PUSHFD", "POPFD", "PUSHFQ", "POPFQ", "LAHF", "SAHF", "CLC", "STC", "CMC", "CLD", "STD", "NOP", "PAUSE", "LFENCE", "MFENCE", "SFENCE", "PREFETCHT0", "PREFETCHT1", "PREFETCHT2", "FWAIT", "WAIT", "FNOP", "PREFETCHNTA", "PREFETCHW", "ENDBR64", "ENDBR32", "RDSSPQ", "RDSSPD", "INCSSPQ", "INCSSPD", "SETSSBSY", "SAVEPREVSSP", "RSTORSSP", "XLATB", "CBW", "CWDE", "CDQE", "CWD", "CDQ", "CQO", "BSWAP", "ADD", "SUB", "CMP", "AND", "TEST", "OR", "XOR", "ADC", "SBB", "INC", "DEC", "NEG", "NOT", "RCL", "RCR", "SHL", "SAL", "SHR", "SAR", "ROL", "ROR", "SHLD", "SHRD", "IMUL", "MUL", "DIV", "IDIV", "CMPXCHG", "CMPXCHG16B", "XADD", "BT", "BTS", "BTR", "BTC", "BSF", "BSR", "TZCNT", "LZCNT", "POPCNT", "JMP", "CALL", "RET", "INT3", "UD2", "INT", "HLT", "LEAVE", "FXSAVE", "FXSAVE64", "FXRSTOR", "FXRSTOR64", "CPUID", "XGETBV", "RDTSC", "RDTSCP", "MOVSB", "MOVSW", "MOVSQ", "MOVSD", "STOSB", "STOSW", "STOSD", "STOSQ", "LODSB", "LODSW", "LODSD", "LODSQ", "SCASB", "SCASW", "SCASD", "SCASQ", "CMPSB", "CMPSW", "CMPSQ", "CMPSD"];

/// mnemonics with a handler in op() (the x87 ones are in x87::has)
fn handled() -> &'static HashSet<String> {
    static SET: std::sync::OnceLock<HashSet<String>> = std::sync::OnceLock::new();
    SET.get_or_init(|| {
        let mut s: HashSet<String> = HANDLED.iter().map(|x| x.to_string()).collect();
        for c in ["O", "NO", "B", "AE", "E", "NE", "BE", "A", "S", "NS", "P", "NP", "L", "GE", "LE", "G"] {
            s.insert(format!("SET{c}"));
            s.insert(format!("CMOV{c}"));
            s.insert(format!("J{c}"));
        }
        s
    })
}

pub fn is_imm(k: OpKind) -> bool {
    matches!(k, OpKind::Immediate8 | OpKind::Immediate8_2nd | OpKind::Immediate16 | OpKind::Immediate32 | OpKind::Immediate64
        | OpKind::Immediate8to16 | OpKind::Immediate8to32 | OpKind::Immediate8to64 | OpKind::Immediate32to64)
}

/// (kind, number, size): g = general, h = AH..BH, x = XMM, y = YMM
pub fn reg_info(r: Register) -> Option<(char, u32, u32)> {
    use Register::*;
    const G64: [Register; 16] = [RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15];
    const G32: [Register; 16] = [EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI, R8D, R9D, R10D, R11D, R12D, R13D, R14D, R15D];
    const G16: [Register; 16] = [AX, CX, DX, BX, SP, BP, SI, DI, R8W, R9W, R10W, R11W, R12W, R13W, R14W, R15W];
    const G8: [Register; 16] = [AL, CL, DL, BL, SPL, BPL, SIL, DIL, R8L, R9L, R10L, R11L, R12L, R13L, R14L, R15L];
    for (i, x) in G64.iter().enumerate() { if *x == r { return Some(('g', i as u32, 8)); } }
    for (i, x) in G32.iter().enumerate() { if *x == r { return Some(('g', i as u32, 4)); } }
    for (i, x) in G16.iter().enumerate() { if *x == r { return Some(('g', i as u32, 2)); } }
    for (i, x) in G8.iter().enumerate() { if *x == r { return Some(('g', i as u32, 1)); } }
    for (i, x) in [AH, CH, DH, BH].iter().enumerate() { if *x == r { return Some(('h', i as u32, 1)); } }
    let n = r as u32;
    if n >= XMM0 as u32 && n <= XMM15 as u32 { return Some(('x', n - XMM0 as u32, 16)); }
    if n >= YMM0 as u32 && n <= YMM15 as u32 { return Some(('y', n - YMM0 as u32, 32)); }
    Option::None
}

pub fn segval(r: Register) -> Option<u64> {
    match r {
        Register::CS => Some(0x23), Register::SS => Some(0x2B), Register::DS => Some(0x2B), Register::ES => Some(0x2B),
        Register::FS => Some(0x53), Register::GS => Some(0x2B), _ => None,
    }
}

pub fn h(v: u64) -> String {
    format!("0x{:x}ull", v)
}
pub fn mask(s: u32) -> &'static str {
    match s { 1 => "0xFFull", 2 => "0xFFFFull", 4 => "0xFFFFFFFFull", _ => "~0ull" }
}
pub fn maskv(s: u32) -> u64 {
    if s >= 8 { u64::MAX } else { (1u64 << (8 * s)) - 1 }
}
pub fn ut(s: u32) -> &'static str {
    match s { 1 => "uint8_t", 2 => "uint16_t", 4 => "uint32_t", _ => "uint64_t" }
}
pub fn st(s: u32) -> &'static str {
    match s { 1 => "int8_t", 2 => "int16_t", 4 => "int32_t", _ => "int64_t" }
}

pub struct Ctx<'a> {
    pub p: &'a Program,
    pub roots: &'a HashSet<u64>,
    pub generic: &'a HashSet<String>,
}

pub struct Func<'a> {
    pub c: &'a Ctx<'a>,
    pub labels: &'a BTreeSet<u64>,
    pub out: Vec<String>,
    pub ins: Instruction,
    pub cas: bool,
    pub cache: &'a mut HashMap<u64, Instruction>,
    pub fl_cache: &'a mut HashMap<u64, bool>,
    pub unsupported: &'a mut Vec<(String, u64)>,
}

impl<'a> Func<'a> {
    pub fn emit(&mut self, s: impl AsRef<str>) {
        self.out.push(format!("    {}", s.as_ref()));
    }

    pub fn bits(&self) -> u32 { self.c.p.m.bits }
    pub fn ptr(&self) -> u32 { self.c.p.m.bits / 8 }

    pub fn reg(&self, r: Register) -> R<(char, u32, u32)> {
        match reg_info(r) {
            Some(x) => Ok(x),
            None => un(format!("reg {}", if r == Register::None { "NONE".to_string() } else { format!("{:?}", r) })),
        }
    }

    pub fn size(&self, i: u32) -> R<u32> {
        let k = self.ins.op_kind(i);
        if k == OpKind::Register {
            return Ok(self.reg(self.ins.op_register(i))?.2);
        }
        if k == OpKind::Memory {
            return Ok(self.ins.memory_size().size() as u32);
        }
        Ok(match k { OpKind::Immediate8 => 1, OpKind::Immediate16 => 2, OpKind::Immediate32 => 4, OpKind::Immediate64 => 8, _ => 8 })
    }

    pub fn addr(&self) -> R<String> {
        let ins = &self.ins;
        let mut a = if ins.is_ip_rel_memory_operand() {
            h(ins.ip_rel_memory_address())
        } else {
            let mut parts: Vec<String> = vec![];
            if ins.memory_base() != Register::None {
                let (_, n, sz) = self.reg(ins.memory_base())?;
                parts.push(if sz == 8 { format!("R({})", n) } else { format!("(uint32_t)R({})", n) });
            }
            if ins.memory_index() != Register::None {
                let (_, n, sz) = self.reg(ins.memory_index())?;
                let v = if sz == 8 { format!("R({})", n) } else { format!("(uint32_t)R({})", n) };
                parts.push(if ins.memory_index_scale() == 1 { v } else { format!("{}*{}", v, ins.memory_index_scale()) });
            }
            if ins.memory_displacement64() != 0 {
                parts.push(h(ins.memory_displacement64()));
            }
            if parts.is_empty() {
                parts.push("0".into());
            }
            format!("({})", parts.join("+"))
        };
        let seg = ins.memory_segment();
        if seg == Register::GS || seg == Register::FS {
            a = format!("(c->gsbase+{})", a);
        }
        Ok(a)
    }

    pub fn rd(&self, i: u32, sz: Option<u32>) -> R<String> {
        let ins = &self.ins;
        let k = ins.op_kind(i);
        if k == OpKind::Register {
            let (kind, n, rsz) = self.reg(ins.op_register(i))?;
            if kind == 'g' {
                return Ok(if rsz == 8 { format!("R({})", n) } else { format!("(uint64_t)({})R({})", ut(rsz), n) });
            }
            if kind == 'h' {
                return Ok(format!("((R({})>>8)&0xFF)", n));
            }
            return un("xmm as gpr");
        }
        if k == OpKind::Memory {
            if self.cas {
                return Ok("ov_".into());
            }
            let s = match sz { Some(s) => s, None => self.size(i)? };
            return Ok(format!("LD{}({})", s * 8, self.addr()?));
        }
        if is_imm(k) {
            let s = match sz { Some(s) => s, None => self.size(0)? };
            return Ok(h(ins.immediate(i) & maskv(s)));
        }
        un(format!("opkind {}", k as u32))
    }

    pub fn wr(&mut self, i: u32, v: &str) -> R<()> {
        let k = self.ins.op_kind(i);
        if k == OpKind::Register {
            let (kind, n, rsz) = self.reg(self.ins.op_register(i))?;
            if kind == 'h' {
                self.emit(format!("R({n}) = (R({n}) & ~0xFF00ull) | ((uint64_t)(uint8_t)({v}) << 8);"));
            } else if rsz == 8 {
                self.emit(format!("R({n}) = {v};"));
            } else if rsz == 4 {
                self.emit(format!("R({n}) = (uint32_t)({v});"));
            } else {
                self.emit(format!("R({n}) = (R({n}) & ~{}) | ({})({v});", mask(rsz), ut(rsz)));
            }
            Ok(())
        } else if k == OpKind::Memory {
            if self.cas {
                self.emit(format!("nv_ = {v};"));
                return Ok(());
            }
            let s = self.size(i)?;
            let a = self.addr()?;
            self.emit(format!("ST{}({a}, {v});", s * 8));
            Ok(())
        } else {
            un("write to non-lvalue")
        }
    }

    pub fn sext(&self, v: &str, s: u32) -> String {
        format!("(uint64_t)(int64_t)({})({})", st(s), v)
    }

    pub fn xmm(&self, i: u32) -> R<u32> {
        let (k, n, _) = self.reg(self.ins.op_register(i))?;
        if k != 'x' {
            return un("ymm");
        }
        Ok(n)
    }

    pub fn xsrc(&self, i: u32, sz: Option<u32>) -> R<String> {
        let k = self.ins.op_kind(i);
        if k == OpKind::Register {
            return Ok(format!("c->x[{}]", self.xmm(i)?));
        }
        if k == OpKind::Memory {
            let s = match sz { Some(s) => s, None => self.size(i)? };
            return Ok(format!("XLD({}, {})", self.addr()?, s));
        }
        un("xmm src")
    }

    pub fn flags_live(&mut self, a: u64) -> bool {
        if let Some(v) = self.fl_cache.get(&a) {
            return *v;
        }
        let v = self.c.p.flags_live(self.cache, a);
        self.fl_cache.insert(a, v);
        v
    }

    pub fn target(&mut self, t: u64) -> String {
        if self.labels.contains(&t) {
            return format!("goto L_{:x};", t);
        }
        let fl = if self.c.p.func_start(t) || !self.flags_live(t) { "SPILL(); " } else { "rt_xfl = rt_flags(&F); SPILL(); " };
        if self.c.roots.contains(&t) {
            return format!("{fl}TAIL F_{:x}(c); return;", t);
        }
        format!("{fl}TAIL rt_jump_fn(c, {})(c); return;", h(t))
    }

    pub fn lift(&mut self) -> u32 {
        self.out.clear();
        let order: Vec<u64> = self.labels.iter().copied().collect();
        let mut bad = 0;
        for lab in order {
            self.out.push(format!("L_{:x}:", lab));
            let mut a = lab;
            loop {
                let ins = self.c.p.decode(self.cache, a);
                self.ins = ins;
                if ins.code() == Code::INVALID {
                    self.emit(format!("SPILL(); rt_trap(c, {}, \"invalid\"); return;", h(a)));
                    break;
                }
                let stop = match self.one() {
                    Ok(s) => s,
                    Err(Unsup(e)) => {
                        let m = mn(&ins);
                        self.unsupported.push((m.to_string(), a));
                        let e40: String = e.chars().take(40).collect();
                        self.emit(format!("SPILL(); rt_trap(c, {}, \"{}: {}\"); return;", h(a), m, e40));
                        bad += 1;
                        true
                    }
                };
                if stop {
                    break;
                }
                a = ins.next_ip();
                if self.labels.contains(&a) {
                    self.emit(format!("goto L_{:x};", a));
                    break;
                }
                if self.c.roots.contains(&a) {
                    let t = self.target(a);
                    self.emit(t);
                    break;
                }
            }
        }
        bad
    }

    pub fn flags(&mut self, op: &str, s: u32, a: &str, b: &str, r: &str) {
        self.emit(format!("FL({op}, {}, {a}, {b}, {r});", s * 8));
    }

    fn one(&mut self) -> R<bool> {
        let ins = self.ins;
        let m = mn(&ins);
        let mem0 = ins.op_count() > 0 && ins.op_kind(0) == OpKind::Memory;
        let mem1 = ins.op_count() > 1 && ins.op_kind(1) == OpKind::Memory;
        let has_op = self.has_handler(&m);
        if !has_op && crate::sse::lookup(self, &m).is_none() {
            return un("no handler");
        }
        let locked = (mem0 && ins.has_lock_prefix()) || (m == "XCHG" && (mem0 || mem1));
        let mark = self.out.len();
        let r = (|| -> R<bool> {
            if locked && self.atomic(&m, mem0)? {
                return Ok(false);
            }
            if locked {
                let a = self.addr()?;
                self.emit(format!("{{ uint64_t lk_ = {a}; ATOMIC_BEGIN(lk_);"));
            }
            let r = if has_op { self.op(&m)? } else { (crate::sse::lookup(self, &m).unwrap())(self, &m)?; false };
            if locked {
                self.emit("ATOMIC_END(lk_); }");
            }
            Ok(r)
        })();
        if r.is_err() {
            self.out.truncate(mark);
        }
        r
    }

    fn atomic(&mut self, m: &str, mem0: bool) -> R<bool> {
        let ins = self.ins;
        let mi = if mem0 { 0 } else { 1 };
        let s = self.size(mi)?;
        if ![1, 2, 4, 8].contains(&s) {
            return Ok(false);
        }
        let b = s * 8;
        if m == "XCHG" {
            let ri = 1 - mi;
            let a = self.addr()?;
            let v = self.rd(ri, None)?;
            self.emit(format!("{{ uint64_t o_ = ATXCHG{b}({a}, {v});"));
            self.wr(ri, "o_")?;
            self.emit("}");
            return Ok(true);
        }
        if m == "CMPXCHG" {
            let acc = format!("(R(0) & {})", mask(s));
            let a = self.addr()?;
            let src = self.rd(1, None)?;
            self.emit(format!("{{ uint64_t e_ = {acc}, d_ = ATCAS{b}({a}, e_, {src}), r_ = (e_ - d_) & {}; FL(F_SUB, {b}, e_, d_, r_);", mask(s)));
            if s == 4 {
                self.emit("if (d_ != e_) R(0) = (uint32_t)d_; }");
            } else if s == 8 {
                self.emit("if (d_ != e_) R(0) = d_; }");
            } else {
                self.emit(format!("if (d_ != e_) R(0) = (R(0) & ~{}) | d_; }}", mask(s)));
            }
            return Ok(true);
        }
        if m == "XADD" {
            let src = self.rd(1, None)?;
            let a = self.addr()?;
            self.emit(format!("{{ uint64_t b_ = {src}, a_ = ATADD{b}({a}, b_), r_ = (a_ + b_) & {}; FL(F_ADD, {b}, a_, b_, r_);", mask(s)));
            self.wr(1, "a_")?;
            self.emit("}");
            return Ok(true);
        }
        if (m == "BTS" || m == "BTR" || m == "BTC") && ins.op_kind(1) == OpKind::Register {
            let (_, n, rs) = self.reg(ins.op_register(1))?;
            let off = format!("(int64_t)({})R({})", st(rs), n);
            let f = match m { "BTS" => "ATOR8", "BTR" => "ATAND8", _ => "ATXOR8" };
            let bit = if m != "BTR" { "(1ull << n_)" } else { "~(1ull << n_)" };
            let a = self.addr()?;
            self.emit(format!("{{ uint64_t ea_ = {a} + (({off}) >> 3); unsigned n_ = ({off}) & 7; uint64_t v_ = {f}(ea_, {bit});"));
            self.emit("rt_setcf(&F, (v_ >> n_) & 1); }");
            return Ok(true);
        }
        const CAS_LOOP: [&str; 12] = ["ADD", "SUB", "AND", "OR", "XOR", "INC", "DEC", "NOT", "NEG", "BTS", "BTR", "BTC"];
        if CAS_LOOP.contains(&m) && mem0 {
            let mark = self.out.len();
            let a = self.addr()?;
            self.emit(format!("{{ uint64_t lk_ = {a}, ov_, nv_; do {{ ov_ = LD{b}(lk_);"));
            self.cas = true;
            let r = self.op(m);
            self.cas = false;
            r?;
            let body = self.out[mark + 1..].join("\n");
            if body.matches("nv_ = ").count() != 1 || has_ldst(&body) {
                self.out.truncate(mark);
                return Ok(false);
            }
            self.emit(format!("}} while (!ATCASOK{b}(lk_, ov_, nv_)); }}"));
            return Ok(true);
        }
        Ok(false)
    }

    fn cc(&self) -> String {
        let c: ConditionCode = self.ins.condition_code();
        format!("CC_{}(&F)", format!("{:?}", c).to_uppercase())
    }

    fn alu(&mut self, expr: &str, flag: &str, store: bool) -> R<()> {
        let s = self.size(0)?;
        let a = self.rd(0, None)?;
        let mut b = self.rd(1, Some(s))?;
        if is_imm(self.ins.op_kind(1)) {
            b = h(self.ins.immediate(1) & maskv(s));
        }
        self.emit(format!("{{ uint64_t a_ = {a}, b_ = {b}, r_ = ({expr}) & {};", mask(s)));
        self.flags(flag, s, "a_", "b_", "r_");
        if store {
            self.wr(0, "r_")?;
        }
        self.emit("}");
        Ok(())
    }

    fn zext_on_zero(&self) -> R<String> {
        if self.size(0)? == 4 && self.ins.op_kind(0) == OpKind::Register {
            let n = self.reg(self.ins.op_register(0))?.1;
            return Ok(format!(" else R({n}) = (uint32_t)R({n});"));
        }
        Ok(String::new())
    }

    fn shift(&mut self, kind: &str) -> R<()> {
        let ins = self.ins;
        let s = self.size(0)?;
        let cnt = if ins.op_kind(1) == OpKind::Register { "R(1)".to_string() } else { h(ins.immediate(1)) };
        let cm = if s == 8 { 63 } else { 31 };
        let a = self.rd(0, None)?;
        self.emit(format!("{{ unsigned n_ = (unsigned)({cnt}) & {cm}; if (n_) {{ uint64_t a_ = {a}, r_;"));
        let w = s * 8;
        if kind == "RCL" || kind == "RCR" {
            let md = if w < 32 { format!(" % {}", w + 1) } else { String::new() };
            self.emit(format!("unsigned k_ = n_{md}; uint64_t fl_ = rt_flags(&F), cf_ = fl_ & 1, of_; r_ = a_;"));
            if kind == "RCL" {
                self.emit(format!("for (unsigned i_ = 0; i_ < k_; i_++) {{ uint64_t o_ = (r_ >> {}) & 1; r_ = ((r_ << 1) | cf_) & {}; cf_ = o_; }}", w - 1, mask(s)));
                self.emit(format!("of_ = ((r_ >> {}) & 1) ^ cf_;", w - 1));
            } else {
                self.emit(format!("of_ = ((a_ >> {}) & 1) ^ cf_;", w - 1));
                self.emit(format!("for (unsigned i_ = 0; i_ < k_; i_++) {{ uint64_t o_ = r_ & 1; r_ = (r_ >> 1) | (cf_ << {}); cf_ = o_; }}", w - 1));
            }
            self.emit("rt_setflags(&F, (fl_ & ~0x801ull) | cf_ | (of_ << 11));");
        }
        match kind {
            "SHL" => self.emit(format!("r_ = (a_ << n_) & {}; FL(F_SHL, {w}, a_, n_, r_);", mask(s))),
            "SHR" => self.emit(format!("r_ = a_ >> n_; FL(F_SHR, {w}, a_, n_, r_);")),
            "SAR" => self.emit(format!("r_ = (uint64_t)(({})a_ >> (n_ > {} ? {} : n_)) & {}; FL(F_SAR, {w}, a_, n_, r_);", st(s), w - 1, w - 1, mask(s))),
            "ROL" | "ROR" => {
                self.emit(format!("unsigned k_ = n_ % {w}; r_ = {kind}{w}(a_, k_);"));
                self.emit(format!("rt_rotflags(&F, r_, {w}, {});", if kind == "ROL" { 1 } else { 0 }));
            }
            _ => {}
        }
        self.wr(0, "r_")?;
        let z = self.zext_on_zero()?;
        self.emit(format!("}}{z} }}"));
        Ok(())
    }

    fn shxd(&mut self, left: bool) -> R<()> {
        let ins = self.ins;
        let s = self.size(0)?;
        let cnt = if ins.op_kind(2) == OpKind::Register { "R(1)".to_string() } else { h(ins.immediate(2)) };
        let cm = if s == 8 { 63 } else { 31 };
        let w = s * 8;
        let a = self.rd(0, None)?;
        let b = self.rd(1, None)?;
        self.emit(format!("{{ unsigned n_ = (unsigned)({cnt}) & {cm}; if (n_) {{ uint64_t a_ = {a}, b_ = {b}, r_;"));
        if left {
            self.emit(format!("r_ = ((a_ << n_) | (b_ >> ({w} - n_))) & {}; FL(F_SHL, {w}, a_, n_, r_);", mask(s)));
        } else {
            self.emit(format!("r_ = ((a_ >> n_) | (b_ << ({w} - n_))) & {}; FL(F_SHR, {w}, a_, n_, r_);", mask(s)));
        }
        self.wr(0, "r_")?;
        let z = self.zext_on_zero()?;
        self.emit(format!("}}{z} }}"));
        Ok(())
    }

    fn bt(&mut self, op: Option<&str>) -> R<()> {
        let ins = self.ins;
        let s = self.size(0)?;
        if ins.op_kind(0) == OpKind::Memory && ins.op_kind(1) == OpKind::Register {
            let (_, n, rs) = self.reg(ins.op_register(1))?;
            let off = format!("(int64_t)({})R({})", st(rs), n);
            let a = self.addr()?;
            self.emit(format!("{{ uint64_t ea_ = {a} + (({off}) >> 3); unsigned n_ = ({off}) & 7; uint64_t v_ = LD8(ea_);"));
            self.emit("rt_setcf(&F, (v_ >> n_) & 1);");
            if let Some(op) = op {
                self.emit(format!("ST8(ea_, {});", op.replace('V', "v_").replace('B', "(1ull << n_)")));
            }
            self.emit("}");
            return Ok(());
        }
        let bits = s * 8;
        let cnt = self.rd(1, None)?;
        let v = self.rd(0, None)?;
        self.emit(format!("{{ uint64_t v_ = {v}; unsigned n_ = (unsigned)({cnt}) & {};", bits - 1));
        self.emit("rt_setcf(&F, (v_ >> n_) & 1);");
        if let Some(op) = op {
            self.wr(0, &op.replace('V', "v_").replace('B', "(1ull << n_)"))?;
        }
        self.emit("}");
        Ok(())
    }

    fn div(&mut self, signed: bool) -> R<()> {
        let s = self.size(0)?;
        let src = self.rd(0, None)?;
        let f = if signed { "idiv" } else { "div" };
        self.emit(format!("rt_div_ip = {}; SPILL(); rt_{f}{}(c, {src}); FILL();", h(self.ins.ip()), s * 8));
        Ok(())
    }

    fn string(&mut self, name: &str) -> R<bool> {
        let s = self.ins.memory_size().size();
        let rep = if self.ins.has_rep_prefix() { "1" } else if self.ins.has_repne_prefix() { "2" } else { "0" };
        let fl = if name == "scas" || name == "cmps" { "&F, " } else { "" };
        self.emit(format!("SPILL(); rt_{name}(c, {fl}{s}, {rep}); FILL();"));
        Ok(false)
    }

    fn zext32(&self) -> R<String> {
        let n = self.reg(self.ins.op_register(0))?.1;
        Ok(format!("R({n}) = (uint32_t)R({n});"))
    }

    pub fn has_handler(&self, m: &str) -> bool {
        crate::x87::has(m) || handled().contains(m)
    }

    /// the instruction's C; true when it ends the block
    pub fn op(&mut self, m: &str) -> R<bool> {
        let ins = self.ins;
        if crate::x87::has(m) {
            crate::x87::op(self, m)?;
            return Ok(false);
        }
        if (m.starts_with("SET") && m.len() <= 5) || m.starts_with("CMOV") || (m.starts_with('J') && m != "JMP") {
            if m.starts_with("SET") {
                let c = self.cc();
                self.wr(0, &c)?;
            } else if m.starts_with("CMOV") {
                let s = self.size(0)?;
                let c = self.cc();
                self.emit(format!("if ({c}) {{"));
                let v = self.rd(1, None)?;
                self.wr(0, &v)?;
                self.emit("}");
                if s == 4 {
                    let z = self.zext32()?;
                    self.emit(format!("else {{ {z} }}"));
                }
            } else {
                let t = ins.near_branch_target();
                let c = self.cc();
                let tg = self.target(t);
                self.emit(format!("if ({c}) {{ {tg} }}"));
            }
            return Ok(false);
        }
        match m {
            "MOV" => {
                if ins.op_kind(0) == OpKind::Register && segval(ins.op_register(0)).is_some() {
                    return Ok(false);
                }
                if ins.op_kind(1) == OpKind::Register {
                    if let Some(v) = segval(ins.op_register(1)) {
                        self.wr(0, &h(v))?;
                        return Ok(false);
                    }
                }
                if ins.op_kind(0) == OpKind::Register && self.reg(ins.op_register(0))?.0 == 'x' {
                    return un("mov xmm");
                }
                let s = self.size(0)?;
                let v = self.rd(1, Some(s))?;
                self.wr(0, &v)?;
            }
            "MOVZX" => { let v = self.rd(1, None)?; self.wr(0, &v)?; }
            "MOVSX" | "MOVSXD" => { let s = self.size(1)?; let v = self.rd(1, None)?; let v = self.sext(&v, s); self.wr(0, &v)?; }
            "LEA" => { let a = self.addr()?; self.wr(0, &a)?; }
            "XCHG" => {
                let a = self.rd(0, None)?;
                let b = self.rd(1, None)?;
                self.emit(format!("{{ uint64_t a_ = {a}, b_ = {b};"));
                self.wr(0, "b_")?;
                self.wr(1, "a_")?;
                self.emit("}");
            }
            "PUSH" => {
                if self.bits() == 32 {
                    let v = if !is_imm(ins.op_kind(0)) { self.rd(0, Some(4))? } else { h(ins.immediate(0) & 0xFFFFFFFF) };
                    self.emit(format!("{{ uint64_t v_ = {v}; R(4) -= 4; ST32(R(4), v_); }}"));
                } else {
                    let v = if !is_imm(ins.op_kind(0)) { self.rd(0, Some(8))? } else { h(ins.immediate(0)) };
                    self.emit(format!("{{ uint64_t v_ = {v}; R(4) -= 8; ST64(R(4), v_); }}"));
                }
            }
            "POP" => {
                if self.bits() == 32 {
                    self.emit("{ uint64_t v_ = LD32(R(4)); R(4) += 4;");
                } else {
                    self.emit("{ uint64_t v_ = LD64(R(4)); R(4) += 8;");
                }
                self.wr(0, "v_")?;
                self.emit("}");
            }
            "PUSHAD" => self.emit("{ uint64_t s_ = R(4); R(4) -= 32; ST32(R(4) + 28, R(0)); ST32(R(4) + 24, R(1)); ST32(R(4) + 20, R(2)); ST32(R(4) + 16, R(3)); ST32(R(4) + 12, s_); ST32(R(4) + 8, R(5)); ST32(R(4) + 4, R(6)); ST32(R(4), R(7)); }"),
            "POPAD" => self.emit("R(7) = LD32(R(4)); R(6) = LD32(R(4) + 4); R(5) = LD32(R(4) + 8); R(3) = LD32(R(4) + 16); R(2) = LD32(R(4) + 20); R(1) = LD32(R(4) + 24); R(0) = LD32(R(4) + 28); R(4) += 32;"),
            "PUSHFD" => self.emit("R(4) -= 4; ST32(R(4), rt_flags(&F));"),
            "POPFD" => self.emit("rt_setflags(&F, LD32(R(4))); R(4) += 4;"),
            "PUSHFQ" => self.emit("R(4) -= 8; ST64(R(4), rt_flags(&F));"),
            "POPFQ" => self.emit("rt_setflags(&F, LD64(R(4))); R(4) += 8;"),
            "LAHF" => self.emit("R(0) = (R(0) & ~0xFF00ull) | ((rt_flags(&F) & 0xD5) << 8) | 0x200;"),
            "SAHF" => self.emit("rt_setflags(&F, (rt_flags(&F) & ~0xD5ull) | ((R(0) >> 8) & 0xD5));"),
            "CLC" => self.emit("rt_setflags(&F, rt_flags(&F) & ~1ull);"),
            "STC" => self.emit("rt_setflags(&F, rt_flags(&F) | 1);"),
            "CMC" => self.emit("rt_setflags(&F, rt_flags(&F) ^ 1);"),
            "CLD" => { if self.bits() == 32 { self.emit("c->df = 0;"); } }
            "STD" => self.emit("c->df = 1;"),
            "NOP" | "LFENCE" | "MFENCE" | "SFENCE" | "PREFETCHT0" | "PREFETCHT1" | "PREFETCHT2" | "FWAIT" | "WAIT" | "FNOP"
            | "PREFETCHNTA" | "PREFETCHW" | "ENDBR64" | "ENDBR32" | "RDSSPQ" | "RDSSPD" | "INCSSPQ" | "INCSSPD" | "SETSSBSY"
            | "SAVEPREVSSP" | "RSTORSSP" => {}
            "PAUSE" => self.emit("SPILL(); rt_pause(c); FILL();"),
            "XLATB" => {
                let base = if self.bits() == 32 { "(uint32_t)R(3)" } else { "R(3)" };
                self.emit(format!("R(0) = (R(0) & ~0xFFull) | LD8({base} + (uint8_t)R(0));"));
            }
            "CBW" => self.emit("R(0) = (R(0) & ~0xFFFFull) | (uint16_t)(int16_t)(int8_t)R(0);"),
            "CWDE" => self.emit("R(0) = (uint32_t)(int32_t)(int16_t)R(0);"),
            "CDQE" => self.emit("R(0) = (uint64_t)(int64_t)(int32_t)R(0);"),
            "CWD" => self.emit("R(2) = (R(2) & ~0xFFFFull) | (((int16_t)R(0) < 0) ? 0xFFFF : 0);"),
            "CDQ" => self.emit("R(2) = ((int32_t)R(0) < 0) ? 0xFFFFFFFFull : 0;"),
            "CQO" => self.emit("R(2) = ((int64_t)R(0) < 0) ? ~0ull : 0;"),
            "BSWAP" => { let s = self.size(0)?; let v = self.rd(0, None)?; self.wr(0, &format!("BSWAP{}({v})", s * 8))?; }
            "ADD" => self.alu("a_ + b_", "F_ADD", true)?,
            "SUB" => self.alu("a_ - b_", "F_SUB", true)?,
            "CMP" => self.alu("a_ - b_", "F_SUB", false)?,
            "AND" => self.alu("a_ & b_", "F_LOGIC", true)?,
            "TEST" => self.alu("a_ & b_", "F_LOGIC", false)?,
            "OR" => self.alu("a_ | b_", "F_LOGIC", true)?,
            "XOR" => {
                if ins.op_kind(0) == OpKind::Register && ins.op_kind(1) == OpKind::Register && ins.op_register(0) == ins.op_register(1) {
                    let s = self.size(0)?;
                    self.wr(0, "0")?;
                    self.flags("F_LOGIC", s, "0", "0", "0");
                } else {
                    self.alu("a_ ^ b_", "F_LOGIC", true)?;
                }
            }
            "ADC" | "SBB" => {
                let s = self.size(0)?;
                let a = self.rd(0, None)?;
                let b = self.rd(1, Some(s))?;
                let (op, fl) = if m == "ADC" { ("+", "F_ADC") } else { ("-", "F_SBB") };
                self.emit(format!("{{ uint64_t ci_ = CF(&F), a_ = {a}, b_ = {b}, r_ = (a_ {op} b_ {op} ci_) & {};", mask(s)));
                self.emit(format!("F.fcin = ci_; FL({fl}, {}, a_, b_, r_);", s * 8));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "INC" | "DEC" => {
                let s = self.size(0)?;
                let a = self.rd(0, None)?;
                let (op, fl) = if m == "INC" { ("+", "F_INC") } else { ("-", "F_DEC") };
                self.emit(format!("{{ uint64_t a_ = {a}, r_ = (a_ {op} 1) & {}; F.fcin = CF(&F); FL({fl}, {}, a_, 1, r_);", mask(s), s * 8));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "NEG" => {
                let s = self.size(0)?;
                let b = self.rd(0, None)?;
                self.emit(format!("{{ uint64_t b_ = {b}, r_ = (0 - b_) & {}; FL(F_SUB, {}, 0, b_, r_);", mask(s), s * 8));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "NOT" => { let s = self.size(0)?; let v = self.rd(0, None)?; self.wr(0, &format!("(~{v}) & {}", mask(s)))?; }
            "RCL" | "RCR" | "SHL" | "SHR" | "SAR" | "ROL" | "ROR" => self.shift(m)?,
            "SAL" => self.shift("SHL")?,
            "SHLD" => self.shxd(true)?,
            "SHRD" => self.shxd(false)?,
            "IMUL" => {
                if ins.op_count() == 1 {
                    let s = self.size(0)?;
                    let src = self.rd(0, None)?;
                    if s == 8 {
                        self.emit(format!("{{ int64_t hi_; uint64_t lo_ = MULS64((int64_t)R(0), (int64_t){src}, &hi_); R(0) = lo_; R(2) = (uint64_t)hi_;"));
                        self.emit("F.fcin = (hi_ != ((int64_t)lo_ >> 63)); FL(F_MUL, 64, 0, 0, lo_); }");
                    } else {
                        let w = s * 8;
                        self.emit(format!("{{ int64_t p_ = (int64_t)({})R(0) * (int64_t)({}){src};", st(s), st(s)));
                        if s == 1 {
                            self.emit("R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_;");
                        } else if s == 2 {
                            self.emit("R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_; R(2) = (R(2) & ~0xFFFFull) | (uint16_t)(p_ >> 16);");
                        } else {
                            self.emit("R(0) = (uint32_t)p_; R(2) = (uint32_t)(p_ >> 32);");
                        }
                        self.emit(format!("F.fcin = (p_ != ({})p_); FL(F_MUL, {w}, 0, 0, (uint64_t)p_ & {}); }}", st(s), mask(s)));
                    }
                    return Ok(false);
                }
                let s = self.size(0)?;
                let a = if ins.op_count() == 3 { self.rd(1, Some(s))? } else { self.rd(0, None)? };
                let b = if ins.op_count() == 3 { h(ins.immediate(2)) } else { self.rd(1, Some(s))? };
                let w = s * 8;
                if s == 8 {
                    self.emit(format!("{{ int64_t hi_; uint64_t r_ = MULS64((int64_t){a}, (int64_t){b}, &hi_); F.fcin = (hi_ != ((int64_t)r_ >> 63));"));
                } else {
                    self.emit(format!("{{ int64_t p_ = (int64_t)({}){a} * (int64_t)({}){b}; uint64_t r_ = (uint64_t)p_ & {}; F.fcin = (p_ != ({})p_);", st(s), st(s), mask(s), st(s)));
                }
                self.emit(format!("FL(F_MUL, {w}, 0, 0, r_);"));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "MUL" => {
                let s = self.size(0)?;
                let src = self.rd(0, None)?;
                match s {
                    8 => self.emit(format!("{{ uint64_t hi_, lo_ = MULU64(R(0), {src}, &hi_); R(0) = lo_; R(2) = hi_; F.fcin = hi_ != 0; FL(F_MUL, 64, 0, 0, lo_); }}")),
                    4 => self.emit(format!("{{ uint64_t p_ = (uint64_t)(uint32_t)R(0) * (uint32_t){src}; R(0) = (uint32_t)p_; R(2) = p_ >> 32; F.fcin = (p_ >> 32) != 0; FL(F_MUL, 32, 0, 0, (uint32_t)p_); }}")),
                    2 => self.emit(format!("{{ uint32_t p_ = (uint32_t)(uint16_t)R(0) * (uint16_t){src}; R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_; R(2) = (R(2) & ~0xFFFFull) | (p_ >> 16); F.fcin = (p_ >> 16) != 0; FL(F_MUL, 16, 0, 0, (uint16_t)p_); }}")),
                    _ => self.emit(format!("{{ uint32_t p_ = (uint32_t)(uint8_t)R(0) * (uint8_t){src}; R(0) = (R(0) & ~0xFFFFull) | (uint16_t)p_; F.fcin = (p_ >> 8) != 0; FL(F_MUL, 8, 0, 0, (uint8_t)p_); }}")),
                }
            }
            "DIV" => self.div(false)?,
            "IDIV" => self.div(true)?,
            "CMPXCHG" => {
                let s = self.size(0)?;
                let acc = format!("(R(0) & {})", mask(s));
                let d = self.rd(0, None)?;
                self.emit(format!("{{ uint64_t d_ = {d}, r_ = ({acc} - d_) & {}; FL(F_SUB, {}, {acc}, d_, r_);", mask(s), s * 8));
                self.emit(format!("if (d_ == {acc}) {{"));
                let v = self.rd(1, None)?;
                self.wr(0, &v)?;
                self.emit("} else {");
                if s == 4 {
                    self.emit("R(0) = (uint32_t)d_;");
                } else if s == 8 {
                    self.emit("R(0) = d_;");
                } else {
                    self.emit(format!("R(0) = (R(0) & ~{}) | d_;", mask(s)));
                }
                self.emit("} }");
            }
            "CMPXCHG16B" => { let a = self.addr()?; self.emit(format!("SPILL(); rt_cmpxchg16b(c, &F, {a}); FILL();")); }
            "XADD" => {
                let s = self.size(0)?;
                let a = self.rd(0, None)?;
                let b = self.rd(1, None)?;
                self.emit(format!("{{ uint64_t a_ = {a}, b_ = {b}, r_ = (a_ + b_) & {}; FL(F_ADD, {}, a_, b_, r_);", mask(s), s * 8));
                self.wr(1, "a_")?;
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "BT" => self.bt(None)?,
            "BTS" => self.bt(Some("V | B"))?,
            "BTR" => self.bt(Some("V & ~B"))?,
            "BTC" => self.bt(Some("V ^ B"))?,
            "BSF" | "BSR" => {
                if m == "BSF" { self.size(0)?; }
                let v = self.rd(1, None)?;
                self.emit(format!("{{ uint64_t v_ = {v}; rt_setzf(&F, v_ == 0); if (v_) {{"));
                self.wr(0, if m == "BSF" { "CTZ64(v_)" } else { "63 - CLZ64(v_)" })?;
                self.emit("} }");
            }
            "TZCNT" => {
                let s = self.size(0)?;
                let v = self.rd(1, None)?;
                self.emit(format!("{{ uint64_t v_ = {v}; uint64_t r_ = v_ ? CTZ64(v_) : {}; rt_setcfzf(&F, v_ == 0, r_ == 0);", s * 8));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "LZCNT" => {
                let s = self.size(0)?;
                let v = self.rd(1, None)?;
                self.emit(format!("{{ uint64_t v_ = {v}; uint64_t r_ = v_ ? CLZ64(v_) - {} : {}; rt_setcfzf(&F, v_ == 0, r_ == 0);", 64 - s * 8, s * 8));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "POPCNT" => {
                let v = self.rd(1, None)?;
                self.emit(format!("{{ uint64_t r_ = POPCNT64({v}); rt_setflags(&F, r_ ? 0 : 0x40);"));
                self.wr(0, "r_")?;
                self.emit("}");
            }
            "JMP" => {
                if ins.op_kind(0) == OpKind::NearBranch64 || ins.op_kind(0) == OpKind::NearBranch32 {
                    let t = self.target(ins.near_branch_target());
                    self.emit(t);
                    return Ok(true);
                }
                let v = self.rd(0, Some(self.ptr()))?;
                let targets: Vec<u64> = self.c.p.jt.get(&ins.ip()).map(|v| v.iter().copied()
                    .filter(|t| self.labels.contains(t) || self.c.roots.contains(t)).collect()).unwrap_or_default();
                if targets.is_empty() {
                    self.emit(format!("{{ static IC ic_; uint64_t t_ = {v}; SPILL(); GuestFn f_ = t_ == ic_.a ? ic_.f : rt_ic_fn(c, t_, &ic_); TAIL f_(c); return; }}"));
                    return Ok(true);
                }
                self.emit(format!("switch ({v}) {{"));
                for t in targets {
                    let tg = self.target(t);
                    self.emit(format!("case {}: {}", h(t), tg));
                }
                self.emit(format!("default: SPILL(); TAIL rt_jump_fn(c, {v})(c); return;"));
                self.emit("}");
                return Ok(true);
            }
            "CALL" => {
                let nxt = h(ins.next_ip());
                let near = ins.op_kind(0) == OpKind::NearBranch64 || ins.op_kind(0) == OpKind::NearBranch32;
                let sj = if near { ins.near_branch_target() } else if ins.is_ip_rel_memory_operand() { ins.ip_rel_memory_address() } else { 0 };
                if sj != 0 && self.c.p.is_setjmp(sj) {
                    self.emit(format!("{{ SPILL(); jmp_buf* sj_ = rt_sj_slot(c, R(1), R(2), {nxt}); if (!setjmp(*sj_)) R(0) = 0; else FILL(); }}"));
                    return Ok(false);
                }
                let p = self.ptr();
                let push = format!("R(4) -= {p}; ST{}(R(4), {nxt});", p * 8);
                if near && self.c.p.sj_funcs.contains(&ins.near_branch_target()) {
                    let t = ins.near_branch_target();
                    let call = if self.c.roots.contains(&t) { format!("F_{:x}(c);", t) } else { format!("rt_call(c, {});", h(t)) };
                    self.emit(format!("{push} SPILL(); {call} FILL(); {{ SPILL(); jmp_buf* sj_ = rt_sj_slot32(c, LD32(R(4)), {nxt}); if (setjmp(*sj_)) FILL(); }}"));
                    return Ok(false);
                }
                if near {
                    let t = ins.near_branch_target();
                    let call = if self.c.roots.contains(&t) { format!("F_{:x}(c);", t) } else { format!("rt_call(c, {});", h(t)) };
                    self.emit(format!("{push} SPILL(); {call} FILLV();"));
                } else {
                    let v = self.rd(0, Some(p))?;
                    self.emit(format!("{{ static IC ic_; uint64_t t_ = {v}; {push} SPILL(); if (t_ == ic_.a) ic_.f(c); else rt_call_ic(c, t_, &ic_); FILLV(); }}"));
                }
            }
            "RET" => {
                let extra = if ins.op_count() > 0 { ins.immediate(0) } else { 0 };
                self.emit(format!("R(4) += {}; SPILL(); return;", self.ptr() as u64 + extra));
                return Ok(true);
            }
            "INT3" => { self.emit(format!("SPILL(); rt_trap(c, {}, \"int3\"); return;", h(ins.ip()))); return Ok(true); }
            "UD2" => { self.emit(format!("SPILL(); rt_trap(c, {}, \"ud2\"); return;", h(ins.ip()))); return Ok(true); }
            "INT" | "HLT" => { self.emit(format!("SPILL(); rt_trap(c, {}, \"int\"); return;", h(ins.ip()))); return Ok(true); }
            "LEAVE" => {
                if self.bits() == 32 {
                    self.emit("R(4) = (uint32_t)R(5); R(5) = LD32(R(4)); R(4) += 4;");
                } else {
                    self.emit("R(4) = R(5); R(5) = LD64(R(4)); R(4) += 8;");
                }
            }
            "FXSAVE" | "FXSAVE64" => { let a = self.addr()?; self.emit(format!("rt_fxsave(c, {a});")); }
            "FXRSTOR" | "FXRSTOR64" => { let a = self.addr()?; self.emit(format!("rt_fxrstor(c, {a});")); }
            "CPUID" => self.emit("SPILL(); rt_cpuid(c); FILL();"),
            "XGETBV" => self.emit("SPILL(); rt_xgetbv(c); FILL();"),
            "RDTSC" => self.emit("SPILL(); rt_rdtsc(c); FILL();"),
            "RDTSCP" => self.emit("SPILL(); rt_rdtsc(c); FILL(); R(1) = 0;"),
            "MOVSB" | "MOVSW" | "MOVSQ" => return self.string("movs"),
            "STOSB" | "STOSW" | "STOSD" | "STOSQ" => return self.string("stos"),
            "LODSB" | "LODSW" | "LODSD" | "LODSQ" => return self.string("lods"),
            "SCASB" | "SCASW" | "SCASD" | "SCASQ" => return self.string("scas"),
            "CMPSB" | "CMPSW" | "CMPSQ" => return self.string("cmps"),
            "MOVSD" => {
                if ins.op_count() == 0 || matches!(ins.op_kind(0), OpKind::MemoryESRDI | OpKind::MemoryESEDI | OpKind::MemoryESDI) {
                    return self.string("movs");
                }
                crate::sse::movsd(self, m)?;
            }
            "CMPSD" => {
                if ins.op_count() == 0 || ins.op_kind(0) != OpKind::Register {
                    return self.string("cmps");
                }
                match crate::sse::lookup(self, "CMPSD") {
                    Some(f) => { f(self, "CMPSD")?; }
                    None => return un("'NoneType' object is not callable"),
                }
            }
            _ => return un("no handler"),
        }
        Ok(false)
    }
}

fn has_ldst(body: &str) -> bool {
    // \b(ST|LD)(8|16|32|64)\(
    let b = body.as_bytes();
    for i in 0..b.len() {
        if i > 0 && (b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_') {
            continue;
        }
        let r = &body[i..];
        for p in ["ST8(", "ST16(", "ST32(", "ST64(", "LD8(", "LD16(", "LD32(", "LD64("] {
            if r.starts_with(p) {
                return true;
            }
        }
    }
    false
}
