//! UEWASM's command line: recompile a game for the web, serve and test builds, check and install the toolchain.
#![allow(dead_code)]
mod build;
mod cdp;
mod deps;
mod media;
mod res;
mod upak;
mod util;
mod webtest;
mod zip;

use clap::{Args, Parser, Subcommand, ValueEnum};
use serde_json::json;
use std::io::{BufRead, IsTerminal, Write};
use std::path::{Path, PathBuf};
use std::process::Command;
use util::*;

#[derive(Parser)]
#[command(name = "uewasm-cli", about = "Windows x64 and x86 game recompiler for the web (Unreal Engine 3, 4 and 5)")]
struct Cli {
    #[command(subcommand)]
    cmd: Option<Cmd>,
}

#[derive(Subcommand)]
enum Cmd {
    /// recompile a game for the web
    Recomp(Recomp),
    /// show what's still missing for a build
    Coverage {
        build: PathBuf,
        #[arg(long, default_value_t = 30)]
        top: usize,
    },
    /// serve a web build locally
    Serve {
        build: PathBuf,
        #[arg(long, default_value_t = 8080)]
        port: u16,
        /// write the page's console log to this file as the game runs
        #[arg(long)]
        log: Option<PathBuf>,
        /// the address to listen on (0.0.0.0 lets other machines join)
        #[arg(long, default_value = "127.0.0.1")]
        host: String,
        /// run the built-in relay for online games (default: when the build was made with --relay)
        #[arg(long, overrides_with = "no_relay")]
        relay: bool,
        #[arg(long)]
        no_relay: bool,
        /// run the WISP server for real connections (default: when the build was made with --wisp)
        #[arg(long, overrides_with = "no_wisp")]
        wisp: bool,
        #[arg(long)]
        no_wisp: bool,
        /// hosts the WISP server may reach, comma-separated (*.example.com); default any public host
        #[arg(long)]
        wisp_allow: Option<String>,
    },
    /// run the built-in relay (and WISP server) alone, for builds made with --relay-url or --wisp-url
    Relay {
        #[arg(long, default_value = "0.0.0.0")]
        host: String,
        #[arg(long, default_value_t = 3000)]
        port: u16,
        /// also run the WISP server
        #[arg(long)]
        wisp: bool,
        /// hosts the WISP server may reach, comma-separated
        #[arg(long)]
        wisp_allow: Option<String>,
    },
    /// run a web build in a headless browser
    Test {
        build: PathBuf,
        #[arg(long, default_value_t = 90)]
        seconds: u64,
        #[arg(long)]
        report: Option<PathBuf>,
        /// page switches, e.g. "eosdebug=1&netdebug=1"
        #[arg(long, default_value = "")]
        query: String,
        /// keys to press, as seconds:key pairs, e.g. "190:Enter,200:Space"; a screenshot is saved just before each
        #[arg(long, default_value = "")]
        press: String,
        /// JavaScript run in the page before it loads (seed storage, stub an API)
        #[arg(long, default_value = "")]
        init: String,
    },
    /// check the toolchain
    Doctor {
        /// where the Emscripten SDK is
        #[arg(long)]
        emsdk: Option<String>,
        /// machine-readable status (and what the given settings still need) for front ends
        #[arg(long)]
        json: bool,
        #[arg(long)]
        native: bool,
        #[arg(long)]
        wasm_opt: bool,
        #[arg(long)]
        videos: bool,
    },
    /// download and install the Emscripten SDK (about 2.5 GB)
    InstallEmsdk {
        /// where to install it (default %USERPROFILE%\emsdk)
        dir: Option<PathBuf>,
    },
    /// install a tool a setting needs: emsdk, rust, msvc (with winget) or ffmpeg
    Install {
        what: String,
        /// where to install it (emsdk only)
        dir: Option<PathBuf>,
    },
    /// the original instructions at a guest address of a build (from its image.bin, as the build relocated them)
    Disasm {
        build: PathBuf,
        /// guest address, hex (0x optional)
        address: String,
        /// how many instructions
        #[arg(default_value_t = 24)]
        count: usize,
    },
    /// open the desktop app (the default with no arguments)
    Gui,
}

