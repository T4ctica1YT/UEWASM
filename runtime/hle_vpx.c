/* Games that link libvpx (Unreal's WebM players and the like) decode VP8/VP9 on the CPU, which costs a lot once the
   code is translated. At startup we find libvpx's decoder interface tables in the image and point them at these
   functions: each decoder becomes a browser VideoDecoder (WebCodecs, normally the GPU's video engine) and the decoded
   picture is copied into an ordinary I420 vpx_image_t. A decoder the browser can't run falls back to the original
   libvpx code, which keeps handling it. Off with ?swvideo=1 or the build option. */
#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <string.h>

/* vpx_codec_iface_t offsets in a 64-bit build (libvpx ABI 5) */
enum { IF_NAME = 0x00, IF_INIT = 0x10, IF_DESTROY = 0x18, IF_CTRL = 0x20, IF_PEEK = 0x28, IF_GET_SI = 0x30, IF_DECODE = 0x38,
       IF_GET_FRAME = 0x40, IF_SET_FB = 0x48 };
/* vpx_image_t */
enum { IMG_FMT = 0, IMG_CS = 4, IMG_RANGE = 8, IMG_W = 12, IMG_H = 16, IMG_BITS = 20, IMG_DW = 24, IMG_DH = 28, IMG_RW = 32, IMG_RH = 36,
       IMG_XS = 40, IMG_YS = 44, IMG_PLANES = 48, IMG_STRIDE = 80, IMG_BPS = 96, IMG_USER = 104, IMG_DATA = 112, IMG_FB = 128, IMG_SIZE = 136 };

typedef struct {
    uint64_t addr, orig[10];        /* original function pointers, indexed by (offset - 0x10) / 8 */
    int vp9;
} Iface;
static Iface ifaces[2];
static int nifaces;

typedef struct {
    uint64_t priv;                  /* what libvpx passes to every call */
    int iface, sw, js;
    uint64_t ctx, img, buf;
    uint32_t bufsize, w, h;
    int have;
    uint32_t n_dec, n_pic, n_late, n_hidden;
    uint64_t fb_get, fb_release, fb_priv, fb, fb_held;
} Dec;
static Dec decs[64];

static Dec* find(uint64_t priv)
{
    for (int i = 0; i < 64; i++) if (decs[i].priv && decs[i].priv == priv) return &decs[i];
    return NULL;
}
static uint64_t orig(const Dec* d, int off) { return ifaces[d ? d->iface : 0].orig[(off - IF_INIT) / 8]; }

#ifdef __EMSCRIPTEN__
#include <emscripten.h>

EM_JS(int, uew_vpx_available, (void), {
    if (typeof VideoDecoder !== "function" || typeof EncodedVideoChunk !== "function") return 0;
    if (typeof location !== "undefined" && /[?&]swvideo=1/.test(location.search)) return 0;
    return Module.uewHwVideo === false ? 0 : 1;
});
EM_JS(int, uew_vpx_open, (int vp9), {
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
});
EM_JS(int, uew_vpx_decode, (int id, const uint8_t* p, int n, int key), {
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
});
/* 0 nothing yet, 1 a picture is ready to take, 2 the decoder failed */
EM_JS(int, uew_vpx_poll, (int id), {
    const s = Module.uewVpx.s.get(id);
    if (!s || s.err) return 2;
    return s.ready.length && s.ready[0].layout ? 1 : 0;
});
EM_JS(int, uew_vpx_info, (int id, int* out), {
    const s = Module.uewVpx.s.get(id), f = s && s.ready[0];
    if (!f) return 0;
    out >>>= 2;
    HEAP32[out] = f.w; HEAP32[out + 1] = f.h;
    HEAP32[out + 2] = f.matrix === "bt709" ? 2 : f.matrix === "bt470bg" ? 1 : f.matrix === "smpte170m" ? 3 : f.matrix === "smpte240m" ? 4 : f.matrix.startsWith("bt2020") ? 5 : f.matrix === "rgb" ? 7 : 0;
    HEAP32[out + 3] = f.full ? 1 : 0;
    return 1;
});
EM_JS(void, uew_vpx_take, (int id, uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs), {
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
});
EM_JS(void, uew_vpx_close, (int id), {
    const V = Module.uewVpx, s = V && V.s.get(id);
    if (!s) return;
    try { if (s.dec.state !== "closed") s.dec.close(); } catch (e) {}
    V.s.delete(id);
});
#else
static int uew_vpx_available(void) { return 0; }
static int uew_vpx_open(int vp9) { (void)vp9; return 0; }
static int uew_vpx_decode(int id, const uint8_t* p, int n, int key) { (void)id; (void)p; (void)n; (void)key; return -1; }
static int uew_vpx_poll(int id) { (void)id; return 2; }
static int uew_vpx_info(int id, int* out) { (void)id; (void)out; return 0; }
static void uew_vpx_take(int id, uint8_t* y, int ys, uint8_t* u, int us, uint8_t* v, int vs) { (void)id; (void)y; (void)ys; (void)u; (void)us; (void)v; (void)vs; }
static void uew_vpx_close(int id) { (void)id; }
#endif

