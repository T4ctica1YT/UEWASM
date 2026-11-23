//! Shader tools for testing the translators: pulling DXBC and D3D9 shaders out of a game install, deriving DXBC opcode
//! numbers from fxc's disassembly, compiling translated WGSL/GLSL in the browser; and the sse.h test generator.
use crate::cdp::{self, Browser};
use crate::*;
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::path::Path;
use std::time::Duration;

fn u32at(d: &[u8], i: usize) -> u32 {
    u32::from_le_bytes(d[i..i + 4].try_into().unwrap())
}

fn find(h: &[u8], n: &[u8], from: usize) -> Option<usize> {
    if from >= h.len() {
        return None;
    }
    memchr_find(&h[from..], n).map(|i| i + from)
}

fn memchr_find(h: &[u8], n: &[u8]) -> Option<usize> {
    h.windows(n.len()).position(|w| w == n)
}

/// a zlib stream from the start of d: what inflates (a stream cut short keeps what came out), None on bad data
fn inflate_prefix(d: &[u8]) -> Option<Vec<u8>> {
    let mut z = flate2::Decompress::new(true);
    let mut out = Vec::with_capacity(1 << 16);
    loop {
        if out.len() == out.capacity() {
            out.reserve(out.capacity());
        }
        let before = (z.total_in(), z.total_out());
        match z.decompress_vec(&d[z.total_in() as usize..], &mut out, flate2::FlushDecompress::None) {
            Ok(flate2::Status::StreamEnd) => return Some(out),
            Ok(_) => {
                if (z.total_in(), z.total_out()) == before && out.len() < out.capacity() {
                    return Some(out);
                }
                if z.total_in() as usize >= d.len() && out.len() < out.capacity() {
                    return Some(out);
                }
            }
            Err(_) => return None,
        }
    }
}

fn walk_files(p: &Path) -> Vec<std::path::PathBuf> {
    util::files_under(p)
}

/// every DXBC shader blob in a game install (shader caches and cooked assets)
pub fn extract_dxbc(a: &[String]) -> bool {
    let (src, out) = match a { [s, o, ..] => (Path::new(s), Path::new(o)), _ => die("usage: uewtest extract-dxbc <game> <out>") };
    std::fs::create_dir_all(out).unwrap();
    let (mut seen, mut kinds) = (HashSet::new(), BTreeMap::<String, u32>::new());
    for f in walk_files(src) {
        let ext = f.extension().map(|e| e.to_string_lossy().to_lowercase()).unwrap_or_default();
        if !["bin", "uasset", "uexp", "ushaderbytecode", "pak", "ubulk"].contains(&ext.as_str()) {
            continue;
        }
        let Ok(raw) = std::fs::read(&f) else { continue };
        let mut data = raw.clone();
        let mut i = find(&raw, b"\x78\x9c", 0);
        while let Some(p) = i {
            if let Some(d) = inflate_prefix(&raw[p..(p + (8 << 20)).min(raw.len())]) {
                data.extend(d);
            }
            i = find(&raw, b"\x78\x9c", p + 2);
        }
        let mut p = find(&data, b"DXBC", 0);
        while let Some(at) = p {
            if at + 32 <= data.len() {
                let size = u32at(&data, at + 24) as usize;
                if 32 < size && size < 4 << 20 && at + size <= data.len() {
                    let blob = &data[at..at + size];
                    let h = util::hex(&util::sha1(blob))[..16].to_string();
                    if seen.insert(h.clone()) {
                        let shex = memchr_find(blob, b"SHEX").or_else(|| memchr_find(blob, b"SHDR"));
                        let stage = shex.filter(|&s| s + 12 <= blob.len()).map_or(9, |s| u32at(blob, s + 8) >> 16);
                        let name = match stage { 0 => "ps", 1 => "vs", 2 => "gs", 3 => "hs", 4 => "ds", 5 => "cs", _ => "xx" };
                        *kinds.entry(name.into()).or_default() += 1;
                        std::fs::write(out.join(format!("{name}_{h}.dxbc")), blob).unwrap();
                    }
                }
            }
            p = find(&data, b"DXBC", at + 4);
        }
    }
    println!("{} unique shaders: {kinds:?}", seen.len());
    true
}