fn disasm(b: &Path, address: &str, count: usize) {
    let meta = std::fs::read_to_string(b.join("gen/meta.c")).unwrap_or_else(|e| die(format!("{}: {e}", b.join("gen/meta.c").display())));
    let lo = regex::Regex::new(r"IMAGE_LO = 0x([0-9a-f]+)ull").unwrap().captures(&meta).and_then(|c| u64::from_str_radix(&c[1], 16).ok())
        .unwrap_or_else(|| die("no IMAGE_LO in gen/meta.c"));
    let bits = if meta.contains("const int RT_G32 = 1;") { 32 } else { 64 };
    let a = u64::from_str_radix(address.trim_start_matches("0x").trim_start_matches("0X"), 16).unwrap_or_else(|_| die("the address should be hex"));
    let img = std::fs::read(b.join("image.bin")).unwrap_or_else(|e| die(e));
    let at = a.checked_sub(lo).filter(|o| (*o as usize) < img.len()).unwrap_or_else(|| die(format!("{a:#x} isn't in the image ({lo:#x}..{:#x})", lo + img.len() as u64))) as usize;
    let code = &img[at..(at + count * 15).min(img.len())];
    let mut d = iced_x86::Decoder::with_ip(bits, code, a, iced_x86::DecoderOptions::NONE);
    let mut f = iced_x86::IntelFormatter::new();
    use iced_x86::Formatter;
    let mut s = String::new();
    for ins in d.iter().take(count) {
        s.clear();
        f.format(&ins, &mut s);
        let off = (ins.ip() - a) as usize;
        let bytes: String = code[off..off + ins.len()].iter().map(|x| format!("{x:02x}")).collect();
        println!("{:x}  {bytes:<24} {s}", ins.ip());
    }
}

#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Renderer { Webgpu, Webgl2 }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Threads { Coop, Real }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Steam { Webworks, Account, Offline }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Eos { Standin, Real }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Epic { Auto, Account, Off }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Quality { Game, Low, Medium, High, Epic }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Priority { High, Low }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum Backend { Epic }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum VideoMode { Fast, Small }
#[derive(Clone, Copy, ValueEnum, PartialEq)]
enum GameDlls { Auto, None }

fn name<T: ValueEnum>(v: T) -> String {
    v.to_possible_value().map(|p| p.get_name().to_string()).unwrap_or_default()
}

