// x87 floating point: the Rust form of x87.py (the register stack lives in the CPU struct as doubles).
use iced_x86::{OpKind, Register};

use crate::func::{Func, Unsup, R};

fn un<T>(s: &str) -> R<T> {
    Err(Unsup(s.into()))
}

fn st(i: u32) -> String {
    format!("FST_({i})")
}

fn sti(f: &Func, k: u32) -> R<u32> {
    let r = f.ins.op_register(k) as u32;
    let s0 = Register::ST0 as u32;
    if r >= s0 && r <= Register::ST7 as u32 {
        return Ok(r - s0);
    }
    un("x87 operand")
}

fn mem_float(f: &Func) -> R<String> {
    let n = f.ins.memory_size().size();
    let a = f.addr()?;
    match n {
        4 => Ok(format!("F32B(LD32({a}))")),
        8 => Ok(format!("F64B(LD64({a}))")),
        10 => Ok(format!("F80B(LD64({a}), LD16({a} + 8))")),
        _ => un("x87 size"),
    }
}

fn mem_int(f: &Func) -> R<String> {
    let n = f.ins.memory_size().size();
    let a = f.addr()?;
    match n {
        2 => Ok(format!("(double)(int16_t)LD16({a})")),
        4 => Ok(format!("(double)(int32_t)LD32({a})")),
        8 => Ok(format!("(double)(int64_t)LD64({a})")),
        _ => un("KeyError"),
    }
}

fn store_float(f: &mut Func, v: &str) -> R<()> {
    let n = f.ins.memory_size().size();
    let a = f.addr()?;
    match n {
        4 => f.emit(format!("ST32({a}, B32F({v}));")),
        8 => f.emit(format!("ST64({a}, B64F({v}));")),
        10 => f.emit(format!("{{ uint64_t m_; uint16_t e_; F80S({v}, &m_, &e_); ST64({a}, m_); ST16({a} + 8, e_); }}")),
        _ => return un("x87 size"),
    }
    Ok(())
}

const ARITH: [&str; 6] = ["ADD", "SUB", "SUBR", "MUL", "DIV", "DIVR"];
const CONSTS: [(&str, &str); 7] = [("FLDZ", "0.0"), ("FLD1", "1.0"), ("FLDPI", "3.14159265358979323846"), ("FLDL2E", "1.44269504088896340736"),
    ("FLDL2T", "3.32192809488736234787"), ("FLDLG2", "0.30102999566398119521"), ("FLDLN2", "0.69314718055994530942")];
const UNARY: [(&str, &str); 7] = [("FCHS", "-{0}"), ("FABS", "fabs({0})"), ("FSQRT", "sqrt({0})"), ("FSIN", "sin({0})"), ("FCOS", "cos({0})"),
    ("FRNDINT", "frnd(c, {0})"), ("F2XM1", "(exp2({0}) - 1.0)")];
const FCMOV: [(&str, &str); 8] = [("FCMOVB", "CC_B"), ("FCMOVE", "CC_E"), ("FCMOVBE", "CC_BE"), ("FCMOVU", "CC_P"), ("FCMOVNB", "CC_AE"),
    ("FCMOVNE", "CC_NE"), ("FCMOVNBE", "CC_A"), ("FCMOVNU", "CC_NP")];
const PLAIN: [&str; 37] = ["FLD", "FILD", "FST", "FSTP", "FIST", "FISTP", "FISTTP", "FXCH", "FTST", "FXAM", "FPTAN", "FSINCOS", "FPATAN",
    "FYL2X", "FYL2XP1", "FSCALE", "FPREM", "FPREM1", "FXTRACT", "FINCSTP", "FDECSTP", "FFREE", "FLDCW", "FNSTCW", "FNSTSW", "FNINIT",
    "FNCLEX", "FNSTENV", "FLDENV", "FNSAVE", "FRSTOR", "FSTSW", "FSTCW", "FINIT", "FCLEX", "FSTENV", "FSAVE"];
