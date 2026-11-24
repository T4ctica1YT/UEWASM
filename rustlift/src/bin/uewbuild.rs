//! The web build's heavy steps, driven by uewasm-cli (its build.rs) through a JSON job file:
//!   uewbuild compile <job.json>   compile the lifted C and the runtime with Emscripten, then link game.js/game.wasm
//!   uewbuild shell <job.json>     write index.html (and _headers, coi-sw.js), cutting game.wasm into parts if big
//!   uewbuild single <web dir> <dest.html> [chunk MB]   one .html holding the whole build, for opening from disk
//! Progress goes to stdout as "@@progress <stage> <done> <total>" lines; everything else printed is for the log.
use flate2::write::GzEncoder;
use flate2::Compression;
use serde_json::{json, Value};
use std::collections::HashMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant, SystemTime};

const SHELL: &str = include_str!("web/shell.html");
const COI_SW: &str = include_str!("web/coi-sw.js");
const MEM64_CHECK: &str = include_str!("web/mem64-check.html");
const COI_LOAD: &str = include_str!("web/coi-load.html");
const EMBED_JS: &str = include_str!("web/embed.html");
const PORTS: [&str; 2] = ["-sUSE_OGG=1", "-sUSE_VORBIS=1"];
const GAME_JS_TAG: &str = "<script src=\"game.js\"></script>";

fn die(msg: &str) -> ! {
    eprintln!("{msg}");
    std::process::exit(1)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let job = |i: usize| -> Value {
        let p = args.get(i).unwrap_or_else(|| die("uewbuild: a job file is needed"));
        serde_json::from_str(&fs::read_to_string(p).unwrap_or_else(|e| die(&format!("uewbuild: {p}: {e}"))))
            .unwrap_or_else(|e| die(&format!("uewbuild: {p}: {e}")))
    };
    match args.first().map(|s| s.as_str()) {
        Some("compile") => compile(&job(1)),
        Some("shell") => shell(&job(1)),
        Some("single") => {
            let (Some(web), Some(dest)) = (args.get(1), args.get(2)) else { die("usage: uewbuild single <web dir> <dest.html> [chunk MB]") };
            single(Path::new(web), Path::new(dest), args.get(3).and_then(|s| s.parse().ok()).unwrap_or(16));
        }
        _ => die("usage: uewbuild compile|shell <job.json> | uewbuild single <web dir> <dest.html>"),
    }
}

fn st<'a>(j: &'a Value, k: &str) -> &'a str {
    j.get(k).and_then(|v| v.as_str()).unwrap_or("")
}
fn flag(j: &Value, k: &str) -> bool {
    j.get(k).and_then(|v| v.as_bool()).unwrap_or(false)
}
fn int(j: &Value, k: &str, d: u64) -> u64 {
    j.get(k).and_then(|v| v.as_u64()).unwrap_or(d)
}
fn strs(j: &Value, k: &str) -> Vec<String> {
    j.get(k).and_then(|v| v.as_array()).map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect()).unwrap_or_default()
}
fn mtime(p: &Path) -> SystemTime {
    fs::metadata(p).and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH)
}
fn sorted_glob(dir: &Path, ext: &str) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file() && p.extension().map_or(false, |e| e == ext))
        .collect();
    v.sort();
    v
}
fn walk_c(dir: &Path, out: &mut Vec<PathBuf>) {
    for e in fs::read_dir(dir).into_iter().flatten().flatten() {
        let p = e.path();
        if p.is_dir() {
            walk_c(&p, out);
        } else if p.extension().map_or(false, |x| x == "c") {
            out.push(p);
        }
    }
}
fn stem(p: &Path) -> String {
    p.file_stem().unwrap_or_default().to_string_lossy().into_owned()
}
fn posix(p: &Path) -> String {
    p.to_string_lossy().replace('\\', "/")
}

/// a tool process: no console window, and the caller's priority (Windows children inherit a lowered class)
fn command(prog: &str, env: &HashMap<String, String>) -> Command {
    let mut c = Command::new(prog);
    c.envs(env);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x0800_0000);
    }
    c
}