#[derive(Args)]
struct Recomp {
    /// the game's .exe, or its install folder
    exe: PathBuf,
    #[arg(short, long)]
    output: Option<PathBuf>,
    #[arg(long, value_enum, default_value = "webgpu")]
    renderer: Renderer,
    #[arg(long, value_enum, default_value = "coop")]
    threads: Threads,
    /// where the Emscripten SDK is (default: the app's choice, EMSDK, or %USERPROFILE%\emsdk)
    #[arg(long)]
    emsdk: Option<String>,
    #[arg(long, default_value = "-O2", value_parser = ["-O2", "-O3", "-Os"], allow_hyphen_values = true)]
    opt: String,
    /// guest memory (up to ~3584 for a normal build; more turns on --mem64)
    #[arg(long, default_value_t = 3072)]
    mem_mb: u64,
    /// 64-bit WebAssembly memory, for games that need more than ~3.5 GB (up to ~15 GB); a little slower, needs a browser with memory64
    #[arg(long)]
    mem64: bool,
    /// most memory the running game may use in all (game, file cache, video memory)
    #[arg(long, default_value_t = 8.0)]
    mem_limit_gb: f64,
    /// render size (WxH); the page scales it to the window. native: the window's own pixel size, for sharp text (?res= in the page address overrides)
    #[arg(long, default_value = "1280x720")]
    resolution: String,
    /// largest file written for the web host (Cloudflare Pages allows 25)
    #[arg(long, default_value_t = 16)]
    part_mb: u64,
    /// compress game files (not audio) for a smaller download
    #[arg(long)]
    compress: bool,
    /// also write one .html holding the whole game, playable straight from disk with no server (cooperative threads only; best for small games)
    #[arg(long)]
    single_file: bool,
    /// compile shaders when first drawn (exact first frames, but freezes while compiling)
    #[arg(long)]
    sync_shaders: bool,
    /// Unreal games: graphics quality (game: the game's own setting); lower settings run faster here
    #[arg(long, value_enum, default_value = "game")]
    quality: Quality,
    /// extra launch argument for the game (repeat for more)
    #[arg(long, allow_hyphen_values = true)]
    game_arg: Vec<String>,
    /// show an FPS counter in the page (Ctrl+Shift+F toggles it)
    #[arg(long)]
    fps: bool,
    /// keep function names in the module, so browser profiles name the game's functions
    #[arg(long)]
    names: bool,
    /// high: fastest, tuned to this PC; low: far less CPU and memory so the PC stays usable (same result)
    #[arg(long, value_enum, default_value = "high")]
    priority: Priority,
    /// offline stand-in for a game's online services (epic: Epic accounts, profiles and menus)
    #[arg(long, value_enum)]
    backend: Option<Backend>,
    /// the game's AES-256 pak key (hex or base64): checked against its encrypted paks, which are packaged decrypted
    #[arg(long)]
    aes_key: Option<String>,
    /// with --backend: stand-in images and data for files the servers would send
    #[arg(long)]
    placeholders: bool,
    /// webworks: Steam features run on a WebWorks server (github.com/Stratus-Games/Webworks), with the offline Steam
    /// when none answers; account: the player signs in to their own Steam account in the page (real SteamID, name and
    /// tickets for the game's online services; lobbies and P2P as with webworks); offline: no Steam server
    #[arg(long, value_enum, default_value = "webworks")]
    steam: Steam,
    /// the WebWorks server the page signs in to (?webworks=URL in the page address overrides it)
    #[arg(long, default_value = "http://localhost:3000")]
    webworks_url: String,
    /// online games: `uewasm-cli serve` also runs the built-in relay at the game's own address, so everyone who opens
    /// it can play together (lobbies, P2P) with no other server
    #[arg(long)]
    relay: bool,
    /// online games: use a relay run elsewhere (`uewasm-cli relay`) at this address
    #[arg(long)]
    relay_url: Option<String>,
    /// real network connections (the game's own servers, the real EOS SDK) through the WISP server `uewasm-cli serve`
    /// runs at the game's address
    #[arg(long)]
    wisp: bool,
    /// real network connections through a WISP server elsewhere (wss://host/path/)
    #[arg(long)]
    wisp_url: Option<String>,
    /// with --steam account and WISP: sign in to Steam through the WISP server too, so the page never contacts Steam
    /// itself (asked at build time when not given)
    #[arg(long, overrides_with = "no_steam_wisp")]
    steam_wisp: bool,
    #[arg(long)]
    no_steam_wisp: bool,
    /// games with Epic Online Services: standin answers the EOS SDK in the page (offline sign-in, local lobbies); real
    /// recompiles the game's own EOS SDK, which signs in to Epic and joins real lobbies through WISP (turns on --wisp)
    #[arg(long, value_enum, default_value = "standin")]
    eos: Eos,
    /// Epic Games sign-in in the page (the player signs in on epicgames.com; the game gets the launcher's one-time code,
    /// through WISP): auto turns it on for games the Epic launcher installed, built with --eos real
    #[arg(long, value_enum, default_value = "auto")]
    epic: Epic,
    /// leave out the UEWASM debug panel toggle (\ or Insert in game)
    #[arg(long)]
    no_debug_panel: bool,
    /// run Binaryen on the linked module: ~10% smaller, needs ~20x its size in RAM
    #[arg(long)]
    wasm_opt: bool,
    /// re-encode game videos taller than this (e.g. 720); 0 keeps them
    #[arg(long, default_value_t = 0)]
    video_height: u32,
    /// fast encodes ~10x quicker; small makes ~30% smaller files
    #[arg(long, value_enum, default_value = "fast")]
    video_mode: VideoMode,
    /// also recompile DLLs shipped with the game
    #[arg(long, value_enum, default_value = "auto")]
    game_dlls: GameDlls,
    /// use built-in CRT instead of recompiling the Microsoft runtime DLLs
    #[arg(long)]
    no_system_dlls: bool,
    /// also build a native verification binary
    #[arg(long)]
    native: bool,
    /// skip the web build
    #[arg(long)]
    native_only: bool,
    /// run the native build afterwards
    #[arg(long)]
    run: bool,
    #[arg(long)]
    debug: bool,
    /// don't ask, use the given or default options
    #[arg(short, long)]
    yes: bool,
    /// print machine-readable progress lines (for front ends)
    #[arg(long)]
    progress: bool,
}

