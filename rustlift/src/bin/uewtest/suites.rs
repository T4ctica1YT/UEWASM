//! The test suites that build a program, recompile it and compare it with the original.
use crate::cdp::{self, Browser};
use crate::*;
use serde_json::json;
use std::path::Path;
use std::process::Command;
use std::time::{Duration, Instant};

const T600: Duration = Duration::from_secs(600);

fn clean(s: &str) -> String {
    s.replace('\r', "")
}

/// every tests/*.c(pp): original, native recompile and WebAssembly build print exactly the same thing
pub fn run_tests(args: &[String]) -> bool {
    let mut names = vec![];
    let (mut no_wasm, mut x86, mut mem64, mut threads) = (false, false, false, s("coop"));
    let mut it = args.iter();
    while let Some(a) = it.next() {
        match a.as_str() {
            "--no-wasm" => no_wasm = true,
            "--x86" => x86 = true,
            "--mem64" => mem64 = true,
            "--threads" => threads = it.next().cloned().unwrap_or_else(|| die("--threads coop|real")),
            n => names.push(n.to_string()),
        }
    }
    let env = msvc(if x86 { "x86" } else { "x64" });
    let mut srcs: Vec<std::path::PathBuf> = std::fs::read_dir(tests_dir()).unwrap().flatten().map(|e| e.path())
        .filter(|p| p.is_file() && matches!(p.extension().and_then(|e| e.to_str()), Some("c" | "cpp")))
        .filter(|p| names.is_empty() || names.iter().any(|n| p.file_stem().map_or(false, |s| s == n.as_str()))).collect();
    srcs.sort();
    let mut failed = 0;
    for src in &srcs {
        let stem = src.file_stem().unwrap().to_string_lossy().to_string();
        let dir = bin_dir(&format!("{stem}{}", if x86 { "32" } else { "" }));
        let exe = dir.join(format!("{stem}.exe"));
        // 32-bit builds link the CRT statically, as most 32-bit games ship, unless the test asks for the DLL
        // ("uewasm-test: /MD" in the source: it tests the runtime's own CRT functions)
        let dll_crt = std::fs::read_to_string(src).unwrap_or_default().contains("uewasm-test: /MD");
        let mut flags: Vec<String> = if src.extension().unwrap() == "cpp" { vec![s("/O2"), s("/MD"), s("/GS"), s("/EHsc"), s("/std:c++17")] } else { vec![s("/O2"), s("/MD"), s("/GS")] };
        if x86 && !dll_crt {
            flags = flags.into_iter().map(|f| f.replace("/MD", "/MT")).collect();
        }
        if !exe.exists() || util::mtime(&exe) < util::mtime(src) {
            let mut a = vec![s("/nologo")];
            a.extend(flags);
            a.extend([format!("/Fe:{}", exe.display()), format!("/Fo:{}\\", dir.display()), util::ps(src)]);
            cl(&env, &a, &dir);
        }
        let out = dir.join("recomp");
        recompile(&exe, &out, if mem64 { 5120 } else { 256 }, &[]);
        let nat = native(&out, mem64);
        let saved = dir.join("Saved");
        let _ = std::fs::remove_dir_all(&saved);
        let r = run(Command::new(&exe).current_dir(&dir), T600);
        let _ = std::fs::remove_dir_all(&saved);
        let save = out.join("save");
        let _ = std::fs::remove_dir_all(&save);
        std::fs::create_dir_all(&save).unwrap();
        let args = [s("--image"), util::ps(&out.join("image.bin")), s("--root"), util::ps(&dir), s("--exe"), format!("{stem}.exe"), s("--save"), util::ps(&save)];
        let mut got = vec![("native", run(Command::new(&nat).args(&args), T600))];
        if !no_wasm {
            wasm(&out, &build::Wasm { node_fs: true, threads: threads.clone(), mem64, ..Default::default() });
            let (n, f) = node();
            got.push(("wasm", run(Command::new(n).args(f).arg("game.js").args(&args).current_dir(out.join("web")), T600)));
        }
        let mut ok = true;
        if saved.exists() {
            ok = false;
            println!("  {}: recompiled build wrote into the install folder", src.file_name().unwrap().to_string_lossy());
        }
        for (name, o) in &got {
            if clean(&o.stdout) != clean(&r.stdout) || o.code != r.code {
                ok = false;
                let (ol, rl): (Vec<&str>, Vec<&str>) = (o.stdout.lines().collect(), r.stdout.lines().collect());
                let diff = (0..ol.len().max(rl.len())).find(|&i| i >= ol.len() || i >= rl.len() || ol[i].trim_end_matches('\r') != rl[i].trim_end_matches('\r'));
                println!("  {} [{name}] exit {} vs {}; first diff at line {}:", src.file_name().unwrap().to_string_lossy(), o.code, r.code,
                         diff.map_or(s("None"), |d| d.to_string()));
                if let Some(d) = diff {
                    println!("    want: {}", rl.get(d).copied().unwrap_or("<eof>"));
                    println!("    got:  {}", ol.get(d).copied().unwrap_or("<eof>"));
                }
                if !o.stderr.trim().is_empty() {
                    let e: String = o.stderr.trim().replace('\n', "\n            ").chars().take(1500).collect();
                    println!("    stderr: {e}");
                }
            }
        }
        println!("{} {} ({})", if ok { "PASS" } else { "FAIL" }, src.file_name().unwrap().to_string_lossy(), got.iter().map(|g| g.0).collect::<Vec<_>>().join(", "));
        failed += (!ok) as u32;
    }
    failed == 0
}

