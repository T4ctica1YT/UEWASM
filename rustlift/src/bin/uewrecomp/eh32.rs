//! 32-bit MSVC C++ exception handling, found statically so the lifted code can resume at a catch.
//! A function with try/catch registers a handler thunk (`mov eax, FuncInfo; jmp __CxxFrameHandler3`) in its prologue.
//! When a catch runs, its funclet returns the address to continue at (`mov eax, continuation; ret`) and the CRT jumps
//! there with the function's own esp and ebp. The lifter gives every such function a host setjmp on entry, keyed by its
//! frame pointer, and lifts each continuation as an entry point (rt_eh_slot32 in runtime/rt.c). SEH scopes
//! (_except_handler3/4) are found the same way. analyze() returns {function start: frame offset} and the continuations.
use std::collections::{BTreeMap, BTreeSet};

const MAGIC: [u32; 3] = [0x1993_0520, 0x1993_0521, 0x1993_0522];

struct Img<'a> {
    img: &'a [u8],
    base: u64,
    text: Vec<(u64, u64)>,
}
impl Img<'_> {
    fn rd(&self, a: u64) -> Option<u32> {
        let o = a.checked_sub(self.base)? as usize;
        self.img.get(o..o + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()))
    }
    fn in_text(&self, a: u64) -> bool {
        let r = a.wrapping_sub(self.base);
        self.text.iter().any(|&(s, e)| s <= r && r < e)
    }
}
fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}

/// img: the module image relocated to base; sections: (rva, size, executable)
pub fn analyze(img: &[u8], base: u64, sections: &[(u32, u32, bool)]) -> (BTreeMap<u64, u32>, BTreeSet<u64>) {
    let im = Img { img, base, text: sections.iter().filter(|s| s.2).map(|s| (s.0 as u64, s.0 as u64 + s.1 as u64)).collect() };
    let (mut thunks, mut conts) = (BTreeSet::new(), BTreeSet::new());
    for &(s, e) in &im.text {
        let blob = &img[(s as usize).min(img.len())..(e as usize).min(img.len())];
        // mov eax, imm32; jmp rel32
        for i in 0..blob.len().saturating_sub(5) {
            if blob[i] != 0xB8 || blob[i + 5] != 0xE9 {
                continue;
            }
            let fi = le32(blob, i + 1) as u64;
            if !im.rd(fi).map_or(false, |m| MAGIC.contains(&m)) {
                continue;
            }
            let (Some(ntry), Some(tmap)) = (im.rd(fi + 12), im.rd(fi + 16)) else { continue };
            if ntry == 0 || ntry > 4096 || im.rd(tmap as u64).is_none() {
                continue;
            }
            thunks.insert(base + s + i as u64);
            // TryBlockMapEntry { tryLow, tryHigh, catchHigh, nCatches, HandlerType* } (20 bytes);
            // HandlerType { adjectives, type, catch object, handler } (16 bytes)
            for t in 0..ntry as u64 {
                let (Some(ncatch), Some(handlers)) = (im.rd(tmap as u64 + 20 * t + 12), im.rd(tmap as u64 + 20 * t + 16)) else { continue };
                if ncatch == 0 || ncatch > 256 {
                    continue;
                }
                for k in 0..ncatch as u64 {
                    let Some(h) = im.rd(handlers as u64 + 16 * k + 12).map(|h| h as u64) else { continue };
                    if !im.in_text(h) {
                        continue;
                    }
                    // the funclet's returns: mov eax, continuation; ret
                    let o = (h - base) as usize;
                    let f = &img[o.min(img.len())..(o + 1024).min(img.len())];
                    let mut j = 0;
                    while j + 6 <= f.len() {
                        if f[j] == 0xB8 && f[j + 5] == 0xC3 {
                            let t = le32(f, j + 1) as u64;
                            if im.in_text(t) && t.abs_diff(h) < 0x10_0000 {
                                conts.insert(t);
                            }
                            j += 6;
                        } else {
                            j += 1;
                        }
                    }
                }
            }
        }
    }
    let mut starts = BTreeMap::new();
    if thunks.is_empty() {
        return (starts, conts);
    }
    // the thunk itself, or a /GS prelude before it (mov edx, [esp+8]; lea eax, [edx+0Ch]; cookie checks)
    let is_thunk = |t: u64| -> bool {
        if thunks.contains(&t) {
            return true;
        }
        let Some(o) = t.checked_sub(base).map(|o| o as usize) else { return false };
        if img.get(o..o + 4) != Some(&[0x8B, 0x54, 0x24, 0x08]) {
            return false;
        }
        thunks.range(t..).next().map_or(false, |&n| n - t <= 64)
    };
    for &(s, e) in &im.text {
        let blob = &img[(s as usize).min(img.len())..(e as usize).min(img.len())];
        for i in 0..blob.len() {
            if (blob[i] != 0x68 && blob[i] != 0xB8) || i + 5 > blob.len() || !is_thunk(le32(blob, i + 1) as u64) {
                continue;
            }
            let a = base + s + i as u64;
            if blob[i] == 0x68 {
                // push ebp; mov ebp, esp; push -1 (or -2); push handler
                if i >= 5 && blob[i - 5..i - 1] == [0x55, 0x8B, 0xEC, 0x6A] && (blob[i - 1] == 0xFF || blob[i - 1] == 0xFE) {
                    starts.insert(a - 5, 4);
                } else if i >= 2 && blob[i - 2..i] == [0x6A, 0xFF] {
                    // frame-pointer-less: push -1; push handler (the CRT then sets ebp to the entry esp)
                    starts.insert(a - 2, 0);
                }
            } else if i + 6 <= blob.len() && blob[i + 5] == 0xE8 {
                // mov eax, handler; call __EH_prolog (ebp = entry esp - 4), maybe after push N for __EH_prolog3
                starts.insert(a, 4);
                if i >= 2 && blob[i - 2] == 0x6A {
                    starts.insert(a - 2, 4);
                }
                if i >= 5 && blob[i - 5] == 0x68 {
                    starts.insert(a - 5, 4);
                }
            }
        }
    }
    seh(&im, &mut starts, &mut conts);
    (starts, conts)
}