/* ---- the original decoder, for streams the browser can't take ---- */
static uint64_t call_orig(CPU* c, const Dec* d, int off, int n, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    uint64_t a[5] = { a0, a1, a2, a3, a4 };
    return rt_invoke(c, orig(d, off), n, a);
}
/* switch a decoder to libvpx: its init sets ctx->priv to its own state, which we then track */
static int go_software(CPU* c, Dec* d, const char* why)
{
    static int said;
    if (said++ < 4) host_log("[video] software VP%d decoding for one stream: %s", ifaces[d->iface].vp9 ? 9 : 8, why);
    if (d->js) { uew_vpx_close(d->js); d->js = 0; }
    uint64_t ours = d->priv;
    ST64(d->ctx + 0x30, 0);
    if ((uint32_t)call_orig(c, d, IF_INIT, 2, d->ctx, 0, 0, 0, 0)) return 0;
    d->priv = LD64(d->ctx + 0x30);
    d->sw = 1;
    if (ours > 1) g_free(ours);
    return 1;
}

/* key frame / shown frame from the frame headers (a VP9 superframe shows if any frame in it does) */
static int vp9_flags(const uint8_t* p, uint32_t n, int* shown)
{
    *shown = 0;
    if (!n) return 0;
    uint32_t sizes[8], nf = 1;
    uint8_t last = p[n - 1];
    if ((last & 0xE0) == 0xC0) {
        uint32_t frames = (last & 7) + 1, mag = ((last >> 3) & 3) + 1, ix = 2 + mag * frames;
        if (n >= ix && p[n - ix] == last) {
            const uint8_t* q = p + n - ix + 1;
            for (uint32_t i = 0; i < frames; i++, q += mag) { sizes[i] = 0; for (uint32_t k = 0; k < mag; k++) sizes[i] |= (uint32_t)q[k] << (8 * k); }
            nf = frames;
        }
    }
    if (nf == 1) sizes[0] = n;
    int key = 0;
    uint32_t off = 0;
    for (uint32_t i = 0; i < nf && off < n; off += sizes[i], i++) {
        uint8_t b = p[off];
        if ((b >> 6) != 2) continue;
        int profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1), bit = profile == 3 ? 2 : 3;
        int show_existing = (b >> bit) & 1;
        if (show_existing) { *shown = 1; continue; }
        int frame_type = bit >= 1 ? (b >> (bit - 1)) & 1 : 0;
        int show = bit >= 2 ? (b >> (bit - 2)) & 1 : (p[off + 1] >> 7) & 1;
        if (i == 0 && frame_type == 0) key = 1;
        if (show) *shown = 1;
    }
    return key;
}
static int vp8_flags(const uint8_t* p, uint32_t n, int* shown)
{
    *shown = n ? (p[0] >> 4) & 1 : 0;
    return n && !(p[0] & 1);
}

static void ensure_image(Dec* d, uint32_t w, uint32_t h)
{
    uint32_t ys = (w + 31) & ~31u, cs = (((w + 1) >> 1) + 31) & ~31u, ch = (h + 1) >> 1;
    uint32_t need = ys * h + 2 * cs * ch + 64;
    if (!d->img) d->img = g_calloc(IMG_SIZE);
    if (!d->fb_get && need > d->bufsize) {
        if (d->buf) g_free(d->buf);
        d->buf = g_alloc(need);
        d->bufsize = need;
    }
    d->w = w;
    d->h = h;
}