/// shell-style words: quotes group, backslash escapes outside single quotes
fn split_words(s: &str) -> Vec<String> {
    let (mut out, mut cur, mut have) = (Vec::new(), String::new(), false);
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
        match c {
            '\'' => {
                have = true;
                for d in it.by_ref() {
                    if d == '\'' {
                        break;
                    }
                    cur.push(d);
                }
            }
            '"' => {
                have = true;
                while let Some(d) = it.next() {
                    match d {
                        '"' => break,
                        '\\' if matches!(it.peek(), Some('"' | '\\' | '$' | '`')) => cur.push(it.next().unwrap()),
                        _ => cur.push(d),
                    }
                }
            }
            '\\' => {
                have = true;
                if let Some(d) = it.next() {
                    cur.push(d);
                }
            }
            c if c.is_whitespace() => {
                if have {
                    out.push(std::mem::take(&mut cur));
                    have = false;
                }
            }
            c => {
                have = true;
                cur.push(c);
            }
        }
    }
    if have {
        out.push(cur);
    }
    out
}

struct Job {
    name: String,
    cmd: Vec<String>,
    mb: f64,
}

/// biggest first, as many at once as the cores and the memory budget allow; progress by estimated size
fn run_jobs(mut jobs: Vec<Job>, what: &str, env: &HashMap<String, String>, workers: usize, budget: f64, times_file: &str) {
    jobs.sort_by(|a, b| b.mb.partial_cmp(&a.mb).unwrap());
    let total = jobs.iter().map(|j| j.mb).sum::<f64>().max(1.0);
    let mut pending: std::collections::VecDeque<Job> = jobs.into();
    let mut running: Vec<(Child, Job, PathBuf, Instant)> = Vec::new();
    let (mut used, mut done) = (0.0f64, 0.0f64);
    let mut times = Vec::new();
    let tmp = std::env::temp_dir();
    println!("@@progress {} 0 {}", what.replace(' ', "_"), total as u64);
    let mut n = 0u64;
    while !pending.is_empty() || !running.is_empty() {
        while let Some(j) = pending.front() {
            if running.len() >= workers.max(1) || (!running.is_empty() && used + j.mb > budget) {
                break;
            }
            let j = pending.pop_front().unwrap();
            n += 1;
            let log = tmp.join(format!("uewbuild-{}-{n}.log", std::process::id()));
            let f = fs::File::create(&log).unwrap_or_else(|e| die(&format!("{}: {e}", log.display())));
            let child = command(&j.cmd[0], env)
                .args(&j.cmd[1..])
                .stdout(f.try_clone().unwrap())
                .stderr(f)
                .spawn()
                .unwrap_or_else(|e| die(&format!("{what} failed to start {}: {e}", j.cmd[0])));
            used += j.mb;
            running.push((child, j, log, Instant::now()));
        }
        std::thread::sleep(Duration::from_millis(50));
        let mut k = 0;
        while k < running.len() {
            let status = running[k].0.try_wait().unwrap_or(None);
            let Some(status) = status else {
                k += 1;
                continue;
            };
            let (_, j, log, t0) = running.remove(k);
            times.push((t0.elapsed().as_secs_f64(), j.name.clone()));
            used -= j.mb;
            done += j.mb;
            println!("@@progress {} {} {}", what.replace(' ', "_"), done as u64, total as u64);
            let text = fs::read_to_string(&log).unwrap_or_default();
            let _ = fs::remove_file(&log);
            if !status.success() {
                for (mut c, _, l, _) in running.drain(..) {
                    let _ = c.kill();
                    let _ = c.wait();
                    let _ = fs::remove_file(l);
                }
                let tail: String = text.chars().rev().take(3000).collect::<Vec<_>>().into_iter().rev().collect();
                die(&format!("{what} failed on {}:\n{tail}", j.name));
            }
        }
    }
    // developer hook: UEW_TIMES=<file> records how long each job took (slowest first)
    if !times_file.is_empty() && !times.is_empty() {
        times.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap());
        let s: String = times.iter().map(|(t, n)| format!("{t:8.1} {n}\n")).collect();
        let _ = fs::write(times_file, s);
    }
}

