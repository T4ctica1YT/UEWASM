// Function discovery for one module: the Rust form of lift.py's Program (roots, regions, jump tables).
use iced_x86::{Code, Decoder, DecoderOptions, FlowControl, Instruction, Mnemonic, OpKind, Register};
use std::collections::{BTreeSet, HashMap, HashSet};

use crate::func::{mn, reg_info};

pub struct Module {
    pub base: u64,
    pub bits: u32,
    pub img: &'static [u8],
    pub image_size: u64,
    pub entry_rva: u64,
    pub sections: Vec<(u64, u64, bool)>,
    pub functions: Vec<(u64, u64)>,
    pub unwind: HashSet<u64>,
    pub family: HashMap<u64, u64>,
    pub exports: Vec<u64>,
    pub tls_callbacks: Vec<u64>,
    pub relocs: Vec<u64>,
    pub sj_slots: HashSet<u64>,
}

pub struct Program {
    pub m: Module,
    pub text: Vec<(u64, u64)>,
    pub roots: HashSet<u64>,
    pub starts: Vec<u64>,
    pub ends: HashMap<u64, u64>,
    pub jt: HashMap<u64, Vec<u64>>,
    pub order: Vec<u64>,
    pub blocks: HashMap<u64, BTreeSet<u64>>,
    pub sj_funcs: HashSet<u64>,
    pub ptr: u64,
    funcs_sorted: Vec<(u64, u64)>,
}

pub fn decode_at(bits: u32, base: u64, img: &[u8], a: u64) -> Instruction {
    let rva = a.wrapping_sub(base);
    if rva >= img.len() as u64 {
        return Instruction::default();
    }
    let r = rva as usize;
    let end = (r + 16).min(img.len());
    let mut d = Decoder::with_ip(bits, &img[r..end], a, DecoderOptions::NONE);
    d.decode()
}

impl Program {
    pub fn new(m: Module) -> Program {
        let text: Vec<(u64, u64)> = m.sections.iter().filter(|s| s.2).map(|s| (s.0, s.1)).collect();
        let mut funcs_sorted = m.functions.clone();
        funcs_sorted.sort();
        let ptr = (m.bits / 8) as u64;
        let mut p = Program {
            m, text, roots: HashSet::new(), starts: vec![], ends: HashMap::new(), jt: HashMap::new(), order: vec![],
            blocks: HashMap::new(), sj_funcs: HashSet::new(), ptr, funcs_sorted,
        };
        if p.m.bits == 32 {
            p.sj_funcs = p.find_setjmp32();
        }
        p
    }

    fn find_setjmp32(&self) -> HashSet<u64> {
        let sig: [u8; 19] = [0x8B, 0x54, 0x24, 0x04, 0x89, 0x2A, 0x89, 0x5A, 0x04, 0x89, 0x7A, 0x08, 0x89, 0x72, 0x0C, 0x89, 0x62, 0x10, 0];
        let sig = &sig[..18];
        let mut found = HashSet::new();
        for &(rva, vsize) in &self.text {
            let s = rva as usize;
            let e = ((rva + vsize) as usize).min(self.m.img.len());
            if s >= e {
                continue;
            }
            let blob = &self.m.img[s..e];
            if blob.len() < sig.len() {
                continue;
            }
            for i in 0..=blob.len() - sig.len() {
                if &blob[i..i + sig.len()] == sig {
                    found.insert(self.m.base + rva + i as u64);
                }
            }
        }
        found
    }

    pub fn is_setjmp(&self, t: u64) -> bool {
        if self.m.sj_slots.contains(&t) {
            return true;
        }
        let o = t.wrapping_sub(self.m.base);
        let img = self.m.img;
        if (o as i64) >= 0 && (o as usize) + 6 < img.len() && img[o as usize] == 0xFF && img[o as usize + 1] == 0x25 {
            let o = o as usize;
            let d = i32::from_le_bytes([img[o + 2], img[o + 3], img[o + 4], img[o + 5]]) as i64;
            return self.m.sj_slots.contains(&((t as i64 + 6 + d) as u64));
        }
        false
    }

