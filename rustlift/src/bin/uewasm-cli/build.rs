//! The build steps: recompile (uewrecomp/uewlift), the web build (uewbuild, then game files through uewpack), and
//! the native verification build (MSVC).
use crate::util::*;
use uewlift::ue2::{is_ue2_system, ue2_compat_files, ue2_profile};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Instant;

/// the Steam interface versions a game's own steam_api was built with: games that use the classic accessors
/// (SteamUser()...) never name one, and WebWorks Steam lays each interface out by its version
pub fn steam_versions(game_root: &Path) -> Vec<String> {
    let re = regex::bytes::Regex::new(r"(?-u)(Steam[A-Za-z]+\d{3}|STEAM[A-Z]+_INTERFACE_VERSION\d{3}|STEAM[A-Z]+_INTERFACE_V\d{3})\x00").unwrap();
    let mut found = std::collections::BTreeSet::new();
    for f in files_under(game_root) {
        let n = f.file_name().unwrap_or_default().to_string_lossy().to_lowercase();
        if n.starts_with("steam_api") && n.ends_with(".dll") {
            if let Ok(d) = std::fs::read(&f) {
                for c in re.captures_iter(&d) {
                    found.insert(String::from_utf8_lossy(&c[1]).into_owned());
                }
            }
        }
    }
    found.into_iter().collect()
}

/// binds imports to runtime functions added since the lift, so a relink picks them up without lifting again
pub fn refresh_thunks(gen: &Path, log: Log) -> Res<u64> {
    if !gen.join("meta.c").exists() {
        return Ok(0);
    }
    let r = run_tool("uewrecomp", &[s("refresh"), ps(gen), ps(&runtime_dir()), ps(&stdcall_cache())], log, &mut |_, _, _| {})?;
    Ok(r.and_then(|v| v.as_u64()).unwrap_or(0))
}

fn s(x: &str) -> String {
    x.to_string()
}

/// DLLs shipped with the game that it may load at runtime: (install root, [paths]); real_eos keeps the game's EOS SDK
pub fn game_dlls(exe: &Path, real_eos: bool, log: Log) -> Res<(PathBuf, Vec<PathBuf>)> {
    let t = rust_tool("uewrecomp", log)?;
    let mut c = Command::new(t);
    c.arg("game-dlls").arg(exe);
    if real_eos {
        c.arg("--real-eos");
    }
    let o = quiet(&mut c).output().map_err(|e| e.to_string())?;
    if !o.status.success() {
        return err(String::from_utf8_lossy(&o.stderr).trim().to_string());
    }
    let v: Value = serde_json::from_slice(&o.stdout).map_err(|e| e.to_string())?;
    let root = PathBuf::from(v["root"].as_str().unwrap_or(""));
    let dlls = v["dlls"].as_array().map(|a| a.iter().filter_map(|d| d.as_str().map(PathBuf::from)).collect()).unwrap_or_default();
    Ok((root, dlls))
}

/// links the exe with its DLLs into image.bin, lifts every function to C in gen/ and writes gen/meta.c and
/// coverage.json. Returns the coverage report.
pub fn recompile(exe: &Path, out: &Path, mem_mb: u64, search: &[String], extra: &[PathBuf], system_dlls: bool, real_eos: bool, log: Log, progress: Progress) -> Res<Value> {
    std::fs::create_dir_all(out).map_err(|e| e.to_string())?;
    run_job("uewrecomp", "recompile", &json!({"exe": ps(exe), "out": ps(out), "mem_mb": mem_mb, "search": search,
        "extra": extra.iter().map(|p| ps(p)).collect::<Vec<_>>(), "system_dlls": system_dlls, "real_eos": real_eos,
        "runtime": ps(&runtime_dir()), "stdcall_cache": ps(&stdcall_cache())}), log, progress)
}

pub struct Emsdk {
    pub env: Vec<(String, String)>,
    pub emcc: PathBuf,
    pub node: PathBuf,
}

fn sorted_glob(dir: &Path, pred: impl Fn(&Path) -> bool) -> Vec<PathBuf> {
    let mut v: Vec<PathBuf> = std::fs::read_dir(dir).map(|r| r.flatten().map(|e| e.path()).filter(|p| pred(p)).collect()).unwrap_or_default();
    v.sort();
    v
}

