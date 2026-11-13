// uewlift: the fast lifter. build.py (recompile with lifter="rust") writes a job file describing each module, runs this,
// and reads back the roots and the unsupported-instruction counts; the C it writes matches the Python lifter's.
mod func;
mod prog;
mod sse;
mod x87;

use rayon::prelude::*;
use regex::Regex;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};

use func::{Ctx, Func};
use prog::{Module, Program};

const XFL_DECL: &str = "#ifdef _MSC_VER\nextern __declspec(thread) uint64_t rt_xfl;\n#else\nextern _Thread_local uint64_t rt_xfl;\n#endif";

fn arr_u64(v: &Value) -> Vec<u64> {
    v.as_array().map(|a| a.iter().map(|x| x.as_u64().unwrap()).collect()).unwrap_or_default()
}

fn pairs(v: &Value) -> Vec<(u64, u64)> {
    v.as_array().map(|a| a.iter().map(|p| (p[0].as_u64().unwrap(), p[1].as_u64().unwrap())).collect()).unwrap_or_default()
}

struct Rx {
    r_use: Regex,
    r_write: Regex,
    x_ref: Regex,
    x_write: Regex,
    f_ref: Regex,
}

/// SPILL/FILL/FILLV expansion and XMM locals (build.py regs_local)
fn regs_local(rx: &Rx, body: Vec<String>, bits: u32) -> (Vec<String>, Vec<u32>) {
    const VOLATILE: [u32; 8] = [0, 1, 2, 4, 8, 9, 10, 11];
    let text = body.join("\n");
    let used: BTreeSet<u32> = rx.r_use.captures_iter(&text).map(|c| c[1].parse().unwrap()).collect();
    let wrote: BTreeSet<u32> = rx.r_write.captures_iter(&text).map(|c| c[1].parse().unwrap()).collect();
    let mut spill: String = wrote.iter().map(|n| format!("c->r[{n}] = r{n}_; ")).collect();
    let mut fill: String = used.iter().map(|n| format!("r{n}_ = c->r[{n}]; ")).collect();
    let mut fillv: String = used.iter().filter(|n| VOLATILE.contains(n) || bits == 32).map(|n| format!("r{n}_ = c->r[{n}]; ")).collect();
    let mut xused: Vec<u32> = vec![];
    let mut body = body;
    if !text.contains("rt_fxsave(") && !text.contains("rt_fxrstor(") && !text.contains("setjmp(") {
        let xs: BTreeSet<u32> = rx.x_ref.captures_iter(&text).map(|c| c[1].parse().unwrap()).collect();
        let xw: BTreeSet<u32> = rx.x_write.captures_iter(&text)
            .map(|c| c.get(1).or(c.get(2)).unwrap().as_str().parse().unwrap()).collect();
        xused = xs.iter().copied().collect();
        spill += &xw.iter().map(|n| format!("c->x[{n}] = x{n}_; ")).collect::<String>();
        fill += &xs.iter().map(|n| format!("x{n}_ = c->x[{n}]; ")).collect::<String>();
        fillv += &xs.iter().filter(|n| **n < 6 || bits == 32).map(|n| format!("x{n}_ = c->x[{n}]; ")).collect::<String>();
        body = body.into_iter().map(|l| rx.x_ref.replace_all(&l, "x${1}_").into_owned()).collect();
    }
    let out = body.into_iter().map(|l| l.replace("SPILL(); ", &spill).replace("FILLV();", &fillv).replace("FILL();", &fill)).collect();
    (out, xused)
}

/// written the way Python's write_text does (CRLF on Windows), so switching lifters doesn't touch unchanged files
fn write_if_changed(path: &Path, text: &str) {
    let text = if cfg!(windows) { text.replace('\n', "\r\n") } else { text.to_string() };
    if let Ok(old) = std::fs::read_to_string(path) {
        if old == text {
            return;
        }
    }
    std::fs::write(path, text).expect("write");
}

struct Chunk {
    prog: usize,
    roots: Vec<u64>,
    out: PathBuf,
}