const COMS: [&str; 12] = ["FCOM", "FCOMP", "FCOMPP", "FUCOM", "FUCOMP", "FUCOMPP", "FICOM", "FICOMP", "FCOMI", "FCOMIP", "FUCOMI", "FUCOMIP"];

fn arith_of(m: &str) -> Option<(&'static str, bool, bool)> {
    for op in ARITH {
        if m == format!("F{op}") { return Some((op, false, false)); }
        if m == format!("F{op}P") { return Some((op, true, false)); }
        if m == format!("FI{op}") { return Some((op, false, true)); }
    }
    None
}

pub fn has(m: &str) -> bool {
    PLAIN.contains(&m) || COMS.contains(&m) || arith_of(m).is_some() || CONSTS.iter().any(|c| c.0 == m)
        || UNARY.iter().any(|c| c.0 == m) || FCMOV.iter().any(|c| c.0 == m)
}

fn arith(f: &mut Func, op: &str, pop: bool, integer: bool) -> R<()> {
    let (d, s) = if f.ins.op_kind(0) == OpKind::Memory {
        (st(0), if integer { mem_int(f)? } else { mem_float(f)? })
    } else {
        (st(sti(f, 0)?), st(sti(f, 1)?))
    };
    let e = match op {
        "ADD" => "d_ + s_", "SUB" => "d_ - s_", "SUBR" => "s_ - d_", "MUL" => "d_ * s_", "DIV" => "d_ / s_", _ => "s_ / d_",
    };
    f.emit(format!("{{ double s_ = {s}, d_ = {d}; {d} = {e}; }}"));
    if pop {
        f.emit("fpop(c);");
    }
    Ok(())
}

fn cmp_src(f: &Func) -> R<String> {
    if f.ins.op_count() == 0 {
        return Ok(st(1));
    }
    let k = f.ins.op_count() - 1;
    if f.ins.op_kind(k) == OpKind::Memory {
        return mem_float(f);
    }
    Ok(st(sti(f, k)?))
}