pub fn emsdk_env() -> Res<Emsdk> {
    let root = crate::deps::find_emsdk().ok_or("Emscripten SDK not found (choose it in the app's Tools tab, pass --emsdk, or run 'uewasm-cli install-emsdk')")?;
    let mut env = vec![(s("EMSDK"), ps(&root)), (s("EM_CONFIG"), ps(&root.join(".emscripten")))];
    if let Some(py) = sorted_glob(&root.join("python"), |p| p.join("python.exe").exists()).last() {
        env.push((s("EMSDK_PYTHON"), ps(&py.join("python.exe"))));
    }
    let mut node = sorted_glob(&root.join("node"), |p| p.join("bin").is_dir()).into_iter().map(|p| p.join("bin")).collect::<Vec<_>>();
    if node.is_empty() {
        node = sorted_glob(&root.join("node"), |p| p.join("node.exe").exists());
    }
    let mut path = vec![root.clone(), root.join("upstream/emscripten")];
    path.extend(node.last().cloned());
    let joined = std::env::join_paths(path.iter().cloned().chain(std::env::split_paths(&std::env::var_os("PATH").unwrap_or_default()))).map_err(|e| e.to_string())?;
    env.push((s("PATH"), joined.to_string_lossy().into()));
    let mut emcc = root.join("upstream/emscripten").join(format!("emcc{EXE}"));
    if !emcc.exists() {
        emcc = root.join("upstream/emscripten/emcc.bat");
    }
    let nexe = which("node").or_else(|| node.last().map(|n| n.join("node.exe"))).unwrap_or_else(|| PathBuf::from("node"));
    Ok(Emsdk { env, emcc, node: nexe })
}

/// newer Node ships JSPI enabled and rejects the old flag
pub fn jspi_flag(node: &Path) -> Vec<String> {
    let ok = quiet(Command::new(node).args(["-e", "process.exit(typeof WebAssembly.Suspending == 'function' ? 0 : 1)"])).status().map_or(false, |s| s.success());
    if ok { vec![] } else { vec![s("--experimental-wasm-jspi")] }
}

// the game signs in to the offline account over WinInet, which the runtime answers (runtime/web/backends/epic.js)
fn epic_args(exe_rel: &str) -> Vec<String> {
    let stem = Path::new(exe_rel).file_stem().unwrap_or_default().to_string_lossy().to_string();
    let app = regex::Regex::new(r"(Client|Game)?-Win64-Shipping$").unwrap().replace(&stem, "").to_string();
    ["-HTTP=WinInet", "-AUTH_TYPE=epic", "-AUTH_LOGIN=Player", "-AUTH_PASSWORD=offline", &format!("-epicapp={app}"), "-epicenv=Prod",
     "-epiclocale=en-us", "-epicportal", "-skippatchcheck", "-nobe", "-noeac", "-fromfl=none", "-fltoken=none"].iter().map(|x| x.to_string()).collect()
}

/// offline stand-ins for online services: (the script the page loads, launch arguments that point the game at it)
pub fn backend(name: &str) -> Option<(&'static str, fn(&str) -> Vec<String>)> {
    match name {
        "epic" => Some(("epic.js", epic_args as fn(&str) -> Vec<String>)),
        _ => None,
    }
}

/// which Unreal Engine generation an installed game is, from its folder layout
pub fn engine_of(exe: &Path) -> Option<&'static str> {
    for top in exe.ancestors().skip(1).take(5) {
        if top.join("Engine").is_dir() {
            if let Ok(rd) = std::fs::read_dir(top) {
                let kids: Vec<PathBuf> = rd.flatten().map(|e| e.path()).filter(|d| d.is_dir() && !d.file_name().unwrap_or_default().eq_ignore_ascii_case("engine")).collect();
                let cooked = kids.iter().any(|d| std::fs::read_dir(d).map_or(false, |r| r.flatten().any(|c| c.path().is_dir() && c.file_name().to_string_lossy().to_lowercase().starts_with("cooked"))));
                if cooked {
                    return Some("Unreal Engine 3");
                }
                if top.join("Engine/Binaries").is_dir() || kids.iter().any(|d| d.join("Content/Paks").is_dir()) {
                    return Some("Unreal Engine 4/5");
                }
            }
            return Some("Unreal Engine");
        }
        // Unreal Engine 1 and 2 installs both use System/Maps/Textures; a UE2 package or renderer tells them apart
        if top.join("System").is_dir() && (top.join("Maps").is_dir() || top.join("Textures").is_dir()
            || top.join("System/D3DDrv.u").is_file() || top.join("System/D3D9Drv.u").is_file()) && is_ue2_system(&top.join("System")) {
            return Some("Unreal Engine 2");
        }
    }
    None
}

