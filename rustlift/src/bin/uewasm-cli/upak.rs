//! Unreal Engine .pak files: the index (version 11, path-hash format), entries, rewriting a pak with some entries
//! replaced, and decrypting older ones (versions 4-10). Encrypted indexes need the game's 32-byte AES key.
use crate::util::*;
use aes::cipher::{BlockCipherDecrypt, KeyInit};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

const MAGIC: u32 = 0x5A6F12E1;

pub struct Aes(aes::Aes256);

impl Aes {
    pub fn new(key: &[u8]) -> Aes {
        Aes(aes::Aes256::new_from_slice(key).expect("32-byte key"))
    }
    /// ECB over whole blocks; a partial tail is left as it is
    pub fn decrypt(&self, b: &mut [u8]) {
        for c in b.chunks_exact_mut(16) {
            let blk: &mut [u8; 16] = c.try_into().unwrap();
            self.0.decrypt_block(blk.into());
        }
    }
}

struct Reader<'a> {
    b: &'a [u8],
    p: usize,
}

impl<'a> Reader<'a> {
    fn need(&self, n: usize) -> Res<()> {
        if self.p + n > self.b.len() { err("pak index ends early") } else { Ok(()) }
    }
    fn u32(&mut self) -> Res<u32> {
        self.need(4)?;
        let v = u32::from_le_bytes(self.b[self.p..self.p + 4].try_into().unwrap());
        self.p += 4;
        Ok(v)
    }
    fn i32(&mut self) -> Res<i32> {
        Ok(self.u32()? as i32)
    }
    fn u64(&mut self) -> Res<u64> {
        self.need(8)?;
        let v = u64::from_le_bytes(self.b[self.p..self.p + 8].try_into().unwrap());
        self.p += 8;
        Ok(v)
    }
    fn raw(&mut self, n: usize) -> Res<&'a [u8]> {
        self.need(n)?;
        let v = &self.b[self.p..self.p + n];
        self.p += n;
        Ok(v)
    }
    fn fstring(&mut self) -> Res<String> {
        let n = self.i32()?;
        let s = if n == 0 {
            String::new()
        } else if n < 0 {
            let r = self.raw((-n) as usize * 2)?;
            String::from_utf16_lossy(&r.chunks_exact(2).map(|c| u16::from_le_bytes([c[0], c[1]])).collect::<Vec<_>>())
        } else {
            self.raw(n as usize)?.iter().map(|&c| c as char).collect()
        };
        Ok(s.trim_end_matches('\0').to_string())
    }
}

#[derive(Clone)]
pub struct Entry {
    pub name: String,
    pub offset: u64,
    pub size: u64,
    pub usize: u64,
    pub method: String,
    pub encrypted: bool,
    pub blocks: Vec<(u64, u64)>,
    pub block_size: u64,
    pub method_index: u32,
    pub dir: String,
    pub file: String,
}

pub struct Pak {
    pub path: PathBuf,
    pub version: u32,
    pub mount: String,
    pub entries: Vec<Entry>,
    pub seed: u64,
    pub footer_names: Vec<u8>,
    pub dirs: Vec<String>,
}

pub struct Footer {
    pub version: u32,
    pub off: u64,
    pub isize: u64,
    pub enc: bool,
    pub methods: Vec<String>,
}

fn tail_of(path: &Path) -> Res<(Vec<u8>, u64, usize)> {
    let size = std::fs::metadata(path).map_err(|e| e.to_string())?.len();
    let mut f = File::open(path).map_err(|e| e.to_string())?;
    let start = size.saturating_sub(1024);
    f.seek(SeekFrom::Start(start)).map_err(|e| e.to_string())?;
    let mut tail = vec![];
    f.read_to_end(&mut tail).map_err(|e| e.to_string())?;
    let m = MAGIC.to_le_bytes();
    let i = (0..tail.len().saturating_sub(3)).rev().find(|&i| tail[i..i + 4] == m).ok_or_else(|| format!("{}: not a pak", path.display()))?;
    Ok((tail, start, i))
}

