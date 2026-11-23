//! Finding (and where possible installing) the tools that builds and particular settings need.
use crate::util::*;
use crate::zip::Zip;
use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::Command;

const EMSDK_ZIP: &str = "https://github.com/emscripten-core/emsdk/archive/refs/heads/main.zip";
// emsdk.py needs a Python the first time; later it uses the one it installs itself
const PYTHON_ZIP: &str = "https://www.python.org/ftp/python/3.13.1/python-3.13.1-embed-amd64.zip";
const FFMPEG_ZIP: &str = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip";
pub const RUSTUP_URL: &str = "https://rustup.rs";
pub const VS_URL: &str = "https://visualstudio.microsoft.com/visual-cpp-build-tools/";

fn user_home() -> PathBuf {
    PathBuf::from(std::env::var("USERPROFILE").or_else(|_| std::env::var("HOME")).unwrap_or_else(|_| ".".into()))
}

pub fn saved_emsdk() -> Option<String> {
    let t = std::fs::read_to_string(settings_dir().join("settings.json")).ok()?;
    let v: Value = serde_json::from_str(&t).ok()?;
    v.get("emsdk")?.as_str().filter(|s| !s.is_empty()).map(String::from)
}

pub fn save_emsdk(p: &Path) {
    let f = settings_dir().join("settings.json");
    let mut v: Value = std::fs::read_to_string(&f).ok().and_then(|t| serde_json::from_str(&t).ok()).unwrap_or(json!({}));
    v["emsdk"] = json!(ps(p));
    let _ = std::fs::create_dir_all(settings_dir());
    let _ = std::fs::write(f, serde_json::to_string_pretty(&v).unwrap_or_default());
}

pub fn default_emsdk_dir() -> PathBuf {
    user_home().join("emsdk")
}

/// the SDK root for a folder the user picked: the root itself, a folder holding 'emsdk', or a folder inside the SDK
pub fn emsdk_at(p: &str) -> Option<PathBuf> {
    let p = PathBuf::from(p.trim().trim_matches('"'));
    if p.as_os_str().is_empty() {
        return None;
    }
    let mut c = vec![p.clone(), p.join("emsdk")];
    c.extend(p.ancestors().skip(1).take(3).map(Path::to_path_buf));
    c.into_iter().find(|c| c.join("upstream/emscripten").is_dir() && c.join("emsdk.py").exists())
}

/// an SDK checkout whose compilers were never installed
pub fn emsdk_partial(p: &str) -> bool {
    let p = PathBuf::from(p.trim().trim_matches('"'));
    p.join("emsdk.py").exists() && !p.join("upstream/emscripten").is_dir()
}

/// UEWASM_EMSDK (the app's or --emsdk's choice), the app's saved choice, EMSDK, then the usual places
pub fn find_emsdk() -> Option<PathBuf> {
    let mut c: Vec<String> = [std::env::var("UEWASM_EMSDK").ok(), saved_emsdk(), std::env::var("EMSDK").ok()].into_iter().flatten().collect();
    c.push(ps(&default_emsdk_dir()));
    c.push(ps(&home().join(".toolchain/emsdk")));
    c.push(ps(&home().join("../.toolchain/emsdk")));
    if cfg!(windows) {
        c.push(format!("{}\\emsdk", std::env::var("SystemDrive").unwrap_or("C:".into())));
    }
    c.iter().find_map(|x| emsdk_at(x))
}

pub fn binaryen(root: &Path) -> bool {
    root.join("upstream/bin").join(format!("wasm-opt{EXE}")).exists()
}

fn vswhere() -> PathBuf {
    PathBuf::from(std::env::var("ProgramFiles(x86)").unwrap_or(r"C:\Program Files (x86)".into())).join("Microsoft Visual Studio/Installer/vswhere.exe")
}

pub fn msvc() -> Option<String> {
    let vw = vswhere();
    if !vw.exists() {
        return None;
    }
    let o = quiet(Command::new(vw).args(["-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
        "-property", "installationPath"])).output().ok()?;
    String::from_utf8_lossy(&o.stdout).lines().next().map(|s| s.trim().to_string()).filter(|s| !s.is_empty())
}

pub fn rust_tools_ready() -> bool {
    let d = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default();
    RUST_TOOLS.iter().all(|t| d.join(format!("{t}{EXE}")).exists()) || cargo().is_some()
}

fn tools_dir() -> PathBuf {
    settings_dir().join("tools")
}