fn read_answer() -> Option<String> {
    let _ = std::io::stdout().flush();
    let mut l = String::new();
    if std::io::stdin().lock().read_line(&mut l).unwrap_or(0) == 0 {
        return None;
    }
    Some(l.trim().to_string())
}

/// a numbered choice on the terminal; names[i] with notes[i], the default marked
fn choose(what: &str, names: &[&str], notes: &[&str], default: usize) -> usize {
    println!("\n{what}:");
    for (i, n) in names.iter().enumerate() {
        println!("  {}{}. {n:7} {}", if i == default { "*" } else { " " }, i + 1, notes[i]);
    }
    loop {
        print!("pick 1-{} [{}]: ", names.len(), default + 1);
        let Some(v) = read_answer() else { return default };
        if v.is_empty() {
            return default;
        }
        if let Ok(i) = v.parse::<usize>() {
            if (1..=names.len()).contains(&i) {
                return i - 1;
            }
        }
        if let Some(i) = names.iter().position(|n| *n == v) {
            return i;
        }
    }
}

fn ask<T: ValueEnum + Copy + PartialEq>(what: &str, default: T, notes: &[&str]) -> T {
    let opts = T::value_variants();
    let names: Vec<String> = opts.iter().map(|o| name(*o)).collect();
    let refs: Vec<&str> = names.iter().map(String::as_str).collect();
    opts[choose(what, &refs, notes, opts.iter().position(|o| *o == default).unwrap_or(0))]
}

fn die(m: impl std::fmt::Display) -> ! {
    eprintln!("{m}");
    std::process::exit(1)
}

/// the game's executable in a folder: its *-Shipping.exe, else the biggest .exe outside redistributables
pub fn find_exe(p: &Path) -> Option<PathBuf> {
    if p.is_file() {
        return Some(p.to_path_buf());
    }
    let all: Vec<PathBuf> = files_under(p).into_iter().filter(|x| x.extension().map_or(false, |e| e.eq_ignore_ascii_case("exe"))).collect();
    let ship: Vec<&PathBuf> = all.iter().filter(|x| x.file_name().unwrap_or_default().to_string_lossy().ends_with("-Shipping.exe")).collect();
    let pool: Vec<&PathBuf> = if !ship.is_empty() { ship } else { all.iter().filter(|x| !x.components().any(|c| c.as_os_str() == "Redist")).collect() };
    pool.into_iter().max_by_key(|x| std::fs::metadata(x).map_or(0, |m| m.len())).cloned()
}

