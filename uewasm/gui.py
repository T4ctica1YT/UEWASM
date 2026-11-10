"""Desktop front end: pick a game, choose build settings, recompile with live progress and a time estimate."""
import json
import os
import queue
import sys
import threading
import time
import traceback
import webbrowser
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from . import __version__

TITLE = "UEWASM"
RENDERERS = {"WebGPU (recommended)": "webgpu", "WebGL 2 (works everywhere)": "webgl2"}
THREADS = {"Cooperative (no special hosting)": "coop", "Real threads (needs COOP/COEP headers)": "real"}
OPTS = {"Balanced (-O2)": "-O2", "Fastest code (-O3, slower build)": "-O3", "Smallest download (-Os)": "-Os"}
RESOLUTIONS = ["1280x720", "1600x900", "1920x1080", "960x540"]
BACKENDS = {"None (games play offline as usual)": None, "Epic online services, offline (sign-in, profiles, menus)": "epic"}
PRIORITIES = {"High (fastest, tuned to this PC)": "high", "Low (keeps the PC usable while it builds)": "low"}
VIDEOS = {"Keep original videos": (0, "fast"), "Shrink to render height, fast encode": (-1, "fast"),
          "Shrink to render height, smallest files (slow)": (-1, "small"), "Shrink to 1080p, fast encode": (1080, "fast")}

# stages in the order they run; seconds per MB of input (the game's executable and DLLs), refined from past builds
STAGES = ["analyze", "lift", "native compile", "native link", "wasm compile", "link", "media", "package"]
LABELS = {"analyze": "Analysing machine code", "lift": "Translating functions to C", "native compile": "Compiling native test build",
          "native link": "Linking native test build", "wasm compile": "Compiling WebAssembly", "link": "Linking WebAssembly",
          "media": "Re-encoding videos",
          "package": "Packaging game files"}
DEFAULT_RATE = {"analyze": 0.9, "lift": 2.2, "native compile": 25.0, "native link": 1.5, "wasm compile": 11.0, "link": 3.5, "media": 20.0, "package": 0.6}


def settings_dir():
    d = Path(os.environ.get("LOCALAPPDATA") or Path.home()) / "UEWASM"
    d.mkdir(parents=True, exist_ok=True)
    return d