fn libcrypto() -> Option<std::path::PathBuf> {
    if let Ok(p) = std::env::var("UEWASM_LIBCRYPTO") {
        return Some(p.into());
    }
    let mut roots = vec![];
    if let Ok(l) = std::env::var("LOCALAPPDATA") {
        roots.push(Path::new(&l).join("Programs/Python"));
    }
    for k in ["ProgramFiles", "ProgramFiles(x86)"] {
        if let Ok(p) = std::env::var(k) {
            roots.push(p.into());
        }
    }
    for r in roots {
        let Ok(rd) = std::fs::read_dir(&r) else { continue };
        let mut v: Vec<_> = rd.flatten().map(|e| e.path()).filter(|p| p.file_name().map_or(false, |n| n.to_string_lossy().starts_with("Python3"))).collect();
        v.sort();
        if let Some(p) = v.iter().rev().map(|p| p.join("DLLs/libcrypto-3.dll")).find(|p| p.exists()) {
            return Some(p);
        }
    }
    None
}

/// OpenSSL's assembly AES recompiled (libcrypto-3.dll from a Python install, UEWASM_LIBCRYPTO overrides), on the SSSE3
/// path and the integer-table path (no AES-NI, as in the runtime)
pub fn aes() -> bool {
    let out = bin_dir("aes");
    let Some(dll) = libcrypto() else {
        println!("SKIP: no libcrypto-3.dll (set UEWASM_LIBCRYPTO)");
        return true;
    };
    std::fs::copy(&dll, out.join("libcrypto-3.dll")).unwrap_or_else(|e| die(e));
    let env = msvc("x64");
    let exe = out.join("aes.exe");
    cl(&env, &[s("/nologo"), s("/O2"), s("/MD"), format!("/Fe:{}", exe.display()), format!("/Fo:{}\\", out.display()), util::ps(&tests_dir().join("aes/aes.c"))], &out);
    let rec = out.join("recomp");
    recompile(&exe, &rec, 256, &[out.join("libcrypto-3.dll")]);
    let nat = native(&rec, false);
    // OPENSSL_ia32cap masks: no AES-NI and no PCLMULQDQ (vpaes, 4-bit GHASH); also no SSSE3 (table AES)
    let mut ok = true;
    for (name, cap) in [("SSSE3 (vpaes)", "~0x200000200000000:0"), ("integer tables", "~0x200020200000000:0")] {
        let want: Vec<String> = run(Command::new(&exe).arg(cap).current_dir(&out), T600).stdout.split('\n').map(|l| l.trim().to_string()).filter(|l| !l.is_empty()).collect();
        let got: Vec<String> = run(Command::new(&nat).arg("--image").arg(rec.join("image.bin")).arg("--root").arg(&out).args(["--exe", "aes.exe", cap]), T600)
            .stdout.split('\n').filter(|l| !l.trim().is_empty() && !l.starts_with('[')).map(|l| l.trim().to_string()).collect();
        let same = got == want;
        ok &= same;
        println!("{name}: {}", if same { "MATCH" } else { "DIFF" });
        if !same {
            println!("  want {want:?}\n  got  {got:?}");
        }
    }
    println!("{}", if ok { "PASS" } else { "FAIL" });
    ok
}