/// the end of a well-formed D3D9 shader starting at off (its version token), else None
fn d3d9_end(d: &[u8], off: usize) -> Option<usize> {
    let n = d.len();
    let mut i = off + 4;
    let major = (u32at(d, off) >> 8) & 0xFF;
    let mut count = 0;
    while i + 4 <= n && count < 200000 {
        let tok = u32at(d, i);
        let op = tok & 0xFFFF;
        if op == 0xFFFF {
            return (tok == 0x0000FFFF).then_some(i + 4);
        }
        if op == 0xFFFE {
            i += 4 + 4 * ((tok >> 16) & 0x7FFF) as usize;
            continue;
        }
        if tok & 0x8000_0000 != 0 || (op > 96 && op != 0xFFFD) {
            return None;
        }
        i += 4 + 4 * (if major >= 2 { (tok >> 24) & 0xF } else { 0 }) as usize;
        count += 1;
    }
    None
}

fn d3d9_blobs(d: &[u8]) -> Vec<&[u8]> {
    // UE3 serializes shader code inside packages at any byte offset, so search for the version tokens themselves
    let mut vs: Vec<u32> = [0xFFFE0000u32, 0xFFFF0000].iter().flat_map(|v| [0x0200, 0x0201, 0x0300].map(|k| v | k)).collect();
    vs.sort();
    let mut out = vec![];
    for v in vs {
        let pat = v.to_le_bytes();
        let mut off = find(d, &pat, 0);
        while let Some(o) = off {
            if let Some(end) = d3d9_end(d, o) {
                if end - o >= 16 {
                    out.push(&d[o..end]);
                }
            }
            off = find(d, &pat, o + 4);
        }
    }
    out
}

/// LZO1X decompression (UE3's default package compression on PC); None on bad data
fn lzo1x(src: &[u8], size: usize) -> Option<Vec<u8>> {
    let mut out: Vec<u8> = vec![];
    let (mut ip, n) = (0usize, src.len());
    let g = |i: usize| src.get(i).copied();
    fn copy_match(out: &mut Vec<u8>, dist: usize, len: usize) -> Option<()> {
        let start = out.len().checked_sub(dist)?;
        for k in 0..len {
            let b = out[start + k];
            out.push(b);
        }
        Some(())
    }
    let lit = |out: &mut Vec<u8>, ip: usize, t: usize| -> Option<()> {
        out.extend_from_slice(src.get(ip..ip + t)?);
        Some(())
    };
    let mut state = 0usize;
    if g(0)? > 17 {
        let t = g(0)? as usize - 17;
        ip = 1;
        lit(&mut out, ip, t)?;
        ip += t;
        state = if t >= 4 { 4 } else { t };
    }
    let long = |ip: &mut usize, mut v: usize, add: usize| -> Option<usize> {
        while g(*ip)? == 0 {
            v += 255;
            *ip += 1;
        }
        v += add + g(*ip)? as usize;
        *ip += 1;
        Some(v)
    };
    while ip < n {
        let t = g(ip)? as usize;
        ip += 1;
        if t < 16 {
            if state == 0 {
                let mut t = t;
                if t == 0 {
                    t = long(&mut ip, 0, 15)?;
                }
                t += 3;
                lit(&mut out, ip, t)?;
                ip += t;
                state = 4;
                continue;
            }
            let b = g(ip)? as usize;
            ip += 1;
            if state == 4 {
                copy_match(&mut out, 1 + 0x0800 + (t >> 2) + (b << 2), 3)?;
            } else {
                copy_match(&mut out, 1 + (t >> 2) + (b << 2), 2)?;
            }
        } else if t >= 64 {
            let b = g(ip)? as usize;
            ip += 1;
            copy_match(&mut out, 1 + ((t >> 2) & 7) + (b << 3), (t >> 5) + 1)?;
        } else if t >= 32 {
            let mut len = t & 31;
            if len == 0 {
                len = long(&mut ip, 0, 31)?;
            }
            let v = g(ip)? as usize | (g(ip + 1)? as usize) << 8;
            ip += 2;
            copy_match(&mut out, 1 + (v >> 2), len + 2)?;
        } else {
            let mut dist = (t & 8) << 11;
            let mut len = t & 7;
            if len == 0 {
                len = long(&mut ip, 0, 7)?;
            }
            let v = g(ip)? as usize | (g(ip + 1)? as usize) << 8;
            ip += 2;
            dist += v >> 2;
            if dist == 0 {
                break;
            }
            copy_match(&mut out, dist + 0x4000, len + 2)?;
        }
        state = (g(ip - 2)? & 3) as usize;
        if state > 0 {
            lit(&mut out, ip, state)?;
            ip += state;
        }
    }
    out.truncate(size);
    Some(out)
}

