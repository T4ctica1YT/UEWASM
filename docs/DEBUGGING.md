# Debugging a crashing or hanging build

These tools find *where* a recompiled game goes wrong when the log only says it trapped or stopped. Most of them are
page switches (add them to the address, e.g. `index.html?tracering=1`); two need a debug build.

For rendering problems (black screen, wrong colours) see [RENDERING.md](RENDERING.md) first.

## Readable crash stacks

Build with function names, so WebAssembly stack traces name the guest functions:

```python
# builds/build_named.py: the normal command line, with names=True passed to the web build
import sys
sys.path.insert(0, r"C:\path\to\UEWASM")
from uewasm.recomp import build
orig = build.build_wasm
build.build_wasm = lambda *a, **k: orig(*a, names=True, **k)
from uewasm.cli import main
if __name__ == "__main__":
    main(sys.argv[1:])
```

```
python builds/build_named.py recomp "C:\Games\MyGame\Binaries\Win32\MyGame.exe" -o builds/MyGame -y
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
Windows' own DLLs (`uewasm/recomp/stdcall.py`, cached in `stdcall32.json`; documented values for the ones that can't
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
python builds/build_named.py recomp "…\MyGame.exe" -o builds/MyGame -y --threads real
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
`setjmp`, and makes each catch continuation and `__except`/`__finally` block an entry point: `uewasm/recomp/eh32.py`).
Unreal Engine 3 relies on this: a failed package or class load is thrown and caught as a warning.

- `?sehdebug=1` logs every exception with the thrown C++ type and its message (UE3 throws `TCHAR*` strings), each
  handler called and its answer, and where the chain ended when nothing caught it:

```
[seh] C++ exception .PA_W (object bffed8fc)
[seh]   message: Failed to find object 'Class Engine.OnlineRecentPlayersList'
[seh]   frame bffed994 handler f1ed64 -> 1
```

- A catch that lands on an address the lifter didn't make an entry point stops with `catch continuation is not a
  lifted entry point`: the funclet's `mov eax, continuation; ret` wasn't recognised by `eh32.py`.

## Steam

`?steamdebug=1` logs, once each, every offline-Steam method a game calls (interface, vtable slot, what it returned and
the caller), e.g. `[steam] ISteamUtils slot 9 -> 3212c (from 9ac315)`. The methods aren't modelled per SDK version:
each returns 0 (a few with slots that never changed answer properly: the user's SteamID and name, the app id, the
language), and on 32-bit games pops the bytes its call site pushed, which the lifter records for every call through a
register or vtable (`rt_call_args`). `[call] no argument size known for the call returning to …` means such a call
site wasn't recorded (its arguments were pushed in an earlier block), so the stack is off from there on.

## Running out of memory

The first failed guest allocation logs the guest stack, the WebAssembly call chain and (with `?tracering=1`) the last
system calls; `?memdebug=1` also logs every reservation of 64 MB or more with its caller. An absurd request (gigabytes)
usually comes from a bad count further up, or from a loop that never ends allocating small blocks.

## Reading the game's code

The recompiled C is in `builds/<game>/gen/funcs_*.c` (`grep -l "^void F_<address>(CPU" builds/<game>/gen/funcs_*.c`).
To read the original instructions, any x86 disassembler works on the exe; the recompiler's own loader gives the
image at the address the build uses:

```python
from uewasm.recomp import pe
import iced_x86 as I
p = pe.load(r"C:\Games\MyGame\Binaries\Win32\MyGame.exe")
img = p.image(0x400000)
for ins in I.Decoder(32, img[0x236d20:0x236d60], ip=0x636d20):
    print(hex(ins.ip), I.Formatter(I.FormatterSyntax.INTEL).format(ins))
```