def load_json(name, default):
    try:
        return json.loads((settings_dir() / name).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return default


def save_json(name, data):
    try:
        (settings_dir() / name).write_text(json.dumps(data, indent=2), encoding="utf-8")
    except OSError:
        pass


def fmt_time(s):
    s = int(max(0, s))
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m {s % 60:02d}s"
    return f"{s // 3600}h {s % 3600 // 60:02d}m"


class Cancelled(Exception):
    pass


class Estimator:
    """Overall time left: the running stage extrapolates from its own rate, later stages use past builds."""

    def __init__(self, stages, input_mb):
        self.stages = stages
        self.input_mb = max(input_mb, 1.0)
        self.rate = {**DEFAULT_RATE, **load_json("timings.json", {})}
        self.started = {}
        self.finished = {}
        self.cur = None
        self.frac = 0.0

    def update(self, stage, done, total):
        now = time.time()
        if stage not in self.started:
            if self.cur and self.cur not in self.finished:
                self.finished[self.cur] = now
            self.started[stage] = now
        self.cur = stage
        self.frac = min(1.0, done / total) if total else 0.0
        if total and done >= total:
            self.finished[stage] = now

    def expected(self, stage):
        return self.rate.get(stage, 1.0) * self.input_mb

    def left(self):
        now = time.time()
        left = 0.0
        for s in self.stages:
            if s in self.finished:
                continue
            if s == self.cur:
                spent = now - self.started[s]
                guess = self.expected(s)
                if self.frac > 0.05 and spent > 3:
                    # trust the measured rate more as the stage goes on
                    measured = spent / self.frac
                    guess = guess + (measured - guess) * min(1.0, self.frac * 2)
                left += max(guess - spent, 0)
            elif s not in self.started:
                left += self.expected(s)
        return left

    def overall(self):
        total = sum(self.expected(s) for s in self.stages) or 1
        left = self.left()
        return max(0.0, min(1.0, 1 - left / max(total, left + 1)))

    def remember(self):
        """blend this build's stage times into the stored rates (only stages that did real work)"""
        for s in self.stages:
            if s in self.started and s in self.finished:
                took = self.finished[s] - self.started[s]
                if took > 2:
                    self.rate[s] = self.rate.get(s, took / self.input_mb) * 0.5 + took / self.input_mb * 0.5
        save_json("timings.json", {k: round(v, 4) for k, v in self.rate.items()})


ASSETS = Path(__file__).resolve().parent / "assets"


def load_logo():
    try:
        return tk.PhotoImage(file=str(ASSETS / "logo_44.png"))
    except tk.TclError:
        return None


class App:
    def __init__(self, root):
        self.root = root
        root.title(f"{TITLE} {__version__}")
        root.minsize(760, 640)
        root.geometry("860x720")
        self.q = queue.Queue()
        self.worker = None
        self.cancel = threading.Event()
        self.server = None
        self.est = None
        self.t0 = 0
        self.last_web = None
        prefs = load_json("settings.json", {})

        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Head.TLabel", font=("Segoe UI", 15, "bold"))
        style.configure("Sub.TLabel", foreground="#555")
        style.configure("Big.TButton", font=("Segoe UI", 10, "bold"), padding=(14, 6))

        main = ttk.Frame(root, padding=14)
        main.pack(fill="both", expand=True)
        main.columnconfigure(0, weight=1)

        head = ttk.Frame(main)
        head.grid(row=0, column=0, sticky="ew")
        self.logo = load_logo()
        if self.logo:
            root.iconphoto(True, self.logo)
            ttk.Label(head, image=self.logo).pack(side="left", padx=(0, 8))
        ttk.Label(head, text=TITLE, style="Head.TLabel").pack(side="left")
        ttk.Label(head, text="  Windows x64 games to WebAssembly", style="Sub.TLabel").pack(side="left", pady=(6, 0))

        # game
        g = ttk.LabelFrame(main, text="Game", padding=10)
        g.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        g.columnconfigure(1, weight=1)
        self.game = tk.StringVar(value=prefs.get("game", ""))
        self.out = tk.StringVar(value=prefs.get("out", ""))
        ttk.Label(g, text="Install folder or .exe").grid(row=0, column=0, sticky="w")
        ttk.Entry(g, textvariable=self.game).grid(row=0, column=1, sticky="ew", padx=6)
        ttk.Button(g, text="Folder…", command=self.pick_folder).grid(row=0, column=2)
        ttk.Button(g, text=".exe…", command=self.pick_exe).grid(row=0, column=3, padx=(4, 0))
        self.info = ttk.Label(g, text="", style="Sub.TLabel")
        self.info.grid(row=1, column=1, columnspan=3, sticky="w", padx=6, pady=(4, 0))
        ttk.Label(g, text="Output folder").grid(row=2, column=0, sticky="w", pady=(8, 0))
        ttk.Entry(g, textvariable=self.out).grid(row=2, column=1, sticky="ew", padx=6, pady=(8, 0))
        ttk.Button(g, text="Browse…", command=self.pick_out).grid(row=2, column=2, columnspan=2, sticky="ew", pady=(8, 0))
        self.game.trace_add("write", lambda *_: self.root.after(300, self.describe))

        # settings
        s = ttk.LabelFrame(main, text="Settings", padding=10)
        s.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        for c in (1, 3):
            s.columnconfigure(c, weight=1)

        def combo(row, col, label, values, key, default):
            ttk.Label(s, text=label).grid(row=row, column=col, sticky="w", pady=3)
            v = tk.StringVar(value=prefs.get(key, default) if prefs.get(key, default) in values else default)
            ttk.Combobox(s, textvariable=v, values=list(values), state="readonly", width=34).grid(row=row, column=col + 1, sticky="ew", padx=(6, 16), pady=3)
            return v

        self.renderer = combo(0, 0, "Renderer", RENDERERS, "renderer", "WebGPU (recommended)")
        self.threads = combo(1, 0, "Threading", THREADS, "threads", "Cooperative (no special hosting)")
        self.opt = combo(2, 0, "Optimisation", OPTS, "opt", "Balanced (-O2)")
        ttk.Label(s, text="Resolution").grid(row=0, column=2, sticky="w")
        self.res = tk.StringVar(value=prefs.get("res", "1280x720"))
        ttk.Combobox(s, textvariable=self.res, values=RESOLUTIONS, width=14).grid(row=0, column=3, sticky="w", padx=6)
        ttk.Label(s, text="Guest memory (MB)").grid(row=1, column=2, sticky="w")
        self.mem = tk.IntVar(value=prefs.get("mem", 3072))
        ttk.Spinbox(s, from_=1024, to=3584, increment=256, textvariable=self.mem, width=12).grid(row=1, column=3, sticky="w", padx=6)
        ttk.Label(s, text="Largest file (MB)").grid(row=2, column=2, sticky="w")
        self.part = tk.IntVar(value=prefs.get("part", 16))
        ttk.Spinbox(s, from_=4, to=24, increment=4, textvariable=self.part, width=12).grid(row=2, column=3, sticky="w", padx=6)
        checks = ttk.Frame(s)
        checks.grid(row=3, column=0, columnspan=4, sticky="w", pady=(8, 0))
        self.game_dlls = tk.BooleanVar(value=prefs.get("game_dlls", True))
        self.sys_dlls = tk.BooleanVar(value=prefs.get("sys_dlls", True))
        self.native = tk.BooleanVar(value=prefs.get("native", False))
        self.compress = tk.BooleanVar(value=prefs.get("compress", False))
        self.wasm_opt = tk.BooleanVar(value=prefs.get("wasm_opt", False))
        self.show_fps = tk.BooleanVar(value=prefs.get("show_fps", False))
        self.async_shaders = tk.BooleanVar(value=prefs.get("async_shaders", True))
        self.debug_panel = tk.BooleanVar(value=prefs.get("debug_panel", True))
        ttk.Checkbutton(checks, text="Recompile the game's own DLLs", variable=self.game_dlls).pack(side="left")
        ttk.Checkbutton(checks, text="Recompile Microsoft runtime DLLs", variable=self.sys_dlls).pack(side="left", padx=14)
        ttk.Checkbutton(checks, text="Also build a native test .exe (needs Visual Studio)", variable=self.native).pack(side="left")
        ttk.Checkbutton(s, text="Compress game files (smaller download)", variable=self.compress).grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Checkbutton(s, text="Shrink code with Binaryen (10% smaller, needs lots of RAM)", variable=self.wasm_opt).grid(
            row=4, column=2, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Checkbutton(s, text="Show an FPS counter (Ctrl+Shift+F toggles it in game)", variable=self.show_fps).grid(
            row=6, column=0, columnspan=4, sticky="w", pady=(6, 0))
        ttk.Checkbutton(s, text="Compile shaders in the background (shorter freezes when new effects first appear)",
                        variable=self.async_shaders).grid(row=7, column=0, columnspan=4, sticky="w", pady=(6, 0))
        ttk.Checkbutton(s, text="UEWASM debug toggle (\\ or Insert opens the debug panel in game)",
                        variable=self.debug_panel).grid(row=8, column=0, columnspan=4, sticky="w", pady=(6, 0))
        ttk.Label(s, text="Build priority").grid(row=9, column=0, sticky="w", pady=(6, 0))
        self.priority = tk.StringVar(value=prefs.get("priority") if prefs.get("priority") in PRIORITIES else next(iter(PRIORITIES)))
        ttk.Combobox(s, textvariable=self.priority, values=list(PRIORITIES), state="readonly", width=34).grid(row=9, column=1, sticky="ew", padx=(6, 16), pady=(6, 0))
        ttk.Label(s, text="Same result either way; Low uses far less CPU and memory", style="Sub.TLabel").grid(row=9, column=2, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Label(s, text="Online services").grid(row=10, column=0, sticky="w", pady=(6, 0))
        self.backend = tk.StringVar(value=prefs.get("backend") if prefs.get("backend") in BACKENDS else next(iter(BACKENDS)))
        ttk.Combobox(s, textvariable=self.backend, values=list(BACKENDS), state="readonly", width=34).grid(row=10, column=1, sticky="ew", padx=(6, 16), pady=(6, 0))
        self.placeholders = tk.BooleanVar(value=prefs.get("placeholders", False))
        ttk.Checkbutton(s, text="Stand-in files for what the servers would send (images, data)", variable=self.placeholders).grid(
            row=10, column=2, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Label(s, text="Videos").grid(row=5, column=0, sticky="w", pady=(6, 0))
        self.videos = tk.StringVar(value=prefs.get("videos") if prefs.get("videos") in VIDEOS else "Keep original videos")
        ttk.Combobox(s, textvariable=self.videos, values=list(VIDEOS), state="readonly", width=34).grid(row=5, column=1, sticky="ew", padx=(6, 16), pady=(6, 0))
        ttk.Label(s, text="Smaller videos download faster and play smoother on slow PCs", style="Sub.TLabel").grid(row=5, column=2, columnspan=2, sticky="w", pady=(6, 0))

        # actions
        a = ttk.Frame(main)
        a.grid(row=3, column=0, sticky="ew", pady=(12, 0))
        self.start_btn = ttk.Button(a, text="Recompile", style="Big.TButton", command=self.start)
        self.start_btn.pack(side="left")
        self.cancel_btn = ttk.Button(a, text="Cancel", command=self.do_cancel, state="disabled")
        self.cancel_btn.pack(side="left", padx=6)
        self.play_btn = ttk.Button(a, text="Play in browser", command=self.play, state="disabled")
        self.play_btn.pack(side="right")
        self.open_btn = ttk.Button(a, text="Open output folder", command=self.open_out)
        self.open_btn.pack(side="right", padx=6)

        # progress
        p = ttk.LabelFrame(main, text="Progress", padding=10)
        p.grid(row=4, column=0, sticky="nsew", pady=(10, 0))
        main.rowconfigure(4, weight=1)
        p.columnconfigure(0, weight=1)
        p.rowconfigure(3, weight=1)
        self.stage = ttk.Label(p, text="Ready")
        self.stage.grid(row=0, column=0, sticky="w")
        self.times = ttk.Label(p, text="", style="Sub.TLabel")
        self.times.grid(row=0, column=1, sticky="e")
        self.bar = ttk.Progressbar(p, maximum=1000)
        self.bar.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(6, 2))
        self.sbar = ttk.Progressbar(p, maximum=1000)
        self.sbar.grid(row=2, column=0, columnspan=2, sticky="ew", pady=(0, 8))
        logf = ttk.Frame(p)
        logf.grid(row=3, column=0, columnspan=2, sticky="nsew")
        logf.columnconfigure(0, weight=1)
        logf.rowconfigure(0, weight=1)
        self.log = tk.Text(logf, height=10, wrap="word", font=("Consolas", 9), state="disabled", relief="flat", background="#fafafa")
        self.log.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.log["yscrollcommand"] = sb.set

        self.toolchain = ttk.Label(main, text="", style="Sub.TLabel")
        self.toolchain.grid(row=5, column=0, sticky="w", pady=(6, 0))
        root.after(100, self.check_tools)
        root.after(100, self.describe)
        root.after(100, self.pump)
        root.protocol("WM_DELETE_WINDOW", self.close)

    # ---- inputs ----

    def pick_folder(self):
        d = filedialog.askdirectory(title="Game install folder")
        if d:
            self.game.set(d)

    def pick_exe(self):
        f = filedialog.askopenfilename(title="Game executable", filetypes=[("Windows executable", "*.exe")])
        if f:
            self.game.set(f)

    def pick_out(self):
        d = filedialog.askdirectory(title="Where to put the build")
        if d:
            self.out.set(d)

    def find_exe(self):
        from .cli import find_exe
        p = Path(self.game.get().strip().strip('"'))
        if not self.game.get().strip() or not p.exists():
            return None
        try:
            return find_exe(p)
        except SystemExit:
            return None

    def describe(self):
        exe = self.find_exe()
        if not exe:
            self.info["text"] = "Pick the game's install folder (the main *-Shipping.exe is found automatically)." if not self.game.get() else "No Windows executable found there."
            return
        engine = "Unreal Engine" if any((p / "Engine").is_dir() for p in list(exe.parents)[:5]) else "unknown engine"
        self.info["text"] = f"{exe.name} · {exe.stat().st_size / 1048576:.0f} MB · {engine}"
        if not self.out.get():
            self.out.set(str(Path.home() / "UEWASM Builds" / exe.stem))

    def check_tools(self):
        def run():
            parts = []
            try:
                from .recomp.build import emsdk_env
                emsdk_env()
                parts.append("Emscripten: found")
            except Exception:
                parts.append("Emscripten: NOT FOUND (install emsdk to %USERPROFILE%\\emsdk or set EMSDK)")
            try:
                import iced_x86  # noqa: F401
            except ImportError:
                parts.append("iced-x86 missing")
            self.q.put(("tools", " · ".join(parts)))
        threading.Thread(target=run, daemon=True).start()

    # ---- building ----

    def save_prefs(self):
        save_json("settings.json", {"game": self.game.get(), "out": self.out.get(), "renderer": self.renderer.get(), "threads": self.threads.get(),
                                    "opt": self.opt.get(), "res": self.res.get(), "mem": self.mem.get(), "part": self.part.get(),
                                    "game_dlls": self.game_dlls.get(), "sys_dlls": self.sys_dlls.get(), "native": self.native.get(),
                                    "compress": self.compress.get(), "videos": self.videos.get(), "wasm_opt": self.wasm_opt.get(), "show_fps": self.show_fps.get(), "async_shaders": self.async_shaders.get(), "debug_panel": self.debug_panel.get(), "priority": self.priority.get(),
                                    "backend": self.backend.get(), "placeholders": self.placeholders.get()})

    def start(self):
        exe = self.find_exe()
        if not exe:
            messagebox.showerror(TITLE, "Choose the game's install folder or .exe first.")
            return
        try:
            w, h = (int(x) for x in self.res.get().lower().split("x"))
        except ValueError:
            messagebox.showerror(TITLE, "Resolution should look like 1280x720.")
            return
        if THREADS[self.threads.get()] == "real":
            messagebox.showinfo(TITLE, "Real-thread builds aren't finished yet; this build will use cooperative threading.")
        out = Path(self.out.get() or Path.home() / "UEWASM Builds" / exe.stem)
        self.save_prefs()
        opts = dict(exe=exe, out=out, renderer=RENDERERS[self.renderer.get()], opt=OPTS[self.opt.get()], size=(w, h), mem=int(self.mem.get()),
                    part=int(self.part.get()), game_dlls=self.game_dlls.get(), sys_dlls=self.sys_dlls.get(), native=self.native.get(),
                    compress=self.compress.get(), videos=VIDEOS[self.videos.get()], wasm_opt=self.wasm_opt.get(), show_fps=self.show_fps.get(), async_shaders=self.async_shaders.get(), debug_panel=self.debug_panel.get(), priority=PRIORITIES[self.priority.get()],
                    backend=BACKENDS[self.backend.get()], placeholders=self.placeholders.get())
        self.cancel.clear()
        self.clear_log()
        self.start_btn["state"] = "disabled"
        self.cancel_btn["state"] = "normal"
        self.play_btn["state"] = "disabled"
        self.t0 = time.time()
        self.bar["value"] = 0
        self.sbar["value"] = 0
        self.worker = threading.Thread(target=self.build, args=(opts,), daemon=True)
        self.worker.start()

    def progress(self, stage, done, total):
        if self.cancel.is_set():
            raise Cancelled()
        self.q.put(("progress", stage, done, total))

    def build(self, o):
        from .recomp import build, resources
        from .recomp.link import game_dlls
        log = lambda m: self.q.put(("log", str(m)))
        try:
            resources.set_mode(o["priority"])
            resources.lower_this_process()
            log(resources.describe())
            root, found = game_dlls(o["exe"])
            extra = found if o["game_dlls"] else []
            input_mb = (o["exe"].stat().st_size + sum(p.stat().st_size for p in extra)) / 1048576
            stages = [s for s in STAGES if o["native"] or not s.startswith("native")]
            self.est = Estimator(stages, input_mb)
            log(f"{o['exe'].name}: {len(extra)} game DLLs, {input_mb:.0f} MB of machine code to translate")
            r = build.recompile(o["exe"], o["out"], mem_mb=o["mem"], extra=extra, search=sorted({str(d.parent) for d in extra}),
                                system_dlls=o["sys_dlls"], log=log, progress=self.progress)
            log(f"{r['functions']} functions, {r['unsupported_total']} unsupported instructions, {len(r['missing_imports'])} Windows functions not implemented yet")
            (Path(o["out"]) / "options.json").write_text(json.dumps({"renderer": o["renderer"], "opt": o["opt"], "mem_mb": o["mem"]}, indent=2))
            if o["native"]:
                build.build_native(o["out"], log=log, progress=self.progress)
            web = build.build_wasm(o["out"], opt=o["opt"], renderer=o["renderer"], game_root=root, exe_rel=o["exe"].relative_to(root).as_posix(),
                                   size=o["size"], part_mb=o["part"], compress=o["compress"], log=log, progress=self.progress,
                                   video_height=o["size"][1] if o["videos"][0] < 0 else o["videos"][0], video_mode=o["videos"][1],
                                   wasm_opt=o["wasm_opt"], show_fps=o["show_fps"], async_shaders=o["async_shaders"], debug_panel=o["debug_panel"],
                                   backend=o.get("backend"), placeholders=o.get("placeholders", False))
            self.est.remember()
            self.q.put(("done", str(web)))
        except Cancelled:
            self.q.put(("cancelled",))
        except Exception as e:
            self.q.put(("failed", f"{e}\n\n{traceback.format_exc(limit=4)}"))

    def do_cancel(self):
        self.cancel.set()
        self.cancel_btn["state"] = "disabled"
        self.stage["text"] = "Cancelling after the current step…"

    # ---- UI updates from the worker ----

    def pump(self):
        try:
            while True:
                m = self.q.get_nowait()
                kind = m[0]
                if kind == "log":
                    self.append(m[1])
                elif kind == "tools":
                    self.toolchain["text"] = m[1]
                elif kind == "progress":
                    _, stage, done, total = m
                    if self.est:
                        self.est.update(stage, done, total)
                    label = LABELS.get(stage, stage)
                    if total and stage in ("lift",):
                        label += f"  ({done}/{total} files)"
                    self.stage["text"] = label
                    if total:
                        self.sbar.stop()
                        self.sbar["mode"] = "determinate"
                        self.sbar["value"] = 1000 * done / total
                    elif str(self.sbar["mode"]) != "indeterminate":
                        self.sbar["mode"] = "indeterminate"
                        self.sbar.start(15)
                elif kind == "done":
                    self.finish(f"Done in {fmt_time(time.time() - self.t0)}", 1000)
                    self.last_web = m[1]
                    self.play_btn["state"] = "normal"
                    self.append(f"\nWeb build ready: {m[1]}\nUpload the whole folder to a static host, or press 'Play in browser'.")
                elif kind == "cancelled":
                    self.finish("Cancelled", None)
                elif kind == "failed":
                    self.finish("Failed", None)
                    self.append("\nERROR: " + m[1])
                    messagebox.showerror(TITLE, m[1].split("\n\n")[0][:600])
        except queue.Empty:
            pass
        if self.worker and self.worker.is_alive() and self.est:
            el = time.time() - self.t0
            self.bar["value"] = 1000 * self.est.overall()
            self.times["text"] = f"elapsed {fmt_time(el)} · about {fmt_time(self.est.left())} left"
        self.root.after(200, self.pump)

    def finish(self, text, bar):
        self.stage["text"] = text
        self.times["text"] = f"elapsed {fmt_time(time.time() - self.t0)}"
        self.sbar.stop()
        self.sbar["mode"] = "determinate"
        if bar is not None:
            self.bar["value"] = bar
            self.sbar["value"] = bar
        self.start_btn["state"] = "normal"
        self.cancel_btn["state"] = "disabled"

    def clear_log(self):
        self.log["state"] = "normal"
        self.log.delete("1.0", "end")
        self.log["state"] = "disabled"

    def append(self, text):
        self.log["state"] = "normal"
        self.log.insert("end", text + "\n")
        self.log.see("end")
        self.log["state"] = "disabled"

    # ---- after the build ----

    def web_dir(self):
        if self.last_web:
            return Path(self.last_web)
        w = Path(self.out.get()) / "web"
        return w if (w / "index.html").exists() else None

    def play(self):
        web = self.web_dir()
        if not web:
            return
        from . import serve
        if self.server:
            self.server.shutdown()
        self.server = serve.background(str(web), 8080 if not self.port_busy(8080) else 0)
        url = f"http://localhost:{self.server.server_port}/index.html"
        self.append(f"Serving {web} at {url} (closes with this window)")
        webbrowser.open(url)

    @staticmethod
    def port_busy(port):
        import socket
        with socket.socket() as s:
            return s.connect_ex(("127.0.0.1", port)) == 0

    def open_out(self):
        d = Path(self.out.get())
        if d.exists():
            os.startfile(d) if os.name == "nt" else webbrowser.open(d.as_uri())

    def close(self):
        if self.worker and self.worker.is_alive() and not messagebox.askyesno(TITLE, "A build is running. Stop it and quit?"):
            return
        self.cancel.set()
        if self.server:
            self.server.shutdown()
        self.root.destroy()


def main():
    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except (AttributeError, OSError):
            pass
    root = tk.Tk()
    ico = ASSETS / "uewasm.ico"
    if ico.exists():
        root.iconbitmap(default=str(ico))
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