fn inflate_or_lzo(buf: &[u8], usize_: usize) -> Option<Vec<u8>> {
    let mut out = vec![];
    if std::io::Read::read_to_end(&mut flate2::read::ZlibDecoder::new(buf), &mut out).is_ok() {
        return Some(out);
    }
    lzo1x(buf, usize_)
}

/// UE3 packages store compressed chunks; every chunk that inflates, else the raw data
fn unpack_ue3(d: &[u8]) -> Vec<Vec<u8>> {
    let mut out = vec![];
    let mut pos = 0;
    while let Some(j) = find(d, b"\xc1\x83\x2a\x9e", pos) {
        pos = j + 4;
        let r = (|| -> Option<Vec<u8>> {
            if j + 16 > d.len() {
                return None;
            }
            let (block, csize, usize_) = (u32at(d, j + 4) as usize, u32at(d, j + 8) as usize, u32at(d, j + 12) as usize);
            let nb = if block > 0 { (usize_ + block - 1) / block } else { 0 };
            if !(0 < nb && nb < 4096 && 0 < csize && csize < d.len()) {
                return None;
            }
            let mut sizes = vec![];
            for b in 0..nb {
                let at = j + 16 + 8 * b;
                if at + 8 > d.len() {
                    return None;
                }
                sizes.push((u32at(d, at) as usize, u32at(d, at + 4) as usize));
            }
            let mut p = j + 16 + 8 * nb;
            let mut buf = vec![];
            for (cs, us) in sizes {
                buf.extend(inflate_or_lzo(d.get(p..p + cs)?, us)?);
                p += cs;
            }
            Some(buf)
        })();
        if let Some(b) = r {
            out.push(b);
        }
    }
    if out.is_empty() { vec![d.to_vec()] } else { out }
}

/// the Direct3D 9 shader blobs (vs/ps 2.0-3.0) from a UE3 game's shader caches, one .dxbc file each (the name is
/// historical: wgsl_dump and wgsl-check take D3D9 bytecode too)
pub fn extract_d3d9(a: &[String]) -> bool {
    let (src, dst) = match a { [s, o, ..] => (Path::new(s), Path::new(o)), _ => die("usage: uewtest extract-d3d9 <game> <out>") };
    std::fs::create_dir_all(dst).unwrap();
    let (mut seen, mut kinds) = (HashSet::new(), BTreeMap::<String, u32>::new());
    let files: Vec<_> = walk_files(src).into_iter().filter(|p| p.file_name().map_or(false, |n| n.to_string_lossy().contains("ShaderCache"))).collect();
    for f in &files {
        let Ok(data) = std::fs::read(f) else { continue };
        let mut parts = vec![data.clone()];
        if f.extension().map_or(true, |e| e != "bin") {
            parts.extend(unpack_ue3(&data));
        }
        for part in &parts {
            for b in d3d9_blobs(part) {
                let h = util::hex(&util::sha1(b))[..16].to_string();
                if !seen.insert(h.clone()) {
                    continue;
                }
                let v = u32at(b, 0);
                let kind = format!("{}_{}_{}", if v >> 16 == 0xFFFF { "ps" } else { "vs" }, (v >> 8) & 0xFF, v & 0xFF);
                *kinds.entry(kind.clone()).or_default() += 1;
                std::fs::write(dst.join(format!("{kind}_{h}.dxbc")), b).unwrap();
            }
        }
    }
    println!("{} shaders from {} files: {}", seen.len(), files.len(), kinds.iter().map(|(k, n)| format!("{k} {n}")).collect::<Vec<_>>().join(", "));
    true
}