/// ffmpeg for --video-height: UEWASM_FFMPEG, one installed by UEWASM, one beside this tool, or one on PATH
pub fn ffmpeg() -> Option<PathBuf> {
    if let Ok(p) = std::env::var("UEWASM_FFMPEG") {
        return Some(PathBuf::from(p));
    }
    let me = std::env::current_exe().ok().and_then(|p| p.parent().map(Path::to_path_buf)).unwrap_or_default();
    [tools_dir().join(format!("ffmpeg{EXE}")), me.join(format!("ffmpeg{EXE}"))].into_iter().find(|p| p.exists()).or_else(|| which("ffmpeg"))
}

/// an Oodle decompressor DLL (Unreal paks compressed with Oodle): UEWASM_OODLE, the game's own copy, or one installed
pub fn oodle(game_root: Option<&Path>) -> Option<PathBuf> {
    if let Ok(p) = std::env::var("UEWASM_OODLE") {
        return Some(PathBuf::from(p));
    }
    let mut c: Vec<PathBuf> = game_root.map(files_under).unwrap_or_default();
    c.extend(files_under(&tools_dir()));
    c.into_iter().find(|p| {
        let n = p.file_name().map(|n| n.to_string_lossy().to_lowercase()).unwrap_or_default();
        n.starts_with("oo2core") && n.ends_with("win64.dll")
    })
}

pub fn winget() -> Option<PathBuf> {
    which("winget")
}

fn python() -> Option<PathBuf> {
    // the Store's python.exe stub isn't a Python
    which("python").filter(|p| !ps(p).to_lowercase().contains("windowsapps"))
}

/// emsdk from GitHub into dest, then its latest compilers (about 2.5 GB); returns the SDK root
pub fn install_emsdk(dest: &Path, log: Log) -> Res<PathBuf> {
    let mut dest = dest.to_path_buf();
    if !dest.join("emsdk.py").exists() {
        if dest.exists() && std::fs::read_dir(&dest).map_or(false, |mut d| d.next().is_some()) {
            dest = dest.join("emsdk");
        }
        std::fs::create_dir_all(&dest).map_err(|e| e.to_string())?;
        log(&format!("downloading emsdk to {}", dest.display()));
        let z = std::env::temp_dir().join(format!("emsdk-{}.zip", unique()));
        download(EMSDK_ZIP, &z, log)?;
        let r = Zip::open(&z).and_then(|zip| zip.extract(&dest, true));
        let _ = std::fs::remove_file(&z);
        r?;
    }
    let py = match python() {
        Some(p) => p,
        None => {
            let pd = tools_dir().join("python");
            let exe = pd.join("python.exe");
            if !exe.exists() {
                let z = std::env::temp_dir().join(format!("python-{}.zip", unique()));
                download(PYTHON_ZIP, &z, log)?;
                let r = Zip::open(&z).and_then(|zip| zip.extract(&pd, false));
                let _ = std::fs::remove_file(&z);
                r?;
            }
            exe
        }
    };
    for step in ["install", "activate"] {
        log(&format!("emsdk {step} latest (the download is large; this takes a while)"));
        let mut c = Command::new(&py);
        c.arg(dest.join("emsdk.py")).args([step, "latest"]).current_dir(&dest);
        for k in ["PYTHONHOME", "PYTHONPATH", "EMSDK", "EM_CONFIG"] {
            c.env_remove(k);
        }
        stream(&mut c, log)?;
    }
    if emsdk_at(&ps(&dest)).is_none() {
        return err(format!("emsdk finished but {} is missing", dest.join("upstream/emscripten").display()));
    }
    Ok(dest)
}

pub fn install_winget(pkg: &str, log: Log, overrides: Option<&str>) -> Res<()> {
    let w = winget().ok_or("winget isn't available on this PC")?;
    let mut c = Command::new(w);
    c.args(["install", "--id", pkg, "-e", "--accept-package-agreements", "--accept-source-agreements"]);
    if let Some(o) = overrides {
        c.args(["--override", o]);
    }
    stream(&mut c, log)
}

pub fn install_ffmpeg(log: Log) -> Res<PathBuf> {
    let z = std::env::temp_dir().join(format!("ffmpeg-{}.zip", unique()));
    download(FFMPEG_ZIP, &z, log)?;
    let zip = Zip::open(&z);
    let _ = std::fs::remove_file(&z);
    let zip = zip?;
    let e = zip.entries.iter().find(|e| e.name.ends_with("/bin/ffmpeg.exe")).ok_or("ffmpeg.exe isn't in the download")?;
    let dst = tools_dir().join("ffmpeg.exe");
    std::fs::create_dir_all(tools_dir()).map_err(|e| e.to_string())?;
    std::fs::write(&dst, zip.read(e)?).map_err(|e| e.to_string())?;
    Ok(dst)
}

