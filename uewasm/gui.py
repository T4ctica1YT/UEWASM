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

from . import __version__, deps

TITLE = "UEWASM"
RENDERERS = {"WebGPU (recommended)": "webgpu", "WebGL 2 (works everywhere)": "webgl2"}
THREADS = {"Cooperative (no special hosting)": "coop", "Real threads (several cores; WebGPU, https or localhost)": "real"}
OPTS = {"Balanced (-O2)": "-O2", "Fastest code (-O3, slower build)": "-O3", "Smallest download (-Os)": "-Os"}
QUALITY = {"Game's own setting": "game", "Low (fastest here)": "low", "Medium": "medium", "High": "high", "Epic": "epic"}
LIFTERS = {"Python (original)": "python", "Rust (same result, much faster)": "rust"}
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


class WrongKey(Exception):
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
# every optional tool, for the Tools tab: key, name, what needs it
TOOLS = [("binaryen", "Binaryen", "Shrink code with Binaryen"), ("msvc", "Visual Studio C++ tools", "Native test .exe"),
         ("rust", "Rust", "Rust recompiler"), ("ffmpeg", "ffmpeg (imageio-ffmpeg)", "Shrinking videos"),
         ("crypto", "pycryptodome", "Decrypting AES content")]
ALL_ON = dict(native=True, lifter="rust", wasm_opt=True, videos=True, aes=True)


def load_logo():
    try:
        return tk.PhotoImage(file=str(ASSETS / "logo_44.png"))
    except tk.TclError:
        return None


def pick(values, saved, default):
    """a saved choice, or the option it was renamed to (same first word)"""
    if saved in values:
        return saved
    if saved:
        for v in values:
            if v.split()[0] == saved.split()[0]:
                return v
    return default


class ScrollFrame(ttk.Frame):
    """a tab page that scrolls when the window is too short for it"""
    active = None

    def __init__(self, parent, bg):
        super().__init__(parent)
        self.canvas = tk.Canvas(self, highlightthickness=0, borderwidth=0, background=bg)
        self.sb = ttk.Scrollbar(self, orient="vertical", command=self.canvas.yview)
        self.inner = ttk.Frame(self.canvas, padding=(12, 10))
        self.win = self.canvas.create_window(0, 0, window=self.inner, anchor="nw")
        self.canvas.configure(yscrollcommand=self.sb.set)
        self.canvas.grid(row=0, column=0, sticky="nsew")
        self.sb.grid(row=0, column=1, sticky="ns")
        self.rowconfigure(0, weight=1)
        self.columnconfigure(0, weight=1)
        self.inner.bind("<Configure>", self.fit)
        self.canvas.bind("<Configure>", self.fit)
        self.bind("<Enter>", lambda _: setattr(ScrollFrame, "active", self))
        self.bind("<Leave>", lambda _: setattr(ScrollFrame, "active", None))
        self.notes = []

    def fit(self, _=None):
        w = self.canvas.winfo_width()
        self.canvas.itemconfigure(self.win, width=w)
        if w > 1 and self.notes:
            x = self.inner.grid_bbox(2, 0)[0] + 14
            for n in self.notes:
                n.configure(wraplength=max(160, w - x - 40))
        h = self.inner.winfo_reqheight()
        self.canvas.configure(scrollregion=(0, 0, self.inner.winfo_reqwidth(), h))
        if h > self.canvas.winfo_height() + 1:
            self.sb.grid()
        else:
            self.sb.grid_remove()
            self.canvas.yview_moveto(0)

    def scroll(self, e):
        if self.sb.winfo_ismapped():
            self.canvas.yview_scroll(-1 if e.delta > 0 else 1, "units")

    @staticmethod
    def wheel(e):
        if ScrollFrame.active:
            ScrollFrame.active.scroll(e)
        return "break"


