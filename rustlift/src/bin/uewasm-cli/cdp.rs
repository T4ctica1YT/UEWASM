//! A headless Edge or Chrome driven over the DevTools protocol (one WebSocket to the browser, flattened sessions):
//! enough for the browser tests, with no Playwright or Node.
use serde_json::{json, Value};
use std::io::{BufRead, BufReader, Read, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

pub type Res<T> = Result<T, String>;

pub fn find_browser() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("UEWASM_BROWSER") {
        return Some(PathBuf::from(p));
    }
    let pf = |k: &str, d: &str| PathBuf::from(std::env::var(k).unwrap_or(d.into()));
    let local = PathBuf::from(std::env::var("LOCALAPPDATA").unwrap_or_default());
    [pf("ProgramFiles(x86)", r"C:\Program Files (x86)").join(r"Microsoft\Edge\Application\msedge.exe"),
     pf("ProgramFiles", r"C:\Program Files").join(r"Microsoft\Edge\Application\msedge.exe"),
     pf("ProgramFiles", r"C:\Program Files").join(r"Google\Chrome\Application\chrome.exe"),
     pf("ProgramFiles(x86)", r"C:\Program Files (x86)").join(r"Google\Chrome\Application\chrome.exe"),
     local.join(r"Google\Chrome\Application\chrome.exe")]
        .into_iter().find(|p| p.exists())
}

/// the flags the tests run with: muted, GPU allowed, never throttled in the background
pub fn default_args(webgpu: bool) -> Vec<String> {
    let mut a: Vec<String> = ["--mute-audio", "--autoplay-policy=user-gesture-required", "--ignore-gpu-blocklist", "--enable-gpu-rasterization",
        "--use-angle=default", "--disable-renderer-backgrounding", "--disable-background-timer-throttling", "--disable-backgrounding-occluded-windows"]
        .iter().map(|s| s.to_string()).collect();
    if webgpu {
        a.extend(["--enable-unsafe-webgpu".into(), "--enable-features=Vulkan,WebGPU".into()]);
    }
    a
}

pub struct Browser {
    child: Child,
    ws: Ws,
    next: u64,
    dir: PathBuf,
    pub events: Vec<Value>,
}

struct Ws {
    s: TcpStream,
    r: BufReader<TcpStream>,
}

impl Ws {
    fn connect(port: u16, path: &str) -> Res<Ws> {
        let s = TcpStream::connect(("127.0.0.1", port)).map_err(|e| e.to_string())?;
        s.set_nodelay(true).ok();
        let mut w = s.try_clone().map_err(|e| e.to_string())?;
        write!(w, "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n")
            .map_err(|e| e.to_string())?;
        let mut r = BufReader::with_capacity(1 << 20, s.try_clone().map_err(|e| e.to_string())?);
        let mut line = String::new();
        r.read_line(&mut line).map_err(|e| e.to_string())?;
        if !line.contains(" 101") {
            return Err(format!("DevTools refused the connection: {}", line.trim()));
        }
        loop {
            line.clear();
            r.read_line(&mut line).map_err(|e| e.to_string())?;
            if line.trim().is_empty() {
                break;
            }
        }
        Ok(Ws { s: w, r })
    }

