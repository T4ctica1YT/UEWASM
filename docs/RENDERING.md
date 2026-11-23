# Fixing shader and rendering problems

This guide covers what to do when a recompiled game shows a black screen, looks too dark, flickers, has missing
objects or shows garbage on screen. Start with **Step 1** whatever the symptom: almost every rendering
problem leaves a line in the log that names its cause.

- [Step 1: get the log](#step-1-get-the-log)
- [Quick reference: symptom → cause → fix](#quick-reference)
- [Everything is black](#everything-is-black)
- [Too dark, too bright or washed out](#too-dark-too-bright-or-washed-out)
- [Glitches: flicker, missing objects, garbage, black blocks](#glitches)
- [Stutters while new effects appear](#stutters-while-new-effects-appear)
- [Page switches for rendering](#page-switches-for-rendering)
- [Unreal Engine settings that work around problems](#unreal-engine-settings-that-work-around-problems)
- [For developers: fixing the shader translator](#for-developers-fixing-the-shader-translator)
- [Reporting a rendering bug](#reporting-a-rendering-bug)

## How rendering works here

The game still thinks it talks to Direct3D 11 (or Direct3D 9: 32-bit Unreal Engine 3 games, see
[Direct3D 9 games](#direct3d-9-games-unreal-engine-3)). UEWASM's runtime takes each D3D11 call and replays it on
**WebGPU** (`runtime/gpu_wgpu.c`, `runtime/web/uew_webgpu.js`), or on **WebGL 2** (`runtime/gpu_web.c`) when
WebGPU isn't available. The game's shaders are DXBC bytecode, and the runtime translates each one to WGSL
(`runtime/dxbc_wgsl.c`) the first time it's used. The browser then compiles the WGSL into a pipeline in the background.

So a rendering problem has one of four causes:

1. **The browser or GPU**: no WebGPU, a software adapter, or a lost device.
2. **The translator**: a shader uses something `dxbc_wgsl.c` can't translate, so its draws are dropped.
3. **The D3D11 → WebGPU mapping**: a texture format, state or resource binding doesn't match what D3D would do.
4. **The game's own settings**: an effect (auto exposure, HDR output, tessellation) that depends on one of the above.

---

## Step 1: get the log

1. Build the game **without** `--no-debug-panel` (the default includes it).
2. Open the game, press **`\`** or **Insert** to open the debug panel, then click **Save log**. Or open the browser's
   DevTools (F12) and read the **Console** tab.
3. Search the log for the prefixes `[webgpu]`, `[gpu]`, `[gl]`, `[d3d11]` and `[hitch]`.

These are the lines that matter:

| Log line | Meaning |
|---|---|
| `[webgpu] GPU: … (software fallback!)` | The browser is rendering on the CPU. Expect black frames or about 1 FPS. See [Black from the start](#black-from-the-start). |
| `[webgpu] this may be the integrated GPU…` | A laptop's slow GPU was picked. Set the browser to *High performance* in Windows graphics settings. |
| `[gpu] WebGPU unavailable, using WebGL 2` | WebGPU didn't start. The WebGL 2 path has no compute shaders, so many effects are missing. |
| `[gl] WebGL2 context creation failed` | Neither API is available. The **null renderer** runs, so the game logic works but nothing is drawn. |
| `[webgpu] requestDevice failed: …` | The GPU refused the device. Usually a driver or browser problem. Update both. |
| `[webgpu] DEVICE LOST (…)` | The GPU was reset (driver crash or Windows' 2-second watchdog). The picture stays black until you reload. |
| `[webgpu] shader skipped: unsupported opcode N` | The translator doesn't know DXBC instruction `N` yet, so every draw that uses that shader is dropped. |
| `[webgpu] shader skipped: hull shaders are not supported by WebGPU` | A tessellated draw (landscape, displacement material) is dropped. |
| `[webgpu] shader 12:7 <message>` | The translated WGSL didn't compile in the browser. This is a translator bug. |
| `[webgpu] geometry stage skipped: …` | A geometry shader failed to translate and its draws are dropped. |
| `[webgpu] last 600 frames: N drawn, M dropped (pixel shader)` | How many draws were dropped and why (`unsupported stage`, `topology`, `vertex shader`, `pixel shader`, `pipeline`, `viewport`, `index buffer`). |
| `[webgpu] frame N (… skipped: A no pass, B no pipeline, C threw …)` | Draws skipped on the JavaScript side. A large `no pipeline` count right after loading is normal while shaders compile. |
| `[webgpu] unsupported texture format N` | A DXGI format with no WebGPU equivalent. Anything using that texture is missing. |
| `[webgpu] no conversion from format N to …` | The texture was created, but its pixel data can't be converted, so it shows as black. |
| `[webgpu] draw skipped: bind group K … is missing` | A resource was freed while still in use. |
| `[webgpu] <validation message>` | WebGPU rejected a call. The first 40 are printed, and every 600 frames the most common kinds are counted. |

The debug panel also shows live counters: **shaders compiling** (pipelines still being built) and **draws
skipped** (draws dropped because their pipeline wasn't ready).

---

## Quick reference

| What you see | Most likely cause | Try first |
|---|---|---|
| Black from the first frame, no menu | No WebGPU, software adapter, null renderer | [Black from the start](#black-from-the-start) |
| Menu/UI draws but the 3D scene is black | Auto exposure or tonemapper pass dropped, NaNs | [UI works, scene is black](#ui-works-but-the-3d-scene-is-black) |
| Was fine, then went black and stayed black | GPU device lost | [Black after it was working](#black-after-it-was-working) |
| Whole image a bit too dark | sRGB back buffer (fixed in this version), wrong HDR output | [Too dark](#too-dark-too-bright-or-washed-out) |
| Objects pop in during the first seconds in a new area | Background shader compiling (by design) | `--sync-shaders` or `?syncshaders=1` |
| An effect never appears | `?skipshaders=1` is on, or a shader was skipped | Remove `skipshaders`, then check the log for `shader skipped` |
| Landscape or terrain missing | Tessellation (hull/domain shaders) isn't supported | `-ExecCmds` with tessellation off, see [below](#unreal-engine-settings-that-work-around-problems) |
| Black squares that spread or flash | NaN/Inf in an HDR target (bloom, TAA) | [Black blocks](#black-or-white-blocks-that-spread) |
| Flicker or shimmering | TAA history or a cache bug | [Flicker](#flicker) |
| Spiky or stretched triangles | Vertex format or typed buffer read | `?expandbuffers=1`, then report |
| Inside-out models or missing faces | Cull mode or winding | `?nocull=1` to confirm, then report |
| Shadows missing or blocky | Depth comparison sampling | `?fulldepthmask=1` to confirm, then report |
| Everything looks right but the GPU is slow | Heavy post-processing | `--quality low` (see [PERFORMANCE.md](PERFORMANCE.md)) |

---

## Everything is black

### Black from the start

The page loads and the status line goes away, but nothing ever appears.

1. **Use current Chrome or Edge.** UEWASM needs WebGPU and JavaScript Promise Integration (JSPI). Firefox and
   older browsers won't run the game, or will fall back to WebGL 2.
2. **Check that the GPU is actually used.** Open `chrome://gpu` (or `edge://gpu`):
   - *WebGPU* should say **Hardware accelerated**.
   - If it says *Software only* or *Disabled*, turn on **Settings → System → Use graphics acceleration when
     available**, restart the browser, and update the GPU driver.
   - If the GPU is on the blocklist, `chrome://flags/#enable-unsafe-webgpu` turns it on anyway. Use that
     for testing only.
3. **On laptops,** open **Windows Settings → System → Display → Graphics**, add the browser, and set it to
   **High performance**. The log prints `this may be the integrated GPU` when it suspects this.
4. **Look for `software fallback!`, `WebGPU unavailable` or `null renderer` in the log.** Any of them means
   the browser didn't provide a usable GPU. Fix that first: nothing else in this guide helps until it's solved.
5. **Serve the build over `http://localhost` or `https://`.** WebGPU only works in secure contexts. Opening
   `index.html` from disk works only with `--single-file` builds. Use `uewasm-cli serve builds\MyGame\web`
   for normal builds.
6. **With `--threads real`,** the page must be cross-origin isolated. The build includes a service worker that
   sets this up. If the page loaded before the worker was installed, reload once.

### UI works but the 3D scene is black

The menus, HUD or loading screen draw, but the world is pure black. This almost always means one full-screen
pass in Unreal's post-processing chain is broken. The scene does render, but a later pass multiplies it by
zero or replaces it with NaN.

The usual culprits, from most to least common:

1. **Auto exposure (eye adaptation).** It runs as a compute shader that measures the scene and writes one
   exposure value. If that pass is dropped, the exposure stays at 0 and the scene is multiplied by 0. Test it
   by turning auto exposure off (see [Unreal Engine settings](#unreal-engine-settings-that-work-around-problems)):
   ```
   ?arg=-ExecCmds="r.EyeAdaptationQuality 0"
   ```
2. **A dropped tonemapper or post-process shader.** Look for `shader skipped` in the log. Lower
   `--quality` (or `r.Tonemapper.Quality 0`) often swaps the shader for a simpler one that translates.
3. **NaNs from bloom or TAA.** One NaN pixel spreads through the blur passes until the whole screen is black.
   Use a [frame capture](#frame-capture) to find the first pass that produces NaNs.
4. **Pipelines still compiling.** For the first few seconds in a new area, draws whose pipeline isn't ready are
   *waited for* (default) or *dropped* (`?skipshaders=1`). If the debug panel shows *shaders compiling* stuck
   above 0, a pipeline failed. Check the log for `[webgpu] pipeline`.

### Black after it was working

The game ran, then the picture went black and stayed black (sound and input may still work). Look for
`[webgpu] DEVICE LOST` in the log. The page also shows a red message and the debug panel says
**GPU DEVICE LOST**.

A lost device can't be recovered without reloading the page. The common reasons:

- **A frame took longer than about 2 seconds on the GPU**, which trips Windows' TDR watchdog. UEWASM
  already splits heavy frames into several submissions, but a very slow GPU can still hit it. Lower
  `--resolution` or `--quality`.
- **A driver crash.** Update the GPU driver. If it keeps happening at the same spot, report it with the log.
- **The GPU process ran out of memory.** Lower `--mem-limit-gb` so the video memory budget fits the card.

---

## Too dark, too bright or washed out

- **Every frame is noticeably darker than on Windows, including menus.** This was a bug in earlier versions:
  games with an sRGB swap chain (`DXGI_FORMAT_*_UNORM_SRGB`) were shown through a linear view. It's fixed in
  `present()` in `runtime/gpu_wgpu.c`. Rebuild the game with this version.
- **Washed out, grey or very bright.** The game may be writing HDR output (scRGB or PQ) that the canvas
  shows as plain 8-bit. Turn it off with `r.HDR.EnableHDROutput 0`.
- **Dark but not black, and the brightness pumps.** Auto exposure is reading a wrong value. Try
  `r.EyeAdaptationQuality 0`.
- **Colours swapped (red and blue).** A BGRA/RGBA mismatch in a texture conversion. Report it with a frame
  capture and the texture's format number from the log.

---

## Glitches

### Black or white blocks that spread

These are NaN or infinity values in a floating-point render target (`rgba16float`, `rg11b10ufloat`). One bad
pixel spreads through blur, bloom and temporal AA.

1. Take a [frame capture](#frame-capture). For each render target it counts NaN, negative and very large
   values, and writes a mask image (red = huge, blue = negative, green = NaN). The first pass whose mask
   isn't empty is the one to look at.
2. Workarounds while you look: `r.BloomQuality 0`, `r.PostProcessAAQuality 0` (turns off TAA), and
   `r.SSR.Quality 0`.

### Flicker

- **Flicker in the first seconds in a new area** is objects appearing as their shaders finish compiling.
  That's expected. `--sync-shaders` or `?syncshaders=1` avoids it, but the game freezes while each shader
  compiles instead.
- **Constant flicker or shimmering** usually comes from temporal AA, or from a caching bug in the runtime.
  Try these one at a time and note which one stops it:
  `?nodrawmemo=1`, `?norescache=1`, `?noringkeep=1`, `?ringreset=1`. Each turns off one optimisation, so the
  game runs slower. The one that fixes it names the bug, which is useful in a report.

### Missing objects or effects

1. Check the log for `shader skipped`, `geometry stage skipped`, `dropped (…)` and `unsupported texture format`.
2. Check that `?skipshaders=1` isn't on. It drops draws whose shader is still compiling, so an effect drawn only
   once (a texture rendered when a menu opens, for example) is lost for good.
3. **Tessellated surfaces** (landscapes with displacement, some UE4 materials) use hull and domain shaders.
   WebGPU has no tessellation stage, so those draws are skipped. In UE4, turning tessellation off makes the
   engine draw the same mesh without it. See the
   [Unreal Engine settings](#unreal-engine-settings-that-work-around-problems).
4. **MSAA:** `?noresolve=1` skips `ResolveSubresource`. It's a diagnostic switch: if something *reappears*
   with it on, the resolve is the bug.

### Inside-out models, holes in geometry

`?nocull=1` turns off back-face culling. If the missing faces come back, the cull mode or front-face winding
is being translated wrong for that draw. Report it with a frame capture.

### Wrong or missing shadows

Shadow maps are sampled as depth textures with comparison samplers. `?fulldepthmask=1` treats every bound
texture as a possible depth texture. If shadows come back with it on, the shader reflection missed a depth
slot. `?depthalways=1` turns off the depth test for draws that don't write depth, which helps tell depth
problems apart from shader problems.

### Spiky, stretched or exploding triangles

This is a vertex buffer read in the wrong format: a vertex shader that reads a typed buffer (UE uses these
for skinning, particles and instancing), or an input layout format that isn't converted correctly. Try
`?expandbuffers=1`, which reads typed buffers through converted copies instead of in place. If it fixes the
problem, report the format.

---

## Stutters while new effects appear

Each new shader is translated and compiled the first time it's drawn. That happens in the background
(default), so the game keeps running, but objects using that shader appear a moment later. The browser
caches compiled shaders, so a second run through the same area is much smoother.

- `[hitch] 230 ms frame: 14 new shaders, 14 new pipelines …` in the log means a frame was slow because of
  shader compiling. That's the cost of a first run, not a bug.
- `[hitch] … MB in N downloads` means the frame waited for game files to download.
- `[hitch] GPU took … ms` means the GPU itself was slow on that frame.

See [PERFORMANCE.md](PERFORMANCE.md) for the build options that reduce stutter and raise FPS.

---

## Page switches for rendering

Add these to the game's address, for example `index.html?syncshaders=1&nocull=1`. The debug panel's
**Switches** section sets the common ones and reloads the page for you. None of them change the build.

| Switch | Effect |
|---|---|
| `?syncshaders=1` | Compile each pipeline when first drawn. Exact first frames, but freezes while compiling. |
| `?skipshaders=1` | Drop draws whose pipeline is still compiling instead of waiting for it. Faster, but can lose one-off effects. |
| `?noresolve=1` | Skip MSAA resolves (diagnostic). |
| `?nocull=1` | Turn off back-face culling (diagnostic). |
| `?depthalways=1` | Turn off the depth test for draws that don't write depth (diagnostic). |
| `?fulldepthmask=1` | Treat every bound texture as a possible depth texture (diagnostic for shadows). |
| `?expandbuffers=1` | Read typed buffers through converted copies instead of in place (diagnostic for vertex garbage). |
| `?nodrawmemo=1`, `?norescache=1`, `?noringkeep=1`, `?ringreset=1` | Turn off one runtime cache each (diagnostic for flicker and stale data). |
| `?syncreadback=1` | Wait for every GPU readback (exact occlusion and exposure results, slower). |
| `?capture=1` | Enable frame capture (see below). |
| `?gpuprof=1` | Time every GPU pass and log the slowest ones (`[gpuprof]` lines) along with their shader source. |
| `?texdebug=1`, `?bufdebug=1`, `?bgdebug=1` | Log texture, buffer and bind group details. Very verbose. |
| `?texfmt=1` | Log the first texture of each format, bind and usage, and the WebGPU format it is stored as (`[texfmt]` lines). |
| `?glyphdebug=1` | Log the creates, maps, updates and copies that touch 8-bit single-channel textures, which is how UI text glyph caches are filled (`[glyph]` lines, first 600). |
| `?arg=-foo` | Pass an extra launch argument to the game (repeat for more). |

From the DevTools console, while the game runs:

| Command | Effect |
|---|---|
| `Module.uewgDebug = true` | Log the WGSL of every new shader and the description of every new pipeline. |
| `window.uewStats = 1` | Every 600 frames, also log the biggest buffer writers. |
| `window.uewLogCreates = 1` | Log every shader and pipeline as it is created (`2` adds bind groups). |
| `window.uewShaderPatch = code => code.replace(…)` | Rewrite WGSL before it's compiled, to test a fix without rebuilding. It only applies to shaders created after you set it, so set it early or reload into an area. |

### Frame capture

1. Reload with `?capture=1` (or tick **Enable frame capture** in the debug panel, then **Reload with these**).
2. Go to the broken spot and click **Capture frame** in the debug panel.
3. A `uewasm-frame-….json` file downloads. It contains every pass and draw of that frame (`events`), a PNG of
   every render target (`images`), the WGSL of every shader used (`shaders`), and NaN, negative and huge value
   counts and masks per target.

Open the PNGs in order to find the first pass where the picture goes wrong. The `ps` and `vs` ids on that draw
point to the shader source in `shaders`.

---

## Unreal Engine settings that work around problems

Unreal reads console variables at startup from `-ExecCmds`. Separate several with commas:

```
index.html?arg=-ExecCmds="r.EyeAdaptationQuality 0, r.BloomQuality 0"
```

To make them part of the build, pass the same thing with `--game-arg` when you build:

```
uewasm-cli recomp "C:\Games\MyGame" --game-arg=-ExecCmds="r.EyeAdaptationQuality 0"
```

You can also type them live in the UE developer console. Click **Enable UE dev console** in the debug panel,
then press `~`.

| Console variable | Use it for |
|---|---|
| `r.EyeAdaptationQuality 0` | Black or pumping scene from auto exposure |
| `r.DefaultFeature.AutoExposure 0` | Same, on projects that set it per post-process volume |
| `r.HDR.EnableHDROutput 0` | Washed out or over-bright image |
| `r.BloomQuality 0` | Bloom NaNs, black blocks |
| `r.PostProcessAAQuality 0` | TAA ghosting, flicker, NaN spread |
| `r.SSR.Quality 0` | Screen-space reflection artefacts |
| `r.AmbientOcclusionLevels 0` | SSAO noise or dark halos |
| `r.MotionBlurQuality 0` | Smearing |
| `r.Tonemapper.Quality 0` | Black or wrong colours from the tonemapper |
| `r.ShadowQuality 0` | Isolate shadow problems (removes dynamic shadows) |
| `r.VolumetricFog 0` | Fog that turns black or flickers |
| `r.Lumen.DiffuseIndirect.Allow 0`, `r.Lumen.Reflections.Allow 0` | UE5: Lumen artefacts (also much faster) |
| `r.Nanite 0` | UE5: missing Nanite meshes (the engine draws the fallback meshes) |
| `r.ScreenPercentage 70` | Lower the internal render resolution |

The build option `--quality low|medium|high|epic` sets all of Unreal's scalability groups except textures in
one go. That's the simplest way to remove heavy effects.

---

## For developers: fixing the shader translator

Where things live:

| File | What it does |
|---|---|
| `runtime/dxbc.c`, `runtime/dxbc.h`, `runtime/dxbc_parse.h` | Parses DXBC containers: token stream, signatures, reflection. |
| `runtime/dxbc_wgsl.c` | Translates DXBC → WGSL. `dxbc_to_wgsl_ex()` is the entry point. Unknown opcodes emit `// unsupported opcode N` and set `error`. |
| `runtime/gpu_wgpu.c` | The D3D11 → WebGPU mapping: `texfmt()` (DXGI → WebGPU formats and conversions), pipeline state, bind groups, geometry shader emulation, `present()`. |
| `runtime/web/uew_webgpu.js` | The WebGPU calls themselves, async pipelines, frame capture, GPU profiling. |
| `runtime/gpu_web.c` | The WebGL 2 backend (DXBC → GLSL ES). |

### A workflow for shader bugs

1. **Collect the game's shaders.** This pulls every DXBC blob out of the install:
   ```
   uewtest extract-dxbc "C:\Games\MyGame" tests/bin/shaders
   ```
2. **Translate them all offline.** This builds the dump tool with MSVC (from a *Developer Command Prompt*):
   ```
   cl /nologo /O2 /Fe:tests\bin\wgsl_dump.exe tests\shaders\wgsl_dump.c runtime\dxbc.c runtime\dxbc_wgsl.c
   tests\bin\wgsl_dump.exe tests\bin\shaders tests\bin\wgsl
   ```
   It prints how many shaders translated and groups the failures, so `unsupported opcode N` shows up with a
   count. `DXBC_TRACE=1` prints each file name, to find the one that crashes the translator.
3. **Compile the WGSL with the browser's real compiler:**
   ```
   uewtest wgsl-check tests/bin/wgsl
   ```
   It groups the errors by kind and writes `wgsl_errors.json`. `uewtest webgl-check` does the same for
   the WebGL 2 path.
4. **Name an unknown opcode.** `uewtest opcode-map tests/bin/shaders` lines the token stream up with
   `fxc /dumpbin` disassembly and tells you which instruction a number is.
5. **Fix `dxbc_wgsl.c`, repeat 2–3 until they're clean, then rebuild the game.** Run the same
   `uewasm-cli recomp` command again with the same output folder. The runtime is always rebuilt, and lifted game
   code that didn't change isn't recompiled.
6. **Check nothing else broke:**
   ```
   uewtest gfx webgpu        # D3D11 triangle, checked per pixel in a headless browser
   uewtest gfx webgpu srgb   # the same on an sRGB swap chain
   uewtest gfx webgl2
   ```

### Common translation mistakes and their symptoms

| Symptom | Usual cause in the translator or mapping |
|---|---|
| Black output from one pass | Wrong component swizzle or write mask, or an integer/float bitcast missing (`asfloat`/`asuint` in HLSL are free in DXBC but explicit in WGSL) |
| NaNs | `rcp`/`rsq`/`div` of 0 behave differently, or a `min`/`max` with NaN operands. DXBC `min`/`max` return the non-NaN operand. |
| Texture looks noisy or garbage | Wrong `texfmt()` entry or `conv` (for example 16-bit unorm stored as half float through `CV_UN16`) |
| Shadows wrong | Comparison sampler or depth texture kind (`depth_srv` mask) not matching how the shader samples |
| Geometry offset or exploding | Constant buffer packing, or a typed buffer read in place with the wrong element format |
| One-pixel seams or a half-pixel shift | `SV_Position` pixel centre, or a `ld`/`Load` offset |

WGSL is stricter than HLSL: no implicit int/float conversion, no out-of-range array indexing without
clamping, uniformity rules for derivatives. When `uewtest wgsl-check` reports an error, the fix almost always
belongs in `dxbc_wgsl.c`, not in the game.

---

## Direct3D 9 games (Unreal Engine 3)

32-bit Unreal Engine 3 games (Dishonored, for one) render through Direct3D 9: `runtime/d3d9.c` maps it onto the same
WebGPU backend, and `runtime/d3d9_wgsl.c` translates their Shader Model 2/3 shaders. Everything above applies, plus a
few D3D9 specifics. 64-bit UE3 games such as Rocket League use Direct3D 11 like UE4 games, so only the sections
above apply to them.

| Symptom | Cause | What to do |
|---|---|---|
| `[d3d9] fixed-function draw skipped` in the log | The game drew without a vertex or pixel shader; fixed-function pipelines aren't emulated | Report it; UE3 itself always uses shaders |
| `[d3d9] CreateTexture: format N not supported` | A D3D9 format with no mapping in `FMTS[]` in `d3d9.c` | Add the format there (with a CPU conversion if WebGPU has no equivalent) |
| `[d3d9] depth buffer WxH doesn't match render target` | D3D9 allows a larger depth buffer than the colour target; WebGPU doesn't | Those draws run without depth. A real fix is to give such passes a matching depth buffer |
| Everything offset by half a pixel, blurry UI | The half-pixel offset in the vertex shader (`D3D9_CB_EXTRA`) isn't being updated with the viewport | Check `set_cb_extra()` runs after `SetViewport`/`SetRenderTarget` |
| Cut-out foliage or text fully missing | Alpha test: D3D9 applies it after the pixel shader; here it's emulated from `D3DRS_ALPHATESTENABLE/ALPHAFUNC/ALPHAREF` | Check those render states reach `set_cb_extra()` |
| Shadows missing | Depth formats are reported as not usable as textures, so UE3 falls back to colour (R32F) shadow maps | Expected; hardware shadow maps (INTZ/D24 sampling) aren't implemented |

`?d3d9trace=<frame>` logs every draw of that frame (counted in presents) as the game issued it: its textures
(D3D9 format, size, sampler addressing), blend states, the bound depth-stencil surface with the stencil and colour
write states, the first pixel and vertex shader constants (`pc0`, `pc6`, `pc7`, `vc0`-`vc3`, `vc6`-`vc13`), and each
shader once as translated to WGSL. To pick the frame while the game runs (a menu reached by hand), call
`Module._uew_d3d9_trace()` from the page's console instead: it traces the next frame. Use it when a
frame capture shows a draw doing something wrong, to tell a translation bug (the game asked for something else) from
what the game really submitted. With Scaleform UIs, `pc6`/`pc7` are the colour transform (multiply, add) and
`vc6`-`vc13` the position and texture matrices.

To test the translator on a whole game, `uewtest extract-d3d9` pulls every D3D9 shader out of a UE3 game's
shader caches (it unpacks LZO/zlib-compressed packages), and `wgsl_dump` / `uewtest wgsl-check` work on them unchanged.
All 39,286 of Dishonored's shaders translate and compile in Edge.

## Reporting a rendering bug

Include:

1. The game, its Unreal Engine version and the build options you used (they're in `builds\<game>\options.json`).
2. The saved log from the debug panel (**Save log**).
3. Browser and version, and the GPU line from the log (`[webgpu] GPU: …`).
4. A frame capture of the broken frame (`?capture=1`, then **Capture frame**).
5. Which page switches, if any, change the problem.