/* ---- the hooks ---- */
static void dec_init(CPU* c, int iface)
{
    uint64_t ctx = A1;
    Dec* d = NULL;
    for (int i = 0; i < 64 && !d; i++) if (!decs[i].priv) d = &decs[i];
    if (!d) { RET(call_orig(c, &(Dec){ .iface = iface }, IF_INIT, 2, ctx, A2, 0, 0, 0)); return; }
    memset(d, 0, sizeof *d);
    d->iface = iface;
    d->ctx = ctx;
    int js = uew_vpx_available() ? uew_vpx_open(ifaces[iface].vp9) : 0;
    if (!js) {
        d->priv = 1;   /* reserve the slot while libvpx initialises */
        if (!go_software(c, d, "the browser has no WebCodecs video decoder")) { d->priv = 0; RET(1); return; }
        RET(0);
        return;
    }
    static int said;
    if (!said++) host_log("[video] VP%d streams decode in the browser's video decoder", ifaces[iface].vp9 ? 9 : 8);
    d->js = js;
    d->priv = g_calloc(256);
    ST64(ctx + 0x30, d->priv);
    RET(0);
}
static void init_vp9(CPU* c) { dec_init(c, ifaces[0].vp9 ? 0 : 1); }
static void init_vp8(CPU* c) { dec_init(c, ifaces[0].vp9 ? 1 : 0); }

static void dec_destroy(CPU* c)
{
    Dec* d = find(A1);
    if (!d) { RET(0); return; }
    uint64_t r = 0;
    if (!d->sw && (d->n_late || d->n_dec < 4)) {
        static int said;
        if (said++ < 40) host_log("[video] stream closed: %u chunks, %u pictures, %u hidden, %u late", d->n_dec, d->n_pic, d->n_hidden, d->n_late);
    }
    if (d->sw) r = call_orig(c, d, IF_DESTROY, 1, d->priv, 0, 0, 0, 0);
    else {
        uew_vpx_close(d->js);
        if (d->fb_held && d->fb_release) { uint64_t a[2] = { d->fb_priv, d->fb_held }; rt_invoke(c, d->fb_release, 2, a); }
        g_free(d->priv);
    }
    if (d->fb) g_free(d->fb);
    if (d->img) g_free(d->img);
    if (d->buf) g_free(d->buf);
    memset(d, 0, sizeof *d);
    RET(r);
}

static int pic_ready(GThread* t) { return uew_vpx_poll((int)t->block_a) != 0; }

static void dec_decode(CPU* c)
{
    uint64_t priv = A1, data = A2, user = A4;
    uint32_t size = (uint32_t)A3;
    Dec* d = find(priv);
    if (!d) { RET(1); return; }
    if (d->sw) { RET(call_orig(c, d, IF_DECODE, 5, priv, data, A3, user, ARG(5))); return; }
    if (!data || !size) { RET(0); return; }
    const uint8_t* p = G2H(data);
    int shown, key = ifaces[d->iface].vp9 ? vp9_flags(p, size, &shown) : vp8_flags(p, size, &shown);
    if (uew_vpx_decode(d->js, p, (int)size, key) < 0) {
        if (!go_software(c, d, "the browser decoder refused the stream")) { RET(1); return; }
        RET(call_orig(c, d, IF_DECODE, 5, d->priv, data, A3, user, ARG(5)));
        return;
    }
    d->have = 0;
    d->n_dec++;
    if (!shown) { d->n_hidden++; RET(0); return; }
    /* this thread waits for the picture; the others keep running */
    GThread* t = sched_current();
    t->block_a = (uint64_t)d->js;
    sched_block(c, pic_ready, host_ticks_ns() + 500000000ull);
    int st = uew_vpx_poll(d->js);
    if (st == 2) {
        /* the stream restarts in libvpx at the next key frame */
        go_software(c, d, "the browser decoder failed");
        RET(0);
        return;
    }
    if (st != 1) {
        d->n_late++;
        static int said;
        if (said++ < 20) host_log("[video] no picture within 500 ms (chunk %u of a stream, %u pictures so far, key %d)", d->n_dec, d->n_pic, key);
        RET(0);
        return;
    }
    d->n_pic++;
    int info[4];
    uew_vpx_info(d->js, info);
    uint32_t w = (uint32_t)info[0], h = (uint32_t)info[1];
    ensure_image(d, w, h);
    uint32_t ys = (w + 31) & ~31u, cs = (((w + 1) >> 1) + 31) & ~31u, ch = (h + 1) >> 1;
    uint32_t need = ys * h + 2 * cs * ch + 64;
    uint64_t base = d->buf;
    if (d->fb_get) {
        /* the game supplies picture memory: take a buffer for this frame, give back the previous one */
        if (!d->fb) d->fb = g_calloc(32);
        ST64(d->fb, 0); ST64(d->fb + 8, 0); ST64(d->fb + 16, 0);
        uint64_t a[3] = { d->fb_priv, need, d->fb };
        if ((uint32_t)rt_invoke(c, d->fb_get, 3, a) || !LD64(d->fb)) { RET(1); return; }
        if (d->fb_held && d->fb_release) { uint64_t r[2] = { d->fb_priv, d->fb_held }; rt_invoke(c, d->fb_release, 2, r); }
        if (!d->fb_held) d->fb_held = g_calloc(32);
        memcpy(G2H(d->fb_held), G2H(d->fb), 24);
        base = LD64(d->fb);
    }
    base = (base + 31) & ~31ull;
    uint64_t y = base, u = y + (uint64_t)ys * h, v = u + (uint64_t)cs * ch;
    uew_vpx_take(d->js, G2H(y), (int)ys, G2H(u), (int)cs, G2H(v), (int)cs);
    uint64_t img = d->img;
    memset(G2H(img), 0, IMG_SIZE);
    ST32(img + IMG_FMT, 0x102);
    ST32(img + IMG_CS, (uint32_t)info[2]);
    ST32(img + IMG_RANGE, (uint32_t)info[3]);
    ST32(img + IMG_W, w); ST32(img + IMG_H, h); ST32(img + IMG_BITS, 8);
    ST32(img + IMG_DW, w); ST32(img + IMG_DH, h); ST32(img + IMG_RW, w); ST32(img + IMG_RH, h);
    ST32(img + IMG_XS, 1); ST32(img + IMG_YS, 1);
    ST64(img + IMG_PLANES, y); ST64(img + IMG_PLANES + 8, u); ST64(img + IMG_PLANES + 16, v);
    ST32(img + IMG_STRIDE, ys); ST32(img + IMG_STRIDE + 4, cs); ST32(img + IMG_STRIDE + 8, cs);
    ST32(img + IMG_BPS, 12);
    ST64(img + IMG_USER, user);
    ST64(img + IMG_DATA, base);
    if (d->fb_get) ST64(img + IMG_FB, LD64(d->fb_held + 16));
    d->have = 1;
    RET(0);
}

