# Debugging a crashing or hanging build

These tools find *where* a recompiled game goes wrong when the log only says it trapped or stopped. Most of them are
page switches (add them to the address, e.g. `index.html?tracering=1`); two need a debug build.

For rendering problems (black screen, wrong colours) see [RENDERING.md](RENDERING.md) first.
When the game runs but computes something wrong (a save that won't load, a failed checksum), see [LIFTER.md](LIFTER.md).

## Keeping the log: `uewasm-cli serve --log`

The tools below print to the game's log. To get it in a file while the game runs, without the debug panel's
*Save log*, serve the build with `--log`:

```
uewasm-cli serve builds\MyGame\web --log builds\MyGame\run.log
```

The server adds a small script to `index.html` that sends every console line, page error and unhandled promise
rejection back to it every half second, and appends them to the file. The file is never cleared: each server start
adds a `=== uewasm serve …` line, each page load a `[uewlog] page …` line with the address and the browser. Lines
start with the seconds since the page loaded, and warnings and errors are marked:

```
=== uewasm serve C:\…\builds\MyGame\web on port 8080, 2026-11-18 14:02:11
0.0 [uewlog] page http://localhost:8080/?memdebug=1 Mozilla/5.0 …
12.4 [warn] …
```

It holds the same lines *Save log* would, and it can be read (or followed with `Get-Content -Wait`) while the game
runs. If the tab crashes, the last half second can be missing.

## Readable crash stacks

Build with function names (`--names`), so WebAssembly stack traces name the guest functions:

```
uewasm-cli recomp "C:\Games\MyGame\Binaries\Win32\MyGame.exe" -o builds/MyGame -y --names
```

Every recompiled frame is then `F_<guest address>`. When the runtime traps (an unknown call target, an invalid
instruction, a call to address 0) it prints:

- `[trap] call chain: … F_636d75 F_a14e20 …`: the guest functions that led there (tail calls leave no frame);
- `[trap] … (rsp=… rax=… rcx=…)` and `[trap] rdx=… rbx=… rbp=… rsi=… rdi=…`: the guest registers.

A crash in a worker with `null function` and no `[trap]` line used to mean a call to address 0 through a call cache;
those now reach the trap with the details above.

## What the game called last: `?tracering=1`

Keeps the last 64 system calls (Windows API and COM methods) quietly and prints them when the game traps:

```
[trap] last 64 system calls:
  CreateThread(0, 0, 9c3f00, 0) ret=9ca1cd rbp=… t=4100
  DirectInput8Create(400000, 800, 11f4fbc, 145bd80) ret=9ca1ee rbp=… t=4100
```

`ret` is the guest return address (the call site), `rbp` the caller's frame pointer, `t` the thread. For 32-bit games
the four values are the first four stack arguments. It is cheap enough to leave on for a whole run.

## Who changed this memory: `?watch=<hex>` and `?watchval=<hex>`

Checks one guest dword after every system call and logs each change with the call it happened in (or, for a change
made by the game's own code, the next call after it):

```
index.html?watch=140A910
[watch] 140a910: 0 -> 12e3000 after memset (thread 4100)
```

`&watchval=2db82d39` logs only changes *to* that value, for a slot (a stack location, say) that changes all the time.

## The stack going wrong

Always on for 32-bit games: the first system call made with a misaligned stack pointer is logged, with the last 64
calls when `?tracering=1` is on:

```
[stack] esp 2db8233d not aligned at memset, called from 49d5f4 (thread 4100)
```

Also always on: the first system call made with `esp` outside the thread's own stack (the TEB's limits), which is
what a stack pointer loaded from something that was never a stack address looks like:

```
[stack] esp d1e054 outside the stack of thread 4100 (beff0010-bfff0010) at SystemParametersInfoW, called from 9c9f4d, ebp 2db82d39
```

The usual cause is a call that pops the wrong number of argument bytes (32-bit `stdcall`). Imports take theirs from
Windows' own DLLs (`rustlift/src/bin/uewrecomp/stdcall.rs`, cached in `stdcall32.json`; documented values for the ones that can't
be read there); COM methods the runtime implements need a per-method table (`com_nargs()`), and an interface that
lacks one says so the first time a 32-bit game creates it:

```
[com] IMMDeviceEnumerator: method 3 has no 32-bit argument count
```

Thread creation is logged too, with each thread's start address, stack and register-state object:

```
[thread] 4128 created: start 9c3f00, parameter 0, from 9ca1cd (stack bbb2ffd0, state 0xcba4c980; creator state 0x1193398)
```

## Debug builds

