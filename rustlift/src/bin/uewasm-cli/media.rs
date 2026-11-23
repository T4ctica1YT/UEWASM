//! Smaller game videos for the web build. Videos are re-encoded at most `height` lines tall, in the codec family they
//! came in (VP8/VP9 WebM stays WebM, H.264 MP4 stays MP4) so the game's own player still decodes them. Unreal .pak
//! files are rewritten with the new videos; everything else in them is copied as stored. Results are cached, so a
//! rebuild only encodes what changed.
use crate::upak;
use crate::util::*;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::Mutex;

fn is_video(name: &str) -> bool {
    let n = name.to_lowercase();
    [".webm", ".mp4", ".mkv", ".mov", ".m4v"].iter().any(|e| n.ends_with(e))
}

fn mode_args(mode: &str, family: &str) -> Vec<&'static str> {
    // "fast" uses the real-time encoder (about 10x faster, about 1.5x larger)
    match (mode, family) {
        ("fast", "vp9") => vec!["-deadline", "realtime", "-cpu-used", "8", "-crf", "32"],
        ("fast", "vp8") => vec!["-deadline", "realtime", "-cpu-used", "8", "-crf", "12"],
        ("fast", _) => vec!["-preset", "veryfast", "-crf", "24"],
        (_, "vp9") => vec!["-deadline", "good", "-cpu-used", "5", "-crf", "32"],
        (_, "vp8") => vec!["-deadline", "good", "-cpu-used", "2", "-crf", "12"],
        _ => vec!["-preset", "slow", "-crf", "24"],
    }
}

/// (codec, height) of the first video stream
fn probe(ff: &Path, path: &Path) -> (Option<String>, u32) {
    let Ok(o) = quiet(Command::new(ff).args(["-hide_banner", "-i"]).arg(path)).output() else { return (None, 0) };
    let e = String::from_utf8_lossy(&o.stderr);
    let re = regex::Regex::new(r"Video: (\w+).*?, (\d+)x(\d+)").unwrap();
    match re.captures(&e) {
        Some(c) => (Some(c[1].to_string()), c[3].parse().unwrap_or(0)),
        None => (None, 0),
    }
}

/// re-encodes src into dst if it is taller than height; false when the original should be kept
fn encode(ff: &Path, src: &Path, dst: &Path, height: u32, mode: &str, threads: usize) -> Res<bool> {
    let (codec, h) = probe(ff, src);
    let Some(codec) = codec else { return Ok(false) };
    if h <= height {
        return Ok(false);
    }
    let family = match codec.as_str() { "vp9" => "vp9", "vp8" => "vp8", _ => "h264" };
    let enc: &[&str] = match family {
        "vp9" => &["-c:v", "libvpx-vp9", "-b:v", "0", "-row-mt", "1", "-tile-columns", "2"],
        "vp8" => &["-c:v", "libvpx", "-b:v", "8M"],
        _ => &["-c:v", "libx264", "-pix_fmt", "yuv420p", "-movflags", "+faststart"],
    };
    let fmt = if family == "h264" { "mp4" } else { "webm" };
    let tmp = PathBuf::from(format!("{}.part", dst.display()));
    let o = quiet(Command::new(ff).args(["-hide_banner", "-loglevel", "error", "-y", "-i"]).arg(src).args(["-vf", &format!("scale=-2:{height}")])
        .args(enc).args(mode_args(mode, family)).args(["-threads", &threads.to_string(), "-c:a", "copy", "-f", fmt]).arg(&tmp))
        .output().map_err(|e| e.to_string())?;
    if !o.status.success() || !tmp.exists() {
        return err(format!("ffmpeg failed on {}:\n{}", src.display(), tail(&String::from_utf8_lossy(&o.stderr), 1500)));
    }
    let size = |p: &Path| std::fs::metadata(p).map_or(0, |m| m.len());
    if size(&tmp) >= size(src) {
        let _ = std::fs::remove_file(&tmp);
        return Ok(false);
    }
    std::fs::rename(&tmp, dst).map_err(|e| e.to_string())?;
    Ok(true)
}

fn key(parts: &[String]) -> String {
    hex(&sha1(parts.join("|").as_bytes()))[..20].to_string()
}