static void dec_get_frame(CPU* c)
{
    uint64_t priv = A1, iter = A2;
    Dec* d = find(priv);
    if (!d) { RET(0); return; }
    if (d->sw) { RET(call_orig(c, d, IF_GET_FRAME, 2, priv, iter, 0, 0, 0)); return; }
    if (!iter || LD64(iter) || !d->have) { RET(0); return; }
    ST64(iter, d->img);
    d->have = 0;
    RET(d->img);
}

static void dec_get_si(CPU* c)
{
    Dec* d = find(A1);
    if (!d) { RET(1); return; }
    if (d->sw) { RET(call_orig(c, d, IF_GET_SI, 2, A1, A2, 0, 0, 0)); return; }
    if (A2) { ST32(A2 + 4, d->w); ST32(A2 + 8, d->h); }
    RET(0);
}

static void dec_set_fb(CPU* c)
{
    Dec* d = find(A1);
    if (!d) { RET(1); return; }
    if (d->sw) { RET(call_orig(c, d, IF_SET_FB, 4, A1, A2, A3, A4, 0)); return; }
    d->fb_get = A2;
    d->fb_release = A3;
    d->fb_priv = A4;
    RET(0);
}

/* controls: each table entry gets its own thunk so we know its id */
typedef struct { int iface, id; uint64_t fn; } Ctrl;
static Ctrl ctrls[128];
static int nctrls;
static void ctrl_call(CPU* c, int k)
{
    Ctrl* e = &ctrls[k];
    Dec* d = find(A1);
    if (!d || d->sw) { uint64_t a[2] = { A1, A2 }; RET(rt_invoke(c, e->fn, 2, a)); return; }
    uint64_t arg = LD64(A2);
    switch (e->id) {
    case 256: case 257: case 258: case 267: if (arg) ST32(arg, 0); RET(0); return;   /* ref updates, corrupted, ref used, quantizer */
    case 260: case 261: if (arg) { ST32(arg, d->w); ST32(arg + 4, d->h); } RET(0); return;   /* frame / display size */
    case 262: if (arg) ST32(arg, 8); RET(0); return;   /* bit depth */
    case 2: case 128: RET(4); return;                  /* reference frame copies: not available */
    default: RET(0); return;                           /* settings: nothing to do */
    }
}
#define C1(k) static void ctrl_##k(CPU* c) { ctrl_call(c, k); }
#define C8(k) C1(k##0) C1(k##1) C1(k##2) C1(k##3) C1(k##4) C1(k##5) C1(k##6) C1(k##7)
C8(0) C8(1) C8(2) C8(3) C8(4) C8(5) C8(6) C8(7) C8(8) C8(9) C8(10) C8(11) C8(12) C8(13) C8(14) C8(15)
#define R1(k) ctrl_##k,
#define R8(k) R1(k##0) R1(k##1) R1(k##2) R1(k##3) R1(k##4) R1(k##5) R1(k##6) R1(k##7)
static const GuestFn ctrl_fns[128] = { R8(0) R8(1) R8(2) R8(3) R8(4) R8(5) R8(6) R8(7) R8(8) R8(9) R8(10) R8(11) R8(12) R8(13) R8(14) R8(15) };