/// launch arguments a game needs here: Unreal games that can pick D3D12 are told to use D3D11
pub fn default_args(exe: &Path) -> Vec<String> {
    let Ok(d) = std::fs::read(exe) else { return vec![] };
    let has = |n: &[u8]| d.windows(n.len()).any(|w| w == n);
    let mut args = if has(b"D3D11RHI") && has(b"D3D12RHI") { vec![s("-dx11")] } else { vec![] };
    // the EOS SDK restarts a game through the launcher unless it was started with what the launcher passes
    if let Some(app) = epic_install(exe) {
        args.push(s("-EpicPortal"));
        if let Some(a) = app {
            args.push(format!("-epicapp={a}"));
        }
        args.extend([s("-epicenv=Prod"), s("-epiclocale=en-US")]);
    }
    args
}

/// a game the Epic launcher installed: Some(its launcher app name, when the launcher's install list has it)
pub fn epic_install(exe: &Path) -> Option<Option<String>> {
    epic_manifest(exe).map(|m| m["AppName"].as_str().map(String::from))
}

/// the page's Epic sign-in: on (account) for games the Epic launcher installed when they run the real EOS SDK, with
/// the game's catalog namespace (the launcher passes it as -epicsandboxid)
fn epic_config(exe: &Path, o: &Wasm) -> Value {
    let m = epic_manifest(exe);
    let on = match o.epic.as_str() { "account" => true, "off" => false, _ => o.eos_real && m.is_some() };
    let m = m.unwrap_or_else(|| json!({}));
    json!({"mode": if on { "account" } else { "off" }, "app": m["AppName"], "namespace": m["CatalogNamespace"]})
}

/// the Epic launcher's record of the game's install (AppName, CatalogNamespace, ...), or an empty object when only its
/// .egstore folder shows the launcher installed it; None for other games
pub fn epic_manifest(exe: &Path) -> Option<Value> {
    let root = exe.ancestors().skip(1).take(6).find(|d| d.join(".egstore").is_dir())?;
    let same = |p: &str| std::fs::canonicalize(p).ok() == std::fs::canonicalize(root).ok();
    let data = std::env::var_os("ProgramData").map(PathBuf::from).unwrap_or_else(|| PathBuf::from(r"C:\ProgramData"));
    for e in std::fs::read_dir(data.join(r"Epic\EpicGamesLauncher\Data\Manifests")).into_iter().flatten().flatten() {
        if e.path().extension().map_or(false, |x| x == "item") {
            let j: Value = std::fs::read_to_string(e.path()).ok().and_then(|t| serde_json::from_str(t.trim_start_matches('\u{feff}')).ok()).unwrap_or(Value::Null);
            if j["InstallLocation"].as_str().map_or(false, same) {
                return Some(j);
            }
        }
    }
    let list: Value = std::fs::read_to_string(data.join(r"Epic\UnrealEngineLauncher\LauncherInstalled.dat")).ok()
        .and_then(|t| serde_json::from_str(t.trim_start_matches('\u{feff}')).ok()).unwrap_or(Value::Null);
    let app = list["InstallationList"].as_array().into_iter().flatten().find(|i| i["InstallLocation"].as_str().map_or(false, same)).cloned();
    Some(app.unwrap_or_else(|| json!({})))
}

const SCALABILITY: &[&str] = &["ShadowQuality", "PostProcessQuality", "EffectsQuality", "FoliageQuality", "ViewDistanceQuality", "AntiAliasingQuality",
    "ReflectionQuality", "GlobalIlluminationQuality", "ShadingQuality", "LandscapeQuality"];

