import json
import os
import re
import shutil
import subprocess
import threading
import time
from pathlib import Path

from . import resources

RUNTIME = Path(__file__).resolve().parents[2] / "runtime"
STDCALL_CACHE = Path(__file__).with_name("stdcall32.json")


def nop_progress(stage, done, total):
    pass


STEAM_VERSION = re.compile(rb"(Steam[A-Za-z]+\d{3}|STEAM[A-Z]+_INTERFACE_VERSION\d{3}|STEAM[A-Z]+_INTERFACE_V\d{3})\x00")


def steam_versions(game_root):
    """the Steam interface versions a game's own steam_api was built with: games that use the classic accessors
    (SteamUser()...) never name one, and WebWorks Steam lays each interface out by its version"""
    found = set()
    for f in Path(game_root).rglob("steam_api*.dll"):
        try:
            found.update(m.decode() for m in STEAM_VERSION.findall(f.read_bytes()))
        except OSError:
            pass
    return sorted(found)


RUST_DIR = Path(__file__).resolve().parents[2] / "rustlift"


def rust_tool(name, log=print):
    """one of the Rust tools' executables (rustlift/: uewlift, uewserve, ...), built with cargo when its sources are
    newer; None without a Rust toolchain"""
    exe = RUST_DIR / "target" / "release" / (name + (".exe" if os.name == "nt" else ""))
    srcs = list((RUST_DIR / "src").rglob("*")) + [RUST_DIR / "Cargo.toml"]
    # the packaged app ships the tools with their sources (unpacking doesn't keep file times)
    if exe.exists() and (getattr(__import__("sys"), "frozen", False) or exe.stat().st_mtime >= max(p.stat().st_mtime for p in srcs if p.is_file())):
        return exe
    cargo = shutil.which("cargo") or str(Path.home() / ".cargo" / "bin" / ("cargo.exe" if os.name == "nt" else "cargo"))
    if not Path(cargo).exists():
        return exe if exe.exists() else None
    log(f"building the Rust tools ({name})")
    r = resources.run([cargo, "build", "--release", "--manifest-path", str(RUST_DIR / "Cargo.toml")], capture_output=True, text=True)
    if r.returncode or not exe.exists():
        raise RuntimeError(f"could not build {name}:\n" + r.stderr[-2000:])
    # cargo leaves a tool it didn't need to relink untouched: mark them all current
    for t in (RUST_DIR / "target" / "release").glob("uew*" + (".exe" if os.name == "nt" else "")):
        os.utime(t)
    return exe


