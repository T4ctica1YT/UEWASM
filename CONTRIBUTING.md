# Working on UEWASM

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the parts fit together. This page covers setup, the
project's rules, recipes for the common kinds of change, and which tests to run.

## Setup

- Windows 10/11 x64, Rust, the Emscripten SDK (`uewasm-cli install-emsdk`), Visual Studio Build Tools with C++ (the
  tests build native programs to compare against), and Edge or Chrome for the browser tests.
- Build the tools after changing anything in `rustlift/`:

  ```
  cargo build --release --offline --manifest-path rustlift/Cargo.toml
  cargo build --release --offline --target x86_64-pc-windows-msvc --manifest-path app/Cargo.toml   # the desktop app
  ```

  They land in `rustlift/target/release/` (`uewasm-cli.exe`, `uewtest.exe`, ...). `uewasm-cli` also rebuilds the
  other tools itself when their sources are newer.
- `uewasm-cli doctor` lists what's missing.
- Runtime changes (`runtime/`) need only a relink, not a new lift: running `uewasm-cli recomp` again with the same
  output folder recompiles changed runtime files and binds newly added `HLE()` functions (`refresh`). Lifter changes
  (`rustlift/src/`) need a full recompile.

## Rules

- **Tools are Rust.** The recompiler, the build and pack steps, the server, the command line, the tests and the
  release packager are all in `rustlift/`, and the desktop app is Rust too. New tools go there as well, not in Python.
  Page-side code that runs in the browser stays JavaScript.
- **Build features from the platform, not from one game.** Implement Windows, Direct3D, Steam or EOS behaviour from
  their documentation and SDK headers, generally, and use games only to check it. A fix that names a game is a
  workaround, and usually a sign the general behaviour is still wrong.
- **Compare with native.** The tests run the original program and the recompiled one and compare output. When
  something behaves differently, write a small program that shows it and add it to a test.
- **No game files, no accounts.** Never commit game files, builds of commercial games, logs from them, or anything tied
  to a real account (tokens, SteamIDs, names). Test games stay on the developer's machine.
- **Style.** LF line endings. Comments say why the code does something unusual (a Windows quirk, what a game relies
  on), not what it does. Keep them short.
- **Commits.** A short title (under ~70 characters) and a description that lists the changes by area.

## Recipes

### A Windows API function a game needs

The log names missing imports (`[load] GetProcAddress(x, Name) not implemented`), and `coverage.json` lists them after
a lift.

1. Add it to the `hle_*.c` file for its area:

   ```c
   /* (hFile, lpBuffer, nBytes, lpRead, lpOverlapped) */
   HLE(ReadFile) { ...; RET(1); }
   ```

   Arguments: `A1`–`A4`, then `ARG(5)` onward. Strings: `GSTR(p)`, or `wstr_to_utf8` for wide strings. Memory:
   `LD32`/`ST32`/`G2H`. Set the error code with `hle_set_error()` when the function fails.
2. `STUB(Name, value)` is fine for something that really has nothing to do. If a game's behaviour depends on the
   function's result, implement it.
3. Write it to match what Windows does, including the error cases. Add a line to a test program in `tests/`
   (`files.c`, `basic.c`, ...) whose output `uewtest run` compares with the real Windows result.

### A browser-side host function

Declare it in C (`int net_recv1(int id, uint8_t* p, int cap);`) and add it to the right `runtime/web/*.js` library:

```js
net_recv1__sig: 'iipi',
net_recv1__proxy: 'sync',      // needed for --threads real
net_recv1: function (id, p, cap) { ... },
```

`rustlift/src/bin/uewbuild.rs` lists what gets linked: libraries (`--js-library`, functions C calls) and page scripts
(`--pre-js`, code that runs before the module, such as `uew_wisp.js`). A new file goes in one of those lists.

### A Steam or EOS call

Steam methods live in `runtime/hle_steam.c`, by interface family and method name. Every SDK version's layout comes
from `runtime/steam_ifaces.h`, so a method needs writing once. EOS functions live in `runtime/hle_eos.c`
(`runtime/eos_api.h` has the full list). See [docs/ONLINE.md](docs/ONLINE.md).

### An instruction the lifter doesn't support

`coverage.json` counts unsupported instructions by mnemonic. Add the instruction to `rustlift/src/func.rs` (or `sse.rs`,
`x87.rs`), add its forms to `rustlift/src/bin/uewtest/isa.rs`, and check `uewtest isa --wasm`. Instructions behind
CPU features the runtime doesn't report (AVX, AES-NI) never run, because games check CPUID first.

### A debug switch

Read it with `host_query_flag("name")` or `host_query_int("name")` (a `?name=1` page parameter). Keep anything it logs
free of credentials, and list it in [docs/DEBUGGING.md](docs/DEBUGGING.md).

## Tests

All of them compare against native Windows. Run the ones that match the change:

| Change | Tests |
|---|---|
| lifter (`rustlift/src/`) | `uewtest isa --wasm`, `uewtest run` (also `--threads real`), `uewtest aes` |
| runtime, kernel32, files, threads | `uewtest run`, `uewtest web` |
| Direct3D, shaders, WebGPU | `uewtest gfx webgpu` (also `srgb`, `d3d9`, `volume`) |
| networking, TLS, crypto | `uewtest net`, `uewtest aes` |
| Steam | `uewtest steam-pair <web build>` |
| the SteamStub decryptor | `cargo test --release --manifest-path rustlift/Cargo.toml --bin uewrecomp` |

Then check a real game or two still boots and renders as before. For performance changes, measure frame rate before
and after ([docs/PERFORMANCE.md](docs/PERFORMANCE.md) shows how to simulate a slow laptop). Results vary from run to
run, so compare more than one run.

## Where to look when something breaks

| Problem | Doc |
|---|---|
| a crash or hang | [docs/DEBUGGING.md](docs/DEBUGGING.md) |
| wrong output with no crash (bad data, failed checksums) | [docs/LIFTER.md](docs/LIFTER.md) |
| missing or wrong graphics | [docs/RENDERING.md](docs/RENDERING.md) |
| slow frames | [docs/PERFORMANCE.md](docs/PERFORMANCE.md) |
| sign-in, servers, Steam, EOS | [docs/ONLINE.md](docs/ONLINE.md) |