    pub fn fam(&self, a: u64) -> Option<u64> {
        let rva = a.wrapping_sub(self.m.base) as i64;
        let i = self.funcs_sorted.partition_point(|f| (f.0 as i64) <= rva);
        if i == 0 {
            return None;
        }
        let (b, e) = self.funcs_sorted[i - 1];
        if !((b as i64) <= rva && rva < e as i64) {
            return None;
        }
        Some(*self.m.family.get(&b).unwrap_or(&b))
    }

    pub fn func_start(&self, a: u64) -> bool {
        let rva = a.wrapping_sub(self.m.base);
        self.m.unwind.contains(&rva) && *self.m.family.get(&rva).unwrap_or(&rva) == rva
    }

    pub fn in_code(&self, a: u64) -> bool {
        let rva = a.wrapping_sub(self.m.base) as i64;
        self.text.iter().any(|&(s, v)| (s as i64) <= rva && rva < (s + v) as i64)
    }

    pub fn decode(&self, cache: &mut HashMap<u64, Instruction>, a: u64) -> Instruction {
        if let Some(i) = cache.get(&a) {
            return *i;
        }
        let i = decode_at(self.m.bits, self.m.base, self.m.img, a);
        cache.insert(a, i);
        i
    }

    pub fn flags_live(&self, cache: &mut HashMap<u64, Instruction>, a: u64) -> bool {
        let mut live = true;
        let mut written: u32 = 0;
        let mut p = a;
        for _ in 0..64 {
            let ins = self.decode(cache, p);
            if ins.code() == Code::INVALID {
                break;
            }
            if ins.rflags_read() & 0x3F & !written != 0 {
                break;
            }
            written |= ins.rflags_modified();
            let fc = ins.flow_control();
            if (written & 0x3F) == 0x3F || fc == FlowControl::Call || fc == FlowControl::Return {
                live = false;
                break;
            }
            if fc != FlowControl::Next {
                break;
            }
            p = ins.next_ip();
        }
        live
    }

    fn rd_ptr(&self, rva: u64) -> u64 {
        let r = rva as usize;
        let img = self.m.img;
        if self.ptr == 8 {
            if r + 8 > img.len() { return 0; }
            u64::from_le_bytes(img[r..r + 8].try_into().unwrap())
        } else {
            if r + 4 > img.len() { return 0; }
            u32::from_le_bytes(img[r..r + 4].try_into().unwrap()) as u64
        }
    }

    fn u32_at(&self, p: usize) -> u32 {
        u32::from_le_bytes(self.m.img[p..p + 4].try_into().unwrap())
    }

    fn insort(&mut self, t: u64) {
        let i = self.starts.partition_point(|&x| x <= t);
        self.starts.insert(i, t);
    }

    pub fn discover(&mut self) {
        let base = self.m.base;
        for &(b, e) in &self.m.functions.clone() {
            self.roots.insert(base + b);
            self.ends.insert(base + b, base + e);
        }
        self.roots.insert(base + self.m.entry_rva);
        for cb in self.m.tls_callbacks.clone() {
            self.roots.insert(base + cb);
        }
        for rva in self.m.exports.clone() {
            if self.in_code(base + rva) {
                self.roots.insert(base + rva);
            }
        }
        for rva in self.m.relocs.clone() {
            let v = self.rd_ptr(rva);
            if self.in_code(v) && !self.in_code(base + rva) {
                self.roots.insert(v);
            }
        }
        if self.m.bits == 32 {
            self.data_pointers();
        }
        let mut s: Vec<u64> = self.roots.iter().copied().collect();
        s.sort();
        self.starts = s.clone();
        let mut work = s;
        let mut seen = HashSet::new();
        let mut cache = HashMap::new();
        while let Some(r) = work.pop() {
            if !seen.insert(r) {
                continue;
            }
            let (labels, extra) = self.scan(&mut cache, r);
            self.set_block(r, labels);
            for t in extra {
                if !self.roots.contains(&t) {
                    self.roots.insert(t);
                    self.insort(t);
                    work.push(t);
                }
            }
        }
        for _ in 0..100 {
            let stale: Vec<u64> = self.order.iter().copied()
                .filter(|r| self.blocks[r].iter().any(|l| l != r && self.roots.contains(l))).collect();
            if stale.is_empty() {
                break;
            }
            for r in stale {
                let (labels, extra) = self.scan(&mut cache, r);
                self.set_block(r, labels);
                for t in extra {
                    if !self.roots.contains(&t) {
                        work.push(t);
                    }
                }
            }
            while let Some(r) = work.pop() {
                if self.roots.contains(&r) && self.blocks.contains_key(&r) {
                    continue;
                }
                if self.roots.insert(r) {
                    self.insort(r);
                } else if !self.starts.binary_search(&r).is_ok() {
                    self.insort(r);
                }
                let (labels, extra) = self.scan(&mut cache, r);
                self.set_block(r, labels);
                for t in extra {
                    if !self.roots.contains(&t) {
                        work.push(t);
                    }
                }
            }
        }
    }