/// Unreal scalability groups for the page to put in the game's own settings file (GameUserSettings.ini); textures
/// are left alone. UE5 reads Saved/Config/Windows, UE4 .../WindowsNoEditor
pub fn quality_files(exe_rel: &str, quality: &str) -> serde_json::Map<String, Value> {
    let mut files = serde_json::Map::new();
    let q = match quality { "low" => 0, "medium" => 1, "high" => 2, "epic" => 3, _ => return files };
    let parts: Vec<&str> = exe_rel.split('/').collect();
    let Some(bi) = parts.iter().position(|p| *p == "Binaries").filter(|&i| i >= 1) else { return files };
    let project = parts[bi - 1];
    // shadows stay at least medium: with none, lights shine through everything and scenes look plainly wrong
    let text = format!("[ScalabilityGroups]\n{}", SCALABILITY.iter().map(|g| format!("sg.{g}={}\n", if *g == "ShadowQuality" { q.max(1) } else { q })).collect::<String>());
    let base = format!("Users/Player/AppData/Local/{project}/Saved/Config");
    for p in ["Windows", "WindowsNoEditor"] {
        files.insert(format!("{base}/{p}/GameUserSettings.ini"), json!(text));
    }
    // low and medium post-processing turn off auto exposure (dark rooms come out far too dark) and the tonemapper's
    // contrast and vignette (corners too bright); together they cost one small pass, so they stay on
    if q < 2 {
        let ea: String = (0..2).map(|i| format!("[PostProcessQuality@{i}]\nr.EyeAdaptationQuality=2\nr.Tonemapper.Quality=5\n")).collect();
        for p in ["Windows", "WindowsNoEditor"] {
            files.insert(format!("{base}/{p}/Scalability.ini"), json!(ea));
        }
    }
    files
}

/// how a running game's memory limit is shared: the wasm heap (guest memory plus ~700 MB of runtime), the file
/// download cache, and the rest for the GPU (reported to the game as video memory)
pub fn memory_split(limit_gb: f64, guest_mb: u64) -> (u64, u64) {
    let total = (limit_gb * 1024.0) as i64;
    let cache = (total / 12).clamp(256, 1024);
    let gpu = (total - guest_mb as i64 - 700 - cache).max(512);
    (cache as u64, gpu as u64)
}

/// one .html holding the whole web build, for opening straight from disk with no server (cooperative threads only)
pub fn write_single_file(web: &Path, dest: &Path, log: Log) -> Res<PathBuf> {
    let exe = rust_tool("uewbuild", log)?;
    let o = quiet(Command::new(exe).arg("single").arg(web).arg(dest).arg("16")).output().map_err(|e| e.to_string())?;
    if !o.status.success() {
        return err(tail(&String::from_utf8_lossy(&o.stderr), 2000).to_string());
    }
    for l in String::from_utf8_lossy(&o.stdout).lines() {
        log(l);
    }
    Ok(dest.to_path_buf())
}

/// C defines for a 64-bit memory build: a 32-bit game keeps its addresses wrapping at 4 GB (UEW_G32, rt.h)
pub fn mem64_defines(out: &Path) -> Vec<String> {
    let g32 = std::fs::read(out.join("gen/meta.c")).map_or(false, |d| String::from_utf8_lossy(&d).contains("const int RT_G32 = 1;"));
    let mut v = vec![s("UEW_MEM64")];
    if g32 {
        v.push(s("UEW_G32"));
    }
    v
}

pub struct Wasm {
    pub opt: String,
    pub mem_mb: Option<u64>,
    pub embed: bool,
    pub node_fs: bool,
    pub threads: String,
    pub renderer: String,
    pub game_root: Option<PathBuf>,
    pub exe_rel: Option<String>,
    pub title: Option<String>,
    pub size: Option<(u32, u32)>, // None: native
    pub part_mb: u64,
    pub compress: bool,
    pub names: bool,
    pub video_height: u32,
    pub video_mode: String,
    pub wasm_opt: bool,
    pub game_args: Option<Vec<String>>,
    pub quality: String,
    pub simd: bool,
    pub show_fps: bool,
    pub async_shaders: bool,
    pub debug_panel: bool,
    pub backend: Option<String>,
    pub placeholders: bool,
    pub aes_key: Option<Vec<u8>>,
    pub mem_limit_gb: f64,
    pub single_file: bool,
    pub web_name: String,
    pub mem64: bool,
    pub steam: String,
    pub webworks_url: String,
    pub relay: bool,
    pub wisp: bool,
    pub wisp_url: Option<String>,
    pub steam_wisp: bool,
    /// auto (account for Epic launcher installs with the real EOS SDK), account or off
    pub epic: String,
    pub eos_real: bool,
}

impl Default for Wasm {
    fn default() -> Wasm {
        Wasm { opt: s("-O2"), mem_mb: None, embed: true, node_fs: false, threads: s("coop"), renderer: s("webgpu"), game_root: None, exe_rel: None,
            title: None, size: Some((1280, 720)), part_mb: 16, compress: false, names: false, video_height: 0, video_mode: s("fast"), wasm_opt: false,
            game_args: None, quality: s("game"), simd: true, show_fps: false, async_shaders: true, debug_panel: true, backend: None, placeholders: false,
            aes_key: None, mem_limit_gb: 8.0, single_file: false, web_name: s("web"), mem64: false, steam: s("webworks"),
            webworks_url: s("http://localhost:3000"), relay: false, wisp: false, wisp_url: None, steam_wisp: false, epic: s("auto"), eos_real: false }
    }
}