Set extra compiler flags for the runtime and lifted code with `UEW_EXTRA_CFLAGS` before building:

| Flag | What it does |
|---|---|
| `-DUEW_G32CHECK` | Logs, once per function, every runtime function that does a 64-bit load or store for a 32-bit game (`[g32] 64-bit guest access in …`). Pointers and `SIZE_T` are 4 bytes there, so most hits are layout bugs; 64-bit fields (`FILETIME`, `LARGE_INTEGER`) are expected. |
| `-DUEW_EBPCHECK` | Records every recompiled function entry per guest thread (name, `esp`, `ebp`). With `?ebpwatch=<hex>`, logs the first function entered with that `ebp`, its caller, registers and top of stack, and this thread's last 40 entries; also logs the first function entered with a misaligned `esp`. The debug panel's thread dump then also prints every thread's last entries, which shows where a busy thread loops. Slow; for finding stack or register corruption and spins. |
| `-DUEW_STORE_CHECK` | (existing) Stores to read-only image ranges or with stray high address bits. |

Example:

```
set UEW_EXTRA_CFLAGS=-DUEW_EBPCHECK
uewasm-cli recomp "…\MyGame.exe" -o builds/MyGame -y --names --threads real
```

## Hangs

- **Cooperative builds (the default) freezing with no output**: a guest thread is spinning on something another
  thread must change, without making a system call, so no other thread gets to run. A failed `lock cmpxchg` and `pause`
  already yield. Try `--threads real`: if the game gets further, the hang was such a spin.