fn compile(j: &Value) {
    let out = PathBuf::from(st(j, "out"));
    let runtime = PathBuf::from(st(j, "runtime"));
    let emcc = st(j, "emcc").to_string();
    let env: HashMap<String, String> =
        j.get("env").and_then(|e| e.as_object()).map(|m| m.iter().map(|(k, v)| (k.clone(), s_of(v))).collect()).unwrap_or_default();
    let (opt, simd, mt, mem64, lto) = (st(j, "opt").to_string(), flag(j, "simd"), st(j, "threads") == "real", flag(j, "mem64"), flag(j, "lto"));
    let debug_panel = flag(j, "debug_panel");
    let t0 = Instant::now();

    // real threads compile every file differently (shared memory, atomics), so their objects are kept apart
    let obj = out.join(format!("{}{}{}", if mt { "wobj_mt" } else { "wobj" }, if mem64 { "_m64" } else { "" }, if lto { "_lto" } else { "" }));
    let _ = fs::create_dir_all(&obj);
    let mut cflags: Vec<String> = vec![opt.clone(), "-w".into(), "-mtail-call".into()];
    if simd {
        cflags.push("-msimd128".into());
    }
    cflags.extend(
        ["-sSUPPORT_LONGJMP=wasm", "-DUEW_WEB_GPU"].iter().map(|s| s.to_string()).chain([
            format!("-I{}", runtime.display()),
            format!("-I{}", out.join("gen").display()),
            "-D_CRT_SECURE_NO_WARNINGS".into(),
            "-fno-strict-aliasing".into(),
        ]),
    );
    cflags.extend(strs(j, "extra_cflags"));
    if st(j, "renderer") == "webgpu" {
        cflags.push("-DUEW_WEBGPU".into());
    }
    if mt {
        cflags.extend(["-pthread".into(), "-DUEW_THREADS".into()]);
    }
    if mem64 {
        cflags.push("-sMEMORY64=1".into());
        cflags.extend(strs(j, "mem64_defines").into_iter().map(|d| format!("-D{d}")));
    }
    if lto {
        cflags.push("-flto=thin".into());
    }
    // generated code only depends on rt.h/sse.h and the opt level; the runtime is always rebuilt (it is small)
    let stamp = obj.join("flags.txt");
    let key = cflags.join(" ");
    if fs::read_to_string(&stamp).ok().as_deref() != Some(key.as_str()) {
        for o in sorted_glob(&obj, "o") {
            let _ = fs::remove_file(o);
        }
        let _ = fs::write(&stamp, &key);
    }
    let newest_h = ["rt.h", "sse.h"].iter().map(|n| mtime(&runtime.join(n))).max().unwrap();
    let gen = sorted_glob(&out.join("gen"), "c");
    // third-party libraries in the runtime (BearSSL, for Schannel): compiled once, objects named bssl_<file>
    let bssl = runtime.join("third_party").join("bearssl");
    let mut lib_srcs = Vec::new();
    walk_c(&bssl.join("src"), &mut lib_srcs);
    lib_srcs.sort();
    let lib_stems: std::collections::HashSet<String> = lib_srcs.iter().map(|p| format!("bssl_{}", stem(p))).collect();
    for o in sorted_glob(&obj, "o") {
        let s = stem(&o);
        if !out.join("gen").join(format!("{s}.c")).exists() && !runtime.join(format!("{s}.c")).exists() && !lib_stems.contains(&s) {
            let _ = fs::remove_file(o);
        }
    }
    let lib_srcs: Vec<PathBuf> = lib_srcs
        .into_iter()
        .filter(|p| {
            let o = obj.join(format!("bssl_{}.o", stem(p)));
            !o.exists() || mtime(&o) < mtime(p)
        })
        .collect();
    let lib_inc = [format!("-I{}", bssl.join("inc").display()), format!("-I{}", bssl.join("src").display())];
    let gen_srcs: Vec<PathBuf> = gen
        .into_iter()
        .filter(|p| {
            let o = obj.join(format!("{}.o", stem(p)));
            !o.exists() || mtime(&o) < mtime(p).max(newest_h)
        })
        .collect();
    let rt_srcs = sorted_glob(&runtime, "c");
    // generated files go straight to clang with emcc's own flags, skipping emcc's per-file startup
    let clang = Path::new(&emcc).parent().and_then(|p| p.parent()).map(|p| p.join("bin").join(if cfg!(windows) { "clang.exe" } else { "clang" }));
    let em = command(&emcc, &env)
        .arg("--cflags")
        .args(&cflags)
        .stderr(Stdio::null())
        .output()
        .map(|o| split_words(&String::from_utf8_lossy(&o.stdout)))
        .unwrap_or_default();
    let clang = clang.filter(|c| c.exists() && !em.is_empty());
    // the debug panel and what it drives are only built in when wanted (nothing for a game's anti-cheat to find)
    let debug_defs: Vec<String> = if debug_panel { vec!["-DUEW_DEBUG_TOOLS".into()] } else { vec![] };
    let mut jobs = Vec::new();
    let lib_set: std::collections::HashSet<&PathBuf> = lib_srcs.iter().collect();
    for (src, generated) in gen_srcs.iter().map(|p| (p, true)).chain(rt_srcs.iter().chain(lib_srcs.iter()).map(|p| (p, false))) {
        let is_lib = lib_set.contains(src);
        let o = obj.join(format!("{}{}.o", if is_lib { "bssl_" } else { "" }, stem(src))).to_string_lossy().into_owned();
        // peak clang memory measured on generated files: ~175 MB plus ~60 MB per MB of source
        let mb = 190.0 + 62.0 * fs::metadata(src).map(|m| m.len()).unwrap_or(0) as f64 / 1048576.0;
        let name = src.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let srcs = src.to_string_lossy().into_owned();
        let cmd: Vec<String> = match (&clang, generated) {
            (Some(c), true) => std::iter::once(c.to_string_lossy().into_owned())
                .chain(em.iter().cloned())
                .chain(cflags.iter().filter(|f| !f.starts_with("-s")).cloned())
                .chain(["-c".into(), srcs, "-o".into(), o])
                .collect(),
            _ => std::iter::once(emcc.clone())
                .chain(cflags.iter().cloned())
                .chain(lib_inc.iter().cloned())
                .chain(PORTS.iter().map(|s| s.to_string()))
                .chain(debug_defs.iter().cloned())
                .chain(["-c".into(), srcs, "-o".into(), o])
                .collect(),
        };
        jobs.push(Job { name, cmd, mb: if clang.is_some() && generated { mb } else { mb + 200.0 } });
    }
    let n_srcs = jobs.len();
    run_jobs(jobs, "wasm compile", &env, int(j, "compile_jobs", 4) as usize, int(j, "budget_mb", 4096) as f64, st(j, "times_file"));

    println!("@@progress link 0 0");
    let web = out.join(st(j, "web_name"));
    let _ = fs::create_dir_all(&web);
    let orsp = out.join("emlink.rsp");
    let objs: Vec<String> = sorted_glob(&obj, "o").iter().map(|p| format!("\"{}\"", posix(p))).collect();
    fs::write(&orsp, objs.join("\n")).unwrap_or_else(|e| die(&format!("{}: {e}", orsp.display())));
    let rt_web = runtime.join("web");
    let mut exports = vec!["_main", "_uew_ctx_start", "_uew_threads"];
    if debug_panel {
        exports.extend(["_uew_set_trace", "_uew_set_speed", "_uew_ue_console", "_uew_d3d9_trace"]);
    }
    let mut link: Vec<String> = vec![if flag(j, "wasm_opt") { opt.clone() } else { "-O1".into() }];
    if lto {
        link.extend(["-flto=thin".into(), "-Wl,--lto-O2".into(), format!("-Wl,--thinlto-jobs={}", int(j, "compile_jobs", 4))]);
    }
    link.push("-mtail-call".into());
    if simd {
        link.push("-msimd128".into());
    }
    link.extend([format!("@{}", orsp.display()), "-o".into(), web.join("game.js").to_string_lossy().into_owned()]);
    link.extend(["-sALLOW_MEMORY_GROWTH=1".into(), "-sINITIAL_MEMORY=64MB".into(), format!("-sMAXIMUM_MEMORY={}", if mem64 { "16GB" } else { "4GB" })]);
    if mem64 {
        link.push("-sMEMORY64=1".into());
    }
    let env_flag = if mt { "-sENVIRONMENT=web,worker,node" } else { "-sENVIRONMENT=web,node" };
    link.extend(["-sSTACK_SIZE=16MB", env_flag, "-sEXIT_RUNTIME=1", "-sMODULARIZE=0", "-sINVOKE_RUN=1"].iter().map(|s| s.to_string()));
    link.extend(PORTS.iter().map(|s| s.to_string()));
    link.extend(
        ["-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", "-sSUPPORT_LONGJMP=wasm", "-sJSPI", "-sJSPI_EXPORTS=['uew_ctx_start']"].iter().map(|s| s.to_string()),
    );
    link.push(format!("-sEXPORTED_FUNCTIONS={}", exports.join(",")));
    let lib = |n: &str| ["--js-library".to_string(), rt_web.join(n).to_string_lossy().into_owned()];
    link.extend(lib("uew_host.js"));
    link.extend(lib("uew_webworks.js"));
    link.extend(lib("uew_steamcm.js"));
    link.extend(lib("uew_epic.js"));
    if st(j, "renderer") == "webgpu" {
        link.extend(lib("uew_webgpu.js"));
    }
    if mt {
        // one worker per core is started with the page (the scheduler's carriers), so none has to load mid-game
        link.extend(["-pthread".into(), "-sPTHREAD_POOL_SIZE=Math.min(4,(globalThis.navigator&&navigator.hardwareConcurrency)||4)".into(),
                     "-sPTHREAD_POOL_SIZE_STRICT=0".into()]);
    }
    if flag(j, "names") {
        link.push("--profiling-funcs".into());
    }
    let pre = |n: &str| ["--pre-js".to_string(), rt_web.join(n).to_string_lossy().into_owned()];
    if flag(j, "game_files") {
        link.extend(pre("uew_files.js"));
        link.extend(pre("uew_save.js"));
        link.extend(pre("uew_wisp.js"));
        if debug_panel {
            link.extend(pre("uew_debug.js"));
        }
        link.extend(["-lidbfs.js".into(), "-sFORCE_FILESYSTEM=1".into()]);
        let b = st(j, "backend_script");
        if !b.is_empty() {
            link.extend(pre(&format!("backends/{b}")));
        }
    }
    if flag(j, "node_fs") {
        link.retain(|x| !x.starts_with("-sENVIRONMENT="));
        link.extend(["-sNODERAWFS=1".into(), "-sENVIRONMENT=node".into()]);
    } else if flag(j, "embed") {
        link.extend(["--embed-file".into(), format!("{}@image.bin", out.join("image.bin").display())]);
    }
    let r = command(&emcc, &env).args(&link).output().unwrap_or_else(|e| die(&format!("emcc link failed to start: {e}")));
    println!("@@progress link 1 1");
    if !r.status.success() {
        let err = String::from_utf8_lossy(&r.stderr);
        let lines: Vec<&str> = err.lines().filter(|l| l.to_lowercase().contains("error") && l.len() < 600).take(20).collect();
        let tail: String = err.chars().rev().take(3000).collect::<Vec<_>>().into_iter().rev().collect();
        die(&format!("emcc link failed:\n{}", if lines.is_empty() { tail } else { lines.join("\n") }));
    }
    // browsers refuse modules over 1 GiB (V8's limit)
    let bytes = fs::metadata(web.join("game.wasm")).map(|m| m.len()).unwrap_or(0);
    let mib = bytes as f64 / 1048576.0;
    if bytes > 1 << 30 {
        println!("warning: the code is {mib:.0} MiB, over the 1024 MiB browsers accept; rebuild with --opt -Os (and --wasm-opt if there is enough memory) to make it smaller");
    } else if bytes > 900 << 20 {
        println!("note: the code is {mib:.0} MiB, close to the 1024 MiB browsers accept");
    }
    println!("@@result {}", json!({ "files": n_srcs, "seconds": t0.elapsed().as_secs_f64(), "wasm_mb": bytes as f64 / 1e6 }));
}

