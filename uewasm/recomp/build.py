import json
import os
import re
import shutil
import subprocess
import time
from collections import Counter
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from . import pe as pemod
from . import resources
from .lift import Func, Program
from .link import THUNK_BASE, Linker

RUNTIME = Path(__file__).resolve().parents[2] / "runtime"
CHUNK = 400



XFL_DECL = """#ifdef _MSC_VER
extern __declspec(thread) uint64_t rt_xfl;
#else
extern _Thread_local uint64_t rt_xfl;
#endif"""


def write_if_changed(path, text):
    path = Path(path)
    try:
        if path.read_text(encoding="utf-8") == text:
            return
    except OSError:
        pass
    path.write_text(text, encoding="utf-8")

def implemented_hle():
    names = set()
    for f in RUNTIME.glob("*.c"):
        text = "\n".join(l for l in f.read_text(encoding="utf-8").splitlines() if not l.lstrip().startswith("#define"))
        names.update(re.findall(r"\b(?:HLE|MATH1F?|MATH2F?|STUB)\((\w+)", text))
    return names


def refresh_thunks(gen):
    """binds imports to HLE functions added to the runtime since the lift, so a relink picks them up without lifting again"""
    meta = gen / "meta.c"
    if not meta.exists():
        return 0
    text = meta.read_text(encoding="utf-8")
    hle = implemented_hle()
    have = set(re.findall(r"^void hle_(\w+)\(CPU\*\);", text, re.M))
    listed = set(re.findall(r'^    \{\w+, "([^"]+)", "[^"]*", \w+\},', text, re.M))
    new = sorted({n for n in re.findall(r'^    \{NULL, "(\w+)", "[^"]*", NULL\},', text, re.M) if n in hle} - have)
    extra = sorted(hle - listed - have)
    if "RT_G32" not in text:
        text += "const int RT_G32 = 0;\nconst unsigned short rt_thunk_pops[] = {0};\n"
        if not new and not extra:
            write_if_changed(meta, text)
    if not new and not extra:
        return 0
    text = re.sub(r'^    \{NULL, "(\w+)", ("[^"]*"), NULL\},',
                  lambda m: f'    {{hle_{m[1]}, "{m[1]}", {m[2]}, T_{m[1]}}},' if m[1] in new else m[0], text, flags=re.M)
    # functions nothing imports, for GetProcAddress
    if extra:
        j = text.index("};", text.index("const Thunk rt_thunks[] = {"))
        text = text[:j] + "".join(f'    {{hle_{n}, "{n}", "hle", T_{n}}},\n' for n in extra) + text[j:]
        text = re.sub(r"const size_t rt_thunk_count = (\d+);", lambda m: f"const size_t rt_thunk_count = {int(m[1]) + len(extra)};", text)
        if "const int RT_G32 = 1;" in text:
            from . import stdcall
            add = ", ".join(str(stdcall.pops("", n) or 0) for n in extra)
            text = re.sub(r"(const unsigned short rt_thunk_pops\[\] = \{[^}]*)\};", lambda m: m[1] + ", " + add + "};", text)
    new = sorted(set(new) | set(extra))
    decl = "".join(f"void hle_{n}(CPU*);\n" for n in new)
    decl += "".join(f"static void T_{n}(CPU* c) {{ if (rt_trace_on > 0) rt_trace_call(c, \"{n}\"); hle_{n}(c); c->r[4] += 8; "
                    f"if (--rt_hle_budget < 0) rt_hle_tick(c); }}\n" for n in new)
    i = text.index("const Thunk rt_thunks[] = {")
    write_if_changed(meta, text[:i] + decl + text[i:])
    return len(new)


def thunk_pops(thunks, bits):
    """rt_thunk_pops[]: argument bytes each import's callee pops (32-bit stdcall); unknown ones are listed"""
    if bits != 32:
        return "const unsigned short rt_thunk_pops[] = {0};"
    from . import stdcall
    vals, unknown = [], []
    for name, dll in thunks:
        v = stdcall.pops(dll if dll != "hle" else "", name)
        if v is None:
            if dll != "hle":
                unknown.append(name)
            v = 0
        vals.append(v)
    stdcall.save()
    if unknown:
        print(f"  stack argument size unknown for {len(unknown)} imports (taken as 0): {', '.join(unknown[:12])}")
    return "const unsigned short rt_thunk_pops[] = {" + ", ".join(map(str, vals)) + "};"


