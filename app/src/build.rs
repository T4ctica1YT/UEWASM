// Running a build: the UEWASM command line in a child process, its --progress lines turned into messages, and the
// time estimate (the same model and stored rates as the Python app).
use std::collections::HashMap;
use std::io::{BufRead, BufReader};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::mpsc::{channel, Receiver};
use std::time::Instant;

use crate::settings::settings_dir;

pub const STAGES: &[&str] = &["analyze", "lift", "native compile", "native link", "wasm compile", "link", "media", "package"];

pub fn label(stage: &str) -> &str {
    match stage {
        "analyze" => "Analysing machine code",
        "lift" => "Translating functions to C",
        "native compile" => "Compiling native test build",
        "native link" => "Linking native test build",
        "wasm compile" => "Compiling WebAssembly",
        "link" => "Linking WebAssembly",
        "media" => "Re-encoding videos",
        "package" => "Packaging game files",
        s => s,
    }
}

fn default_rate(stage: &str) -> f64 {
    match stage {
        "analyze" => 0.9, "lift" => 2.2, "native compile" => 25.0, "native link" => 1.5, "wasm compile" => 11.0, "link" => 3.5,
        "media" => 20.0, "package" => 0.6, _ => 1.0,
    }
}

#[cfg(windows)]
use std::os::windows::process::CommandExt;
const NO_WINDOW: u32 = 0x0800_0000;

fn command(exe: &Path) -> Command {
    let mut c = Command::new(exe);
    #[cfg(windows)]
    c.creation_flags(NO_WINDOW);
    c
}

/// how to run the command line: uewasm-cli.exe beside this app (the packaged build), else uewasm.py with a Python found
/// near it (a checkout)
fn cli() -> Result<(PathBuf, Vec<String>), String> {
    let me = std::env::current_exe().map_err(|e| e.to_string())?;
    let dir = me.parent().unwrap_or(Path::new("."));
    let packaged = dir.join("uewasm-cli.exe");
    if packaged.exists() {
        return Ok((packaged, vec![]));
    }
    for d in me.ancestors() {
        let script = d.join("uewasm.py");
        if !script.exists() {
            continue;
        }
        let pythons = [d.join(".venv/Scripts/python.exe"), d.join("../.venv/Scripts/python.exe"), d.join("venv/Scripts/python.exe")];
        for p in pythons {
            if p.exists() {
                return Ok((p, vec!["-u".into(), script.to_string_lossy().into()]));
            }
        }
        for p in ["py", "python", "python3"] {
            if command(Path::new(p)).arg("--version").output().map_or(false, |o| o.status.success()) {
                let mut a = vec![];
                if p == "py" { a.push("-3".into()); }
                a.extend(["-u".into(), script.to_string_lossy().into()]);
                return Ok((PathBuf::from(p), a));
            }
        }
        return Err("UEWASM needs Python 3 to build (no Python found)".into());
    }
    Err("can't find the UEWASM command line (uewasm-cli.exe or uewasm.py)".into())
}

pub enum Msg {
    Log(String),
    Progress(String, f64, f64),
    Error(String, String),
    Done(String),
}

pub struct Job {
    child: Child,
    rx: Receiver<Msg>,
    readers: Vec<std::thread::JoinHandle<()>>,
    pub cancelled: bool,
}

impl Job {
    pub fn start(args: &[String]) -> Result<Job, String> {
        let (exe, pre) = cli()?;
        let mut child = command(&exe).args(&pre).args(args).stdout(Stdio::piped()).stderr(Stdio::piped()).stdin(Stdio::null())
            .env("PYTHONIOENCODING", "utf-8").spawn().map_err(|e| format!("couldn't start {}: {e}", exe.display()))?;
        let (tx, rx) = channel();
        let out = child.stdout.take().unwrap();
        let err = child.stderr.take().unwrap();
        let tx2 = tx.clone();
        let r1 = std::thread::spawn(move || {
            for line in BufReader::new(out).lines().map_while(Result::ok) {
                let parts: Vec<&str> = line.split('\t').collect();
                let m = match parts.as_slice() {
                    ["@@progress", s, d, t] => Msg::Progress(s.to_string(), d.parse().unwrap_or(0.0), t.parse().unwrap_or(0.0)),
                    ["@@error", k, text] => Msg::Error(k.to_string(), text.to_string()),
                    ["@@done", w] => Msg::Done(w.to_string()),
                    _ => Msg::Log(line),
                };
                if tx.send(m).is_err() { break; }
            }
        });
        let r2 = std::thread::spawn(move || {
            for line in BufReader::new(err).lines().map_while(Result::ok) {
                if tx2.send(Msg::Log(line)).is_err() { break; }
            }
        });
        Ok(Job { child, rx, readers: vec![r1, r2], cancelled: false })
    }