/// DXBC opcode numbers, by aligning the token stream with fxc's disassembly
pub fn opcode_map(a: &[String]) -> bool {
    let dir = Path::new(a.first().map(String::as_str).unwrap_or_else(|| die("usage: uewtest opcode-map <dir> [n]")));
    let limit = a.get(1).and_then(|x| x.parse().ok()).unwrap_or(400usize);
    let kits = Path::new(r"C:\Program Files (x86)\Windows Kits\10\bin");
    let mut fxcs: Vec<_> = std::fs::read_dir(kits).map(|r| r.flatten().map(|e| e.path().join("x64/fxc.exe")).filter(|p| p.exists()).collect()).unwrap_or_default();
    fxcs.sort();
    let fxc = fxcs.pop().unwrap_or_else(|| die("fxc.exe not found (Windows SDK)"));
    let mut files: Vec<_> = std::fs::read_dir(dir).unwrap().flatten().map(|e| e.path()).filter(|p| p.extension().map_or(false, |e| e == "dxbc")).collect();
    files.sort();
    files.truncate(limit);
    let mut votes: BTreeMap<u32, HashMap<String, u32>> = BTreeMap::new();
    let num = regex::Regex::new(r"^[0-9a-fx.\-+, ]+[,}]?$").unwrap();
    let ver = regex::Regex::new(r"^(ps|vs|gs|hs|ds|cs)_\d").unwrap();
    for f in files {
        let blob = std::fs::read(&f).unwrap();
        let Some(p) = memchr_find(&blob, b"SHEX").or_else(|| memchr_find(&blob, b"SHDR")) else { continue };
        let count = u32at(&blob, p + 4) as usize / 4;
        let toks: Vec<u32> = (0..count).filter(|i| p + 8 + 4 * i + 4 <= blob.len()).map(|i| u32at(&blob, p + 8 + 4 * i)).collect();
        let mut ops = vec![];
        let mut i = 2;
        while i < (toks.get(1).copied().unwrap_or(0) as usize).min(toks.len()) {
            let t = toks[i];
            let op = t & 0x7FF;
            let n = if op == 53 { toks.get(i + 1).copied().unwrap_or(0) as usize } else { ((t >> 24) & 0x7F) as usize };
            if op != 53 {
                ops.push(op);
            }
            if n == 0 {
                break;
            }
            i += n;
        }
        let asm = run(std::process::Command::new(&fxc).args(["/nologo", "/dumpbin"]).arg(&f), Duration::from_secs(120)).stdout;
        let mut names = vec![];
        for line in asm.lines() {
            let s = line.trim();
            if s.is_empty() || s.starts_with("//") || ver.is_match(s) || s.starts_with('{') || s.starts_with('}') || s.starts_with("dcl_immediateConstantBuffer") || num.is_match(s) {
                continue;
            }
            let w = s.split_whitespace().next().unwrap_or("");
            names.push(if !s.starts_with("dcl_") { w.split('_').next().unwrap_or("").to_string() } else { w.split('_').take(2).collect::<Vec<_>>().join("_") });
        }
        if names.len() != ops.len() {
            continue;
        }
        for (o, n) in ops.iter().zip(names) {
            *votes.entry(*o).or_default().entry(n).or_default() += 1;
        }
    }
    for (o, v) in votes {
        let mut c: Vec<(String, u32)> = v.into_iter().collect();
        c.sort_by(|a, b| b.1.cmp(&a.1).then(a.0.cmp(&b.0)));
        c.truncate(2);
        println!("{o} {:?}", c.iter().map(|(n, k)| (n.as_str(), *k)).collect::<Vec<_>>());
    }
    true
}

