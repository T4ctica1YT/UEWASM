//! SteamStub (variant 3.x, x64) unwrapping, so a wrapped game exe can be recompiled. The stub keeps an XOR-chained
//! header just before its entry point in the .bind section, with the real entry point and, unless the "no encryption"
//! flag is set, the AES key and IV that decrypt the code section.
const SIG: u32 = 0xC0DE_C0DF;
const HEADER: usize = 0xF0;
const NO_ENCRYPTION: u32 = 0x04;

pub struct Header {
    pub image_base: u64,
    pub oep: u64,
    pub app_id: u32,
    flags: u32,
    code_va: u64,
    key: [u8; 32],
    iv: [u8; 16],
    stolen: [u8; 16],
}

fn u32_at(d: &[u8], o: usize) -> u32 {
    d.get(o..o + 4).map_or(0, |b| u32::from_le_bytes(b.try_into().unwrap()))
}
fn u64_at(d: &[u8], o: usize) -> u64 {
    d.get(o..o + 8).map_or(0, |b| u64::from_le_bytes(b.try_into().unwrap()))
}

fn xor_chain(data: &[u8]) -> Vec<u8> {
    let mut out = data.to_vec();
    let mut key = u32_at(data, 0);
    let mut i = 4;
    while i + 4 <= data.len() {
        let v = u32_at(data, i);
        out[i..i + 4].copy_from_slice(&(v ^ key).to_le_bytes());
        key = v;
        i += 4;
    }
    out
}

/// (pe header offset, sections as (header offset, name, vsize, va, rsize, raw))
fn sections(d: &[u8]) -> (usize, Vec<(usize, String, u32, u32, u32, u32)>) {
    let pe = u32_at(d, 0x3C) as usize;
    let nsec = d.get(pe + 6..pe + 8).map_or(0, |b| u16::from_le_bytes([b[0], b[1]]) as usize);
    let osz = d.get(pe + 20..pe + 22).map_or(0, |b| u16::from_le_bytes([b[0], b[1]]) as usize);
    let first = pe + 24 + osz;
    let secs = (0..nsec)
        .map(|i| {
            let o = first + 40 * i;
            let name: String = d.get(o..o + 8).unwrap_or(&[]).iter().take_while(|&&b| b != 0).map(|&b| b as char).collect();
            (o, name, u32_at(d, o + 8), u32_at(d, o + 12), u32_at(d, o + 16), u32_at(d, o + 20))
        })
        .collect();
    (pe, secs)
}

fn header(d: &[u8]) -> Option<Header> {
    let (pe, secs) = sections(d);
    if !secs.iter().any(|s| s.1 == ".bind") {
        return None;
    }
    let ep = u32_at(d, pe + 24 + 16);
    let off = secs.iter().find(|s| s.3 <= ep && (ep as u64) < s.3 as u64 + s.2.max(s.4) as u64).map(|s| (s.5 + ep - s.3) as usize)?;
    if off < HEADER || off > d.len() {
        return None;
    }
    let h = xor_chain(&d[off - HEADER..off]);
    if u32_at(&h, 4) != SIG {
        return None;
    }
    Some(Header {
        image_base: u64_at(&h, 0x08),
        oep: u64_at(&h, 0x20),
        app_id: u32_at(&h, 0x38),
        flags: u32_at(&h, 0x3C),
        code_va: u64_at(&h, 0x48),
        key: h[0x58..0x78].try_into().unwrap(),
        iv: h[0x78..0x88].try_into().unwrap(),
        stolen: h[0x88..0x98].try_into().unwrap(),
    })
}

/// the unwrapped exe and the stub's header, or the data unchanged when it isn't a SteamStub 3.x x64 exe
pub fn unwrap(data: Vec<u8>) -> (Vec<u8>, Option<Header>) {
    let Some(h) = header(&data) else { return (data, None) };
    let mut d = data;
    let (pe, secs) = sections(&d);
    if h.flags & NO_ENCRYPTION == 0 {
        if let Some(code) = secs.iter().find(|s| s.3 as u64 <= h.code_va && h.code_va < s.3 as u64 + s.2 as u64) {
            let (raw, rsize) = (code.5 as usize, code.4 as usize);
            let aes = Aes256::new(&h.key);
            let mut iv = h.iv;
            aes.decrypt_block(&mut iv);
            let mut buf = h.stolen.to_vec();
            buf.extend_from_slice(&d[raw..(raw + rsize).min(d.len())]);
            buf.truncate(buf.len() / 16 * 16);
            let mut prev = iv;
            for blk in buf.chunks_mut(16) {
                let c: [u8; 16] = blk.try_into().unwrap();
                let mut p = c;
                aes.decrypt_block(&mut p);
                for k in 0..16 {
                    blk[k] = p[k] ^ prev[k];
                }
                prev = c;
            }
            let n = buf.len().min(rsize);
            d[raw..raw + n].copy_from_slice(&buf[..n]);
        }
    }
    let mut oep = h.oep;
    if h.image_base != 0 && oep >= h.image_base {
        oep -= h.image_base;
    }
    d[pe + 40..pe + 44].copy_from_slice(&(oep as u32).to_le_bytes());
    (d, Some(h))
}