fn launch(args: &[&str]) -> Browser {
    let a: Vec<String> = args.iter().map(|x| x.to_string()).collect();
    Browser::launch(&a).unwrap_or_else(|e| die(e))
}

/// real connections from a recompiled program through WISP, against uewserve's WISP server (needs the internet)
pub fn net() -> bool {
    let out = bin_dir("net");
    let env = msvc("x64");
    let exe = out.join("nettest.exe");
    cl(&env, &[s("/nologo"), s("/O1"), s("/MD"), format!("/Fe:{}", exe.display()), format!("/Fo:{}\\", out.display()), util::ps(&tests_dir().join("net/nettest.c"))], &out);
    let nat = run(&mut Command::new(&exe), Duration::from_secs(60)).stdout;
    println!("native:\n{nat}");
    let rec = out.join("recomp");
    recompile(&exe, &rec, 256, &[]);
    let web = wasm(&rec, &build::Wasm { game_root: Some(out.clone()), exe_rel: Some(s("nettest.exe")), wisp: true, ..Default::default() });
    let srv = cdp::Server::start(&uewserve(), &web, &[]).unwrap_or_else(|e| die(e));
    let mut b = launch(&["--mute-audio", "--enable-unsafe-webgpu"]);
    let mut pg = b.new_page(1280, 720).unwrap_or_else(|e| die(e));
    b.call(Some(&pg.sid.clone()), "Page.navigate", json!({"url": format!("http://localhost:{}/index.html?netdebug=1", srv.port)})).unwrap_or_else(|e| die(e));
    let t0 = Instant::now();
    while t0.elapsed() < Duration::from_secs(90) && !pg.console.iter().any(|l| text(l).trim() == "DONE") {
        let _ = pg.wait(&mut b, Duration::from_millis(250));
    }
    b.close();
    drop(srv);
    let lines: Vec<String> = pg.console.iter().map(|l| text(l).to_string()).collect();
    println!("web:\n{}", lines.iter().filter(|l| l.starts_with("OK") || l.starts_with("FAIL") || l.starts_with("[wisp]")).cloned().collect::<Vec<_>>().join("\n"));
    let want: Vec<&str> = nat.lines().filter(|l| l.starts_with("OK")).map(str::trim).collect();
    let got: Vec<&str> = lines.iter().filter(|l| l.starts_with("OK")).map(|l| l.trim()).collect();
    for (w, g) in want.iter().zip(&got) {
        if w != g {
            println!("differs:\n  native: {w}\n  web:    {g}");
        }
    }
    let ok = !want.is_empty() && want == got;
    println!("{}", if ok { "PASS" } else { "FAIL" });
    ok
}

/// a console line without the "[type] " the page wrapper adds
pub fn text(l: &str) -> &str {
    match l.split_once("] ") {
        Some((t, rest)) if t.starts_with('[') && !t.contains(' ') => rest,
        _ => l,
    }
}

