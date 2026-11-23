//! Lays out game files for the web runtime: data/index.txt plus each file cut into parts small enough for static hosts
//! (Cloudflare Pages allows 25 MiB per file). Executables and debug files are listed but not uploaded.
//! With --compress every 1 MiB block is deflated on its own so reads stay random access; audio and anything that
//! doesn't shrink is stored as is. Files unchanged since the last run (web.files beside the folder) are left alone.
//!   uewpack <game root> <web dir> [--part-mb N] [--compress] [--workers N] [--extra VIRT=PATH]... [--replace REL=PATH]...
use flate2::write::DeflateEncoder;
use flate2::Compression;
use rayon::prelude::*;
use regex::Regex;
use std::collections::{HashMap, HashSet};
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::UNIX_EPOCH;

const BLOCK: usize = 1 << 20;

struct Entry {
    virt: String,
    src: PathBuf,
    stub: bool,
}

fn die(msg: &str) -> ! {
    eprintln!("uewpack: {msg}");
    std::process::exit(2)
}

/// open for writing without writing through a hard link into the game install
fn create(p: &Path) -> File {
    let _ = fs::remove_file(p);
    File::create_new(p).unwrap_or_else(|e| die(&format!("can't write {}: {e}", p.display())))
}

/// copy a slice of src to dst, or hard link the whole file when that is the slice; bytes written
fn place(src: &Path, dst: &Path, offset: u64, size: u64, whole: u64) -> u64 {
    let _ = fs::remove_file(dst);
    if offset == 0 && size == whole {
        if fs::hard_link(src, dst).is_ok() {
            return 0;
        }
        fs::copy(src, dst).unwrap_or_else(|e| die(&format!("can't copy {}: {e}", src.display())));
        return size;
    }
    let mut f = File::open(src).unwrap_or_else(|e| die(&format!("can't read {}: {e}", src.display())));
    f.seek(SeekFrom::Start(offset)).unwrap();
    let mut o = create(dst);
    std::io::copy(&mut f.take(size), &mut o).unwrap_or_else(|e| die(&format!("can't write {}: {e}", dst.display())));
    size
}

fn part_name(i: usize, k: usize) -> String {
    if k == 0 { i.to_string() } else { format!("{i}.{k}") }
}

/// (part names, block sizes for the index, bytes written)
fn raw(i: usize, src: &Path, data: &Path, part: u64) -> (Vec<String>, String, u64) {
    let size = fs::metadata(src).map(|m| m.len()).unwrap_or(0);
    let n = ((size + part - 1) / part).max(1);
    let mut names = Vec::new();
    let mut written = 0;
    for k in 0..n as usize {
        let name = part_name(i, k);
        let off = k as u64 * part;
        written += place(src, &data.join(&name), off, part.min(size - off.min(size)), size);
        names.push(name);
    }
    (names, String::new(), written)
}

fn deflated(i: usize, src: &Path, data: &Path, part: u64) -> (Vec<String>, String, u64) {
    let mut f = File::open(src).unwrap_or_else(|e| die(&format!("can't read {}: {e}", src.display())));
    let mut names = vec![part_name(i, 0)];
    let mut out = create(&data.join(&names[0]));
    let (mut used, mut written) = (0u64, 0u64);
    let mut sizes = Vec::new();
    let mut raw = vec![0u8; BLOCK];
    loop {
        let mut n = 0;
        while n < BLOCK {
            let r = f.read(&mut raw[n..]).unwrap_or(0);
            if r == 0 {
                break;
            }
            n += r;
        }
        if n == 0 {
            break;
        }
        let mut z = DeflateEncoder::new(Vec::with_capacity(n), Compression::new(6));
        z.write_all(&raw[..n]).unwrap();
        let c = z.finish().unwrap();
        let blk: &[u8] = if (c.len() as f64) < n as f64 * 0.95 { &c } else { &raw[..n] };
        if used > 0 && used + blk.len() as u64 > part {
            names.push(part_name(i, names.len()));
            out = create(&data.join(names.last().unwrap()));
            used = 0;
        }
        out.write_all(blk).unwrap_or_else(|e| die(&format!("can't write: {e}")));
        used += blk.len() as u64;
        written += blk.len() as u64;
        sizes.push(format!("{:x}", blk.len()));
        if n < BLOCK {
            break;
        }
    }
    (names, sizes.join(","), written)
}

fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        match e.file_type() {
            Ok(t) if t.is_dir() => walk(&p, out),
            Ok(_) => out.push(p),
            Err(_) => {}
        }
    }
}

fn main() {
    let mut args = std::env::args().skip(1);
    let (mut pos, mut extra, mut replace) = (Vec::new(), Vec::new(), HashMap::new());
    let (mut part_mb, mut compress, mut workers) = (16u64, false, 0usize);
    while let Some(a) = args.next() {
        let mut val = || args.next().unwrap_or_else(|| die("an option needs a value"));
        match a.as_str() {
            "--part-mb" => part_mb = val().parse().unwrap_or_else(|_| die("bad --part-mb")),
            "--workers" => workers = val().parse().unwrap_or_else(|_| die("bad --workers")),
            "--compress" => compress = true,
            "--extra" | "--replace" => {
                let v = val();
                let (k, p) = v.split_once('=').unwrap_or_else(|| die("--extra VIRT=PATH / --replace REL=PATH"));
                if a == "--extra" {
                    extra.push((k.to_string(), PathBuf::from(p)));
                } else {
                    replace.insert(k.to_string(), PathBuf::from(p));
                }
            }
            o if o.starts_with("--") => die(&format!("unknown option {o}")),
            o => pos.push(PathBuf::from(o)),
        }
    }
    let [game_root, web] = &pos[..] else { die("usage: uewpack <game root> <web dir> [options]") };
    if workers > 0 {
        let _ = rayon::ThreadPoolBuilder::new().num_threads(workers).build_global();
    }
    let stub_re = Regex::new(r"(?i)\.(exe|dll|pdb|sym|lib|exp|bat|cmd|sh|command)$").unwrap();
    let skip_re = Regex::new(r"(?i)(^|/)(Saved|Intermediate|DerivedDataCache)/|(^|/)steam_appid\.txt$|\.(log|dmp)$").unwrap();
    let audio_re = Regex::new(r"(?i)\.(ogg|wav|mp3|wem|bnk|opus|flac|xwb|xsb|fsb|bank|m4a|aac|wma)$").unwrap();

    let data = web.join("data");
    fs::create_dir_all(&data).unwrap_or_else(|e| die(&format!("can't make {}: {e}", data.display())));
    let part = part_mb << 20;
    let mut files = Vec::new();
    walk(game_root, &mut files);
    // a build made inside the game's folder doesn't pack its own output
    let out_dir = web.parent().and_then(|p| p.canonicalize().ok());
    files.retain(|f| out_dir.as_ref().map_or(true, |o| !f.canonicalize().map_or(false, |c| c.starts_with(o))));
    let mut rels: Vec<(String, PathBuf)> = files
        .into_iter()
        .filter_map(|p| Some((p.strip_prefix(game_root).ok()?.to_string_lossy().replace('\\', "/"), p)))
        .collect();
    rels.sort_by(|a, b| a.0.to_lowercase().cmp(&b.0.to_lowercase()).then(a.0.cmp(&b.0)));
    let mut entries: Vec<Entry> = rels
        .into_iter()
        .filter(|(rel, _)| !skip_re.is_match(rel))
        .map(|(rel, p)| {
            let stub = stub_re.is_match(&p.file_name().unwrap_or_default().to_string_lossy());
            Entry { virt: format!("/game/{rel}"), src: replace.get(&rel).cloned().unwrap_or(p), stub }
        })
        .collect();
    entries.extend(extra.into_iter().map(|(virt, src)| Entry { virt, src, stub: false }));

    // what each file id held last time, kept beside (not inside) the published folder
    let stamp = web.parent().unwrap_or(Path::new(".")).join(format!("{}.files", web.file_name().unwrap_or_default().to_string_lossy()));
    let mut before: HashMap<String, (String, Vec<String>, String)> = HashMap::new();
    for line in fs::read_to_string(&stamp).unwrap_or_default().lines() {
        let f: Vec<&str> = line.split('\t').collect();
        let get = |k: usize| f.get(k).copied().unwrap_or("").to_string();
        let names = get(2);
        before.insert(get(0), (get(1), if names.is_empty() { vec![] } else { names.split(',').map(String::from).collect() }, get(3)));
    }
    let mut rows = Vec::new();
    let mut after: Vec<(usize, String, Vec<String>, String)> = Vec::new();
    let mut jobs = Vec::new();
    let mut total = 0u64;
    for (i, e) in entries.iter().enumerate() {
        let md = fs::metadata(&e.src).unwrap_or_else(|err| die(&format!("can't read {}: {err}", e.src.display())));
        let mt = md.modified().ok().and_then(|t| t.duration_since(UNIX_EPOCH).ok()).unwrap_or_default();
        let z = compress && !e.stub && !audio_re.is_match(&e.src.to_string_lossy()) && md.len() > 0;
        rows.push((md.len(), (mt.as_secs() + 11644473600) * 10_000_000, if e.stub { 1 } else if z { 2 } else { 0 }, e.virt.clone(), String::new()));
        if e.stub {
            continue;
        }
        total += md.len();
        let sig = format!("{}|{}|{}|{}|{}", if z { 'z' } else { 'r' }, e.src.display(), md.len(), mt.as_nanos(), part);
        match before.get(&i.to_string()) {
            Some((s, names, blocks)) if *s == sig && names.iter().all(|n| data.join(n).exists()) => {
                rows[i].4 = blocks.clone();
                after.push((i, sig, names.clone(), blocks.clone()));
            }
            _ => jobs.push((i, sig, z, md.len())),
        }
    }
    let work = jobs.iter().map(|j| j.3).sum::<u64>().max(1);
    let moved = AtomicU64::new(0);
    println!("@@progress package 0 {work}");
    let results: Vec<_> = jobs
        .par_iter()
        .map(|(i, sig, z, size)| {
            let r = if *z { deflated(*i, &entries[*i].src, &data, part) } else { raw(*i, &entries[*i].src, &data, part) };
            let m = moved.fetch_add(*size, Ordering::Relaxed) + size;
            println!("@@progress package {m} {work}");
            (*i, sig.clone(), r)
        })
        .collect();
    let mut written = 0;
    for (i, sig, (names, blocks, w)) in results {
        rows[i].4 = blocks.clone();
        written += w;
        after.push((i, sig, names, blocks));
    }
    after.sort_by_key(|a| a.0);
    let keep: HashSet<&String> = after.iter().flat_map(|a| a.2.iter()).collect();
    for old in fs::read_dir(&data).into_iter().flatten().flatten() {
        let n = old.file_name().to_string_lossy().into_owned();
        if n != "index.txt" && !keep.contains(&n) {
            let _ = fs::remove_file(old.path());
        }
    }
    let mut index = format!("uewasm-files {} {part}\n", if compress { 2 } else { 1 });
    for (size, mtime, flag, virt, blocks) in &rows {
        index += &format!("{size}\t{mtime}\t{flag}\t{virt}");
        if !blocks.is_empty() {
            index += &format!("\t{blocks}");
        }
        index.push('\n');
    }
    create(&data.join("index.txt")).write_all(index.as_bytes()).unwrap();
    let st: Vec<String> = after.iter().map(|(i, s, n, b)| format!("{i}\t{s}\t{}\t{b}", n.join(","))).collect();
    fs::write(&stamp, st.join("\n")).unwrap_or_else(|e| die(&format!("can't write {}: {e}", stamp.display())));
    let served: u64 = fs::read_dir(&data).into_iter().flatten().flatten().filter_map(|e| e.metadata().ok()).map(|m| m.len()).sum();
    println!(
        "game files: {} listed, {} MB of game data served as {} MB ({} files updated, {} MB written, the rest linked or unchanged)",
        entries.len(),
        (total as f64 / 1048576.0).round(),
        (served as f64 / 1048576.0).round(),
        jobs.len(),
        (written as f64 / 1048576.0).round()
    );
}