static int in_image(uint64_t a) { return a >= IMAGE_LO && a < IMAGE_HI; }

static void hook(uint64_t ia, int vp9)
{
    if (nifaces == 2) return;
    if (LD32(ia + 8) != 5) return;
    for (int off = IF_INIT; off <= IF_SET_FB; off += 8) if (off != IF_CTRL && !in_image(LD64(ia + off))) return;
    Iface* f = &ifaces[nifaces];
    f->addr = ia;
    f->vp9 = vp9;
    for (int off = IF_INIT; off <= IF_SET_FB; off += 8) f->orig[(off - IF_INIT) / 8] = LD64(ia + off);
    int me = nifaces++;
    ST64(ia + IF_INIT, rt_dyn_thunk(vp9 ? init_vp9 : init_vp8, vp9 ? "vpx_vp9_init" : "vpx_vp8_init"));
    ST64(ia + IF_DESTROY, rt_dyn_thunk(dec_destroy, "vpx_destroy"));
    ST64(ia + IF_GET_SI, rt_dyn_thunk(dec_get_si, "vpx_get_si"));
    ST64(ia + IF_DECODE, rt_dyn_thunk(dec_decode, "vpx_decode"));
    ST64(ia + IF_GET_FRAME, rt_dyn_thunk(dec_get_frame, "vpx_get_frame"));
    ST64(ia + IF_SET_FB, rt_dyn_thunk(dec_set_fb, "vpx_set_fb"));
    /* a copy of the control table whose entries come to ctrl_call */
    uint64_t tab = LD64(ia + IF_CTRL);
    if (in_image(tab)) {
        int n = 0;
        while (n < 64 && LD64(tab + 16ull * n + 8)) n++;
        if (nctrls + n <= 128) {
            uint64_t copy = g_calloc(16ull * (n + 1));
            for (int i = 0; i < n; i++) {
                Ctrl* e = &ctrls[nctrls];
                e->iface = me;
                e->id = (int)LD32(tab + 16ull * i);
                e->fn = LD64(tab + 16ull * i + 8);
                ST32(copy + 16ull * i, (uint32_t)e->id);
                ST64(copy + 16ull * i + 8, rt_dyn_thunk(ctrl_fns[nctrls], "vpx_control"));
                nctrls++;
            }
            ST64(ia + IF_CTRL, copy);
        }
    }
    host_log("[video] libvpx VP%d decoder found at %llx", vp9 ? 9 : 8, (unsigned long long)ia);
}

/* finds "WebM Project VP9 Decoder" / "VP8 Decoder" names and the interface tables pointing at them */
void hle_vpx_init(void)
{
    if (!uew_vpx_available()) return;
    static const char* names[2] = { "WebM Project VP9 Decoder", "WebM Project VP8 Decoder" };
    const uint8_t* lo = G2H(IMAGE_LO);
    size_t span = (size_t)(IMAGE_HI - IMAGE_LO);
    for (int k = 0; k < 2; k++) {
        size_t nl = strlen(names[k]);
        for (size_t i = 0; i + nl < span; i++) {
            if (lo[i] != 'W' || memcmp(lo + i, names[k], nl)) continue;
            uint64_t sa = IMAGE_LO + i;
            for (size_t j = 0; j + 8 <= span; j += 8) {
                uint64_t v;
                memcpy(&v, lo + j, 8);
                if (v == sa) hook(IMAGE_LO + j, k == 0);
            }
            break;
        }
    }
}
