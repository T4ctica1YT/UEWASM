#ifdef __EMSCRIPTEN__
#include "host.h"

#include <emscripten.h>

EM_JS(void, web_input_init, (void), {
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
});

/* standard-mapping pad -> XInput: buttons, triggers 0-255, sticks -32768..32767 (up positive) */
EM_JS(int, host_gamepad, (int index, uint16_t* buttons, uint8_t* triggers, int16_t* sticks), {
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
});

/* WebAudio: an AudioWorklet plays chunks posted from here; the context starts on the first key or click */
EM_JS(int, js_audio_open, (int rate), {
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
});

EM_JS(int, js_audio_queued, (void), {
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
});

EM_JS(void, js_audio_restart, (void), { const A = Module.uewAudio; if (A) A.start = -1; });
EM_JS(void, js_audio_write, (const float* frames, int count), {
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
});

/* fast-forward: the output is paced by the game clock and the samples are dropped, so audio-timed engines keep up */
double host_speed(void);
static int audio_rate = 48000, ff_on;
static uint64_t ff_pushed, ff_start;
int host_audio_open(int rate) { audio_rate = rate; return js_audio_open(rate); }
int host_audio_queued(void)
{
    if (host_speed() == 1.0) {
        if (ff_on) { ff_on = 0; js_audio_restart(); }
        return js_audio_queued();
    }
    uint64_t now = host_ticks_ns();
    if (!ff_on) { ff_on = 1; ff_pushed = 0; ff_start = now; }
    uint64_t played = (now - ff_start) * (uint64_t)audio_rate / 1000000000ull;
    if (ff_pushed < played) { ff_start = now; ff_pushed = 0; return 0; }
    return (int)(ff_pushed - played);
}
void host_audio_write(const float* frames, int count)
{
    if (host_speed() == 1.0 && !ff_on) { js_audio_write(frames, count); return; }
    if (host_speed() == 1.0) { host_audio_queued(); js_audio_write(frames, count); return; }
    if (!ff_on) host_audio_queued();
    ff_pushed += (uint64_t)count;
}

EM_JS(void, host_rumble, (int index, int lo, int hi), {
    const pads = typeof navigator !== "undefined" && navigator.getGamepads ? navigator.getGamepads() : [];
    const p = Array.from(pads).filter(x => x && x.connected)[index];
    const a = p && p.vibrationActuator;
    if (a && a.playEffect) a.playEffect("dual-rumble", { duration: 200, strongMagnitude: lo / 65535, weakMagnitude: hi / 65535 }).catch(() => {});
});

EM_JS(int, web_next_event, (int* out), {
    const q = Module.uewInput;
    if (!q || !q.length) return 0;
    const e = q.shift();
    for (let i = 0; i < 5; i++) HEAP32[(out >>> 2) + i] = e[i];
    return 1;
});

void host_poll_events(void)
{
    static int inited;
    if (!inited) { web_input_init(); inited = 1; }
    int e[5];
    while (web_next_event(e)) user32_event(e[0], e[1], e[2], e[3], e[4]);
}

/* ?name=1 in the page address: switches for comparing behaviours while debugging */
EM_JS(int, host_query_flag, (const char* name), {
    if (typeof location === "undefined") return 0;
    return new RegExp("[?&]" + UTF8ToString(name >>> 0) + "=1(&|$)").test(location.search) ? 1 : 0;
});
/* a number the page sets on Module (build settings such as the memory budget), or 0 */
EM_JS(int, host_page_int, (const char* name), {
    const v = typeof Module !== "undefined" ? Module[UTF8ToString(name >>> 0)] : 0;
    return v ? v | 0 : 0;
});
/* ?name=N in the page address, or 0 */
EM_JS(int, host_query_int, (const char* name), {
    if (typeof location === "undefined") return 0;
    const m = new RegExp("[?&]" + UTF8ToString(name >>> 0) + "=(\\d+)").exec(location.search);
    return m ? +m[1] : 0;
});
EM_JS(char*, web_trace_filter, (void), {
    const s = (typeof window !== "undefined" && window.uewTraceOnly) || "";
    return stringToNewUTF8(s ? "," + s + "," : "");
});
const char* host_trace_filter(void) { return web_trace_filter(); }

EM_ASYNC_JS(void, host_frame_yield, (void), {
    await new Promise(r => (typeof requestAnimationFrame !== "undefined" ? requestAnimationFrame : setTimeout)(r));
});

EM_JS(int, web_canvas_w, (void), { const c = Module.canvas || (typeof document !== "undefined" && document.getElementById("canvas")); return c ? c.width : 1280; });
EM_JS(int, web_canvas_h, (void), { const c = Module.canvas || (typeof document !== "undefined" && document.getElementById("canvas")); return c ? c.height : 720; });
void host_canvas_size(int* w, int* h) { *w = web_canvas_w(); *h = web_canvas_h(); }

EM_JS(void, host_show_cursor, (int show), { const c = Module.canvas; if (c) c.style.cursor = show ? "default" : "none"; });
EM_JS(void, host_lock_pointer, (int lock), {
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
});
#endif