fn main() {
    let job_path = std::env::args().nth(1).expect("usage: uewlift <job.json>");
    let job: Value = serde_json::from_str(&std::fs::read_to_string(&job_path).expect("job")).expect("job json");
    let gen = PathBuf::from(job["gen"].as_str().unwrap());
    let chunk = job["chunk"].as_u64().unwrap_or(400) as usize;
    let lo = job["lo"].as_u64().unwrap();
    let span: &'static [u8] = Box::leak(std::fs::read(job["image"].as_str().unwrap()).expect("image").into_boxed_slice());
    let sse_h = std::fs::read_to_string(Path::new(job["runtime"].as_str().unwrap()).join("sse.h")).expect("sse.h");
    let generic: HashSet<String> = Regex::new(r"(?m)^XFN\((\w+)\)").unwrap().captures_iter(&sse_h).map(|c| c[1].to_string()).collect();

    let t0 = std::time::Instant::now();
    let modules: Vec<Module> = job["modules"].as_array().unwrap().iter().map(|m| {
        let base = m["base"].as_u64().unwrap();
        let size = m["image_size"].as_u64().unwrap();
        let s = (base - lo) as usize;
        let e = (s + size as usize).min(span.len());
        Module {
            base,
            bits: m["bits"].as_u64().unwrap() as u32,
            img: &span[s..e],
            image_size: size,
            entry_rva: m["entry_rva"].as_u64().unwrap(),
            sections: m["sections"].as_array().unwrap().iter()
                .map(|x| (x[0].as_u64().unwrap(), x[1].as_u64().unwrap(), x[2].as_bool().unwrap())).collect(),
            functions: pairs(&m["functions"]),
            unwind: arr_u64(&m["unwind"]).into_iter().collect(),
            family: pairs(&m["family"]).into_iter().collect(),
            exports: arr_u64(&m["exports"]),
            tls_callbacks: arr_u64(&m["tls_callbacks"]),
            relocs: arr_u64(&m["relocs"]),
            sj_slots: arr_u64(&m["sj_slots"]).into_iter().collect(),
        }
    }).collect();
    let progs: Vec<Program> = modules.into_par_iter().map(|m| {
        let mut p = Program::new(m);
        p.discover();
        p
    }).collect();
    let t_disc = t0.elapsed().as_secs_f64();

    let mut all_roots: HashSet<u64> = HashSet::new();
    for p in &progs {
        all_roots.extend(p.blocks.keys().copied());
    }
    let mut chunks: Vec<Chunk> = vec![];
    for (pi, p) in progs.iter().enumerate() {
        let mut mroots: Vec<u64> = p.blocks.keys().copied().collect();
        mroots.sort();
        for ch in mroots.chunks(chunk) {
            let n = chunks.len();
            chunks.push(Chunk { prog: pi, roots: ch.to_vec(), out: gen.join(format!("funcs_{:04}.c", n)) });
        }
    }
    let rx = Rx {
        r_use: Regex::new(r"\bR\((\d+)\)").unwrap(),
        r_write: Regex::new(r"\bR\((\d+)\)\s*(?:[-+&|^]|<<|>>)?=([^=]|$)").unwrap(),
        x_ref: Regex::new(r"c->x\[(\d+)\]").unwrap(),
        x_write: Regex::new(r"&c->x\[(\d+)\]|c->x\[(\d+)\](?:\.\w+\[\d+\])?\s*(?:[-+*/&|^]|<<|>>)?=([^=]|$)").unwrap(),
        f_ref: Regex::new(r"\bF_([0-9a-f]+)\b").unwrap(),
    };
    let results: Vec<(BTreeMap<String, u64>, BTreeMap<String, Vec<String>>, u64)> = chunks.par_iter().map(|ch| {
        let p = &progs[ch.prog];
        let ctx = Ctx { p, roots: &all_roots, generic: &generic };
        let mut cache = HashMap::new();
        let mut fl_cache = HashMap::new();
        let mut unsupported: Vec<(String, u64)> = vec![];
        let mut out: Vec<String> = vec!["#define LIFTED 1".into(), "#include \"rt.h\"".into(), "#include \"funcs.h\"".into(), String::new()];
        let mut bad = 0u64;
        for &r in &ch.roots {
            let labels = &p.blocks[&r];
            let (body, entry) = {
                let mut f = Func { c: &ctx, labels, out: vec![], ins: Default::default(), cas: false, cache: &mut cache,
                                   fl_cache: &mut fl_cache, unsupported: &mut unsupported };
                bad += f.lift() as u64;
                let entry = if p.func_start(r) || !f.flags_live(r) { "2" } else { "rt_xfl" };
                (std::mem::take(&mut f.out), entry)
            };
            out.push(format!("void F_{:x}(CPU* __restrict c) {{", r));
            let (mut body, xused) = regs_local(&rx, body, p.m.bits);
            let used: BTreeSet<u32> = body.iter().flat_map(|l| rx.r_use.captures_iter(l).map(|c| c[1].parse::<u32>().unwrap()).collect::<Vec<_>>()).collect();
            let mut decl = if used.is_empty() { String::new() } else {
                format!(" uint64_t {};", used.iter().map(|n| format!("r{n}_ = c->r[{n}]")).collect::<Vec<_>>().join(", "))
            };
            if !xused.is_empty() {
                decl += &format!(" XMM {};", xused.iter().map(|n| format!("x{n}_ = c->x[{n}]")).collect::<Vec<_>>().join(", "));
            }
            out.push(format!("    uint8_t* const mem_ = MEM; Flags F = {{0}}; F.fop = F_RAW; F.rawfl = {entry}; F.fsz = 64;{decl}"));
            if body.iter().any(|l| l.contains("setjmp(")) {
                body = body.into_iter().map(|l| l.replace("TAIL ", "")).collect();
            }
            out.extend(body);
            out.push("}".into());
            out.push(String::new());
        }
        let mut text = out.join("\n");
        if text.contains("rt_sj_slot(") || text.contains("rt_sj_slot32(") {
            text = text.replacen("#include \"funcs.h\"", "#include \"funcs.h\"\n#include <setjmp.h>\njmp_buf* rt_sj_slot(CPU* c, uint64_t jb, uint64_t frame, uint64_t rip);\njmp_buf* rt_sj_slot32(CPU* c, uint64_t jb, uint64_t rip);", 1);
        }
        if text.contains("rt_xfl") {
            text = text.replacen("#include \"funcs.h\"", &format!("#include \"funcs.h\"\n{XFL_DECL}"), 1);
        }
        let mut called: Vec<u64> = rx.f_ref.captures_iter(&text).map(|c| u64::from_str_radix(&c[1], 16).unwrap()).collect::<BTreeSet<_>>().into_iter().collect();
        called.sort_by_key(|v| format!("{:x}", v));
        let decls: String = called.iter().map(|n| format!("void F_{:x}(CPU* __restrict);\n", n)).collect();
        text = text.replacen("#include \"funcs.h\"", decls.trim_end_matches('\n'), 1);
        write_if_changed(&ch.out, &text);
        let mut u: BTreeMap<String, u64> = BTreeMap::new();
        let mut at: BTreeMap<String, Vec<String>> = BTreeMap::new();
        for (m, a) in unsupported {
            *u.entry(m.clone()).or_default() += 1;
            let e = at.entry(m).or_default();
            if e.len() < 5 {
                e.push(format!("0x{:x}", a));
            }
        }
        (u, at, bad)
    }).collect();
    let t_lift = t0.elapsed().as_secs_f64() - t_disc;

    let mods: Vec<Value> = progs.iter().map(|p| {
        let mut r: Vec<u64> = p.blocks.keys().copied().collect();
        r.sort();
        json!({ "roots": r })
    }).collect();
    let ch: Vec<Value> = results.into_iter().map(|(u, at, bad)| json!({ "u": u, "at": at, "bad": bad })).collect();
    let out = json!({ "modules": mods, "chunks": ch, "files": chunks.len(), "seconds": [t_disc, t_lift] });
    println!("{}", out);
}
