// SSE instructions: the Rust form of sse.py (explicit forms, then sse.h's XFN helpers for the rest).
use iced_x86::OpKind;

use crate::func::{h, is_imm, mask, Func, Unsup, R};

pub type Handler = fn(&mut Func, &str) -> R<()>;

fn un<T>(s: &str) -> R<T> {
    Err(Unsup(s.into()))
}

fn is_x(f: &Func, i: u32) -> bool {
    f.ins.op_kind(i) == OpKind::Register && f.reg(f.ins.op_register(i)).map_or(false, |r| r.0 == 'x')
}

fn store_x(f: &mut Func, val: &str, n: u32) -> R<()> {
    let a = f.addr()?;
    f.emit(format!("XST({a}, {val}, {n});"));
    Ok(())
}

const SHIFTS: [&str; 8] = ["PSLLW", "PSLLD", "PSLLQ", "PSRLW", "PSRLD", "PSRLQ", "PSRAW", "PSRAD"];

pub fn lookup(f: &Func, m: &str) -> Option<Handler> {
    let e: Option<Handler> = match m {
        "MOVAPS" | "MOVUPS" | "MOVAPD" | "MOVUPD" | "MOVDQA" | "MOVDQU" | "LDDQU" | "MOVNTPS" | "MOVNTPD" | "MOVNTDQ" | "MOVNTDQA"
        | "VMOVAPS" | "VMOVUPS" | "VMOVAPD" | "VMOVUPD" | "VMOVDQA" | "VMOVDQU" | "VLDDQU" | "VMOVNTPS" | "VMOVNTDQ" => Some(mov128),
        "MOVSS" | "VMOVSS" => Some(movss),
        "MOVSD" | "VMOVSD" => Some(movsd),
        "MOVD" | "MOVQ" | "VMOVD" | "VMOVQ" => Some(movdq),
        "MOVLPS" | "MOVLPD" | "MOVHPS" | "MOVHPD" => Some(movlh),
        "MOVHLPS" => Some(movhlps),
        "MOVLHPS" => Some(movlhps),
        "MOVMSKPS" | "MOVMSKPD" | "PMOVMSKB" | "VMOVMSKPS" | "VMOVMSKPD" | "VPMOVMSKB" => Some(movmsk),
        "CVTSI2SS" | "CVTSI2SD" | "VCVTSI2SS" | "VCVTSI2SD" => Some(cvtsi2),
        "CVTTSS2SI" | "CVTTSD2SI" | "CVTSS2SI" | "CVTSD2SI" | "VCVTTSS2SI" | "VCVTTSD2SI" | "VCVTSS2SI" | "VCVTSD2SI" => Some(cvt2si),
        "UCOMISS" | "COMISS" | "VUCOMISS" | "VCOMISS" => Some(ucomiss),
        "UCOMISD" | "COMISD" | "VUCOMISD" | "VCOMISD" => Some(ucomisd),
        "PEXTRB" | "PEXTRW" | "PEXTRD" | "PEXTRQ" | "EXTRACTPS" | "VPEXTRB" | "VPEXTRW" | "VPEXTRD" | "VPEXTRQ" | "VEXTRACTPS" => Some(pextr),
        "PINSRB" | "PINSRW" | "PINSRD" | "PINSRQ" => Some(pinsr),
        "PTEST" | "VPTEST" => Some(ptest),
        "PCMPESTRI" | "PCMPISTRI" | "PCMPESTRM" | "PCMPISTRM" | "PCMPESTRI64" | "PCMPISTRI64" | "PCMPESTRM64" | "PCMPISTRM64"
        | "VPCMPESTRI64" | "VPCMPESTRM64" | "VPCMPESTRI" | "VPCMPISTRI" | "VPCMPESTRM" | "VPCMPISTRM" => Some(pcmpstr),
        "CRC32" => Some(crc32),
        "LDMXCSR" | "VLDMXCSR" => Some(ldmxcsr),
        "STMXCSR" | "VSTMXCSR" => Some(stmxcsr),
        "MOVNTI" => Some(movnti),
        "BLENDVPS" | "BLENDVPD" | "PBLENDVB" => Some(blendv),
        "VZEROUPPER" | "VZEROALL" | "EMMS" => Some(vzu),
        _ => None,
    };
    if e.is_some() {
        return e;
    }
    let base = m.strip_prefix('V').unwrap_or(m);
    if f.c.generic.contains(base) || f.c.generic.contains(m) {
        return Some(generic);
    }
    None
}

