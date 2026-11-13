// UEWASM desktop app (egui): pick a game, choose build settings, and recompile with live progress and a time estimate.
// The build itself runs in the UEWASM command line (uewasm-cli.exe beside this app, or uewasm.py with Python), which
// reports its stages on stdout (--progress).
#![windows_subsystem = "windows"]

mod build;
mod serve;
mod settings;

use eframe::egui;
use std::path::{Path, PathBuf};
use std::time::Instant;

use build::{Estimator, Job, Msg};
use settings::Settings;

const RENDERERS: &[(&str, &str)] = &[("WebGPU (recommended)", "webgpu"), ("WebGL 2 (works everywhere)", "webgl2")];
const THREADS: &[(&str, &str)] = &[("Cooperative (no special hosting)", "coop"), ("Real threads (several cores; WebGPU, https or localhost)", "real")];
const OPTS: &[(&str, &str)] = &[("Balanced (-O2)", "-O2"), ("Fastest code (-O3, slower build)", "-O3"), ("Smallest download (-Os)", "-Os")];
const QUALITY: &[(&str, &str)] = &[("Game's own setting", "game"), ("Low (fastest here)", "low"), ("Medium", "medium"), ("High", "high"), ("Epic", "epic")];
const LIFTERS: &[(&str, &str)] = &[("Python (original)", "python"), ("Rust (same result, much faster; needs Rust installed)", "rust")];
const PRIORITIES: &[(&str, &str)] = &[("High (fastest, tuned to this PC)", "high"), ("Low (keeps the PC usable while it builds)", "low")];
const BACKENDS: &[(&str, &str)] = &[("None (games play offline as usual)", ""), ("Epic online services, offline (sign-in, profiles, menus)", "epic")];
/// video choice: (label, height (0 keep, -1 render height), mode)
const VIDEOS: &[(&str, i32, &str)] = &[("Keep original videos", 0, "fast"), ("Shrink to render height, fast encode", -1, "fast"),
    ("Shrink to render height, smallest files (slow)", -1, "small"), ("Shrink to 1080p, fast encode", 1080, "fast")];
const RESOLUTIONS: &[&str] = &["1280x720", "1600x900", "1920x1080", "960x540"];

fn main() -> eframe::Result {
    let icon = eframe::icon_data::from_png_bytes(include_bytes!("../../uewasm/assets/logo_44.png")).ok();
    let mut vp = egui::ViewportBuilder::default().with_title("UEWASM").with_inner_size([900.0, 760.0]).with_min_inner_size([780.0, 640.0]);
    if let Some(i) = icon {
        vp = vp.with_icon(i);
    }
    eframe::run_native("UEWASM", eframe::NativeOptions { viewport: vp, ..Default::default() }, Box::new(|_cc| Ok(Box::new(App::new()))))
}

struct App {
    s: Settings,
    info: String,
    described: String,
    job: Option<Job>,
    est: Option<Estimator>,
    stage: String,
    stage_frac: Option<f32>,
    log: String,
    started: Option<Instant>,
    finished: Option<String>,
    last_web: Option<PathBuf>,
    server: Option<serve::Server>,
    tools: String,
    error: Option<String>,
}

fn combo(ui: &mut egui::Ui, id: &str, value: &mut String, items: &[&str]) {
    egui::ComboBox::from_id_salt(id).width(330.0).selected_text(value.as_str()).show_ui(ui, |ui| {
        for it in items {
            ui.selectable_value(value, it.to_string(), *it);
        }
    });
}

fn pick<'a>(table: &'a [(&'a str, &'a str)], label: &str) -> &'a str {
    table.iter().find(|x| x.0 == label).unwrap_or(&table[0]).1
}

fn fmt_time(s: f64) -> String {
    let s = s.max(0.0) as u64;
    if s < 60 { format!("{s}s") } else if s < 3600 { format!("{}m {:02}s", s / 60, s % 60) } else { format!("{}h {:02}m", s / 3600, s % 3600 / 60) }
}

impl App {
    fn new() -> App {
        let s = Settings::load();
        let tools = build::check_tools();
        let mut a = App { s, info: String::new(), described: "\u{0}".into(), job: None, est: None, stage: "Ready".into(), stage_frac: Some(0.0),
            log: String::new(), started: None, finished: None, last_web: None, server: None, tools, error: None };
        a.describe();
        a
    }