pub fn footer(path: &Path) -> Res<Footer> {
    let (tail, _, i) = tail_of(path)?;
    if i + 24 > tail.len() {
        return err(format!("{}: not a pak", path.display()));
    }
    let version = u32::from_le_bytes(tail[i + 4..i + 8].try_into().unwrap());
    let off = u64::from_le_bytes(tail[i + 8..i + 16].try_into().unwrap());
    let isize = u64::from_le_bytes(tail[i + 16..i + 24].try_into().unwrap());
    let enc = i > 0 && tail[i - 1] != 0;
    let mut methods = vec![];
    let mut j = i + 44;
    while j + 32 <= tail.len() {
        let s = &tail[j..j + 32];
        let n: String = s.iter().take_while(|&&c| c != 0).map(|&c| c as char).collect();
        if !n.is_empty() {
            methods.push(n);
        }
        j += 32;
    }
    Ok(Footer { version, off, isize, enc, methods })
}

fn read_at(f: &mut File, off: u64, size: u64, key: Option<&Aes>) -> Res<Vec<u8>> {
    f.seek(SeekFrom::Start(off)).map_err(|e| e.to_string())?;
    let mut b = vec![0u8; size as usize];
    f.read_exact(&mut b).map_err(|e| e.to_string())?;
    if let Some(k) = key {
        b.truncate(b.len() / 16 * 16);
        k.decrypt(&mut b);
    }
    Ok(b)
}

fn encoded(r: &mut Reader, name: String, methods: &[String]) -> Res<Entry> {
    let flags = r.u32()?;
    let method = (flags >> 23) & 0x3F;
    let enc = flags >> 22 & 1 != 0;
    let nblocks = (flags >> 6) & 0xFFFF;
    // all six bits set: the real block size follows as its own field
    let bsize = if flags & 0x3F == 0x3F { r.u32()? as u64 } else { ((flags & 0x3F) as u64) << 11 };
    let off = if flags >> 31 & 1 != 0 { r.u32()? as u64 } else { r.u64()? };
    let usize_ = if flags >> 30 & 1 != 0 { r.u32()? as u64 } else { r.u64()? };
    let size = if method != 0 { if flags >> 29 & 1 != 0 { r.u32()? as u64 } else { r.u64()? } } else { usize_ };
    let mut blocks = vec![];
    if nblocks == 1 && !enc {
        blocks.push((0, size));
    } else if nblocks > 0 {
        // block starts are relative to the entry; the entry header (serialized FPakEntry) precedes the data
        let mut pos = 0u64;
        for _ in 0..nblocks {
            let n = r.u32()? as u64;
            blocks.push((pos, pos + n));
            pos += if enc { (n + 15) / 16 * 16 } else { n };
        }
    }
    let m = if method > 0 { methods.get(method as usize - 1).cloned().unwrap_or_default() } else { String::new() };
    Ok(Entry { name, offset: off, size, usize: usize_, method: m, encrypted: enc, blocks, block_size: bsize, method_index: method, dir: String::new(), file: String::new() })
}