fn recomp(mut a: Recomp) {
    if let Some(e) = &a.emsdk {
        std::env::set_var("UEWASM_EMSDK", e);
    }
    let prog_on = a.progress;
    let mut prog = |stage: &str, done: f64, total: f64| {
        if prog_on {
            println!("@@progress\t{stage}\t{done}\t{total}");
            let _ = std::io::stdout().flush();
        }
    };
    let mut log = |l: &str| {
        println!("{l}");
        let _ = std::io::stdout().flush();
    };
    let exe = find_exe(&a.exe).unwrap_or_else(|| die(format!("no .exe in {}", a.exe.display())));
    if a.mem_mb > 3584 && !a.mem64 {
        println!("{} MB of game memory doesn't fit a 32-bit module (4 GB in all): building with 64-bit memory (--mem64)", a.mem_mb);
        a.mem64 = true;
    }
    if a.mem64 && a.mem_mb > 15 * 1024 {
        die("--mem-mb: browsers give a 64-bit module at most 16 GB, so game memory can be at most 15360 MB");
    }
    let tty = std::io::stdin().is_terminal();
    if !a.yes && tty {
        a.renderer = ask("renderer", a.renderer, &["WebGPU (faster, compute shaders; Chrome/Edge/Safari 26+; falls back to WebGL 2)",
                                                   "WebGL 2 only (smaller, works in every modern browser)"]);
        a.threads = ask("threads", a.threads, &["Cooperative: all game threads share one core, no special hosting needed",
                                                "Real threads: game threads on several cores (WebGPU only; needs https or localhost, isolation set up automatically)"]);
        let opts = ["-O2", "-O3", "-Os"];
        let i = choose("opt", &opts, &["Balanced build time and speed", "Fastest code, slower build", "Smallest download"],
                       opts.iter().position(|o| *o == a.opt).unwrap_or(0));
        a.opt = opts[i].into();
    }
    let out = a.output.clone().unwrap_or_else(|| std::env::current_dir().unwrap_or_default().join("builds").join(exe.file_stem().unwrap_or_default()));
    if a.epic == Epic::Account && !(a.wisp || a.wisp_url.is_some()) {
        println!("--epic account signs in to Epic through WISP: turning on --wisp (the WISP server `uewasm-cli serve` runs)");
        a.wisp = true;
    }
    if a.eos == Eos::Real && !(a.wisp || a.wisp_url.is_some()) {
        println!("--eos real reaches Epic's servers through WISP: turning on --wisp (the WISP server `uewasm-cli serve` runs)");
        a.wisp = true;
    }
    let wisp_on = a.wisp || a.wisp_url.is_some();
    if a.steam_wisp && !wisp_on {
        die("--steam-wisp needs --wisp or --wisp-url");
    }
    let mut steam_wisp = a.steam_wisp;
    if a.steam == Steam::Account && wisp_on && !a.steam_wisp && !a.no_steam_wisp && !a.yes && tty {
        print!("Send the Steam sign-in through the WISP server too (the page never contacts Steam itself)? [y/N] ");
        steam_wisp = read_answer().map_or(false, |v| v.to_lowercase().starts_with('y'));
    }
    let (root, found) = build::game_dlls(&exe, a.eos == Eos::Real, &mut log).unwrap_or_else(|e| die(e));
    let (extra, search): (Vec<PathBuf>, Vec<String>) = if a.game_dlls == GameDlls::Auto {
        let mut s: Vec<String> = found.iter().filter_map(|d| d.parent().map(ps)).collect();
        s.sort();
        s.dedup();
        (found, s)
    } else {
        (vec![], vec![])
    };
    let exe_rel = exe.strip_prefix(&root).unwrap_or(&exe).to_string_lossy().replace('\\', "/");
    let mut aes = None;
    if let Some(k) = &a.aes_key {
        let key = upak::parse_key(k).unwrap_or_else(|| die("--aes-key should be 64 hex digits (0x optional) or base64 of 32 bytes"));
        let bad: Vec<String> = upak::encrypted_paks(&root).into_iter().filter(|p| !upak::check_key(p, &key)).take(5)
            .map(|p| p.file_name().unwrap_or_default().to_string_lossy().into()).collect();
        if !bad.is_empty() {
            if a.progress {
                println!("@@error\twrongkey\tThe AES key is wrong for {}.", bad.join(", "));
            }
            die(format!("the AES key is wrong for {}; give the game's own correct key", bad.join(", ")));
        }
        aes = Some(key);
    }
    set_low_priority(a.priority == Priority::Low);
    lower_this_process();
    println!("{}", res::describe());
    let code_mb = (std::fs::metadata(&exe).map_or(0, |m| m.len()) + extra.iter().map(|p| std::fs::metadata(p).map_or(0, |m| m.len())).sum::<u64>()) as f64 / 1048576.0;
    println!("{}: {} game DLLs, {code_mb:.0} MB of machine code to translate", exe.file_name().unwrap_or_default().to_string_lossy(), extra.len());
    let r = build::recompile(&exe, &out, a.mem_mb, &search, &extra, !a.no_system_dlls, a.eos == Eos::Real, &mut log, &mut prog).unwrap_or_else(|e| die(e));
    println!("{} functions in {} modules, {} unsupported instructions, {} imports not implemented yet (details in {})",
             r["functions"], r["modules"].as_array().map_or(0, |m| m.len()), r["unsupported_total"],
             r["missing_imports"].as_array().map_or(0, |m| m.len()), out.join("coverage.json").display());
    let opts = json!({"renderer": name(a.renderer), "threads": name(a.threads), "opt": a.opt, "mem_mb": a.mem_mb, "mem64": a.mem64});
    let _ = std::fs::write(out.join("options.json"), serde_json::to_string_pretty(&opts).unwrap());
    if a.native {
        let native = build::build_native(&out, a.debug, a.mem64, &mut log, &mut prog).unwrap_or_else(|e| die(e));
        if a.run {
            let _ = Command::new(native).arg("--image").arg(out.join("image.bin")).arg("--root").arg(&root).arg("--save").arg(out.join("save"))
                .arg("--exe").arg(&exe_rel).status();
        }
    }
    if !a.native_only {
        let size = if a.resolution.eq_ignore_ascii_case("native") {
            None
        } else {
            let p: Vec<u32> = a.resolution.to_lowercase().split('x').filter_map(|x| x.trim().parse().ok()).collect();
            if p.len() != 2 {
                die("--resolution should look like 1280x720, or native");
            }
            Some((p[0], p[1]))
        };
        let game_args = if a.game_arg.is_empty() {
            None
        } else {
            let mut g = build::default_args(&root.join(&exe_rel));
            g.extend(a.game_arg.iter().cloned());
            Some(g)
        };
        let o = build::Wasm {
            opt: a.opt.clone(), threads: name(a.threads), renderer: name(a.renderer), game_root: Some(root.clone()), exe_rel: Some(exe_rel.clone()),
            size, part_mb: a.part_mb, compress: a.compress, video_height: a.video_height, video_mode: name(a.video_mode), wasm_opt: a.wasm_opt,
            show_fps: a.fps, async_shaders: !a.sync_shaders, debug_panel: !a.no_debug_panel, backend: a.backend.map(name), placeholders: a.placeholders,
            aes_key: aes, mem_limit_gb: a.mem_limit_gb, steam: name(a.steam), webworks_url: a.relay_url.clone().unwrap_or(a.webworks_url.clone()),
            relay: a.relay, wisp: a.wisp, wisp_url: a.wisp_url.clone(), steam_wisp, single_file: a.single_file, quality: name(a.quality),
            names: a.names, mem64: a.mem64, game_args, epic: name(a.epic), eos_real: a.eos == Eos::Real, ..Default::default()
        };
        let web = build::build_wasm(&out, &o, &mut log, &mut prog).unwrap_or_else(|e| die(e));
        println!("web build ready in {} (upload the whole folder; test it with: uewasm-cli test {})", web.display(), web.display());
        if a.progress {
            println!("@@done\t{}", web.display());
        }
        if a.single_file {
            println!("single-file build in {} (open the .html directly)", out.join("single").display());
        }
    }
}

