# Getting a smooth frame rate

A recompiled game runs the original x86 code as WebAssembly and replays every Direct3D 11 call on WebGPU, so
both the CPU side and the GPU side cost more than they do natively. This page lists what actually makes a
difference, roughly in order of impact. For visual problems, see [RENDERING.md](RENDERING.md).

## 1. Find out what's slow

Turn on the FPS counter (`--fps` at build time, or **Ctrl+Shift+F** in game). It shows the frame rate, the
frame time and the worst frame in each half second. Then open the debug panel (`\` or **Insert**) and check
**Recent hitches**:

| Log line | What's slow | Go to |
|---|---|---|
| `[hitch] … N new shaders, N new pipelines` | First-time shader compiling | [Shader stutter](#3-shader-stutter) |
| `[hitch] … MB in N downloads (… ms waiting)` | Game files still downloading | [Downloads](#5-downloads-and-loading) |
| `[hitch] GPU took … ms for a frame` | The GPU | [GPU-bound](#4-gpu-bound) |
| Low FPS with no hitch lines | The CPU (game code and draw calls) | [CPU-bound](#2-cpu-bound) |

`?gpuprof=1` in the address logs the slowest GPU passes of slow frames (`[gpuprof]` lines), along with the shader
behind the worst one.

## 2. CPU-bound

This is the usual case. Every game thread runs on one browser thread by default, and every draw call is
translated on the CPU.

- **`--quality low`** (or `medium`). Fewer effects means fewer draw calls, and draw calls are the most
  expensive thing here. Dead as Disco gains about 10% at `low`.
- **`--threads real`.** Game threads run on several cores instead of taking turns on one. It helps games that
  do a lot of work outside the render thread (physics, animation, streaming). It needs WebGPU and `https://` or
  `localhost`, and the build includes a service worker for the cross-origin isolation it needs.
- **`--opt -O3`.** Slightly faster code, a longer build and a larger download.
- **Keep DevTools closed while playing.** An open debugger makes WebAssembly noticeably slower.
- **Remove the debug panel from release builds** (`--no-debug-panel`). It also removes the debug hooks.

## 3. Shader stutter

Each shader is translated and compiled the first time it's drawn. That's why the first visit to a new area
stutters and the second one doesn't: the browser keeps compiled shaders in its cache.

- **Leave background shader compiling on** (the default). The game keeps running and the new object appears a
  moment later. `--sync-shaders` is exact but freezes instead.
- **`?skipshaders=1`** removes the stutter completely by dropping draws whose shader isn't ready yet. The cost
  is that an effect drawn only once, such as a render-to-texture when a menu opens, may stay missing.
- **Warm the cache.** Walking through the main areas once before playing seriously makes later runs smooth on
  that browser.

## 4. GPU-bound

- **`--resolution`**, for example `1280x720` or `960x540`. The page scales the picture to the window, so a lower
  render size costs little visually and helps the most on slow GPUs.
- **`r.ScreenPercentage 70`** lowers Unreal's internal resolution and keeps the UI sharp. Pass it with
  `--game-arg=-ExecCmds="r.ScreenPercentage 70"`.
- **Turn off the expensive effects** with `--quality low`, or one by one:
  `r.SSR.Quality 0`, `r.AmbientOcclusionLevels 0`, `r.VolumetricFog 0`, `r.MotionBlurQuality 0`. On UE5, also
  `r.Lumen.DiffuseIndirect.Allow 0` and `r.Lumen.Reflections.Allow 0`.
- **Use the dedicated GPU on laptops.** In **Windows Settings → System → Display → Graphics**, set the browser to
  **High performance**. The log warns when it looks like the integrated GPU was picked.
- **Check that the browser uses the GPU.** `chrome://gpu` should say WebGPU is **Hardware accelerated**. A
  `software fallback!` line in the log means everything is drawn on the CPU.

## 5. Downloads and loading

- The page downloads game files as they're read and remembers what was read during startup, so the second
  launch prefetches it. Hosting with **HTTP Range** support (most static hosts, including Cloudflare Pages)
  avoids re-downloading whole parts.
- **`--compress`** makes the download smaller. Audio is left alone because it's already compressed.
- **`--video-height 720`** re-encodes large intro videos so they decode faster.
- **`--wasm-opt`** makes the module about 10% smaller, so it starts sooner.

## 6. Memory

If the tab crashes or the browser reports it's out of memory:

- `--mem-mb` is the game's own memory (the browser's limit is 4096 in total).
- `--mem-limit-gb` caps the whole game, including the file cache and video memory. Lower it on PCs with 8 GB
  of RAM or less.

## 7. Building faster

These don't change the result, only how long the build takes:

- **`--lifter rust`** lifts machine code several times faster. It needs a Rust toolchain and is built
  automatically the first time.
- **`--priority low`** keeps the PC usable during a build. It's slower but produces the same output.
- Rebuilding into the same output folder only recompiles lifted files that changed.

## A good starting point

For a mid-range PC and a UE4 game:

```
python uewasm.py recomp "C:\Games\MyGame" --lifter rust --threads real --quality medium --resolution 1280x720 --fps
```

If it's still slow, drop to `--quality low` and `--resolution 960x540`. If it stutters only in new areas, that's
first-time shader compiling and it goes away on the next visit.