pub fn open_pak(path: &Path, key: Option<&[u8]>) -> Res<Pak> {
    let ft = footer(path)?;
    if ft.version < 11 {
        return err(format!("{}: pak version {} is not supported yet", path.display(), ft.version));
    }
    let aes = key.filter(|_| ft.enc).map(Aes::new);
    let mut f = File::open(path).map_err(|e| e.to_string())?;
    let ib = read_at(&mut f, ft.off, ft.isize, aes.as_ref())?;
    let mut idx = Reader { b: &ib, p: 0 };
    let mount = idx.fstring()?;
    if !mount.starts_with("../") && !mount.starts_with('/') {
        return err(format!("{}: index didn't decrypt (wrong or missing AES key)", path.display()));
    }
    idx.i32()?;
    let seed = idx.u64()?;
    if idx.u32()? != 0 {
        idx.u64()?;
        idx.u64()?;
        idx.raw(20)?;
    }
    if idx.u32()? == 0 {
        return err(format!("{}: no full directory index", path.display()));
    }
    let (fd_off, fd_size) = (idx.u64()?, idx.u64()?);
    idx.raw(20)?;
    let enc_size = idx.i32()? as usize;
    let enc_bytes = idx.raw(enc_size)?.to_vec();
    let fdb = read_at(&mut f, fd_off, fd_size, aes.as_ref())?;
    let mut fd = Reader { b: &fdb, p: 0 };
    let (mut entries, mut dirs) = (vec![], vec![]);
    for _ in 0..fd.i32()? {
        let d = fd.fstring()?;
        dirs.push(d.clone());
        for _ in 0..fd.i32()? {
            let n = fd.fstring()?;
            let eo = fd.i32()?;
            if eo < 0 {
                continue;
            }
            let mut r = Reader { b: &enc_bytes, p: eo as usize };
            let mut e = encoded(&mut r, format!("{d}{n}").trim_start_matches('/').to_string(), &ft.methods)?;
            e.dir = d.clone();
            e.file = n;
            entries.push(e);
        }
    }
    let (tail, _, i) = tail_of(path)?;
    let footer_names = tail[(i + 44).min(tail.len())..(i + 44 + 160).min(tail.len())].to_vec();
    Ok(Pak { path: path.to_path_buf(), version: ft.version, mount, entries, seed, footer_names, dirs })
}

/// the key, as eight 4-byte immediate stores (or four 8-byte ones, or plain data) in shipping builds
pub fn find_aes_key(exe: &Path, pak: &Path) -> Option<Vec<u8>> {
    let ft = footer(pak).ok()?;
    if !ft.enc {
        return None;
    }
    let mut f = File::open(pak).ok()?;
    let block = read_at(&mut f, ft.off, 16, None).ok()?;
    let data = std::fs::read(exe).ok()?;
    let ok = |k: &[u8]| {
        let mut d = block.clone();
        Aes::new(k).decrypt(&mut d);
        let n = i32::from_le_bytes(d[0..4].try_into().unwrap());
        4 < n && n < 1024 && &d[4..7] == b"../"
    };
    let pats: [(regex::bytes::Regex, usize, usize, usize); 2] = [
        (regex::bytes::Regex::new(r"(?-u)\xc7[\x40-\x47]").unwrap(), 3, 8, 4),
        (regex::bytes::Regex::new(r"(?-u)[\x48\x49][\xb8-\xbf]").unwrap(), 2, 4, 8),
    ];
    for (pat, at, n, width) in &pats {
        let hits: Vec<(usize, &[u8])> = pat.find_iter(&data).filter(|m| m.start() + at + width <= data.len())
            .map(|m| (m.start(), &data[m.start() + at..m.start() + at + width])).collect();
        for k in 0..(hits.len() + 1).saturating_sub(*n) {
            let w = &hits[k..k + n];
            if w[n - 1].0 - w[0].0 <= 96 {
                let key: Vec<u8> = w.iter().flat_map(|x| x.1.iter().copied()).collect();
                if ok(&key) {
                    return Some(key);
                }
            }
        }
    }
    let mut p = 0;
    while p + 32 < data.len() {
        let k = &data[p..p + 32];
        if k.iter().filter(|&&c| c == 0).count() <= 6 && ok(k) {
            return Some(k.to_vec());
        }
        p += 4;
    }
    None
}

/// the serialized entry record that precedes each entry's data (version 11)
pub fn header_size(e: &Entry) -> u64 {
    if e.method.is_empty() { 53 } else { 57 + 16 * e.blocks.len() as u64 }
}

type OodleFn = unsafe extern "system" fn(*const u8, isize, *mut u8, isize, i32, i32, i32, *mut u8, isize, *mut u8, *mut u8, *mut u8, isize, i32) -> isize;