/// the web build: compile and link (uewbuild), then the game's files and the page
pub fn build_wasm(out: &Path, o: &Wasm, log: Log, progress: Progress) -> Res<PathBuf> {
    if !["webgpu", "webgl2"].contains(&o.renderer.as_str()) {
        return err("renderer must be webgpu or webgl2");
    }
    if !["coop", "real"].contains(&o.threads.as_str()) {
        return err("threads must be coop or real");
    }
    if o.threads == "real" && o.renderer != "webgpu" {
        return err("real threads need the WebGPU renderer (WebGL only works from the page's own thread)");
    }
    if o.single_file && o.threads == "real" {
        return err("a single-file build can't use real threads (a page opened from disk can't be cross-origin isolated); use --threads coop");
    }
    let compress = o.compress || o.single_file;
    let em = emsdk_env()?;
    let bound = refresh_thunks(&out.join("gen"), log)?;
    if bound > 0 {
        log(&format!("{bound} imports bound to runtime functions added since the lift"));
    }
    let plan = crate::res::plan();
    let backend = o.backend.as_deref().filter(|_| o.game_root.is_some()).and_then(backend);
    let envmap: serde_json::Map<String, Value> = em.env.iter().map(|(k, v)| (k.clone(), json!(v))).collect();
    let var = |k: &str| std::env::var(k).unwrap_or_default();
    let res = run_job("uewbuild", "compile", &json!({
        "out": ps(out), "runtime": ps(&runtime_dir()), "emcc": ps(&em.emcc), "opt": o.opt, "simd": o.simd, "renderer": o.renderer, "threads": o.threads,
        "env": envmap, "mem64": o.mem64, "mem64_defines": if o.mem64 { mem64_defines(out) } else { vec![] },
        // developer hooks: UEW_LTO=thin, UEW_EXTRA_CFLAGS (e.g. -DUEW_STORE_CHECK), UEW_TIMES=<file>
        "lto": var("UEW_LTO") == "thin", "extra_cflags": var("UEW_EXTRA_CFLAGS").split_whitespace().collect::<Vec<_>>(),
        "times_file": var("UEW_TIMES"), "debug_panel": o.debug_panel, "names": o.names, "wasm_opt": o.wasm_opt,
        "web_name": o.web_name, "game_files": o.game_root.is_some(), "backend_script": backend.map_or("", |b| b.0),
        "node_fs": o.node_fs, "embed": o.embed && o.game_root.is_none(), "compile_jobs": plan.compile_jobs, "budget_mb": plan.budget_mb}), log, progress)?;
    let web = out.join(&o.web_name);
    let wasm_mb = res["wasm_mb"].as_f64().unwrap_or(0.0);
    if let (Some(root), Some(exe_rel)) = (&o.game_root, &o.exe_rel) {
        let mut replace: Vec<(String, PathBuf)> = vec![];
        if let Some(k) = &o.aes_key {
            replace = crate::upak::decrypt_paks(root, k, out, log, progress)?;
        }
        if o.video_height > 0 {
            let r = crate::media::shrink(root, &root.join(exe_rel), out, o.video_height, &o.video_mode, o.aes_key.clone(), log, progress)?;
            for (k, v) in r {
                replace.retain(|x| x.0 != k);
                replace.push((k, v));
            }
        }
        if let Some(profile) = ue2_profile(&root.join(exe_rel)) {
            let bits = if profile.bits == 0 { "unknown".to_string() } else { format!("{}-bit", profile.bits) };
            let version = profile.version.map_or_else(|| "UE2".to_string(), |v| format!("UE2 package {v}"));
            log(&format!("[ue2] {version}, {bits}: {} renderer, {} config file(s)", profile.renderer.label(), profile.configs.len()));
            replace.extend(ue2_compat_files(root, out, &profile, log)?);
        }
        let args = uewlift::pack::game_files_args(root, &web, o.part_mb, plan.package_workers, compress, &out.join("image.bin"), &replace);
        run_tool("uewpack", &args, log, progress)?;
        let stem = Path::new(exe_rel).file_stem().unwrap_or_default().to_string_lossy().to_string();
        let save = format!("/save/{}", stem.chars().filter(|c| c.is_ascii_alphanumeric() || *c == '_' || *c == '-').collect::<String>());
        let mut game_args = o.game_args.clone().unwrap_or_else(|| default_args(&root.join(exe_rel)));
        if let Some((_, f)) = backend {
            game_args.extend(f(exe_rel));
        }
        let mut args = vec![s("--image"), s("/image.bin"), s("--root"), s("/game"), s("--save"), save.clone(), s("--exe"), exe_rel.clone()];
        args.extend(game_args);
        let opts: Value = std::fs::read_to_string(out.join("options.json")).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(Value::Null);
        let guest = o.mem_mb.or(opts["mem_mb"].as_u64()).unwrap_or(3072);
        let (cache_mb, gpu_mb) = memory_split(o.mem_limit_gb, guest);
        log(&format!("memory limit {} GB: {guest} MB game memory, {cache_mb} MB download cache, {gpu_mb} MB video memory", o.mem_limit_gb));
        let (w, h) = o.size.unwrap_or((1280, 720));
        let wisp = if o.wisp || o.wisp_url.is_some() {
            json!({"url": o.wisp_url.clone().unwrap_or_else(|| s("origin")), "steam": o.steam_wisp && o.steam == "account"})
        } else {
            json!({})
        };
        let title = o.title.clone().unwrap_or_else(|| stem.clone());
        run_job("uewbuild", "shell", &json!({"web": ps(&web), "args": args, "title": title, "width": w, "height": h, "native": o.size.is_none(),
            "save": save, "part_mb": 24, "show_fps": o.show_fps, "async_shaders": o.async_shaders, "debug_panel": o.debug_panel,
            "exe": ps(&root.join(exe_rel)), "fallback_icon": ps(&assets_dir().join("uewasm.ico")), "placeholders": o.placeholders,
            "cache_mb": cache_mb, "gpu_mb": gpu_mb, "threads": o.threads, "seed": quality_files(exe_rel, &o.quality), "mem64": o.mem64,
            "steam": {"mode": o.steam, "url": o.webworks_url, "relay": o.relay, "versions": steam_versions(root)}, "wisp": wisp,
            "epic": epic_config(&root.join(exe_rel), o)}), log, progress)?;
        for (mark, on, what) in [("uew-relay", o.relay, "the relay"), ("uew-wisp", o.wisp && o.wisp_url.is_none(), "the WISP server")] {
            let p = web.join(mark);
            if on {
                std::fs::write(&p, format!("this build goes online through {what} `uewasm-cli serve` runs\n")).map_err(|e| e.to_string())?;
            } else if p.exists() {
                let _ = std::fs::remove_file(&p);
            }
        }
        if o.single_file {
            let name: String = title.chars().filter(|c| !r#"<>:"/\|?*"#.contains(*c)).collect::<String>().trim().to_string();
            let name = if name.is_empty() { s("game") } else { name };
            write_single_file(&web, &out.join("single").join(format!("{name}.html")), log)?;
        }
    }
    log(&format!("wasm build: {} ({}{}, {} files compiled, {:.1}s, {wasm_mb:.1} MB wasm)", web.display(), o.renderer,
                 if o.mem64 { ", 64-bit memory" } else { "" }, res["files"].as_u64().unwrap_or(0), res["seconds"].as_f64().unwrap_or(0.0)));
    Ok(web)
}

pub fn msvc_env(arch: &str) -> Res<HashMap<String, String>> {
    let path = crate::deps::msvc().ok_or("Visual Studio C++ tools not found")?;
    let bat = Path::new(&path).join(format!("VC/Auxiliary/Build/vcvars{}.bat", if arch == "x64" { "64" } else { "32" }));
    let comspec = std::env::var("ComSpec").unwrap_or(r"C:\Windows\System32\cmd.exe".into());
    let mut c = Command::new(comspec);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.raw_arg(format!("/d /s /c \"call \"{}\" >nul && set\"", bat.display()));
    }
    let o = quiet(&mut c).output().map_err(|e| e.to_string())?;
    let env: HashMap<String, String> = String::from_utf8_lossy(&o.stdout).lines().filter_map(|l| l.split_once('=')).map(|(k, v)| (k.to_uppercase(), v.to_string())).collect();
    if !env.contains_key("VCTOOLSINSTALLDIR") {
        return err("couldn't set up the MSVC environment");
    }
    Ok(env)
}

