//! Unpacking .zip downloads (Emscripten's SDK, Python, ffmpeg): stored and deflated entries, Zip64 sizes.
use crate::util::{err, Res};
use std::io::Read;
use std::path::Path;

pub struct Entry {
    pub name: String,
    method: u16,
    csize: u64,
    local: u64,
}

fn u16a(b: &[u8], i: usize) -> u16 {
    u16::from_le_bytes([b[i], b[i + 1]])
}
fn u32a(b: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(b[i..i + 4].try_into().unwrap())
}
fn u64a(b: &[u8], i: usize) -> u64 {
    u64::from_le_bytes(b[i..i + 8].try_into().unwrap())
}

pub struct Zip {
    data: Vec<u8>,
    pub entries: Vec<Entry>,
}

impl Zip {
    pub fn open(p: &Path) -> Res<Zip> {
        let data = std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?;
        let Some(eocd) = (0..data.len().saturating_sub(21)).rev().take(70000).find(|&i| u32a(&data, i) == 0x06054b50) else {
            return err(format!("{}: not a zip file", p.display()));
        };
        let (mut count, mut cd) = (u16a(&data, eocd + 10) as u64, u32a(&data, eocd + 16) as u64);
        if eocd >= 20 && u32a(&data, eocd - 20) == 0x07064b50 {
            let z = u64a(&data, eocd - 12) as usize;
            count = u64a(&data, z + 32);
            cd = u64a(&data, z + 48);
        }
        let mut entries = vec![];
        let mut i = cd as usize;
        for _ in 0..count {
            if u32a(&data, i) != 0x02014b50 {
                return err("zip: bad central directory");
            }
            let method = u16a(&data, i + 10);
            let (mut csize, mut usize_, mut local) = (u32a(&data, i + 20) as u64, u32a(&data, i + 24) as u64, u32a(&data, i + 42) as u64);
            let (nl, xl, cl) = (u16a(&data, i + 28) as usize, u16a(&data, i + 30) as usize, u16a(&data, i + 32) as usize);
            let name = String::from_utf8_lossy(&data[i + 46..i + 46 + nl]).replace('\\', "/");
            let mut x = i + 46 + nl;
            while x + 4 <= i + 46 + nl + xl {
                let (id, n) = (u16a(&data, x), u16a(&data, x + 2) as usize);
                if id == 1 {
                    let mut j = x + 4;
                    if usize_ == 0xFFFF_FFFF { usize_ = u64a(&data, j); j += 8; }
                    if csize == 0xFFFF_FFFF { csize = u64a(&data, j); j += 8; }
                    if local == 0xFFFF_FFFF { local = u64a(&data, j); }
                }
                x += 4 + n;
            }
            entries.push(Entry { name, method, csize, local });
            i += 46 + nl + xl + cl;
        }
        Ok(Zip { data, entries })
    }

    pub fn read(&self, e: &Entry) -> Res<Vec<u8>> {
        let l = e.local as usize;
        let start = l + 30 + u16a(&self.data, l + 26) as usize + u16a(&self.data, l + 28) as usize;
        let raw = &self.data[start..start + e.csize as usize];
        match e.method {
            0 => Ok(raw.to_vec()),
            8 => {
                let mut out = vec![];
                flate2::read::DeflateDecoder::new(raw).read_to_end(&mut out).map_err(|x| format!("{}: {x}", e.name))?;
                Ok(out)
            }
            m => err(format!("{}: compression method {m} not supported", e.name)),
        }
    }

    /// every entry under dest; strip_top drops the archive's single top folder
    pub fn extract(&self, dest: &Path, strip_top: bool) -> Res<()> {
        for e in &self.entries {
            let rel = if strip_top { e.name.split_once('/').map(|x| x.1).unwrap_or("") } else { e.name.as_str() };
            if rel.is_empty() || rel.split('/').any(|c| c == "..") {
                continue;
            }
            let t = dest.join(rel);
            if rel.ends_with('/') {
                std::fs::create_dir_all(&t).map_err(|x| x.to_string())?;
                continue;
            }
            if let Some(p) = t.parent() {
                std::fs::create_dir_all(p).map_err(|x| x.to_string())?;
            }
            std::fs::write(&t, self.read(e)?).map_err(|x| format!("{}: {x}", t.display()))?;
        }
        Ok(())
    }
}