pub fn op(f: &mut Func, m: &str) -> R<()> {
    let ins = f.ins;
    if let Some((op, pop, integer)) = arith_of(m) {
        return arith(f, op, pop, integer);
    }
    if let Some(c) = CONSTS.iter().find(|c| c.0 == m) {
        f.emit(format!("fpush(c, {});", c.1));
        return Ok(());
    }
    if let Some(u) = UNARY.iter().find(|c| c.0 == m) {
        let e = u.1.replace("{0}", &st(0));
        f.emit(format!("{} = {e}; c->fsw &= ~0x400u;", st(0)));
        return Ok(());
    }
    if let Some(c) = FCMOV.iter().find(|c| c.0 == m) {
        let s = sti(f, 1)?;
        f.emit(format!("if ({}(&F)) {} = {};", c.1, st(0), st(s)));
        return Ok(());
    }
    match m {
        "FCOM" | "FCOMP" | "FCOMPP" | "FUCOM" | "FUCOMP" | "FUCOMPP" => {
            let pops = match m { "FCOM" | "FUCOM" => 0, "FCOMP" | "FUCOMP" => 1, _ => 2 };
            let src = cmp_src(f)?;
            f.emit(format!("fcom(c, {}, {src});", st(0)));
            for _ in 0..pops { f.emit("fpop(c);"); }
        }
        "FICOM" | "FICOMP" => {
            let src = mem_int(f)?;
            f.emit(format!("fcom(c, {}, {src});", st(0)));
            if m == "FICOMP" { f.emit("fpop(c);"); }
        }
        "FCOMI" | "FCOMIP" | "FUCOMI" | "FUCOMIP" => {
            let s = sti(f, 1)?;
            f.emit(format!("rt_ucomi(&F, {}, {});", st(0), st(s)));
            if m.ends_with('P') { f.emit("fpop(c);"); }
        }
        "FLD" => {
            if ins.op_kind(0) == OpKind::Memory {
                let v = mem_float(f)?;
                f.emit(format!("fpush(c, {v});"));
            } else {
                let s = sti(f, 0)?;
                f.emit(format!("{{ double v_ = {}; fpush(c, v_); }}", st(s)));
            }
        }
        "FILD" => { let v = mem_int(f)?; f.emit(format!("fpush(c, {v});")); }
        "FST" | "FSTP" => {
            if ins.op_kind(0) == OpKind::Memory {
                store_float(f, &st(0))?;
            } else {
                let s = sti(f, 0)?;
                f.emit(format!("{} = {};", st(s), st(0)));
            }
            if m == "FSTP" { f.emit("fpop(c);"); }
        }
        "FIST" | "FISTP" | "FISTTP" => {
            let n = ins.memory_size().size();
            let v = format!("fist(c, {}, {}, {})", st(0), n * 8, if m == "FISTTP" { 1 } else { 0 });
            let a = f.addr()?;
            f.emit(format!("ST{}({a}, {v});", n * 8));
            if m != "FIST" { f.emit("fpop(c);"); }
        }
        "FXCH" => {
            let i = if ins.op_count() > 1 { sti(f, 1)? } else { 1 };
            f.emit(format!("{{ double t_ = {}; {} = {}; {} = t_; }}", st(0), st(0), st(i), st(i)));
        }
        "FTST" => f.emit(format!("fcom(c, {}, 0.0);", st(0))),
        "FXAM" => f.emit("fxam(c);"),
        "FPTAN" => f.emit(format!("{} = tan({}); fpush(c, 1.0); c->fsw &= ~0x400u;", st(0), st(0))),
        "FSINCOS" => f.emit(format!("{{ double v_ = {}; {} = sin(v_); fpush(c, cos(v_)); c->fsw &= ~0x400u; }}", st(0), st(0))),
        "FPATAN" => f.emit(format!("{} = atan2({}, {}); fpop(c);", st(1), st(1), st(0))),
        "FYL2X" => f.emit(format!("{} = {} * log2({}); fpop(c);", st(1), st(1), st(0))),
        "FYL2XP1" => f.emit(format!("{} = {} * log2({} + 1.0); fpop(c);", st(1), st(1), st(0))),
        "FSCALE" => f.emit(format!("{} = ldexp({}, (int)trunc({}));", st(0), st(0), st(1))),
        "FPREM" => f.emit(format!("{} = fmod({}, {}); c->fsw &= ~0x400u;", st(0), st(0), st(1))),
        "FPREM1" => f.emit(format!("{} = remainder({}, {}); c->fsw &= ~0x400u;", st(0), st(0), st(1))),
        "FXTRACT" => f.emit(format!("{{ int e_; double m_ = frexp({}, &e_); {} = (double)(e_ - 1); fpush(c, m_ * 2.0); }}", st(0), st(0))),
        "FINCSTP" => f.emit("fpop(c);"),
        "FDECSTP" => f.emit("c->ftop = (c->ftop - 1) & 7;"),
        "FFREE" => {}
        "FLDCW" => { let a = f.addr()?; f.emit(format!("c->fcw = (uint32_t)LD16({a});")); }
        "FNSTCW" | "FSTCW" => f.wr(0, "c->fcw")?,
        "FNSTSW" | "FSTSW" => f.wr(0, "fstsw(c)")?,
        "FNINIT" | "FINIT" => f.emit("c->ftop = 0; c->fsw = 0; c->fcw = 0x27F;"),
        "FNCLEX" | "FCLEX" => f.emit("c->fsw &= 0x7F00u;"),
        "FNSTENV" | "FSTENV" => { let a = f.addr()?; f.emit(format!("rt_fnstenv(c, {a});")); }
        "FLDENV" => { let a = f.addr()?; f.emit(format!("rt_fldenv(c, {a});")); }
        "FNSAVE" | "FSAVE" => { let a = f.addr()?; f.emit(format!("rt_fnsave(c, {a});")); }
        "FRSTOR" => { let a = f.addr()?; f.emit(format!("rt_frstor(c, {a});")); }
        _ => return un("no handler"),
    }
    Ok(())
}