fn mov128(f: &mut Func, _m: &str) -> R<()> {
    if is_x(f, 0) {
        let d = f.xmm(0)?;
        let s = f.xsrc(1, Some(16))?;
        f.emit(format!("c->x[{d}] = {s};"));
    } else {
        let s = format!("c->x[{}]", f.xmm(1)?);
        store_x(f, &s, 16)?;
    }
    Ok(())
}

fn movs(f: &mut Func, n: u32) -> R<()> {
    if f.ins.op_count() == 3 {
        let (d, a, b) = (f.xmm(0)?, f.xmm(1)?, f.xmm(2)?);
        f.emit(format!("{{ XMM t_ = c->x[{a}]; memcpy(&t_, &c->x[{b}], {n}); c->x[{d}] = t_; }}"));
    } else if is_x(f, 0) && is_x(f, 1) {
        let (d, s) = (f.xmm(0)?, f.xmm(1)?);
        f.emit(format!("memcpy(&c->x[{d}], &c->x[{s}], {n});"));
    } else if is_x(f, 0) {
        let d = f.xmm(0)?;
        let a = f.addr()?;
        f.emit(format!("c->x[{d}] = XLD({a}, {n});"));
    } else {
        let s = format!("c->x[{}]", f.xmm(1)?);
        store_x(f, &s, n)?;
    }
    Ok(())
}

fn movss(f: &mut Func, _m: &str) -> R<()> { movs(f, 4) }
pub fn movsd(f: &mut Func, _m: &str) -> R<()> { movs(f, 8) }

fn movdq(f: &mut Func, m: &str) -> R<()> {
    let n = if m.ends_with("MOVD") { 4 } else { 8 };
    if is_x(f, 0) {
        if is_x(f, 1) {
            let (s, d) = (f.xmm(1)?, f.xmm(0)?);
            f.emit(format!("{{ XMM t_ = {{0}}; t_.q[0] = c->x[{s}].q[0]; c->x[{d}] = t_; }}"));
        } else if f.ins.op_kind(1) == OpKind::Memory {
            let d = f.xmm(0)?;
            let a = f.addr()?;
            f.emit(format!("c->x[{d}] = XLD({a}, {n});"));
        } else {
            let v = f.rd(1, None)?;
            let d = f.xmm(0)?;
            f.emit(format!("{{ XMM t_ = {{0}}; t_.q[0] = {v} & {}; c->x[{d}] = t_; }}", mask(n)));
        }
    } else {
        let v = format!("c->x[{}].q[0]", f.xmm(1)?);
        if f.ins.op_kind(0) == OpKind::Memory {
            let a = f.addr()?;
            f.emit(format!("ST{}({a}, {v});", n * 8));
        } else {
            let w = if n == 8 { v } else { format!("(uint32_t){v}") };
            f.wr(0, &w)?;
        }
    }
    Ok(())
}

fn movlh(f: &mut Func, m: &str) -> R<()> {
    let lane = if m.as_bytes()[3] == b'H' { 1 } else { 0 };
    if is_x(f, 0) {
        let d = f.xmm(0)?;
        let a = f.addr()?;
        f.emit(format!("c->x[{d}].q[{lane}] = LD64({a});"));
    } else {
        let a = f.addr()?;
        let s = f.xmm(1)?;
        f.emit(format!("ST64({a}, c->x[{s}].q[{lane}]);"));
    }
    Ok(())
}

fn movhlps(f: &mut Func, _m: &str) -> R<()> {
    let (d, s) = (f.xmm(0)?, f.xmm(1)?);
    f.emit(format!("c->x[{d}].q[0] = c->x[{s}].q[1];"));
    Ok(())
}

fn movlhps(f: &mut Func, _m: &str) -> R<()> {
    let (d, s) = (f.xmm(0)?, f.xmm(1)?);
    f.emit(format!("c->x[{d}].q[1] = c->x[{s}].q[0];"));
    Ok(())
}