fn s_of(v: &Value) -> String {
    v.as_str().map(String::from).unwrap_or_else(|| v.to_string())
}

fn shell(j: &Value) {
    let web = PathBuf::from(st(j, "web"));
    let part_mb = int(j, "part_mb", 24);
    let wasm = web.join("game.wasm");
    let old: Vec<PathBuf> = fs::read_dir(&web).into_iter().flatten().flatten()
        .filter(|e| e.file_name().to_string_lossy().starts_with("game.wasm.")).map(|e| e.path()).collect();
    if wasm.is_file() {
        for p in &old {
            let _ = fs::remove_file(p);
        }
    } else if old.is_empty() {
        die(&format!("{}: no game.wasm (the link step didn't write one)", web.display()));
    }
    let mut parts = Vec::new();
    if !wasm.is_file() {
        let mut kept: Vec<(usize, String)> = old.iter().filter_map(|p| {
            let n = p.file_name()?.to_string_lossy().to_string();
            Some((n.strip_prefix("game.wasm.")?.parse().ok()?, n))
        }).collect();
        kept.sort();
        parts = kept.into_iter().map(|k| k.1).collect();
    }
    let step = (part_mb << 20) as usize;
    if fs::metadata(&wasm).map(|m| m.len()).unwrap_or(0) > step as u64 {
        let data = fs::read(&wasm).unwrap_or_else(|e| die(&format!("{}: {e}", wasm.display())));
        for (i, c) in data.chunks(step).enumerate() {
            let n = format!("game.wasm.{i}");
            fs::write(web.join(&n), c).unwrap_or_else(|e| die(&format!("{n}: {e}")));
            parts.push(n);
        }
        let _ = fs::remove_file(&wasm);
    }
    let (w, h) = (int(j, "width", 1280), int(j, "height", 720));
    let jb = |k: &str| if flag(j, k) { "true" } else { "false" };
    let js = |k: &str, d: Value| j.get(k).cloned().filter(|v| !v.is_null()).unwrap_or(d).to_string();
    let mut html = SHELL
        .replace("__ICON__", &write_favicon(&web, st(j, "exe"), st(j, "fallback_icon")))
        .replace("__TITLE__", st(j, "title"))
        .replace("__W__", &w.to_string())
        .replace("__H__", &h.to_string())
        .replace("__RES__", if flag(j, "native") { "native" } else { "" })
        .replace("__PARTS__", &json!(parts).to_string())
        .replace("__ARGS__", &js("args", json!([])))
        .replace("__SAVE__", &js("save", json!("")))
        .replace("__SEED__", &js("seed", json!({})))
        .replace("__FPS__", jb("show_fps"))
        .replace("__ASYNC__", jb("async_shaders"))
        .replace("__DEBUG__", jb("debug_panel"))
        .replace("__PLACEHOLDERS__", jb("placeholders"))
        .replace("__CACHEMB__", &int(j, "cache_mb", 640).to_string())
        .replace("__GPUMB__", &int(j, "gpu_mb", 2048).to_string())
        .replace("__STEAM__", &js("steam", json!({ "mode": "offline" })))
        .replace("__EPIC__", &js("epic", json!({ "mode": "off" })))
        .replace("__WISP__", &js("wisp", json!({})));
    let mem64 = flag(j, "mem64");
    let tag = format!("{GAME_JS_TAG}\n");
    if st(j, "threads") == "real" {
        let load = if mem64 {
            COI_LOAD.replace("  if (self.crossOriginIsolated)", "  if (!window.uewMem64Ok) {}\n  else if (self.crossOriginIsolated)")
        } else {
            COI_LOAD.to_string()
        };
        html = html.replace(&tag, &format!("{}{load}", if mem64 { MEM64_CHECK } else { "" }));
        fs::write(web.join("coi-sw.js"), COI_SW).unwrap_or_else(|e| die(&format!("coi-sw.js: {e}")));
    } else {
        if mem64 {
            html = html.replace(&tag, &format!("{MEM64_CHECK}<script>if (window.uewMem64Ok) document.write('<script src=\"game.js\"><\\/script>');</script>\n"));
        }
        let _ = fs::remove_file(web.join("coi-sw.js"));
    }
    fs::write(web.join("index.html"), html).unwrap_or_else(|e| die(&format!("index.html: {e}")));
    // Cloudflare Pages reads this; other hosts ignore it
    let headers = [
        "/*", "  Cross-Origin-Opener-Policy: same-origin", "  Cross-Origin-Embedder-Policy: require-corp", "",
        "/game.wasm*", "  Content-Type: application/wasm", "",
        "/data/*", "  Content-Type: application/octet-stream", "  Cache-Control: public, max-age=3600, must-revalidate", "",
    ];
    fs::write(web.join("_headers"), headers.join("\n")).unwrap_or_else(|e| die(&format!("_headers: {e}")));
}

