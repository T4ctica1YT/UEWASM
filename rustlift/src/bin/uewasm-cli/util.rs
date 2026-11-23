//! Shared pieces: where UEWASM's files are, running the other tools, Windows process settings, SHA-1.
use serde_json::Value;
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::sync::OnceLock;
use std::time::SystemTime;

pub type Res<T> = Result<T, String>;
pub const EXE: &str = if cfg!(windows) { ".exe" } else { "" };

pub fn err<T>(m: impl Into<String>) -> Res<T> {
    Err(m.into())
}

/// the folder holding runtime/ (a checkout, or the unpacked release); UEWASM_HOME overrides
pub fn home() -> &'static Path {
    static H: OnceLock<PathBuf> = OnceLock::new();
    H.get_or_init(|| {
        if let Ok(h) = std::env::var("UEWASM_HOME") {
            return PathBuf::from(h);
        }
        let me = std::env::current_exe().unwrap_or_default();
        for d in me.ancestors().skip(1) {
            if d.join("runtime").join("rt.h").exists() {
                return d.to_path_buf();
            }
        }
        std::env::current_dir().unwrap_or_default()
    })
}

pub fn runtime_dir() -> PathBuf {
    home().join("runtime")
}

pub fn assets_dir() -> PathBuf {
    home().join("assets")
}

pub fn stdcall_cache() -> PathBuf {
    assets_dir().join("stdcall32.json")
}

pub fn settings_dir() -> PathBuf {
    let base = std::env::var("LOCALAPPDATA").or_else(|_| std::env::var("USERPROFILE")).unwrap_or_else(|_| ".".into());
    PathBuf::from(base).join("UEWASM")
}

pub fn mtime(p: &Path) -> SystemTime {
    std::fs::metadata(p).and_then(|m| m.modified()).unwrap_or(SystemTime::UNIX_EPOCH)
}

pub fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    let mut v: Vec<_> = rd.flatten().map(|e| e.path()).collect();
    v.sort();
    for p in v {
        if p.is_dir() {
            walk(&p, out);
        } else {
            out.push(p);
        }
    }
}

pub fn files_under(dir: &Path) -> Vec<PathBuf> {
    let mut v = vec![];
    walk(dir, &mut v);
    v
}

/// a program on PATH (with Windows' executable extensions)
pub fn which(name: &str) -> Option<PathBuf> {
    let path = std::env::var_os("PATH")?;
    let exts: Vec<String> = if cfg!(windows) && Path::new(name).extension().is_none() {
        std::env::var("PATHEXT").unwrap_or(".EXE;.BAT;.CMD".into()).split(';').map(|s| s.to_lowercase()).collect()
    } else {
        vec![String::new()]
    };
    for d in std::env::split_paths(&path) {
        for e in &exts {
            let p = d.join(format!("{name}{e}"));
            if p.is_file() {
                return Some(p);
            }
        }
    }
    None
}

pub fn cargo() -> Option<PathBuf> {
    which("cargo").or_else(|| {
        let p = PathBuf::from(std::env::var("USERPROFILE").or_else(|_| std::env::var("HOME")).unwrap_or_default()).join(".cargo/bin").join(format!("cargo{EXE}"));
        p.exists().then_some(p)
    })
}

pub const RUST_TOOLS: &[&str] = &["uewlift", "uewrecomp", "uewbuild", "uewpack", "uewserve"];

fn tool_dir() -> PathBuf {
    std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default()
}

fn newest_source() -> Option<SystemTime> {
    let rl = home().join("rustlift");
    if !rl.join("Cargo.toml").exists() {
        return None;
    }
    let mut v = files_under(&rl.join("src"));
    v.push(rl.join("Cargo.toml"));
    v.iter().map(|p| mtime(p)).max()
}

/// one of the Rust tools, beside this one; in a checkout it is rebuilt with cargo when its sources are newer
pub fn rust_tool(name: &str, log: &mut dyn FnMut(&str)) -> Res<PathBuf> {
    let exe = tool_dir().join(format!("{name}{EXE}"));
    let stale = newest_source().map_or(false, |src| !exe.exists() || mtime(&exe) < src);
    if exe.exists() && !stale {
        return Ok(exe);
    }
    let Some(cargo) = cargo() else {
        return if exe.exists() { Ok(exe) } else { err(format!("{name} isn't built and there's no Rust toolchain to build it (https://rustup.rs)")) };
    };
    log(&format!("building the Rust tools ({name})"));
    let bins: Vec<&str> = RUST_TOOLS.iter().flat_map(|t| ["--bin", t]).collect();
    let out = quiet(Command::new(cargo).args(["build", "--release", "--manifest-path"]).arg(home().join("rustlift/Cargo.toml")).args(bins))
        .output().map_err(|e| e.to_string())?;
    if !out.status.success() || !exe.exists() {
        let e = String::from_utf8_lossy(&out.stderr);
        return err(format!("could not build {name}:\n{}", tail(&e, 2000)));
    }
    // cargo leaves a tool it didn't need to relink untouched: mark them all current
    for t in RUST_TOOLS {
        let p = tool_dir().join(format!("{t}{EXE}"));
        if let Ok(f) = std::fs::File::options().append(true).open(&p) {
            let _ = f.set_modified(SystemTime::now());
        }
    }
    Ok(exe)
}