struct Rng(u64);

impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

fn make_game(game: &Path) {
    std::fs::create_dir_all(game.join("Content/Sub")).unwrap();
    std::fs::create_dir_all(game.join("Binaries")).unwrap();
    let mut r = Rng(0x9E3779B97F4A7C15);
    let big: Vec<u8> = (0..3_100_000).map(|_| r.next() as u8).collect();
    std::fs::write(game.join("Content/big.bin"), big).unwrap();
    std::fs::write(game.join("Content/Sub/small.txt"), "hello from the game folder\n").unwrap();
    std::fs::write(game.join("Content/notes.txt"), "x").unwrap();
    let words = ["neighbor", "basement", "key", "door", "window", "crow", "toy", "glue"];
    let w: Vec<&str> = (0..400_000).map(|_| words[(r.next() % 8) as usize]).collect();
    std::fs::write(game.join("Content/words.txt"), w.join(" ")).unwrap();
    let mut dll = b"MZ".to_vec();
    dll.extend([0u8; 1000]);
    std::fs::write(game.join("Binaries/helper.dll"), dll).unwrap();
}

/// one page load per query (each after the last, same browser storage); the console lines of all of them
fn run_browser(web: &Path, ranges: bool, queries: &[&str]) -> Vec<String> {
    let srv = cdp::Server::start(&uewserve(), web, if ranges { &[] } else { &["--no-ranges"] }).unwrap_or_else(|e| die(e));
    let mut b = launch(&["--mute-audio"]);
    let mut page = b.new_page(1280, 720).unwrap_or_else(|e| die(e));
    let mut out: Vec<String> = vec![];
    for q in queries {
        let n = out.len();
        let sid = page.sid.clone();
        let _ = b.call(Some(&sid), "Page.navigate", json!({"url": format!("http://localhost:{}/index.html{q}", srv.port)}));
        let t0 = Instant::now();
        let mut seen = 0;
        let collect = |page: &mut cdp::Page, out: &mut Vec<String>, seen: &mut usize| {
            for l in &page.console[*seen..] {
                out.push(text(l).to_string());
            }
            *seen = page.console.len();
            for e in page.errors.drain(..) {
                out.push(format!("PAGEERROR {e}"));
            }
        };
        page.console.clear();
        while t0.elapsed() < Duration::from_secs(60) && !out[n..].iter().any(|l| l.trim() == "done" || l.starts_with("PAGEERROR")) {
            let _ = page.wait(&mut b, Duration::from_millis(250));
            collect(&mut page, &mut out, &mut seen);
        }
        let _ = page.wait(&mut b, Duration::from_secs(6));
        collect(&mut page, &mut out, &mut seen);
    }
    b.close();
    drop(srv);
    out
}