    fn describe(&mut self) {
        if self.described == self.s.game {
            return;
        }
        self.described = self.s.game.clone();
        match build::find_exe(Path::new(self.s.game.trim().trim_matches('"'))) {
            None => {
                self.info = if self.s.game.trim().is_empty() { "Pick the game's install folder (the main *-Shipping.exe is found automatically).".into() }
                            else { "No Windows executable found there.".into() };
            }
            Some(exe) => {
                let mb = std::fs::metadata(&exe).map(|m| m.len()).unwrap_or(0) as f64 / 1048576.0;
                let engine = if exe.ancestors().take(5).any(|p| p.join("Engine").is_dir()) { "Unreal Engine" } else { "unknown engine" };
                self.info = format!("{} · {:.0} MB · {}", exe.file_name().unwrap_or_default().to_string_lossy(), mb, engine);
                if self.s.out.trim().is_empty() {
                    let home = std::env::var("USERPROFILE").unwrap_or_else(|_| ".".into());
                    self.s.out = Path::new(&home).join("UEWASM Builds").join(exe.file_stem().unwrap_or_default()).to_string_lossy().into();
                }
            }
        }
    }

    fn aes_note(&self) -> (&'static str, bool) {
        if !self.s.aes_on {
            return ("", true);
        }
        let t = self.s.aes_key.trim();
        if t.is_empty() {
            ("Enter the game's AES key (64 hex digits, 0x optional) to build", false)
        } else if build::parse_key(t).is_none() {
            ("That isn't a 256-bit AES key (64 hex digits or base64)", false)
        } else {
            ("Checked against the game's encrypted paks when the build starts", true)
        }
    }

    fn start(&mut self) {
        self.error = None;
        let Some(exe) = build::find_exe(Path::new(self.s.game.trim().trim_matches('"'))) else {
            self.error = Some("Choose the game's install folder or .exe first.".into());
            return;
        };
        let Some((w, h)) = self.s.resolution.to_lowercase().split_once('x').and_then(|(a, b)| Some((a.trim().parse::<u32>().ok()?, b.trim().parse::<u32>().ok()?))) else {
            self.error = Some("Resolution should look like 1280x720.".into());
            return;
        };
        let threads = pick(THREADS, &self.s.threads);
        if threads == "real" && self.s.single {
            self.error = Some("A single .html file can't use real threads (pages opened from disk can't be cross-origin isolated). Pick cooperative threading, or untick the single-file option.".into());
            return;
        }
        if !self.aes_note().1 {
            return;
        }
        let video = VIDEOS.iter().find(|v| v.0 == self.s.videos).unwrap_or(&VIDEOS[0]);
        let out = if self.s.out.trim().is_empty() { PathBuf::from("UEWASM Builds").join(exe.file_stem().unwrap_or_default()) } else { PathBuf::from(self.s.out.trim()) };
        let mut args: Vec<String> = vec!["recomp".into(), exe.to_string_lossy().into(), "-o".into(), out.to_string_lossy().into(), "-y".into(), "--progress".into(),
            "--renderer".into(), pick(RENDERERS, &self.s.renderer).into(), "--threads".into(), threads.into(), "--opt".into(), pick(OPTS, &self.s.opt).into(),
            "--lifter".into(), pick(LIFTERS, &self.s.lifter).into(), "--priority".into(), pick(PRIORITIES, &self.s.priority).into(), "--quality".into(), pick(QUALITY, &self.s.quality).into(),
            "--mem-mb".into(), self.s.mem.to_string(), "--mem-limit-gb".into(), self.s.mem_limit.to_string(), "--resolution".into(), format!("{w}x{h}"),
            "--part-mb".into(), self.s.part.to_string(), "--game-dlls".into(), if self.s.game_dlls { "auto".into() } else { "none".into() }];
        let video_h = if video.1 < 0 { h as i32 } else { video.1 };
        if video_h > 0 {
            args.extend(["--video-height".into(), video_h.to_string(), "--video-mode".into(), video.2.into()]);
        }
        let backend = pick(BACKENDS, &self.s.backend);
        if !backend.is_empty() {
            args.extend(["--backend".into(), backend.into()]);
            if self.s.placeholders { args.push("--placeholders".into()); }
        }
        for (on, flag) in [(!self.s.sys_dlls, "--no-system-dlls"), (self.s.native, "--native"), (self.s.compress, "--compress"), (self.s.wasm_opt, "--wasm-opt"),
                           (self.s.show_fps, "--fps"), (!self.s.async_shaders, "--sync-shaders"), (!self.s.debug_panel, "--no-debug-panel"), (self.s.single, "--single-file")] {
            if on { args.push(flag.into()); }
        }
        if self.s.aes_on {
            args.extend(["--aes-key".into(), self.s.aes_key.trim().into()]);
        }
        self.s.save();
        self.log.clear();
        self.finished = None;
        self.last_web = None;
        self.stage = "Starting…".into();
        self.stage_frac = None;
        self.started = Some(Instant::now());
        let stages: Vec<&str> = build::STAGES.iter().copied().filter(|s| self.s.native || !s.starts_with("native")).collect();
        self.est = Some(Estimator::new(&stages, 1.0));
        match Job::start(&args) {
            Ok(j) => self.job = Some(j),
            Err(e) => {
                self.finish("Failed");
                self.error = Some(e);
            }
        }
    }