fn ext(name: &str) -> String {
    Path::new(name).extension().map(|e| format!(".{}", e.to_string_lossy().to_lowercase())).unwrap_or_default()
}

enum Src {
    Pak { rel: String, entry: upak::Entry },
    File(PathBuf),
}

struct Job {
    src: Src,
    cache: PathBuf,
    size: u64,
}

/// {path relative to game_root: replacement file} for files that got smaller
pub fn shrink(game_root: &Path, exe: &Path, out: &Path, height: u32, mode: &str, mut key_in: Option<Vec<u8>>, log: Log, progress: Progress)
    -> Res<Vec<(String, PathBuf)>> {
    let ff = crate::deps::ffmpeg().ok_or("ffmpeg wasn't found: run `uewasm-cli install ffmpeg`, or set UEWASM_FFMPEG")?;
    let oodle = crate::deps::oodle(Some(game_root));
    let cache = out.join("media");
    std::fs::create_dir_all(&cache).map_err(|e| e.to_string())?;
    let mut jobs = vec![];
    let mut paks: HashMap<String, (upak::Pak, Option<Vec<u8>>)> = HashMap::new();
    for p in files_under(game_root) {
        let rel = p.strip_prefix(game_root).unwrap_or(&p).to_string_lossy().replace('\\', "/");
        let name = p.file_name().unwrap_or_default().to_string_lossy().to_string();
        if p.extension().map_or(false, |e| e.eq_ignore_ascii_case("pak")) {
            let r = (|| -> Res<()> {
                let ft = upak::footer(&p)?;
                if ft.version < 11 {
                    return Ok(());
                }
                if key_in.is_none() && ft.enc {
                    let k = upak::find_aes_key(exe, &p);
                    log(if k.is_some() { "pak AES key found in the executable" } else { "pak index is encrypted and no key was found; paks are left as they are" });
                    key_in = Some(k.unwrap_or_default());
                }
                let k = key_in.clone().filter(|k| !k.is_empty());
                let pk = upak::open_pak(&p, k.as_deref())?;
                let vids: Vec<upak::Entry> = pk.entries.iter().filter(|e| is_video(&e.name)).cloned().collect();
                if !vids.is_empty() {
                    for e in vids {
                        let c = cache.join(format!("{}{}", key(&[rel.clone(), e.name.clone(), e.offset.to_string(), e.size.to_string(), height.to_string(), mode.into()]), ext(&e.name)));
                        jobs.push(Job { size: e.size, src: Src::Pak { rel: rel.clone(), entry: e }, cache: c });
                    }
                    paks.insert(rel.clone(), (pk, k));
                }
                Ok(())
            })();
            if let Err(e) = r {
                log(&format!("  {rel}: {e}"));
            }
        } else if is_video(&name) {
            let md = std::fs::metadata(&p).map_err(|e| e.to_string())?;
            let mt = md.modified().ok().and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok()).map_or(0, |d| d.as_nanos());
            let c = cache.join(format!("{}{}", key(&[rel.clone(), md.len().to_string(), mt.to_string(), height.to_string(), mode.into()]), ext(&name)));
            jobs.push(Job { size: md.len(), src: Src::File(p.clone()), cache: c });
        }
    }
    if jobs.is_empty() {
        return Ok(vec![]);
    }
    let total: u64 = jobs.iter().map(|j| j.size).sum();
    log(&format!("{} videos ({:.1} GiB): re-encoding to {height}p ({mode})", jobs.len(), total as f64 / (1u64 << 30) as f64));
    let plan = crate::res::plan();
    let next = Mutex::new(0usize);
    let results: Mutex<Vec<(usize, Res<bool>)>> = Mutex::new(vec![]);
    let logs: Mutex<Vec<String>> = Mutex::new(vec![]);
    let done_bytes = Mutex::new(0u64);
    let (tx, rx) = std::sync::mpsc::channel::<u64>();
    std::thread::scope(|s| {
        for _ in 0..plan.media_workers.max(1) {
            let tx = tx.clone();
            let (next, results, logs, done_bytes, jobs, paks, oodle, ff, plan) = (&next, &results, &logs, &done_bytes, &jobs, &paks, &oodle, &ff, &plan);
            s.spawn(move || loop {
                let i = {
                    let mut n = next.lock().unwrap();
                    let i = *n;
                    *n += 1;
                    i
                };
                let Some(j) = jobs.get(i) else { break };
                let keep = PathBuf::from(format!("{}.keep", j.cache.display()));
                let r = if j.cache.exists() || keep.exists() {
                    Ok(j.cache.exists())
                } else {
                    let r = (|| -> Res<bool> {
                        let (path, tmp) = match &j.src {
                            Src::Pak { rel, entry } => {
                                let (pk, k) = &paks[rel];
                                let tmp = j.cache.with_extension(format!("src{}", ext(&entry.name)));
                                let mut f = std::fs::File::open(&pk.path).map_err(|e| e.to_string())?;
                                std::fs::write(&tmp, upak::read_entry(&mut f, entry, k.as_deref(), oodle.as_deref())?).map_err(|e| e.to_string())?;
                                (tmp.clone(), Some(tmp))
                            }
                            Src::File(p) => (p.clone(), None),
                        };
                        let ok = encode(&ff, &path, &j.cache, height, mode, plan.media_threads);
                        if let Some(t) = tmp {
                            let _ = std::fs::remove_file(t);
                        }
                        let ok = ok?;
                        if !ok {
                            let _ = std::fs::write(&keep, b"");
                        }
                        Ok(ok)
                    })();
                    r
                };
                let r = match r {
                    // an entry that can't be read here (no Oodle DLL) keeps its original
                    Err(e) if matches!(j.src, Src::Pak { .. }) && e.contains("Oodle") => {
                        logs.lock().unwrap().push(format!("  {e}; keeping the original"));
                        Ok(false)
                    }
                    r => r,
                };
                let d = {
                    let mut d = done_bytes.lock().unwrap();
                    *d += j.size;
                    *d
                };
                let _ = tx.send(d);
                results.lock().unwrap().push((i, r));
            });
        }
        drop(tx);
        for d in rx {
            progress("media", d as f64, total as f64);
        }
    });
    for l in logs.into_inner().unwrap() {
        log(&l);
    }
    let mut loose = vec![];
    let mut repl: HashMap<String, HashMap<String, PathBuf>> = HashMap::new();
    let mut res = results.into_inner().unwrap();
    res.sort_by_key(|r| r.0);
    for (i, r) in res {
        if r? {
            let j = &jobs[i];
            match &j.src {
                Src::Pak { rel, entry } => {
                    repl.entry(rel.clone()).or_default().insert(entry.name.clone(), j.cache.clone());
                }
                Src::File(p) => loose.push((p.strip_prefix(game_root).unwrap_or(p).to_string_lossy().replace('\\', "/"), j.cache.clone())),
            }
        }
    }
    let mut result = loose;
    let mut rels: Vec<&String> = repl.keys().collect();
    rels.sort();
    for rel in rels {
        let r = &repl[rel];
        let (pk, _) = &paks[rel];
        let dst = out.join("paks").join(rel);
        std::fs::create_dir_all(dst.parent().unwrap()).map_err(|e| e.to_string())?;
        let mut names: Vec<String> = r.iter().map(|(n, c)| format!("{n}|{}", c.display())).collect();
        names.sort();
        let mut parts = vec!["pak-writer-2".to_string()];
        parts.extend(names);
        parts.push(std::fs::metadata(&pk.path).map_or(0, |m| m.len()).to_string());
        let sig = key(&parts);
        let stamp = dst.with_extension("stamp");
        if !dst.exists() || std::fs::read_to_string(&stamp).ok().as_deref() != Some(sig.as_str()) {
            upak::write_pak(pk, &dst, r)?;
            std::fs::write(&stamp, &sig).map_err(|e| e.to_string())?;
        }
        result.push((rel.clone(), dst));
    }
    let size = |p: &Path| std::fs::metadata(p).map_or(0, |m| m.len());
    let before: u64 = result.iter().map(|(r, _)| size(&game_root.join(r))).sum();
    let after: u64 = result.iter().map(|(_, p)| size(p)).sum();
    log(&format!("videos: {} files went from {:.2} GiB to {:.2} GiB", result.len(), before as f64 / (1u64 << 30) as f64, after as f64 / (1u64 << 30) as f64));
    Ok(result)
}