def run_tool(name, args, log=print, progress=nop_progress):
    """runs one of the Rust tools; its "@@progress stage done total" lines go to progress, "@@result <json>" is returned,
    everything else it prints goes to log"""
    exe = rust_tool(name, log)
    if exe is None:
        raise RuntimeError(f"{name} isn't built and there's no Rust toolchain to build it (https://rustup.rs)")
    p = subprocess.Popen([str(exe), *map(str, args)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8",
                         errors="replace", **resources.proc_flags())
    err = []
    t = threading.Thread(target=lambda: err.append(p.stderr.read()), daemon=True)
    t.start()
    result = None
    for line in p.stdout:
        if line.startswith("@@progress "):
            _, stage, done, total = line.split()
            progress(stage.replace("_", " "), int(done), int(total))
        elif line.startswith("@@result "):
            result = json.loads(line[9:])
        elif line.strip():
            log(line.rstrip())
    t.join()
    if p.wait():
        raise RuntimeError("".join(err).strip()[-4000:] or f"{name} {args[0]} failed")
    return result


def run_job(name, cmd, job, log=print, progress=nop_progress):
    """a Rust tool given its work as a JSON job file"""
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump(job, f)
    try:
        return run_tool(name, [cmd, f.name], log, progress) or {}
    finally:
        os.unlink(f.name)


def refresh_thunks(gen):
    """binds imports to runtime functions added since the lift, so a relink picks them up without lifting again"""
    if not (Path(gen) / "meta.c").exists():
        return 0
    return run_tool("uewrecomp", ["refresh", gen, RUNTIME, STDCALL_CACHE]) or 0


def game_dlls(exe, real_eos=False):
    """DLLs shipped with the game that it may load at runtime (PhysX, codecs, middleware): (install root, [paths]);
    real_eos keeps the game's EOS SDK among them"""
    r = json.loads(resources.run([str(rust_tool("uewrecomp")), "game-dlls", str(exe)] + (["--real-eos"] if real_eos else []),
                                 capture_output=True, text=True, check=True).stdout)
    return Path(r["root"]), [Path(d) for d in r["dlls"]]


def recompile(exe, out, mem_mb=1024, log=print, search=(), extra=(), system_dlls=True, progress=nop_progress, real_eos=False):
    """links the exe with its DLLs into image.bin, lifts every function to C in gen/ and writes gen/meta.c and
    coverage.json (uewrecomp and uewlift, rustlift/). progress(stage, done, total): total 0 means no measurable size.
    real_eos: recompile the game's own EOS SDK (it reaches Epic through WISP) instead of the runtime's stand-in.
    Returns the coverage report."""
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    return run_job("uewrecomp", "recompile", {"exe": str(exe), "out": str(out), "mem_mb": mem_mb, "search": [str(x) for x in search],
                                              "extra": [str(x) for x in extra], "system_dlls": system_dlls, "real_eos": real_eos, "runtime": str(RUNTIME),
                                              "stdcall_cache": str(STDCALL_CACHE)}, log, progress)


def emsdk_env():
    from ..deps import find_emsdk
    root = find_emsdk()
    if not root:
        raise RuntimeError("Emscripten SDK not found (choose it in the app's Tools tab, pass --emsdk, or run 'uewasm install-emsdk')")
    env = dict(os.environ)
    env["EMSDK"] = str(root)
    env["EM_CONFIG"] = str(root / ".emscripten")
    py = sorted((root / "python").glob("*/python.exe"))
    if py:
        env["EMSDK_PYTHON"] = str(py[-1])
    node = sorted((root / "node").glob("*/bin")) or sorted(p for p in (root / "node").glob("*") if (p / "node.exe").exists())
    env["PATH"] = os.pathsep.join([str(root), str(root / "upstream/emscripten")] + [str(n) for n in node[-1:]] + [env.get("PATH", "")])
    emcc = root / "upstream/emscripten" / ("emcc.exe" if os.name == "nt" else "emcc")
    if not emcc.exists():
        emcc = root / "upstream/emscripten/emcc.bat"
    nexe = shutil.which("node") or (str(node[-1] / "node.exe") if node else "node")
    return env, str(emcc), nexe


def jspi_flag(node):
    """newer Node ships JSPI enabled and rejects the old flag"""
    r = resources.run([node, "-e", "process.exit(typeof WebAssembly.Suspending == 'function' ? 0 : 1)"])
    return [] if r.returncode == 0 else ["--experimental-wasm-jspi"]


def _epic_args(exe_rel):
    # the game signs in to the offline account over WinInet, which the runtime answers (runtime/web/backends/epic.js)
    app = re.sub(r"(Client|Game)?-Win64-Shipping$", "", exe_rel.stem)
    return ["-HTTP=WinInet", "-AUTH_TYPE=epic", "-AUTH_LOGIN=Player", "-AUTH_PASSWORD=offline", f"-epicapp={app}", "-epicenv=Prod",
            "-epiclocale=en-us", "-epicportal", "-skippatchcheck", "-nobe", "-noeac", "-fromfl=none", "-fltoken=none"]


# offline stand-ins for online services: a script the page loads, and launch arguments that point the game at it
BACKENDS = {"epic": {"script": "epic.js", "args": _epic_args}}


def engine_of(exe):
    """which Unreal Engine generation an installed game is, from its folder layout: "Unreal Engine 3" (cooked packages
    in <Game>/Cooked*, binaries in Binaries/Win32 or Win64 at the top), "Unreal Engine 4/5" (Engine/Binaries and
    <Project>/Content/Paks), "Unreal Engine" when it's Unreal but unclear, or None"""
    for top in list(Path(exe).parents)[:5]:
        if not (top / "Engine").is_dir():
            continue
        try:
            kids = [d for d in top.iterdir() if d.is_dir() and d.name.lower() != "engine"]
            if any(c.is_dir() and c.name.lower().startswith("cooked") for d in kids for c in d.iterdir()):
                return "Unreal Engine 3"
        except OSError:
            pass
        if (top / "Engine" / "Binaries").is_dir() or any(top.glob("*/Content/Paks")):
            return "Unreal Engine 4/5"
        return "Unreal Engine"
    return None


def default_args(exe):
    """launch arguments a game needs here: Unreal games that can pick D3D12 are told to use D3D11, which this runtime
    translates to WebGPU"""
    try:
        data = Path(exe).read_bytes()
    except OSError:
        return []
    return ["-dx11"] if b"D3D11RHI" in data and b"D3D12RHI" in data else []


QUALITY = {"low": 0, "medium": 1, "high": 2, "epic": 3}
SCALABILITY = ("ShadowQuality", "PostProcessQuality", "EffectsQuality", "FoliageQuality", "ViewDistanceQuality", "AntiAliasingQuality",
               "ReflectionQuality", "GlobalIlluminationQuality", "ShadingQuality", "LandscapeQuality")


def quality_files(exe_rel, quality):
    """Unreal scalability groups for the page to put in the game's own settings file (GameUserSettings.ini, where the
    game's options menu keeps them); textures are left alone. UE5 reads Saved/Config/Windows, UE4 .../WindowsNoEditor"""
    parts = Path(exe_rel).parts
    if quality not in QUALITY or "Binaries" not in parts or parts.index("Binaries") < 1:
        return {}
    project = parts[parts.index("Binaries") - 1]
    # shadows stay at least medium: with none, lights shine through everything and scenes look plainly wrong
    text = "[ScalabilityGroups]\n" + "".join(f"sg.{g}={max(QUALITY[quality], 1) if g == 'ShadowQuality' else QUALITY[quality]}\n" for g in SCALABILITY)
    base = f"Users/Player/AppData/Local/{project}/Saved/Config"
    files = {f"{base}/{p}/GameUserSettings.ini": text for p in ("Windows", "WindowsNoEditor")}
    # low and medium post-processing turn off auto exposure (dark rooms come out far too dark) and the tonemapper's
    # contrast and vignette (corners too bright); together they cost one small pass, so they stay on
    if QUALITY[quality] < 2:
        ea = "".join(f"[PostProcessQuality@{i}]\nr.EyeAdaptationQuality=2\nr.Tonemapper.Quality=5\n" for i in (0, 1))
        files.update({f"{base}/{p}/Scalability.ini": ea for p in ("Windows", "WindowsNoEditor")})
    return files


def memory_split(limit_gb, guest_mb):
    """how a running game's memory limit is shared: the wasm heap (guest memory plus ~700 MB of runtime), the file
    download cache, and the rest for the GPU (reported to the game as video memory)"""
    total = int(limit_gb * 1024)
    cache = max(256, min(1024, total // 12))
    gpu = max(512, total - guest_mb - 700 - cache)
    return cache, gpu


def _uewbuild(cmd, job, log=print, progress=nop_progress):
    return run_job("uewbuild", cmd, job, log, progress)


def write_shell(web, args, title="Game", size=(1280, 720), save="", part_mb=24, show_fps=False, async_shaders=True, exe=None, debug_panel=True,
                placeholders=False, cache_mb=640, gpu_mb=2048, threads="coop", seed=None, mem64=False, steam=None, wisp=None):
    """index.html for a web build; game.wasm is cut into parts when it is too big for static hosts. mem64: the page
    checks for WebAssembly memory64 first and says so instead of failing to compile"""
    # size "native": the page sizes the canvas to the window's pixels at start (1280x720 until then, and as the fallback)
    native = size == "native"
    w, h = (1280, 720) if native else size
    _uewbuild("shell", {"web": str(web), "args": args, "title": title, "width": w, "height": h, "native": native, "save": save,
                        "part_mb": part_mb, "show_fps": show_fps, "async_shaders": async_shaders, "debug_panel": debug_panel,
                        "exe": str(exe or ""), "fallback_icon": str(Path(__file__).resolve().parents[1] / "assets" / "uewasm.ico"),
                        "placeholders": placeholders, "cache_mb": int(cache_mb), "gpu_mb": int(gpu_mb), "threads": threads,
                        "seed": seed or {}, "mem64": mem64, "steam": steam or {"mode": "offline"}, "wisp": wisp or {}})


def write_single_file(web, dest, log=print, chunk_mb=16):
    """one .html holding the whole web build (code and game files), for opening straight from disk with no server.
    Only for cooperative-thread builds: real threads need cross-origin isolation, which a file:// page can't have."""
    exe = rust_tool("uewbuild", log)
    r = resources.run([str(exe), "single", str(web), str(dest), str(chunk_mb)], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(r.stderr.strip()[-2000:])
    for line in r.stdout.splitlines():
        log(line)
    return Path(dest)


def mem_budget_mb():
    """RAM the compile jobs may share (see resources.plan; UEW_BUILD_MB overrides)"""
    return resources.plan()["budget_mb"]


def run_jobs(jobs, what, progress=nop_progress):
    """jobs: (name, cmd, env, estimated MB). Biggest first, as many at once as the cores and the memory budget allow.
    Progress is reported by estimated size, which tracks compile time."""
    import tempfile
    pending = sorted(jobs, key=lambda j: -j[3])
    workers = resources.plan()["compile_jobs"]
    budget = mem_budget_mb()
    running, used = [], 0
    total = sum(j[3] for j in jobs) or 1
    done = 0
    started, times = {}, []
    progress(what, 0, total)
    try:
        while pending or running:
            while pending and len(running) < workers and (not running or used + pending[0][3] <= budget):
                name, cmd, env, mb = pending.pop(0)
                log_f = tempfile.TemporaryFile("w+", encoding="utf-8", errors="replace")
                running.append((subprocess.Popen(cmd, env=env, stdout=log_f, stderr=subprocess.STDOUT, **resources.proc_flags()), name, mb, log_f))
                started[name] = time.time()
                used += mb
            time.sleep(0.05)
            for job in running[:]:
                p, name, mb, log_f = job
                if p.poll() is None:
                    continue
                running.remove(job)
                times.append((time.time() - started[name], name))
                used -= mb
                done += mb
                progress(what, done, total)
                if p.returncode:
                    log_f.seek(0)
                    raise RuntimeError(f"{what} failed on {name}:\n" + log_f.read()[-3000:])
                log_f.close()
    finally:
        for p, *_ in running:
            p.kill()
        # developer hook: UEW_TIMES=<file> records how long each job took (slowest first)
        if os.environ.get("UEW_TIMES") and times:
            Path(os.environ["UEW_TIMES"]).write_text("".join(f"{t:8.1f} {n}\n" for t, n in sorted(times, reverse=True)), encoding="utf-8")


# emscripten ports the runtime links (native stand-ins for game DLLs); fetched into the emsdk cache on first use
PORTS = ["-sUSE_OGG=1", "-sUSE_VORBIS=1"]


def build_wasm(out, log=print, opt="-O2", mem_mb=None, embed=True, node_fs=False, threads="coop", renderer="webgpu",
               game_root=None, exe_rel=None, title=None, size=(1280, 720), part_mb=16, compress=False, names=False, progress=nop_progress, video_height=0, video_mode="fast",
               wasm_opt=False, game_args=None, quality=None, simd=True, show_fps=False, async_shaders=True, debug_panel=True, backend=None,
               placeholders=False, aes_key=None, mem_limit_gb=8, single_file=False, web_name="web", mem64=False,
               steam="webworks", webworks_url="http://localhost:3000", relay=False, wisp=False, wisp_url=None,
               steam_wisp=False):
    """mem64: build a wasm64 module (64-bit memory), so guest memory can be larger than the 4 GB a wasm32 module can
    address (games that need more than ~3 GB). Needs a browser with WebAssembly memory64 (Chrome/Edge/Brave 133+); 64-bit
    addressing costs some speed, so it is only used when asked for.
    aes_key: the game's pak key (32 bytes); encrypted paks are checked against it (a wrong key fails the build) and
    older-format ones are packaged decrypted, so they compress.
    wasm_opt: also run Binaryen over the linked module. It trims ~10% off the size but needs roughly 20x the
    module size in RAM (11 GB for a 550 MB game), so it is off unless asked for.
    relay: online play through the built-in relay that `uewasm serve` runs at the game's own address.
    wisp / wisp_url: real network connections (the game's own servers, the real EOS SDK) through a WISP server: wisp
    uses the one `uewasm serve` runs at the game's address, wisp_url one elsewhere.
    steam_wisp: with steam="account" and WISP, the Steam sign-in and connection go through the WISP server too."""
    if renderer not in ("webgpu", "webgl2"):
        raise ValueError("renderer must be webgpu or webgl2")
    if threads not in ("coop", "real"):
        raise ValueError("threads must be coop or real")
    if threads == "real" and renderer != "webgpu":
        raise ValueError("real threads need the WebGPU renderer (WebGL only works from the page's own thread)")
    if single_file and threads == "real":
        raise ValueError("a single-file build can't use real threads (a page opened from disk can't be cross-origin isolated); use --threads coop")
    if single_file:
        compress = True
    out = Path(out)
    env, emcc, _ = emsdk_env()
    bound = refresh_thunks(out / "gen")
    if bound:
        log(f"{bound} imports bound to runtime functions added since the lift")
    plan = resources.plan()
    if not game_root:
        backend = None
    res = _uewbuild("compile", {
        "out": str(out), "runtime": str(RUNTIME), "emcc": emcc, "opt": opt, "simd": simd, "renderer": renderer, "threads": threads,
        "env": {k: env[k] for k in ("EMSDK", "EM_CONFIG", "EMSDK_PYTHON", "PATH") if k in env},
        "mem64": mem64, "mem64_defines": mem64_defines(out) if mem64 else [],
        # developer hooks: UEW_LTO=thin (ThinLTO into its own objects), UEW_EXTRA_CFLAGS (e.g. -DUEW_STORE_CHECK), UEW_TIMES=<file>
        "lto": os.environ.get("UEW_LTO") == "thin", "extra_cflags": os.environ.get("UEW_EXTRA_CFLAGS", "").split(),
        "times_file": os.environ.get("UEW_TIMES", ""), "debug_panel": debug_panel, "names": names, "wasm_opt": wasm_opt,
        "web_name": web_name, "game_files": bool(game_root), "backend_script": BACKENDS[backend]["script"] if backend else "",
        "node_fs": node_fs, "embed": embed and not game_root, "compile_jobs": plan["compile_jobs"], "budget_mb": plan["budget_mb"]},
        log, progress)
    web = out / web_name
    wasm_mb = res.get("wasm_mb", 0)
    if game_root:
        from .package import package
        replace = {}
        if aes_key:
            from .upak import decrypt_paks
            replace = decrypt_paks(game_root, aes_key, out, log=log, progress=progress)
        if video_height:
            from .media import shrink
            replace.update(shrink(game_root, Path(game_root) / exe_rel, out, video_height, video_mode, log=log, progress=progress, key=aes_key))
        package(game_root, web, extra={"/image.bin": out / "image.bin"}, part_mb=part_mb, compress=compress, log=log, progress=progress,
                replace=replace)
        save = "/save/" + re.sub(r"[^A-Za-z0-9_-]", "", Path(exe_rel).stem)
        if game_args is None:
            game_args = default_args(Path(game_root) / exe_rel)
        if backend:
            game_args = [*game_args, *BACKENDS[backend]["args"](Path(exe_rel))]
        args = ["--image", "/image.bin", "--root", "/game", "--save", save, "--exe", exe_rel, *game_args]
        guest = mem_mb or json.loads((out / "options.json").read_text()).get("mem_mb", 3072) if (out / "options.json").exists() else (mem_mb or 3072)
        cache_mb, gpu_mb = memory_split(mem_limit_gb, guest)
        log(f"memory limit {mem_limit_gb} GB: {guest} MB game memory, {cache_mb} MB download cache, {gpu_mb} MB video memory")
        write_shell(web, args, title or Path(exe_rel).stem, size, save, show_fps=show_fps, async_shaders=async_shaders,
                    exe=Path(game_root) / exe_rel, debug_panel=debug_panel, placeholders=placeholders, cache_mb=cache_mb, gpu_mb=gpu_mb,
                    threads=threads, seed=quality_files(exe_rel, quality), mem64=mem64,
                    steam={"mode": steam, "url": webworks_url, "relay": bool(relay), "versions": steam_versions(game_root)},
                    wisp={**({"url": wisp_url} if wisp_url else {"url": "origin"}), "steam": bool(steam_wisp and steam == "account")}
                    if wisp_url or wisp else {})
        for mark, on, what in (("uew-relay", relay, "the relay"), ("uew-wisp", wisp and not wisp_url, "the WISP server")):
            if on:
                (web / mark).write_text(f"this build goes online through {what} `uewasm serve` runs\n")
            elif (web / mark).exists():
                (web / mark).unlink()
        if single_file:
            name = re.sub(r'[<>:"/\\|?*]', "", title or Path(exe_rel).stem).strip() or "game"
            write_single_file(web, out / "single" / f"{name}.html", log=log)
    log(f"wasm build: {web} ({renderer}{', 64-bit memory' if mem64 else ''}, {res.get('files', 0)} files compiled, {res.get('seconds', 0):.1f}s, {wasm_mb:.1f} MB wasm)")
    return web


def msvc_env(arch="x64"):
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    path = resources.run([str(vswhere), "-latest", "-products", "*", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                           "-property", "installationPath"], capture_output=True, text=True).stdout.strip().splitlines()[0]
    bat = Path(path) / f"VC/Auxiliary/Build/vcvars{'64' if arch == 'x64' else '32'}.bat"
    comspec = os.environ.get("ComSpec", r"C:\Windows\System32\cmd.exe")
    out = resources.run(f'"{comspec}" /d /s /c "call "{bat}" >nul && set"', capture_output=True, text=True).stdout
    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k.upper()] = v
    if "VCTOOLSINSTALLDIR" not in env:
        raise RuntimeError("couldn't set up the MSVC environment")
    return env


def tool(env, name):
    p = shutil.which(name, path=env.get("PATH"))
    if not p:
        raise RuntimeError(f"{name} not found")
    return p


def mem64_defines(out):
    """C defines for a 64-bit memory build: a 32-bit game keeps its addresses wrapping at 4 GB (UEW_G32, rt.h)"""
    meta = Path(out) / "gen" / "meta.c"
    g32 = meta.exists() and "const int RT_G32 = 1;" in meta.read_text(encoding="utf-8", errors="replace")
    return ["UEW_MEM64", *(["UEW_G32"] if g32 else [])]


def build_native(out, log=print, debug=False, progress=nop_progress, mem64=False):
    """mem64: guest addresses past 4 GB, as in a --mem64 web build (the native build is 64-bit, so only the address
    macros change)"""
    out = Path(out)
    obj = out / ("obj_m64" if mem64 else "obj")
    obj.mkdir(exist_ok=True)
    env = msvc_env()
    gen = sorted((out / "gen").glob("*.c"))
    stale = [p for p in gen if not (obj / (p.stem + ".obj")).exists() or (obj / (p.stem + ".obj")).stat().st_mtime < p.stat().st_mtime]
    if (out / "gen" / "funcs.h").exists() and any((obj / (p.stem + ".obj")).exists() for p in gen):
        newest_h = max((RUNTIME / n).stat().st_mtime for n in ("rt.h", "sse.h"))
        stale = [p for p in gen if p in stale or (obj / (p.stem + ".obj")).stat().st_mtime < newest_h]
    for o in obj.glob("*.obj"):
        if not (out / "gen" / (o.stem + ".c")).exists():
            o.unlink()
    srcs = stale + sorted(RUNTIME.glob("*.c"))
    flags = ["/nologo", "/c", "/O1" if not debug else "/Od", "/W0", "/bigobj",
             f"/I{RUNTIME}", f"/I{out / 'gen'}", f"/Fo{obj}\\", "/D_CRT_SECURE_NO_WARNINGS", "/std:c11"]
    if debug:
        flags.append("/Z7")
    if mem64:
        flags += [f"/D{d}" for d in mem64_defines(out)]
    t0 = time.time()
    cl = tool(env, "cl")
    # BearSSL (Schannel's TLS): its own object folder, compiled when missing or older than its source
    bssl, bobj = RUNTIME / "third_party" / "bearssl", obj / "bssl"
    bobj.mkdir(exist_ok=True)
    inc = [f"/I{bssl / 'inc'}", f"/I{bssl / 'src'}"]
    flags += [f"/I{bssl / 'inc'}"]
    lib = [p for p in sorted((bssl / "src").rglob("*.c"))
           if not (bobj / (p.stem + ".obj")).exists() or (bobj / (p.stem + ".obj")).stat().st_mtime < p.stat().st_mtime]
    jobs = [(p.name, [cl, *flags, str(p)], env, 80 + 40 * p.stat().st_size / 1048576) for p in srcs]
    jobs += [(p.name, [cl, *[f for f in flags if not f.startswith("/Fo")], *inc, f"/Fo{bobj}\\", str(p)], env, 80) for p in lib]
    run_jobs(jobs, "native compile", progress)
    progress("native link", 0, 0)
    exe = out / "native.exe"
    lrsp = out / "link.rsp"
    lrsp.write_text("\n".join(f'"{p}"' for p in [*obj.glob("*.obj"), *bobj.glob("*.obj")]), encoding="utf-8")
    r = resources.run([tool(env, "link"), "/nologo", f"/OUT:{exe}", "/STACK:268435456", f"/MAP:{out / 'native.map'}", f"@{lrsp}"],
                       env=env, capture_output=True, text=True)
    progress("native link", 1, 1)
    if r.returncode:
        raise RuntimeError("link failed:\n" + r.stdout[-2000:])
    log(f"native build: {exe} ({len(stale)} generated files rebuilt, {time.time() - t0:.1f}s)")
    return exe
