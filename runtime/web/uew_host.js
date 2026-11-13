// Browser-side functions the runtime calls (page, input, audio, files, network, video). With real threads a call
// made on a worker runs on the page's thread (__proxy: 'sync'); single-threaded builds call them directly.
addToLibrary({
  web_input_init__proxy: 'sync',
  web_input_init__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  web_input_init: function () {
    if (Module.uewInput) return;
    const q = Module.uewInput = [];
    const canvas = Module.canvas || (typeof document !== "undefined" && document.getElementById("canvas"));
    if (!canvas) return;
    const codes = { Escape: 0x1B, Tab: 0x09, Enter: 0x0D, Space: 0x20, Backspace: 0x08, ShiftLeft: 0xA0, ShiftRight: 0xA1,
        ControlLeft: 0xA2, ControlRight: 0xA3, AltLeft: 0xA4, AltRight: 0xA5, CapsLock: 0x14, ArrowLeft: 0x25, ArrowUp: 0x26,
        ArrowRight: 0x27, ArrowDown: 0x28, Insert: 0x2D, Delete: 0x2E, Home: 0x24, End: 0x23, PageUp: 0x21, PageDown: 0x22,
        Backquote: 0xC0, Minus: 0xBD, Equal: 0xBB, BracketLeft: 0xDB, BracketRight: 0xDD, Backslash: 0xDC, Semicolon: 0xBA,
        Quote: 0xDE, Comma: 0xBC, Period: 0xBE, Slash: 0xBF };
    const vk = (e) => {
        if (codes[e.code]) return codes[e.code];
        if (/^Key[A-Z]$/.test(e.code)) return e.code.charCodeAt(3);
        if (/^Digit[0-9]$/.test(e.code)) return e.code.charCodeAt(5);
        if (/^F([1-9]|1[0-2])$/.test(e.code)) return 0x6F + parseInt(e.code.slice(1));
        if (/^Numpad[0-9]$/.test(e.code)) return 0x60 + parseInt(e.code.slice(6));
        return 0;
    };
    const generic = (k) => k === 0xA0 || k === 0xA1 ? 0x10 : k === 0xA2 || k === 0xA3 ? 0x11 : k === 0xA4 || k === 0xA5 ? 0x12 : 0;
    const key = (down) => (e) => {
        const k = vk(e);
        if (!k) return;
        e.preventDefault();
        const g = generic(k);
        if (g) q.push([down ? 1 : 2, g, 0, 0, 0]);
        q.push([down ? 1 : 2, k, 0, 0, 0]);
        if (down && e.key.length === 1) q.push([3, e.key.codePointAt(0), 0, 0, 0]);
    };
    window.addEventListener("keydown", key(true));
    window.addEventListener("keyup", key(false));
    const pos = (e) => {
        const r = canvas.getBoundingClientRect();
        return [Math.round((e.clientX - r.left) * canvas.width / r.width), Math.round((e.clientY - r.top) * canvas.height / r.height)];
    };
    canvas.addEventListener("mousemove", (e) => { const [x, y] = pos(e); q.push([4, x, y, e.movementX | 0, e.movementY | 0]); });
    const btn = [0, 2, 1, 3, 4];
    canvas.addEventListener("mousedown", (e) => { e.preventDefault(); q.push([5, btn[e.button] ?? 0, 1, 0, 0]); });
    window.addEventListener("mouseup", (e) => q.push([5, btn[e.button] ?? 0, 0, 0, 0]));
    canvas.addEventListener("wheel", (e) => { e.preventDefault(); q.push([6, e.deltaY < 0 ? 1 : -1, 0, 0, 0]); }, { passive: false });
    canvas.addEventListener("contextmenu", (e) => e.preventDefault());
    window.addEventListener("focus", () => q.push([8, 1, 0, 0, 0]));
    window.addEventListener("blur", () => q.push([8, 0, 0, 0, 0]));
    // the render size is fixed by the page unless it opts into following the window
    if (Module.uewAutoSize) new ResizeObserver(() => {
        const w = Math.max(1, Math.round(canvas.clientWidth * devicePixelRatio)), h = Math.max(1, Math.round(canvas.clientHeight * devicePixelRatio));
        if (w !== canvas.width || h !== canvas.height) { canvas.width = w; canvas.height = h; q.push([7, w, h, 0, 0]); }
    }).observe(canvas);
},
  host_gamepad__proxy: 'sync',
  host_gamepad__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_gamepad: function (index, buttons, triggers, sticks) {
    const pads = typeof navigator !== "undefined" && navigator.getGamepads ? navigator.getGamepads() : [];
    const p = Array.from(pads).filter(x => x && x.connected)[index];
    if (!p) return 0;
    const b = p.buttons, on = i => b[i] && b[i].pressed;
    const bits = [[12, 0x1], [13, 0x2], [14, 0x4], [15, 0x8], [9, 0x10], [8, 0x20], [10, 0x40], [11, 0x80], [4, 0x100],
                  [5, 0x200], [16, 0x400], [0, 0x1000], [1, 0x2000], [2, 0x4000], [3, 0x8000]];
    let m = 0;
    for (const [i, bit] of bits) if (on(i)) m |= bit;
    HEAPU16[buttons >>> 1] = m;
    HEAPU8[triggers >>> 0] = Math.round((b[6] ? b[6].value : 0) * 255);
    HEAPU8[(triggers >>> 0) + 1] = Math.round((b[7] ? b[7].value : 0) * 255);
    const ax = (i, flip) => {
        const v = (p.axes[i] || 0) * (flip ? -1 : 1);
        return Math.max(-32768, Math.min(32767, Math.round(v * 32767)));
    };
    HEAP16[sticks >>> 1] = ax(0); HEAP16[(sticks >>> 1) + 1] = ax(1, true);
    HEAP16[(sticks >>> 1) + 2] = ax(2); HEAP16[(sticks >>> 1) + 3] = ax(3, true);
    return 1;
},
  js_audio_open__proxy: 'sync',
  js_audio_open__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  js_audio_open: function (rate) {
    if (Module.uewAudio) return 1;
    if (typeof AudioContext === "undefined" || !globalThis.AudioWorkletNode) return 0;
    const A = Module.uewAudio = { ctx: new AudioContext({ sampleRate: rate, latencyHint: "interactive" }), node: null, pushed: 0, start: -1, rate, pending: [] };
    const code = `class P extends AudioWorkletProcessor {
        constructor() { super(); this.q = []; this.off = 0; this.port.onmessage = (e) => this.q.push(e.data); }
        process(inp, out) {
            const L = out[0][0], R = out[0][1] || out[0][0];
            for (let i = 0; i < L.length; i++) {
                while (this.q.length && this.off >= this.q[0].length) { this.q.shift(); this.off = 0; }
                if (!this.q.length) { L[i] = 0; R[i] = 0; continue; }
                const b = this.q[0]; L[i] = b[this.off]; R[i] = b[this.off + 1]; this.off += 2;
            }
            return true;
        } }
        registerProcessor("uew-out", P);`;
    const url = URL.createObjectURL(new Blob([code], { type: "text/javascript" }));
    A.ctx.audioWorklet.addModule(url).then(() => {
        A.node = new AudioWorkletNode(A.ctx, "uew-out", { outputChannelCount: [2] });
        A.node.connect(A.ctx.destination);
        for (const b of A.pending) A.node.port.postMessage(b, [b.buffer]);
        A.pending = [];
    });
    const resume = () => { if (A.ctx.state !== "running") A.ctx.resume(); };
    for (const ev of ["keydown", "mousedown", "touchstart", "pointerdown"]) window.addEventListener(ev, resume, { capture: true });
    return 1;
},
  js_audio_queued__proxy: 'sync',
  js_audio_queued__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  js_audio_queued: function () {
    const A = Module.uewAudio;
    if (!A) return 0;
    // until the page may play sound, time still passes (games wait for sounds to end) and samples are dropped
    const live = A.ctx.state === "running" && !!A.node;
    const now = live ? A.ctx.currentTime : performance.now() / 1000;
    if (A.live !== live || A.start < 0) { A.live = live; A.start = now; A.pushed = 0; }
    const played = (now - A.start) * A.rate;
    // the worklet ran dry: start counting again from now
    if (A.pushed < played) { A.start = now; A.pushed = 0; return 0; }
    return Math.round(A.pushed - played);
},
  js_audio_restart__proxy: 'sync',
  js_audio_restart__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  js_audio_restart: function () { const A = Module.uewAudio; if (A) A.start = -1; },
  js_audio_write__proxy: 'sync',
  js_audio_write__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  js_audio_write: function (frames, count) {
    const A = Module.uewAudio;
    if (!A) return;
    const src = HEAPF32.subarray((frames >>> 2), (frames >>> 2) + count * 2);
    // loudest recent sample (every 8th), for tests that check sound is really produced
    for (let i = 0; i < src.length; i += 8) { const v = Math.abs(src[i]); if (v > (A.peak || 0)) A.peak = v; }
    if (!A.live) { A.pushed += count; return; }
    const b = new Float32Array(count * 2);
    b.set(src);
    A.pushed += count;
    if (A.node) A.node.port.postMessage(b, [b.buffer]);
    else if (A.pending.length < 64) A.pending.push(b);
},
  host_rumble__proxy: 'sync',
  host_rumble__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_rumble: function (index, lo, hi) {
    const pads = typeof navigator !== "undefined" && navigator.getGamepads ? navigator.getGamepads() : [];
    const p = Array.from(pads).filter(x => x && x.connected)[index];
    const a = p && p.vibrationActuator;
    if (a && a.playEffect) a.playEffect("dual-rumble", { duration: 200, strongMagnitude: lo / 65535, weakMagnitude: hi / 65535 }).catch(() => {});
},
  web_next_event__proxy: 'sync',
  web_next_event__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  web_next_event: function (out) {
    const q = Module.uewInput;
    if (!q || !q.length) return 0;
    const e = q.shift();
    for (let i = 0; i < 5; i++) HEAP32[(out >>> 2) + i] = e[i];
    return 1;
},
  host_query_flag__proxy: 'sync',
  // exit from a worker thread unwinds its suspended stacks with these; they end the process, they aren't errors
  host_query_flag__postset: "if (typeof process == 'object' && process.on) process.on('unhandledRejection', e => { if (e == 'unwind' || (e && e.name == 'ExitStatus')) return; throw e; });" +
    "if (typeof addEventListener == 'function') addEventListener('unhandledrejection', e => { if (e.reason == 'unwind' || (e.reason && e.reason.name == 'ExitStatus')) e.preventDefault(); });" +
    // a crash on another core reaches the page as a bare message; print the wasm stack (function names) from where it happened
    "if (typeof ENVIRONMENT_IS_PTHREAD != 'undefined' && ENVIRONMENT_IS_PTHREAD && typeof self != 'undefined') self.addEventListener('error', e => { if (e.error && e.error.stack) err('[worker crash] ' + e.error.stack); });",
  host_query_flag__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_query_flag: function (name) {
    if (typeof location === "undefined") return 0;
    return new RegExp("[?&]" + UTF8ToString(name >>> 0) + "=1(&|$)").test(location.search) ? 1 : 0;
},
  host_query_hex__proxy: 'sync',
  host_query_hex__deps: ['$UTF8ToString'],
  host_query_hex__sig: 'dp',
  host_query_hex: function (name) {
    if (typeof location === "undefined") return 0;
    const m = new RegExp("[?&]" + UTF8ToString(name >>> 0) + "=([0-9a-fA-F]+)(&|$)").exec(location.search);
    return m ? parseInt(m[1], 16) : 0;
  },
  host_page_int__proxy: 'sync',
  host_page_int__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_page_int: function (name) {
    const v = typeof Module !== "undefined" ? Module[UTF8ToString(name >>> 0)] : 0;
    return v ? v | 0 : 0;
},
  host_query_int__proxy: 'sync',
  host_query_int__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_query_int: function (name) {
    if (typeof location === "undefined") return 0;
    const m = new RegExp("[?&]" + UTF8ToString(name >>> 0) + "=(\\d+)").exec(location.search);
    return m ? +m[1] : 0;
},
  web_trace_filter__proxy: 'sync',
  web_trace_filter__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  web_trace_filter: function () {
    const s = (typeof window !== "undefined" && window.uewTraceOnly) || "";
    return stringToNewUTF8(s ? "," + s + "," : "");
},
  web_canvas_w__proxy: 'sync',
  web_canvas_w__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  web_canvas_w: function () { const c = Module.canvas || (typeof document !== "undefined" && document.getElementById("canvas")); return c ? c.width : 1280; },
  web_canvas_h__proxy: 'sync',
  web_canvas_h__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  web_canvas_h: function () { const c = Module.canvas || (typeof document !== "undefined" && document.getElementById("canvas")); return c ? c.height : 720; },
  host_show_cursor__proxy: 'sync',
  host_show_cursor__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_show_cursor: function (show) { const c = Module.canvas; if (c) c.style.cursor = show ? "default" : "none"; },
  host_lock_pointer__proxy: 'sync',
  host_lock_pointer__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  host_lock_pointer: function (lock) {
    const c = Module.canvas;
    if (!c || typeof document === "undefined") return;
    Module.uewWantLock = !!lock;
    if (!lock && document.pointerLockElement === c) document.exitPointerLock();
    if (lock && !Module.uewLockHooked) {
        Module.uewLockHooked = true;
        // the browser refuses a lock right after the user left one (Esc); the next click asks again
        c.addEventListener("click", () => {
            if (!Module.uewWantLock || document.pointerLockElement === c) return;
            try { const p = c.requestPointerLock(); if (p && p.catch) p.catch(() => {}); } catch (e) {}
        });
    }
},
  uew_rf_find__proxy: 'sync',
  uew_rf_find__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_rf_find: function (path, size, mtime) {
    const f = Module.uewFiles && Module.uewFiles.lookup(UTF8ToString(path >>> 0));
    if (!f) return -2;
    HEAPF64[size >>> 3] = f.size || 0;
    HEAPF64[mtime >>> 3] = f.mtime || 0;
    return f.dir ? -1 : f.id;
},
  uew_rf_list__proxy: 'sync',
  uew_rf_list__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_rf_list: function (path) {
    const s = Module.uewFiles && Module.uewFiles.list(UTF8ToString(path >>> 0));
    return s == null ? 0 : stringToNewUTF8(s);
},
  uew_rf_read_cached__proxy: 'sync',
  uew_rf_read_cached__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_rf_read_cached: function (id, off, len, dst) {
    return Module.uewFiles.readCached(id, off, len, dst);
},
  uew_rf_start__proxy: 'sync',
  uew_rf_start__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_rf_start: function (id, off, len) { Module.uewFiles.start(id, off, len); },
  uew_rf_ready__proxy: 'sync',
  uew_rf_ready__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_rf_ready: function (id, off, len) { return Module.uewFiles.ready(id, off, len); },
  inet_claims__proxy: 'sync',
  inet_claims__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  inet_claims: function (host) {
    const n = Module.uewNet;
    return n && n.http && (!n.claim || n.claim(UTF8ToString(host >>> 0))) ? 1 : 0;
},
  inet_request__proxy: 'sync',
  inet_request__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  inet_request: function (method, url, headers, body, len, status, hlen) {
    const n = Module.uewNet;
    let r;
    try {
        r = n.http({ method: UTF8ToString(method >>> 0), url: UTF8ToString(url >>> 0), headers: UTF8ToString(headers >>> 0),
                     body: HEAPU8.slice(body >>> 0, (body >>> 0) + len) });
    } catch (e) { console.log("[net] handler failed: " + e); r = { status: 500, headers: {}, body: "" }; }
    r = r || { status: 404, headers: {}, body: "" };
    let b = r.body;
    if (typeof b === "string") b = new TextEncoder().encode(b);
    else if (b && !(b instanceof Uint8Array)) b = new TextEncoder().encode(JSON.stringify(b));
    b = b || new Uint8Array(0);
    let h = "";
    const hs = Object.assign({ "Content-Length": String(b.length) }, r.headers || {});
    for (const k in hs) h += k + ": " + hs[k] + "\r\n";
    n.last = { h: new TextEncoder().encode(h), b };
    HEAP32[(status >>> 0) >> 2] = r.status || 200;
    HEAP32[(hlen >>> 0) >> 2] = n.last.h.length;
    return b.length;
},
  inet_take__proxy: 'sync',
  inet_take__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  inet_take: function (headers, body) {
    const l = Module.uewNet.last;
    HEAPU8.set(l.h, headers >>> 0);
    HEAPU8.set(l.b, body >>> 0);
    Module.uewNet.last = null;
},
  net_claim__proxy: 'sync',
  net_claim__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  net_claim: function (name) {
    const n = Module.uewNet;
    if (!n || !n.claim) return 0;
    const s = UTF8ToString(name >>> 0);
    if (!n.claim(s)) return 0;
    n.names = n.names || [""];
    let i = n.names.indexOf(s);
    if (i < 0) { i = n.names.length; n.names.push(s); }
    return i;
},
  net_open__proxy: 'sync',
  net_open__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  net_open: function (route, port) {
    const n = Module.uewNet;
    const c = n && n.open ? n.open(n.names[route], port) : null;
    if (!c) return 0;
    n.conns = n.conns || [null];
    const id = n.conns.length;
    c.q = []; c.qlen = 0; c.closed = false;
    c.push = data => { if (data && data.length) { c.q.push(data); c.qlen += data.length; } };
    c.end = () => { c.closed = true; };
    n.conns.push(c);
    return id;
},
  net_send__proxy: 'sync',
  net_send__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  net_send: function (id, p, n) {
    const c = Module.uewNet.conns[id];
    if (c && c.send) c.send(HEAPU8.slice(p >>> 0, (p >>> 0) + n));
},
  net_avail__proxy: 'sync',
  net_avail__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  net_avail: function (id) {
    const c = Module.uewNet && Module.uewNet.conns ? Module.uewNet.conns[id] : null;
    if (!c) return -1;
    return c.qlen ? c.qlen : c.closed ? -1 : 0;
},
  net_recv__proxy: 'sync',
  net_recv__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  net_recv: function (id, p, cap) {
    const c = Module.uewNet.conns[id];
    if (!c) return -1;
    if (!c.qlen) return c.closed ? -1 : 0;
    let n = 0;
    while (n < cap && c.q.length) {
        const d = c.q[0], k = Math.min(cap - n, d.length);
        HEAPU8.set(d.subarray(0, k), (p >>> 0) + n);
        n += k;
        if (k == d.length) c.q.shift(); else c.q[0] = d.subarray(k);
    }
    c.qlen -= n;
    return n;
},
  net_close__proxy: 'sync',
  net_close__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  net_close: function (id) {
    const c = Module.uewNet && Module.uewNet.conns ? Module.uewNet.conns[id] : null;
    if (c) { if (c.close) c.close(); Module.uewNet.conns[id] = null; }
},
  uew_vpx_available__proxy: 'sync',
  uew_vpx_available__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_available: function () {
    if (typeof VideoDecoder !== "function" || typeof EncodedVideoChunk !== "function") return 0;
    if (typeof location !== "undefined" && /[?&]swvideo=1/.test(location.search)) return 0;
    return Module.uewHwVideo === false ? 0 : 1;
},
  uew_vpx_open__proxy: 'sync',
  uew_vpx_open__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_open: function (vp9) {
    const V = Module.uewVpx || (Module.uewVpx = { n: 0, s: new Map() });
    const id = ++V.n, s = { vp9, ready: [], err: false, ts: 0 };
    try {
        s.dec = new VideoDecoder({
            output: f => {
                const rect = f.visibleRect, cs = f.colorSpace || {}, fmt = f.format;
                if (fmt !== "I420" && fmt !== "NV12") {
                    if (!V.fmtLogged) { V.fmtLogged = 1; console.log("[video] decoder gives " + fmt + " pictures; using the software decoder"); }
                    f.close(); s.err = true; return;
                }
                let buf;
                try { buf = new Uint8Array(f.allocationSize({ rect })); } catch (e) { f.close(); s.err = true; return; }
                const item = { buf, layout: null, fmt, w: rect.width, h: rect.height, matrix: cs.matrix || "", full: !!cs.fullRange };
                s.ready.push(item);
                f.copyTo(buf, { rect }).then(layout => { item.layout = layout; f.close(); }, () => { f.close(); s.err = true; });
            },
            error: e => { s.err = true; if (!V.errLogged) { V.errLogged = 1; console.log("[video] hardware decoder failed (" + e.message + "); using the software decoder"); } },
        });
    } catch (e) { return 0; }
    V.s.set(id, s);
    return id;
},
  uew_vpx_decode__proxy: 'sync',
  uew_vpx_decode__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_decode: function (id, p, n, key) {
    const s = Module.uewVpx.s.get(id);
    if (!s || s.err) return -1;
    try {
        if (s.dec.state === "unconfigured") {
            if (!key) return -1;
            s.dec.configure({ codec: s.vp9 ? "vp09.00.10.08" : "vp8", optimizeForLatency: true, hardwareAcceleration: "prefer-hardware" });
        }
        p >>>= 0;
        s.dec.decode(new EncodedVideoChunk({ type: key ? "key" : "delta", timestamp: ++s.ts, data: HEAPU8.slice(p, p + n) }));
        return 0;
    } catch (e) { s.err = true; return -1; }
},
  uew_vpx_poll__proxy: 'sync',
  uew_vpx_poll__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_poll: function (id) {
    const s = Module.uewVpx.s.get(id);
    if (!s || s.err) return 2;
    return s.ready.length && s.ready[0].layout ? 1 : 0;
},
  uew_vpx_info__proxy: 'sync',
  uew_vpx_info__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_info: function (id, out) {
    const s = Module.uewVpx.s.get(id), f = s && s.ready[0];
    if (!f) return 0;
    out >>>= 2;
    HEAP32[out] = f.w; HEAP32[out + 1] = f.h;
    HEAP32[out + 2] = f.matrix === "bt709" ? 2 : f.matrix === "bt470bg" ? 1 : f.matrix === "smpte170m" ? 3 : f.matrix === "smpte240m" ? 4 : f.matrix.startsWith("bt2020") ? 5 : f.matrix === "rgb" ? 7 : 0;
    HEAP32[out + 3] = f.full ? 1 : 0;
    return 1;
},
  uew_vpx_take__proxy: 'sync',
  uew_vpx_take__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_take: function (id, y, ys, u, us, v, vs) {
    const s = Module.uewVpx.s.get(id), f = s && s.ready.shift();
    if (!f) return;
    const L = f.layout, b = f.buf, w = f.w, h = f.h, cw = (w + 1) >> 1, ch = (h + 1) >> 1, H = HEAPU8;
    y >>>= 0; u >>>= 0; v >>>= 0;
    for (let r = 0; r < h; r++) H.set(b.subarray(L[0].offset + r * L[0].stride, L[0].offset + r * L[0].stride + w), y + r * ys);
    if (f.fmt === "I420") {
        for (let r = 0; r < ch; r++) {
            H.set(b.subarray(L[1].offset + r * L[1].stride, L[1].offset + r * L[1].stride + cw), u + r * us);
            H.set(b.subarray(L[2].offset + r * L[2].stride, L[2].offset + r * L[2].stride + cw), v + r * vs);
        }
    } else {
        for (let r = 0; r < ch; r++) {
            let o = L[1].offset + r * L[1].stride, du = u + r * us, dv = v + r * vs;
            for (let x = 0; x < cw; x++, o += 2) { H[du + x] = b[o]; H[dv + x] = b[o + 1]; }
        }
    }
},
  uew_vpx_close__proxy: 'sync',
  uew_vpx_close__deps: ['$UTF8ToString', '$stringToNewUTF8'],
  uew_vpx_close: function (id) {
    const V = Module.uewVpx, s = V && V.s.get(id);
    if (!s) return;
    try { if (s.dec.state !== "closed") s.dec.close(); } catch (e) {}
    V.s.delete(id);
},
  uew_js_frame_request__proxy: 'sync',
  uew_js_frame_request: function (seq) {
    if (!Module.uewWatchdog && typeof setInterval !== "undefined" && typeof document !== "undefined") {
      // a game that stops presenting while the page is visible: log what every thread is doing, once per stall
      let last = -1, since = performance.now(), dumped = false;
      Module.uewWatchdog = setInterval(() => {
        const n = Module.uewPresents || 0, now = performance.now();
        if (n !== last || document.visibilityState !== "visible") { last = n; since = now; dumped = false; return; }
        if (!dumped && n > 600 && now - since > 8000 && Module._uew_threads) {
          dumped = true;
          console.log("[watchdog] no frame for 8 s; threads:");
          Module._uew_threads();
        }
      }, 1000);
    }
    Module.uewFrameSeqPtr = seq >>> 0;
    if (Module.uewFramePending) return;
    Module.uewFramePending = 1;
    const go = () => {
      Module.uewFramePending = 0;
      Module.uewFrameSeq = (Module.uewFrameSeq | 0) + 1;
      // the count lives in wasm memory, so threads on every core see it
      HEAP32[Module.uewFrameSeqPtr >>> 2] = Module.uewFrameSeq;
      const w = Module.uewIdleWake;
      if (w) { Module.uewIdleWake = null; w(); }
    };
    (typeof requestAnimationFrame !== "undefined" ? requestAnimationFrame : setTimeout)(go);
  },
});