/// SEH scope records { enclosing level, filter (0 for __finally), handler } from table on, while they look valid
fn scope_records(im: &Img, table: u64) -> Vec<(u64, u64)> {
    let mut recs = Vec::new();
    for i in 0..64u64 {
        let (Some(lvl), Some(flt), Some(hnd)) = (im.rd(table + 12 * i), im.rd(table + 12 * i + 4), im.rd(table + 12 * i + 8)) else { break };
        let (flt, hnd) = (flt as u64, hnd as u64);
        if !im.in_text(hnd) || (flt != 0 && !im.in_text(flt)) {
            break;
        }
        if lvl != 0xFFFF_FFFF && lvl != 0xFFFF_FFFE && lvl as u64 >= i {
            break;
        }
        recs.push((flt, hnd));
    }
    recs
}

/// __try/__except and __finally: the CRT calls their filters and finally blocks through the scope table and jumps into
/// the __except blocks with the function's frame. Frames are set up inline (push ebp; mov ebp, esp; push -1/-2; push
/// scope table; push handler) or through __SEH_prolog3/4 (push N; push scope table; call)
fn seh(im: &Img, starts: &mut BTreeMap<u64, u32>, conts: &mut BTreeSet<u64>) {
    let (img, base) = (im.img, im.base);
    let prolog = |t: u64| -> u32 {
        let Some(o) = t.checked_sub(base).map(|o| o as usize) else { return 0 };
        if img.get(o) != Some(&0x68) {
            return 0;
        }
        let tail = img.get(o + 5..(o + 12).min(img.len())).unwrap_or(&[]);
        if tail == [0x64, 0xFF, 0x35, 0, 0, 0, 0] {
            4
        } else if tail.len() >= 6 && tail[..6] == [0x64, 0xA1, 0, 0, 0, 0] {
            3
        } else {
            0
        }
    };
    let mut add = |start: u64, table: u64, seh4: bool| {
        let recs = scope_records(im, table + if seh4 { 16 } else { 0 });
        if recs.is_empty() {
            return;
        }
        starts.insert(start, 4);
        for (flt, hnd) in recs {
            conts.insert(hnd);
            if flt != 0 {
                conts.insert(flt);
            }
        }
    };
    for &(s, e) in &im.text {
        let blob = &img[(s as usize).min(img.len())..(e as usize).min(img.len())];
        for i in 0..blob.len() {
            if blob[i] != 0x68 || i + 10 > blob.len() {
                continue;
            }
            let a = base + s + i as u64;
            let table = le32(blob, i + 1) as u64;
            if i >= 5 && blob[i - 5..i - 1] == [0x55, 0x8B, 0xEC, 0x6A] && (blob[i - 1] == 0xFF || blob[i - 1] == 0xFE) && blob[i + 5] == 0x68 {
                add(a - 5, table, blob[i - 1] == 0xFE);
            } else if blob[i + 5] == 0xE8 {
                let rel = i32::from_le_bytes(blob[i + 6..i + 10].try_into().unwrap()) as i64;
                let kind = prolog((a as i64 + 10 + rel) as u64);
                if kind == 0 {
                    continue;
                }
                if i >= 2 && blob[i - 2] == 0x6A {
                    add(a - 2, table, kind == 4);
                }
                if i >= 5 && blob[i - 5] == 0x68 {
                    add(a - 5, table, kind == 4);
                }
            }
        }
    }
}