    pub fn drain(&self) -> Vec<Msg> {
        self.rx.try_iter().collect()
    }

    /// after the process ended: its last lines once the readers reach the end of its output
    pub fn finish_output(&mut self) -> Vec<Msg> {
        for r in self.readers.drain(..) {
            let _ = r.join();
        }
        self.drain()
    }

    /// the exit code once the process has ended
    pub fn exited(&self) -> Option<i32> {
        // try_wait needs &mut; a clone of the handle isn't available, so ask the OS through the pid
        let pid = self.child.id();
        #[cfg(windows)]
        {
            use std::os::windows::io::AsRawHandle;
            unsafe extern "system" {
                fn GetExitCodeProcess(h: *mut std::ffi::c_void, code: *mut u32) -> i32;
            }
            let mut code: u32 = 0;
            let ok = unsafe { GetExitCodeProcess(self.child.as_raw_handle() as *mut _, &mut code) };
            let _ = pid;
            if ok != 0 && code != 259 { Some(code as i32) } else { None }
        }
        #[cfg(not(windows))]
        { let _ = pid; None }
    }

    /// stops the build and every compiler it started
    pub fn cancel(&mut self) {
        self.cancelled = true;
        let pid = self.child.id().to_string();
        let _ = command(Path::new("taskkill")).args(["/PID", &pid, "/T", "/F"]).output();
        let _ = self.child.kill();
    }
}

/// overall time left: the running stage extrapolates from its own rate, later stages use past builds (timings.json)
pub struct Estimator {
    stages: Vec<String>,
    pub input_mb: f64,
    rate: HashMap<String, f64>,
    started: HashMap<String, Instant>,
    finished: HashMap<String, Instant>,
    cur: Option<String>,
    frac: f64,
}

impl Estimator {
    pub fn new(stages: &[&str], input_mb: f64) -> Estimator {
        let mut rate: HashMap<String, f64> = STAGES.iter().map(|s| (s.to_string(), default_rate(s))).collect();
        if let Ok(t) = std::fs::read_to_string(settings_dir().join("timings.json")) {
            if let Ok(serde_json::Value::Object(m)) = serde_json::from_str::<serde_json::Value>(&t) {
                for (k, v) in m { if let Some(f) = v.as_f64() { rate.insert(k, f); } }
            }
        }
        Estimator { stages: stages.iter().map(|s| s.to_string()).collect(), input_mb, rate, started: HashMap::new(), finished: HashMap::new(), cur: None, frac: 0.0 }
    }

    pub fn update(&mut self, stage: &str, done: f64, total: f64) {
        let now = Instant::now();
        if !self.started.contains_key(stage) {
            if let Some(c) = &self.cur {
                if !self.finished.contains_key(c) { self.finished.insert(c.clone(), now); }
            }
            self.started.insert(stage.to_string(), now);
        }
        self.cur = Some(stage.to_string());
        self.frac = if total > 0.0 { (done / total).min(1.0) } else { 0.0 };
        if total > 0.0 && done >= total {
            self.finished.insert(stage.to_string(), now);
        }
    }

    fn expected(&self, s: &str) -> f64 {
        self.rate.get(s).copied().unwrap_or(1.0) * self.input_mb
    }

    pub fn left(&self) -> f64 {
        let mut left = 0.0;
        for s in &self.stages {
            if self.finished.contains_key(s) { continue; }
            if Some(s) == self.cur.as_ref() {
                let spent = self.started[s].elapsed().as_secs_f64();
                let mut guess = self.expected(s);
                if self.frac > 0.05 && spent > 3.0 {
                    let measured = spent / self.frac;
                    guess += (measured - guess) * (self.frac * 2.0).min(1.0);
                }
                left += (guess - spent).max(0.0);
            } else if !self.started.contains_key(s) {
                left += self.expected(s);
            }
        }
        left
    }