fn movmsk(f: &mut Func, m: &str) -> R<()> {
    let name = m.trim_start_matches('V');
    let v = format!("x_{name}(c->x[{}])", f.xmm(1)?);
    f.wr(0, &v)
}

fn cvtsi2(f: &mut Func, m: &str) -> R<()> {
    let src = if f.ins.op_count() == 2 { 1 } else { 2 };
    let n = f.size(src)?;
    let v = format!("(int64_t)(int{}_t){}", n * 8, f.rd(src, None)?);
    let d = f.xmm(0)?;
    if f.ins.op_count() == 3 {
        let s = f.xmm(1)?;
        f.emit(format!("c->x[{d}] = c->x[{s}];"));
    }
    if m.ends_with("SS") {
        f.emit(format!("c->x[{d}].f[0] = (float)({v});"));
    } else {
        f.emit(format!("c->x[{d}].g[0] = (double)({v});"));
    }
    Ok(())
}

fn cvt2si(f: &mut Func, m: &str) -> R<()> {
    let n = f.size(0)?;
    let sd = m.contains("SD2");
    let src = f.xsrc(1, Some(if sd { 8 } else { 4 }))?;
    let lane = if sd { format!("({src}).g[0]") } else { format!("({src}).f[0]") };
    let fname = if m.contains("CVTT") { "CVTT" } else { "CVTR" };
    f.wr(0, &format!("{fname}{}((double){lane})", n * 8))
}

fn ucomiss(f: &mut Func, _m: &str) -> R<()> {
    let d = f.xmm(0)?;
    let s = f.xsrc(1, Some(4))?;
    f.emit(format!("rt_ucomi(&F, c->x[{d}].f[0], ({s}).f[0]);"));
    Ok(())
}

fn ucomisd(f: &mut Func, _m: &str) -> R<()> {
    let d = f.xmm(0)?;
    let s = f.xsrc(1, Some(8))?;
    f.emit(format!("rt_ucomi(&F, c->x[{d}].g[0], ({s}).g[0]);"));
    Ok(())
}

fn pextr(f: &mut Func, m: &str) -> R<()> {
    let n: u64 = match m.as_bytes()[m.len() - 1] { b'B' => 1, b'W' => 2, b'D' => 4, b'Q' => 8, _ => 4 };
    let imm = f.ins.immediate(2);
    let lanes = 16 / n;
    let v = format!("XLANE{}(c->x[{}], {})", n * 8, f.xmm(1)?, imm % lanes);
    if f.ins.op_kind(0) == OpKind::Memory {
        let a = f.addr()?;
        f.emit(format!("ST{}({a}, {v});", n * 8));
        Ok(())
    } else {
        f.wr(0, &v)
    }
}

fn pinsr(f: &mut Func, m: &str) -> R<()> {
    let n: u64 = match m.as_bytes()[m.len() - 1] { b'B' => 1, b'W' => 2, b'D' => 4, _ => 8 };
    let imm = f.ins.immediate(2) % (16 / n);
    let v = if f.ins.op_kind(1) == OpKind::Memory { format!("LD{}({})", n * 8, f.addr()?) } else { f.rd(1, None)? };
    let d = f.xmm(0)?;
    f.emit(format!("XSETLANE{}(c->x[{d}], {imm}, {v});", n * 8));
    Ok(())
}

fn ptest(f: &mut Func, _m: &str) -> R<()> {
    let d = f.xmm(0)?;
    let s = f.xsrc(1, Some(16))?;
    f.emit(format!("rt_ptest(&F, c->x[{d}], {s});"));
    Ok(())
}

fn pcmpstr(f: &mut Func, m: &str) -> R<()> {
    let expl = if m.contains("ESTR") { 1 } else { 0 };
    let maskm = if m.ends_with('M') { 1 } else { 0 };
    let w = f.ins.op_code().operand_size() == 64;
    let (la, lb) = if w { ("R(0)", "R(2)") } else { ("(int64_t)(int32_t)R(0)", "(int64_t)(int32_t)R(2)") };
    let d = f.xmm(0)?;
    let s = f.xsrc(1, Some(16))?;
    let call = format!("rt_pcmpstr(&F, &c->x[0], c->x[{d}], {s}, {}, {la}, {lb}, {expl}, {maskm})", f.ins.immediate(2) & 0xFF);
    if maskm == 1 {
        f.emit(format!("{call};"));
    } else {
        f.emit(format!("R(1) = {call};"));
    }
    Ok(())
}