fn oodle_fn(dll: &Path) -> Res<OodleFn> {
    #[cfg(windows)]
    unsafe {
        let w: Vec<u16> = dll.as_os_str().to_string_lossy().encode_utf16().chain([0]).collect();
        let h = LoadLibraryW(w.as_ptr());
        if h == 0 {
            return err(format!("couldn't load {}", dll.display()));
        }
        let p = GetProcAddress(h, b"OodleLZ_Decompress\0".as_ptr());
        if p.is_null() {
            return err(format!("{} has no OodleLZ_Decompress", dll.display()));
        }
        return Ok(std::mem::transmute::<*const std::ffi::c_void, OodleFn>(p));
    }
    #[allow(unreachable_code)]
    err("Oodle needs Windows")
}

/// the entry's original bytes: decrypted and decompressed. oodle: an Oodle DLL for Oodle-compressed entries
pub fn read_entry(f: &mut File, e: &Entry, key: Option<&[u8]>, oodle: Option<&Path>) -> Res<Vec<u8>> {
    let aes = if e.encrypted { key.map(Aes::new) } else { None };
    let base = e.offset + header_size(e);
    let mut chunk = |start: u64, n: u64| -> Res<Vec<u8>> {
        let want = if aes.is_some() { (n + 15) / 16 * 16 } else { n };
        let mut b = read_at(f, base + start, want, None)?;
        if let Some(a) = &aes {
            a.decrypt(&mut b);
        }
        b.truncate(n as usize);
        Ok(b)
    };
    if e.method.is_empty() {
        return chunk(0, e.size);
    }
    let mut out = Vec::with_capacity(e.usize as usize);
    for &(s, t) in &e.blocks {
        let raw = chunk(s, t - s)?;
        let want = (if e.block_size > 0 { e.block_size } else { e.usize }).min(e.usize - out.len() as u64) as usize;
        match e.method.as_str() {
            "Oodle" => {
                let dll = oodle.ok_or_else(|| format!("{}: Oodle-compressed, and no Oodle DLL (oo2core_*_win64.dll) was found; set UEWASM_OODLE", e.name))?;
                let dec = oodle_fn(dll)?;
                let mut o = vec![0u8; want];
                let n = unsafe { dec(raw.as_ptr(), raw.len() as isize, o.as_mut_ptr(), want as isize, 1, 0, 0, std::ptr::null_mut(), 0,
                    std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut(), 0, 3) };
                if n != want as isize {
                    return err(format!("{}: Oodle decompression failed", e.name));
                }
                out.extend_from_slice(&o);
            }
            "Zlib" => {
                flate2::read::ZlibDecoder::new(&raw[..]).read_to_end(&mut out).map_err(|x| format!("{}: {x}", e.name))?;
            }
            "Gzip" => {
                flate2::read::GzDecoder::new(&raw[..]).read_to_end(&mut out).map_err(|x| format!("{}: {x}", e.name))?;
            }
            m => return err(format!("{}: compression {m} not supported", e.name)),
        }
    }
    Ok(out)
}

/// FNV-1a 64 of the lowercase UTF-16 path relative to the mount point, offset basis plus the pak's seed
pub fn path_hash(path: &str, seed: u64) -> u64 {
    let mut h = 0xcbf29ce484222325u64.wrapping_add(seed);
    for u in path.to_lowercase().encode_utf16() {
        for b in u.to_le_bytes() {
            h = (h ^ b as u64).wrapping_mul(0x100000001b3);
        }
    }
    h
}

fn stored_len(e: &Entry) -> u64 {
    let pad = |n: u64| if e.encrypted { (n + 15) / 16 * 16 } else { n };
    if e.method.is_empty() {
        return pad(e.size);
    }
    let (s, t) = *e.blocks.last().unwrap();
    s + pad(t - s)
}