fn tool_in(env: &HashMap<String, String>, name: &str) -> Res<PathBuf> {
    let path = env.get("PATH").cloned().unwrap_or_default();
    std::env::split_paths(&path).map(|d| d.join(format!("{name}.exe"))).find(|p| p.is_file()).ok_or_else(|| format!("{name} not found"))
}

pub struct CJob {
    pub name: String,
    pub cmd: Vec<String>,
    pub env: HashMap<String, String>,
    pub mb: f64,
}

/// biggest first, as many at once as the cores and the memory budget allow; progress by estimated size
pub fn run_jobs(mut jobs: Vec<CJob>, what: &str, progress: Progress) -> Res<()> {
    jobs.sort_by(|a, b| b.mb.partial_cmp(&a.mb).unwrap());
    let plan = crate::res::plan();
    let (workers, budget) = (plan.compile_jobs, plan.budget_mb as f64);
    let total = jobs.iter().map(|j| j.mb).sum::<f64>().max(1.0);
    let mut pending: std::collections::VecDeque<CJob> = jobs.into();
    let mut running: Vec<(std::process::Child, String, f64, PathBuf, Instant)> = vec![];
    let (mut used, mut done) = (0.0, 0.0);
    let mut times = vec![];
    progress(what, 0.0, total);
    let r = (|| -> Res<()> {
        while !pending.is_empty() || !running.is_empty() {
            while let Some(j) = pending.front() {
                if running.len() >= workers || (!running.is_empty() && used + j.mb > budget) {
                    break;
                }
                let j = pending.pop_front().unwrap();
                let logp = std::env::temp_dir().join(format!("uewjob-{}.log", unique()));
                let lf = std::fs::File::create(&logp).map_err(|e| e.to_string())?;
                let lf2 = lf.try_clone().map_err(|e| e.to_string())?;
                let mut c = Command::new(&j.cmd[0]);
                c.args(&j.cmd[1..]).env_clear().envs(&j.env).stdout(Stdio::from(lf)).stderr(Stdio::from(lf2));
                let child = quiet(&mut c).spawn().map_err(|e| format!("{}: {e}", j.name))?;
                used += j.mb;
                running.push((child, j.name, j.mb, logp, Instant::now()));
            }
            std::thread::sleep(std::time::Duration::from_millis(50));
            let mut i = 0;
            while i < running.len() {
                match running[i].0.try_wait().map_err(|e| e.to_string())? {
                    None => i += 1,
                    Some(st) => {
                        let (_, name, mb, logp, t0) = running.remove(i);
                        times.push((t0.elapsed().as_secs_f64(), name.clone()));
                        used -= mb;
                        done += mb;
                        progress(what, done, total);
                        let text = std::fs::read_to_string(&logp).unwrap_or_default();
                        let _ = std::fs::remove_file(&logp);
                        if !st.success() {
                            return err(format!("{what} failed on {name}:\n{}", tail(&text, 3000)));
                        }
                    }
                }
            }
        }
        Ok(())
    })();
    for (c, _, _, l, _) in running.iter_mut() {
        let _ = c.kill();
        let _ = std::fs::remove_file(l);
    }
    // developer hook: UEW_TIMES=<file> records how long each job took (slowest first)
    if let Ok(f) = std::env::var("UEW_TIMES") {
        if !times.is_empty() {
            times.sort_by(|a, b| b.0.partial_cmp(&a.0).unwrap());
            let _ = std::fs::write(f, times.iter().map(|(t, n)| format!("{t:8.1} {n}\n")).collect::<String>());
        }
    }
    r
}