    fn finish(&mut self, text: &str) {
        self.stage = text.into();
        self.stage_frac = Some(if text.starts_with("Done") { 1.0 } else { 0.0 });
        self.finished = Some(format!("elapsed {}", fmt_time(self.started.map(|t| t.elapsed().as_secs_f64()).unwrap_or(0.0))));
        self.job = None;
    }

    fn pump(&mut self) {
        let Some(job) = &mut self.job else { return };
        let exited = job.exited();
        let msgs = if exited.is_some() { job.finish_output() } else { job.drain() };
        for m in msgs {
            match m {
                Msg::Log(l) => {
                    // the size of the machine code to translate, which the time estimate scales by
                    if let Some(mb) = l.split(" game DLLs, ").nth(1).and_then(|r| r.split(" MB").next()).and_then(|x| x.trim().parse::<f64>().ok()) {
                        if let Some(e) = &mut self.est { e.input_mb = mb.max(1.0); }
                    }
                    self.log.push_str(&l);
                    self.log.push('\n');
                }
                Msg::Progress(stage, done, total) => {
                    if let Some(e) = &mut self.est { e.update(&stage, done, total); }
                    let mut label = build::label(&stage).to_string();
                    if total > 0.0 && stage == "lift" { label += &format!("  ({done:.0}/{total:.0} files)"); }
                    self.stage = label;
                    self.stage_frac = if total > 0.0 { Some((done / total) as f32) } else { None };
                }
                Msg::Error(kind, text) => {
                    self.error = Some(if kind == "wrongkey" { format!("{text} Enter the game's own correct AES key.") } else { text });
                }
                Msg::Done(web) => {
                    self.last_web = Some(PathBuf::from(&web));
                    self.log.push_str(&format!("\nWeb build ready: {web}\nUpload the whole folder to a static host, or press 'Play in browser'.\n"));
                }
            }
        }
        if let Some(code) = exited {
            if self.last_web.is_some() && code == 0 {
                if let Some(e) = &self.est { e.remember(); }
                let t = fmt_time(self.started.map(|t| t.elapsed().as_secs_f64()).unwrap_or(0.0));
                self.finish(&format!("Done in {t}"));
            } else if self.job.as_ref().map_or(false, |j| j.cancelled) {
                self.finish("Cancelled");
            } else {
                self.finish("Failed");
                if self.error.is_none() {
                    let tail: Vec<&str> = self.log.lines().rev().take(6).collect();
                    self.error = Some(tail.into_iter().rev().collect::<Vec<_>>().join("\n"));
                }
            }
        }
    }

    fn web_dir(&self) -> Option<PathBuf> {
        if let Some(w) = &self.last_web { return Some(w.clone()); }
        let w = Path::new(self.s.out.trim()).join("web");
        w.join("index.html").exists().then_some(w)
    }
}