fn fstr(s: &str) -> Vec<u8> {
    if s.is_empty() {
        return 0i32.to_le_bytes().to_vec();
    }
    if s.chars().all(|c| (c as u32) < 256) {
        let mut b: Vec<u8> = s.chars().map(|c| c as u8).collect();
        b.push(0);
        let mut o = (b.len() as i32).to_le_bytes().to_vec();
        o.extend(b);
        o
    } else {
        let u: Vec<u16> = s.encode_utf16().chain([0]).collect();
        let mut o = (-(u.len() as i32)).to_le_bytes().to_vec();
        o.extend(u.iter().flat_map(|x| x.to_le_bytes()));
        o
    }
}

fn varint(v: u64, out: &mut Vec<u8>) {
    if v < 1 << 32 { out.extend((v as u32).to_le_bytes()) } else { out.extend(v.to_le_bytes()) }
}

fn encode(offset: u64, usize_: u64, size: u64, method_index: u32, encrypted: bool, blocks: &[(u64, u64)], bsize: u64) -> Vec<u8> {
    let mut flags = (method_index & 0x3F) << 23 | (encrypted as u32) << 22 | (blocks.len() as u32 & 0xFFFF) << 6;
    let small = bsize > 0 && bsize % 2048 == 0 && bsize >> 11 < 0x3F;
    flags |= if small { (bsize >> 11) as u32 } else if !blocks.is_empty() { 0x3F } else { 0 };
    flags |= ((offset < 1 << 32) as u32) << 31 | ((usize_ < 1 << 32) as u32) << 30 | ((size < 1 << 32) as u32) << 29;
    let mut out = flags.to_le_bytes().to_vec();
    if !blocks.is_empty() && !small {
        out.extend((bsize as u32).to_le_bytes());
    }
    varint(offset, &mut out);
    varint(usize_, &mut out);
    if method_index != 0 {
        varint(size, &mut out);
    }
    if !blocks.is_empty() && !(blocks.len() == 1 && !encrypted) {
        for &(s, t) in blocks {
            out.extend(((t - s) as u32).to_le_bytes());
        }
    }
    out
}

fn io(e: std::io::Error) -> String {
    e.to_string()
}

