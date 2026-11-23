//! `uewasm-cli test`: a web build in a headless browser, checked for a graphics context, frames, errors and a screen
//! that isn't blank.
use crate::cdp::{self, Browser};
use crate::util::*;
use serde_json::{json, Value};
use std::path::Path;
use std::time::{Duration, Instant};

const PROBE: &str = r#"
(() => {
  const s = window.__uewasm = { contexts: [], frames: 0, start: performance.now() };
  const get = HTMLCanvasElement.prototype.getContext;
  HTMLCanvasElement.prototype.getContext = function (t, a) {
    const c = get.call(this, t, a);
    if (c && !s.contexts.includes(t)) s.contexts.push(t);
    return c;
  };
  if (navigator.gpu) {
    const req = navigator.gpu.requestAdapter.bind(navigator.gpu);
    navigator.gpu.requestAdapter = (...x) => { s.contexts.push("webgpu"); return req(...x); };
  }
  const tick = () => { s.frames++; requestAnimationFrame(tick); };
  requestAnimationFrame(tick);
})();
"#;

const FATAL: &[&str] = &["abort(", "out of memory", "runtimeerror", "uncaught", "failed to compile", "webgl context lost", "exception thrown"];

pub fn run(build_dir: &Path, seconds: u64, report_dir: Option<&Path>) -> Res<Value> {
    let build_dir = std::fs::canonicalize(build_dir).map_err(|e| format!("{}: {e}", build_dir.display()))?;
    let read = |p: &Path| std::fs::read_to_string(p).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok());
    // a build folder's web/ beside the options it was built with (renderer: WebGPU needs its browser flags)
    let info = read(&build_dir.join("uewasm.json")).or_else(|| build_dir.parent().and_then(|p| read(&p.join("options.json")))).unwrap_or(json!({}));
    let srv = cdp::Server::start(&rust_tool("uewserve", &mut |l| println!("{l}"))?, &build_dir, &[])?;
    let url = format!("http://localhost:{}/index.html", srv.port);
    let mut result = json!({"url": url, "build": info.get("build"), "passed": false});
    let mut b = Browser::launch(&cdp::default_args(info["renderer"].as_str() == Some("webgpu")))?;
    let r = (|| -> Res<()> {
        let mut page = b.new_page(1280, 720)?;
        page.add_init_script(&mut b, PROBE)?;
        let t0 = Instant::now();
        page.goto(&mut b, &url, Duration::from_secs(120))?;
        let (mut fatal, mut last, mut fps) = (None::<String>, (0f64, Instant::now()), vec![]);
        last.1 = t0;
        while t0.elapsed().as_secs() < seconds {
            page.wait(&mut b, Duration::from_secs(2))?;
            let frames = page.eval(&mut b, "window.__uewasm ? window.__uewasm.frames : 0")?.as_f64().unwrap_or(0.0);
            let now = Instant::now();
            fps.push((frames - last.0) / now.duration_since(last.1).as_secs_f64().max(1e-3));
            last = (frames, now);
            if let Some(bad) = page.console.iter().chain(page.errors.iter()).find(|l| FATAL.iter().any(|k| l.to_lowercase().contains(k))) {
                fatal = Some(bad.clone());
                break;
            }
        }
        let state = page.eval(&mut b, "window.__uewasm || {}")?;
        let shot = page.screenshot(&mut b)?;
        let variance = cdp::png_variance(&shot);
        let tail: Vec<f64> = fps.iter().rev().take(10).copied().collect();
        let contexts = state["contexts"].clone();
        result["seconds"] = json!((t0.elapsed().as_secs_f64() * 10.0).round() / 10.0);
        result["contexts"] = if contexts.is_array() { contexts } else { json!([]) };
        result["fps_avg"] = json!((tail.iter().sum::<f64>() / tail.len().max(1) as f64 * 10.0).round() / 10.0);
        result["screen_variance"] = json!((variance * 10.0).round() / 10.0);
        result["fatal"] = json!(fatal);
        result["page_errors"] = json!(page.errors.iter().take(20).collect::<Vec<_>>());
        result["failed_requests"] = json!(page.failed.iter().take(20).collect::<Vec<_>>());
        result["console_tail"] = json!(page.console.iter().rev().take(40).rev().collect::<Vec<_>>());
        result["passed"] = json!(result["contexts"].as_array().map_or(false, |a| !a.is_empty()) && fatal.is_none() && page.errors.is_empty() && variance > 20.0);
        if let Some(rd) = report_dir {
            std::fs::create_dir_all(rd).map_err(|e| e.to_string())?;
            std::fs::write(rd.join("screenshot.png"), &shot).map_err(|e| e.to_string())?;
            std::fs::write(rd.join("report.json"), serde_json::to_string_pretty(&result).unwrap()).map_err(|e| e.to_string())?;
            let mut lines = page.console.clone();
            lines.extend(page.errors.iter().map(|e| format!("[pageerror] {e}")));
            std::fs::write(rd.join("console.txt"), lines.join("\n")).map_err(|e| e.to_string())?;
        }
        Ok(())
    })();
    b.close();
    drop(srv);
    r?;
    Ok(result)
}

pub fn summary(r: &Value) -> String {
    let ctx = r["contexts"].as_array().filter(|a| !a.is_empty()).map(|a| a.iter().filter_map(|x| x.as_str()).collect::<Vec<_>>().join(", ")).unwrap_or("none".into());
    let mut lines = vec![format!("Headless test: {}", if r["passed"].as_bool() == Some(true) { "PASS" } else { "FAIL" }),
        format!("  graphics contexts: {ctx}"), format!("  frame rate (browser rAF): {}", r["fps_avg"]),
        format!("  screen variance: {} (near 0 means a blank screen)", r["screen_variance"])];
    let cut = |s: &str| s.chars().take(300).collect::<String>();
    if let Some(f) = r["fatal"].as_str() {
        lines.push(format!("  fatal: {}", cut(f)));
    }
    for e in r["page_errors"].as_array().into_iter().flatten().take(5) {
        lines.push(format!("  page error: {}", cut(e.as_str().unwrap_or(""))));
    }
    for f in r["failed_requests"].as_array().into_iter().flatten().take(5) {
        lines.push(format!("  failed request: {}", cut(f.as_str().unwrap_or(""))));
    }
    lines.join("\n")
}