    fn set_block(&mut self, r: u64, labels: BTreeSet<u64>) {
        if self.blocks.insert(r, labels).is_none() {
            self.order.push(r);
        }
    }

    fn data_pointers(&mut self) {
        let base = self.m.base;
        let lo = self.text.iter().map(|t| t.0).min().unwrap_or(0) + base;
        let hi = self.text.iter().map(|t| t.0 + t.1).max().unwrap_or(0) + base;
        let secs = self.m.sections.clone();
        for (rva, vsize, exec) in secs {
            if exec {
                continue;
            }
            let s = rva as usize;
            let e = ((rva + vsize) as usize).min(self.m.img.len());
            if e < s + 4 {
                continue;
            }
            let mut o = s;
            while o + 4 <= e {
                let v = self.u32_at(o) as u64;
                if lo <= v && v < hi && self.in_code(v) {
                    self.roots.insert(v);
                }
                o += 4;
            }
        }
    }

    fn region_end(&self, r: u64) -> u64 {
        if let Some(e) = self.ends.get(&r) {
            return *e;
        }
        let i = self.starts.partition_point(|&x| x <= r);
        let mut nxt = if i < self.starts.len() { self.starts[i] } else { r + 0x100000 };
        let rva = r.wrapping_sub(self.m.base);
        for &(s, v) in &self.text {
            if s <= rva && rva < s + v {
                nxt = nxt.min(self.m.base + s + v);
            }
        }
        nxt
    }

    fn window(&self, ins: &Instruction) -> Vec<Instruction> {
        let mut best: Vec<Instruction> = vec![];
        for back in (1..=96u64).rev() {
            let a = ins.ip().wrapping_sub(back);
            let rva = a.wrapping_sub(self.m.base) as i64;
            if rva < 0 {
                continue;
            }
            let r = rva as usize;
            if r >= self.m.img.len() {
                continue;
            }
            let end = (r + back as usize + 16).min(self.m.img.len());
            let mut d = Decoder::with_ip(self.m.bits, &self.m.img[r..end], a, DecoderOptions::NONE);
            let mut got = vec![];
            while d.can_decode() {
                let x = d.decode();
                if x.ip() >= ins.ip() || x.code() == Code::INVALID {
                    break;
                }
                got.push(x);
            }
            if !got.is_empty() && got.last().unwrap().next_ip() == ins.ip() && got.len() > best.len() {
                best = got;
                if best.len() >= 12 {
                    break;
                }
            }
        }
        let k = best.len().saturating_sub(11);
        let mut out: Vec<Instruction> = best[k..].to_vec();
        out.push(*ins);
        out
    }