/// AES-256 decryption (FIPS-197), all the stub needs
pub struct Aes256 {
    rk: [[u8; 16]; 15],
    inv: [u8; 256],
}

fn xtime(x: u8) -> u8 {
    (x << 1) ^ if x & 0x80 != 0 { 0x1B } else { 0 }
}
fn gmul(mut a: u8, mut b: u8) -> u8 {
    let mut p = 0;
    while b != 0 {
        if b & 1 != 0 {
            p ^= a;
        }
        a = xtime(a);
        b >>= 1;
    }
    p
}
fn sbox() -> [u8; 256] {
    let mut s = [0u8; 256];
    for i in 0..256 {
        // multiplicative inverse in GF(2^8), then the affine transform
        let inv = if i == 0 { 0 } else { (1..=255u8).find(|&j| gmul(i as u8, j) == 1).unwrap() };
        let mut x = inv;
        let mut r = inv;
        for _ in 0..4 {
            x = x.rotate_left(1);
            r ^= x;
        }
        s[i] = r ^ 0x63;
    }
    s
}

impl Aes256 {
    pub fn new(key: &[u8; 32]) -> Self {
        let s = sbox();
        let mut inv = [0u8; 256];
        for (i, &v) in s.iter().enumerate() {
            inv[v as usize] = i as u8;
        }
        let mut w = [[0u8; 4]; 60];
        for i in 0..8 {
            w[i].copy_from_slice(&key[4 * i..4 * i + 4]);
        }
        let mut rcon = 1u8;
        for i in 8..60 {
            let mut t = w[i - 1];
            if i % 8 == 0 {
                t = [s[t[1] as usize] ^ rcon, s[t[2] as usize], s[t[3] as usize], s[t[0] as usize]];
                rcon = xtime(rcon);
            } else if i % 8 == 4 {
                t = t.map(|b| s[b as usize]);
            }
            for k in 0..4 {
                w[i][k] = w[i - 8][k] ^ t[k];
            }
        }
        let mut rk = [[0u8; 16]; 15];
        for r in 0..15 {
            for c in 0..4 {
                rk[r][4 * c..4 * c + 4].copy_from_slice(&w[4 * r + c]);
            }
        }
        Aes256 { rk, inv }
    }

    pub fn decrypt_block(&self, b: &mut [u8; 16]) {
        let add = |b: &mut [u8; 16], k: &[u8; 16]| (0..16).for_each(|i| b[i] ^= k[i]);
        add(b, &self.rk[14]);
        for r in (0..14).rev() {
            // inverse shift rows (column-major state), inverse sub bytes
            let t = *b;
            for c in 0..4 {
                for row in 0..4 {
                    b[4 * ((c + row) % 4) + row] = self.inv[t[4 * c + row] as usize];
                }
            }
            add(b, &self.rk[r]);
            if r > 0 {
                for c in 0..4 {
                    let a = [b[4 * c], b[4 * c + 1], b[4 * c + 2], b[4 * c + 3]];
                    for row in 0..4 {
                        b[4 * c + row] = gmul(a[row], 14) ^ gmul(a[(row + 1) % 4], 11) ^ gmul(a[(row + 2) % 4], 13) ^ gmul(a[(row + 3) % 4], 9);
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn fips197_aes256() {
        let key: [u8; 32] = core::array::from_fn(|i| i as u8);
        let mut c = [0x8e, 0xa2, 0xb7, 0xca, 0x51, 0x67, 0x45, 0xbf, 0xea, 0xfc, 0x49, 0x90, 0x4b, 0x49, 0x60, 0x89];
        super::Aes256::new(&key).decrypt_block(&mut c);
        assert_eq!(c, [0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff]);
    }
}