class App:
    def __init__(self, root):
        self.root = root
        root.title(f"{TITLE} {__version__}")
        root.minsize(780, 640)
        root.geometry("920x860")
        self.q = queue.Queue()
        self.worker = None
        self.cancel = threading.Event()
        self.server = None
        self.est = None
        self.t0 = 0
        self.last_web = None
        self.needs = {}
        prefs = load_json("settings.json", {})

        style = ttk.Style()
        if "vista" in style.theme_names():
            style.theme_use("vista")
        style.configure("Head.TLabel", font=("Segoe UI", 15, "bold"))
        style.configure("Section.TLabel", font=("Segoe UI", 11, "bold"))
        style.configure("Sub.TLabel", foreground="#555")
        style.configure("Ok.TLabel", foreground="#1a7f37")
        style.configure("Bad.TLabel", foreground="#b42318")
        style.configure("Big.TButton", font=("Segoe UI", 10, "bold"), padding=(14, 6))
        style.configure("TNotebook.Tab", padding=(12, 4))
        style.configure("Dlg.TButton", padding=(10, 4))
        bg = style.lookup("TFrame", "background") or "SystemButtonFace"
        # wheel over a tab scrolls the tab instead of changing whichever list or number is under the pointer
        root.bind_all("<MouseWheel>", ScrollFrame.wheel)
        for cls in ("TCombobox", "TSpinbox"):
            root.bind_class(cls, "<MouseWheel>", ScrollFrame.wheel)

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

        # settings, one tab per topic: a label, the control, and a grey note on what it does
        nb = ttk.Notebook(main)
        nb.grid(row=2, column=0, sticky="nsew", pady=(10, 0))
        main.rowconfigure(2, weight=1)
        self.pages = []

        def tab(title):
            sf = ScrollFrame(nb, bg)
            nb.add(sf, text=title)
            self.pages.append(sf)
            f = sf.inner
            f.columnconfigure(1, minsize=260)
            f.columnconfigure(2, weight=1)
            return f

        def note(f, row, text):
            if text:
                n = ttk.Label(f, text=text, style="Sub.TLabel", wraplength=330, justify="left")
                n.grid(row=row, column=2, sticky="w", padx=(14, 0), pady=4)
                f.master.master.notes.append(n)

        def combo(f, label, values, key, default, hint=None, editable=False):
            r = f.grid_size()[1]
            ttk.Label(f, text=label).grid(row=r, column=0, sticky="w", pady=4, padx=(0, 10))
            v = tk.StringVar(value=prefs.get(key, default) if editable else pick(list(values), prefs.get(key), default))
            ttk.Combobox(f, textvariable=v, values=list(values), state="normal" if editable else "readonly").grid(row=r, column=1, sticky="ew", pady=4)
            note(f, r, hint)
            return v

        def spin(f, label, key, default, lo, hi, step, hint=None, kind=tk.IntVar):
            r = f.grid_size()[1]
            ttk.Label(f, text=label).grid(row=r, column=0, sticky="w", pady=4, padx=(0, 10))
            v = kind(value=prefs.get(key, default))
            ttk.Spinbox(f, from_=lo, to=hi, increment=step, textvariable=v, width=12).grid(row=r, column=1, sticky="w", pady=4)
            note(f, r, hint)
            return v

        def check(f, text, key, default, hint=None, command=None):
            r = f.grid_size()[1]
            v = tk.BooleanVar(value=prefs.get(key, default))
            ttk.Checkbutton(f, text=text, variable=v, command=command).grid(row=r, column=0, columnspan=2, sticky="w", pady=4)
            note(f, r, hint)
            return v

        b = tab("Build")
        self.lifter = combo(b, "Recompiler", LIFTERS, "lifter", "Python (original)", "Rust translates a big game in seconds instead of minutes")
        self.opt = combo(b, "Optimisation", OPTS, "opt", "Balanced (-O2)")
        self.priority = combo(b, "Build priority", PRIORITIES, "priority", next(iter(PRIORITIES)), "Same result either way; Low uses far less CPU and memory")
        self.game_dlls = check(b, "Recompile the game's own DLLs", "game_dlls", True)
        self.sys_dlls = check(b, "Recompile Microsoft runtime DLLs", "sys_dlls", True)
        self.wasm_opt = check(b, "Shrink code with Binaryen", "wasm_opt", False, "About 10% smaller; needs a lot of memory while building")
        self.native = check(b, "Also build a native test .exe", "native", False, "For checking the translation on Windows; needs Visual Studio's C++ tools")

        p = tab("Graphics & performance")
        self.renderer = combo(p, "Renderer", RENDERERS, "renderer", "WebGPU (recommended)")
        self.threads = combo(p, "Threading", THREADS, "threads", "Cooperative (no special hosting)", "Real threads use several CPU cores; the host must allow it (uewasm serve does)")
        self.quality = combo(p, "Graphics (Unreal)", QUALITY, "quality", "Game's own setting", "Applied at every start; lower runs faster in the browser")
        self.res = combo(p, "Resolution", RESOLUTIONS, "res", "1280x720", "Type any size, like 1366x768", editable=True)
        self.mem = spin(p, "Guest memory (MB)", "mem", 3072, 1024, 3584, 256, "Memory the game itself can use")
        self.mem_limit = spin(p, "Memory limit when run (GB)", "mem_limit", 8, 4, 32, 1, "All the browser tab may use, including downloads and video memory", tk.DoubleVar)
        self.show_fps = check(p, "Show an FPS counter", "show_fps", False, "Ctrl+Shift+F toggles it in game")
        self.async_shaders = check(p, "Compile shaders in the background", "async_shaders", True, "Shorter freezes when new effects first appear")
        self.debug_panel = check(p, "UEWASM debug panel", "debug_panel", True, "\\ or Insert opens it in game")

        f = tab("Game files")
        self.videos = combo(f, "Videos", VIDEOS, "videos", "Keep original videos", "Smaller videos download faster and play smoother on slow PCs")
        self.part = spin(f, "Largest file (MB)", "part", 16, 4, 24, 4, "Game data is split into files of at most this size")
        self.compress = check(f, "Compress game files", "compress", False, "Smaller download")
        self.single = check(f, "Also make a single .html", "single", False, "Plays straight from disk with no server; cooperative threads only")
        self.aes_on = check(f, "Decrypt AES encrypted content", "aes_on", False, command=self.aes_changed)
        r = f.grid_size()[1]
        self.aes_key = tk.StringVar()
        self.aes_entry = ttk.Entry(f, textvariable=self.aes_key)
        self.aes_entry.grid(row=r, column=0, columnspan=3, sticky="ew", pady=(0, 2))
        self.aes_note = ttk.Label(f, text="", style="Sub.TLabel")
        self.aes_note.grid(row=r + 1, column=0, columnspan=3, sticky="w")
        self.aes_key.trace_add("write", lambda *_: self.aes_changed())

        o = tab("Online")
        self.backend = combo(o, "Online services", BACKENDS, "backend", next(iter(BACKENDS)), "Answers the game's online sign-in without a server")
        self.placeholders = check(o, "Stand-in files for what the servers would send", "placeholders", False, "Images and data the game would download")

        t = tab("Tools")
        t.columnconfigure(1, minsize=0, weight=1)
        t.columnconfigure(2, weight=0)
        ttk.Label(t, text="Emscripten SDK", style="Section.TLabel").grid(row=0, column=0, columnspan=4, sticky="w")
        ttk.Label(t, text="Every build needs it. Choose the folder it's installed in (the one with emsdk.bat), or let UEWASM install it.",
                  style="Sub.TLabel").grid(row=1, column=0, columnspan=4, sticky="w", pady=(2, 6))
        self.emsdk = tk.StringVar(value=prefs.get("emsdk", ""))
        ttk.Label(t, text="Folder").grid(row=2, column=0, sticky="w", padx=(0, 10))
        ttk.Entry(t, textvariable=self.emsdk).grid(row=2, column=1, sticky="ew")
        ttk.Button(t, text="Browse…", command=lambda: self.browse_emsdk(None)).grid(row=2, column=2, padx=(6, 0))
        self.ems_install = ttk.Button(t, text="Install…", command=lambda: self.do_install(self.needs.get("emsdk") or self.emsdk_need()))
        self.ems_install.grid(row=2, column=3, padx=(6, 0))
        self.ems_status = ttk.Label(t, text="Checking…", style="Sub.TLabel")
        self.ems_status.grid(row=3, column=1, columnspan=3, sticky="w", pady=(4, 0))
        ttk.Separator(t).grid(row=4, column=0, columnspan=4, sticky="ew", pady=10)
        ttk.Label(t, text="Needed only by some settings", style="Section.TLabel").grid(row=5, column=0, columnspan=4, sticky="w", pady=(0, 4))
        self.tool_rows = {}
        for i, (key, name, used) in enumerate(TOOLS):
            r = 6 + i
            ttk.Label(t, text=name).grid(row=r, column=0, sticky="w", padx=(0, 10), pady=2)
            lab = ttk.Label(t, text="", style="Sub.TLabel")
            lab.grid(row=r, column=1, sticky="w", pady=2)
            btn = ttk.Button(t, text="Install…", command=lambda k=key: self.do_install(self.needs[k]))
            btn.grid(row=r, column=3, padx=(6, 0), pady=2)
            btn.grid_remove()
            self.tool_rows[key] = (lab, btn, used)
        ttk.Button(t, text="Check again", command=self.check_tools).grid(row=6 + len(TOOLS), column=0, sticky="w", pady=(10, 0))
        self.emsdk.trace_add("write", lambda *_: self.emsdk_changed())
        self.emsdk_changed(check=False)

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
        main.rowconfigure(4, weight=4, minsize=150)
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
        self.log = tk.Text(logf, height=6, wrap="word", font=("Consolas", 9), state="disabled", relief="flat", background="#fafafa")
        self.log.grid(row=0, column=0, sticky="nsew")
        sb = ttk.Scrollbar(logf, command=self.log.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.log["yscrollcommand"] = sb.set

        self.toolchain = ttk.Label(main, text="", style="Sub.TLabel")
        self.toolchain.grid(row=5, column=0, sticky="w", pady=(6, 0))
        self.aes_changed()
        root.after(50, self.size_tabs)
        root.after(100, self.check_tools)
        root.after(100, self.describe)
        root.after(100, self.pump)
        root.protocol("WM_DELETE_WINDOW", self.close)

    def size_tabs(self):
        """tabs as tall as the tallest page needs, up to a point; shorter windows scroll the page"""
        self.root.update_idletasks()
        h = min(max(sf.inner.winfo_reqheight() for sf in self.pages), 420)
        for sf in self.pages:
            sf.canvas.configure(height=h)

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

    # ---- tools ----

    def emsdk_changed(self, check=True):
        v = self.emsdk.get().strip().strip('"')
        if v:
            os.environ["UEWASM_EMSDK"] = v
        else:
            os.environ.pop("UEWASM_EMSDK", None)
        if check:
            if getattr(self, "_ems_timer", None):
                self.root.after_cancel(self._ems_timer)
            self._ems_timer = self.root.after(500, self.check_tools)

    def check_tools(self):
        def run():
            try:
                self.q.put(("tools", deps.status(), {n.key: n for n in deps.missing(ALL_ON)}))
            except Exception as e:
                self.q.put(("log", f"tool check failed: {e}"))
        threading.Thread(target=run, daemon=True).start()

    def show_tools(self, st, needs):
        self.needs = needs
        if st["emsdk"]:
            self.ems_status.configure(text=f"Found: {st['emsdk']}", style="Ok.TLabel")
        else:
            typed = self.emsdk.get().strip()
            self.ems_status.configure(style="Bad.TLabel", text=("No Emscripten SDK in that folder. " if typed else "Not found. ")
                                      + "Choose the folder it's in, or press Install.")
        found = {"binaryen": st["binaryen"], "msvc": st["msvc"], "rust": st["rust"], "ffmpeg": st["ffmpeg"], "crypto": st["crypto"]}
        for key, (lab, btn, used) in self.tool_rows.items():
            ok = found[key]
            lab.configure(text=(f"Found{': ' + ok if isinstance(ok, str) and len(ok) < 70 else ''}" if ok else
                                "Comes with Emscripten" if key == "binaryen" and not st["emsdk"] else f"Not found (needed by: {used})"),
                          style="Ok.TLabel" if ok else "Sub.TLabel")
            if not ok and key in needs and needs[key].install:
                btn.grid()
            else:
                btn.grid_remove()
        parts = ["Emscripten: found" if st["emsdk"] else "Emscripten: NOT FOUND (see the Tools tab)"]
        try:
            import iced_x86  # noqa: F401
        except ImportError:
            parts.append("iced-x86 missing")
        self.toolchain["text"] = " · ".join(parts)

    def emsdk_need(self):
        return deps.Need("emsdk", "Emscripten", "", "", install=("Install Emscripten for me", None), browse=True)

    def browse_emsdk(self, then):
        """True when a usable SDK was chosen; False otherwise (or when an install was started instead)"""
        d = filedialog.askdirectory(title="Emscripten SDK folder (the one with emsdk.bat)", initialdir=self.emsdk.get() or str(Path.home()))
        if not d:
            return False
        r = deps.emsdk_at(d)
        if r:
            self.emsdk.set(str(r))
            return True
        if deps.emsdk_partial(d):
            if messagebox.askyesno(TITLE, f"{d} has emsdk, but its compilers were never installed.\n\nInstall them now (about 2.5 GB)?"):
                self.run_job("Emscripten", lambda log, cancel: deps.install_emsdk(d, log, cancel), "emsdk", then)
            return False
        messagebox.showerror(TITLE, f"There's no Emscripten SDK in {d}.\n\nChoose the emsdk folder (it holds emsdk.bat and an 'upstream' folder).")
        return False

    def ask_emsdk_dest(self):
        dest = deps.default_emsdk_dir()
        c = messagebox.askyesnocancel(TITLE, f"Install Emscripten into {dest}?\n\nIt takes about 2.5 GB of disk.\n\n"
                                             "Yes: install there.   No: choose another folder.")
        if c is None:
            return None
        if c:
            return dest
        d = filedialog.askdirectory(title="Where to install Emscripten")
        return Path(d) if d else None

    def do_install(self, need, then=None):
        if self.busy():
            return
        if need.key == "emsdk":
            dest = self.ask_emsdk_dest()
            if not dest:
                return
            fn = lambda log, cancel: deps.install_emsdk(dest, log, cancel)
        else:
            fn = need.install[1]
        self.run_job(need.name, fn, need.key, then)

    def run_job(self, name, fn, key, then):
        self.cancel.clear()
        self.clear_log()
        self.est = None
        self.t0 = time.time()
        self.start_btn["state"] = "disabled"
        self.cancel_btn["state"] = "normal"
        self.stage["text"] = f"Installing {name}…"
        self.bar["value"] = 0
        self.sbar["mode"] = "indeterminate"
        self.sbar.start(15)

        def work():
            try:
                r = fn(lambda m: self.q.put(("log", str(m))), self.cancel)
                self.q.put(("installed", key, name, str(r) if r else "", then))
            except InterruptedError:
                self.q.put(("cancelled",))
            except Exception as e:
                self.q.put(("failed", f"Installing {name} failed: {e}"))
        self.worker = threading.Thread(target=work, daemon=True)
        self.worker.start()

    def busy(self):
        return bool(self.worker and self.worker.is_alive())

    def ask_need(self, n):
        """what to do about a missing tool: 'install', 'browse', 'off', or None to cancel"""
        d = tk.Toplevel(self.root)
        d.title(TITLE)
        d.transient(self.root)
        d.resizable(False, False)
        f = ttk.Frame(d, padding=18)
        f.pack(fill="both", expand=True)
        ttk.Label(f, text=n.title, style="Section.TLabel").pack(anchor="w")
        ttk.Label(f, text=n.text, wraplength=480, justify="left").pack(anchor="w", pady=(8, 16))
        res = {"v": None}

        def choose(v):
            res["v"] = v
            d.destroy()
        row = ttk.Frame(f)
        row.pack(fill="x")
        first = None
        for label, v in ((n.install and n.install[0], "install"), (n.browse and "Choose its folder…", "browse"), (n.off, "off")):
            if label:
                btn = ttk.Button(row, text=label, style="Dlg.TButton", command=lambda v=v: choose(v))
                btn.pack(side="left", padx=(0, 6))
                first = first or btn
        ttk.Button(row, text="Cancel", style="Dlg.TButton", command=lambda: choose(None)).pack(side="right")
        if n.url:
            ttk.Button(row, text="More info", style="Dlg.TButton", command=lambda: webbrowser.open(n.url)).pack(side="right", padx=6)
        d.protocol("WM_DELETE_WINDOW", lambda: choose(None))
        d.bind("<Escape>", lambda _: choose(None))
        d.update_idletasks()
        x = self.root.winfo_rootx() + (self.root.winfo_width() - d.winfo_width()) // 2
        y = self.root.winfo_rooty() + (self.root.winfo_height() - d.winfo_height()) // 3
        d.geometry(f"+{max(x, 0)}+{max(y, 0)}")
        if first:
            first.focus_set()
        d.grab_set()
        d.wait_window()
        return res["v"]

    def turn_off(self, key):
        if key == "binaryen":
            self.wasm_opt.set(False)
        elif key == "msvc":
            self.native.set(False)
        elif key == "rust":
            self.lifter.set("Python (original)")
        elif key == "ffmpeg":
            self.videos.set("Keep original videos")
        elif key == "crypto":
            self.aes_on.set(False)
            self.aes_changed()

    def resolve_needs(self):
        """stop before building when the chosen settings need a tool this PC lacks; True to go ahead"""
        o = dict(native=self.native.get(), lifter=LIFTERS[self.lifter.get()], wasm_opt=self.wasm_opt.get(),
                 videos=VIDEOS[self.videos.get()][0] != 0, aes=self.aes_on.get())
        for n in deps.missing(o):
            c = self.ask_need(n)
            if c is None:
                return False
            if c == "install":
                self.do_install(n, then=self.start)
                return False
            if c == "browse":
                return self.browse_emsdk(self.start) and self.resolve_needs()
            self.turn_off(n.key)
        return True

    # ---- AES key ----

    def aes_changed(self):
        """the key box only when decrypting; Recompile greyed out while it is on and the key is missing or malformed"""
        from .recomp.upak import parse_key
        on = self.aes_on.get()
        self.aes_entry["state"] = "normal" if on else "disabled"
        text = self.aes_key.get().strip()
        ok = not on or parse_key(text) is not None
        if not on:
            self.aes_note["text"] = "The game's AES key goes in the box above"
        elif not text:
            self.aes_note["text"] = "Enter the game's AES key (64 hex digits, 0x optional) to build"
        elif not ok:
            self.aes_note["text"] = "That isn't a 256-bit AES key (64 hex digits or base64)"
        else:
            self.aes_note["text"] = "Checked against the game's encrypted paks when the build starts"
        if not self.busy():
            self.start_btn["state"] = "normal" if ok else "disabled"

    def ask_aes_key(self, why):
        from tkinter import simpledialog
        from .recomp.upak import parse_key
        while True:
            t = simpledialog.askstring(TITLE, why + "\n\nEnter your correct AES key:", parent=self.root)
            if t is None:
                return
            if parse_key(t):
                self.aes_key.set(t.strip())
                return
            why = "That isn't a 256-bit AES key (64 hex digits or base64)."

    # ---- building ----

    def save_prefs(self):
        save_json("settings.json", {"game": self.game.get(), "out": self.out.get(), "renderer": self.renderer.get(), "threads": self.threads.get(),
                                    "opt": self.opt.get(), "res": self.res.get(), "mem": self.mem.get(), "part": self.part.get(),
                                    "game_dlls": self.game_dlls.get(), "sys_dlls": self.sys_dlls.get(), "native": self.native.get(),
                                    "compress": self.compress.get(), "videos": self.videos.get(), "wasm_opt": self.wasm_opt.get(), "show_fps": self.show_fps.get(), "async_shaders": self.async_shaders.get(), "debug_panel": self.debug_panel.get(), "priority": self.priority.get(),
                                    "backend": self.backend.get(), "placeholders": self.placeholders.get(), "aes_on": self.aes_on.get(),
                                    "mem_limit": self.mem_limit.get(), "single": self.single.get(), "lifter": self.lifter.get(), "quality": self.quality.get(),
                                    "emsdk": self.emsdk.get().strip()})

    def start(self):
        if self.busy():
            return
        exe = self.find_exe()
        if not exe:
            messagebox.showerror(TITLE, "Choose the game's install folder or .exe first.")
            return
        try:
            w, h = (int(x) for x in self.res.get().lower().split("x"))
        except ValueError:
            messagebox.showerror(TITLE, "Resolution should look like 1280x720.")
            return
        if THREADS[self.threads.get()] == "real" and self.single.get():
            messagebox.showerror(TITLE, "A single .html file can't use real threads (pages opened from disk can't be cross-origin isolated). "
                                        "Pick cooperative threading, or untick the single-file option.")
            return
        out = Path(self.out.get() or Path.home() / "UEWASM Builds" / exe.stem)
        from .recomp.upak import parse_key
        aes = parse_key(self.aes_key.get()) if self.aes_on.get() else None
        if self.aes_on.get() and not aes:
            return
        if not self.resolve_needs():
            return
        self.save_prefs()
        opts = dict(exe=exe, out=out, renderer=RENDERERS[self.renderer.get()], opt=OPTS[self.opt.get()], size=(w, h), mem=int(self.mem.get()),
                    part=int(self.part.get()), game_dlls=self.game_dlls.get(), sys_dlls=self.sys_dlls.get(), native=self.native.get(),
                    compress=self.compress.get(), videos=VIDEOS[self.videos.get()], wasm_opt=self.wasm_opt.get(), show_fps=self.show_fps.get(), async_shaders=self.async_shaders.get(), debug_panel=self.debug_panel.get(), priority=PRIORITIES[self.priority.get()],
                    backend=BACKENDS[self.backend.get()], placeholders=self.placeholders.get(), aes=aes, mem_limit=float(self.mem_limit.get()),
                    threads=THREADS[self.threads.get()], single=self.single.get(), lifter=LIFTERS[self.lifter.get()], quality=QUALITY[self.quality.get()])
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
            if o.get("aes"):
                from .recomp import upak
                paks = upak.encrypted_paks(root)
                bad = [p.name for p in paks if not upak.check_key(p, o["aes"])]
                if bad:
                    raise WrongKey(f"The AES key is wrong for {', '.join(bad[:5])}.")
                log(f"AES key opens all {len(paks)} encrypted paks" if paks else "AES key given, but no pak here is encrypted")
            extra = found if o["game_dlls"] else []
            input_mb = (o["exe"].stat().st_size + sum(p.stat().st_size for p in extra)) / 1048576
            stages = [s for s in STAGES if o["native"] or not s.startswith("native")]
            self.est = Estimator(stages, input_mb)
            log(f"{o['exe'].name}: {len(extra)} game DLLs, {input_mb:.0f} MB of machine code to translate")
            r = build.recompile(o["exe"], o["out"], mem_mb=o["mem"], extra=extra, search=sorted({str(d.parent) for d in extra}),
                                system_dlls=o["sys_dlls"], log=log, progress=self.progress, lifter=o.get("lifter", "python"))
            log(f"{r['functions']} functions, {r['unsupported_total']} unsupported instructions, {len(r['missing_imports'])} Windows functions not implemented yet")
            (Path(o["out"]) / "options.json").write_text(json.dumps({"renderer": o["renderer"], "opt": o["opt"], "mem_mb": o["mem"]}, indent=2))
            if o["native"]:
                build.build_native(o["out"], log=log, progress=self.progress)
            web = build.build_wasm(o["out"], opt=o["opt"], renderer=o["renderer"], game_root=root, exe_rel=o["exe"].relative_to(root).as_posix(),
                                   size=o["size"], part_mb=o["part"], compress=o["compress"], log=log, progress=self.progress,
                                   video_height=o["size"][1] if o["videos"][0] < 0 else o["videos"][0], video_mode=o["videos"][1],
                                   wasm_opt=o["wasm_opt"], show_fps=o["show_fps"], async_shaders=o["async_shaders"], debug_panel=o["debug_panel"],
                                   backend=o.get("backend"), placeholders=o.get("placeholders", False), aes_key=o.get("aes"),
                                   mem_limit_gb=o.get("mem_limit", 8), threads=o.get("threads", "coop"), single_file=o.get("single", False), quality=o.get("quality"))
            self.est.remember()
            self.q.put(("done", str(web)))
        except Cancelled:
            self.q.put(("cancelled",))
        except WrongKey as e:
            self.q.put(("badkey", str(e)))
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
                    self.show_tools(m[1], m[2])
                elif kind == "installed":
                    _, key, name, where, then = m
                    self.finish(f"{name} installed", 1000)
                    self.append(f"\n{name} installed" + (f" in {where}" if where else ""))
                    if key == "emsdk" and where:
                        self.emsdk.set(where)
                    self.check_tools()
                    if then:
                        self.root.after(800, then)
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
                elif kind == "badkey":
                    self.finish("Failed: wrong AES key", None)
                    self.append("\nERROR: " + m[1] + " Enter the game's own correct AES key.")
                    self.aes_changed()
                    self.ask_aes_key(m[1])
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
        self.aes_changed()

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
        self.save_prefs()
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