- A spin also blocks the page, so page-side tools (the debug panel's thread dump) don't respond; use `?tracering=1`
  to see the last system calls before it. With real threads the thread dump still works, and with `?tracering=1` it
  prints those last calls too.

## Exceptions (32-bit games)

C++ `throw` and SEH `__try` work in 32-bit games: `RaiseException` walks the thread's handler chain (`fs:[0]`) as
Windows does, and a catch resumes in the catching function (the lifter gives every function with a try block a host
`setjmp`, and makes each catch continuation and `__except`/`__finally` block an entry point:
`rustlift/src/bin/uewrecomp/eh32.rs`).
Unreal Engine 3 relies on this: a failed package or class load is thrown and caught as a warning.

- `?sehdebug=1` logs every exception with the thrown C++ type and its message (UE3 throws `TCHAR*` strings), each
  handler called and its answer, and where the chain ended when nothing caught it:

```
[seh] C++ exception .PA_W (object bffed8fc)
[seh]   message: Failed to find object 'Class Engine.OnlineRecentPlayersList'
[seh]   frame bffed994 handler f1ed64 -> 1
```

- A catch that lands on an address the lifter didn't make an entry point stops with `catch continuation is not a
  lifted entry point`: the funclet's `mov eax, continuation; ret` wasn't recognised by `eh32.rs`.

## Online sign-in errors

`?httpdebug=1` logs the HTTPS a game makes through Windows' Schannel (the EOS SDK, for one): each request's method,
host and path (form fields by name), and each answer's status, with the body of failed answers. When a game reports a
sign-in error, that body says what the service actually objected to, for example
`[http] api.epicgames.dev: HTTP/1.1 401 Unauthorized: {"errorCode":"errors.com.epicgames.eos.auth.external.invalid_token","errorMessage":"Steam Session Ticket unexpected length: 32."}`.
Tokens, credentials and successful answers' bodies are left out, so the log can be shared. It offers servers only
HTTP/1.1, so the traffic stays readable.

`?clearsave=<path>` deletes a file or folder inside the save folder (the browser's copy of the game's writable files)
before the game starts, for example a download cache an online SDK keeps trusting:
`?clearsave=Users/Player/AppData/Local/<Game>/Saved/PersistentDownloadDir/EOSCache`. `?filetrace=1` shows the paths
the game uses.

## Steam

`?steamdebug=1` logs, once each, every offline-Steam method a game calls (interface, vtable slot, what it returned and
the caller), e.g. `[steam] ISteamUtils slot 9 -> 3212c (from 9ac315)`. The methods aren't modelled per SDK version:
each returns 0 (a few with slots that never changed answer properly: the user's SteamID and name, the app id, the
language), and on 32-bit games pops the bytes its call site pushed, which the lifter records for every call through a
register or vtable (`rt_call_args`). `[call] no argument size known for the call returning to …` means such a call
site wasn't recorded (its arguments were pushed in an earlier block), so the stack is off from there on.

## Running out of memory

The first failed guest allocation logs the guest stack, the WebAssembly call chain and (with `?tracering=1`) the last
system calls. An absurd request (gigabytes) usually comes from a bad count further up, or from a loop that never ends
allocating small blocks.

Every 600 frames, WebGPU builds log how much guest memory is in use, the most that was ever in use, and the size of
the WebAssembly memory:

```
[mem] game memory 4030 MB in use, 6568 MB at most, of 7168 MB; wasm memory 7504 MB
[mem] runtime heap 336 MB (game memory excluded); 16766 shaders, 13998 distinct (45 MB bytecode, 40 MB distinct; 256 drawn so far, 2 MB reflection), buffer shadows 28 MB, 3815 buffers and textures (11 MB of records)
[mem] game heap (malloc/HeapAlloc): 149 MB asked for, 161 MB held (147 MB in blocks of 64 KB and up), 0 MB free in size-class lists
```

The second line is the runtime's own share of the WebAssembly memory. Shaders with identical bytecode share one copy
(and one set of translations and pipelines), and a shader is reflected (about 12 KB) only when it is first drawn, so
games that create thousands of shaders at load don't pay for duplicates or for the ones they never use. The heap figure
also counts each game thread's 4 MB native stack, most of which is never touched and so takes no RAM. The third line is
what the game allocates through the C runtime and `HeapAlloc`; memory it takes with `VirtualAlloc` (most engines'
own allocators) is only in the first line.

Freed blocks are zeroed when they are freed rather than when they are handed out again, so memory a game frees and
never reuses (a loader's buffers) holds zeros, which Windows compresses to almost nothing if it trims the tab under
memory pressure. `?nozerofree=1` goes back to zeroing at reuse.

`[memline]` lines trace guest memory while it climbs (a line per 256 MB step) and when it falls by two steps or more,
which shows the shape of a loading peak: what is allocated at once, what builds up, and what is freed afterwards.

The page's download cache can be resized for a test with `?cachemb=N`; `Module.uewFiles.refetched` in the console
counts bytes downloaded again after the cache evicted them (what a smaller cache costs). With `?cachetrace=1`,
`Module.uewFiles.trace` records every block the game reads (its key) and every block that arrives (`-key-1`), in order,
so cache sizes and policies can be replayed offline against a real session instead of tried one run at a time.

WebAssembly memory can't hand pages back, so the "at most" figure (plus about 700 MB of runtime) is what the page
keeps in RAM for the rest of the session, even after the game frees it. `--mem-mb` has to stay above it with some
headroom; setting it higher only reserves address space, it doesn't use RAM.

`?memdebug=1` shows where the memory goes. It logs the first 200 `VirtualAlloc` calls of 64 MB or more (and failed
ones) with the four arguments, the result, the caller and the thread; for 64-bit games each one also gets a deeper
guest stack, up to 48 code addresses, nearest first. Every `VirtualFree` of a block of 64 MB or more is logged too,
with the whole block and whether it was released or only decommitted:

```
[mem] VirtualAlloc(0, f8000000, 3000, 4) = 3c0000000 from 1418a2c3e thread 4100
[mem] stack: 1418a2c3e 14187f512 14187f6a0 1410e5a92 140f3b2c8 140f3a011 …
[mem] VirtualFree(3c0000000, 0, 8000): block 3c0000000+f8000000 released from 1418a2d10
```

The first addresses on a `[mem] stack:` line are the engine's own allocator, the same for every large block; the
game code that asked for the block is further down, usually from the first address that differs between two such
lines. Some entries are stale values left on the stack, so one address that doesn't fit with its neighbours can be
skipped. The others are return addresses inside a function: the nearest `F_<address>` below one in the recompiled C
is that function (see [Reading the game's code](#reading-the-games-code)). A block allocated again and again and
never released points at a leak.

In a `--mem64` build, small blocks (under 4 MB) are placed above 4 GB on purpose, so code that keeps a guest address
in 32 bits breaks on the first allocation instead of deep into a game.

## Reading the game's code

The recompiled C is in `builds/<game>/gen/funcs_*.c` (`grep -l "^void F_<address>(CPU" builds/<game>/gen/funcs_*.c`).
To read the original instructions at a guest address, `uewasm-cli disasm` decodes them from the build's `image.bin`
(every module already relocated to the addresses the build uses; 32- or 64-bit from `gen/meta.c`):

```
uewasm-cli disasm builds\MyGame 636d20 40
```

Any other x86 disassembler works too: an address `A` is at file offset `A - IMAGE_LO` of `image.bin` (`IMAGE_LO` is
in `gen/meta.c`, usually `0x400000`).