fn shader_files(dir: &Path, ext: &str, limit: usize) -> Vec<std::path::PathBuf> {
    let mut v: Vec<_> = std::fs::read_dir(dir).unwrap_or_else(|e| die(format!("{}: {e}", dir.display()))).flatten().map(|e| e.path())
        .filter(|p| p.extension().map_or(false, |e| e == ext)).collect();
    v.sort();
    if limit > 0 {
        v.truncate(limit);
    }
    v
}

/// groups compile errors by their message with numbers and quoted names taken out
fn group(bad: &[(String, String)], first: impl Fn(&str) -> String, key: impl Fn(&str) -> String, top: usize) -> Vec<(usize, String, String, String)> {
    let mut kinds: HashMap<String, (usize, String, String)> = HashMap::new();
    let mut order = vec![];
    for (n, log) in bad {
        let f = first(log);
        let k = key(&f);
        let e = kinds.entry(k.clone()).or_insert_with(|| {
            order.push(k.clone());
            (0, n.clone(), f.clone())
        });
        e.0 += 1;
    }
    let mut v: Vec<(usize, String, String, String)> = order.into_iter().map(|k| {
        let (c, n, f) = kinds[&k].clone();
        (c, k, n, f)
    }).collect();
    v.sort_by(|a, b| b.0.cmp(&a.0));
    v.truncate(top);
    v
}

fn page_for(root: &Path, html: &str, flags: &[&str]) -> (cdp::Server, Browser) {
    std::fs::write(root.join("check.html"), html).unwrap();
    let srv = cdp::Server::start(&uewserve(), root, &[]).unwrap_or_else(|e| die(e));
    let b = Browser::launch(&flags.iter().map(|s| s.to_string()).collect::<Vec<_>>()).unwrap_or_else(|e| die(e));
    (srv, b)
}

