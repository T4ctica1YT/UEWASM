// "Play in browser": the web build served on localhost with the cross-origin isolation headers real threads need, and
// HTTP Range requests (game files are read in blocks). Stops when the app closes or another build is served. When
// uewserve (rustlift/) is at hand it does the serving, which also runs the built-in relay for builds made with --relay.
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

pub struct Server {
    pub port: u16,
    stop: Arc<AtomicBool>,
    http: Option<Arc<tiny_http::Server>>,
    child: Option<std::process::Child>,
}

impl Drop for Server {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(h) = &self.http {
            h.unblock();
        }
        if let Some(c) = &mut self.child {
            let _ = c.kill();
            let _ = c.wait();
        }
    }
}

/// uewserve beside the app (the packaged build) or in the source tree's rustlift/target/release
fn uewserve() -> Option<PathBuf> {
    let exe = std::env::current_exe().ok()?;
    let name = if cfg!(windows) { "uewserve.exe" } else { "uewserve" };
    exe.ancestors().skip(1).take(5).flat_map(|d| [d.join(name), d.join("rustlift").join("target").join("release").join(name)]).find(|p| p.is_file())
}

fn start_uewserve(exe: &Path, root: &Path) -> Option<Server> {
    use std::io::BufRead;
    let mut cmd = std::process::Command::new(exe);
    cmd.arg(root).args(["--port", "8080"]).stdout(std::process::Stdio::piped()).stderr(std::process::Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000);
    }
    let mut child = cmd.spawn().ok()?;
    let out = child.stdout.take()?;
    let mut lines = std::io::BufReader::new(out).lines();
    let port = lines.by_ref().map_while(Result::ok).find_map(|l| l.strip_prefix("@@port ").and_then(|p| p.trim().parse().ok()));
    std::thread::spawn(move || for _ in lines {});
    match port {
        Some(port) => Some(Server { port, stop: Arc::new(AtomicBool::new(false)), http: None, child: Some(child) }),
        None => {
            let _ = child.kill();
            None
        }
    }
}

fn mime(p: &Path) -> &'static str {
    match p.extension().and_then(|e| e.to_str()).unwrap_or("").to_ascii_lowercase().as_str() {
        "wasm" => "application/wasm", "js" => "text/javascript", "html" => "text/html", "css" => "text/css", "json" => "application/json",
        "txt" => "text/plain", "png" => "image/png", "ico" => "image/x-icon", _ => "application/octet-stream",
    }
}

impl Server {
    pub fn start(root: &Path) -> Result<Server, String> {
        if let Some(s) = uewserve().and_then(|e| start_uewserve(&e, root)) {
            return Ok(s);
        }
        let http = tiny_http::Server::http("127.0.0.1:8080").or_else(|_| tiny_http::Server::http("127.0.0.1:0")).map_err(|e| e.to_string())?;
        let port = http.server_addr().to_ip().map(|a| a.port()).unwrap_or(0);
        let http = Arc::new(http);
        let stop = Arc::new(AtomicBool::new(false));
        let root = root.to_path_buf();
        for _ in 0..8 {
            let (h, s, r) = (http.clone(), stop.clone(), root.clone());
            std::thread::spawn(move || {
                while !s.load(Ordering::Relaxed) {
                    match h.recv() {
                        Ok(req) => handle(&r, req),
                        Err(_) => break,
                    }
                }
            });
        }
        Ok(Server { port, stop, http: Some(http), child: None })
    }
}

fn header(k: &str, v: &str) -> tiny_http::Header {
    tiny_http::Header::from_bytes(k.as_bytes(), v.as_bytes()).unwrap()
}

fn handle(root: &PathBuf, req: tiny_http::Request) {
    let url = req.url().split('?').next().unwrap_or("/").to_string();
    let rel = url.trim_start_matches('/');
    let rel = if rel.is_empty() { "index.html" } else { rel };
    let path = root.join(rel.replace("%20", " "));
    let common = [header("Cross-Origin-Opener-Policy", "same-origin"), header("Cross-Origin-Embedder-Policy", "require-corp"),
                  header("Cross-Origin-Resource-Policy", "same-origin"), header("Cache-Control", "no-cache"), header("Accept-Ranges", "bytes")];
    // never outside the build folder
    let ok = path.canonicalize().ok().zip(root.canonicalize().ok()).map_or(false, |(p, r)| p.starts_with(r)) && path.is_file();
    if !ok {
        let mut r = tiny_http::Response::from_string("not found").with_status_code(404);
        for h in common { r.add_header(h); }
        let _ = req.respond(r);
        return;
    }
    let Ok(mut f) = std::fs::File::open(&path) else { return };
    let size = f.metadata().map(|m| m.len()).unwrap_or(0);
    let range = req.headers().iter().find(|h| h.field.equiv("Range")).map(|h| h.value.as_str().to_string());
    let span = range.as_deref().and_then(|r| r.strip_prefix("bytes=")).and_then(|r| {
        let (a, b) = r.split_once('-')?;
        let a: u64 = a.parse().ok()?;
        let b: u64 = if b.is_empty() { size.saturating_sub(1) } else { b.parse::<u64>().ok()?.min(size.saturating_sub(1)) };
        (a <= b && a < size).then_some((a, b))
    });
    let mut hs: Vec<tiny_http::Header> = common.into_iter().collect();
    hs.push(header("Content-Type", mime(&path)));
    match span {
        Some((a, b)) => {
            let n = b - a + 1;
            let _ = f.seek(SeekFrom::Start(a));
            hs.push(header("Content-Range", &format!("bytes {a}-{b}/{size}")));
            let r = tiny_http::Response::new(tiny_http::StatusCode(206), hs, f.take(n), Some(n as usize), None);
            let _ = req.respond(r);
        }
        None => {
            let r = tiny_http::Response::new(tiny_http::StatusCode(200), hs, f, Some(size as usize), None);
            let _ = req.respond(r);
        }
    }
}