fn b64(b: &[u8]) -> String {
    const T: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(b.len() / 3 * 4 + 4);
    for c in b.chunks(3) {
        let n = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        for i in 0..4 {
            s.push(if i <= c.len() { T[(n >> (18 - 6 * i)) as usize & 63] as char } else { '=' });
        }
    }
    s
}

/// one .html holding the whole web build (code and game files), for opening straight from disk with no server. Only for
/// cooperative-thread builds: real threads need cross-origin isolation, which a file:// page can't have.
fn single(web: &Path, dest: &Path, chunk_mb: usize) {
    let mut html = fs::read_to_string(web.join("index.html")).unwrap_or_else(|_| die(&format!("not a web build: {}", web.display())));
    if !html.contains("game.js") {
        die(&format!("not a web build: {}", web.display()));
    }
    if let Ok(ico) = fs::read(web.join("favicon.ico")) {
        html = html.replace("href=\"favicon.ico\"", &format!("href=\"data:image/x-icon;base64,{}\"", b64(&ico)));
    }
    let mut parts: Vec<(usize, PathBuf)> = fs::read_dir(web)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let n = e.file_name().to_string_lossy().into_owned();
            n.strip_prefix("game.wasm.").and_then(|k| k.parse().ok()).map(|k| (k, e.path()))
        })
        .collect();
    parts.sort();
    let code: Vec<u8> = if parts.is_empty() {
        fs::read(web.join("game.wasm")).unwrap_or_else(|e| die(&format!("game.wasm: {e}")))
    } else {
        parts.iter().flat_map(|(_, p)| fs::read(p).unwrap_or_default()).collect()
    };
    let data = web.join("data");
    let js = fs::read_to_string(web.join("game.js")).unwrap_or_default().replace("</script", "<\\/script");
    let i = html.find(GAME_JS_TAG).unwrap_or_else(|| die("index.html has no game.js tag"));
    let tmp = dest.with_extension("tmp");
    if let Some(p) = dest.parent() {
        let _ = fs::create_dir_all(p);
    }
    let mut f = std::io::BufWriter::new(fs::File::create(&tmp).unwrap_or_else(|e| die(&format!("{}: {e}", tmp.display()))));
    let put = |f: &mut std::io::BufWriter<fs::File>, name: &str, b: &[u8]| {
        write!(f, "<script type=\"application/x-uew\" data-n=\"{name}\">{}</script>\n", b64(b)).unwrap();
    };
    f.write_all(html[..i].as_bytes()).unwrap();
    let mut z = GzEncoder::new(Vec::new(), Compression::new(6));
    z.write_all(&code).unwrap();
    let z = z.finish().unwrap();
    for (k, c) in z.chunks(chunk_mb << 20).enumerate() {
        put(&mut f, &format!("wasm.{k}"), c);
    }
    if let Ok(index) = fs::read_to_string(data.join("index.txt")) {
        // script text isn't entity-decoded; only "</script" would end it early
        write!(f, "<script type=\"application/x-uew\" data-n=\"index\">{}</script>\n", index.replace("</script", "<\\/script")).unwrap();
        let mut names: Vec<PathBuf> = fs::read_dir(&data).into_iter().flatten().flatten().map(|e| e.path()).collect();
        names.sort();
        for p in names {
            let n = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
            if n != "index.txt" {
                put(&mut f, &n, &fs::read(&p).unwrap_or_default());
            }
        }
    }
    f.write_all(EMBED_JS.as_bytes()).unwrap();
    write!(f, "<script>\n{js}\n</script>\n").unwrap();
    f.write_all(html[i + GAME_JS_TAG.len()..].as_bytes()).unwrap();
    drop(f);
    let _ = fs::remove_file(dest);
    fs::rename(&tmp, dest).unwrap_or_else(|e| die(&format!("{}: {e}", dest.display())));
    let mb = fs::metadata(dest).map(|m| m.len()).unwrap_or(0) as f64 / 1048576.0;
    println!(
        "single file: {} ({mb:.0} MB; code {:.0} MB, {:.0} MB compressed)",
        dest.display(),
        code.len() as f64 / 1048576.0,
        z.len() as f64 / 1048576.0
    );
    if mb > 1500.0 {
        println!("note: browsers may not open a page this big; single-file builds suit smaller games");
    }
}