/// translated WGSL compiled by the browser's WebGPU compiler (headless), errors grouped
pub fn wgsl_check(a: &[String]) -> bool {
    let root = Path::new(a.first().map(String::as_str).unwrap_or_else(|| die("usage: uewtest wgsl-check <dir> [n]")));
    let files = shader_files(root, "wgsl", a.get(1).and_then(|x| x.parse().ok()).unwrap_or(0));
    let (srv, mut b) = page_for(root, r#"<!doctype html><script>
window.dev = (async () => { const a = await navigator.gpu.requestAdapter(); return a.requestDevice({ requiredLimits: { maxSampledTexturesPerShaderStage: a.limits.maxSampledTexturesPerShaderStage, maxStorageBuffersPerShaderStage: a.limits.maxStorageBuffersPerShaderStage } }); })();
window.run = async (items) => {
  const dev = await window.dev; const bad = [];
  for (const [n, code] of items) {
    const m = dev.createShaderModule({ code });
    const info = await m.getCompilationInfo();
    const errs = info.messages.filter(x => x.type === 'error');
    if (errs.length) bad.push([n, errs.map(e => `${e.lineNum}:${e.linePos} ${e.message}`).join('\n')]);
  }
  return bad;
};
</script>"#, &["--mute-audio", "--enable-unsafe-webgpu", "--ignore-gpu-blocklist"]);
    let mut bad: Vec<(String, String)> = vec![];
    for chunk in files.chunks(400) {
        let mut page = b.new_page(800, 600).unwrap_or_else(|e| die(e));
        page.goto(&mut b, &format!("http://localhost:{}/check.html", srv.port), Duration::from_secs(60)).unwrap_or_else(|e| die(e));
        let items: Vec<Value> = chunk.iter().map(|f| json!([f.file_name().unwrap().to_string_lossy(), String::from_utf8_lossy(&std::fs::read(f).unwrap())])).collect();
        let r = page.eval(&mut b, &format!("window.run({})", Value::Array(items))).unwrap_or_else(|e| die(e));
        for x in r.as_array().cloned().unwrap_or_default() {
            bad.push((x[0].as_str().unwrap_or("").into(), x[1].as_str().unwrap_or("").into()));
        }
    }
    b.close();
    drop(srv);
    println!("compiled {} / {}", files.len() - bad.len(), files.len());
    let num = regex::Regex::new(r"\d+").unwrap();
    let quoted = regex::Regex::new(r"'[^']*'").unwrap();
    let rows = group(&bad, |log| log.lines().next().unwrap_or("?").to_string(),
                     |f| quoted.replace_all(&num.replace_all(f.split_once(' ').map_or(f, |x| x.1), "N"), "'X'").chars().take(120).collect(), 20);
    for (c, k, n, f) in rows {
        println!("{c:6}  {k}\n          e.g. {n}: {}", f.chars().take(200).collect::<String>());
    }
    let out: Vec<Value> = bad.iter().take(3000).map(|(n, l)| json!([n, l])).collect();
    std::fs::write(root.join("wgsl_errors.json"), serde_json::to_string_pretty(&out).unwrap()).unwrap();
    true
}

/// translated GLSL compiled by the browser's GLSL ES compiler (headless), errors grouped
pub fn webgl_check(a: &[String]) -> bool {
    let root = Path::new(a.first().map(String::as_str).unwrap_or_else(|| die("usage: uewtest webgl-check <dir> [n]")));
    let files = shader_files(root, "glsl", a.get(1).and_then(|x| x.parse().ok()).unwrap_or(0));
    let mut bundles = vec![];
    for (i, chunk) in files.chunks(500).enumerate() {
        let name = format!("bundle_{i:03}.json");
        let items: Vec<Value> = chunk.iter().map(|f| json!([f.file_name().unwrap().to_string_lossy(), String::from_utf8_lossy(&std::fs::read(f).unwrap())])).collect();
        std::fs::write(root.join(&name), Value::Array(items).to_string()).unwrap();
        bundles.push(name);
    }
    let (srv, mut b) = page_for(root, r#"<!doctype html><script>
window.run = async (bundles) => {
  const gl = new OffscreenCanvas(4, 4).getContext('webgl2');
  const bad = []; let total = 0;
  for (const b of bundles) {
    for (const [n, src] of await (await fetch(b)).json()) {
      total++;
      const s = gl.createShader(n.startsWith('vs') ? gl.VERTEX_SHADER : gl.FRAGMENT_SHADER);
      gl.shaderSource(s, src); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) bad.push([n, gl.getShaderInfoLog(s)]);
      gl.deleteShader(s);
    }
  }
  return { total, bad };
};
</script>"#, &["--mute-audio", "--ignore-gpu-blocklist"]);
    let mut page = b.new_page(800, 600).unwrap_or_else(|e| die(e));
    page.goto(&mut b, &format!("http://localhost:{}/check.html", srv.port), Duration::from_secs(60)).unwrap_or_else(|e| die(e));
    let res = page.eval(&mut b, &format!("window.run({})", json!(bundles))).unwrap_or_else(|e| die(e));
    b.close();
    drop(srv);
    for n in &bundles {
        let _ = std::fs::remove_file(root.join(n));
    }
    let bad: Vec<(String, String)> = res["bad"].as_array().cloned().unwrap_or_default().iter()
        .map(|x| (x[0].as_str().unwrap_or("").to_string(), x[1].as_str().unwrap_or("").to_string())).collect();
    let total = res["total"].as_u64().unwrap_or(0) as usize;
    println!("compiled {} / {total}", total - bad.len());
    let num = regex::Regex::new(r"\d+").unwrap();
    let quoted = regex::Regex::new(r"'[^']*'").unwrap();
    let rows = group(&bad, |log| log.lines().find(|l| l.contains("ERROR")).or(log.lines().next()).unwrap_or("?").to_string(),
                     |f| quoted.replace_all(&num.replace_all(f.split_once("ERROR:").map_or(f, |x| x.1).trim(), "N"), "'X'").to_string(), 25);
    for (c, k, n, f) in rows {
        println!("{c:6}  {k}   e.g. {n}: {}", f.chars().take(160).collect::<String>());
    }
    let out: Vec<Value> = bad.iter().take(2000).map(|(n, l)| json!([n, l])).collect();
    std::fs::write(root.join("webgl_errors.json"), serde_json::to_string_pretty(&out).unwrap()).unwrap();
    true
}

/// tests/sse/sse_test.c: every SIMD op in sse.h against its scalar reference
pub fn sse_test_c(s: &str) -> String {
    let caps = |re: &str| -> Vec<String> { regex::Regex::new(re).unwrap().captures_iter(s).map(|c| c[1].to_string()).collect() };
    let two = caps(r"XS\((\w+)\)\(XMM\* d, XMM s\)");
    let imm = caps(r"XS\((\w+)\)\(XMM\* d, XMM s, int \w+\)");
    let ret = caps(r"XSR\(uint64_t, (\w+)\)\(XMM s\)");
    let bv = caps(r"XSR\(void, (\w+)\)\(XMM\* d, XMM s, XMM m\)");
    // 0 integer lanes, 1 float lanes, 2 double lanes
    let mode = |n: &str| -> u32 {
        if ["AND", "OR", "XOR", "BLEND", "UNPCK", "SHUFP", "MOVS"].iter().any(|p| n.starts_with(p)) || n == "MOVDDUP" {
            0
        } else if n.ends_with("PS") || n == "CVTDQ2PS" {
            1
        } else if n.ends_with("PD") {
            2
        } else {
            0
        }
    };
    let mut l: Vec<String> = [
        "/* SIMD ops in runtime/sse.h against the scalar reference (generated by uewtest sse) */",
        "#include <stdint.h>", "#include <stdio.h>", "#include <string.h>", "#include <math.h>",
        "typedef union { uint8_t b[16]; int8_t sb[16]; uint16_t w[8]; int16_t sw[8]; uint32_t d[4]; int32_t sd[4]; uint64_t q[2]; int64_t sq[2]; float f[4]; double g[2]; } XMM;",
        "static inline uint64_t CVTT32(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? 0x80000000ull : (uint64_t)(uint32_t)(int32_t)v; }",
        "static inline uint64_t CVTR32(double v) { return CVTT32(nearbyint(v)); }",
        "#include \"../../runtime/sse.h\"",
        "static uint64_t st = 88172645463325252ull;",
        "static uint64_t rnd64(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }",
        "static const uint32_t SPF[] = { 0x7FC00000, 0xFFC00000, 0x7F800000, 0xFF800000, 0x80000000, 0, 0x3F800000, 0xBF800000, 0x4F000000, 0xCF000000, 0x00000001, 0x7F7FFFFF };",
        "static const uint64_t SPD[] = { 0x7FF8000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x8000000000000000ull, 0, 0x3FF0000000000000ull, 0x41E0000000000000ull };",
        "static void fill(XMM* x, int mode)",
        "{",
        "    for (int i = 0; i < 2; i++) x->q[i] = rnd64();",
        "    int k = (int)(rnd64() % 8);",
        "    if (mode == 1 && k < 3) for (int i = 0; i < 4; i++) x->d[i] = SPF[rnd64() % 12];",
        "    else if (mode == 1 && k < 6) for (int i = 0; i < 4; i++) x->f[i] = (float)((int)(rnd64() % 2001) - 1000) / 8.0f;",
        "    else if (mode == 2 && k < 3) for (int i = 0; i < 2; i++) x->q[i] = SPD[rnd64() % 7];",
        "    else if (mode == 2 && k < 6) for (int i = 0; i < 2; i++) x->g[i] = (double)((int)(rnd64() % 2001) - 1000) / 8.0;",
        "    else if (k == 5) for (int i = 0; i < 8; i++) x->w[i] = (uint16_t)((rnd64() & 1) ? 0x8000 : (rnd64() & 1) ? 0x7FFF : 0xFFFF);",
        "    else if (k == 6) for (int i = 0; i < 16; i++) x->b[i] = (uint8_t)((rnd64() & 1) ? 0x80 : (rnd64() & 1) ? 0x7F : 0xFF);",
        "    else if (k == 7) { x->q[0] = rnd64() % 80; x->q[1] = rnd64(); }",
        "}",
        "/* equal bits, or NaN in both at the same float lanes */",
        "static int same(const XMM* a, const XMM* b, int mode)",
        "{",
        "    if (!memcmp(a, b, 16)) return 1;",
        "    if (mode == 1) { for (int i = 0; i < 4; i++) if (a->d[i] != b->d[i] && !(isnan(a->f[i]) && isnan(b->f[i]))) return 0; return 1; }",
        "    if (mode == 2) { for (int i = 0; i < 2; i++) if (a->q[i] != b->q[i] && !(isnan(a->g[i]) && isnan(b->g[i]))) return 0; return 1; }",
        "    return 0;",
        "}",
        "static int bad;",
        "static void report(const char* n, int imm, const XMM* d0, const XMM* s0, const XMM* r, const XMM* e)",
        "{",
        "    if (bad++ > 40) return;",
        "    printf(\"MISMATCH %s imm=%d\\n d=%016llx%016llx s=%016llx%016llx\\n got=%016llx%016llx\\n ref=%016llx%016llx\\n\", n, imm,",
        "           (unsigned long long)d0->q[1], (unsigned long long)d0->q[0], (unsigned long long)s0->q[1], (unsigned long long)s0->q[0],",
        "           (unsigned long long)r->q[1], (unsigned long long)r->q[0], (unsigned long long)e->q[1], (unsigned long long)e->q[0]);",
        "}",
        "#define T2(N, M) { for (int it = 0; it < 200000; it++) { XMM d, s, r, e; fill(&d, M); fill(&s, M); r = d; e = d; x_##N(&r, s); xs_##N(&e, s); if (!same(&r, &e, M)) report(#N, 0, &d, &s, &r, &e); } }",
        "#define TI(N, M, IM) { for (int imm = 0; imm < (IM); imm++) for (int it = 0; it < 3000; it++) { XMM d, s, r, e; fill(&d, M); fill(&s, M); r = d; e = d; x_##N(&r, s, imm); xs_##N(&e, s, imm); if (!same(&r, &e, M)) report(#N, imm, &d, &s, &r, &e); } }",
        "#define TR(N) { for (int it = 0; it < 200000; it++) { XMM s; fill(&s, (int)(it % 3)); if (x_##N(s) != xs_##N(s)) { XMM z = { 0 }; report(#N, 0, &z, &s, &z, &z); } } }",
        "#define TB(N) { for (int it = 0; it < 200000; it++) { XMM d, s, m, r, e; fill(&d, 0); fill(&s, 0); fill(&m, 0); r = d; e = d; x_##N(&r, s, m); xs_##N(&e, s, m); if (!same(&r, &e, 0)) report(#N, 0, &d, &s, &r, &e); } }",
        "int main(void)", "{"].iter().map(|x| x.to_string()).collect();
    l.extend(two.iter().map(|n| format!("    T2({n}, {})", mode(n))));
    l.extend(imm.iter().map(|n| format!("    TI({n}, {}, {})", mode(n),
        if n.starts_with("CMP") || n.starts_with("ROUND") { 32 } else if n == "BLENDPD" { 4 } else if n == "BLENDPS" { 16 } else { 256 })));
    l.extend(ret.iter().map(|n| format!("    TR({n})")));
    l.extend(bv.iter().map(|n| format!("    TB({n})")));
    l.push(format!("    printf(\"%d ops checked, %d mismatches\\n\", {}, bad);", two.len() + imm.len() + ret.len() + bv.len()));
    l.extend(["    return bad != 0;".to_string(), "}".into(), String::new()]);
    l.join("\n")
}