fn crc32(f: &mut Func, _m: &str) -> R<()> {
    let n = f.size(1)?;
    let v = format!("rt_crc32({}, {}, {n})", f.rd(0, None)?, f.rd(1, None)?);
    f.wr(0, &v)
}

fn ldmxcsr(f: &mut Func, _m: &str) -> R<()> {
    let a = f.addr()?;
    f.emit(format!("c->mxcsr = (uint32_t)LD32({a});"));
    Ok(())
}

fn stmxcsr(f: &mut Func, _m: &str) -> R<()> {
    let a = f.addr()?;
    f.emit(format!("ST32({a}, c->mxcsr);"));
    Ok(())
}

fn movnti(f: &mut Func, _m: &str) -> R<()> {
    let v = f.rd(1, None)?;
    f.wr(0, &v)
}

fn blendv(f: &mut Func, m: &str) -> R<()> {
    let src = if f.ins.op_kind(1) == OpKind::Memory { format!("XLD({}, 16)", f.addr()?) } else { format!("c->x[{}]", f.xmm(1)?) };
    let d = f.xmm(0)?;
    f.emit(format!("x_{m}(&c->x[{d}], {src}, c->x[0]);"));
    Ok(())
}

fn vzu(_f: &mut Func, _m: &str) -> R<()> {
    Ok(())
}

fn generic(f: &mut Func, m: &str) -> R<()> {
    let ins = f.ins;
    let name: &str = if m.starts_with('V') && f.c.generic.contains(&m[1..]) { &m[1..] } else { m };
    if !f.c.generic.contains(name) {
        return un("no helper");
    }
    if !is_x(f, 0) {
        return un("non-xmm dst");
    }
    let last = ins.op_count() - 1;
    let imm = is_imm(ins.op_kind(last));
    let nops = ins.op_count() - if imm { 1 } else { 0 };
    let immarg = if imm { format!(", {}", ins.immediate(last) & 0xFF) } else { String::new() };
    let src_i = nops.wrapping_sub(1);
    let sk = if nops >= 1 { ins.op_kind(src_i) } else { OpKind::Register };
    let src = if nops >= 1 && sk == OpKind::Memory {
        format!("XLD({}, {})", f.addr()?, ins.memory_size().size())
    } else if nops >= 1 && sk == OpKind::Register && is_x(f, src_i) {
        format!("c->x[{}]", f.xmm(src_i)?)
    } else if (nops >= 1 && is_imm(sk)) || nops == 1 {
        "c->x[0]".to_string()
    } else {
        return un("src");
    };
    let d = f.xmm(0)?;
    if imm && SHIFTS.contains(&name) {
        let srcx = if nops == 2 { format!("c->x[{}]", f.xmm(1)?) } else { format!("c->x[{d}]") };
        f.emit(format!("{{ XMM t_ = {srcx}; x_{name}(&t_, immx({})); c->x[{d}] = t_; }}", ins.immediate(last) & 0xFF));
        return Ok(());
    }
    if name == "PSLLDQ" || name == "PSRLDQ" {
        let srcx = if nops == 2 { format!("c->x[{}]", f.xmm(1)?) } else { format!("c->x[{d}]") };
        f.emit(format!("x_{name}(&c->x[{d}], {srcx}{immarg});"));
        return Ok(());
    }
    if m.starts_with('V') && nops == 3 {
        let a = f.xmm(1)?;
        f.emit(format!("{{ XMM t_ = c->x[{a}]; x_{name}(&t_, {src}{immarg}); c->x[{d}] = t_; }}"));
    } else if m.starts_with('V') && nops == 4 {
        let a = f.xmm(1)?;
        let b = f.xmm(3)?;
        f.emit(format!("{{ XMM t_ = c->x[{a}]; XMM s_ = {src}; x_{name}(&t_, s_, c->x[{b}]); c->x[{d}] = t_; }}"));
    } else if nops == 1 {
        f.emit(format!("x_{name}(&c->x[{d}], c->x[{d}]{immarg});"));
    } else {
        f.emit(format!("x_{name}(&c->x[{d}], {src}{immarg});"));
    }
    let _ = h(0);
    Ok(())
}
