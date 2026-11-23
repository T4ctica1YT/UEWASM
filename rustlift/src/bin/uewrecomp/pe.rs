//! PE images (x86 and x86-64): sections, imports, delay imports, exports, relocations, unwind data, TLS.
use std::collections::{BTreeMap, HashMap};
use std::path::Path;

#[derive(Clone)]
pub struct Section {
    pub rva: u32,
    pub vsize: u32,
    pub raw: u32,
    pub rsize: u32,
    pub flags: u32,
}
impl Section {
    pub fn executable(&self) -> bool {
        self.flags & 0x2000_0000 != 0
    }
}

#[derive(Clone)]
pub struct Import {
    pub dll: String,
    pub name: String,
    pub iat_rva: u32,
}

/// an export: an RVA, or a forwarder ("DLL.Name")
#[derive(Clone)]
pub enum Export {
    Rva(u32),
    Forward(String),
}

#[derive(Clone, Copy)]
pub struct Tls {
    pub start: u64,
    pub end: u64,
    pub index: u64,
    pub zero: u32,
}

pub struct Pe {
    pub data: Vec<u8>,
    pub image_base: u64,
    pub entry_rva: u32,
    pub image_size: u32,
    pub bits: u32,
    pub sections: Vec<Section>,
    pub imports: Vec<Import>,
    pub delay_imports: Vec<Import>,
    pub relocs: Vec<u32>,
    pub functions: Vec<(u32, u32)>,
    pub unwind: BTreeMap<u32, u32>,
    pub family: BTreeMap<u32, u32>,
    pub exports: HashMap<String, Export>,
    pub exports_ord: BTreeMap<u32, Export>,
    pub ord_names: HashMap<u32, String>,
    pub tls_callbacks: Vec<u32>,
    pub tls: Option<Tls>,
}

fn u16_at(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]) as u32)
}
fn u32_at(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()))
}
fn u64_at(d: &[u8], o: usize) -> Option<u64> {
    d.get(o..o + 8).map(|b| u64::from_le_bytes(b.try_into().unwrap()))
}

impl Pe {
    pub fn ptr(&self) -> u32 {
        self.bits / 8
    }
    pub fn off(&self, rva: u32) -> Option<usize> {
        for s in &self.sections {
            if s.rva <= rva && (rva as u64) < s.rva as u64 + s.vsize.max(s.rsize) as u64 {
                let d = rva - s.rva;
                return if d < s.rsize { Some((s.raw + d) as usize) } else { None };
            }
        }
        if self.sections.first().map_or(true, |s| rva < s.rva) {
            Some(rva as usize)
        } else {
            None
        }
    }
    pub fn u32(&self, rva: u32) -> Option<u32> {
        u32_at(&self.data, self.off(rva)?)
    }
    pub fn uptr(&self, rva: u32) -> Option<u64> {
        let o = self.off(rva)?;
        if self.bits == 64 { u64_at(&self.data, o) } else { u32_at(&self.data, o).map(|v| v as u64) }
    }
    pub fn cstr(&self, rva: u32) -> Option<String> {
        let o = self.off(rva)?;
        let end = self.data[o..].iter().position(|&b| b == 0)? + o;
        Some(self.data[o..end].iter().map(|&b| b as char).collect())
    }
    /// the module image relocated to base, with the header naming that base (as Windows' loader leaves it)
    pub fn image(&self, base: u64) -> Vec<u8> {
        let mut img = vec![0u8; self.image_size as usize];
        let hdr = self.sections.first().map_or(0x400, |s| s.raw as usize).min(self.data.len()).min(img.len());
        img[..hdr].copy_from_slice(&self.data[..hdr]);
        for s in &self.sections {
            let n = s.rsize.min(if s.vsize != 0 { s.vsize } else { s.rsize }) as usize;
            let (src, dst) = (s.raw as usize, s.rva as usize);
            let n = n.min(self.data.len().saturating_sub(src)).min(img.len().saturating_sub(dst));
            img[dst..dst + n].copy_from_slice(&self.data[src..src + n]);
        }
        let delta = base.wrapping_sub(self.image_base);
        if let Some(e) = u32_at(&img, 0x3C) {
            let opt = e as usize + 24;
            if self.bits == 64 {
                if opt + 32 <= img.len() {
                    img[opt + 24..opt + 32].copy_from_slice(&base.to_le_bytes());
                }
            } else if opt + 32 <= img.len() {
                img[opt + 28..opt + 32].copy_from_slice(&(base as u32).to_le_bytes());
            }
        }
        for &rva in &self.relocs {
            let r = rva as usize;
            if self.bits == 64 {
                if let Some(v) = u64_at(&img, r) {
                    img[r..r + 8].copy_from_slice(&v.wrapping_add(delta).to_le_bytes());
                }
            } else if let Some(v) = u32_at(&img, r) {
                img[r..r + 4].copy_from_slice(&v.wrapping_add(delta as u32).to_le_bytes());
            }
        }
        img
    }
}