    fn send(&mut self, text: &str) -> Res<()> {
        let p = text.as_bytes();
        let mut f = vec![0x81u8];
        if p.len() < 126 {
            f.push(0x80 | p.len() as u8);
        } else if p.len() < 65536 {
            f.push(0x80 | 126);
            f.extend((p.len() as u16).to_be_bytes());
        } else {
            f.push(0x80 | 127);
            f.extend((p.len() as u64).to_be_bytes());
        }
        let mask = [0x37u8, 0xfa, 0x21, 0x3d];
        f.extend(mask);
        f.extend(p.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        self.s.write_all(&f).map_err(|e| e.to_string())
    }

    /// one message; None when the wait ran out
    fn recv(&mut self, wait: Duration) -> Res<Option<String>> {
        self.r.get_ref().set_read_timeout(Some(wait.max(Duration::from_millis(1)))).ok();
        let mut msg = vec![];
        loop {
            let mut h = [0u8; 2];
            match self.r.read_exact(&mut h) {
                Ok(()) => {}
                Err(e) if msg.is_empty() && matches!(e.kind(), std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut) => return Ok(None),
                Err(e) => return Err(format!("DevTools connection lost: {e}")),
            }
            // the rest of a frame that started arriving is read without a deadline
            self.r.get_ref().set_read_timeout(Some(Duration::from_secs(60))).ok();
            let op = h[0] & 0x0f;
            let mut n = (h[1] & 0x7f) as u64;
            if n == 126 {
                let mut b = [0u8; 2];
                self.r.read_exact(&mut b).map_err(|e| e.to_string())?;
                n = u16::from_be_bytes(b) as u64;
            } else if n == 127 {
                let mut b = [0u8; 8];
                self.r.read_exact(&mut b).map_err(|e| e.to_string())?;
                n = u64::from_be_bytes(b);
            }
            let mut p = vec![0u8; n as usize];
            self.r.read_exact(&mut p).map_err(|e| e.to_string())?;
            match op {
                0x8 => return Err("DevTools closed the connection".into()),
                0x9 => {
                    let mut f = vec![0x8au8, 0x80 | p.len().min(125) as u8, 0, 0, 0, 0];
                    f.extend(&p[..p.len().min(125)]);
                    self.s.write_all(&f).map_err(|e| e.to_string())?;
                    continue;
                }
                0xA => continue,
                _ => msg.extend(p),
            }
            if h[0] & 0x80 != 0 {
                return Ok(Some(String::from_utf8_lossy(&msg).into_owned()));
            }
        }
    }
}

impl Browser {
    pub fn launch(args: &[String]) -> Res<Browser> {
        let exe = find_browser().ok_or("no Edge or Chrome found (set UEWASM_BROWSER to a Chromium browser)")?;
        let dir = std::env::temp_dir().join(format!("uewasm-browser-{}-{}", std::process::id(), Instant::now().elapsed().as_nanos() ^ rand_seed()));
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        let mut c = Command::new(&exe);
        c.args(["--headless=new", "--remote-debugging-port=0", "--no-first-run", "--no-default-browser-check", "--disable-sync", "--hide-scrollbars"])
            .arg(format!("--user-data-dir={}", dir.display())).args(args).arg("about:blank")
            .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            c.creation_flags(0x0800_0000);
        }
        let mut child = c.spawn().map_err(|e| format!("{}: {e}", exe.display()))?;
        let port_file = dir.join("DevToolsActivePort");
        let t0 = Instant::now();
        let (port, path) = loop {
            if let Ok(t) = std::fs::read_to_string(&port_file) {
                let mut l = t.lines();
                if let (Some(p), Some(w)) = (l.next(), l.next()) {
                    if let Ok(p) = p.trim().parse::<u16>() {
                        break (p, w.trim().to_string());
                    }
                }
            }
            if t0.elapsed() > Duration::from_secs(30) || child.try_wait().ok().flatten().is_some() {
                let _ = child.kill();
                return Err(format!("{} didn't start its DevTools endpoint", exe.display()));
            }
            std::thread::sleep(Duration::from_millis(50));
        };
        let ws = Ws::connect(port, &path)?;
        Ok(Browser { child, ws, next: 1, dir, events: vec![] })
    }

    /// a command; events that arrive meanwhile are kept in events
    pub fn call(&mut self, session: Option<&str>, method: &str, params: Value) -> Res<Value> {
        self.call_wait(session, method, params, Duration::from_secs(120))
    }