/// game files over HTTP: the recompiled native build matches the original, then the web build against a server with
/// and without range requests, raw and deflated
pub fn web() -> bool {
    let outd = bin_dir("web");
    let game = outd.join("game");
    make_game(&game);
    let env = msvc("x64");
    let exe = game.join("assets.exe");
    cl(&env, &[s("/nologo"), s("/O2"), s("/MD"), format!("/Fe:{}", exe.display()), format!("/Fo:{}\\", outd.display()), util::ps(&tests_dir().join("web/assets.c"))], &outd);
    let save = outd.join("save");
    let _ = std::fs::remove_dir_all(&save);
    let _ = std::fs::remove_dir_all(game.join("Saved"));
    let lines = |o: Out| -> Vec<String> { clean(&o.stdout).trim().lines().map(String::from).collect() };
    let mut want = vec![];
    for _ in 0..2 {
        want.extend(lines(run(Command::new(&exe).current_dir(&game), T600)));
    }
    let _ = std::fs::remove_dir_all(game.join("Saved"));
    let out = outd.join("recomp");
    recompile(&exe, &out, 256, &[]);
    let nat = native(&out, false);
    let mut got = vec![];
    for _ in 0..2 {
        got.extend(lines(run(Command::new(&nat).arg("--image").arg(out.join("image.bin")).arg("--root").arg(&game).arg("--save").arg(&save).args(["--exe", "assets.exe"]), T600)));
    }
    let mut ok = got == want;
    println!("native: {}", if ok { "MATCH" } else { "DIFF" });
    if !ok {
        println!("  want {want:?}\n  got  {got:?}");
    }
    let want: Vec<String> = want.into_iter().filter(|l| !l.trim().is_empty()).collect();
    for compress in [false, true] {
        let mut logs: Vec<String> = vec![];
        let web = build::build_wasm(&out, &build::Wasm { game_root: Some(game.clone()), exe_rel: Some(s("assets.exe")), part_mb: 1, compress, ..Default::default() },
                                    &mut |l: &str| logs.push(l.to_string()), &mut noprog()).unwrap_or_else(|e| die(e));
        println!("{}", logs.first().cloned().unwrap_or_default());
        for ranges in [true, false] {
            // a third load with ?clearsave=Saved starts without the save again, as the first run did
            let lines: Vec<String> = run_browser(&web, ranges, &["", "", "?clearsave=Saved"]).into_iter().filter(|l| !l.trim().is_empty() && !l.starts_with('[')).collect();
            let first = &want[..want.len() / 2];
            let third: Vec<String> = lines.iter().skip(want.len()).take(first.len()).cloned().collect();
            if third != first {
                println!("  ?clearsave=Saved: the save was still there: {third:?}");
                ok = false;
            }
            let head: Vec<String> = lines.iter().take(want.len()).cloned().collect();
            let same = head == want;
            println!("web ({}, {}): {}", if compress { "deflated" } else { "raw" }, if ranges { "range requests" } else { "whole parts" }, if same { "MATCH" } else { "DIFF" });
            if !same {
                println!("  want {want:?}\n  got  {:?}", lines.iter().take(40).collect::<Vec<_>>());
            }
            ok &= same;
        }
    }
    println!("{}", if ok { "PASS" } else { "FAIL" });
    ok
}

/// RGB at (x, y) of an RGB or RGBA PNG
fn png_pixels(data: &[u8]) -> impl Fn(usize, usize) -> (u8, u8, u8) {
    let (mut pos, mut idat, mut w, mut h, mut bpp) = (8usize, vec![], 0usize, 0usize, 3usize);
    while pos + 8 <= data.len() {
        let n = u32::from_be_bytes(data[pos..pos + 4].try_into().unwrap()) as usize;
        let body = &data[pos + 8..pos + 8 + n];
        match &data[pos + 4..pos + 8] {
            b"IHDR" => {
                w = u32::from_be_bytes(body[0..4].try_into().unwrap()) as usize;
                h = u32::from_be_bytes(body[4..8].try_into().unwrap()) as usize;
                bpp = if body[9] == 6 { 4 } else { 3 };
            }
            b"IDAT" => idat.extend_from_slice(body),
            _ => {}
        }
        pos += 12 + n;
    }
    let mut raw = vec![];
    let _ = flate2::read::ZlibDecoder::new(&idat[..]).read_to_end(&mut raw);
    let stride = w * bpp;
    let mut rows: Vec<Vec<u8>> = vec![];
    let mut prev = vec![0u8; stride];
    for y in 0..h {
        let at = y * (stride + 1);
        let f = raw[at];
        let mut line = raw[at + 1..at + 1 + stride].to_vec();
        for i in 0..stride {
            let a = if i >= bpp { line[i - bpp] as i32 } else { 0 };
            let b = prev[i] as i32;
            let c = if i >= bpp { prev[i - bpp] as i32 } else { 0 };
            let x = line[i] as i32;
            line[i] = (match f {
                1 => x + a,
                2 => x + b,
                3 => x + (a + b) / 2,
                4 => {
                    let p = a + b - c;
                    let (pa, pb, pc) = ((p - a).abs(), (p - b).abs(), (p - c).abs());
                    x + if pa <= pb && pa <= pc { a } else if pb <= pc { b } else { c }
                }
                _ => x,
            } & 255) as u8;
        }
        prev = line.clone();
        rows.push(line);
    }
    move |x: usize, y: usize| (rows[y][x * bpp], rows[y][x * bpp + 1], rows[y][x * bpp + 2])
}