impl eframe::App for App {
    fn ui(&mut self, ui: &mut egui::Ui, _frame: &mut eframe::Frame) {
        self.pump();
        self.describe();
        if self.job.is_some() {
            ui.ctx().request_repaint_after(std::time::Duration::from_millis(200));
        }
        let running = self.job.is_some();
        egui::CentralPanel::default().show_inside(ui, |ui| {
            ui.horizontal(|ui| {
                ui.heading(egui::RichText::new("UEWASM").strong());
                ui.label(egui::RichText::new("Windows x64 games to WebAssembly").weak());
            });
            ui.add_space(6.0);
            ui.group(|ui| {
                ui.label(egui::RichText::new("Game").strong());
                egui::Grid::new("game").num_columns(2).spacing([8.0, 6.0]).show(ui, |ui| {
                    ui.label("Install folder or .exe");
                    ui.horizontal(|ui| {
                        ui.add(egui::TextEdit::singleline(&mut self.s.game).desired_width(470.0));
                        if ui.button("Folder…").clicked() {
                            if let Some(d) = rfd::FileDialog::new().set_title("Game install folder").pick_folder() { self.s.game = d.to_string_lossy().into(); }
                        }
                        if ui.button(".exe…").clicked() {
                            if let Some(f) = rfd::FileDialog::new().add_filter("Windows executable", &["exe"]).pick_file() { self.s.game = f.to_string_lossy().into(); }
                        }
                    });
                    ui.end_row();
                    ui.label("");
                    ui.label(egui::RichText::new(&self.info).weak());
                    ui.end_row();
                    ui.label("Output folder");
                    ui.horizontal(|ui| {
                        ui.add(egui::TextEdit::singleline(&mut self.s.out).desired_width(470.0));
                        if ui.button("Browse…").clicked() {
                            if let Some(d) = rfd::FileDialog::new().set_title("Where to put the build").pick_folder() { self.s.out = d.to_string_lossy().into(); }
                        }
                    });
                    ui.end_row();
                });
            });
            ui.add_space(6.0);
            ui.group(|ui| {
                ui.label(egui::RichText::new("Settings").strong());
                egui::Grid::new("settings").num_columns(4).spacing([10.0, 6.0]).show(ui, |ui| {
                    let labels = |t: &[(&'static str, &'static str)]| -> Vec<&'static str> { t.iter().map(|x| x.0).collect() };
                    ui.label("Renderer"); combo(ui, "renderer", &mut self.s.renderer, &labels(RENDERERS));
                    ui.label("Resolution");
                    egui::ComboBox::from_id_salt("res").selected_text(self.s.resolution.as_str()).show_ui(ui, |ui| {
                        for r in RESOLUTIONS { ui.selectable_value(&mut self.s.resolution, r.to_string(), *r); }
                    });
                    ui.end_row();
                    ui.label("Threading"); combo(ui, "threads", &mut self.s.threads, &labels(THREADS));
                    ui.label("Guest memory (MB)"); ui.add(egui::DragValue::new(&mut self.s.mem).range(1024..=3584).speed(256.0));
                    ui.end_row();
                    ui.label("Optimisation"); combo(ui, "opt", &mut self.s.opt, &labels(OPTS));
                    ui.label("Largest file (MB)"); ui.add(egui::DragValue::new(&mut self.s.part).range(4..=24));
                    ui.end_row();
                    ui.label("Recompiler"); combo(ui, "lifter", &mut self.s.lifter, &labels(LIFTERS));
                    ui.label("Memory limit when run (GB)"); ui.add(egui::DragValue::new(&mut self.s.mem_limit).range(4..=32));
                    ui.end_row();
                    ui.label("Graphics (Unreal)"); combo(ui, "quality", &mut self.s.quality, &labels(QUALITY));
                    ui.end_row();
                    ui.label("Videos"); combo(ui, "videos", &mut self.s.videos, &VIDEOS.iter().map(|v| v.0).collect::<Vec<_>>());
                    ui.label(egui::RichText::new("Smaller videos download faster").weak()); ui.label("");
                    ui.end_row();
                    ui.label("Build priority"); combo(ui, "priority", &mut self.s.priority, &labels(PRIORITIES));
                    ui.label(egui::RichText::new("Same result either way").weak()); ui.label("");
                    ui.end_row();
                    ui.label("Online services"); combo(ui, "backend", &mut self.s.backend, &labels(BACKENDS));
                    ui.checkbox(&mut self.s.placeholders, "Stand-in files for what servers would send"); ui.label("");
                    ui.end_row();
                });
                ui.horizontal_wrapped(|ui| {
                    ui.checkbox(&mut self.s.game_dlls, "Recompile the game's own DLLs");
                    ui.checkbox(&mut self.s.sys_dlls, "Recompile Microsoft runtime DLLs");
                    ui.checkbox(&mut self.s.native, "Also build a native test .exe (needs Visual Studio)");
                });
                ui.horizontal_wrapped(|ui| {
                    ui.checkbox(&mut self.s.compress, "Compress game files (smaller download)");
                    ui.checkbox(&mut self.s.wasm_opt, "Shrink code with Binaryen (10% smaller, needs lots of RAM)");
                });
                ui.checkbox(&mut self.s.single, "Also make a single .html that plays straight from disk (no server; cooperative threads only)");
                ui.checkbox(&mut self.s.show_fps, "Show an FPS counter (Ctrl+Shift+F toggles it in game)");
                ui.checkbox(&mut self.s.async_shaders, "Compile shaders in the background (shorter freezes when new effects first appear)");
                ui.checkbox(&mut self.s.debug_panel, "UEWASM debug toggle (\\ or Insert opens the debug panel in game)");
                ui.horizontal(|ui| {
                    ui.checkbox(&mut self.s.aes_on, "Decrypt AES encrypted content");
                    ui.add_enabled(self.s.aes_on, egui::TextEdit::singleline(&mut self.s.aes_key).desired_width(520.0).hint_text("0x + 64 hex digits"));
                });
                let (note, _) = self.aes_note();
                if !note.is_empty() { ui.label(egui::RichText::new(note).weak()); }
            });
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                let can = !running && self.aes_note().1;
                if ui.add_enabled(can, egui::Button::new(egui::RichText::new("Recompile").strong()).min_size(egui::vec2(120.0, 30.0))).clicked() {
                    self.start();
                }
                if ui.add_enabled(running, egui::Button::new("Cancel")).clicked() {
                    if let Some(j) = &mut self.job { j.cancel(); }
                    self.stage = "Cancelling…".into();
                }
                ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                    let web = self.web_dir();
                    if ui.add_enabled(web.is_some() && !running, egui::Button::new("Play in browser")).clicked() {
                        if let Some(w) = web {
                            self.server = None;
                            match serve::Server::start(&w) {
                                Ok(s) => {
                                    let url = format!("http://localhost:{}/index.html", s.port);
                                    self.log.push_str(&format!("Serving {} at {url} (closes with this window)\n", w.display()));
                                    build::open(&url);
                                    self.server = Some(s);
                                }
                                Err(e) => self.error = Some(format!("Couldn't start the local server: {e}")),
                            }
                        }
                    }
                    if ui.button("Open output folder").clicked() && Path::new(self.s.out.trim()).exists() {
                        build::open(self.s.out.trim());
                    }
                });
            });
            ui.add_space(8.0);
            ui.group(|ui| {
                ui.horizontal(|ui| {
                    ui.label(&self.stage);
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if let (Some(t), Some(e), true) = (self.started, &self.est, running) {
                            ui.label(egui::RichText::new(format!("elapsed {} · about {} left", fmt_time(t.elapsed().as_secs_f64()), fmt_time(e.left()))).weak());
                        } else if let Some(f) = &self.finished {
                            ui.label(egui::RichText::new(f).weak());
                        }
                    });
                });
                let overall = if running { self.est.as_ref().map_or(0.0, |e| e.overall()) as f32 } else if self.stage.starts_with("Done") { 1.0 } else { 0.0 };
                ui.add(egui::ProgressBar::new(overall));
                match self.stage_frac {
                    Some(f) => { ui.add(egui::ProgressBar::new(f)); }
                    None => { ui.add(egui::ProgressBar::new(0.0).animate(true)); }
                }
                egui::ScrollArea::vertical().stick_to_bottom(true).auto_shrink([false, false]).max_height(ui.available_height() - 24.0).show(ui, |ui| {
                    ui.add(egui::TextEdit::multiline(&mut self.log.as_str()).font(egui::TextStyle::Monospace).desired_width(f32::INFINITY));
                });
            });
            ui.label(egui::RichText::new(&self.tools).weak().small());
        });
        if let Some(e) = self.error.clone() {
            let mut open = true;
            egui::Window::new("UEWASM").collapsible(false).resizable(false).anchor(egui::Align2::CENTER_CENTER, [0.0, 0.0]).open(&mut open).show(ui.ctx(), |ui| {
                ui.label(&e);
                if ui.button("OK").clicked() { self.error = None; }
            });
            if !open { self.error = None; }
        }
    }

    fn on_exit(&mut self) {
        if let Some(j) = &mut self.job { j.cancel(); }
        self.s.save();
    }
}