    pub fn call_wait(&mut self, session: Option<&str>, method: &str, params: Value, timeout: Duration) -> Res<Value> {
        let id = self.next;
        self.next += 1;
        let mut m = json!({"id": id, "method": method, "params": params});
        if let Some(s) = session {
            m["sessionId"] = json!(s);
        }
        self.ws.send(&m.to_string())?;
        let end = Instant::now() + timeout;
        loop {
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(format!("{method} timed out"));
            }
            let Some(t) = self.ws.recv(left)? else { continue };
            let v: Value = serde_json::from_str(&t).unwrap_or(Value::Null);
            if v["id"].as_u64() == Some(id) {
                if let Some(e) = v.get("error") {
                    return Err(format!("{method}: {}", e["message"].as_str().unwrap_or("error")));
                }
                return Ok(v["result"].clone());
            }
            if v.get("method").is_some() {
                self.events.push(v);
            }
        }
    }

    /// collects events for this long
    pub fn pump(&mut self, d: Duration) -> Res<()> {
        let end = Instant::now() + d;
        loop {
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Ok(());
            }
            if let Some(t) = self.ws.recv(left)? {
                let v: Value = serde_json::from_str(&t).unwrap_or(Value::Null);
                if v.get("method").is_some() {
                    self.events.push(v);
                }
            }
        }
    }

    /// waits for an event by name (and session), keeping the rest
    pub fn wait_event(&mut self, session: &str, method: &str, timeout: Duration) -> Res<Value> {
        let end = Instant::now() + timeout;
        loop {
            if let Some(i) = self.events.iter().position(|e| e["method"] == method && e["sessionId"] == session) {
                return Ok(self.events.remove(i));
            }
            let left = end.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return Err(format!("{method} didn't arrive in {}s", timeout.as_secs()));
            }
            if let Some(t) = self.ws.recv(left)? {
                let v: Value = serde_json::from_str(&t).unwrap_or(Value::Null);
                if v.get("method").is_some() {
                    self.events.push(v);
                }
            }
        }
    }

    /// a new page (tab) with its own session, viewport and the page/console/network events turned on
    pub fn new_page(&mut self, width: u32, height: u32) -> Res<Page> {
        self.page_in(None, width, height)
    }

    /// a page in a browser context of its own (separate storage and cookies, like a second player)
    pub fn new_context_page(&mut self, width: u32, height: u32) -> Res<Page> {
        let c = self.call(None, "Target.createBrowserContext", json!({}))?;
        let id = c["browserContextId"].as_str().ok_or("no browser context")?.to_string();
        self.page_in(Some(&id), width, height)
    }

    fn page_in(&mut self, context: Option<&str>, width: u32, height: u32) -> Res<Page> {
        let mut p = json!({"url": "about:blank"});
        if let Some(c) = context {
            p["browserContextId"] = json!(c);
        }
        let t = self.call(None, "Target.createTarget", p)?;
        let tid = t["targetId"].as_str().ok_or("no target")?.to_string();
        let a = self.call(None, "Target.attachToTarget", json!({"targetId": tid, "flatten": true}))?;
        let sid = a["sessionId"].as_str().ok_or("no session")?.to_string();
        for m in ["Page.enable", "Runtime.enable", "Network.enable"] {
            self.call(Some(&sid), m, json!({}))?;
        }
        self.call(Some(&sid), "Emulation.setDeviceMetricsOverride", json!({"width": width, "height": height, "deviceScaleFactor": 1, "mobile": false}))?;
        Ok(Page { sid, urls: Default::default(), console: vec![], errors: vec![], failed: vec![], responses: vec![] })
    }

    pub fn close(mut self) {
        let _ = self.call_wait(None, "Browser.close", json!({}), Duration::from_secs(5));
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_secs(5) && self.child.try_wait().ok().flatten().is_none() {
            std::thread::sleep(Duration::from_millis(50));
        }
        let _ = self.child.kill();
        let _ = self.child.wait();
        for _ in 0..20 {
            if std::fs::remove_dir_all(&self.dir).is_ok() || !self.dir.exists() {
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }
}

fn rand_seed() -> u128 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0)
}

/// a page's session and what it has logged so far
pub struct Page {
    pub sid: String,
    urls: std::collections::HashMap<String, String>,
    pub console: Vec<String>,
    pub errors: Vec<String>,
    pub failed: Vec<String>,
    /// (url, status) of every response
    pub responses: Vec<(String, u64)>,
}

fn arg_text(a: &Value) -> String {
    match a.get("value") {
        Some(Value::String(s)) => s.clone(),
        Some(v) if !v.is_null() => v.to_string(),
        _ => a["description"].as_str().or(a["unserializableValue"].as_str()).unwrap_or(a["type"].as_str().unwrap_or("")).to_string(),
    }
}