    pub fn overall(&self) -> f64 {
        let total: f64 = self.stages.iter().map(|s| self.expected(s)).sum::<f64>().max(1.0);
        let left = self.left();
        (1.0 - left / total.max(left + 1.0)).clamp(0.0, 1.0)
    }

    /// blends this build's stage times into the stored rates (only stages that did real work)
    pub fn remember(&self) {
        let mut rate = self.rate.clone();
        for s in &self.stages {
            if let (Some(a), Some(b)) = (self.started.get(s), self.finished.get(s)) {
                let took = b.duration_since(*a).as_secs_f64();
                if took > 2.0 {
                    let r = took / self.input_mb;
                    let old = rate.get(s).copied().unwrap_or(r);
                    rate.insert(s.clone(), old * 0.5 + r * 0.5);
                }
            }
        }
        let m: serde_json::Map<String, serde_json::Value> = rate.into_iter().map(|(k, v)| (k, serde_json::json!((v * 10000.0).round() / 10000.0))).collect();
        let _ = std::fs::write(settings_dir().join("timings.json"), serde_json::to_string_pretty(&m).unwrap_or_default());
    }
}

/// the game's main executable: the file itself, else the folder's *-Shipping.exe, else its biggest .exe outside Redist
pub fn find_exe(p: &Path) -> Option<PathBuf> {
    if p.as_os_str().is_empty() || !p.exists() {
        return None;
    }
    if p.is_file() {
        return Some(p.to_path_buf());
    }
    let mut shipping = vec![];
    let mut all = vec![];
    let mut stack = vec![p.to_path_buf()];
    let mut seen = 0;
    while let Some(d) = stack.pop() {
        let Ok(rd) = std::fs::read_dir(&d) else { continue };
        for e in rd.flatten() {
            seen += 1;
            if seen > 200_000 { break; }
            let path = e.path();
            if path.is_dir() {
                stack.push(path);
            } else if path.extension().map_or(false, |x| x.eq_ignore_ascii_case("exe")) {
                let name = path.file_name().unwrap_or_default().to_string_lossy().to_string();
                if name.ends_with("-Shipping.exe") { shipping.push(path.clone()); }
                if !path.components().any(|c| c.as_os_str() == "Redist") { all.push(path); }
            }
        }
    }
    let pool = if shipping.is_empty() { all } else { shipping };
    pool.into_iter().max_by_key(|p| std::fs::metadata(p).map(|m| m.len()).unwrap_or(0))
}

/// a 256-bit AES key: 64 hex digits (0x optional) or base64 of 32 bytes
pub fn parse_key(t: &str) -> Option<Vec<u8>> {
    let t = t.trim();
    let h = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    if h.len() == 64 && h.chars().all(|c| c.is_ascii_hexdigit()) {
        return (0..32).map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).ok()).collect();
    }
    let b = base64(t)?;
    (b.len() == 32).then_some(b)
}

fn base64(s: &str) -> Option<Vec<u8>> {
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let s = s.trim_end_matches('=');
    let mut out = vec![];
    let mut buf = 0u32;
    let mut n = 0;
    for c in s.bytes() {
        let v = A.iter().position(|&x| x == c)? as u32;
        buf = buf << 6 | v;
        n += 6;
        if n >= 8 {
            n -= 8;
            out.push((buf >> n) as u8);
        }
    }
    Some(out)
}

/// one-line toolchain status for the bottom of the window
pub fn check_tools() -> String {
    let mut parts = vec![];
    let home = std::env::var("USERPROFILE").unwrap_or_default();
    let em = std::env::var("EMSDK").ok().map(PathBuf::from).into_iter().chain([Path::new(&home).join("emsdk")])
        .any(|d| d.join("upstream/emscripten").is_dir());
    parts.push(if em { "Emscripten: found".to_string() } else { "Emscripten: NOT FOUND (install emsdk to %USERPROFILE%\\emsdk or set EMSDK)".into() });
    parts.push(match cli() { Ok((p, _)) => format!("builder: {}", p.file_name().unwrap_or_default().to_string_lossy()), Err(e) => e });
    parts.join(" · ")
}

/// a URL or folder in the default app
pub fn open(what: &str) {
    let _ = command(Path::new("cmd")).args(["/c", "start", "", what]).spawn();
}