/// Rewrites a pak with some entries replaced (name -> file). Untouched entries are copied as stored (still compressed
/// and encrypted with the game's key); replacements are stored plain. The index is written unencrypted.
pub fn write_pak(pak: &Pak, dst: &Path, replace: &std::collections::HashMap<String, PathBuf>) -> Res<()> {
    let mut entries: Vec<&Entry> = pak.entries.iter().collect();
    entries.sort_by_key(|e| e.offset);
    let tmp = PathBuf::from(format!("{}.tmp", dst.display()));
    let mut src = File::open(&pak.path).map_err(io)?;
    let mut out = std::io::BufWriter::new(File::create(&tmp).map_err(io)?);
    let mut pos = 0u64;
    let mut out_entries: Vec<(&Entry, Vec<u8>)> = vec![];
    let mut buf = vec![0u8; 8 << 20];
    for e in entries {
        let start = pos;
        if let Some(r) = replace.get(&e.name) {
            let n = std::fs::metadata(r).map_err(io)?.len();
            let mut h = Sha1::new();
            let mut rf = File::open(r).map_err(io)?;
            loop {
                let k = rf.read(&mut buf).map_err(io)?;
                if k == 0 {
                    break;
                }
                h.update(&buf[..k]);
            }
            let mut rec = vec![];
            rec.extend(0i64.to_le_bytes());
            rec.extend((n as i64).to_le_bytes());
            rec.extend((n as i64).to_le_bytes());
            rec.extend(0u32.to_le_bytes());
            rec.extend(h.finish());
            rec.push(0);
            rec.extend(0u32.to_le_bytes());
            out.write_all(&rec).map_err(io)?;
            std::io::copy(&mut File::open(r).map_err(io)?, &mut out).map_err(io)?;
            pos += 53 + n;
            out_entries.push((e, encode(start, n, n, 0, false, &[], 0)));
        } else {
            let n = header_size(e) + stored_len(e);
            src.seek(SeekFrom::Start(e.offset)).map_err(io)?;
            let mut left = n;
            while left > 0 {
                let k = src.read(&mut buf[..left.min(8 << 20) as usize]).map_err(io)?;
                if k == 0 {
                    return err(format!("{}: entry {} runs past the end", pak.path.display(), e.name));
                }
                out.write_all(&buf[..k]).map_err(io)?;
                left -= k as u64;
            }
            pos += n;
            out_entries.push((e, encode(start, e.usize, e.size, e.method_index, e.encrypted, &e.blocks, e.block_size)));
        }
    }
    let mut enc = vec![];
    let mut where_ = std::collections::HashMap::new();
    for (e, b) in &out_entries {
        where_.insert(e.name.clone(), enc.len() as i32);
        enc.extend(b);
    }
    // keep empty directories too: the engine asks whether they exist (e.g. the ICU data folder)
    let mut dirs: Vec<(String, Vec<(String, i32)>)> = pak.dirs.iter().map(|d| (d.clone(), vec![])).collect();
    for (e, _) in &out_entries {
        let o = where_[&e.name];
        match dirs.iter_mut().find(|(d, _)| *d == e.dir) {
            Some((_, v)) => v.push((e.file.clone(), o)),
            None => dirs.push((e.dir.clone(), vec![(e.file.clone(), o)])),
        }
    }
    let mut dir_index = (dirs.len() as i32).to_le_bytes().to_vec();
    for (d, fs) in &dirs {
        dir_index.extend(fstr(d));
        dir_index.extend((fs.len() as i32).to_le_bytes());
        for (f, o) in fs {
            dir_index.extend(fstr(f));
            dir_index.extend(o.to_le_bytes());
        }
    }
    let mut hashes: Vec<(u64, i32)> = out_entries.iter().map(|(e, _)| (path_hash(&e.name, pak.seed), where_[&e.name])).collect();
    hashes.sort();
    let mut hash_index = (hashes.len() as i32).to_le_bytes().to_vec();
    for (h, o) in &hashes {
        hash_index.extend(h.to_le_bytes());
        hash_index.extend(o.to_le_bytes());
    }
    hash_index.extend(&dir_index);
    let index_off = pos;
    let primary = |ph: u64, fd: u64| {
        let mut b = fstr(&pak.mount);
        b.extend((out_entries.len() as i32).to_le_bytes());
        b.extend(pak.seed.to_le_bytes());
        b.extend(1u32.to_le_bytes());
        b.extend(ph.to_le_bytes());
        b.extend((hash_index.len() as u64).to_le_bytes());
        b.extend(sha1(&hash_index));
        b.extend(1u32.to_le_bytes());
        b.extend(fd.to_le_bytes());
        b.extend((dir_index.len() as u64).to_le_bytes());
        b.extend(sha1(&dir_index));
        b.extend((enc.len() as i32).to_le_bytes());
        b.extend(&enc);
        b.extend(0i32.to_le_bytes());
        b
    };
    let size0 = primary(0, 0).len() as u64;
    let ph_off = index_off + size0;
    let fd_off = ph_off + hash_index.len() as u64;
    let prim = primary(ph_off, fd_off);
    out.write_all(&prim).map_err(io)?;
    out.write_all(&hash_index).map_err(io)?;
    out.write_all(&dir_index).map_err(io)?;
    let mut ft = vec![0u8; 17];
    ft.extend(MAGIC.to_le_bytes());
    ft.extend(pak.version.to_le_bytes());
    ft.extend((index_off as i64).to_le_bytes());
    ft.extend((prim.len() as i64).to_le_bytes());
    ft.extend(sha1(&prim));
    ft.extend(&pak.footer_names);
    out.write_all(&ft).map_err(io)?;
    out.flush().map_err(io)?;
    drop(out);
    std::fs::rename(&tmp, dst).map_err(io)
}

