mergeInto(LibraryManager.library, {
  uewg_init__async: true,
  uewg_init: async function (canvasW, canvasH, ringPtr, ringSize) {
    // pointers above 2 GB arrive as negative numbers
    ringPtr >>>= 0;
    if (!navigator.gpu) return 0;
    const adapter = await navigator.gpu.requestAdapter({ powerPreference: "high-performance" });
    if (!adapter) return 0;
    const want = ["texture-compression-bc", "texture-compression-bc-sliced-3d", "float32-filterable", "float32-blendable", "rg11b10ufloat-renderable", "depth32float-stencil8",
      "depth-clip-control", "indirect-first-instance", "texture-formats-tier1", "texture-component-swizzle", "bgra8unorm-storage", "dual-source-blending"];
    // debug: window.uewGpuProfile = 1 times every pass on the GPU and logs the slowest ones
    if (typeof window !== "undefined" && window.uewGpuProfile) want.push("timestamp-query");
    const features = want.filter(f => adapter.features.has(f));
    const L = adapter.limits, req = {};
    for (const k of ["maxSampledTexturesPerShaderStage", "maxSamplersPerShaderStage", "maxStorageBuffersPerShaderStage", "maxStorageTexturesPerShaderStage",
      "maxUniformBuffersPerShaderStage", "maxBindingsPerBindGroup", "maxInterStageShaderVariables", "maxColorAttachmentBytesPerSample", "maxBufferSize",
      "maxStorageBufferBindingSize", "maxDynamicUniformBuffersPerPipelineLayout", "maxComputeWorkgroupStorageSize", "maxComputeInvocationsPerWorkgroup",
      "maxTextureArrayLayers", "maxTextureDimension2D", "maxVertexAttributes", "maxVertexBuffers"]) if (k in L) req[k] = L[k];
    let device;
    try { device = await adapter.requestDevice({ requiredFeatures: features, requiredLimits: req }); }
    catch (e) { console.log("[webgpu] requestDevice failed: " + e.message); return 0; }
    const canvas = Module.canvas || document.getElementById("canvas");
    const ctx = canvas && canvas.getContext("webgpu");
    if (!ctx) return 0;
    const format = navigator.gpu.getPreferredCanvasFormat();
    ctx.configure({ device, format, alphaMode: "opaque" });
    const g = Module.uewg = {
      device, queue: device.queue, ctx, canvasFormat: format, o: [null], free: [], features: new Set(features), errors: 0,
      ring: device.createBuffer({ size: ringSize, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_SRC | GPUBufferUsage.COPY_DST }),
      ringPtr, enc: null, pass: null, blit: null, frames: 0, draws: 0, dummies: new Map(),
    };
    g.errKinds = new Map();
    device.addEventListener("uncapturederror", e => {
      const m = e.error.message;
      if (g.errors++ < 40) console.log("[webgpu] " + m.split("\n").slice(0, 3).join(" | "));
      const kind = m.split("\n")[0].replace(/\[[^\]]*\]/g, "[]").replace(/\d+/g, "N").slice(0, 140);
      g.errKinds.set(kind, (g.errKinds.get(kind) || 0) + 1);
    });
    g.add = (x) => { if (g.free.length) { const i = g.free.pop(); g.o[i] = x; return i; } g.o.push(x); return g.o.length - 1; };
    g.encoder = () => g.enc || (g.enc = device.createCommandEncoder());
    // background shader compiles: on unless the page asks for ?syncshaders=1 or the build turned them off
    g.pendingPipes = 0;
    g.pending = new Set();
    // ?skipshaders=1: draws whose pipeline is still compiling are dropped instead of waited for (faster, but an
    // effect drawn only once, like a texture rendered when a screen opens, then stays missing)
    g.skipPending = typeof location !== "undefined" && /[?&]skipshaders=1/.test(location.search);
    g.asyncShaders = () => {
      if (g.asyncMode === undefined)
        g.asyncMode = !(typeof location !== "undefined" && /[?&]syncshaders=1/.test(location.search)) && Module.uewAsyncShaders !== false;
      return g.asyncMode;
    };
    if (features.includes("timestamp-query")) {
      const N = 1024, P = g.prof = { N, qs: device.createQuerySet({ type: "timestamp", count: N * 2 }), n: 0, passes: [], cur: null, busy: false,
        res: device.createBuffer({ size: N * 16, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC }), shaders: new Map(), shown: new Set() };
      const wrap = enc => {
        const br = enc.beginRenderPass.bind(enc), bc = enc.beginComputePass.bind(enc);
        const stamp = (kind, d) => {
          if (P.n >= N) { P.cur = null; return d; }
          P.cur = { kind, pipes: new Map(), draws: 0 };
          P.passes.push(P.cur);
          const tw = { querySet: P.qs, beginningOfPassWriteIndex: P.n * 2, endOfPassWriteIndex: P.n * 2 + 1 };
          P.n++;
          return Object.assign({}, d || {}, { timestampWrites: tw });
        };
        enc.beginRenderPass = d => br(stamp("render", d));
        enc.beginComputePass = d => bc(stamp("compute", d));
        return enc;
      };
      g.encoder = () => g.enc || (g.enc = wrap(device.createCommandEncoder()));
      P.frameEnd = () => {
        if (!P.n) return;
        if (P.busy) { P.n = 0; P.passes = []; return; }
        g.endPass();
        const n = P.n, passes = P.passes, enc = g.encoder();
        const out = device.createBuffer({ size: n * 16, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
        enc.resolveQuerySet(P.qs, 0, n * 2, P.res, 0);
        enc.copyBufferToBuffer(P.res, 0, out, 0, n * 16);
        P.n = 0; P.passes = []; P.busy = true;
        const frame = g.frames;
        setTimeout(() => out.mapAsync(GPUMapMode.READ).then(() => {
          const t = new BigInt64Array(out.getMappedRange());
          const ms = [];
          let total = 0;
          for (let i = 0; i < n; i++) { const v = Number(t[2 * i + 1] - t[2 * i]) / 1e6; ms.push(v > 0 && v < 1e4 ? v : 0); total += ms[i]; }
          out.unmap(); out.destroy(); P.busy = false;
          if (total < 12) return;
          const top = ms.map((v, i) => [v, i]).sort((a, b) => b[0] - a[0]).slice(0, 5);
          console.log(`[gpuprof] frame ${frame}: ${total.toFixed(1)} ms GPU in ${n} passes; slowest: ` + top.map(([v, i]) => {
            const ps = passes[i], pp = [...ps.pipes].sort((a, b) => b[1] - a[1]).slice(0, 3);
            return `#${i} ${ps.kind} ${v.toFixed(1)} ms ${ps.draws} draws [` + pp.map(([id, c]) => { const pl = g.o[id] || {}; return `pipe ${id} vs ${pl.__vs} ps ${pl.__ps} x${c}`; }).join(", ") + "]";
          }).join(" | "));
          // the pixel shader behind the worst pass, once
          const worst = passes[top[0][1]];
          if (worst) for (const [id] of worst.pipes) {
            const pl = g.o[id] || {}, sh = pl.__ps || pl.__cs;
            if (sh && !P.shown.has(sh) && P.shown.size < 12 && P.shaders.has(sh)) { P.shown.add(sh); console.log(`[gpuprof] shader ${sh}:
${P.shaders.get(sh)}`); break; }
          }
        }, () => { out.destroy(); P.busy = false; }), 0);
      };
    }
    g.endPass = () => { if (g.pass) { g.pass.end(); g.pass = null; } };
    // a command buffer that runs too long on the GPU trips Windows' GPU watchdog (~2 s), and the browser then loses the
    // device for good. Heavy frames (UE's GPU benchmark: hundreds of passes of long shaders) are split into several
    // submissions; an ordinary frame (~50 passes) stays one.
    g.passesInCb = 0;
    g.passBudget = () => { if (++g.passesInCb > 64 && g.enc) g.flush(0); };
    // only the part of the ring written since the last flush goes up; what is already there stays valid for the frame
    g.ringDone = 0;
    // flushes that do not know the write position (readbacks) read it from the C side: work already recorded may use
    // ring data written since the last upload
    g.flush = (used) => {
      g.endPass();
      if (!used && Module.uewRingUsedPtr) used = HEAPU32[Module.uewRingUsedPtr >>> 2];
      used = (used + 3) & ~3;
      if (used < g.ringDone) g.ringDone = 0;
      if (used > g.ringDone) g.queue.writeBuffer(g.ring, g.ringDone, HEAPU8, g.ringPtr + g.ringDone, used - g.ringDone);
      g.ringDone = used;
      if (g.enc) { g.queue.submit([g.enc.finish()]); g.enc = null; }
      g.passesInCb = 0;
    };
    g.dummy = (key) => {
      let x = g.dummies.get(key);
      if (x) return x;
      const [dim, type] = key.split(":");
      if (key === "buf" || key === "sbuf") x = device.createBuffer({ size: 256, usage: GPUBufferUsage.STORAGE });
      else if (key === "stex") x = device.createTexture({ size: [1, 1], format: "r32float", usage: GPUTextureUsage.STORAGE_BINDING }).createView();
      else {
        const fmt = type === "depth" ? "depth32float" : type === "sint" ? "rgba8sint" : type === "uint" ? "rgba8uint" : "rgba8unorm";
        const layers = dim.startsWith("cube") ? 6 : 1;
        const tdim = dim === "1d" ? "1d" : dim === "3d" ? "3d" : "2d";
        x = device.createTexture({ size: [1, 1, layers], dimension: tdim, format: fmt, usage: GPUTextureUsage.TEXTURE_BINDING }).createView({ dimension: dim });
      }
      g.dummies.set(key, x);
      return x;
    };
    g.emptyLayout = device.createBindGroupLayout({ entries: [] });
    g.zeroVB = () => g.zvb || (g.zvb = device.createBuffer({ size: 65536, usage: GPUBufferUsage.VERTEX }));
    // a bad call is logged and skipped rather than stopping the game
    g.guard = (what, fn) => { try { fn(); } catch (e) { g.skip.threw++; g.skip.last = `${what}: ${e.message} @ ${(e.stack || "").split("\n").slice(1, 3).join(" < ")}`; if (g.errors++ < 40) console.log(`[webgpu] ${what}: ${e.message}`); } };
    g.skip = { nopass: 0, nopipe: 0, threw: 0, last: "" };
    g.hitch = { last: 0, tex: 0, texPx: 0, upBytes: 0, sh: 0, pipe: 0, psh: 0, ppipe: 0, rb: 0, bytes: 0, req: 0, fms: 0, logged: 0 };
    // debug: window.uewCaptureFrame = N reads back every render target of frame N into Module.uewgCaptured
    g.capFrame = (typeof window != "undefined" && window.uewCaptureFrame) | 0;
    g.cap = null;
    g.capEvent = (e) => { if (g.cap && g.cap.events.length < 4000) g.cap.events.push(e); };
    g.capTarget = (att, aspect) => {
      const v = att && att.view, t = v && v.__tex;
      if (!t || g.cap.reads.length >= 400) return null;
      const bpp = { "rgba8unorm": 4, "rgba8unorm-srgb": 4, "bgra8unorm": 4, "bgra8unorm-srgb": 4, "rgb10a2unorm": 4, "rg11b10ufloat": 4, "r32float": 4,
        "rg32float": 8, "rgba32float": 16, "rgba16float": 8, "rg16float": 4, "r16float": 2, "r8unorm": 1, "rg8unorm": 2, "depth32float": 4, "depth32float-stencil8": 4 }[t.format];
      if (!bpp || t.dimension !== "2d") return null;
      const mip = v.__mip || 0, w = Math.max(1, t.width >> mip), h = Math.max(1, t.height >> mip), bpr = (w * bpp + 255) & ~255;
      const buf = device.createBuffer({ size: bpr * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      const src = { texture: t, mipLevel: mip, origin: [0, 0, v.__layer || 0] };
      if (aspect) src.aspect = aspect;
      g.encoder().copyTextureToBuffer(src, { buffer: buf, bytesPerRow: bpr, rowsPerImage: h }, [w, h, 1]);
      const r = { buf, w, h, bpr, fmt: t.format, tex: t.__id, mip, layer: v.__layer || 0 };
      g.cap.reads.push(r);
      return g.cap.reads.length - 1;
    };
    // clears wait for the next pass and become its load operation; anything else runs them first
    g.pendClears = [];
    g.flushClears = () => {
      for (const d of g.pendClears) {
        const colorAttachments = d.color.filter(c => c).map(c => ({ view: c.view, loadOp: "clear", storeOp: "store", clearValue: c.clear, depthSlice: c.slice >= 0 ? c.slice : undefined }));
        let depthStencilAttachment;
        if (d.depth) {
          const z = d.depth;
          depthStencilAttachment = { view: z.view, depthLoadOp: z.dclear !== undefined ? "clear" : "load", depthStoreOp: "store", depthClearValue: z.dclear ?? 1 };
          if (z.stencil) Object.assign(depthStencilAttachment, { stencilLoadOp: z.sclear !== undefined ? "clear" : "load", stencilStoreOp: "store", stencilClearValue: z.sclear ?? 0 });
        }
        if (colorAttachments.length || depthStencilAttachment) g.encoder().beginRenderPass({ colorAttachments, depthStencilAttachment }).end();
      }
      g.pendClears.length = 0;
    };
    g.endPass = (keepClears) => {
      if (g.pass) {
        g.pass.end();
        g.pass = null;
        if (g.cap && g.cap.cur) {
          const c = g.cap.cur;
          g.cap.cur = null;
          c.reads = c.desc.colorAttachments.map(a => a ? g.capTarget(a) : null);
          if (c.desc.depthStencilAttachment) c.reads.push(g.capTarget(c.desc.depthStencilAttachment, "depth-only"));
          delete c.desc;
          g.capEvent(c);
        }
      }
      if (!keepClears && g.pendClears.length) g.flushClears();
    };
    g.capFinish = async (cap) => {
      const half = (u) => { const e = (u >> 10) & 31, m = u & 1023, s = u & 32768 ? -1 : 1; return e === 0 ? s * m * 5.96e-8 : e === 31 ? (m ? NaN : s * Infinity) : s * (1 + m / 1024) * Math.pow(2, e - 15); };
      const uf = (v, mb) => { const e = v >> mb, m = v & ((1 << mb) - 1); return e === 0 ? m / (1 << mb) * Math.pow(2, -14) : e === 31 ? Infinity : (1 + m / (1 << mb)) * Math.pow(2, e - 15); };
      const out = [];
      for (const r of cap.reads) {
        await r.buf.mapAsync(GPUMapMode.READ);
        const raw = new Uint8Array(r.buf.getMappedRange().slice(0));
        r.buf.unmap(); r.buf.destroy();
        const dv = new DataView(raw.buffer), px = new Float32Array(r.w * r.h * 4);
        for (let y = 0; y < r.h; y++) for (let x = 0; x < r.w; x++) {
          const o = y * r.bpr, k = (y * r.w + x) * 4;
          let c = [0, 0, 0, 1];
          switch (r.fmt) {
            case "rgba8unorm": case "rgba8unorm-srgb": { const q = o + x * 4; c = [raw[q] / 255, raw[q + 1] / 255, raw[q + 2] / 255, raw[q + 3] / 255]; break; }
            case "bgra8unorm": case "bgra8unorm-srgb": { const q = o + x * 4; c = [raw[q + 2] / 255, raw[q + 1] / 255, raw[q] / 255, raw[q + 3] / 255]; break; }
            case "rgb10a2unorm": { const u = dv.getUint32(o + x * 4, true); c = [(u & 1023) / 1023, ((u >> 10) & 1023) / 1023, ((u >> 20) & 1023) / 1023, (u >>> 30) / 3]; break; }
            case "rg11b10ufloat": { const u = dv.getUint32(o + x * 4, true); c = [uf(u & 2047, 6), uf((u >> 11) & 2047, 6), uf(u >>> 22, 5), 1]; break; }
            case "r32float": case "depth32float": case "depth32float-stencil8": { const f = dv.getFloat32(o + x * 4, true); c = [f, f, f, 1]; break; }
            case "rg32float": c = [dv.getFloat32(o + x * 8, true), dv.getFloat32(o + x * 8 + 4, true), 0, 1]; break;
            case "rgba32float": for (let j = 0; j < 4; j++) c[j] = dv.getFloat32(o + x * 16 + j * 4, true); break;
            case "rgba16float": for (let j = 0; j < 4; j++) c[j] = half(dv.getUint16(o + x * 8 + j * 2, true)); break;
            case "rg16float": c = [half(dv.getUint16(o + x * 4, true)), half(dv.getUint16(o + x * 4 + 2, true)), 0, 1]; break;
            case "r16float": { const f = half(dv.getUint16(o + x * 2, true)); c = [f, f, f, 1]; break; }
            case "r8unorm": { const f = raw[o + x] / 255; c = [f, f, f, 1]; break; }
            case "rg8unorm": c = [raw[o + x * 2] / 255, raw[o + x * 2 + 1] / 255, 0, 1]; break;
          }
          px.set(c, k);
        }
        const mn = [Infinity, Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity, -Infinity], sum = [0, 0, 0, 0];
        let bad = 0, neg = 0, huge = 0;
        const mask = new Uint8Array(r.w * r.h);
        for (let i = 0; i < px.length; i++) {
          const v = px[i], j = i & 3;
          if (!isFinite(v)) { bad++; mask[i >> 2] = 3; continue; }
          if (v < mn[j]) mn[j] = v; if (v > mx[j]) mx[j] = v; sum[j] += v;
          if (j < 3 && v < 0) { neg++; mask[i >> 2] |= 1; }
          if (j < 3 && v > 1000) { huge++; mask[i >> 2] |= 2; }
        }
        let maskPng = null;
        if (neg || huge || bad) {
          const mc = document.createElement("canvas"); mc.width = r.w; mc.height = r.h;
          const m2 = mc.getContext("2d"), mi = m2.createImageData(r.w, r.h);
          for (let i = 0; i < r.w * r.h; i++) { mi.data[i * 4] = mask[i] & 2 ? 255 : 0; mi.data[i * 4 + 2] = mask[i] & 1 ? 255 : 0; mi.data[i * 4 + 1] = mask[i] === 3 ? 255 : 0; mi.data[i * 4 + 3] = 255; }
          m2.putImageData(mi, 0, 0);
          maskPng = mc.toDataURL("image/png");
        }
        const depth = r.fmt.startsWith("depth");
        const cv = document.createElement("canvas"); cv.width = r.w; cv.height = r.h;
        const c2 = cv.getContext("2d"), img = c2.createImageData(r.w, r.h);
        for (let i = 0; i < r.w * r.h; i++) for (let j = 0; j < 4; j++) {
          let v = px[i * 4 + j];
          if (!isFinite(v)) v = j === 0 ? 1 : 0;
          else if (j < 3) v = depth ? (v - mn[0]) / Math.max(1e-9, mx[0] - mn[0]) : v / (1 + Math.max(0, v - 1));
          img.data[i * 4 + j] = j === 3 ? 255 : Math.max(0, Math.min(255, v * 255));
        }
        c2.putImageData(img, 0, 0);
        const n = r.w * r.h;
        // HDR float targets: also an exposure-normalised copy (mean luminance -> 0.18) so dark scene colour is readable
        let hdrPng = null;
        if (/float/.test(r.fmt) && !depth) {
          const lum = (sum[0] + sum[1] + sum[2]) / (3 * n), k = lum > 0 ? 0.18 / lum : 1;
          const img2 = c2.createImageData(r.w, r.h);
          for (let i = 0; i < n; i++) for (let j = 0; j < 4; j++) {
            let v = px[i * 4 + j] * (j < 3 ? k : 1);
            if (!isFinite(v)) v = 0;
            if (j < 3) v = Math.pow(v / (1 + v), 1 / 2.2);
            img2.data[i * 4 + j] = j === 3 ? 255 : Math.max(0, Math.min(255, v * 255));
          }
          const cv2 = document.createElement("canvas"); cv2.width = r.w; cv2.height = r.h;
          cv2.getContext("2d").putImageData(img2, 0, 0);
          hdrPng = cv2.toDataURL("image/png");
        }
        out.push({ fmt: r.fmt, w: r.w, h: r.h, tex: r.tex, mip: r.mip, layer: r.layer, min: mn, max: mx, mean: sum.map(s => s / n), nonfinite: bad, neg, huge, mask: maskPng, hdr: hdrPng, png: cv.toDataURL("image/png") });
      }
      // raw mip 0 of textures asked for by id (compressed ones too), base64
      const raw = {};
      let want = window.uewCaptureTex || [];
      if (want === "auto") {
        want = new Set();
        for (const e of cap.events) if (e.draws && e.draws.length >= 20) for (const r of (e.draws[0].res[3] || []).slice(0, 4)) if (typeof r === "number") want.add(r);
        for (const e of cap.events) for (const d of e.draws || []) for (const grp of d.res || []) for (const r of grp || [])
          if (typeof r === "number" && g.o[r] instanceof GPUTexture && g.o[r].depthOrArrayLayers === 6) want.add(r);
      }
      const items = [];
      for (const id of want) {
        const t = g.o[id];
        if (!(t instanceof GPUTexture)) continue;
        const mips = t.depthOrArrayLayers === 6 ? t.mipLevelCount : 1;
        for (let m = 0; m < mips; m++) items.push([id, m]);
      }
      for (const [id, mip] of items) {
        const t = g.o[id];
        if (!(t instanceof GPUTexture) || t.dimension !== "2d") continue;
        const bc = /^bc/.test(t.format), blk = bc ? (/^bc[14]/.test(t.format) ? 8 : 16) : 0;
        const bpp = { "rgba8unorm": 4, "rgba8unorm-srgb": 4, "bgra8unorm": 4, "rgba16float": 8, "r8unorm": 1, "rg8unorm": 2, "r16float": 2, "rgba32float": 16 }[t.format];
        if (!bc && !bpp) { raw[id] = { fmt: t.format }; continue; }
        const tw = Math.max(1, t.width >> mip), th = Math.max(1, t.height >> mip);
        const bw = bc ? (tw + 3) >> 2 : tw, bh = bc ? (th + 3) >> 2 : th, rowb = bw * (bc ? blk : bpp), bpr = (rowb + 255) & ~255;
        const L = Math.min(t.depthOrArrayLayers, 6);
        const buf = device.createBuffer({ size: bpr * bh * L, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        const e = device.createCommandEncoder();
        e.copyTextureToBuffer({ texture: t, mipLevel: mip }, { buffer: buf, bytesPerRow: bpr, rowsPerImage: bh }, [bc ? bw * 4 : tw, bc ? bh * 4 : th, L]);
        g.queue.submit([e.finish()]);
        await buf.mapAsync(GPUMapMode.READ);
        const src = new Uint8Array(buf.getMappedRange()), dst = new Uint8Array(rowb * bh * L);
        for (let y = 0; y < bh * L; y++) dst.set(src.subarray(y * bpr, y * bpr + rowb), y * rowb);
        buf.unmap(); buf.destroy();
        let bin = "";
        for (let i = 0; i < dst.length; i += 32768) bin += String.fromCharCode.apply(null, dst.subarray(i, i + 32768));
        raw[mip ? `${id}m${mip}` : id] = { fmt: t.format, w: tw, h: th * L, data: btoa(bin) };
      }
      const perDraw = [];
      for (const r of cap.perDraw) {
        await r.buf.mapAsync(GPUMapMode.READ);
        const u = new Uint16Array(r.buf.getMappedRange());
        let neg = 0, huge = 0;
        for (let y = 0; y < r.h; y++) for (let x = 0; x < r.w * 4; x++) {
          if ((x & 3) === 3) continue;
          const v = u[(y * r.bpr >> 1) + x];
          if (v & 0x8000 && v & 0x7FFF) neg++; else if ((v & 0x7FFF) > 0x63D0) huge++;
        }
        r.buf.unmap(); r.buf.destroy();
        perDraw.push({ pass: r.pass, draw: r.draw, ps: r.ps, tex: r.tex, neg, huge });
      }
      const px = [];
      for (const r of cap.px) {
        await r.buf.mapAsync(GPUMapMode.READ);
        const dv = new DataView(r.buf.getMappedRange());
        const h2f = (u) => { const e = (u >> 10) & 31, m = u & 1023, sg = u & 32768 ? -1 : 1; return e === 0 ? sg * m * 5.96e-8 : e === 31 ? NaN : sg * (1 + m / 1024) * Math.pow(2, e - 15); };
        for (const [x, y] of window.uewCapturePixels) {
          if (x >= r.w || y >= r.h) continue;
          const o = y * r.bpr + x * r.bpp;
          const val = r.kind === "z" ? [dv.getFloat32(o, true)] : [0, 2, 4, 6].map(k => +h2f(dv.getUint16(o + k, true)).toPrecision(5));
          px.push({ kind: r.kind, x, y, pass: r.pass, draw: r.draw, ps: r.ps, vs: r.vs, n: r.n, first: r.first, val });
        }
        r.buf.unmap(); r.buf.destroy();
      }
      Module.uewgCaptured = { events: cap.events, images: out, shaders: g.capShaders, raw, perDraw, px };
      console.log(`[webgpu] captured frame ${g.frames}: ${cap.events.length} events, ${out.length} targets`);
    };
    g.capShaders = {};
    g.stat = { pass: 0, reopen: 0, write: 0, writeBytes: 0, ringCopy: 0, texWrite: 0, bg: 0, draws: 0 };
    g.openPass = () => {
      g.stat.pass++;
      g.pass = g.encoder().beginRenderPass(g.lastPass);
      if (g.cap) g.cap.cur = { pass: g.cap.events.length, desc: g.lastPass, draws: [] };
    };
    const ai = adapter.info || {};
    console.log(`[webgpu] GPU: ${[ai.vendor, ai.architecture, ai.description].filter(x => x).join(" / ") || "not reported"}${ai.isFallbackAdapter ? " (software fallback!)" : ""}; features: ${features.join(" ")}`);
    if (/intel|integrated/i.test(`${ai.vendor} ${ai.architecture} ${ai.description}`))
      console.log("[webgpu] this may be the integrated GPU: Windows Settings > System > Display > Graphics, set your browser to High performance");
    // bit 1: 24-bit depth can be stored as 32-bit float (more precise, and readable by copies)
    return 1 | (g.features.has("depth32float-stencil8") ? 2 : 0);
  },

  uewg_max_dynamic_ubo: function () { return Module.uewg.device.limits.maxDynamicUniformBuffersPerPipelineLayout; },

  uewg_buffer_create: function (size, usage) {
    const g = Module.uewg;
    let u = GPUBufferUsage.COPY_DST | GPUBufferUsage.COPY_SRC;
    if (usage & 1) u |= GPUBufferUsage.VERTEX;
    if (usage & 2) u |= GPUBufferUsage.INDEX;
    if (usage & 4) u |= GPUBufferUsage.UNIFORM;
    if (usage & 8) u |= GPUBufferUsage.STORAGE;
    if (usage & 16) u |= GPUBufferUsage.INDIRECT;
    try { return g.add(g.device.createBuffer({ size: Math.max(256, (size + 255) & ~255), usage: u })); }
    catch (e) { console.log("[webgpu] buffer " + e.message); return 0; }
  },

  uewg_buffer_write: function (id, offset, ptr, size) {
    ptr >>>= 0;
    const g = Module.uewg, b = g.o[id];
    if (!b) return;
    const n = Math.min((size + 3) & ~3, b.size - offset);
    if (n > 0) {
      g.queue.writeBuffer(b, offset, HEAPU8, ptr, n);
      g.stat.write++; g.stat.writeBytes += n;
      if (g.stat.by) g.stat.by.set(id, [(g.stat.by.get(id) || [0])[0] + n, b.size, offset]);
    }
  },

  uewg_buffer_copy_ring: function (ringOff, dst, dstOff, size, ringUsed) {
    const g = Module.uewg, b = g.o[dst];
    if (!b) return;
    g.endPass();
    const n = Math.min(size, b.size - dstOff);
    if (n > 0) { g.encoder().copyBufferToBuffer(g.ring, ringOff, b, dstOff, n); g.stat.ringCopy++; }
  },

  uewg_texture_create: function (jsonPtr) {
    const g = Module.uewg;
    const d = JSON.parse(UTF8ToString(jsonPtr >>> 0));
    let usage = GPUTextureUsage.TEXTURE_BINDING | GPUTextureUsage.COPY_DST | GPUTextureUsage.COPY_SRC;
    if (d.rt) usage |= GPUTextureUsage.RENDER_ATTACHMENT;
    if (d.uav) usage |= GPUTextureUsage.STORAGE_BINDING;
    try {
      const t = g.device.createTexture({ size: [d.w, d.h, d.d], mipLevelCount: d.mips, format: d.format, dimension: d.dim, usage, viewFormats: d.viewFormats || [] });
      g.hitch.tex++; g.hitch.texPx += d.w * d.h * d.d;
      const id = g.add(t);
      if (g.capFrame) t.__id = id;
      return id;
    } catch (e) { console.log("[webgpu] texture " + d.format + ": " + e.message); return 0; }
  },

  uewg_texture_write: function (id, mip, x, y, z, w, h, d, ptr, bpr, rows, size) {
    ptr >>>= 0;
    const g = Module.uewg, t = g.o[id];
    if (!t) return;
    // compressed uploads cover whole blocks, even for mips smaller than one
    if (/^(bc|etc2|eac|astc)/.test(t.format)) { w = (w + 3) & ~3; h = (h + 3) & ~3; }
    g.stat.texWrite++;
    g.hitch.upBytes += bpr * rows * d;
    try { g.queue.writeTexture({ texture: t, mipLevel: mip, origin: [x, y, z] }, HEAPU8, { offset: ptr, bytesPerRow: bpr, rowsPerImage: rows }, [w, h, d]); }
    catch (e) { if (g.errors++ < 40) console.log("[webgpu] upload " + e.message); }
  },

  uewg_view_create: function (jsonPtr) {
    const g = Module.uewg;
    const d = JSON.parse(UTF8ToString(jsonPtr >>> 0));
    const t = g.o[d.tex];
    if (!t) return 0;
    const desc = { dimension: d.dim, baseMipLevel: d.mip, mipLevelCount: d.mips, baseArrayLayer: d.layer, arrayLayerCount: d.layers, aspect: d.aspect };
    if (d.format) desc.format = d.format;
    if (d.swizzle && g.features.has("texture-component-swizzle")) desc.swizzle = d.swizzle;
    try {
      const v = t.createView(desc);
      if (g.capFrame) { v.__tex = t; v.__mip = d.mip; v.__layer = d.layer; }
      return g.add(v);
    } catch (e) { if (g.errors++ < 40) console.log("[webgpu] view " + e.message); return 0; }
  },

  uewg_sampler_create: function (jsonPtr) {
    const g = Module.uewg;
    return g.add(g.device.createSampler(JSON.parse(UTF8ToString(jsonPtr >>> 0))));
  },

  uewg_shader_create: function (srcPtr) {
    const g = Module.uewg;
    let code = UTF8ToString(srcPtr >>> 0);
    // debug: window.uewShaderPatch(code) may return a replacement source
    if (window.uewShaderPatch) code = window.uewShaderPatch(code) || code;
    g.hitch.sh++;
    if (window.uewLogCreates) console.log(`[create] shader ${code.length} chars`);
    const m = g.device.createShaderModule({ code });
    if (Module.uewgDebug) console.log("[webgpu] shader " + code);
    m.getCompilationInfo().then(info => {
      for (const x of info.messages) if (x.type === "error" && g.errors++ < 40) console.log(`[webgpu] shader ${x.lineNum}:${x.linePos} ${x.message}`);
    });
    const id = g.add(m);
    if (g.capFrame) g.capShaders[id] = code;
    if (g.prof) g.prof.shaders.set(id, code);
    return id;
  },

  uewg_layout_create: function (jsonPtr) {
    const g = Module.uewg;
    const groups = JSON.parse(UTF8ToString(jsonPtr >>> 0));
    const bgls = groups.map(entries => entries.length ? g.device.createBindGroupLayout({ entries }) : g.emptyLayout);
    return g.add({ pl: g.device.createPipelineLayout({ bindGroupLayouts: bgls }), bgls });
  },

  uewg_render_pipeline_create: function (jsonPtr) {
    const g = Module.uewg;
    const d = JSON.parse(UTF8ToString(jsonPtr >>> 0));
    if (Module.uewgDebug) console.log("[webgpu] pipeline " + UTF8ToString(jsonPtr >>> 0));
    g.hitch.pipe++;
    if (window.uewLogCreates) console.log(`[create] pipeline vs ${d.vs} ps ${d.ps}`);
    const desc = {
      layout: g.o[d.layout].pl,
      vertex: { module: g.o[d.vs], entryPoint: "main", buffers: d.buffers },
      fragment: d.ps ? { module: g.o[d.ps], entryPoint: "main", targets: d.targets } : undefined,
      primitive: d.primitive,
      depthStencil: d.depth,
      multisample: { mask: d.sampleMask >>> 0 },
    };
    const done = p => { if (g.capFrame) p.__d = d; p.__vs = d.vs; p.__ps = d.ps; return p; };
    if (g.asyncShaders()) {
      // compiled on the browser's worker threads; draws that need it are skipped until it is ready
      const id = g.add(null);
      g.pendingPipes++;
      g.pending.add(id);
      g.device.createRenderPipelineAsync(desc).then(p => { g.o[id] = done(p); g.pendingPipes--; g.pending.delete(id); Module.uewWake && Module.uewWake(); },
        e => { g.pendingPipes--; g.pending.delete(id); Module.uewWake && Module.uewWake(); if (g.errors++ < 40) console.log("[webgpu] pipeline " + e.message); });
      return id;
    }
    try { return g.add(done(g.device.createRenderPipeline(desc))); }
    catch (e) { if (g.errors++ < 40) console.log("[webgpu] pipeline " + e.message); return 0; }
  },

  // 1 when the pipeline can be used (or failed), 0 while it is still compiling and should be waited for
  uewg_pipe_ready: function (id) { const g = Module.uewg; return g.skipPending || !g.pending.has(id) ? 1 : 0; },

  uewg_compute_pipeline_create: function (layoutId, cs) {
    const g = Module.uewg;
    const desc = { layout: g.o[layoutId].pl, compute: { module: g.o[cs], entryPoint: "main" } };
    const done = p => { p.__cs = cs; p.__ps = cs; return p; };
    if (g.asyncShaders()) {
      const id = g.add(null);
      g.pendingPipes++;
      g.pending.add(id);
      g.device.createComputePipelineAsync(desc).then(p => { g.o[id] = done(p); g.pendingPipes--; g.pending.delete(id); Module.uewWake && Module.uewWake(); },
        e => { g.pendingPipes--; g.pending.delete(id); Module.uewWake && Module.uewWake(); if (g.errors++ < 40) console.log("[webgpu] compute pipeline " + e.message); });
      return id;
    }
    try { return g.add(done(g.device.createComputePipeline(desc))); }
    catch (e) { if (g.errors++ < 40) console.log("[webgpu] compute pipeline " + e.message); return 0; }
  },

  uewg_bindgroup_create: function (layoutId, group, jsonPtr) {
    const g = Module.uewg;
    if (window.uewLogCreates > 1) console.log(`[create] bindgroup`);
    g.stat.bg++;
    const layout = layoutId < 0 ? g.emptyLayout : g.o[layoutId].bgls[group];
    const entries = JSON.parse(UTF8ToString(jsonPtr >>> 0)).map(e => {
      if (e.ring) return { binding: e.b, resource: { buffer: g.ring, offset: e.off || 0, size: e.size } };
      if (e.buf !== undefined) return { binding: e.b, resource: { buffer: g.o[e.buf], offset: e.off || 0, size: e.size } };
      if (e.dummy) { const x = g.dummy(e.dummy); return { binding: e.b, resource: x instanceof GPUBuffer ? { buffer: x } : x }; }
      return { binding: e.b, resource: g.o[e.id] };
    });
    try {
      const bg = g.device.createBindGroup({ layout, entries });
      if (g.capFrame) bg.__sz = JSON.parse(UTF8ToString(jsonPtr >>> 0)).filter(e => e.ring).map(e => [e.b, e.off || -1, e.size]);
      if (g.capFrame) bg.__r = entries.map(e => e.resource && e.resource.__tex ? e.resource.__tex.__id : e.resource && e.resource.buffer ? "b" : e.resource instanceof GPUSampler ? "s" : "?");
      return g.add(bg);
    }
    catch (e) { if (g.errors++ < 40) console.log("[webgpu] bind group " + e.message); return 0; }
  },

  // like release, but the id is never handed out again (cached bind groups may still name it)
  uewg_retire: function (id) {
    const g = Module.uewg, x = g.o[id];
    if (!x) return;
    if (x instanceof GPUBuffer || x instanceof GPUTexture) x.destroy();
    g.o[id] = null;
    if (g.pst) g.pst.pass = null;
    if (g.rbl && g.rbl.size) for (const k of g.rbl.keys()) if (k.startsWith(id + ":")) g.rbl.delete(k);
  },

  uewg_release: function (id) {
    const g = Module.uewg, x = g.o[id];
    if (!x) return;
    if (x instanceof GPUBuffer || x instanceof GPUTexture) x.destroy();
    g.o[id] = null;
    if (g.pst) g.pst.pass = null;
    g.free.push(id);
  },

  uewg_begin_pass: function (jsonPtr) {
    const g = Module.uewg;
    g.passBudget();
    g.endPass(true);
    const d = JSON.parse(UTF8ToString(jsonPtr >>> 0));
    const colorAttachments = d.color.map(c => c ? { view: g.o[c.view], loadOp: "load", storeOp: "store", depthSlice: c.slice >= 0 ? c.slice : undefined } : null);
    let depthStencilAttachment;
    if (d.depth) {
      const z = d.depth;
      depthStencilAttachment = { view: g.o[z.view] };
      if (z.ro & 1) depthStencilAttachment.depthReadOnly = true;
      else Object.assign(depthStencilAttachment, { depthLoadOp: "load", depthStoreOp: "store" });
      if (z.stencil) {
        if (z.ro & 2) depthStencilAttachment.stencilReadOnly = true;
        else Object.assign(depthStencilAttachment, { stencilLoadOp: "load", stencilStoreOp: "store" });
      }
    }
    // the reopen descriptor always loads; the first opening takes over matching pending clears
    g.lastPass = { colorAttachments, depthStencilAttachment };
    let first = g.lastPass;
    if (g.pendClears.length) {
      const ca = colorAttachments.map(a => a && { ...a }), za = depthStencilAttachment && { ...depthStencilAttachment };
      for (const pc of g.pendClears) {
        for (const c of pc.color) {
          if (!c) continue;
          const k = ca.findIndex(a => a && a.view === c.view && (a.depthSlice ?? -1) === (c.slice >= 0 ? c.slice : -1));
          if (k >= 0) { ca[k].loadOp = "clear"; ca[k].clearValue = c.clear; c.view = null; }
        }
        if (pc.depth && za && za.view === pc.depth.view && !za.depthReadOnly && !(pc.depth.stencil && pc.depth.sclear !== undefined && za.stencilReadOnly)) {
          if (pc.depth.dclear !== undefined) { za.depthLoadOp = "clear"; za.depthClearValue = pc.depth.dclear; }
          if (pc.depth.sclear !== undefined && za.stencilLoadOp) { za.stencilLoadOp = "clear"; za.stencilClearValue = pc.depth.sclear; }
          pc.depth = null;
        }
        pc.color = pc.color.filter(c => c && c.view);
      }
      g.pendClears = g.pendClears.filter(pc => pc.color.length || pc.depth);
      g.flushClears();
      first = { colorAttachments: ca, depthStencilAttachment: za };
    }
    g.stat.pass++;
    g.pass = g.encoder().beginRenderPass(first);
    if (g.cap) g.cap.cur = { pass: g.cap.events.length, desc: g.lastPass, draws: [] };
  },

  uewg_clear: function (jsonPtr, ringUsed) {
    const g = Module.uewg;
    g.endPass(true);
    const d = JSON.parse(UTF8ToString(jsonPtr >>> 0));
    g.capEvent({ clear: JSON.parse(JSON.stringify(d)) });
    for (const c of d.color) c.view = g.o[c.view];
    if (d.depth) d.depth.view = g.o[d.depth.view];
    // a later clear of the same target replaces an earlier one
    for (const pc of g.pendClears) {
      pc.color = pc.color.filter(c => !d.color.some(n => n.view === c.view && n.slice === c.slice));
      if (pc.depth && d.depth && pc.depth.view === d.depth.view && d.depth.dclear !== undefined && (d.depth.sclear !== undefined || !pc.depth.stencil)) pc.depth = null;
    }
    g.pendClears = g.pendClears.filter(pc => pc.color.length || pc.depth);
    g.pendClears.push(d);
  },

  uewg_draw: function (p) { Module.uewg.guard("draw", () => {
    const g = Module.uewg;
    // a copy or dispatch since the last draw ended the pass: carry on with the same targets
    if (!g.pass && g.lastPass) { g.stat.reopen++; g.openPass(); }
    const pass = g.pass;
    if (!pass) { g.skip.nopass++; return; }
    const I = HEAP32, F = HEAPF32;
    let i = p >>> 2;
    const pipeId = I[i++], pipe = g.o[pipeId];
    if (!pipe) { g.skip.nopipe++; return; }
    // state already set in this pass is not set again
    let st = g.pst;
    if (!st || st.pass !== pass) {
      st = g.pst = { pass, pipe: -1, bg: new Int32Array(4).fill(-1), off: new Uint32Array(32), nd: new Int32Array(4), vb: new Int32Array(48).fill(-2),
        vp: new Float32Array(6).fill(NaN), sc: new Int32Array(4).fill(-1), sref: -1, bc: new Float32Array(4).fill(NaN), ib: new Int32Array(3).fill(-2) };
    }
    if (st.pipe !== pipeId) { pass.setPipeline(pipe); st.pipe = pipeId; }
    if (g.prof && g.prof.cur) { const c = g.prof.cur; c.draws++; c.pipes.set(pipeId, (c.pipes.get(pipeId) || 0) + 1); }
    const ngroups = I[i++];
    // a group whose object is gone would invalidate the whole frame's commands: drop just this draw
    for (let k = 0, j = i; k < ngroups; k++) {
      if (!g.o[I[j]]) {
        g.skip.nobg = (g.skip.nobg || 0) + 1;
        if ((g.nobgLogged = (g.nobgLogged || 0) + 1) <= 5) console.log(`[webgpu] draw skipped: bind group ${k} (id ${I[j]}) is missing, pipeline ${pipeId}`);
        return;
      }
      j += 2 + I[j + 1];
    }
    for (let k = 0; k < ngroups; k++) {
      const bg = I[i++], ndyn = I[i++];
      let same = st.bg[k] === bg && st.nd[k] === ndyn;
      for (let d = 0; d < ndyn && same; d++) if (st.off[k * 8 + d] !== HEAPU32[i + d]) same = false;
      if (!same) {
        // offsets straight from the heap: no array per call
        pass.setBindGroup(k, g.o[bg], HEAPU32, i, ndyn);
        st.bg[k] = bg; st.nd[k] = ndyn;
        for (let d = 0; d < ndyn && d < 8; d++) st.off[k * 8 + d] = HEAPU32[i + d];
      }
      i += ndyn;
    }
    const nvb = I[i++];
    for (let k = 0; k < nvb; k++) {
      const slot = I[i], id = I[i + 1], off = I[i + 2], o = slot * 3;
      if (st.vb[o] !== id || st.vb[o + 1] !== off) { pass.setVertexBuffer(slot, id < 0 ? g.zeroVB() : g.o[id], off); st.vb[o] = id; st.vb[o + 1] = off; }
      i += 3;
    }
    if (st.vp[0] !== F[i] || st.vp[1] !== F[i + 1] || st.vp[2] !== F[i + 2] || st.vp[3] !== F[i + 3] || st.vp[4] !== F[i + 4] || st.vp[5] !== F[i + 5]) {
      pass.setViewport(F[i], F[i + 1], F[i + 2], F[i + 3], F[i + 4], F[i + 5]);
      st.vp.set(F.subarray(i, i + 6));
    }
    i += 6;
    if (st.sc[0] !== I[i] || st.sc[1] !== I[i + 1] || st.sc[2] !== I[i + 2] || st.sc[3] !== I[i + 3]) {
      pass.setScissorRect(I[i], I[i + 1], I[i + 2], I[i + 3]);
      st.sc.set(I.subarray(i, i + 4));
    }
    i += 4;
    const sref = I[i] >>> 0;
    if (st.sref !== sref) { pass.setStencilReference(sref); st.sref = sref; }
    i++;
    if (st.bc[0] !== F[i] || st.bc[1] !== F[i + 1] || st.bc[2] !== F[i + 2] || st.bc[3] !== F[i + 3]) {
      pass.setBlendConstant([F[i], F[i + 1], F[i + 2], F[i + 3]]);
      st.bc.set(F.subarray(i, i + 4));
    }
    i += 4;
    const indexed = I[i++], count = I[i++], instances = I[i++], first = I[i++], baseVertex = I[i++], firstInstance = I[i++];
    // count -1: indirect, with the arguments buffer and offset in place of instances and first
    if (indexed) {
      if (st.ib[0] !== I[i] || st.ib[1] !== I[i + 1] || st.ib[2] !== I[i + 2]) {
        pass.setIndexBuffer(g.o[I[i]], I[i + 1] ? "uint32" : "uint16", I[i + 2]);
        st.ib.set(I.subarray(i, i + 3));
      }
      if (count === -1) pass.drawIndexedIndirect(g.o[instances], first);
      else pass.drawIndexed(count, instances, first, baseVertex, firstInstance);
      g.stat.draws++;
    } else if (count === -1) pass.drawIndirect(g.o[instances], first);
    else pass.draw(count, instances, first, firstInstance);
    if (g.cap && g.cap.cur && g.cap.cur.draws.length < 3000) {
      const pd = pipe.__d || {};
      const groups = [], cbs = {};
      for (let k = 0, j = (p >>> 2) + 2; k < ngroups; k++) {
        const bg = g.o[I[j]], nd = I[j + 1];
        groups.push(bg && bg.__r);
        // constant buffer contents straight from the ring (first draws of a pass only)
        if (bg && bg.__sz && g.cap.cur.draws.length < 8) {
          let d = 0;
          for (const [b, off, size] of bg.__sz) {
            const o = off >= 0 ? off : HEAPU32[j + 2 + d++];
            cbs[`${k}:${b}`] = Array.from(HEAPF32.subarray((g.ringPtr + o) >>> 2, (g.ringPtr + o + size) >>> 2)).map(x => +x.toPrecision(5));
          }
        }
        j += 2 + nd;
      }
      g.cap.cur.draws.push({ vs: pd.vs, ps: pd.ps, count, instances, indexed, first, baseVertex, topo: pd.primitive && pd.primitive.topology, targets: pd.targets, depth: pd.depth, buffers: pd.buffers, prim: pd.primitive, vp: Array.from(st.vp), sc: Array.from(st.sc), sref, res: groups, cbs });
      // debug: window.uewCapturePixels = [[x, y], ...] samples depth and colour target 0 there after every draw
      if (window.uewCapturePixels && g.cap.px.length < 400) {
        pass.end();
        const enc = g.encoder(), atts = [];
        const z = g.lastPass.depthStencilAttachment, zt = z && z.view.__tex;
        if (zt && zt.format === "depth32float-stencil8") atts.push(["z", zt, "depth-only", 4]);
        const c0 = g.lastPass.colorAttachments[0], ct = c0 && c0.view.__tex;
        if (ct && ct.format === "rgba16float" && ct.dimension === "2d") atts.push(["c", ct, "all", 8]);
        for (const [kind, t, aspect, bpp] of atts) {
          // depth can only be copied whole
          const bpr = (t.width * bpp + 255) & ~255;
          const buf = g.device.createBuffer({ size: bpr * t.height, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
          enc.copyTextureToBuffer({ texture: t, aspect }, { buffer: buf, bytesPerRow: bpr, rowsPerImage: t.height }, [t.width, t.height, 1]);
          g.cap.px.push({ buf, kind, bpr, bpp, w: t.width, h: t.height, pass: g.cap.cur.pass, draw: g.cap.cur.draws.length - 1, ps: pd.ps, vs: pd.vs, n: count, first });
        }
        g.pass = g.encoder().beginRenderPass(g.lastPass);
      }
      // debug: window.uewCapturePerDraw reads colour target 0 back after every draw of the frame
      const a0 = window.uewCapturePerDraw && g.lastPass.colorAttachments[0], t0 = a0 && a0.view.__tex;
      if (t0 && t0.format === "rgba16float" && t0.dimension === "2d" && g.cap.perDraw.length < 200) {
        g.pass.end();
        const w = t0.width, h = t0.height, bpr = (w * 8 + 255) & ~255;
        const buf = g.device.createBuffer({ size: bpr * h, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
        g.encoder().copyTextureToBuffer({ texture: t0 }, { buffer: buf, bytesPerRow: bpr, rowsPerImage: h }, [w, h, 1]);
        g.cap.perDraw.push({ buf, w, h, bpr, pass: g.cap.cur.pass, draw: g.cap.cur.draws.length - 1, ps: pd.ps, tex: t0.__id });
        g.pass = g.encoder().beginRenderPass(g.lastPass);
      }
    }
    g.draws++;
  }); },

  uewg_dispatch: function (p) { Module.uewg.guard("dispatch", () => {
    const g = Module.uewg, I = HEAP32;
    let i = p >>> 2;
    const pipe = g.o[I[i++]];
    const cb = I[i++], ndyn = I[i++];
    const dyn0 = i;
    i += ndyn;
    const res = I[i++], x = I[i++], y = I[i++], z = I[i++];
    if (!pipe) return;
    g.passBudget();
    g.endPass();
    const cp = g.encoder().beginComputePass();
    cp.setPipeline(pipe);
    if (g.prof && g.prof.cur) { g.prof.cur.draws++; g.prof.cur.pipes.set(I[p >>> 2], 1); }
    cp.setBindGroup(0, g.o[cb], HEAPU32, dyn0, ndyn);
    cp.setBindGroup(1, g.o[res]);
    if (x === -1) cp.dispatchWorkgroupsIndirect(g.o[y], z);
    else cp.dispatchWorkgroups(x, y, z);
    cp.end();
    if (g.cap) g.capEvent({ dispatch: [x, y, z], cs: pipe.__cs, res: g.o[res] && g.o[res].__r });
  }); },

  // geometry shader emulation: compute pass that buckets triangles per layer, then one indirect draw per layer
  uewg_gs: function (p) { Module.uewg.guard("gs", () => {
    const g = Module.uewg, I = HEAP32, F = HEAPF32;
    let i = p >>> 2;
    const cpipe = g.o[I[i++]], wx = I[i++], wy = I[i++], nl = I[i++], cap = I[i++], args = g.o[I[i++]];
    const groups = () => { const r = []; for (let k = 0; k < 4; k++) { const bg = I[i++], nd = I[i++]; r.push([g.o[bg], i, nd]); i += nd; } return r; };
    const cg = groups();
    const rpipe = g.o[I[i++]];
    const rg = groups();
    const vp = [F[i], F[i + 1], F[i + 2], F[i + 3], F[i + 4], F[i + 5]]; i += 6;
    const sc = [I[i], I[i + 1], I[i + 2], I[i + 3]]; i += 4;
    const sref = I[i++] >>> 0, bc = [F[i], F[i + 1], F[i + 2], F[i + 3]]; i += 4;
    const ncol = I[i++], hasDepth = I[i++], stencil = I[i++], ro = I[i++];
    if (!cpipe || !rpipe || !args) { g.skip.nopipe++; return; }
    g.endPass();
    const enc = g.encoder();
    enc.clearBuffer(args, 0, nl * 16);
    if (!g.gsFix) {
      const m = g.device.createShaderModule({ code: `@group(0) @binding(0) var<storage, read_write> a: array<u32>; @group(0) @binding(1) var<uniform> q: vec4<u32>;
        @compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3<u32>) { let l = id.x; if (l >= q.y) { return; }
        a[l * 4u] = min(a[l * 4u], q.x); a[l * 4u + 1u] = 1u; a[l * 4u + 2u] = l * q.x; a[l * 4u + 3u] = 0u; }` });
      g.gsFix = g.device.createComputePipeline({ layout: "auto", compute: { module: m, entryPoint: "main" } });
      g.gsFixUB = new Map();
    }
    const key = cap + ":" + nl;
    let ub = g.gsFixUB.get(key);
    if (!ub) { ub = g.device.createBuffer({ size: 16, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST }); g.queue.writeBuffer(ub, 0, new Uint32Array([cap, nl, 0, 0])); g.gsFixUB.set(key, ub); }
    const cp = enc.beginComputePass();
    cp.setPipeline(cpipe);
    for (let k = 0; k < 4; k++) cp.setBindGroup(k, cg[k][0], HEAPU32, cg[k][1], cg[k][2]);
    cp.dispatchWorkgroups(wx, wy);
    cp.setPipeline(g.gsFix);
    cp.setBindGroup(0, g.device.createBindGroup({ layout: g.gsFix.getBindGroupLayout(0), entries: [{ binding: 0, resource: { buffer: args } }, { binding: 1, resource: { buffer: ub } }] }));
    cp.dispatchWorkgroups(Math.ceil(nl / 64));
    cp.end();
    for (let L = 0; L < nl; L++) {
      const colorAttachments = [];
      for (let c = 0; c < ncol; c++) { const v = I[i++], sl = I[i++]; colorAttachments.push(v ? { view: g.o[v], loadOp: "load", storeOp: "store", depthSlice: sl >= 0 ? sl : undefined } : null); }
      let depthStencilAttachment;
      if (hasDepth) {
        depthStencilAttachment = { view: g.o[I[i++]] };
        if (ro & 1) depthStencilAttachment.depthReadOnly = true; else Object.assign(depthStencilAttachment, { depthLoadOp: "load", depthStoreOp: "store" });
        if (stencil) { if (ro & 2) depthStencilAttachment.stencilReadOnly = true; else Object.assign(depthStencilAttachment, { stencilLoadOp: "load", stencilStoreOp: "store" }); }
      }
      const pass = enc.beginRenderPass({ colorAttachments, depthStencilAttachment });
      pass.setPipeline(rpipe);
      for (let k = 0; k < 4; k++) pass.setBindGroup(k, rg[k][0], HEAPU32, rg[k][1], rg[k][2]);
      pass.setViewport(vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
      pass.setScissorRect(sc[0], sc[1], sc[2], sc[3]);
      pass.setStencilReference(sref);
      pass.setBlendConstant(bc);
      pass.drawIndirect(args, L * 16);
      pass.end();
    }
    g.capEvent({ gs: nl, cap });
    g.draws++;
  }); },

  uewg_copy_texture: function (src, smip, slayer, sx, sy, sz, dst, dmip, dlayer, dx, dy, dz, w, h, d) {
    const g = Module.uewg;
    g.endPass();
    if (g.o[src] && /^(bc|etc2|eac|astc)/.test(g.o[src].format)) { w = (w + 3) & ~3; h = (h + 3) & ~3; }
    try {
      g.encoder().copyTextureToTexture({ texture: g.o[src], mipLevel: smip, origin: [sx, sy, sz + slayer] },
        { texture: g.o[dst], mipLevel: dmip, origin: [dx, dy, dz + dlayer] }, [w, h, d]);
      g.capEvent({ copy: [src, smip, slayer, dst, dmip, dlayer, w, h, d] });
    } catch (e) { if (g.errors++ < 40) console.log("[webgpu] copy " + e.message); }
  },

  uewg_clear_buffer: function (id, off, size) {
    const g = Module.uewg, b = g.o[id];
    if (!b) return;
    g.endPass();
    const n = Math.min(size, b.size - off) & ~3;
    if (n > 0) g.encoder().clearBuffer(b, off, n);
  },

  // GenerateMips: each level drawn from the one above with a linear filter
  uewg_gen_mips: function (id) { Module.uewg.guard("mips", () => {
    const g = Module.uewg, t = g.o[id];
    if (!(t instanceof GPUTexture) || t.mipLevelCount < 2 || t.dimension !== "2d" || /^(bc|depth|stencil)|int$/.test(t.format)) return;
    g.endPass();
    if (!g.mipMod) {
      g.mipMod = g.device.createShaderModule({ code: `@group(0) @binding(0) var t: texture_2d<f32>; @group(0) @binding(1) var s: sampler;
        struct V { @builtin(position) p: vec4<f32>, @location(0) uv: vec2<f32> };
        @vertex fn vs(@builtin(vertex_index) i: u32) -> V { let uv = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u)); var o: V; o.p = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0); o.uv = uv; return o; }
        @fragment fn fs(v: V) -> @location(0) vec4<f32> { return textureSampleLevel(t, s, v.uv, 0.0); }` });
      g.mipSampler = g.device.createSampler({ magFilter: "linear", minFilter: "linear" });
      g.mipPipes = new Map();
    }
    let pipe = g.mipPipes.get(t.format);
    if (!pipe) {
      pipe = g.device.createRenderPipeline({ layout: "auto", vertex: { module: g.mipMod, entryPoint: "vs" }, fragment: { module: g.mipMod, entryPoint: "fs", targets: [{ format: t.format }] } });
      g.mipPipes.set(t.format, pipe);
    }
    const enc = g.encoder();
    for (let layer = 0; layer < t.depthOrArrayLayers; layer++) for (let m = 1; m < t.mipLevelCount; m++) {
      const view = (k) => t.createView({ dimension: "2d", baseMipLevel: k, mipLevelCount: 1, baseArrayLayer: layer, arrayLayerCount: 1 });
      const bg = g.device.createBindGroup({ layout: pipe.getBindGroupLayout(0), entries: [{ binding: 0, resource: view(m - 1) }, { binding: 1, resource: g.mipSampler }] });
      const p = enc.beginRenderPass({ colorAttachments: [{ view: view(m), loadOp: "clear", storeOp: "store", clearValue: [0, 0, 0, 0] }] });
      p.setPipeline(pipe);
      p.setBindGroup(0, bg);
      p.draw(3);
      p.end();
    }
  }); },

  uewg_readback__async: true,
  uewg_readback: async function (kind, id, mip, layer, w, h, d, ptr, pitch, slice, rowbytes, rows) {
    ptr >>>= 0;
    const g = Module.uewg, src = g.o[id];
    if (!src) return;
    g.flush(0);
    const bpr = kind ? (rowbytes + 255) & ~255 : 0, size = kind ? bpr * rows * d : (w + 3) & ~3;
    const tmp = g.device.createBuffer({ size: Math.max(4, size), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const enc = g.device.createCommandEncoder();
    try {
      if (kind) enc.copyTextureToBuffer({ texture: src, mipLevel: mip, origin: [0, 0, layer] }, { buffer: tmp, bytesPerRow: bpr, rowsPerImage: rows }, [w, h, d]);
      else enc.copyBufferToBuffer(src, 0, tmp, 0, Math.min(size, src.size));
      g.queue.submit([enc.finish()]);
      await tmp.mapAsync(GPUMapMode.READ);
    } catch (e) { if (g.errors++ < 40) console.log("[webgpu] readback " + e.message); tmp.destroy(); return; }
    const data = new Uint8Array(tmp.getMappedRange());
    // the heap may have grown while we waited
    if (!kind) HEAPU8.set(data.subarray(0, w), ptr);
    else for (let z = 0; z < d; z++) for (let y = 0; y < rows; y++)
      HEAPU8.set(data.subarray((z * rows + y) * bpr, (z * rows + y) * bpr + Math.min(rowbytes, pitch)), ptr + z * slice + y * pitch);
    tmp.unmap();
    tmp.destroy();
  },

  // the same copy without suspending: start, poll done, finish (green threads keep running meanwhile)
  uewg_readback_start: function (kind, id, mip, layer, w, h, d, ptr, pitch, slice, rowbytes, rows) {
    const g = Module.uewg, src = g.o[id];
    if (!src) return 0;
    g.flush(0);
    const bpr = kind ? (rowbytes + 255) & ~255 : 0, size = kind ? bpr * rows * d : (w + 3) & ~3;
    const tmp = g.device.createBuffer({ size: Math.max(4, size), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    const enc = g.device.createCommandEncoder();
    try {
      if (kind) enc.copyTextureToBuffer({ texture: src, mipLevel: mip, origin: [0, 0, layer] }, { buffer: tmp, bytesPerRow: bpr, rowsPerImage: rows }, [w, h, d]);
      else enc.copyBufferToBuffer(src, 0, tmp, 0, Math.min(size, src.size));
      g.queue.submit([enc.finish()]);
    } catch (e) { if (g.errors++ < 40) console.log("[webgpu] readback " + e.message); tmp.destroy(); return 0; }
    g.hitch.rb++;
    const t = { kind, w, d, rows, bpr, rowbytes, ptr: ptr >>> 0, pitch, slice, tmp, done: false, ok: false };
    const t0 = performance.now();
    tmp.mapAsync(GPUMapMode.READ).then(() => {
      t.ok = true; t.done = true;
      Module.uewWake && Module.uewWake();
      const ms = performance.now() - t0;
      if (ms > 50 && (g.rbSlow = (g.rbSlow || 0) + 1) < 60)
        console.log(`[readback] ${kind ? "texture" : "buffer"} ${w}x${h}x${d} (${size} bytes, ${src.format || ""}) took ${ms.toFixed(0)} ms`);
    },
      e => { if (g.errors++ < 40) console.log("[webgpu] readback " + e.message); t.done = true; });
    g.rb = g.rb || new Map();
    g.rbN = (g.rbN || 0) + 1;
    g.rb.set(g.rbN, t);
    return g.rbN;
  },
  // small reads the game repeats (probes, picking): hand back the newest finished copy at once and start the next one,
  // so the render thread never waits on the browser's map latency (2-3 frames). 0 = nothing finished yet: wait as usual.
  uewg_readback_latest: function (kind, id, mip, layer, w, h, d, ptr, pitch, slice, rowbytes, rows) {
    const g = Module.uewg, src = g.o[id];
    if (!src || (typeof location !== "undefined" && /[?&]syncreadback=1/.test(location.search))) return 0;
    g.rbl = g.rbl || new Map();
    const k = id + ":" + mip + ":" + layer;
    let e = g.rbl.get(k);
    if (!e) { e = { busy: false, data: null }; g.rbl.set(k, e); }
    let got = 0;
    if (e.data) {
      ptr >>>= 0;
      const bpr = kind ? (rowbytes + 255) & ~255 : 0;
      if (!kind) HEAPU8.set(e.data.subarray(0, w), ptr);
      else for (let z = 0; z < d; z++) for (let y = 0; y < rows; y++)
        HEAPU8.set(e.data.subarray((z * rows + y) * bpr, (z * rows + y) * bpr + Math.min(rowbytes, pitch)), ptr + z * slice + y * pitch);
      got = 1;
    }
    if (!e.busy) {
      g.flush(0);
      const bpr = kind ? (rowbytes + 255) & ~255 : 0, size = kind ? bpr * rows * d : (w + 3) & ~3;
      const tmp = g.device.createBuffer({ size: Math.max(4, size), usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
      const enc = g.device.createCommandEncoder();
      try {
        if (kind) enc.copyTextureToBuffer({ texture: src, mipLevel: mip, origin: [0, 0, layer] }, { buffer: tmp, bytesPerRow: bpr, rowsPerImage: rows }, [w, h, d]);
        else enc.copyBufferToBuffer(src, 0, tmp, 0, Math.min(size, src.size));
        g.queue.submit([enc.finish()]);
      } catch (err) { tmp.destroy(); return got; }
      e.busy = true;
      tmp.mapAsync(GPUMapMode.READ).then(() => { e.data = new Uint8Array(tmp.getMappedRange()).slice(); tmp.unmap(); tmp.destroy(); e.busy = false; },
        () => { tmp.destroy(); e.busy = false; });
    }
    return got;
  },

  uewg_readback_done: function (n) { const t = Module.uewg.rb && Module.uewg.rb.get(n); return !t || t.done ? 1 : 0; },
  uewg_readback_finish: function (n) {
    const g = Module.uewg, t = g.rb && g.rb.get(n);
    if (!t) return;
    g.rb.delete(n);
    if (t.ok) {
      const data = new Uint8Array(t.tmp.getMappedRange());
      if (!t.kind) HEAPU8.set(data.subarray(0, t.w), t.ptr);
      else for (let z = 0; z < t.d; z++) for (let y = 0; y < t.rows; y++)
        HEAPU8.set(data.subarray((z * t.rows + y) * t.bpr, (z * t.rows + y) * t.bpr + Math.min(t.rowbytes, t.pitch)), t.ptr + z * t.slice + y * t.pitch);
      t.tmp.unmap();
    }
    t.tmp.destroy();
  },

  uewg_copy_buffer: function (src, soff, dst, doff, size) {
    const g = Module.uewg, s = g.o[src], d = g.o[dst];
    if (!s || !d) return;
    g.endPass();
    const n = Math.min((size + 3) & ~3, s.size - soff, d.size - doff);
    if (n > 0) g.encoder().copyBufferToBuffer(s, soff, d, doff, n);
  },

  uewg_present: function (view, ringUsed) {
    const g = Module.uewg;
    Module.uewPresents = (Module.uewPresents || 0) + 1;
    if (g.prof) g.prof.frameEnd();
    // long frames: what happened in them (new shaders and pipelines, file downloads)
    const now = performance.now(), h = g.hitch, F = Module.uewFiles || {};
    if (h.last && now - h.last > 100 && g.frames > 60 && h.logged < 300) {
      h.logged++;
      console.log(`[hitch] ${(now - h.last).toFixed(0)} ms frame: ${h.sh + h.psh} new shaders, ${h.pipe + h.ppipe} new pipelines (this frame and the one before), ${h.rb} GPU readbacks, ` +
        `${(((F.fetched || 0) - h.bytes) / 1048576).toFixed(1)} MB in ${(F.requests || 0) - h.req} downloads (${((F.fetchMs || 0) - h.fms).toFixed(0)} ms waiting)`);
    }
    h.last = now; h.psh = h.sh; h.ppipe = h.pipe; h.sh = 0; h.pipe = 0; h.rb = 0; h.bytes = F.fetched || 0; h.req = F.requests || 0; h.fms = F.fetchMs || 0;
    g.endPass();
    if (!g.blit) {
      const code = `@group(0) @binding(0) var t: texture_2d<f32>; @group(0) @binding(1) var s: sampler;
        struct V { @builtin(position) p: vec4<f32>, @location(0) uv: vec2<f32> };
        @vertex fn vs(@builtin(vertex_index) i: u32) -> V { let uv = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u)); var o: V; o.p = vec4<f32>(uv * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0); o.uv = uv; return o; }
        @fragment fn fs(v: V) -> @location(0) vec4<f32> { return vec4<f32>(textureSampleLevel(t, s, v.uv, 0.0).rgb, 1.0); }`;
      const m = g.device.createShaderModule({ code });
      g.blit = g.device.createRenderPipeline({ layout: "auto", vertex: { module: m, entryPoint: "vs" }, fragment: { module: m, entryPoint: "fs", targets: [{ format: g.canvasFormat }] }, primitive: { topology: "triangle-list" } });
      g.blitSampler = g.device.createSampler({ magFilter: "linear", minFilter: "linear" });
      g.blitGroups = new Map();
    }
    const src = g.o[view];
    if (g.cap) g.capEvent({ present: view, tex: src && src.__tex && src.__tex.__id });
    if (src) {
      let bg = g.blitGroups.get(src);
      if (!bg) { bg = g.device.createBindGroup({ layout: g.blit.getBindGroupLayout(0), entries: [{ binding: 0, resource: src }, { binding: 1, resource: g.blitSampler }] }); g.blitGroups.set(src, bg); }
      const p = g.encoder().beginRenderPass({ colorAttachments: [{ view: g.ctx.getCurrentTexture().createView(), loadOp: "clear", storeOp: "store", clearValue: [0, 0, 0, 1] }] });
      p.setPipeline(g.blit);
      p.setBindGroup(0, bg);
      p.draw(3);
      p.end();
    }
    g.flush(ringUsed);
    g.lastPass = null;
    // GPU side of the frame: how long until the queue finished it, and what it was given
    {
      const h = g.hitch, t0 = performance.now(), info = `${h.tex} textures made (${(h.texPx / 1e6).toFixed(1)} Mpx), ${(h.upBytes / 1048576).toFixed(1)} MB uploaded`;
      h.tex = 0; h.texPx = 0; h.upBytes = 0;
      g.queue.onSubmittedWorkDone().then(() => {
        const ms = performance.now() - t0;
        if (ms > 150 && (h.gpuLogged = (h.gpuLogged || 0) + 1) < 100) console.log(`[hitch] GPU took ${ms.toFixed(0)} ms for a frame: ${info}`);
      });
    }
    g.frames++;
    if (g.capFrame && window.uewCaptureFrame) g.capFrame = window.uewCaptureFrame | 0;
    if (g.cap && g.frames === g.capFrame) { const c = g.cap; g.cap = null; g.capFinish(c); }
    if (g.capFrame && g.frames === g.capFrame - 1) g.cap = { events: [], reads: [], cur: null, perDraw: [], px: [] };
    if (g.frames === 1 || g.frames % 600 === 0) {
      const s = g.skip;
      console.log(`[webgpu] frame ${g.frames} (${g.draws} draws so far; skipped: ${s.nopass} no pass, ${s.nopipe} no pipeline, ${s.threw} threw${s.last ? " — " + s.last : ""})`);
      g.skip = { nopass: 0, nopipe: 0, threw: 0, last: "" };
      const top = [...g.errKinds].sort((a, b) => b[1] - a[1]).slice(0, 6);
      for (const [k, n] of top) console.log(`[webgpu]   ${n} x ${k}`);
      g.errKinds.clear();
      const st = g.stat, f = g.frames === 1 ? 1 : 600;
      if (Module.uewgStats || (typeof window != "undefined" && window.uewStats))
        if (st.by) console.log("[webgpu] biggest writers (id: bytes/frame, buffer size, last offset): " + [...st.by].sort((a, b) => b[1][0] - a[1][0]).slice(0, 5).map(([k, v]) => `${k}: ${(v[0] / f / 1024).toFixed(0)} KiB of ${(v[1] / 1024).toFixed(0)} KiB @${v[2]}`).join(", "));
        console.log(`[webgpu] per frame: ${(st.pass / f).toFixed(1)} passes (${(st.reopen / f).toFixed(1)} reopened), ${(st.write / f).toFixed(1)} buffer writes (${(st.writeBytes / f / 1024).toFixed(0)} KiB), ${(st.ringCopy / f).toFixed(1)} ring copies, ${(st.texWrite / f).toFixed(1)} texture uploads, ${(st.bg / f).toFixed(1)} bind groups made, ${(st.draws / f).toFixed(0)} draws`);
      g.stat = { pass: 0, reopen: 0, write: 0, writeBytes: 0, ringCopy: 0, texWrite: 0, bg: 0, draws: 0, by: (Module.uewgStats || (typeof window != "undefined" && window.uewStats)) ? new Map() : null };
    }
  },

  // the C side starts the ring over after this one
  uewg_flush: function (ringUsed) { Module.uewg.flush(ringUsed); Module.uewg.ringDone = 0; },
  uewg_submit: function (ringUsed) { Module.uewg.flush(ringUsed); },
  uewg_ring_used_at: function (p) { Module.uewRingUsedPtr = p >>> 0; },
  uewg_ring_reset: function () { Module.uewg.ringDone = 0; },
  // a ticket that resolves to the time the GPU finishes everything submitted so far
  uewg_timestamp: function () {
    const g = Module.uewg;
    const t = g.tsNext = (g.tsNext || 0) + 1;
    (g.ts || (g.ts = new Map())).set(t, -1);
    g.queue.onSubmittedWorkDone().then(() => { g.ts.set(t, performance.now()); Module.uewWake && Module.uewWake(); }, () => g.ts.set(t, performance.now()));
    if (g.ts.size > 4096) for (const k of g.ts.keys()) { if (g.ts.size <= 2048) break; g.ts.delete(k); }
    return t;
  },
  uewg_timestamp_get: function (t, out) {
    const g = Module.uewg, v = g.ts ? g.ts.get(t) : undefined;
    if (v === undefined) { HEAPF64[out >>> 3] = performance.now(); return 1; }
    if (v < 0) return 0;
    HEAPF64[out >>> 3] = v;
    return 1;
  },
});
