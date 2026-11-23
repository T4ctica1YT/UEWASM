# How UEWASM works

This page is for people changing UEWASM itself. It follows a game from its `.exe` to a running web page and says where
each part lives. For using the tool, see the [README](../README.md).

## The pipeline

```
game .exe + DLLs ──uewrecomp──▶ image.bin + gen/*.c ──uewbuild (emcc)──▶ game.wasm + game.js ──uewpack──▶ web/
                      │                 ▲                                      ▲
                      └──uewlift────────┘                         runtime/*.c, runtime/web/*.js
```

1. **Link** (`rustlift/src/bin/uewrecomp/`). The PE loader (`pe.rs`) maps the exe and every DLL it needs into one
   guest address space, applies relocations, removes SteamStub (`steamstub.rs`), and binds each import either to a
   recompiled DLL or to a runtime function (`link.rs`). The result is `image.bin`, the guest memory image.
2. **Lift** (`rustlift/src/`, binary `uewlift`). Every function found in the code (exports, `.pdata`, call targets,
   jump tables) becomes C: one `void F_<address>(CPU* c)` per function, spread over `gen/funcs_*.c`.
   `gen/meta.c` holds the address → function table and the import thunks. `coverage.json` counts instructions the
   lifter can't translate yet.
3. **Compile** (`rustlift/src/bin/uewbuild.rs`). Emscripten compiles `gen/`, the runtime (`runtime/*.c`) and
   BearSSL (`runtime/third_party/bearssl`) into one wasm module, links the JavaScript libraries from `runtime/web/`,
   and writes the page from `rustlift/src/bin/web/shell.html`.
4. **Pack** (`rustlift/src/bin/uewpack.rs`). The game's own files are split into download parts the page fetches on
   demand.

`uewasm-cli` (`rustlift/src/bin/uewasm-cli/`, `build.rs`) drives these steps; the desktop app (`app/`) is a front end
that runs it with options. The same lifted C also builds natively (`build_native`), which is how the tests compare
the recompiled program with the original exe without a browser.

## The lifted code

Each guest function becomes a C function over a `CPU` struct (`runtime/rt.h`): sixteen 64-bit registers `r[]`,
sixteen XMM registers `x[]`, MXCSR, and the x87 stack for 32-bit games. Inside a function, the registers it uses are
kept in C locals (`r3_`, `x9_`) and written back to `c->r[]` around calls (`SPILL()`/`FILL()` in
`rustlift/src/main.rs`). Guest memory is one flat array: `G2H(addr)` turns a guest address into a host pointer, and
`LD32`/`ST32` and friends read and write it.

Flags are computed lazily (`Flags F` holds the last operation and its operands), so most instructions never
materialise EFLAGS. SSE goes through `runtime/sse.h`, using wasm SIMD where it helps; x87 goes through
`runtime/x87.h`.

Direct calls become C calls (`F_401000(c)`). Indirect calls go through `rt_call`, which looks the target up in the
function table, with inline caches at each call site. A target with no lifted function is a runtime import, or a gap
the log reports.

What the lifter assumes about calling conventions is in [LIFTER.md](LIFTER.md). Those assumptions are where subtle
wrong-output bugs come from.

## The runtime

`runtime/` stands in for Windows. Each Windows API function a game imports is a C function written with the `HLE()`
macro (`runtime/host.h`), for example `HLE(CreateFileW)`. The recompiler finds every `HLE(name)` and `STUB(name, v)`
in `runtime/*.c` by scanning the source, so adding one is enough to bind it. Arguments arrive as `A1`–`A4`
(`rcx`, `rdx`, `r8`, `r9`) and `ARG(n)` for the rest, and the result goes back with `RET(v)`.