fn coverage(b: &Path, top: usize) {
    let t = std::fs::read_to_string(b.join("coverage.json")).unwrap_or_else(|e| die(format!("{}: {e}", b.join("coverage.json").display())));
    let r: serde_json::Value = serde_json::from_str(&t).unwrap_or_else(|e| die(e));
    println!("{}\n  functions {}, unsupported instructions {}", r["exe"].as_str().unwrap_or(""), r["functions"], r["unsupported_total"]);
    if let Some(u) = r["unsupported"].as_object() {
        for (k, v) in u.iter().take(top) {
            let ex: Vec<&str> = r["unsupported_examples"][k].as_array().map(|a| a.iter().take(2).filter_map(|x| x.as_str()).collect()).unwrap_or_default();
            println!("  {:>8} {k}  e.g. {}", v.to_string(), ex.join(", "));
        }
    }
    let m = r["missing_imports"].as_array().cloned().unwrap_or_default();
    println!("  missing imports: {}", m.len());
    for x in m.iter().take(top) {
        println!("    {}", x.as_str().unwrap_or(""));
    }
}

fn run_child(mut c: Command) -> ! {
    match c.status() {
        Ok(s) => std::process::exit(s.code().unwrap_or(1)),
        Err(e) => die(e),
    }
}

fn uewserve() -> PathBuf {
    rust_tool("uewserve", &mut |l| println!("{l}")).unwrap_or_else(|e| die(e))
}