/// the D3D11 triangle (or D3D9, or a volume texture): the recompiled build matches the original, then it renders in a
/// headless browser and its pixels are checked
pub fn gfx(args: &[String]) -> bool {
    let has = |k: &str| args.iter().any(|a| a == k);
    let (d3d9, volume, mem64, srgb, debug) = (has("d3d9"), has("volume"), has("mem64"), has("srgb"), has("debug"));
    let kits = Path::new(r"C:\Program Files (x86)\Windows Kits\10\bin");
    let mut fxcs: Vec<_> = std::fs::read_dir(kits).map(|r| r.flatten().map(|e| e.path().join("x64/fxc.exe")).filter(|p| p.exists()).collect()).unwrap_or_default();
    fxcs.sort();
    let fxc = fxcs.into_iter().next().unwrap_or_else(|| die("fxc.exe not found (Windows SDK)"));
    let out_dir = bin_dir(if d3d9 { "gfx9" } else if volume { "gfxvol" } else { "gfx" });
    let name = if d3d9 { "triangle9" } else if volume { "volume" } else { "triangle" };
    let env = msvc(if d3d9 { "x86" } else { "x64" });
    let sm = if d3d9 { "3_0" } else { "5_0" };
    let mut entries = vec![("VSMain", format!("vs_{sm}"), format!("{name}_vs.h")), ("PSMain", format!("ps_{sm}"), format!("{name}_ps.h"))];
    if volume {
        entries.extend([("GSMain", s("gs_5_0"), s("volume_gs.h")), ("VSShow", s("vs_5_0"), s("volume_showvs.h")), ("PSShow", s("ps_5_0"), s("volume_showps.h"))]);
    }
    let hlsl = tests_dir().join(format!("gfx/{name}.hlsl"));
    for (entry, prof, h) in &entries {
        let o = run(Command::new(&fxc).args(["/nologo", &format!("/T{prof}"), &format!("/E{entry}"), "/Vn", &format!("g_{entry}")])
            .arg(format!("/Fh{}", out_dir.join(h).display())).arg(&hlsl), T600);
        if o.code != 0 {
            die(format!("fxc failed:\n{}{}", o.stdout, o.stderr));
        }
    }
    let exe = out_dir.join(format!("{name}.exe"));
    cl(&env, &[s("/nologo"), s("/O2"), s(if d3d9 { "/MT" } else { "/MD" }), s("/EHsc"), format!("/I{}", out_dir.display()), format!("/Fe:{}", exe.display()),
               format!("/Fo:{}\\", out_dir.display()), util::ps(&tests_dir().join(format!("gfx/{name}.cpp")))], &out_dir);
    // srgb: the same scene on an sRGB back buffer, which must reach the screen gamma-encoded like on Windows
    let extra: Vec<&str> = if srgb { vec!["srgb"] } else { vec![] };
    let r = run(Command::new(&exe).args(&extra).current_dir(&out_dir), T600).stdout;
    let out = out_dir.join("recomp");
    recompile(&exe, &out, if mem64 { 5120 } else { 256 }, &[]);
    let nat = native(&out, mem64);
    let got = run(Command::new(&nat).arg("--image").arg(out.join("image.bin")).arg("--root").arg(&out_dir).arg("--exe").arg(format!("{name}.exe")).args(&extra), T600);
    println!("original: {}", r.trim().replace('\n', " | "));
    println!("native  : {} {}", got.stdout.trim().replace('\n', " | "), if clean(&got.stdout) == clean(&r) { "MATCH" } else { "DIFF" });
    if !got.stderr.trim().is_empty() {
        println!("    {}", got.stderr.trim().chars().take(800).collect::<String>());
    }
    let renderer = args.iter().find(|a| *a == "webgpu" || *a == "webgl2").cloned().unwrap_or(s("webgpu"));
    println!("renderer: {renderer}");
    let web = wasm(&out, &build::Wasm { renderer: renderer.clone(), mem64, ..Default::default() });
    std::fs::write(web.join("index.html"), format!(r#"<!doctype html><body style="margin:0;background:#000"><canvas id="canvas" width="640" height="360"></canvas><script>var Module={{canvas:document.getElementById("canvas"),arguments:["--exe","{name}.exe"{}],uewgDebug:location.search.includes("debug"),print:t=>console.log(t),printErr:t=>console.log(t)}};</script><script src="game.js"></script></body>"#,
        if srgb { r#","srgb""# } else { "" })).unwrap();
    let srv = cdp::Server::start(&uewserve(), &web, &[]).unwrap_or_else(|e| die(e));
    let mut b = launch(&["--mute-audio", "--ignore-gpu-blocklist", "--use-angle=d3d11", "--enable-unsafe-webgpu"]);
    let mut page = b.new_page(640, 360).unwrap_or_else(|e| die(e));
    let url = format!("http://localhost:{}/index.html{}", srv.port, if debug { "?debug" } else { "" });
    page.goto(&mut b, &url, Duration::from_secs(120)).unwrap_or_else(|e| die(e));
    let _ = page.wait(&mut b, Duration::from_millis(2500));
    let shot = page.screenshot(&mut b).unwrap_or_else(|e| die(e));
    b.close();
    drop(srv);
    std::fs::write(out_dir.join("web_frame.png"), &shot).unwrap();
    let px = png_pixels(&shot);
    let (center, corner) = (px(320, 200), px(10, 10));
    let mut logs: Vec<String> = page.console.iter().map(|l| text(l).to_string()).collect();
    logs.extend(page.errors.iter().map(|e| format!("PAGEERROR {e}")));
    let joined = logs.iter().filter(|l| !l.trim().is_empty()).cloned().collect::<Vec<_>>().join(" | ");
    println!("browser log: {}", joined.chars().take(if debug { 20000 } else { 900 }).collect::<String>());
    let near = |a: (u8, u8, u8), b: (u8, u8, u8), t: i32| (a.0 as i32 - b.0 as i32).abs() < t && (a.1 as i32 - b.1 as i32).abs() < t && (a.2 as i32 - b.2 as i32).abs() < t;
    if volume {
        // the middle of the volume: red and green about half, blue the slice (16 of 32)
        println!("center pixel {center:?} (expect about (128, 128, 131): uv and slice of the volume's middle)");
        let ok = near(center, (128, 128, 131), 24);
        println!("RENDER {}", if ok { "PASS" } else { "FAIL" });
        return ok;
    }
    // clear colour (0.05, 0.1, 0.3): stored as is, or sRGB-encoded on an sRGB back buffer
    let want = if srgb { (63, 89, 149) } else { (13, 25, 76) };
    println!("corner pixel {corner:?} (expect about {want:?} clear colour)");
    println!("center pixel {center:?} (expect a mix of red/green/blue from the triangle)");
    let ok = near(corner, want, 12) && center != corner && center.0.max(center.1).max(center.2) > 40;
    println!("RENDER {}", if ok { "PASS" } else { "FAIL" });
    ok
}

/// a web build of steamtest.exe twice against the relay or a WebWorks server: one page hosts, one joins, each in its
/// own browser context (so each signs in as its own player). Passes when the joining page prints DONE
pub fn steam_pair(args: &[String]) -> bool {
    let web = Path::new(args.first().map(String::as_str).unwrap_or_else(|| die("usage: uewtest steam-pair <web dir> [seconds]")));
    let limit = args.get(1).and_then(|x| x.parse::<f64>().ok()).unwrap_or(120.0);
    let srv = cdp::Server::start(&uewserve(), web, &[]).unwrap_or_else(|e| die(e));
    let base = format!("http://localhost:{}/index.html", srv.port);
    let mut b = launch(&["--mute-audio", "--enable-unsafe-webgpu", "--ignore-gpu-blocklist"]);
    let keep = |l: &str| ["OK", "FAIL", "DONE", "callback", "[steam]", "[webworks]", "rror"].iter().any(|k| l.contains(k));
    let mut pages: Vec<(String, cdp::Page, usize)> = vec![];
    let mut logs: HashMap<String, Vec<String>> = HashMap::new();
    let pump = |b: &mut Browser, pages: &mut Vec<(String, cdp::Page, usize)>, logs: &mut HashMap<String, Vec<String>>, d: Duration| {
        let _ = b.pump(d);
        for (role, pg, seen) in pages.iter_mut() {
            pg.absorb(b);
            for l in &pg.console[*seen..] {
                let t = text(l).to_string();
                if keep(&t) {
                    println!("[{role}] {t}");
                    logs.entry(role.clone()).or_default().push(t);
                }
            }
            *seen = pg.console.len();
        }
    };
    for role in ["host", "join"] {
        let pg = b.new_context_page(1280, 720).unwrap_or_else(|e| die(e));
        let _ = b.call(Some(&pg.sid.clone()), "Page.navigate", json!({"url": format!("{base}?arg={role}&steamdebug=1")}));
        pages.push((role.to_string(), pg, 0));
        if role == "host" {
            let t0 = Instant::now();
            while t0.elapsed() < Duration::from_secs(30) && !logs.get("host").map_or(false, |v| v.iter().any(|l| l.contains("lobby") && l.contains("created"))) {
                pump(&mut b, &mut pages, &mut logs, Duration::from_millis(500));
            }
        }
    }
    let t0 = Instant::now();
    let mut ok = false;
    while t0.elapsed().as_secs_f64() < limit {
        if logs.get("join").map_or(false, |v| v.iter().any(|l| l.contains("DONE"))) {
            ok = true;
            break;
        }
        if logs.values().flatten().any(|l| l.starts_with("FAIL")) {
            break;
        }
        pump(&mut b, &mut pages, &mut logs, Duration::from_millis(500));
    }
    pump(&mut b, &mut pages, &mut logs, Duration::from_millis(1500));
    b.close();
    drop(srv);
    println!("{}", if ok { "PASS" } else { "FAIL" });
    ok
}

/// every SIMD op in runtime/sse.h against its scalar reference: writes tests/sse/sse_test.c, builds it with emcc and
/// runs it in Node
pub fn sse() -> bool {
    let dir = tests_dir().join("sse");
    let src = std::fs::read_to_string(util::runtime_dir().join("sse.h")).unwrap_or_else(|e| die(e));
    std::fs::write(dir.join("sse_test.c"), crate::shaders::sse_test_c(&src)).unwrap();
    let em = build::emsdk_env().unwrap_or_else(|e| die(e));
    let o = run(Command::new(&em.emcc).args(["-O2", "-msimd128", "sse_test.c", "-o", "sse_test.js"]).envs(em.env.iter().map(|(k, v)| (k, v))).current_dir(&dir), T600);
    if o.code != 0 {
        println!("{}", util::tail(&o.stderr, 4000));
        return false;
    }
    let r = run(Command::new(&em.node).arg("sse_test.js").current_dir(&dir), T600);
    println!("{} {}", util::tail(&r.stdout, 8000), util::tail(&r.stderr, 2000));
    for f in ["sse_test.js", "sse_test.wasm"] {
        let _ = std::fs::remove_file(dir.join(f));
    }
    r.code == 0
}