    fn jump_table(&self, r: u64, end: u64, insns: &[Instruction]) -> BTreeSet<u64> {
        let mut targets = BTreeSet::new();
        let mut bound: Option<u64> = None;
        let mut load: Option<Instruction> = None;
        for ins in insns {
            let m = mn(ins);
            let k = if m == "JMP" { 0 } else { 1 };
            if (m == "MOV" || m == "MOVSXD" || m == "JMP") && ins.op_count() > k && ins.op_kind(k) == OpKind::Memory
                && ins.memory_index() != Register::None && ins.memory_index_scale() == 4 {
                load = Some(*ins);
            }
        }
        let mut alias = HashSet::new();
        if let Some(l) = load {
            if let Some(ri) = reg_info(l.memory_index()) {
                alias.insert(ri.1);
            }
        }
        for ins in insns.iter().rev() {
            if let Some(l) = load {
                if ins.ip() >= l.ip() {
                    continue;
                }
            }
            let m = mn(ins);
            if (m == "MOV" || m == "MOVZX" || m == "MOVSXD") && ins.op_count() == 2 && ins.op_kind(0) == OpKind::Register
                && reg_info(ins.op_register(0)).map_or(false, |x| alias.contains(&x.1)) {
                if ins.op_kind(1) == OpKind::Register && reg_info(ins.op_register(1)).is_some() {
                    alias.insert(reg_info(ins.op_register(1)).unwrap().1);
                } else if ins.op_kind(1) == OpKind::Memory && reg_info(ins.memory_index()).is_some() {
                    alias.insert(reg_info(ins.memory_index()).unwrap().1);
                }
            } else if m == "CMP" && ins.op_count() == 2 && ins.op_kind(0) == OpKind::Register && crate::func::is_imm(ins.op_kind(1))
                && reg_info(ins.op_register(0)).map_or(false, |x| alias.contains(&x.1)) {
                let v = ins.immediate(1);
                if v < 4096 {
                    bound = Some(v + 1);
                }
                break;
            }
        }
        let img = self.m.img;
        if let Some(b) = bound {
            for ins in insns.iter().rev() {
                if mn(ins) == "MOVZX" && ins.op_kind(1) == OpKind::Memory && ins.memory_size().size() == 1
                    && ins.memory_index() != Register::None && ins.memory_index_scale() == 1 {
                    let bt = ((ins.memory_displacement64() & 0xFFFFFFFF) as i64) - if self.m.bits == 32 { self.m.base as i64 } else { 0 };
                    if 0 < bt && (bt as u64 + b) as usize <= img.len() {
                        let s = bt as usize;
                        bound = Some(*img[s..s + b as usize].iter().max().unwrap() as u64 + 1);
                    }
                    break;
                }
            }
        }
        if self.m.bits == 32 {
            for ins in insns {
                let m = mn(ins);
                let k = if m == "JMP" { 0 } else { 1 };
                if !(m == "JMP" || m == "MOV") || ins.op_count() <= k || ins.op_kind(k) != OpKind::Memory
                    || ins.memory_index() == Register::None || ins.memory_index_scale() != 4 || ins.memory_base() != Register::None {
                    continue;
                }
                let tbl = (ins.memory_displacement64() & 0xFFFFFFFF) as i64 - self.m.base as i64;
                if !(0 < tbl && (tbl as u64) < self.m.image_size) {
                    continue;
                }
                for n in 0..bound.unwrap_or(1024) {
                    let p = tbl as usize + 4 * n as usize;
                    if p + 4 > img.len() {
                        break;
                    }
                    let t = self.u32_at(p) as u64;
                    if !self.in_code(t) {
                        break;
                    }
                    targets.insert(t);
                }
            }
            return targets;
        }
        for ins in insns {
            let m = mn(ins);
            if (m == "MOV" || m == "MOVSXD") && ins.op_count() == 2 && ins.op_kind(1) == OpKind::Memory
                && ins.memory_index() != Register::None && ins.memory_index_scale() == 4 && ins.memory_size().size() == 4 {
                let tbl = ins.memory_displacement64() & 0xFFFFFFFF;
                if !(0 < tbl && tbl < self.m.image_size) {
                    continue;
                }
                for k in 0..bound.unwrap_or(4096) {
                    let p = (tbl + 4 * k) as usize;
                    if p + 4 > img.len() {
                        break;
                    }
                    let t = self.m.base + self.u32_at(p) as u64;
                    if bound.is_some() {
                        if !self.in_code(t) {
                            break;
                        }
                    } else if !(r <= t && t < end) && (self.fam(t).is_none() || self.fam(t) != self.fam(r)) {
                        break;
                    }
                    targets.insert(t);
                }
            }
        }
        let mut lea: HashMap<Register, u64> = HashMap::new();
        for ins in insns {
            let m = mn(ins);
            if m == "LEA" && ins.is_ip_rel_memory_operand() {
                lea.insert(ins.op_register(0), ins.ip_rel_memory_address());
            } else if m == "MOVSXD" && ins.op_kind(1) == OpKind::Memory && lea.contains_key(&ins.memory_base())
                && ins.memory_index_scale() == 4 && ins.memory_displacement64() == 0 {
                let tbl = lea[&ins.memory_base()];
                for k in 0..bound.unwrap_or(4096) {
                    let p = tbl as i64 - self.m.base as i64 + 4 * k as i64;
                    if !(0 <= p && (p + 4) as usize <= img.len()) {
                        break;
                    }
                    let d = self.u32_at(p as usize) as i32 as i64;
                    let t = (tbl as i64 + d) as u64;
                    if !self.in_code(t) || (bound.is_none() && !(r <= t && t < end) && (self.fam(t).is_none() || self.fam(t) != self.fam(r))) {
                        break;
                    }
                    targets.insert(t);
                }
            }
        }
        targets
    }

