# Finding recompiler bugs

When a recompiled game computes something different from the original, with no crash at the point of the mistake, the
cause is usually the lifter or a runtime function. This page covers how to find such a bug and the assumptions that
cause most of them.

## Symptoms

- Data that's fine natively is rejected: a checksum or signature fails, a decrypt reports a bad tag, a save loads as
  "corrupt".
- A huge or negative allocation size, a loop that runs forever, a wrong branch taken: a value read from data that was
  computed wrongly earlier.
- It happens the same way every run, in every browser, and also in the native build of the lifted code.

That last point matters most. If the native build (`build_native`) shows it too, it's not the browser or WebGPU, and
you can debug it with ordinary tools.

## Method

### 1. Reproduce outside the game

Find a small program that runs the same code. Library code is often available elsewhere: a Python install ships
OpenSSL (`libcrypto-3.dll`), zlib and SQLite DLLs, and a test exe can `LoadLibrary` one and call its exports.
`uewtest aes` (`aes()` in `rustlift/src/bin/uewtest/suites.rs`, with `tests/aes/aes.c`) is the pattern:

1. compile a test exe that prints hashes of its results,
2. run it natively,
3. recompile it with the DLL as an extra module (`build::recompile(exe, out, .., &[dll], ..)`), then
   `build::build_native(out)`,
4. run the recompiled program and compare line by line.

Libraries pick code paths from CPUID, so force the path the runtime takes. The runtime reports SSE up to SSE4.2 and
POPCNT, but no AVX, AES-NI or PCLMULQDQ (`rt_cpuid` in `runtime/rt.c`). For OpenSSL that means
`OPENSSL_ia32cap=~0x200000200000000:0`.

### 2. Narrow it to one function

Call smaller pieces until one differs. Compare intermediate values rather than final outputs: a key schedule that
matches while the cipher output doesn't shows the error is in the cipher.

### 3. Read the lifted C

Find the function in `gen/` by its address. Exports are at `module base + export RVA`, and the module base is in the
linker log:

```
grep -l "^void F_5a2ef0(CPU\* __restrict c) {" out/gen/*.c
```

Each instruction becomes a few lines of C, so it can be compared with the original assembly, or with the source when
it's open (OpenSSL's perlasm, for example).

### 4. Instrument it

Add a `printf` at a label inside the lifted function (a loop head, say), rebuild with `build_native` (it recompiles
only changed files), and compare against a reference implementation (a few lines of C built natively, or any
language you like for a throwaway check). The first value that differs points at the instructions in between. Don't
re-run `recompile` afterwards, or it regenerates the file and removes your prints.

### 5. Fix it and add a test

Fix the lifter (`rustlift/src/func.rs` for instructions and calls, `sse.rs`, `x87.rs`) or the runtime. Then add the
case to a test that compares against native: a form in `rustlift/src/bin/uewtest/isa.rs` for one instruction, or a
program like `tests/aes/` with its suite in `uewtest` for a pattern.

## Assumptions the lifter makes

Most wrong-output bugs come from code that breaks one of these. Compilers never break them; hand-written assembly
sometimes does.

- **Callee-saved registers.** On x64, a function must preserve `rbx`, `rbp`, `rsi`, `rdi`, `r12`–`r15` and
  `xmm6`–`xmm15`. The lifter keeps them in C locals across calls (`FILLV()` reloads only the volatile ones). Hand-written
  routines sometimes return results in them: OpenSSL's AES cores return the state in `ebx`, and vpaes leaves its
  constants in `xmm9`–`xmm15`. The x64 unwind rules make this detectable: a function that changes a callee-saved
  register must have a `.pdata` entry. So after a direct call to a target without one, the lifter reloads everything
  (`FILL()`). Indirect calls still assume the ABI. 32-bit code always reloads everything, since its conventions vary.
- **Flags across calls.** Flags are lazy and normally die at a call. For 32-bit code that passes flags into or out of
  a callee, the lifter checks the target (`flags_read_first`, `flags_live_after_call`).
- **Function boundaries.** Functions come from `.pdata`, exports, call targets and the jump tables the lifter can
  resolve. Nothing is lifted at run time: a call or jump to an address with no lifted function goes down
  `rt_call`'s slow path (`jump_slow` in `runtime/rt.c`) and shows up in the log.
- **Self-modifying code.** Not supported. Packed code has to be unpacked at link time (SteamStub is), and protected
  executables aren't handled.

## Useful switches

| Switch | Use |
|---|---|
| `?tracering=1` | the last calls into the runtime, dumped when something fails |
| `?watch=<hex>`, `?watchval=<hex>` | who writes a guest address |
| `?filetrace=1` | every file opened or checked |
| `?envdebug=1` | every environment variable read or set |
| `?inputdebug=1` | windows, raw input registrations, and where each key goes |
| `?httpdebug=1`, `?tlsdebug=1`, `?netdebug=1` | HTTPS answers, TLS handshakes, socket calls |
| `?steamdebug=1`, `?eosdebug=1` | Steam and EOS calls |

The full list is in [DEBUGGING.md](DEBUGGING.md).