_W = {}

# Win64: a call may change these (rax rcx rdx r8-r11); rsp comes back moved by the callee's ret
VOLATILE = {0, 1, 2, 4, 8, 9, 10, 11}
_WRITE = re.compile(r"\bR\((\d+)\)\s*(?:[-+&|^]|<<|>>)?=(?!=)")


def regs_local(body, bits=64):
    """expands SPILL/FILL/FILLV for one lifted function: spill only registers it writes, reload only ones it reads
    (after a guest call, only those the callee may have changed; 32-bit code has no fixed convention to lean on)"""
    text = "\n".join(body)
    used = sorted({int(n) for n in re.findall(r"\bR\((\d+)\)", text)})
    wrote = sorted({int(n) for n in _WRITE.findall(text)})
    spill = "".join(f"c->r[{n}] = r{n}_; " for n in wrote) or ""
    fill = "".join(f"r{n}_ = c->r[{n}]; " for n in used)
    fillv = "".join(f"r{n}_ = c->r[{n}]; " for n in used if n in VOLATILE or bits == 32)
    return [l.replace("SPILL(); ", spill).replace("FILLV();", fillv).replace("FILL();", fill) for l in body]


def _init_worker(all_roots, comments, mode="high"):
    resources.set_mode(mode)
    resources.lower_this_process()
    _W["roots"] = all_roots
    _W["comments"] = comments
    _W["progs"] = {}


def _lift_chunk(args):
    path, base, roots, blocks, ends, out_path, jt = args
    prog = _W["progs"].get(path)
    if prog is None:
        prog = Program(pemod.load(path), base)
        prog.roots = _W["roots"]
        prog.comments = _W["comments"]
        _W["progs"][path] = prog
    prog.blocks = blocks
    prog.ends = ends
    prog.jt = jt
    prog.unsupported.clear()
    prog.unsupported_at.clear()
    out = ['#define LIFTED 1', '#include "rt.h"', '#include "funcs.h"', ""]
    bad = 0
    for r in roots:
        fn = Func(prog, r)
        bad += fn.lift()
        out.append(f"void F_{r:x}(CPU* __restrict c) {{")
        entry = "2" if prog.func_start(r) or not prog.flags_live(r) else "rt_xfl"
        body = regs_local(fn.out, prog.bits)
        used = sorted({int(n) for l in body for n in re.findall(r"\bR\((\d+)\)", l)})
        decl = (" uint64_t " + ", ".join(f"r{n}_ = c->r[{n}]" for n in used) + ";") if used else ""
        out.append(f"    uint8_t* const mem_ = MEM; Flags F = {{0}}; F.fop = F_RAW; F.rawfl = {entry}; F.fsz = 64;{decl}")
        if any("setjmp(" in l for l in body):
            # a frame that holds a host setjmp can't be replaced by a tail call
            body = [l.replace("TAIL ", "") for l in body]
        out.extend(body)
        out.append("}")
        out.append("")
    prog.cache.clear()
    text = "\n".join(out)
    if "rt_sj_slot(" in text or "rt_sj_slot32(" in text:
        text = text.replace('#include "funcs.h"', '#include "funcs.h"\n#include <setjmp.h>\njmp_buf* rt_sj_slot(CPU* c, uint64_t jb, uint64_t frame, uint64_t rip);\n'
                            'jmp_buf* rt_sj_slot32(CPU* c, uint64_t jb, uint64_t rip);', 1)
    if "rt_xfl" in text:
        text = text.replace('#include "funcs.h"', '#include "funcs.h"\n' + XFL_DECL, 1)
    write_if_changed(out_path, text)
    return dict(prog.unsupported), {k: [hex(a) for a in v] for k, v in prog.unsupported_at.items()}, bad