    fn own(&self, r: u64, t: u64, end: u64) -> bool {
        r <= t && t < end && (t == r || !self.roots.contains(&t))
    }

    fn scan(&mut self, cache: &mut HashMap<u64, Instruction>, r: u64) -> (BTreeSet<u64>, BTreeSet<u64>) {
        let end = self.region_end(r);
        let mut labels = BTreeSet::new();
        labels.insert(r);
        let mut work = vec![r];
        let mut done = HashSet::new();
        let mut extra = BTreeSet::new();
        while let Some(mut a) = work.pop() {
            while !done.contains(&a) {
                if a != r && self.roots.contains(&a) {
                    extra.insert(a);
                    break;
                }
                done.insert(a);
                let ins = self.decode(cache, a);
                if ins.code() == Code::INVALID {
                    break;
                }
                let fc = ins.flow_control();
                let nxt = ins.next_ip();
                let m = mn(&ins);
                if fc == FlowControl::ConditionalBranch {
                    let t = ins.near_branch_target();
                    if self.own(r, t, end) {
                        labels.insert(t);
                        work.push(t);
                    } else {
                        extra.insert(t);
                    }
                    if nxt == r || !self.roots.contains(&nxt) {
                        labels.insert(nxt);
                    }
                } else if fc == FlowControl::UnconditionalBranch {
                    let t = ins.near_branch_target();
                    if self.own(r, t, end) {
                        labels.insert(t);
                        work.push(t);
                    } else if self.in_code(t) {
                        extra.insert(t);
                    }
                    break;
                } else if fc == FlowControl::IndirectBranch {
                    let w = self.window(&ins);
                    let tbl = self.jump_table(r, end, &w);
                    if !tbl.is_empty() {
                        self.jt.insert(ins.ip(), tbl.iter().copied().collect());
                    }
                    for t in tbl {
                        if self.own(r, t, end) {
                            labels.insert(t);
                            work.push(t);
                        } else {
                            extra.insert(t);
                        }
                    }
                    break;
                } else if fc == FlowControl::Call {
                    let t = ins.near_branch_target();
                    if self.in_code(t) {
                        extra.insert(t);
                    }
                } else if fc == FlowControl::Return || fc == FlowControl::Exception || m == "INT3" || m == "UD2" || m == "HLT" {
                    break;
                } else if fc == FlowControl::Interrupt {
                    break;
                }
                if m == "LEA" && ins.is_ip_rel_memory_operand() && self.in_code(ins.ip_rel_memory_address()) {
                    extra.insert(ins.ip_rel_memory_address());
                }
                if self.m.bits == 32 {
                    for i in 0..ins.op_count() {
                        let k = ins.op_kind(i);
                        if k == OpKind::Immediate32 || k == OpKind::Immediate8to32 {
                            let v = ins.immediate(i) & 0xFFFFFFFF;
                            if self.in_code(v) {
                                extra.insert(v);
                            }
                        }
                    }
                }
                a = nxt;
            }
        }
        self.ends.entry(r).or_insert(end);
        let _ = Mnemonic::INVALID;
        (labels, extra)
    }
}