fn gui() -> ! {
    let me = std::env::current_exe().unwrap_or_default();
    let dir = me.parent().unwrap_or(Path::new("."));
    let cands = [dir.join("UEWASM.exe"), home().join("app/target/x86_64-pc-windows-msvc/release/uewasm-app.exe"), home().join("app/target/release/uewasm-app.exe")];
    match cands.iter().find(|p| p.exists()) {
        Some(p) => {
            Command::new(p).spawn().unwrap_or_else(|e| die(e));
            std::process::exit(0)
        }
        None => die("the desktop app isn't built: cargo build --release --target x86_64-pc-windows-msvc --manifest-path app/Cargo.toml"),
    }
}

fn main() {
    let cli = Cli::parse();
    let Some(cmd) = cli.cmd else { gui() };
    match cmd {
        Cmd::Recomp(r) => recomp(r),
        Cmd::Coverage { build, top } => coverage(&build, top),
        Cmd::Serve { build, port, log, host, relay, no_relay, wisp, no_wisp, wisp_allow } => {
            let mut c = Command::new(uewserve());
            c.arg(&build).args(["--port", &port.to_string(), "--host", &host]);
            if wisp {
                c.arg("--wisp");
            } else if no_wisp {
                c.arg("--no-wisp");
            }
            if let Some(w) = wisp_allow {
                c.args(["--wisp-allow", &w]);
            }
            if let Some(l) = log {
                c.arg("--log").arg(l);
            }
            if relay {
                c.arg("--relay");
            } else if no_relay {
                c.arg("--no-relay");
            }
            run_child(c)
        }
        Cmd::Relay { host, port, wisp, wisp_allow } => {
            let mut c = Command::new(uewserve());
            c.args(["--relay-only", "--host", &host, "--port", &port.to_string()]);
            if wisp {
                c.arg("--wisp");
            }
            if let Some(w) = wisp_allow {
                c.args(["--wisp-allow", &w]);
            }
            run_child(c)
        }
        Cmd::Test { build, seconds, report, query, press, init } => {
            let r = webtest::run(&build, seconds, report.as_deref(), &query, &press, &init).unwrap_or_else(|e| die(e));
            println!("{}", webtest::summary(&r));
            std::process::exit(if r["passed"].as_bool() == Some(true) { 0 } else { 1 });
        }
        Cmd::Doctor { emsdk, json: as_json, native, wasm_opt, videos } => {
            if let Some(e) = emsdk {
                std::env::set_var("UEWASM_EMSDK", e);
            }
            let st = deps::status();
            if as_json {
                println!("{}", json!({"status": st, "missing": deps::missing(native, wasm_opt, videos)}));
                return;
            }
            let on = |k: &str| st[k].as_str().map(String::from);
            println!("UEWASM {}", env!("CARGO_PKG_VERSION"));
            println!("  emscripten (web builds): {}", on("emsdk").unwrap_or("not found (uewasm-cli install-emsdk, or --emsdk PATH)".into()));
            println!("  binaryen (--wasm-opt): {}", if st["binaryen"].as_bool() == Some(true) { "ok" } else { "not found" });
            println!("  MSVC (--native test builds): {}", on("msvc").unwrap_or("not found".into()));
            println!("  Rust (the recompiler's tools): {}", on("rust").unwrap_or("not found (https://rustup.rs)".into()));
            println!("  ffmpeg (--video-height): {}", on("ffmpeg").unwrap_or("not found (uewasm-cli install ffmpeg)".into()));
        }
        Cmd::InstallEmsdk { dir } => match deps::install("emsdk", dir.as_deref(), &mut |l| println!("{l}")) {
            Ok(m) => println!("{m}"),
            Err(e) => die(e),
        },
        Cmd::Install { what, dir } => match deps::install(&what, dir.as_deref(), &mut |l| {
            println!("{l}");
            let _ = std::io::stdout().flush();
        }) {
            Ok(m) => println!("{m}"),
            Err(e) => die(e),
        },
        Cmd::Disasm { build, address, count } => disasm(&build, &address, count),
        Cmd::Gui => gui(),
    }
}