/// an AES-256 key as 64 hex digits (0x optional) or base64; None when it isn't one
pub fn parse_key(text: &str) -> Option<Vec<u8>> {
    use base64::Engine;
    let mut t: String = text.trim().chars().filter(|c| *c != ' ').collect();
    if t.len() >= 2 && t[..2].eq_ignore_ascii_case("0x") {
        t = t[2..].to_string();
    }
    if t.len() == 64 && t.chars().all(|c| c.is_ascii_hexdigit()) {
        return (0..32).map(|i| u8::from_str_radix(&t[2 * i..2 * i + 2], 16).ok()).collect();
    }
    let b = base64::engine::general_purpose::STANDARD.decode(t.as_bytes()).ok()?;
    (b.len() == 32).then_some(b)
}

/// paks under root whose index is encrypted
pub fn encrypted_paks(root: &Path) -> Vec<PathBuf> {
    files_under(root).into_iter().filter(|p| p.extension().map_or(false, |e| e.eq_ignore_ascii_case("pak")))
        .filter(|p| footer(p).map_or(false, |f| f.enc)).collect()
}

/// the key decrypts this pak's index into something that starts like one (a mount point FString)
pub fn check_key(pak: &Path, key: &[u8]) -> bool {
    let Ok(ft) = footer(pak) else { return false };
    if !ft.enc {
        return true;
    }
    let Ok(mut f) = File::open(pak) else { return false };
    let Ok(d) = read_at(&mut f, ft.off, 16, Some(&Aes::new(key))) else { return false };
    let n = i32::from_le_bytes(d[0..4].try_into().unwrap());
    0 < n && n < 1024 && (&d[4..7] == b"../" || d[4] == b'/')
}

struct Legacy {
    off: u64,
    size: u64,
    method: u32,
    blocks: Vec<(u64, u64)>,
    flags: u8,
    flag_at: usize,
}

/// entries of a pre-path-hash index (versions 3-10)
fn legacy_index(idx: &mut Reader, version: u32) -> Res<Vec<Legacy>> {
    let mut out = vec![];
    for _ in 0..idx.i32()? {
        idx.fstring()?;
        let (off, size, _usize) = (idx.u64()?, idx.u64()?, idx.u64()?);
        let method = idx.u32()?;
        if version <= 1 {
            idx.u64()?;
        }
        idx.raw(20)?;
        let mut blocks = vec![];
        if method != 0 {
            for _ in 0..idx.i32()? {
                blocks.push((idx.u64()?, idx.u64()?));
            }
        }
        let flag_at = idx.p;
        let flags = idx.raw(1)?[0];
        idx.u32()?;
        out.push(Legacy { off, size, method, blocks, flags, flag_at });
    }
    Ok(out)
}

fn legacy_entries(path: &Path, key: &[u8]) -> Res<(Footer, Vec<u8>, Vec<Legacy>)> {
    let ft = footer(path)?;
    let mut f = File::open(path).map_err(io)?;
    let aes = Aes::new(key);
    let idx = read_at(&mut f, ft.off, ft.isize, if ft.enc { Some(&aes) } else { None })?;
    let mut r = Reader { b: &idx, p: 0 };
    r.fstring()?;
    let v = legacy_index(&mut r, ft.version)?;
    Ok((ft, idx, v))
}

