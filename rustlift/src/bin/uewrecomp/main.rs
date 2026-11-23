//! The recompile step: links a game exe with its DLLs into one guest image (image.bin), has uewlift lift every
//! function to C, and writes the tables the runtime starts from (gen/meta.c) and the coverage report.
//!   uewrecomp recompile <job.json>      job: exe, out, mem_mb, search[], extra[], system_dlls, runtime, stdcall_cache
//!   uewrecomp refresh <gen dir> <runtime dir> <stdcall cache>   bind imports to runtime functions added since the lift
//!   uewrecomp game-dlls <exe> [--real-eos]   the DLLs a game ships that it may load, as JSON {root, dlls}
//! A job with "real_eos": true links the game's own EOS SDK instead of leaving it to the runtime's stand-in.
//! Progress goes to stdout as "@@progress <stage> <done> <total>", the report as "@@result <json>"; the rest is log.
mod eh32;
mod link;
mod pe;
mod stdcall;
mod steamstub;

use regex::Regex;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet, HashSet};
use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::time::Instant;

const CHUNK: usize = 400;
const SETJMP: &[&str] = &["_setjmp", "_setjmpex", "setjmp", "_setjmp3", "__intrinsic_setjmp", "__intrinsic_setjmpex"];

fn die(msg: &str) -> ! {
    eprintln!("{msg}");
    std::process::exit(1)
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let arg = |i: usize| args.get(i).cloned().unwrap_or_else(|| die("usage: uewrecomp recompile <job.json> | refresh <gen> <runtime> <stdcall cache> | game-dlls <exe>"));
    match args.first().map(|s| s.as_str()) {
        Some("recompile") => {
            let p = arg(1);
            let job: Value = serde_json::from_str(&std::fs::read_to_string(&p).unwrap_or_else(|e| die(&format!("{p}: {e}"))))
                .unwrap_or_else(|e| die(&format!("{p}: {e}")));
            recompile(&job);
        }
        Some("refresh") => {
            let mut sc = stdcall::Stdcall::new(Path::new(&arg(3)));
            println!("@@result {}", refresh_thunks(Path::new(&arg(1)), Path::new(&arg(2)), &mut sc));
            sc.save();
        }
        Some("game-dlls") => {
            let (root, dlls) = link::game_dlls(Path::new(&arg(1)), args.iter().any(|a| a == "--real-eos"));
            let d: Vec<String> = dlls.iter().map(|p| p.to_string_lossy().into_owned()).collect();
            println!("{}", json!({ "root": root.to_string_lossy(), "dlls": d }));
        }
        _ => die("usage: uewrecomp recompile <job.json> | refresh <gen> <runtime> <stdcall cache> | game-dlls <exe>"),
    }
}

fn write_if_changed(path: &Path, text: &str) {
    if std::fs::read_to_string(path).ok().as_deref() != Some(text) {
        std::fs::write(path, text).unwrap_or_else(|e| die(&format!("{}: {e}", path.display())));
    }
}

/// every runtime function the C sources implement (HLE(name), MATH1(name), STUB(name)...), #define lines aside
fn implemented_hle(runtime: &Path) -> BTreeSet<String> {
    let re = Regex::new(r"\b(?:HLE|MATH1F?|MATH2F?|STUB)\((\w+)").unwrap();
    let mut names = BTreeSet::new();
    let mut files: Vec<PathBuf> = std::fs::read_dir(runtime).into_iter().flatten().flatten().map(|e| e.path()).filter(|p| p.extension().map_or(false, |e| e == "c")).collect();
    files.sort();
    for f in files {
        let text = std::fs::read_to_string(&f).unwrap_or_default();
        for line in text.lines().filter(|l| !l.trim_start().starts_with("#define")) {
            names.extend(re.captures_iter(line).map(|c| c[1].to_string()));
        }
    }
    names
}

fn c_str(s: &str) -> String {
    format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""))
}