// ---- the game's own icon as a favicon: the main icon group in the exe's resources, rebuilt as an .ico ----

fn rd16(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]) as u32)
}
fn rd32(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// .ico bytes of the exe's first icon group
fn exe_icon(d: &[u8]) -> Option<Vec<u8>> {
    const RT_ICON: u32 = 3;
    const RT_GROUP_ICON: u32 = 14;
    let pe = rd32(d, 0x3C)? as usize;
    let nsec = rd16(d, pe + 6)? as usize;
    let opt = pe + 24;
    let ddir = opt + if rd16(d, opt)? == 0x20B { 112 } else { 96 };
    let rsrc = rd32(d, ddir + 16)?;
    let sec = opt + rd16(d, pe + 20)? as usize;
    let secs: Vec<[u32; 4]> =
        (0..nsec).filter_map(|i| Some([rd32(d, sec + i * 40 + 8)?, rd32(d, sec + i * 40 + 12)?, rd32(d, sec + i * 40 + 16)?, rd32(d, sec + i * 40 + 20)?])).collect();
    let off = |rva: u32| secs.iter().find(|s| s[1] <= rva && rva < s[1] + s[0].max(s[2])).map(|s| (rva - s[1] + s[3]) as usize);
    if rsrc == 0 {
        return None;
    }
    let base = off(rsrc)?;
    let entries = |o: usize| -> Vec<(Option<u32>, u32)> {
        let (named, ids) = (rd16(d, base + o + 12).unwrap_or(0), rd16(d, base + o + 14).unwrap_or(0));
        (0..(named + ids) as usize)
            .filter_map(|i| {
                let (name, target) = (rd32(d, base + o + 16 + 8 * i)?, rd32(d, base + o + 20 + 8 * i)?);
                Some((if name & 0x8000_0000 != 0 { None } else { Some(name) }, target))
            })
            .collect()
    };
    let mut res: std::collections::BTreeMap<u32, std::collections::BTreeMap<u32, Vec<u8>>> = Default::default();
    for (tid, t) in entries(0) {
        let Some(tid) = tid.filter(|t| *t == RT_ICON || *t == RT_GROUP_ICON) else { continue };
        if t & 0x8000_0000 == 0 {
            continue;
        }
        for (rid, r) in entries((t & 0x7FFF_FFFF) as usize) {
            let Some(rid) = rid.filter(|_| r & 0x8000_0000 != 0) else { continue };
            if let Some(&(_, leaf)) = entries((r & 0x7FFF_FFFF) as usize).first() {
                if leaf & 0x8000_0000 != 0 {
                    continue;
                }
                let (drva, dsize) = (rd32(d, base + leaf as usize)?, rd32(d, base + leaf as usize + 4)? as usize);
                if let Some(o) = off(drva) {
                    if let Some(b) = d.get(o..o + dsize) {
                        res.entry(tid).or_default().insert(rid, b.to_vec());
                    }
                }
            }
        }
    }
    let g = res.get(&RT_GROUP_ICON)?.values().next()?;
    let icons = res.get(&RT_ICON).cloned().unwrap_or_default();
    let mut items = Vec::new();
    for i in 0..rd16(g, 4)? as usize {
        let e = 6 + 14 * i;
        let rid = rd16(g, e + 12)?;
        if let Some(data) = icons.get(&rid) {
            items.push((g.get(e..e + 4)?.to_vec(), rd16(g, e + 4)?, rd16(g, e + 6)?, data));
        }
    }
    if items.is_empty() {
        return None;
    }
    let mut out = vec![0, 0, 1, 0];
    out.extend_from_slice(&(items.len() as u16).to_le_bytes());
    let mut at = 6 + 16 * items.len() as u32;
    for (whc, planes, bits, data) in &items {
        out.extend_from_slice(&[whc[0], whc[1], whc[2], 0]);
        out.extend_from_slice(&(*planes as u16).to_le_bytes());
        out.extend_from_slice(&(*bits as u16).to_le_bytes());
        out.extend_from_slice(&(data.len() as u32).to_le_bytes());
        out.extend_from_slice(&at.to_le_bytes());
        at += data.len() as u32;
    }
    for (_, _, _, data) in &items {
        out.extend_from_slice(data);
    }
    Some(out)
}

/// favicon.ico in the web folder: the game's icon, else the fallback (UEWASM's); its href
fn write_favicon(web: &Path, exe: &str, fallback: &str) -> String {
    let data = (!exe.is_empty()).then(|| fs::read(exe).ok()).flatten().and_then(|d| exe_icon(&d)).or_else(|| fs::read(fallback).ok());
    match data {
        Some(d) if fs::write(web.join("favicon.ico"), &d).is_ok() => "favicon.ico".into(),
        _ => "data:,".into(),
    }
}
