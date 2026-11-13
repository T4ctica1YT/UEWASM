// The app's choices, kept between runs in %LOCALAPPDATA%\UEWASM\app_settings.json (beside the build-time estimates).
use serde_json::{json, Value};
use std::path::PathBuf;

pub fn settings_dir() -> PathBuf {
    let base = std::env::var("LOCALAPPDATA").or_else(|_| std::env::var("USERPROFILE")).unwrap_or_else(|_| ".".into());
    let d = PathBuf::from(base).join("UEWASM");
    let _ = std::fs::create_dir_all(&d);
    d
}

pub struct Settings {
    pub game: String,
    pub out: String,
    pub renderer: String,
    pub threads: String,
    pub opt: String,
    pub lifter: String,
    pub quality: String,
    pub videos: String,
    pub priority: String,
    pub backend: String,
    pub resolution: String,
    pub mem: u32,
    pub part: u32,
    pub mem_limit: u32,
    pub game_dlls: bool,
    pub sys_dlls: bool,
    pub native: bool,
    pub compress: bool,
    pub wasm_opt: bool,
    pub single: bool,
    pub show_fps: bool,
    pub async_shaders: bool,
    pub debug_panel: bool,
    pub placeholders: bool,
    pub aes_on: bool,
    pub aes_key: String,
}

impl Settings {
    pub fn load() -> Settings {
        let v: Value = std::fs::read_to_string(settings_dir().join("app_settings.json")).ok()
            .and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(Value::Null);
        let s = |k: &str, d: &str| v.get(k).and_then(|x| x.as_str()).unwrap_or(d).to_string();
        let n = |k: &str, d: u32| v.get(k).and_then(|x| x.as_u64()).map(|x| x as u32).unwrap_or(d);
        let b = |k: &str, d: bool| v.get(k).and_then(|x| x.as_bool()).unwrap_or(d);
        Settings {
            game: s("game", ""), out: s("out", ""), renderer: s("renderer", "WebGPU (recommended)"), threads: s("threads", "Cooperative (no special hosting)"),
            opt: s("opt", "Balanced (-O2)"), lifter: s("lifter", "Python (original)"), quality: s("quality", "Game's own setting"), videos: s("videos", "Keep original videos"),
            priority: s("priority", "High (fastest, tuned to this PC)"), backend: s("backend", "None (games play offline as usual)"),
            resolution: s("resolution", "1280x720"), mem: n("mem", 3072), part: n("part", 16), mem_limit: n("mem_limit", 8),
            game_dlls: b("game_dlls", true), sys_dlls: b("sys_dlls", true), native: b("native", false), compress: b("compress", false),
            wasm_opt: b("wasm_opt", false), single: b("single", false), show_fps: b("show_fps", false), async_shaders: b("async_shaders", true),
            debug_panel: b("debug_panel", true), placeholders: b("placeholders", false), aes_on: b("aes_on", false), aes_key: String::new(),
        }
    }

    /// the key itself is never written to disk
    pub fn save(&self) {
        let v = json!({
            "game": self.game, "out": self.out, "renderer": self.renderer, "threads": self.threads, "opt": self.opt, "lifter": self.lifter, "quality": self.quality,
            "videos": self.videos, "priority": self.priority, "backend": self.backend, "resolution": self.resolution, "mem": self.mem,
            "part": self.part, "mem_limit": self.mem_limit, "game_dlls": self.game_dlls, "sys_dlls": self.sys_dlls, "native": self.native,
            "compress": self.compress, "wasm_opt": self.wasm_opt, "single": self.single, "show_fps": self.show_fps,
            "async_shaders": self.async_shaders, "debug_panel": self.debug_panel, "placeholders": self.placeholders, "aes_on": self.aes_on,
        });
        let _ = std::fs::write(settings_dir().join("app_settings.json"), serde_json::to_string_pretty(&v).unwrap_or_default());
    }
}