fn thunk_line(n: &str) -> String {
    format!("static void T_{n}(CPU* c) {{ if (rt_trace_on > 0) rt_trace_call(c, {}); hle_{n}(c); c->r[4] += 8; if (--rt_hle_budget < 0) rt_hle_tick(c); }}", c_str(n))
}

/// binds imports to runtime functions added since the lift, so a relink picks them up without lifting again
fn refresh_thunks(gen: &Path, runtime: &Path, sc: &mut stdcall::Stdcall) -> usize {
    let meta = gen.join("meta.c");
    let Ok(mut text) = std::fs::read_to_string(&meta) else { return 0 };
    let hle = implemented_hle(runtime);
    let have: HashSet<String> = Regex::new(r"(?m)^void hle_(\w+)\(CPU\*\);").unwrap().captures_iter(&text).map(|c| c[1].to_string()).collect();
    let listed: HashSet<String> =
        Regex::new(r#"(?m)^    \{\w+, "([^"]+)", "[^"]*", \w+\},"#).unwrap().captures_iter(&text).map(|c| c[1].to_string()).collect();
    let null_re = Regex::new(r#"(?m)^    \{NULL, "(\w+)", ("[^"]*"), NULL\},"#).unwrap();
    let new: BTreeSet<String> = null_re.captures_iter(&text).map(|c| c[1].to_string()).filter(|n| hle.contains(n) && !have.contains(n)).collect();
    let extra: BTreeSet<String> = hle.iter().filter(|n| !listed.contains(*n) && !have.contains(*n)).cloned().collect();
    if !text.contains("RT_G32") {
        text += "const int RT_G32 = 0;\nconst unsigned short rt_thunk_pops[] = {0};\n";
        if new.is_empty() && extra.is_empty() {
            write_if_changed(&meta, &text);
        }
    }
    if new.is_empty() && extra.is_empty() {
        return 0;
    }
    text = null_re
        .replace_all(&text, |c: &regex::Captures| {
            if new.contains(&c[1]) { format!("    {{hle_{0}, \"{0}\", {1}, T_{0}}},", &c[1], &c[2]) } else { c[0].to_string() }
        })
        .into_owned();
    // functions nothing imports, for GetProcAddress
    if !extra.is_empty() {
        let start = text.find("const Thunk rt_thunks[] = {").unwrap_or(0);
        let j = text[start..].find("};").map(|k| k + start).unwrap_or(text.len());
        let add: String = extra.iter().map(|n| format!("    {{hle_{n}, \"{n}\", \"hle\", T_{n}}},\n")).collect();
        text.insert_str(j, &add);
        let cnt = Regex::new(r"const size_t rt_thunk_count = (\d+);").unwrap();
        text = cnt
            .replace(&text, |c: &regex::Captures| format!("const size_t rt_thunk_count = {};", c[1].parse::<usize>().unwrap_or(0) + extra.len()))
            .into_owned();
        if text.contains("const int RT_G32 = 1;") {
            let add: Vec<String> = extra.iter().map(|n| sc.pops("", n).unwrap_or(0).to_string()).collect();
            let pops = Regex::new(r"(const unsigned short rt_thunk_pops\[\] = \{[^}]*)\};").unwrap();
            text = pops.replace(&text, |c: &regex::Captures| format!("{}, {}}};", &c[1], add.join(", "))).into_owned();
        }
    }
    let all: BTreeSet<String> = new.union(&extra).cloned().collect();
    let mut decl: String = all.iter().map(|n| format!("void hle_{n}(CPU*);\n")).collect();
    for n in &all {
        decl += &thunk_line(n);
        decl.push('\n');
    }
    let i = text.find("const Thunk rt_thunks[] = {").unwrap_or(0);
    text.insert_str(i, &decl);
    write_if_changed(&meta, &text);
    all.len()
}

/// the game's Steam app id: steam_appid.txt beside the exe, or the Steam library manifest whose install folder holds the
/// game; 0 when neither says
fn steam_appid(exe: &Path) -> u64 {
    let num = Regex::new(r"^\s*(\d+)").unwrap();
    for d in exe.ancestors().skip(1).take(5) {
        if let Ok(t) = std::fs::read(d.join("steam_appid.txt")) {
            if let Some(c) = num.captures(&String::from_utf8_lossy(&t)) {
                return c[1].parse().unwrap_or(0);
            }
        }
    }
    let parts: Vec<String> = exe.components().map(|c| c.as_os_str().to_string_lossy().to_lowercase()).collect();
    let Some(i) = parts.iter().position(|p| p == "common") else { return 0 };
    if !parts.contains(&"steamapps".to_string()) || i + 1 >= parts.len() {
        return 0;
    }
    let apps: PathBuf = exe.components().take(i).collect();
    let (dir_re, id_re) = (Regex::new(r#""installdir"\s*"([^"]*)""#).unwrap(), Regex::new(r#""appid"\s*"(\d+)""#).unwrap());
    for e in std::fs::read_dir(&apps).into_iter().flatten().flatten() {
        let n = e.file_name().to_string_lossy().into_owned();
        if !(n.starts_with("appmanifest_") && n.ends_with(".acf")) {
            continue;
        }
        let t = String::from_utf8_lossy(&std::fs::read(e.path()).unwrap_or_default()).into_owned();
        if let (Some(m), Some(a)) = (dir_re.captures(&t), id_re.captures(&t)) {
            if m[1].to_lowercase() == parts[i + 1] {
                return a[1].parse().unwrap_or(0);
            }
        }
    }
    0
}

fn recompile(job: &Value) {
    let s = |k: &str| job.get(k).and_then(|v| v.as_str()).unwrap_or("").to_string();
    let paths = |k: &str| -> Vec<PathBuf> { job.get(k).and_then(|v| v.as_array()).map(|a| a.iter().filter_map(|x| x.as_str()).map(PathBuf::from).collect()).unwrap_or_default() };
    let (exe, out, runtime) = (PathBuf::from(s("exe")), PathBuf::from(s("out")), PathBuf::from(s("runtime")));
    let mem_mb = job.get("mem_mb").and_then(|v| v.as_u64()).unwrap_or(1024);
    let gen = out.join("gen");
    std::fs::create_dir_all(&gen).unwrap_or_else(|e| die(&format!("{}: {e}", gen.display())));
    let t0 = Instant::now();
    let hle = implemented_hle(&runtime);
    let mut lk = link::Linker::new(&exe, &paths("search"), hle.iter().cloned().collect(), job.get("system_dlls").and_then(|v| v.as_bool()).unwrap_or(true),
                                    job.get("real_eos").and_then(|v| v.as_bool()).unwrap_or(false), &paths("extra"))
        .unwrap_or_else(|e| die(&e));
    let mut sc = stdcall::Stdcall::new(Path::new(&s("stdcall_cache")));
    sc.search = lk.search.clone();
    let images = lk.images();
    let lo = lk.modules[0].base;
    let mut span = vec![0u8; (lk.end - lo) as usize];
    for (m, img) in lk.modules.iter().zip(&images) {
        let o = (m.base - lo) as usize;
        span[o..o + img.len()].copy_from_slice(img);
    }
    std::fs::write(out.join("image.bin"), &span).unwrap_or_else(|e| die(&format!("image.bin: {e}")));
    drop(span);
    drop(images);

    // the lift itself: uewlift, beside this tool
    let mut mods = Vec::new();
    for m in &lk.modules {
        let p = &m.pe;
        let exports: Vec<u32> = p.exports.values().chain(p.exports_ord.values()).filter_map(|e| if let pe::Export::Rva(r) = e { Some(*r) } else { None }).collect();
        let sj: Vec<u64> = if p.bits == 64 { p.imports.iter().filter(|i| SETJMP.contains(&i.name.as_str())).map(|i| m.base + i.iat_rva as u64).collect() } else { vec![] };
        let (eh_starts, eh_conts) = if p.bits == 32 {
            let secs: Vec<(u32, u32, bool)> = p.sections.iter().map(|s| (s.rva, s.vsize, s.executable())).collect();
            eh32::analyze(&p.image(m.base), m.base, &secs)
        } else {
            Default::default()
        };
        mods.push(json!({
            "eh_starts": eh_starts.iter().map(|(a, o)| json!([a, o])).collect::<Vec<_>>(), "eh_conts": eh_conts,
            "base": m.base, "bits": p.bits, "image_size": p.image_size, "entry_rva": p.entry_rva,
            "sections": p.sections.iter().map(|s| json!([s.rva, s.vsize, s.executable()])).collect::<Vec<_>>(),
            "functions": p.functions.iter().map(|f| json!([f.0, f.1])).collect::<Vec<_>>(),
            "unwind": p.unwind.keys().collect::<Vec<_>>(), "family": p.family.iter().map(|(b, r)| json!([b, r])).collect::<Vec<_>>(),
            "exports": exports, "tls_callbacks": p.tls_callbacks, "relocs": p.relocs, "sj_slots": sj,
        }));
    }
    let job_path = out.join("lift_job.json");
    let lj = json!({ "image": out.join("image.bin").to_string_lossy(), "lo": lo, "gen": gen.to_string_lossy(), "runtime": runtime.to_string_lossy(),
                     "chunk": CHUNK, "modules": mods });
    std::fs::write(&job_path, lj.to_string()).unwrap_or_else(|e| die(&format!("{}: {e}", job_path.display())));
    println!("@@progress analyze 0 0");
    let lifter = std::env::current_exe().ok().and_then(|p| p.parent().map(|d| d.join(if cfg!(windows) { "uewlift.exe" } else { "uewlift" }))).unwrap();
    let mut cmd = std::process::Command::new(&lifter);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000);
    }
    let r = cmd.arg(&job_path).output().unwrap_or_else(|e| die(&format!("{}: {e}", lifter.display())));
    let _ = std::fs::remove_file(&job_path);
    if !r.status.success() {
        let e = String::from_utf8_lossy(if r.stderr.is_empty() { &r.stdout } else { &r.stderr }).into_owned();
        die(&format!("the lifter failed:\n{}", &e[e.len().saturating_sub(3000)..]));
    }
    let res: Value = serde_json::from_slice(&r.stdout).unwrap_or_else(|e| die(&format!("the lifter's answer: {e}")));
    let mut roots = BTreeSet::new();
    let mut call_args = BTreeMap::new();
    for (m, mr) in lk.modules.iter().zip(res["modules"].as_array().unwrap_or(&vec![])) {
        let mroots: Vec<u64> = mr["roots"].as_array().map(|a| a.iter().filter_map(|x| x.as_u64()).collect()).unwrap_or_default();
        println!("  {}: {} functions at 0x{:x}", m.name, mroots.len(), m.base);
        roots.extend(mroots);
        for ca in mr["call_args"].as_array().into_iter().flatten() {
            call_args.insert(ca[0].as_u64().unwrap_or(0), ca[1].as_u64().unwrap_or(0));
        }
    }
    let secs = |k: usize| res["seconds"][k].as_f64().unwrap_or(0.0);
    println!("{} modules, {} functions ({:.1}s; discovery {:.1}s, lifting {:.1}s)", lk.modules.len(), roots.len(), t0.elapsed().as_secs_f64(), secs(0), secs(1));
    let mut funcs_h = String::from("#pragma once\n#include \"rt.h\"\n");
    for r in &roots {
        let _ = writeln!(funcs_h, "void F_{r:x}(CPU* __restrict);");
    }
    write_if_changed(&gen.join("funcs.h"), &funcs_h);
    let mut unsupported: BTreeMap<String, u64> = BTreeMap::new();
    let mut where_: BTreeMap<String, Value> = BTreeMap::new();
    let mut bad = 0u64;
    for ch in res["chunks"].as_array().into_iter().flatten() {
        for (k, v) in ch["u"].as_object().into_iter().flatten() {
            *unsupported.entry(k.clone()).or_default() += v.as_u64().unwrap_or(0);
        }
        for (k, v) in ch["at"].as_object().into_iter().flatten() {
            where_.entry(k.clone()).or_insert_with(|| v.clone());
        }
        bad += ch["bad"].as_u64().unwrap_or(0);
    }
    println!("@@progress lift 1 1");
    let ntasks = res["files"].as_u64().unwrap_or(0) as usize;
    println!("lifted into {ntasks} C files ({:.1}s), {bad} instructions unsupported", t0.elapsed().as_secs_f64());

    // the tables the runtime starts from
    let mut l = vec!["#include \"host.h\"".to_string(), "#include \"funcs.h\"".into(), String::new()];
    l.push(format!("const uint64_t IMAGE_LO = 0x{lo:x}ull, IMAGE_HI = 0x{:x}ull, GUEST_MEM_MB = {mem_mb}ull;", lk.end));
    l.push(format!("const int RT_G32 = {};", (lk.bits == 32) as u32));
    let mut cbs: Vec<u64> = Vec::new();
    l.push("const ModuleInfo rt_modules[] = {".into());
    for &i in &lk.init_order {
        let m = &lk.modules[i];
        let first = cbs.len();
        cbs.extend(m.pe.tls_callbacks.iter().map(|&x| m.base + x as u64));
        let t = m.pe.tls.map(|t| (m.base + t.start, m.base + t.end, m.base + t.index, t.zero)).unwrap_or((0, 0, 0, 0));
        l.push(format!(
            "    {{{}, 0x{:x}ull, 0x{:x}ull, 0x{:x}ull, {}, 0x{:x}ull, 0x{:x}ull, 0x{:x}ull, {}ull, {}, {}}},",
            c_str(&m.name), m.base, m.pe.image_size, m.pe.entry_rva, m.is_dll() as u32, t.0, t.1, t.2, t.3, first, cbs.len() - first
        ));
    }
    l.push("};".into());
    l.push(format!("const size_t rt_module_count = {};", lk.init_order.len()));
    let cb_list: Vec<String> = cbs.iter().chain(std::iter::once(&0)).map(|x| format!("0x{x:x}ull")).collect();
    l.push(format!("const uint64_t rt_tls_callbacks[] = {{{}}};", cb_list.join(", ")));
    // every implemented runtime function gets a thunk, so GetProcAddress and delay-loads find ones nothing imports
    let imported: HashSet<&String> = lk.thunks.iter().map(|t| &t.0).collect();
    let mut thunks = lk.thunks.clone();
    thunks.extend(hle.iter().filter(|n| !imported.contains(n)).map(|n| (n.clone(), "hle".to_string())));
    let impl_: BTreeSet<&String> = thunks.iter().map(|t| &t.0).filter(|n| hle.contains(*n)).collect();
    l.extend(impl_.iter().map(|n| format!("void hle_{n}(CPU*);")));
    l.extend(impl_.iter().map(|n| thunk_line(n)));
    l.push("const Thunk rt_thunks[] = {".into());
    for (name, dll) in &thunks {
        let (f, call) = if hle.contains(name) { (format!("hle_{name}"), format!("T_{name}")) } else { ("NULL".into(), "NULL".into()) };
        l.push(format!("    {{{f}, {}, {}, {call}}},", c_str(name), c_str(dll)));
    }
    l.push("};".into());
    l.push(format!("const size_t rt_thunk_count = {};", thunks.len()));
    if lk.bits == 32 {
        let mut unknown = Vec::new();
        let vals: Vec<String> = thunks
            .iter()
            .map(|(name, dll)| {
                let v = sc.pops(if dll == "hle" { "" } else { dll }, name);
                if v.is_none() && dll != "hle" {
                    unknown.push(name.clone());
                }
                v.unwrap_or(0).to_string()
            })
            .collect();
        sc.save();
        if !unknown.is_empty() {
            println!("  stack argument size unknown for {} imports (taken as 0): {}", unknown.len(), unknown.iter().take(12).cloned().collect::<Vec<_>>().join(", "));
        }
        l.push(format!("const unsigned short rt_thunk_pops[] = {{{}}};", vals.join(", ")));
    } else {
        l.push("const unsigned short rt_thunk_pops[] = {0};".into());
    }
    l.push("const FnEntry rt_fn_table[] = {".into());
    l.extend(roots.iter().map(|r| format!("    {{0x{r:x}ull, F_{r:x}}},")));
    l.push("    {0, NULL}};".into());
    l.push(format!("const size_t rt_fn_count = {};", roots.len()));
    // 32-bit: bytes pushed for each indirect call, by return address, for runtime stand-ins called through vtables
    let sites: Vec<String> = call_args.iter().map(|(a, n)| format!("0x{a:x}, {n}")).collect();
    l.push(format!("const uint32_t rt_call_args[] = {{{}}};", if sites.is_empty() { "0, 0".into() } else { sites.join(", ") }));
    l.push(format!("const size_t rt_call_arg_count = {};", call_args.len()));
    l.push(format!("const uint32_t rt_steam_appid = {};", steam_appid(&exe)));
    write_if_changed(&gen.join("meta.c"), &(l.join("\n") + "\n"));
    let mut keep: HashSet<String> = (0..ntasks).map(|i| format!("funcs_{i:04}.c")).collect();
    keep.extend(["meta.c".to_string(), "funcs.h".to_string()]);
    for e in std::fs::read_dir(&gen).into_iter().flatten().flatten() {
        if !keep.contains(&e.file_name().to_string_lossy().into_owned()) {
            let _ = std::fs::remove_file(e.path());
        }
    }

    // the coverage report, most common unsupported instructions first
    let mut by_count: Vec<(&String, &u64)> = unsupported.iter().collect();
    by_count.sort_by(|a, b| b.1.cmp(a.1).then(a.0.cmp(b.0)));
    let modules: Vec<String> = lk.modules.iter().map(|m| format!("{}: {}", json!(m.name), json!(m.path.to_string_lossy()))).collect();
    let uns: Vec<String> = by_count.iter().map(|(k, v)| format!("{}: {v}", json!(k))).collect();
    let ex: Vec<String> = by_count.iter().filter_map(|(k, _)| where_.get(*k).map(|v| format!("{}: {}", json!(k), v))).collect();
    let report = format!(
        "{{\"exe\": {}, \"modules\": {{{}}}, \"functions\": {}, \"imports\": {}, \"unsupported_total\": {bad}, \"unsupported\": {{{}}}, \"unsupported_examples\": {{{}}}, \"missing_imports\": {}, \"seconds\": {:.1}}}",
        json!(exe.to_string_lossy()),
        modules.join(", "),
        roots.len(),
        lk.modules.iter().map(|m| m.pe.imports.len()).sum::<usize>(),
        uns.join(", "),
        ex.join(", "),
        json!(lk.missing),
        t0.elapsed().as_secs_f64()
    );
    let pretty = serde_json::from_str::<Value>(&report).map(|_| report.clone()).unwrap_or_else(|e| die(&format!("report: {e}")));
    std::fs::write(out.join("coverage.json"), &pretty).unwrap_or_else(|e| die(&format!("coverage.json: {e}")));
    println!("@@result {report}");
}