/// A copy of an older pak (versions 4-10) with the index and every entry decrypted in place: same layout, flags
/// cleared, so the engine reads it as a plain pak (and the data compresses).
pub fn decrypt_pak(path: &Path, key: &[u8], dst: &Path, log: Log) -> Res<()> {
    let ft = footer(path)?;
    if !(4..=10).contains(&ft.version) {
        return err(format!("{}: pak version {} can't be decrypted here (versions 4-10 are)", path.display(), ft.version));
    }
    if !check_key(path, key) {
        return err(format!("{}: the AES key doesn't decrypt this pak", path.display()));
    }
    let aes = Aes::new(key);
    let tmp = PathBuf::from(format!("{}.tmp", dst.display()));
    std::fs::copy(path, &tmp).map_err(io)?;
    let (_, mut index, entries) = legacy_entries(path, key)?;
    let mut f = File::options().read(true).write(true).open(&tmp).map_err(io)?;
    let dec = |f: &mut File, a: u64, n: u64| -> Res<()> {
        let n = (n + 15) / 16 * 16;
        let mut b = read_at(f, a, n, None)?;
        aes.decrypt(&mut b);
        f.seek(SeekFrom::Start(a)).map_err(io)?;
        f.write_all(&b).map_err(io)
    };
    let mut count = 0;
    for e in &entries {
        if e.flags & 1 == 0 {
            continue;
        }
        count += 1;
        let hdr = 53 + if e.method != 0 { 4 + 16 * e.blocks.len() as u64 } else { 0 };
        if e.method != 0 {
            for &(s, t) in &e.blocks {
                dec(&mut f, if ft.version < 5 { s } else { e.off + s }, t - s)?;
            }
        } else {
            dec(&mut f, e.off + hdr, e.size)?;
        }
        f.seek(SeekFrom::Start(e.off + hdr - 5)).map_err(io)?;
        f.write_all(&[e.flags & !1]).map_err(io)?;
        index[e.flag_at] = e.flags & !1;
    }
    f.seek(SeekFrom::Start(ft.off)).map_err(io)?;
    f.write_all(&index).map_err(io)?;
    drop(f);
    let (_, base, i) = tail_of(&tmp)?;
    let mut f = File::options().read(true).write(true).open(&tmp).map_err(io)?;
    f.seek(SeekFrom::Start(base + i as u64 - 1)).map_err(io)?;
    f.write_all(&[0]).map_err(io)?;
    f.seek(SeekFrom::Start(base + i as u64 + 24)).map_err(io)?;
    f.write_all(&sha1(&index)).map_err(io)?;
    drop(f);
    std::fs::rename(&tmp, dst).map_err(io)?;
    log(&format!("  {}: {count} of {} entries decrypted", path.file_name().unwrap_or_default().to_string_lossy(), entries.len()));
    Ok(())
}

/// {path relative to game_root: decrypted copy} for every encrypted pak the key opens; a wrong key fails
pub fn decrypt_paks(game_root: &Path, key: &[u8], out: &Path, log: Log, progress: Progress) -> Res<Vec<(String, PathBuf)>> {
    let paks = encrypted_paks(game_root);
    let bad: Vec<String> = paks.iter().filter(|p| !check_key(p, key)).take(5).map(|p| p.file_name().unwrap_or_default().to_string_lossy().into()).collect();
    if !bad.is_empty() {
        return err(format!("The AES key is wrong for {}. Enter the game's own correct AES key.", bad.join(", ")));
    }
    let total = paks.iter().map(|p| std::fs::metadata(p).map_or(0, |m| m.len())).sum::<u64>().max(1) as f64;
    let (mut result, mut done) = (vec![], 0f64);
    for p in &paks {
        let rel = p.strip_prefix(game_root).unwrap_or(p).to_string_lossy().replace('\\', "/");
        let ft = footer(p)?;
        if ft.version >= 11 {
            log(&format!("  {rel}: pak version {}, left encrypted (the game decrypts it as it plays)", ft.version));
            continue;
        }
        if legacy_entries(p, key)?.2.iter().all(|e| e.flags & 1 == 0) {
            log(&format!("  {rel}: only the index is encrypted; the game reads it with its own key, so it is served as it is"));
            continue;
        }
        let dst = out.join("paks_decrypted").join(&rel);
        std::fs::create_dir_all(dst.parent().unwrap()).map_err(io)?;
        let stamp = dst.with_extension("stamp");
        let md = std::fs::metadata(p).map_err(io)?;
        let mt = md.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_nanos());
        let sig = format!("{}:{}:{}", md.len(), mt, &hex(key)[..8]);
        if !(dst.exists() && std::fs::read_to_string(&stamp).ok().as_deref() == Some(sig.as_str())) {
            decrypt_pak(p, key, &dst, log)?;
            std::fs::write(&stamp, &sig).map_err(io)?;
        }
        result.push((rel, dst));
        done += md.len() as f64;
        progress("decrypt", done, total);
    }
    Ok(result)
}