pub fn install(what: &str, dir: Option<&Path>, log: Log) -> Res<String> {
    match what {
        "emsdk" => {
            let root = install_emsdk(dir.unwrap_or(&default_emsdk_dir()), log)?;
            save_emsdk(&root);
            Ok(format!("Emscripten installed in {}", root.display()))
        }
        "rust" => install_winget("Rustlang.Rustup", log, None).map(|_| "Rust installed".into()),
        "msvc" => install_winget("Microsoft.VisualStudio.2022.BuildTools", log,
            Some("--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended")).map(|_| "Build Tools installed".into()),
        "ffmpeg" => install_ffmpeg(log).map(|p| format!("ffmpeg installed in {}", p.display())),
        _ => err(format!("nothing called {what} to install (emsdk, rust, msvc, ffmpeg)")),
    }
}

/// what the chosen options need that this PC lacks (native, wasm_opt, videos), for front ends
pub fn missing(native: bool, wasm_opt: bool, videos: bool) -> Vec<Value> {
    let mut out = vec![];
    let wg = winget().is_some();
    match find_emsdk() {
        None => {
            let looked = std::env::var("UEWASM_EMSDK").ok().or_else(saved_emsdk);
            out.push(json!({"key": "emsdk", "name": "Emscripten", "title": "Emscripten isn't installed",
                "text": format!("UEWASM compiles games to WebAssembly with the Emscripten SDK, and it wasn't found{}.\n\nUEWASM can download and install it for you (about 2.5 GB), or you can point it at an existing install.",
                    looked.map(|l| format!(" (looked in {l})")).unwrap_or_default()),
                "install": "Install Emscripten for me", "browse": true, "url": "https://emscripten.org/docs/getting_started/downloads.html"}));
        }
        Some(root) if wasm_opt && !binaryen(&root) => out.push(json!({"key": "binaryen", "name": "Binaryen", "title": "Binaryen isn't in this Emscripten install",
            "text": format!("'Shrink code with Binaryen' runs wasm-opt, which wasn't found in {}.", root.join("upstream/bin").display()), "off": "Build without Binaryen"})),
        _ => {}
    }
    if native && msvc().is_none() {
        out.push(json!({"key": "msvc", "name": "Visual Studio Build Tools", "title": "Visual Studio C++ tools aren't installed",
            "text": format!("The native test .exe is built with Microsoft's C++ compiler (Visual Studio or its Build Tools, with 'Desktop development with C++').{}",
                if wg { "\n\nUEWASM can install the Build Tools for you with winget (about 6 GB; Windows asks for administrator rights)." } else { "" }),
            "install": if wg { json!("Install Build Tools for me") } else { Value::Null }, "off": "Skip the native test .exe", "url": VS_URL}));
    }
    if !rust_tools_ready() {
        out.push(json!({"key": "rust", "name": "Rust", "title": "Rust isn't installed",
            "text": format!("The recompiler's tools (rustlift/) are built from source the first time they're used, which needs the Rust toolchain.{}",
                if wg { "\n\nUEWASM can install Rust for you with winget." } else { "" }),
            "install": if wg { json!("Install Rust for me") } else { Value::Null }, "url": RUSTUP_URL}));
    }
    if videos && ffmpeg().is_none() {
        out.push(json!({"key": "ffmpeg", "name": "ffmpeg", "title": "The video encoder isn't installed",
            "text": "Shrinking videos uses ffmpeg, which wasn't found.\n\nUEWASM can download it for you (about 150 MB).",
            "install": "Install it for me", "off": "Keep the original videos"}));
    }
    out
}

/// one line per tool, for the app's Tools tab and 'uewasm-cli doctor'
pub fn status() -> Value {
    let root = find_emsdk();
    json!({
        "emsdk": root.as_deref().map(ps),
        "binaryen": root.as_deref().map_or(false, binaryen),
        "msvc": msvc(),
        "rust": cargo().map(|p| ps(&p)).or_else(|| rust_tools_ready().then(|| "built".to_string())),
        "ffmpeg": ffmpeg().map(|p| ps(&p)),
        "winget": winget().is_some(),
    })
}