def _lifted(ex, tasks, progress):
    for n, r in enumerate(ex.map(_lift_chunk, tasks, chunksize=4)):
        progress("lift", n + 1, len(tasks))
        yield r


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def nop_progress(stage, done, total):
    pass


def recompile(exe, out, mem_mb=1024, comments=False, jobs=None, log=print, search=(), extra=(), system_dlls=True, progress=nop_progress):
    """progress(stage, done, total) is called as work completes; total 0 means the stage has no measurable size"""
    out = Path(out)
    gen = out / "gen"
    gen.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    hle = implemented_hle()
    link = Linker(exe, search=search, hle_names=hle, recompile_system=system_dlls, extra=extra)
    images = link.images()

    progs = []
    all_roots = set()
    sizes = [m.pe.image_size for m in link.order]
    for k, m in enumerate(link.order):
        progress("analyze", sum(sizes[:k]), sum(sizes))
        prog = Program(m.pe, m.base)
        prog.discover()
        progs.append((m, prog))
        all_roots.update(prog.blocks)
        log(f"  {m.name}: {len(prog.blocks)} functions at 0x{m.base:x}")
    roots = sorted(all_roots)
    log(f"{len(link.order)} modules, {len(roots)} functions ({time.time() - t0:.1f}s)")

    span = bytearray(link.end - link.order[0].base)
    lo = link.order[0].base
    for m, img in images:
        span[m.base - lo:m.base - lo + len(img)] = img
    (out / "image.bin").write_bytes(bytes(span))
    write_if_changed(gen / "funcs.h", '#pragma once\n#include "rt.h"\n' + "".join(f"void F_{r:x}(CPU* __restrict);\n" for r in roots))

    tasks = []
    for m, prog in progs:
        mroots = sorted(prog.blocks)
        for i in range(0, len(mroots), CHUNK):
            ch = mroots[i:i + CHUNK]
            jt = {k: v for k, v in prog.jt.items() if m.base <= k < m.base + m.pe.image_size}
            tasks.append((str(m.path), m.base, ch, {r: prog.blocks[r] for r in ch}, {r: prog.ends.get(r) for r in ch},
                          str(gen / f"funcs_{len(tasks):04d}.c"), {k: v for k, v in jt.items() if ch[0] <= k < max(prog.ends.get(r) or r + 0x100000 for r in ch)}))
    unsupported = Counter()
    where = {}
    bad = 0
    for _, prog in progs:
        prog.cache.clear()
    workers = jobs or resources.plan()["lift_workers"]
    progress("analyze", 1, 1)
    with ProcessPoolExecutor(max_workers=workers, initializer=_init_worker, initargs=(all_roots, comments, resources.mode())) as ex:
        try:
            results = list(_lifted(ex, tasks, progress))
        except BaseException:
            ex.shutdown(wait=False, cancel_futures=True)
            raise
        for u, at, b in results:
            unsupported.update(u)
            for k, v in at.items():
                where.setdefault(k, v)
            bad += b
    log(f"lifted into {len(tasks)} C files ({time.time() - t0:.1f}s), {bad} instructions unsupported")

    lines = ['#include "host.h"', '#include "funcs.h"', ""]
    lines.append(f"const uint64_t IMAGE_LO = 0x{lo:x}ull, IMAGE_HI = 0x{link.end:x}ull, GUEST_MEM_MB = {mem_mb}ull;")
    lines.append(f"const int RT_G32 = {int(link.bits == 32)};")
    cbs = []
    lines.append("const ModuleInfo rt_modules[] = {")
    for m in link.init_order:
        t = m.pe.tls
        first = len(cbs)
        cbs += [m.base + x for x in m.pe.tls_callbacks]
        tls = (m.base + t["start"], m.base + t["end"], m.base + t["index"], t["zero"]) if t else (0, 0, 0, 0)
        lines.append(f"    {{{c_str(m.name)}, 0x{m.base:x}ull, 0x{m.pe.image_size:x}ull, 0x{m.pe.entry_rva:x}ull, {int(m.is_dll)}, "
                     f"0x{tls[0]:x}ull, 0x{tls[1]:x}ull, 0x{tls[2]:x}ull, {tls[3]}ull, {first}, {len(cbs) - first}}},")
    lines.append("};")
    lines.append(f"const size_t rt_module_count = {len(link.init_order)};")
    lines.append("const uint64_t rt_tls_callbacks[] = {" + ", ".join(f"0x{x:x}ull" for x in cbs + [0]) + "};")
    # every implemented HLE function gets a thunk, so GetProcAddress and delay-loads find ones nothing imports
    imported = {n for n, _ in link.thunks}
    thunks = list(link.thunks) + [(n, "hle") for n in sorted(hle - imported)]
    impl = sorted({n for n, _ in thunks if n in hle})
    lines += [f"void hle_{n}(CPU*);" for n in impl]
    lines += [f"static void T_{n}(CPU* c) {{ if (rt_trace_on > 0) rt_trace_call(c, {c_str(n)}); hle_{n}(c); c->r[4] += 8; "
              f"if (--rt_hle_budget < 0) rt_hle_tick(c); }}" for n in impl]
    lines.append("const Thunk rt_thunks[] = {")
    for name, dll in thunks:
        fn, call = (f"hle_{name}", f"T_{name}") if name in hle else ("NULL", "NULL")
        lines.append(f"    {{{fn}, {c_str(name)}, {c_str(dll)}, {call}}},")
    lines.append("};")
    lines.append(f"const size_t rt_thunk_count = {len(thunks)};")
    lines.append(thunk_pops(thunks, link.bits))
    lines.append("const FnEntry rt_fn_table[] = {")
    lines += [f"    {{0x{r:x}ull, F_{r:x}}}," for r in roots]
    lines.append("    {0, NULL}};")
    lines.append(f"const size_t rt_fn_count = {len(roots)};")
    write_if_changed(gen / "meta.c", "\n".join(lines) + "\n")
    keep = {Path(t[5]).name for t in tasks} | {"meta.c", "funcs.h"}
    for f in gen.iterdir():
        if f.name not in keep:
            f.unlink()

    report = {
        "exe": str(exe), "modules": {m.name: str(m.path) for m in link.order}, "functions": len(roots),
        "imports": sum(len(m.pe.imports) for m in link.order),
        "unsupported_total": bad, "unsupported": dict(unsupported.most_common()),
        "unsupported_examples": where, "missing_imports": link.missing,
        "seconds": round(time.time() - t0, 1),
    }
    (out / "coverage.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    return report



def emsdk_env():
    here = Path(__file__).resolve()
    cands = [os.environ.get("EMSDK"), Path.home() / "emsdk", here.parents[2] / ".toolchain/emsdk", here.parents[3] / ".toolchain/emsdk"]
    for c in cands:
        if c and (Path(c) / "upstream/emscripten").is_dir():
            root = Path(c)
            break
    else:
        raise RuntimeError("Emscripten SDK not found (set EMSDK)")
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


SHELL = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<link rel="icon" href="__ICON__">
<style>
  :root { color-scheme: dark; }
  html, body { margin: 0; height: 100%; background: #000; overflow: hidden; }
  canvas { position: absolute; inset: 0; margin: auto; max-width: 100%; max-height: 100%; aspect-ratio: __W__ / __H__;
           width: 100%; height: auto; outline: none; }
  @media (min-aspect-ratio: __W__ / __H__) { canvas { width: auto; height: 100%; } }
  #status { position: fixed; left: 16px; bottom: 12px; font: 13px system-ui, sans-serif; color: #9a9a9a; pointer-events: none; }
  #fps { position: fixed; left: 8px; top: 6px; font: 600 13px ui-monospace, Consolas, monospace; color: #e8e8e8; background: #000a;
         padding: 2px 6px; border-radius: 4px; pointer-events: none; display: none; }
</style>
</head>
<body>
<canvas id="canvas" width="__W__" height="__H__" tabindex="0"></canvas>
<div id="status">Loading…</div>
<div id="fps"></div>
<script>
  // frame counter: Ctrl+Shift+F toggles it (remembered), ?fps=1 or ?fps=0 in the address overrides
  (() => {
    const el = document.getElementById("fps");
    let on = __FPS__;
    try { const v = localStorage.getItem("uewFps"); if (v !== null) on = v === "1"; } catch (e) {}
    const q = new URLSearchParams(location.search).get("fps");
    if (q !== null) on = q !== "0";
    const show = () => { el.style.display = on ? "block" : "none"; };
    show();
    addEventListener("keydown", e => {
      if (e.ctrlKey && e.shiftKey && e.code === "KeyF") {
        on = !on; show();
        try { localStorage.setItem("uewFps", on ? "1" : "0"); } catch (err) {}
        e.preventDefault(); e.stopImmediatePropagation();
      }
    }, true);
    let last = performance.now(), frames = 0, worst = 0, prev = 0, prevT = last;
    const tick = now => {
      const n = (window.Module && Module.uewPresents) || 0;
      if (n !== prev) { worst = Math.max(worst, now - prevT); prev = n; prevT = now; }
      if (now - last >= 500) {
        const fps = (n - frames) * 1000 / (now - last);
        if (on) el.textContent = `${fps.toFixed(0)} FPS  ${fps ? (1000 / fps).toFixed(1) : "-"} ms  worst ${worst.toFixed(0)} ms`;
        frames = n; last = now; worst = 0;
      }
      requestAnimationFrame(tick);
    };
    requestAnimationFrame(tick);
  })();
</script>
<script>
  const status = document.getElementById("status");
  const parts = __PARTS__;
  var Module = {
    canvas: document.getElementById("canvas"),
    arguments: __ARGS__.concat(new URLSearchParams(location.search).getAll("arg")),
    uewSave: __SAVE__,
    uewAsyncShaders: __ASYNC__,
    uewDebugPanel: __DEBUG__,
    uewNetPlaceholders: __PLACEHOLDERS__,
    uewCacheMB: __CACHEMB__,
    uewGpuMB: __GPUMB__,
    print: t => console.log(t),
    printErr: t => console.log(t),
    setStatus: t => { if (t) status.textContent = t; },
    onRuntimeInitialized: () => { status.textContent = ""; },
  };
  if (parts.length) {
    Module.instantiateWasm = (imports, done) => {
      (async () => {
        let got = 0;
        const bufs = await Promise.all(parts.map(async u => {
          const b = new Uint8Array(await (await fetch(u)).arrayBuffer());
          got += b.length;
          status.textContent = `Loading code ${Math.round(got / 1048576)} MB`;
          return b;
        }));
        const all = new Uint8Array(bufs.reduce((n, b) => n + b.length, 0));
        let o = 0;
        for (const b of bufs) { all.set(b, o); o += b.length; }
        status.textContent = "Compiling…";
        const r = await WebAssembly.instantiate(all, imports);
        done(r.instance, r.module);
      })().catch(e => { status.textContent = "Failed to load: " + e.message; });
      return {};
    };
  }
</script>
<script src="game.js"></script>
</body>
</html>
"""


def _epic_args(exe_rel):
    # the game signs in to the offline account over WinInet, which the runtime answers (runtime/web/backends/epic.js)
    app = re.sub(r"(Client|Game)?-Win64-Shipping$", "", exe_rel.stem)
    return ["-HTTP=WinInet", "-AUTH_TYPE=epic", "-AUTH_LOGIN=Player", "-AUTH_PASSWORD=offline", f"-epicapp={app}", "-epicenv=Prod",
            "-epiclocale=en-us", "-epicportal", "-skippatchcheck", "-nobe", "-noeac", "-fromfl=none", "-fltoken=none"]


# offline stand-ins for online services: a script the page loads, and launch arguments that point the game at it
BACKENDS = {"epic": {"script": "epic.js", "args": _epic_args}}


def default_args(exe):
    """launch arguments a game needs here: Unreal games that can pick D3D12 are told to use D3D11, which this runtime
    translates to WebGPU"""
    try:
        data = Path(exe).read_bytes()
    except OSError:
        return []
    return ["-dx11"] if b"D3D11RHI" in data and b"D3D12RHI" in data else []


def memory_split(limit_gb, guest_mb):
    """how a running game's memory limit is shared: the wasm heap (guest memory plus ~700 MB of runtime), the file
    download cache, and the rest for the GPU (reported to the game as video memory)"""
    total = int(limit_gb * 1024)
    cache = max(256, min(1024, total // 12))
    gpu = max(512, total - guest_mb - 700 - cache)
    return cache, gpu


def write_shell(web, args, title="Game", size=(1280, 720), save="", part_mb=24, show_fps=False, async_shaders=True, exe=None, debug_panel=True,
                placeholders=False, cache_mb=640, gpu_mb=2048):
    """index.html for a web build; game.wasm is cut into parts when it is too big for static hosts"""
    import json
    wasm = web / "game.wasm"
    for old in web.glob("game.wasm.*"):
        old.unlink()
    parts = []
    if wasm.stat().st_size > part_mb << 20:
        data = wasm.read_bytes()
        step = part_mb << 20
        for i in range(0, len(data), step):
            (web / f"game.wasm.{i // step}").write_bytes(data[i:i + step])
            parts.append(f"game.wasm.{i // step}")
        wasm.unlink()
    from .icon import write_favicon
    icon = write_favicon(web, exe) or "data:,"
    html = (SHELL.replace("__ICON__", icon).replace("__TITLE__", title).replace("__W__", str(size[0])).replace("__H__", str(size[1]))
            .replace("__PARTS__", json.dumps(parts)).replace("__ARGS__", json.dumps(args)).replace("__SAVE__", json.dumps(save))
            .replace("__FPS__", "true" if show_fps else "false")
            .replace("__ASYNC__", "true" if async_shaders else "false")
            .replace("__DEBUG__", "true" if debug_panel else "false").replace("__PLACEHOLDERS__", "true" if placeholders else "false")
            .replace("__CACHEMB__", str(int(cache_mb))).replace("__GPUMB__", str(int(gpu_mb))))
    (web / "index.html").write_text(html, encoding="utf-8")
    # Cloudflare Pages reads this; other hosts ignore it
    (web / "_headers").write_text("\n".join([
        "/*", "  Cross-Origin-Opener-Policy: same-origin", "  Cross-Origin-Embedder-Policy: require-corp", "",
        "/game.wasm*", "  Content-Type: application/wasm", "",
        "/data/*", "  Content-Type: application/octet-stream", "  Cache-Control: public, max-age=3600, must-revalidate", ""]),
        encoding="utf-8")


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
    progress(what, 0, total)
    try:
        while pending or running:
            while pending and len(running) < workers and (not running or used + pending[0][3] <= budget):
                name, cmd, env, mb = pending.pop(0)
                log_f = tempfile.TemporaryFile("w+", encoding="utf-8", errors="replace")
                running.append((subprocess.Popen(cmd, env=env, stdout=log_f, stderr=subprocess.STDOUT, **resources.proc_flags()), name, mb, log_f))
                used += mb
            time.sleep(0.05)
            for job in running[:]:
                p, name, mb, log_f = job
                if p.poll() is None:
                    continue
                running.remove(job)
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


# emscripten ports the runtime links (native stand-ins for game DLLs); fetched into the emsdk cache on first use
PORTS = ["-sUSE_OGG=1", "-sUSE_VORBIS=1"]


def build_wasm(out, log=print, opt="-O2", mem_mb=None, embed=True, node_fs=False, threads="coop", renderer="webgpu",
               game_root=None, exe_rel=None, title=None, size=(1280, 720), part_mb=16, compress=False, names=False, progress=nop_progress, video_height=0, video_mode="fast",
               wasm_opt=False, game_args=None, simd=True, show_fps=False, async_shaders=True, debug_panel=True, backend=None,
               placeholders=False, aes_key=None, mem_limit_gb=8):
    """aes_key: the game's pak key (32 bytes); encrypted paks are checked against it (a wrong key fails the build) and
    older-format ones are packaged decrypted, so they compress.
    wasm_opt: also run Binaryen over the linked module. It trims ~10% off the size but needs roughly 20x the
    module size in RAM (11 GB for a 550 MB game), so it is off unless asked for."""
    if renderer not in ("webgpu", "webgl2"):
        raise ValueError("renderer must be webgpu or webgl2")
    if threads not in ("coop", "real"):
        raise ValueError("threads must be coop or real")
    if threads == "real":
        raise NotImplementedError("real (Web Worker) threading is not finished yet; use --threads coop")
    out = Path(out)
    obj = out / "wobj"
    obj.mkdir(exist_ok=True)
    env, emcc, _ = emsdk_env()
    # simd: wasm SIMD (every current browser) lets clang turn the emulated SSE lanes into real vector ops
    cflags = [opt, "-w", "-mtail-call", *(["-msimd128"] if simd else []), "-sSUPPORT_LONGJMP=wasm", "-DUEW_WEB_GPU", f"-I{RUNTIME}", f"-I{out / 'gen'}", "-D_CRT_SECURE_NO_WARNINGS", "-fno-strict-aliasing"]
    # developer hook: extra compiler flags for every file (e.g. -DUEW_STORE_CHECK for a store-checking debug build)
    cflags += os.environ.get("UEW_EXTRA_CFLAGS", "").split()
    if renderer == "webgpu":
        cflags.append("-DUEW_WEBGPU")
    # generated code only depends on rt.h/sse.h and the opt level; the runtime is always rebuilt (it is small)
    stamp = obj / "flags.txt"
    key = " ".join(cflags)
    if not stamp.exists() or stamp.read_text() != key:
        for o in obj.glob("*.o"):
            o.unlink()
        stamp.write_text(key)
    newest_h = max((RUNTIME / n).stat().st_mtime for n in ("rt.h", "sse.h"))
    bound = refresh_thunks(out / "gen")
    if bound:
        log(f"{bound} imports bound to runtime functions added since the lift")
    gen = sorted((out / "gen").glob("*.c"))
    for o in obj.glob("*.o"):
        if not (out / "gen" / (o.stem + ".c")).exists() and not (RUNTIME / (o.stem + ".c")).exists():
            o.unlink()

    def stale(p):
        o = obj / (p.stem + ".o")
        return not o.exists() or o.stat().st_mtime < max(p.stat().st_mtime, newest_h)
    gen_srcs = [p for p in gen if stale(p)]
    srcs = gen_srcs + sorted(RUNTIME.glob("*.c"))
    t0 = time.time()
    # generated files go straight to clang with emcc's own flags, skipping emcc's per-file startup
    clang = Path(emcc).parents[1] / "bin" / ("clang.exe" if os.name == "nt" else "clang")
    import shlex
    em = shlex.split(resources.run([emcc, "--cflags", *cflags], env=env, capture_output=True, text=True).stdout)
    direct = clang.exists() and em
    # the debug panel and what it drives (UE console enabler, game speed, call tracing) are only built in when wanted, so
    # a build without them carries none of that code (nothing for a game's anti-cheat to find)
    debug_defs = ["-DUEW_DEBUG_TOOLS"] if debug_panel else []
    exports = ["_main", "_uew_ctx_start", "_uew_threads"] + (["_uew_set_trace", "_uew_set_speed", "_uew_ue_console"] if debug_panel else [])
    jobs = []
    for src in srcs:
        o = str(obj / (src.stem + ".o"))
        # peak clang memory measured on generated files: ~175 MB plus ~60 MB per MB of source
        mb = 190 + 62 * src.stat().st_size / 1048576
        if direct and src in gen_srcs:
            jobs.append((src.name, [str(clang), *em, *(f for f in cflags if not f.startswith("-s")), "-c", str(src), "-o", o], env, mb))
        else:
            jobs.append((src.name, [emcc, *cflags, *PORTS, *debug_defs, "-c", str(src), "-o", o], env, mb + 200))
    run_jobs(jobs, "wasm compile", progress)
    progress("link", 0, 0)
    web = out / "web"
    web.mkdir(exist_ok=True)
    orsp = out / "emlink.rsp"
    orsp.write_text("\n".join(f'"{p.as_posix()}"' for p in obj.glob("*.o")), encoding="utf-8")
    link = [emcc, opt if wasm_opt else "-O1", "-mtail-call", *(["-msimd128"] if simd else []), f"@{orsp}", "-o", str(web / "game.js"),
            "-sALLOW_MEMORY_GROWTH=1", "-sINITIAL_MEMORY=64MB", "-sMAXIMUM_MEMORY=4GB", "-sSTACK_SIZE=16MB", "-sENVIRONMENT=web,node",
            "-sEXIT_RUNTIME=1", "-sMODULARIZE=0", "-sINVOKE_RUN=1", *PORTS,
            "-sMIN_WEBGL_VERSION=2", "-sMAX_WEBGL_VERSION=2", "-sSUPPORT_LONGJMP=wasm", "-sJSPI", "-sJSPI_EXPORTS=['uew_ctx_start']", "-sEXPORTED_FUNCTIONS=" + ",".join(exports)]
    if renderer == "webgpu":
        link += ["--js-library", str(RUNTIME / "web" / "uew_webgpu.js")]
    if names:
        link.append("--profiling-funcs")
    if game_root:
        embed = False
        link += ["--pre-js", str(RUNTIME / "web" / "uew_files.js"), "--pre-js", str(RUNTIME / "web" / "uew_save.js"), *(["--pre-js", str(RUNTIME / "web" / "uew_debug.js")] if debug_panel else []), "-lidbfs.js",
                 "-sFORCE_FILESYSTEM=1"]
        if backend:
            link += ["--pre-js", str(RUNTIME / "web" / "backends" / BACKENDS[backend]["script"])]
    if node_fs:
        link += ["-sNODERAWFS=1", "-sENVIRONMENT=node"]
        link.remove("-sENVIRONMENT=web,node")
    elif embed:
        link += ["--embed-file", f"{out / 'image.bin'}@image.bin"]
    r = resources.run(link, env=env, capture_output=True, text=True)
    progress("link", 1, 1)
    if r.returncode:
        errs = [l for l in r.stderr.splitlines() if "error" in l.lower() and len(l) < 600]
        raise RuntimeError("emcc link failed:\n" + "\n".join(errs[:20] or [r.stderr[-3000:]]))
    wasm_mb = (web / "game.wasm").stat().st_size / 1e6
    # browsers refuse modules over 1 GiB (V8's limit)
    wasm_bytes = (web / "game.wasm").stat().st_size
    if wasm_bytes > 1 << 30:
        log(f"warning: the code is {wasm_bytes / 1048576:.0f} MiB, over the 1024 MiB browsers accept; rebuild with --opt -Os "
            "(and --wasm-opt if there is enough memory) to make it smaller")
    elif wasm_bytes > 900 << 20:
        log(f"note: the code is {wasm_bytes / 1048576:.0f} MiB, close to the 1024 MiB browsers accept")
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
                    exe=Path(game_root) / exe_rel, debug_panel=debug_panel, placeholders=placeholders, cache_mb=cache_mb, gpu_mb=gpu_mb)
    log(f"wasm build: {web} ({renderer}, {len(srcs)} files compiled, {time.time() - t0:.1f}s, {wasm_mb:.1f} MB wasm)")
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


def build_native(out, log=print, debug=False, progress=nop_progress):
    out = Path(out)
    obj = out / "obj"
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
    t0 = time.time()
    cl = tool(env, "cl")
    run_jobs([(p.name, [cl, *flags, str(p)], env, 80 + 40 * p.stat().st_size / 1048576) for p in srcs], "native compile", progress)
    progress("native link", 0, 0)
    exe = out / "native.exe"
    lrsp = out / "link.rsp"
    lrsp.write_text("\n".join(f'"{p}"' for p in obj.glob("*.obj")), encoding="utf-8")
    r = resources.run([tool(env, "link"), "/nologo", f"/OUT:{exe}", "/STACK:268435456", f"/MAP:{out / 'native.map'}", f"@{lrsp}"],
                       env=env, capture_output=True, text=True)
    progress("native link", 1, 1)
    if r.returncode:
        raise RuntimeError("link failed:\n" + r.stdout[-2000:])
    log(f"native build: {exe} ({len(stale)} generated files rebuilt, {time.time() - t0:.1f}s)")
    return exe