pub fn tail(s: &str, n: usize) -> &str {
    let mut i = s.len().saturating_sub(n);
    while !s.is_char_boundary(i) {
        i += 1;
    }
    s[i..].trim()
}

// ---- process priority and console windows

static LOW: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

pub fn set_low_priority(low: bool) {
    LOW.store(low, std::sync::atomic::Ordering::Relaxed);
}

pub fn low_priority() -> bool {
    LOW.load(std::sync::atomic::Ordering::Relaxed)
}

const BELOW_NORMAL: u32 = 0x4000;
const IDLE: u32 = 0x40;
const NO_WINDOW: u32 = 0x0800_0000;

#[cfg(windows)]
#[link(name = "kernel32")]
extern "system" {
    fn GetCurrentProcess() -> isize;
    fn SetPriorityClass(h: isize, class: u32) -> i32;
    pub fn GlobalMemoryStatusEx(p: *mut MemStatus) -> i32;
    pub fn GetLogicalProcessorInformation(p: *mut u8, len: *mut u32) -> i32;
    pub fn LoadLibraryW(name: *const u16) -> isize;
    pub fn GetProcAddress(h: isize, name: *const u8) -> *const std::ffi::c_void;
}

#[repr(C)]
#[derive(Default)]
pub struct MemStatus {
    pub len: u32,
    pub load: u32,
    pub total_phys: u64,
    pub avail_phys: u64,
    pub rest: [u64; 5],
}

/// no console window, and the build's lowered priority
pub fn quiet(c: &mut Command) -> &mut Command {
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(NO_WINDOW | if low_priority() { IDLE } else { BELOW_NORMAL });
    }
    c
}

/// this process at the same lowered priority as the tool processes
pub fn lower_this_process() {
    #[cfg(windows)]
    unsafe {
        SetPriorityClass(GetCurrentProcess(), if low_priority() { IDLE } else { BELOW_NORMAL });
    }
}

// ---- the tools' line protocol

pub type Log<'a> = &'a mut dyn FnMut(&str);
pub type Progress<'a> = &'a mut dyn FnMut(&str, f64, f64);

/// runs a Rust tool: its "@@progress stage done total" lines go to progress, "@@result <json>" is returned, the rest
/// of its output goes to log
pub fn run_tool(name: &str, args: &[String], log: Log, progress: Progress) -> Res<Option<Value>> {
    let exe = rust_tool(name, log)?;
    let mut child = quiet(Command::new(&exe).args(args)).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| format!("{name}: {e}"))?;
    let mut se = child.stderr.take().unwrap();
    let errs = std::thread::spawn(move || {
        let mut s = String::new();
        let _ = se.read_to_string(&mut s);
        s
    });
    let mut result = None;
    for line in BufReader::new(child.stdout.take().unwrap()).split(b'\n').map_while(Result::ok) {
        let line = String::from_utf8_lossy(&line);
        let line = line.trim_end();
        if let Some(r) = line.strip_prefix("@@progress ") {
            let p: Vec<&str> = r.split_whitespace().collect();
            if p.len() == 3 {
                progress(&p[0].replace('_', " "), p[1].parse().unwrap_or(0.0), p[2].parse().unwrap_or(0.0));
            }
        } else if let Some(r) = line.strip_prefix("@@result ") {
            result = serde_json::from_str(r).ok();
        } else if !line.trim().is_empty() {
            log(line);
        }
    }
    let errs = errs.join().unwrap_or_default();
    let st = child.wait().map_err(|e| e.to_string())?;
    if !st.success() {
        let t = tail(&errs, 4000);
        return err(if t.is_empty() { format!("{name} {} failed", args.first().map(String::as_str).unwrap_or("")) } else { t.to_string() });
    }
    Ok(result)
}