pub fn load(path: &Path) -> Result<Pe, String> {
    let data = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
    parse(path, data)
}

pub fn parse(path: &Path, data: Vec<u8>) -> Result<Pe, String> {
    let bad = |m: &str| format!("{}: {m}", path.display());
    if data.get(..2) != Some(b"MZ") {
        return Err(bad("not a PE file"));
    }
    let (data, stub) = crate::steamstub::unwrap(data);
    if let Some(h) = stub {
        println!("{}: SteamStub wrapper removed (app {}, entry {:#x})", path.file_name().unwrap_or_default().to_string_lossy(), h.app_id, h.oep);
    }
    let pe_off = u32_at(&data, 0x3C).ok_or_else(|| bad("truncated"))? as usize;
    if data.get(pe_off..pe_off + 4) != Some(b"PE\0\0") {
        return Err(bad("bad PE signature"));
    }
    let machine = u16_at(&data, pe_off + 4).ok_or_else(|| bad("truncated"))?;
    let nsec = u16_at(&data, pe_off + 6).unwrap_or(0) as usize;
    let optsize = u16_at(&data, pe_off + 20).unwrap_or(0) as usize;
    if machine != 0x8664 && machine != 0x14C {
        return Err(bad("only x86 and x86-64 executables are supported"));
    }
    let opt = pe_off + 24;
    let magic = u16_at(&data, opt).unwrap_or(0);
    if !((machine == 0x8664 && magic == 0x20B) || (machine == 0x14C && magic == 0x10B)) {
        return Err(bad("unexpected optional header"));
    }
    let bits = if magic == 0x20B { 64 } else { 32 };
    let entry_rva = u32_at(&data, opt + 16).unwrap_or(0);
    let image_base = if bits == 64 { u64_at(&data, opt + 24).unwrap_or(0) } else { u32_at(&data, opt + 28).unwrap_or(0) as u64 };
    let image_size = u32_at(&data, opt + 56).unwrap_or(0);
    let dd = opt + if bits == 64 { 112 } else { 96 };
    let ndirs = u32_at(&data, dd - 4).unwrap_or(0).min(16) as usize;
    let dirs: Vec<(u32, u32)> = (0..ndirs).map(|i| (u32_at(&data, dd + 8 * i).unwrap_or(0), u32_at(&data, dd + 8 * i + 4).unwrap_or(0))).collect();
    let mut sections = Vec::new();
    let mut so = opt + optsize;
    for _ in 0..nsec {
        sections.push(Section {
            vsize: u32_at(&data, so + 8).unwrap_or(0),
            rva: u32_at(&data, so + 12).unwrap_or(0),
            rsize: u32_at(&data, so + 16).unwrap_or(0),
            raw: u32_at(&data, so + 20).unwrap_or(0),
            flags: u32_at(&data, so + 36).unwrap_or(0),
        });
        so += 40;
    }
    let mut pe = Pe {
        data,
        image_base,
        entry_rva,
        image_size,
        bits,
        sections,
        imports: vec![],
        delay_imports: vec![],
        relocs: vec![],
        functions: vec![],
        unwind: BTreeMap::new(),
        family: BTreeMap::new(),
        exports: HashMap::new(),
        exports_ord: BTreeMap::new(),
        ord_names: HashMap::new(),
        tls_callbacks: vec![],
        tls: None,
    };
    let d = |i: usize| dirs.get(i).copied().unwrap_or((0, 0));
    let p_sz = pe.ptr();
    let top = 1u64 << (bits - 1);

    let (rva, _) = d(1);
    if rva != 0 {
        let mut p = rva;
        while let Some(o) = pe.off(p) {
            let (ilt, name_rva, iat) = (u32_at(&pe.data, o).unwrap_or(0), u32_at(&pe.data, o + 12).unwrap_or(0), u32_at(&pe.data, o + 16).unwrap_or(0));
            if name_rva == 0 {
                break;
            }
            let dll = pe.cstr(name_rva).unwrap_or_default();
            let (mut q, mut slot) = (if ilt != 0 { ilt } else { iat }, iat);
            while let Some(v) = pe.uptr(q) {
                if v == 0 {
                    break;
                }
                let name = if v & top != 0 { format!("#{}", v & 0xFFFF) } else { pe.cstr(((v & 0x7FFF_FFFF) + 2) as u32).unwrap_or_default() };
                pe.imports.push(Import { dll: dll.clone(), name, iat_rva: slot });
                q += p_sz;
                slot += p_sz;
            }
            p += 20;
        }
    }

    let (rva, _) = d(13);
    let mut p = rva;
    while rva != 0 {
        let Some(o) = pe.off(p) else { break };
        let (attrs, name_rva, iat, int_rva) =
            (u32_at(&pe.data, o).unwrap_or(0), u32_at(&pe.data, o + 4).unwrap_or(0), u32_at(&pe.data, o + 12).unwrap_or(0), u32_at(&pe.data, o + 16).unwrap_or(0));
        if name_rva == 0 {
            break;
        }
        // attribute bit 0 clear: an old (VC6) descriptor holding addresses instead of RVAs
        let fix = if attrs & 1 != 0 { 0 } else { image_base as u32 };
        let dll = pe.cstr(name_rva.wrapping_sub(fix)).unwrap_or_default();
        let (mut q, mut slot) = (int_rva.wrapping_sub(fix), iat.wrapping_sub(fix));
        while pe.off(q).is_some() {
            let Some(v) = pe.uptr(q) else { break };
            if v == 0 {
                break;
            }
            let name = if v & top != 0 {
                format!("#{}", v & 0xFFFF)
            } else {
                pe.cstr(((v & 0x7FFF_FFFF) as u32).wrapping_sub(fix) + 2).unwrap_or_default()
            };
            pe.delay_imports.push(Import { dll: dll.clone(), name, iat_rva: slot });
            q += p_sz;
            slot += p_sz;
        }
        p += 32;
    }

    let (rva, size) = d(0);
    if rva != 0 {
        if let Some(o) = pe.off(rva) {
            let g = |k: usize| u32_at(&pe.data, o + k).unwrap_or(0);
            let (ordbase, nfun, nnames, afun, anames, aords) = (g(16), g(20), g(24), g(28), g(32), g(36));
            let funcs: Vec<u32> = (0..nfun.min(1 << 20)).map(|i| pe.u32(afun + 4 * i).unwrap_or(0)).collect();
            let target = |f: u32| if rva <= f && f < rva + size { Export::Forward(pe.cstr(f).unwrap_or_default()) } else { Export::Rva(f) };
            let mut ords = BTreeMap::new();
            for (i, &f) in funcs.iter().enumerate() {
                if f != 0 {
                    ords.insert(ordbase + i as u32, target(f));
                }
            }
            let mut names = HashMap::new();
            let mut ord_names = HashMap::new();
            for i in 0..nnames.min(1 << 20) {
                let nm = pe.u32(anames + 4 * i).and_then(|r| pe.cstr(r)).unwrap_or_default();
                let ordi = pe.off(aords + 2 * i).and_then(|o| u16_at(&pe.data, o)).unwrap_or(0);
                ord_names.insert(ordbase + ordi, nm.clone());
                if let Some(&f) = funcs.get(ordi as usize) {
                    if f != 0 {
                        names.insert(nm, target(f));
                    }
                }
            }
            pe.exports_ord = ords;
            pe.exports = names;
            pe.ord_names = ord_names;
        }
    }

    let (rva, size) = d(5);
    let (mut p, end) = (rva, rva as u64 + size as u64);
    let want = if bits == 64 { 10 } else { 3 };
    while rva != 0 && (p as u64) < end {
        let Some(o) = pe.off(p) else { break };
        let (page, bsize) = (u32_at(&pe.data, o).unwrap_or(0), u32_at(&pe.data, o + 4).unwrap_or(0));
        if bsize < 8 {
            break;
        }
        for k in 0..((bsize - 8) / 2) as usize {
            if let Some(e) = u16_at(&pe.data, o + 8 + 2 * k) {
                if e >> 12 == want {
                    pe.relocs.push(page + (e & 0xFFF));
                }
            }
        }
        p += bsize;
    }

    let (rva, size) = d(3);
    if rva != 0 {
        for i in 0..size / 12 {
            let (Some(b), Some(e), Some(u)) = (pe.u32(rva + 12 * i), pe.u32(rva + 12 * i + 4), pe.u32(rva + 12 * i + 8)) else { continue };
            if b != 0 && e > b {
                pe.functions.push((b, e));
                pe.unwind.insert(b, u);
            }
        }
    }
    let mut parent = HashMap::new();
    for (&b, &u) in &pe.unwind {
        let Some(o) = pe.off(u) else { continue };
        if o + 4 > pe.data.len() {
            continue;
        }
        let (flags, ncodes) = (pe.data[o] >> 3, pe.data[o + 2] as usize);
        if flags & 4 != 0 {
            let co = o + 4 + 2 * ((ncodes + 1) & !1);
            if let Some(v) = u32_at(&pe.data, co) {
                if co + 12 <= pe.data.len() {
                    parent.insert(b, v);
                }
            }
        }
    }
    let fam: Vec<(u32, u32)> = pe
        .unwind
        .keys()
        .map(|&b| {
            let (mut r, mut n) = (b, 0);
            while let (Some(&q), true) = (parent.get(&r), n < 32) {
                r = q;
                n += 1;
            }
            (b, r)
        })
        .collect();
    pe.family = fam.into_iter().collect();

    let (rva, _) = d(9);
    if rva != 0 {
        if let Some(o) = pe.off(rva) {
            let rdp = |k: usize| if bits == 64 { u64_at(&pe.data, o + 8 * k).unwrap_or(0) } else { u32_at(&pe.data, o + 4 * k).unwrap_or(0) as u64 };
            let (start, end, idx, cb) = (rdp(0), rdp(1), rdp(2), rdp(3));
            let zero = u32_at(&pe.data, o + 4 * p_sz as usize).unwrap_or(0);
            pe.tls = Some(Tls { start: start.wrapping_sub(image_base), end: end.wrapping_sub(image_base), index: idx.wrapping_sub(image_base), zero });
            if cb != 0 {
                let mut p = cb.wrapping_sub(image_base) as u32;
                while pe.off(p).is_some() {
                    match pe.uptr(p) {
                        Some(v) if v != 0 => pe.tls_callbacks.push(v.wrapping_sub(image_base) as u32),
                        _ => break,
                    }
                    p += p_sz;
                }
            }
        }
    }
    Ok(pe)
}