impl Page {
    /// moves this page's events out of the browser's list into console / errors / failed requests
    pub fn absorb(&mut self, b: &mut Browser) {
        let mine: Vec<Value> = {
            let (m, rest): (Vec<Value>, Vec<Value>) = b.events.drain(..).partition(|e| e["sessionId"] == self.sid.as_str()
                && matches!(e["method"].as_str(), Some("Runtime.consoleAPICalled" | "Runtime.exceptionThrown" | "Network.requestWillBeSent" | "Network.loadingFailed" | "Network.responseReceived")));
            b.events = rest;
            m
        };
        for e in mine {
            let p = &e["params"];
            match e["method"].as_str().unwrap_or("") {
                "Runtime.consoleAPICalled" => {
                    let t = p["type"].as_str().unwrap_or("log");
                    let t = if t == "warning" { "warning" } else { t };
                    let text = p["args"].as_array().map(|a| a.iter().map(arg_text).collect::<Vec<_>>().join(" ")).unwrap_or_default();
                    self.console.push(format!("[{t}] {text}"));
                }
                "Runtime.exceptionThrown" => {
                    let d = &p["exceptionDetails"];
                    let text = d["exception"]["description"].as_str().or(d["text"].as_str()).unwrap_or("error").to_string();
                    self.errors.push(text);
                }
                "Network.requestWillBeSent" => {
                    self.urls.insert(p["requestId"].as_str().unwrap_or("").into(), p["request"]["url"].as_str().unwrap_or("").into());
                }
                "Network.loadingFailed" => {
                    let url = self.urls.get(p["requestId"].as_str().unwrap_or("")).cloned().unwrap_or_default();
                    if !p["canceled"].as_bool().unwrap_or(false) {
                        self.failed.push(format!("{url} {}", p["errorText"].as_str().unwrap_or("")));
                    }
                }
                "Network.responseReceived" => {
                    let r = &p["response"];
                    let st = r["status"].as_u64().unwrap_or(0);
                    let url = r["url"].as_str().unwrap_or("").to_string();
                    if st >= 400 {
                        self.failed.push(format!("{url} HTTP {st}"));
                    }
                    self.responses.push((url, st));
                }
                _ => {}
            }
        }
    }

    pub fn add_init_script(&self, b: &mut Browser, src: &str) -> Res<()> {
        b.call(Some(&self.sid), "Page.addScriptToEvaluateOnNewDocument", json!({"source": src})).map(|_| ())
    }

    /// navigates and waits for the load event
    pub fn goto(&mut self, b: &mut Browser, url: &str, timeout: Duration) -> Res<()> {
        b.events.retain(|e| !(e["method"] == "Page.loadEventFired" && e["sessionId"] == self.sid.as_str()));
        let r = b.call(Some(&self.sid), "Page.navigate", json!({"url": url}))?;
        if let Some(e) = r["errorText"].as_str() {
            return Err(format!("{url}: {e}"));
        }
        b.wait_event(&self.sid.clone(), "Page.loadEventFired", timeout)?;
        self.absorb(b);
        Ok(())
    }

    pub fn eval(&mut self, b: &mut Browser, expr: &str) -> Res<Value> {
        let r = b.call(Some(&self.sid), "Runtime.evaluate", json!({"expression": expr, "returnByValue": true, "awaitPromise": true}))?;
        self.absorb(b);
        if let Some(e) = r.get("exceptionDetails") {
            return Err(e["exception"]["description"].as_str().or(e["text"].as_str()).unwrap_or("error").to_string());
        }
        Ok(r["result"]["value"].clone())
    }

    pub fn wait(&mut self, b: &mut Browser, d: Duration) -> Res<()> {
        b.pump(d)?;
        self.absorb(b);
        Ok(())
    }

    pub fn screenshot(&mut self, b: &mut Browser) -> Res<Vec<u8>> {
        use base64::Engine;
        let r = b.call(Some(&self.sid), "Page.captureScreenshot", json!({"format": "png"}))?;
        self.absorb(b);
        base64::engine::general_purpose::STANDARD.decode(r["data"].as_str().unwrap_or("")).map_err(|e| e.to_string())
    }

    /// clicks at a point of the page (mouse down and up)
    pub fn click(&mut self, b: &mut Browser, x: f64, y: f64) -> Res<()> {
        for t in ["mousePressed", "mouseReleased"] {
            b.call(Some(&self.sid), "Input.dispatchMouseEvent", json!({"type": t, "x": x, "y": y, "button": "left", "clickCount": 1}))?;
        }
        Ok(())
    }

    pub fn key(&mut self, b: &mut Browser, key: &str, code: &str, vk: u32) -> Res<()> {
        for t in ["rawKeyDown", "keyUp"] {
            b.call(Some(&self.sid), "Input.dispatchKeyEvent", json!({"type": t, "key": key, "code": code, "windowsVirtualKeyCode": vk}))?;
        }
        Ok(())
    }
}