/// a Rust tool given its work as a JSON job file
pub fn run_job(name: &str, cmd: &str, job: &Value, log: Log, progress: Progress) -> Res<Value> {
    let f = std::env::temp_dir().join(format!("uewjob-{}-{}.json", std::process::id(), unique()));
    std::fs::write(&f, job.to_string()).map_err(|e| e.to_string())?;
    let r = run_tool(name, &[cmd.to_string(), f.to_string_lossy().into()], log, progress);
    let _ = std::fs::remove_file(&f);
    Ok(r?.unwrap_or(Value::Object(Default::default())))
}

pub fn unique() -> u64 {
    static N: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
    let t = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0);
    t ^ N.fetch_add(1, std::sync::atomic::Ordering::Relaxed) << 48
}

pub fn ps(p: &Path) -> String {
    p.to_string_lossy().into_owned()
}

// ---- SHA-1 (pak index hashes, cache names)

pub struct Sha1 {
    h: [u32; 5],
    buf: Vec<u8>,
    len: u64,
}

impl Sha1 {
    pub fn new() -> Sha1 {
        Sha1 { h: [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0], buf: Vec::with_capacity(64), len: 0 }
    }

    pub fn update(&mut self, mut d: &[u8]) {
        self.len += d.len() as u64;
        if !self.buf.is_empty() {
            let n = (64 - self.buf.len()).min(d.len());
            self.buf.extend_from_slice(&d[..n]);
            d = &d[n..];
            if self.buf.len() == 64 {
                let b: [u8; 64] = self.buf[..].try_into().unwrap();
                self.block(&b);
                self.buf.clear();
            }
        }
        while d.len() >= 64 {
            self.block(d[..64].try_into().unwrap());
            d = &d[64..];
        }
        self.buf.extend_from_slice(d);
    }

    fn block(&mut self, block: &[u8; 64]) {
        let mut w = [0u32; 80];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(block[4 * i..4 * i + 4].try_into().unwrap());
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = self.h;
        for (i, &wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A827999),
                20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
                _ => (b ^ c ^ d, 0xCA62C1D6),
            };
            let t = a.rotate_left(5).wrapping_add(f).wrapping_add(e).wrapping_add(k).wrapping_add(wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        for (x, y) in self.h.iter_mut().zip([a, b, c, d, e]) {
            *x = x.wrapping_add(y);
        }
    }

    pub fn finish(mut self) -> [u8; 20] {
        let bits = self.len * 8;
        let mut pad = vec![0x80u8];
        while (self.len as usize + pad.len()) % 64 != 56 {
            pad.push(0);
        }
        pad.extend_from_slice(&bits.to_be_bytes());
        let l = self.len;
        self.update(&pad);
        self.len = l;
        let mut out = [0u8; 20];
        for (i, v) in self.h.iter().enumerate() {
            out[4 * i..4 * i + 4].copy_from_slice(&v.to_be_bytes());
        }
        out
    }
}

pub fn sha1(d: &[u8]) -> [u8; 20] {
    let mut s = Sha1::new();
    s.update(d);
    s.finish()
}

pub fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{x:02x}")).collect()
}

/// a file download with Windows' own curl.exe (no TLS stack of our own)
pub fn download(url: &str, dest: &Path, log: Log) -> Res<()> {
    log(&format!("downloading {url}"));
    let curl = which("curl").ok_or("curl.exe wasn't found (it ships with Windows 10 1803 and later)")?;
    let part = dest.with_extension("part");
    let st = quiet(Command::new(curl).args(["-fL", "--retry", "3", "-s", "-S", "-o"]).arg(&part).arg(url)).status().map_err(|e| e.to_string())?;
    if !st.success() {
        let _ = std::fs::remove_file(&part);
        return err(format!("download failed: {url}"));
    }
    std::fs::rename(&part, dest).map_err(|e| e.to_string())
}

/// runs a command, sending each line it prints to log
pub fn stream(c: &mut Command, log: Log) -> Res<()> {
    let mut child = quiet(c).stdout(Stdio::piped()).stderr(Stdio::piped()).spawn().map_err(|e| e.to_string())?;
    let se = child.stderr.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel::<String>();
    let tx2 = tx.clone();
    std::thread::spawn(move || {
        for l in BufReader::new(se).lines().map_while(Result::ok) {
            let _ = tx2.send(l);
        }
    });
    let so = child.stdout.take().unwrap();
    std::thread::spawn(move || {
        for l in BufReader::new(so).lines().map_while(Result::ok) {
            let _ = tx.send(l);
        }
    });
    for l in rx {
        if !l.trim().is_empty() {
            log(l.trim_end());
        }
    }
    let st = child.wait().map_err(|e| e.to_string())?;
    if !st.success() {
        return err(format!("{:?} failed (exit code {})", c.get_program(), st.code().unwrap_or(-1)));
    }
    Ok(())
}
