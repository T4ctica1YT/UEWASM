//! UEWASM's tests: each builds a program with MSVC, recompiles it and compares the original, the native recompile and
//! the web build (Node, or headless Edge/Chrome over the DevTools protocol).
//!
//!     uewtest run [names] [--no-wasm] [--x86] [--threads coop|real] [--mem64]
//!     uewtest isa [--wasm] [--filter X] [--trials N]
//!     uewtest aes | net | web | sse
//!     uewtest gfx [webgpu|webgl2] [d3d9] [volume] [srgb] [mem64] [debug]
//!     uewtest steam-pair <web dir> [seconds]
//!     uewtest extract-dxbc <game> <out> | extract-d3d9 <game> <out> | opcode-map <dir> [n]
//!     uewtest wgsl-check <dir> [n] | webgl-check <dir> [n]
#![allow(dead_code)]
#[path = "../uewasm-cli/build.rs"]
mod build;
#[path = "../uewasm-cli/cdp.rs"]
mod cdp;
#[path = "../uewasm-cli/deps.rs"]
mod deps;
#[path = "../uewasm-cli/media.rs"]
mod media;
#[path = "../uewasm-cli/res.rs"]
mod res;
#[path = "../uewasm-cli/upak.rs"]
mod upak;
#[path = "../uewasm-cli/util.rs"]
mod util;
#[path = "../uewasm-cli/zip.rs"]
mod zip;

mod isa;
mod shaders;
mod suites;

use std::collections::HashMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

pub fn die(m: impl std::fmt::Display) -> ! {
    eprintln!("{m}");
    std::process::exit(1)
}

pub fn tests_dir() -> PathBuf {
    util::home().join("tests")
}

pub fn bin_dir(name: &str) -> PathBuf {
    let d = tests_dir().join("bin").join(name);
    std::fs::create_dir_all(&d).unwrap_or_else(|e| die(e));
    d
}

pub fn nolog() -> impl FnMut(&str) {
    |_: &str| {}
}

pub fn noprog() -> impl FnMut(&str, f64, f64) {
    |_: &str, _: f64, _: f64| {}
}

pub fn msvc(arch: &str) -> HashMap<String, String> {
    build::msvc_env(arch).unwrap_or_else(|e| die(e))
}

pub fn tool(env: &HashMap<String, String>, name: &str) -> PathBuf {
    let path = env.get("PATH").cloned().unwrap_or_default();
    std::env::split_paths(&path).map(|d| d.join(format!("{name}.exe"))).find(|p| p.is_file()).unwrap_or_else(|| die(format!("{name} not found")))
}

pub struct Out {
    pub code: i32,
    pub stdout: String,
    pub stderr: String,
}

/// a program's exit code and output; killed after the timeout
pub fn run(c: &mut Command, timeout: Duration) -> Out {
    let mut child = c.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().unwrap_or_else(|e| die(format!("{:?}: {e}", c.get_program())));
    let (mut so, mut se) = (child.stdout.take().unwrap(), child.stderr.take().unwrap());
    let a = std::thread::spawn(move || {
        let mut v = vec![];
        let _ = so.read_to_end(&mut v);
        v
    });
    let b = std::thread::spawn(move || {
        let mut v = vec![];
        let _ = se.read_to_end(&mut v);
        v
    });
    let t0 = Instant::now();
    let code = loop {
        if let Ok(Some(s)) = child.try_wait() {
            break s.code().unwrap_or(-1);
        }
        if t0.elapsed() > timeout {
            let _ = child.kill();
            let _ = child.wait();
            break -1;
        }
        std::thread::sleep(Duration::from_millis(10));
    };
    let lossy = |v: Vec<u8>| String::from_utf8_lossy(&v).into_owned();
    Out { code, stdout: lossy(a.join().unwrap_or_default()), stderr: lossy(b.join().unwrap_or_default()) }
}

/// MSVC with the given arguments in dir; exits with its output on failure
pub fn cl(env: &HashMap<String, String>, args: &[String], dir: &Path) {
    let o = run(Command::new(tool(env, "cl")).args(args).env_clear().envs(env).current_dir(dir), Duration::from_secs(600));
    if o.code != 0 {
        die(format!("cl failed:\n{}", util::tail(&o.stdout, 2000)));
    }
}

pub fn recompile(exe: &Path, out: &Path, mem_mb: u64, extra: &[PathBuf]) {
    build::recompile(exe, out, mem_mb, &[], extra, true, false, &mut nolog(), &mut noprog()).unwrap_or_else(|e| die(e));
}

pub fn native(out: &Path, mem64: bool) -> PathBuf {
    build::build_native(out, false, mem64, &mut nolog(), &mut noprog()).unwrap_or_else(|e| die(e))
}

pub fn wasm(out: &Path, o: &build::Wasm) -> PathBuf {
    build::build_wasm(out, o, &mut nolog(), &mut noprog()).unwrap_or_else(|e| die(e))
}

pub fn node() -> (PathBuf, Vec<String>) {
    let em = build::emsdk_env().unwrap_or_else(|e| die(e));
    let flags = build::jspi_flag(&em.node);
    (em.node, flags)
}

pub fn uewserve() -> PathBuf {
    util::rust_tool("uewserve", &mut |l| println!("{l}")).unwrap_or_else(|e| die(e))
}

pub fn s(x: &str) -> String {
    x.to_string()
}

fn main() {
    let a: Vec<String> = std::env::args().skip(1).collect();
    let Some(cmd) = a.first() else {
        die("usage: uewtest run|isa|aes|net|web|gfx|steam-pair|sse|extract-dxbc|extract-d3d9|opcode-map|wgsl-check|webgl-check [...]");
    };
    let rest = &a[1..];
    let ok = match cmd.as_str() {
        "run" => suites::run_tests(rest),
        "isa" => isa::main(rest),
        "aes" => suites::aes(),
        "net" => suites::net(),
        "web" => suites::web(),
        "gfx" => suites::gfx(rest),
        "steam-pair" => suites::steam_pair(rest),
        "sse" => suites::sse(),
        "extract-dxbc" => shaders::extract_dxbc(rest),
        "extract-d3d9" => shaders::extract_d3d9(rest),
        "opcode-map" => shaders::opcode_map(rest),
        "wgsl-check" => shaders::wgsl_check(rest),
        "webgl-check" => shaders::webgl_check(rest),
        c => die(format!("no test called {c}")),
    };
    std::process::exit(if ok { 0 } else { 1 });
}