/// a PNG's pixel variance, sampled every 8th row and column (near 0 means a blank screen)
pub fn png_variance(data: &[u8]) -> f64 {
    if data.len() < 8 || &data[..8] != b"\x89PNG\r\n\x1a\n" {
        return 0.0;
    }
    let (mut pos, mut idat, mut w, mut h, mut ctype) = (8usize, vec![], 0usize, 0usize, 2u8);
    while pos + 8 <= data.len() {
        let n = u32::from_be_bytes(data[pos..pos + 4].try_into().unwrap()) as usize;
        let kind = &data[pos + 4..pos + 8];
        let body = &data[pos + 8..(pos + 8 + n).min(data.len())];
        if kind == b"IHDR" {
            w = u32::from_be_bytes(body[0..4].try_into().unwrap()) as usize;
            h = u32::from_be_bytes(body[4..8].try_into().unwrap()) as usize;
            ctype = body[9];
        } else if kind == b"IDAT" {
            idat.extend_from_slice(body);
        }
        pos += 12 + n;
    }
    let bpp = if ctype == 6 { 4 } else { 3 };
    let mut raw = vec![];
    if flate2::read::ZlibDecoder::new(&idat[..]).read_to_end(&mut raw).is_err() {
        return 0.0;
    }
    let stride = w * bpp;
    let mut prev = vec![0u8; stride];
    let mut samples: Vec<[f64; 3]> = vec![];
    for y in 0..h {
        let at = y * (stride + 1);
        if at + 1 + stride > raw.len() {
            break;
        }
        let f = raw[at];
        let mut line = raw[at + 1..at + 1 + stride].to_vec();
        for i in 0..stride {
            let a = if i >= bpp { line[i - bpp] as i32 } else { 0 };
            let b = prev[i] as i32;
            let c = if i >= bpp { prev[i - bpp] as i32 } else { 0 };
            let x = line[i] as i32;
            line[i] = match f {
                1 => (x + a) & 255,
                2 => (x + b) & 255,
                3 => (x + (a + b) / 2) & 255,
                4 => {
                    let p = a + b - c;
                    let (pa, pb, pc) = ((p - a).abs(), (p - b).abs(), (p - c).abs());
                    (x + if pa <= pb && pa <= pc { a } else if pb <= pc { b } else { c }) & 255
                }
                _ => x,
            } as u8;
        }
        if y % 8 == 0 {
            let mut x = 0;
            while x + 3 <= stride {
                samples.push([line[x] as f64, line[x + 1] as f64, line[x + 2] as f64]);
                x += bpp * 8;
            }
        }
        prev = line;
    }
    if samples.is_empty() {
        return 0.0;
    }
    (0..3).map(|ch| {
        let mean = samples.iter().map(|s| s[ch]).sum::<f64>() / samples.len() as f64;
        samples.iter().map(|s| (s[ch] - mean).powi(2)).sum::<f64>() / samples.len() as f64
    }).sum()
}

/// uewserve on a free port beside a test: .port, dropped (or .stop()) to end it
pub struct Server {
    child: Child,
    pub port: u16,
}

impl Server {
    pub fn start(uewserve: &Path, root: &Path, extra: &[&str]) -> Res<Server> {
        let mut c = Command::new(uewserve);
        c.arg(root).args(["--port", "0", "--host", "127.0.0.1"]).args(extra).stdout(Stdio::piped()).stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            c.creation_flags(0x0800_0000);
        }
        let mut child = c.spawn().map_err(|e| e.to_string())?;
        let out = child.stdout.take().unwrap();
        let mut r = BufReader::new(out);
        let mut line = String::new();
        let port = loop {
            line.clear();
            if r.read_line(&mut line).map_err(|e| e.to_string())? == 0 {
                let _ = child.kill();
                return Err("uewserve didn't start".into());
            }
            if let Some(p) = line.trim().strip_prefix("@@port ") {
                break p.parse::<u16>().map_err(|e| e.to_string())?;
            }
        };
        std::thread::spawn(move || {
            let mut sink = String::new();
            while r.read_line(&mut sink).map_or(false, |n| n > 0) {
                sink.clear();
            }
        });
        Ok(Server { child, port })
    }
}

impl Drop for Server {
    fn drop(&mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}