/// the native verification build. mem64: guest addresses past 4 GB, as in a --mem64 web build
pub fn build_native(out: &Path, debug: bool, mem64: bool, log: Log, progress: Progress) -> Res<PathBuf> {
    let rt = runtime_dir();
    let obj = out.join(if mem64 { "obj_m64" } else { "obj" });
    std::fs::create_dir_all(&obj).map_err(|e| e.to_string())?;
    let env = msvc_env("x64")?;
    let gen = sorted_glob(&out.join("gen"), |p| p.extension().map_or(false, |e| e == "c"));
    let objf = |p: &Path, d: &Path| d.join(format!("{}.obj", p.file_stem().unwrap_or_default().to_string_lossy()));
    let mut stale: Vec<PathBuf> = gen.iter().filter(|p| !objf(p, &obj).exists() || mtime(&objf(p, &obj)) < mtime(p)).cloned().collect();
    if out.join("gen/funcs.h").exists() && gen.iter().any(|p| objf(p, &obj).exists()) {
        let newest_h = ["rt.h", "sse.h"].iter().map(|n| mtime(&rt.join(n))).max().unwrap();
        stale = gen.iter().filter(|p| stale.contains(p) || mtime(&objf(p, &obj)) < newest_h).cloned().collect();
    }
    for o in sorted_glob(&obj, |p| p.extension().map_or(false, |e| e == "obj")) {
        if !out.join("gen").join(format!("{}.c", o.file_stem().unwrap_or_default().to_string_lossy())).exists() {
            let _ = std::fs::remove_file(o);
        }
    }
    let mut srcs = stale.clone();
    srcs.extend(sorted_glob(&rt, |p| p.extension().map_or(false, |e| e == "c")));
    let mut flags: Vec<String> = ["/nologo", "/c", if debug { "/Od" } else { "/O1" }, "/W0", "/bigobj"].iter().map(|x| x.to_string()).collect();
    flags.extend([format!("/I{}", rt.display()), format!("/I{}", out.join("gen").display()), format!("/Fo{}\\", obj.display()),
                  s("/D_CRT_SECURE_NO_WARNINGS"), s("/std:c11")]);
    if debug {
        flags.push(s("/Z7"));
    }
    if mem64 {
        flags.extend(mem64_defines(out).iter().map(|d| format!("/D{d}")));
    }
    let t0 = Instant::now();
    let cl = ps(&tool_in(&env, "cl")?);
    // BearSSL (Schannel's TLS): its own object folder, compiled when missing or older than its source
    let bssl = rt.join("third_party/bearssl");
    let bobj = obj.join("bssl");
    std::fs::create_dir_all(&bobj).map_err(|e| e.to_string())?;
    let inc = [format!("/I{}", bssl.join("inc").display()), format!("/I{}", bssl.join("src").display())];
    flags.push(format!("/I{}", bssl.join("inc").display()));
    let lib: Vec<PathBuf> = files_under(&bssl.join("src")).into_iter().filter(|p| p.extension().map_or(false, |e| e == "c"))
        .filter(|p| !objf(p, &bobj).exists() || mtime(&objf(p, &bobj)) < mtime(p)).collect();
    let mut jobs = vec![];
    for p in &srcs {
        let mut cmd = vec![cl.clone()];
        cmd.extend(flags.iter().cloned());
        cmd.push(ps(p));
        let mb = 80.0 + 40.0 * std::fs::metadata(p).map_or(0, |m| m.len()) as f64 / 1048576.0;
        jobs.push(CJob { name: p.file_name().unwrap_or_default().to_string_lossy().into(), cmd, env: env.clone(), mb });
    }
    for p in &lib {
        let mut cmd = vec![cl.clone()];
        cmd.extend(flags.iter().filter(|f| !f.starts_with("/Fo")).cloned());
        cmd.extend(inc.iter().cloned());
        cmd.push(format!("/Fo{}\\", bobj.display()));
        cmd.push(ps(p));
        jobs.push(CJob { name: p.file_name().unwrap_or_default().to_string_lossy().into(), cmd, env: env.clone(), mb: 80.0 });
    }
    run_jobs(jobs, "native compile", progress)?;
    progress("native link", 0.0, 0.0);
    let exe = out.join("native.exe");
    let lrsp = out.join("link.rsp");
    let objs: Vec<PathBuf> = sorted_glob(&obj, |p| p.extension().map_or(false, |e| e == "obj")).into_iter()
        .chain(sorted_glob(&bobj, |p| p.extension().map_or(false, |e| e == "obj"))).collect();
    std::fs::write(&lrsp, objs.iter().map(|p| format!("\"{}\"", p.display())).collect::<Vec<_>>().join("\n")).map_err(|e| e.to_string())?;
    let o = quiet(Command::new(tool_in(&env, "link")?).env_clear().envs(&env).args(["/nologo", &format!("/OUT:{}", exe.display()), "/STACK:268435456",
        &format!("/MAP:{}", out.join("native.map").display()), &format!("@{}", lrsp.display())])).output().map_err(|e| e.to_string())?;
    progress("native link", 1.0, 1.0);
    if !o.status.success() {
        return err(format!("link failed:\n{}", tail(&String::from_utf8_lossy(&o.stdout), 2000)));
    }
    log(&format!("native build: {} ({} generated files rebuilt, {:.1}s)", exe.display(), stale.len(), t0.elapsed().as_secs_f64()));
    Ok(exe)
}