| Files | What they cover |
|---|---|
| `rt.c`, `rt.h` | function table, indirect calls, CPUID, exceptions, guest memory |
| `sched.c` / `sched_mt.c` | guest threads: green threads on one browser thread, or real threads on several cores (`--threads real`) |
| `host.c`, `host_web.c`, `host_fs.c` | the boundary to the browser or the native host: time, files, idle, frames |
| `hle_kernel32.c`, `hle_file.c`, `hle_sync.c`, `hle_process.c`, `hle_misc.c`, `hle_more.c`, `hle_extra.c`, `hle_compat.c` | kernel32 and friends: memory, files, mappings, synchronisation, processes |
| `hle_user32.c`, `hle_win32.c` | windows, messages, input |
| `hle_crt.c` | C runtime functions the runtime serves directly |
| `d3d11.c`, `d3d9.c`, `gpu_wgpu.c`, `dxbc*.c`, `d3d9_wgsl.c` | Direct3D 11/9 on WebGPU, with DXBC and D3D9 shaders translated to WGSL |
| `hle_xaudio.c`, `hle_dsound.c`, `hle_vorbis.c`, `hle_vpx.c` | audio and video |
| `hle_net.c`, `hle_inet.c`, `hle_schannel.c`, `hle_certstore.c`, `hle_crypto.c` | Winsock, WinINet, TLS, certificates, CryptoAPI and CNG |
| `hle_steam.c`, `hle_eos.c`, `hle_platform.c` | Steam, Epic Online Services, platform glue (see [ONLINE.md](ONLINE.md)) |

`runtime/web/` holds the browser side: `uew_host.js` (the host functions C calls), `uew_webgpu.js` (WebGPU),
`uew_files.js` (game file downloads), `uew_save.js` (the save folder in IndexedDB), and the online clients
(`uew_wisp.js`, `uew_webworks.js`, `uew_steamcm.js`). They're Emscripten libraries (`addToLibrary`), and each function
declares its signature (`__sig`) and, for real threads, `__proxy: 'sync'`.

### Threads and frames

By default every guest thread is a green thread switched on the browser's main thread (`sched.c`). A thread that
blocks (`sched_block` with a check function, `sched_sleep`) yields to the others. When none can run, the scheduler
gives the browser a turn (`host_idle`), which is when downloads, network data and input arrive. A game's `Present`
waits for the browser's next animation frame (`sched_wait_frame`).

That makes busy waiting expensive in a way it isn't natively: a thread polling in a tight loop keeps the page from
painting. Blocking calls should block on a check (`sched_block`) or sleep for milliseconds, not spin.

### Files

The game's own files are read-only and downloaded on demand (`/game` in the guest file system). Everything a game
writes goes to the save folder (`/save/<exe name>/`), which persists in the browser's IndexedDB.

## The tools

Every tool is Rust (`rustlift/`, one crate):

| Binary | Job |
|---|---|
| `uewasm-cli` | the command line: `recomp` runs the steps below in order, plus `serve`, `relay`, `test`, `doctor`, `install`, `coverage`, `disasm` |
| `uewrecomp` | link, lift, `refresh` (bind runtime functions added since the lift), `game-dlls` |
| `uewlift` | the lifter, run by `uewrecomp` |
| `uewbuild` | compile and link the web build, write the page |
| `uewpack` | split and compress game files |
| `uewserve` | local server: headers for threads, byte ranges, the online relay and the WISP server |
| `gen_steam_ifaces`, `gen_eos_api`, `gen_ca_roots` | generate `runtime/steam_ifaces.h`, `runtime/eos_api.h`, `runtime/ca_roots.h` from the SDK headers and Mozilla's roots |
| `uewtest` | the test suites (`run`, `isa`, `aes`, `net`, `web`, `gfx`, `steam-pair`, `sse`) and shader tools |
| `uewdist` | the release: builds everything and lays it out in `dist/` |

`uewasm-cli`'s modules are shared: `uewtest` includes them with `#[path]` (the build steps, the DevTools client
`cdp.rs` that drives headless Edge/Chrome, the `.pak` reader). The CLI finds the other tools beside itself, and in a
checkout rebuilds them with cargo when their sources are newer. It finds `runtime/` and `assets/` from its own
location (`UEWASM_HOME` overrides). The desktop app (`app/`) is Rust/egui and runs the CLI.
