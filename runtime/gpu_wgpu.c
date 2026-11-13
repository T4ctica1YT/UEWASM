#if defined(__EMSCRIPTEN__) && defined(UEW_WEBGPU)
#include "dxbc.h"
#include "gpu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <strings.h>

int uewg_init(int w, int h, void* ring, int ring_size);
int uewg_max_dynamic_ubo(void);
int uewg_max_storage(void);
int uewg_has_feature(const char* name);
int uewg_buffer_create(int size, int usage);
void uewg_buffer_write(int id, int offset, const void* p, int size);
void uewg_buffer_copy_ring(int ring_off, int dst, int dst_off, int size, int ring_used);
int uewg_texture_create(const char* json);
void uewg_texture_write(int id, int mip, int x, int y, int z, int w, int h, int d, const void* p, int bpr, int rows, int size);
int uewg_view_create(const char* json);
int uewg_sampler_create(const char* json);
int uewg_shader_create(const char* src);
int uewg_layout_create(const char* json);
int uewg_render_pipeline_create(const char* json);
int uewg_compute_pipeline_create(int layout, int cs);
int uewg_bindgroup_create(int layout, int group, const char* json);
int uewg_bindgroup_bin(int layout, int group, const void* ents, int n);
void uewg_release(int id);
void uewg_retire(int id);
void uewg_begin_pass(const char* json);
void uewg_clear(const char* json, int ring_used);
void uewg_draw(const int32_t* p);
void uewg_dispatch(const int32_t* p);
void uewg_copy_texture(int src, int smip, int slayer, int sx, int sy, int sz, int dst, int dmip, int dlayer, int dx, int dy, int dz, int w, int h, int d);
void uewg_copy_buffer(int src, int soff, int dst, int doff, int size);
void uewg_present(int view, int ring_used);
void uewg_gs(const int32_t* p);
void uewg_clear_buffer(int id, int off, int size);
void uewg_gen_mips(int tex);
int uewg_readback_start(int kind, int id, int mip, int layer, int w, int h, int d, void* dst, int pitch, int slice, int rowbytes, int rows);
int uewg_readback_done(int n);
int uewg_pipe_ready(int id);
int uewg_readback_latest(int kind, int id, int mip, int layer, int w, int h, int d, void* dst, int pitch, int slice, int rowbytes, int rows);
void uewg_readback_finish(int n);
#include "sched.h"
static int rb_done(GThread* t) { return uewg_readback_done((int)t->block_a); }
INIT_FN(rb_names) { sched_name_check(rb_done, "GPU readback", NULL); }
static int pipe_ready(GThread* t) { return uewg_pipe_ready((int)t->block_a); }
/* a pipeline still compiling in the browser: only this thread waits for it, and no draw is dropped */
static void wait_pipe(int id)
{
    if (!id || uewg_pipe_ready(id)) return;
    sched_current()->block_a = (uint64_t)id;
    sched_block_ext(sched_current_cpu(), pipe_ready, ~0ull);
}
INIT_FN(pipe_names) { sched_name_check(pipe_ready, "shader compile", NULL); }
/* waits for the GPU copy on this thread only */
static void uewg_readback(int kind, int id, int mip, int layer, int w, int h, int d, void* dst, int pitch, int slice, int rowbytes, int rows)
{
    int n = uewg_readback_start(kind, id, mip, layer, w, h, d, dst, pitch, slice, rowbytes, rows);
    if (!n) return;
    sched_current()->block_a = (uint64_t)n;
    sched_block_ext(sched_current_cpu(), rb_done, ~0ull);
    uewg_readback_finish(n);
}
void uewg_flush(int ring_used);

/* ---- text building ---- */

typedef struct { char* p; size_t n, cap; } Buf;
static void bp(Buf* b, const char* fmt, ...)
{
    va_list a;
    for (;;) {
        va_start(a, fmt);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, a);
        va_end(a);
        if (k < 0) return;
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = (b->cap + (size_t)k + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
}
static void breset(Buf* b) { b->n = 0; if (b->p) b->p[0] = 0; }

/* ---- formats ---- */

enum { FF_DEPTH = 1, FF_STENCIL = 2, FF_UINT = 4, FF_SINT = 8, FF_SRGB_PAIR = 16, FF_NORENDER = 32, FF_STORAGE = 64, FF_NOBLEND = 128 };
/* 16-bit norm formats aren't filterable in WebGPU, so they are stored as half floats (CV_UN16/CV_SN16) */
enum { CV_NONE, CV_RGB32, CV_565, CV_5551, CV_4444, CV_X8, CV_UN16, CV_SN16, CV_SUBST };

typedef struct { const char* name; uint8_t bpb, block, flags, conv; const char* view_pair; } TexFmt;

static int g_d32s8;

static TexFmt texfmt(uint32_t f, int ds)
{
    if (ds) switch (f) {
        case 19: case 20: case 21: case 22: return (TexFmt){ "depth32float-stencil8", 8, 1, FF_DEPTH | FF_STENCIL };
        case 39: case 40: case 41: return (TexFmt){ "depth32float", 4, 1, FF_DEPTH };
        case 44: case 45: case 46: case 47: return g_d32s8 ? (TexFmt){ "depth32float-stencil8", 8, 1, FF_DEPTH | FF_STENCIL } : (TexFmt){ "depth24plus-stencil8", 4, 1, FF_DEPTH | FF_STENCIL };
        case 53: case 55: case 56: return (TexFmt){ "depth16unorm", 2, 1, FF_DEPTH };
        }
    switch (f) {
    case 1: case 2: return (TexFmt){ "rgba32float", 16, 1, FF_STORAGE | FF_NOBLEND };
    case 3: return (TexFmt){ "rgba32uint", 16, 1, FF_UINT | FF_STORAGE | FF_NOBLEND };
    case 4: return (TexFmt){ "rgba32sint", 16, 1, FF_SINT | FF_STORAGE | FF_NOBLEND };
    case 5: case 6: return (TexFmt){ "rgba32float", 16, 1, FF_NOBLEND, CV_RGB32 };
    case 7: return (TexFmt){ "rgba32uint", 16, 1, FF_UINT | FF_NOBLEND, CV_RGB32 };
    case 8: return (TexFmt){ "rgba32sint", 16, 1, FF_SINT | FF_NOBLEND, CV_RGB32 };
    case 9: case 10: return (TexFmt){ "rgba16float", 8, 1, FF_STORAGE };
    case 11: return (TexFmt){ "rgba16float", 8, 1, 0, CV_UN16 };
    case 12: return (TexFmt){ "rgba16uint", 8, 1, FF_UINT | FF_STORAGE | FF_NOBLEND };
    case 13: return (TexFmt){ "rgba16float", 8, 1, 0, CV_SN16 };
    case 14: return (TexFmt){ "rgba16sint", 8, 1, FF_SINT | FF_STORAGE | FF_NOBLEND };
    case 15: case 16: return (TexFmt){ "rg32float", 8, 1, FF_STORAGE | FF_NOBLEND };
    case 17: return (TexFmt){ "rg32uint", 8, 1, FF_UINT | FF_STORAGE | FF_NOBLEND };
    case 18: return (TexFmt){ "rg32sint", 8, 1, FF_SINT | FF_STORAGE | FF_NOBLEND };
    case 19: case 20: case 21: case 22: return (TexFmt){ "depth32float-stencil8", 8, 1, FF_DEPTH | FF_STENCIL };
    case 23: case 24: return (TexFmt){ "rgb10a2unorm", 4, 1, 0 };
    case 25: return (TexFmt){ "rgb10a2uint", 4, 1, FF_UINT | FF_NOBLEND };
    case 26: return (TexFmt){ "rg11b10ufloat", 4, 1, 0 };
    case 27: case 28: return (TexFmt){ "rgba8unorm", 4, 1, FF_SRGB_PAIR | FF_STORAGE, 0, "rgba8unorm-srgb" };
    case 29: return (TexFmt){ "rgba8unorm-srgb", 4, 1, FF_SRGB_PAIR, 0, "rgba8unorm" };
    case 30: return (TexFmt){ "rgba8uint", 4, 1, FF_UINT | FF_STORAGE | FF_NOBLEND };
    case 31: return (TexFmt){ "rgba8snorm", 4, 1, FF_STORAGE | FF_NORENDER };
    case 32: return (TexFmt){ "rgba8sint", 4, 1, FF_SINT | FF_STORAGE | FF_NOBLEND };
    case 33: case 34: return (TexFmt){ "rg16float", 4, 1, 0 };
    case 35: return (TexFmt){ "rg16float", 4, 1, 0, CV_UN16 };
    case 36: return (TexFmt){ "rg16uint", 4, 1, FF_UINT | FF_NOBLEND };
    case 37: return (TexFmt){ "rg16float", 4, 1, 0, CV_SN16 };
    case 38: return (TexFmt){ "rg16sint", 4, 1, FF_SINT | FF_NOBLEND };
    case 39: case 41: return (TexFmt){ "r32float", 4, 1, FF_STORAGE | FF_NOBLEND };
    case 40: return (TexFmt){ "depth32float", 4, 1, FF_DEPTH };
    case 42: return (TexFmt){ "r32uint", 4, 1, FF_UINT | FF_STORAGE | FF_NOBLEND };
    case 43: return (TexFmt){ "r32sint", 4, 1, FF_SINT | FF_STORAGE | FF_NOBLEND };
    case 44: case 45: case 46: case 47: return g_d32s8 ? (TexFmt){ "depth32float-stencil8", 8, 1, FF_DEPTH | FF_STENCIL } : (TexFmt){ "depth24plus-stencil8", 4, 1, FF_DEPTH | FF_STENCIL };
    case 48: case 49: return (TexFmt){ "rg8unorm", 2, 1, 0 };
    case 50: return (TexFmt){ "rg8uint", 2, 1, FF_UINT | FF_NOBLEND };
    case 51: return (TexFmt){ "rg8snorm", 2, 1, FF_NORENDER };
    case 52: return (TexFmt){ "rg8sint", 2, 1, FF_SINT | FF_NOBLEND };
    case 53: case 54: return (TexFmt){ "r16float", 2, 1, 0 };
    case 55: return (TexFmt){ "depth16unorm", 2, 1, FF_DEPTH };
    case 56: return (TexFmt){ "r16float", 2, 1, 0, CV_UN16 };
    case 57: return (TexFmt){ "r16uint", 2, 1, FF_UINT | FF_NOBLEND };
    case 58: return (TexFmt){ "r16float", 2, 1, 0, CV_SN16 };
    case 59: return (TexFmt){ "r16sint", 2, 1, FF_SINT | FF_NOBLEND };
    case 60: case 61: case 65: return (TexFmt){ "r8unorm", 1, 1, 0 };
    case 62: return (TexFmt){ "r8uint", 1, 1, FF_UINT | FF_NOBLEND };
    case 63: return (TexFmt){ "r8snorm", 1, 1, FF_NORENDER };
    case 64: return (TexFmt){ "r8sint", 1, 1, FF_SINT | FF_NOBLEND };
    case 67: return (TexFmt){ "rgb9e5ufloat", 4, 1, FF_NORENDER };
    case 70: case 71: return (TexFmt){ "bc1-rgba-unorm", 8, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc1-rgba-unorm-srgb" };
    case 72: return (TexFmt){ "bc1-rgba-unorm-srgb", 8, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc1-rgba-unorm" };
    case 73: case 74: return (TexFmt){ "bc2-rgba-unorm", 16, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc2-rgba-unorm-srgb" };
    case 75: return (TexFmt){ "bc2-rgba-unorm-srgb", 16, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc2-rgba-unorm" };
    case 76: case 77: return (TexFmt){ "bc3-rgba-unorm", 16, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc3-rgba-unorm-srgb" };
    case 78: return (TexFmt){ "bc3-rgba-unorm-srgb", 16, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc3-rgba-unorm" };
    case 79: case 80: return (TexFmt){ "bc4-r-unorm", 8, 4, FF_NORENDER };
    case 81: return (TexFmt){ "bc4-r-snorm", 8, 4, FF_NORENDER };
    case 82: case 83: return (TexFmt){ "bc5-rg-unorm", 16, 4, FF_NORENDER };
    case 84: return (TexFmt){ "bc5-rg-snorm", 16, 4, FF_NORENDER };
    case 85: return (TexFmt){ "rgba8unorm", 4, 1, 0, CV_565 };
    case 86: return (TexFmt){ "rgba8unorm", 4, 1, 0, CV_5551 };
    case 87: case 90: return (TexFmt){ "bgra8unorm", 4, 1, FF_SRGB_PAIR, 0, "bgra8unorm-srgb" };
    case 88: case 92: return (TexFmt){ "bgra8unorm", 4, 1, FF_SRGB_PAIR, CV_X8, "bgra8unorm-srgb" };
    case 91: return (TexFmt){ "bgra8unorm-srgb", 4, 1, FF_SRGB_PAIR, 0, "bgra8unorm" };
    case 93: return (TexFmt){ "bgra8unorm-srgb", 4, 1, FF_SRGB_PAIR, CV_X8, "bgra8unorm" };
    case 94: case 95: return (TexFmt){ "bc6h-rgb-ufloat", 16, 4, FF_NORENDER };
    case 96: return (TexFmt){ "bc6h-rgb-float", 16, 4, FF_NORENDER };
    case 97: case 98: return (TexFmt){ "bc7-rgba-unorm", 16, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc7-rgba-unorm-srgb" };
    case 99: return (TexFmt){ "bc7-rgba-unorm-srgb", 16, 4, FF_NORENDER | FF_SRGB_PAIR, 0, "bc7-rgba-unorm" };
    case 115: return (TexFmt){ "rgba8unorm", 4, 1, 0, CV_4444 };
    }
    return (TexFmt){ NULL };
}

static int is_ds_typeless(uint32_t f) { return f == 19 || f == 39 || f == 44 || f == 53; }

static const char* vfmt(uint32_t f)
{
    switch (f) {
    case 2: return "float32x4"; case 3: return "uint32x4"; case 4: return "sint32x4";
    case 6: return "float32x3"; case 7: return "uint32x3"; case 8: return "sint32x3";
    case 10: return "float16x4"; case 11: return "unorm16x4"; case 12: return "uint16x4"; case 13: return "snorm16x4"; case 14: return "sint16x4";
    case 16: return "float32x2"; case 17: return "uint32x2"; case 18: return "sint32x2";
    case 24: return "unorm10-10-10-2";
    case 28: case 29: return "unorm8x4"; case 30: return "uint8x4"; case 31: return "snorm8x4"; case 32: return "sint8x4";
    case 34: return "float16x2"; case 35: return "unorm16x2"; case 36: return "uint16x2"; case 37: return "snorm16x2"; case 38: return "sint16x2";
    case 41: return "float32"; case 42: return "uint32"; case 43: return "sint32";
    case 49: return "unorm8x2"; case 50: return "uint8x2"; case 51: return "snorm8x2"; case 52: return "sint8x2";
    case 54: return "float16"; case 56: return "unorm16"; case 57: return "uint16"; case 58: return "snorm16"; case 59: return "sint16";
    case 61: return "unorm8"; case 62: return "uint8"; case 63: return "snorm8"; case 64: return "sint8";
    case 87: case 88: return "unorm8x4-bgra";
    }
    return NULL;
}

/* ---- backend objects ---- */

typedef struct WTex {
    int id, view_all, kind;
    TexFmt fmt;
    uint32_t w, h, layers, mips;
    uint32_t version, used;
    int buf_usage;
    /* last copy of this constant buffer in the ring: reused while the contents and frame are the same */
    uint32_t cb_serial, cb_version, cb_first, cb_size, cb_off;
    uint8_t* shadow;        /* last uploaded contents of a big CPU-written buffer: only changed bytes go up again */
    int gpu_written;        /* bound as a UAV or cleared since that upload: the shadow no longer matches the GPU copy */
} WTex;

enum { WV_MAX = 6 };
typedef struct WView {
    int rt_view;
    struct { int dim, id; } v[WV_MAX];
    int nv;
    int depth_aspect, stencil_aspect;
    uint32_t mip, mips, layer, layers, first_elem, num_elem;
    int typed_buf, typed_buf_size;
    uint32_t typed_version;
    int read_only;
    int slice;              /* render target slice of a 3D texture, or -1 */
    uint32_t wsize;         /* slices of a 3D render target view */
    int* layer_views;       /* single-layer attachment views, made for layered (geometry shader) draws */
    int nlayer_views;
} WView;

/* the resources a draw bound last time with this shader, and the bind group they made */
typedef struct Memo { int valid, layout, group, bg; uint32_t pass_gen, bg_epoch; int n; void* refs[160]; struct WTex* bufs[16]; int nbufs; } Memo;

typedef struct Variant {
    struct Variant* next;
    Memo memo;
    uint32_t depth[8];   /* depth-compared SRVs, then integer SRVs on 32-bit float textures */
    uint64_t sig;
    char uav_key[64];
    uint64_t vkey;   /* variant_ex: UAV formats + interpolation, hashed */
    DxWgsl w;
    int module, failed;
    /* slots the shader uses, so per-draw binding walks only those */
    int slots_ready, nsrv, nsamp, nuav;
    uint8_t srv_ix[128], samp_ix[16], uav_ix[8];
    int mbuf, mbuf_size;    /* overflow buffer for read-only buffers past the storage-buffer limit */
} Variant;

static void variant_slots(Variant* v)
{
    if (v->slots_ready) return;
    for (int i = 0; i < 128; i++) if (v->w.srv[i].used) v->srv_ix[v->nsrv++] = (uint8_t)i;
    for (int i = 0; i < 16; i++) if (v->w.samp[i].used) v->samp_ix[v->nsamp++] = (uint8_t)i;
    for (int i = 0; i < 8; i++) if (v->w.uav[i].used) v->uav_ix[v->nuav++] = (uint8_t)i;
    v->slots_ready = 1;
}

typedef struct WShader { Variant* vars; DxWgsl refl; int refl_ok; uint8_t srv_ix[128]; int nsrv; } WShader;

typedef struct Program {
    struct Program* next;
    Variant *vs, *ps, *cs;
    int kind;               /* 0 normal, 1 geometry compute (vs + gs), 2 layered draw (passthrough vs, no vertex buffers) */
    uint64_t xh;
    int layout;
    int cb_group[2];
    int cb_slots[2][16], ncb[2];
    uint32_t cb_static[2];  /* constant buffers past the device's dynamic-offset limit, by index into cb_slots */
    int empty_ok;
} Program;

typedef struct PipeKey {
    Program* prog;
    GpuLayout* layout;
    GpuState *blend, *depth, *raster;
    uint32_t stride[16];
    uint32_t rt_fmt[8], ds_fmt, sample_mask;
    uint8_t topology, strip32, ds_ro, pad;
} PipeKey;

typedef struct Pipe { struct Pipe* next; PipeKey k; uint64_t h; int id; } Pipe;

static Program* programs[1024];
static Pipe* pipes[1024];
static int g_ok, g_errors;
static uint32_t g_serial = 1;
static uint8_t* ring;
/* the constant ring runs on across frames: a constant buffer the game has not touched keeps its copy until the ring wraps
   over it (ring_gen counts wraps) */
static uint32_t ring_size = 64u << 20, ring_used, ring_gen;
static int pass_open;
static int expand_bufs = -1;
/* why open passes end (source line), for the per-frame report */
static uint16_t brk_line[64]; static uint32_t brk_n[64];
static void pass_break(int line) { if (pass_open) for (int i = 0; i < 64; i++) if (brk_line[i] == line || !brk_line[i]) { brk_line[i] = (uint16_t)line; brk_n[i]++; break; } pass_open = 0; }
#define PASS_BREAK() pass_break(__LINE__)
static uint32_t pass_gen = 1, bg_epoch = 1;   /* bumped when pass targets change / cached groups are dropped */
static GpuView* pass_rt[8];
static GpuView* pass_ds;
static int empty_bg;
static Buf jb;

static WTex* T(GpuRes* r) { return r ? (WTex*)r->backend : NULL; }
static WView* V(GpuView* v) { return v ? (WView*)v->backend : NULL; }

/* 64-bit hash of a key, 8 bytes a step with full mixing (caches trust it to tell keys apart) */
static uint64_t fnv(const void* p, size_t n, uint64_t h)
{
    const uint8_t* b = p;
    h ^= n * 0x9E3779B97F4A7C15ull;
    for (; n >= 8; b += 8, n -= 8) {
        uint64_t w;
        memcpy(&w, b, 8);
        h = (h ^ w) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
    }
    if (n) {
        uint64_t w = 0;
        memcpy(&w, b, n);
        h = (h ^ w) * 0x94D049BB133111EBull;
        h ^= h >> 29;
    }
    h *= 0x9E3779B97F4A7C15ull;
    return h ^ (h >> 32);
}

void uewg_submit(int ring_used);
void uewg_ring_used_at(uint32_t* p);
int uewg_timestamp(void);
int uewg_timestamp_get(int ticket, double* ms);
void uewg_ring_reset(void);
void uewg_texture_copy_ring(int ring_off, int id, int mip, int x, int y, int z, int w, int h, int d, int bpr, int rows);
void uewg_expand_typed(int src, int dst, int first, int n, int fmt, int bpe);
void uewg_buffer_copy(int src, int src_off, int dst, int dst_off, int size);
/* uploads what is new in the ring and submits the pending work */
static void ring_submit(void)
{
    uewg_submit((int)ring_used);
    PASS_BREAK();
}
/* the ring is full: submit, then start over from the front */
static void ring_flush(void)
{
    uewg_flush((int)ring_used);
    ring_used = 0;
    ring_gen++;
    PASS_BREAK();
    g_serial++;
}
/* debug switches for the draw-path caches: ?nodrawmemo=1 ?norescache=1 ?noringkeep=1 */
static int dbg_flag(const char* n, int* v) { if (*v < 0) *v = host_query_flag(n); return *v; }
static int no_dmemo = -1, no_rcache = -1, no_ringkeep = -1, full_dmask = -1, ring_reset = -1;
/* a copy made in the ring's current lap is still there (nothing is overwritten until the wrap, which submits first).
   Copies from the previous lap are not reused even where the write head hasn't reached them yet: a draw recorded now
   would only run after later writes in the same submission may have passed over them. */
static int ring_live(uint32_t gen, uint32_t off)
{
    (void)off;
    if (dbg_flag("noringkeep", &no_ringkeep)) return 0;
    return gen == ring_gen;
}

static uint64_t st_bg_rc, st_cb_same, st_ring_bytes, st_cb_hit, st_cb_miss, st_cb_dedup, st_gs, st_static, st_bg_memo, st_bg_hash, st_bg_new, st_ind, st_iid_ind, st_iid_lost;
static uint32_t ring_put(const void* p, uint32_t n, uint32_t pad_to)
{
    st_ring_bytes += pad_to > n ? pad_to : n;
    uint32_t need = pad_to > n ? pad_to : n;
    uint32_t off = (ring_used + 255) & ~255u;
    if (off + need > ring_size) { ring_flush(); off = 0; }
    if (n) memcpy(ring + off, p, n);
    if (need > n) memset(ring + off + n, 0, need - n);
    ring_used = off + ((need + 3) & ~3u);
    return off;
}

/* ---- resources ---- */

static uint16_t float_to_half(float x)
{
    uint32_t u;
    memcpy(&u, &x, 4);
    uint32_t s = (u >> 16) & 0x8000;
    int e = (int)((u >> 23) & 255) - 112;
    uint32_t m = u & 0x7FFFFF;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    uint32_t h = s | (uint32_t)e << 10 | m >> 13;
    if (m & 0x1000) h++;
    return (uint16_t)h;
}

static void tex_conv(TexFmt* f, const uint8_t* src, uint8_t* dst, uint32_t texels)
{
    if (f->conv == CV_UN16 || f->conv == CV_SN16) {
        uint32_t n = texels * (f->bpb / 2);
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = (uint16_t)(src[i * 2] | src[i * 2 + 1] << 8);
            float x = f->conv == CV_UN16 ? v / 65535.0f : ((int16_t)v < -32767 ? -1.0f : (int16_t)v / 32767.0f);
            uint16_t h = float_to_half(x);
            dst[i * 2] = (uint8_t)h;
            dst[i * 2 + 1] = (uint8_t)(h >> 8);
        }
        return;
    }
    for (uint32_t i = 0; i < texels; i++) {
        switch (f->conv) {
        case CV_RGB32: memcpy(dst + i * 16, src + i * 12, 12); { uint32_t one = f->flags & (FF_UINT | FF_SINT) ? 1 : 0x3F800000; memcpy(dst + i * 16 + 12, &one, 4); } break;
        case CV_565: { uint16_t v = (uint16_t)(src[i * 2] | src[i * 2 + 1] << 8);
            dst[i * 4] = (uint8_t)(((v >> 11) & 31) * 255 / 31); dst[i * 4 + 1] = (uint8_t)(((v >> 5) & 63) * 255 / 63); dst[i * 4 + 2] = (uint8_t)((v & 31) * 255 / 31); dst[i * 4 + 3] = 255; break; }
        case CV_5551: { uint16_t v = (uint16_t)(src[i * 2] | src[i * 2 + 1] << 8);
            dst[i * 4] = (uint8_t)(((v >> 10) & 31) * 255 / 31); dst[i * 4 + 1] = (uint8_t)(((v >> 5) & 31) * 255 / 31); dst[i * 4 + 2] = (uint8_t)((v & 31) * 255 / 31); dst[i * 4 + 3] = v >> 15 ? 255 : 0; break; }
        case CV_4444: { uint16_t v = (uint16_t)(src[i * 2] | src[i * 2 + 1] << 8);
            dst[i * 4] = (uint8_t)(((v >> 8) & 15) * 17); dst[i * 4 + 1] = (uint8_t)(((v >> 4) & 15) * 17); dst[i * 4 + 2] = (uint8_t)((v & 15) * 17); dst[i * 4 + 3] = (uint8_t)((v >> 12) * 17); break; }
        case CV_X8: memcpy(dst + i * 4, src + i * 4, 3); dst[i * 4 + 3] = 255; break;
        }
    }
}

static uint32_t src_bpb(uint32_t f) { uint32_t blk; return dxgi_format_bytes(f, &blk); }

static float half_to_float(uint16_t h);
static float small_float(uint32_t v, int mbits)
{
    uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (!e) return m ? ldexpf((float)m, -14 - mbits) : 0.0f;
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}
/* texels of a DXGI format as floats, for textures stored in a substitute storage format (CV_SUBST) */
static int decode_texels(uint32_t f, const uint8_t* s, float* out, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        float* o = out + 4 * i;
        o[0] = o[1] = o[2] = 0; o[3] = 1;
        switch (f) {
        case 61: case 60: case 62: o[0] = s[i] / 255.0f; break;                                  /* R8 */
        case 63: case 64: o[0] = (int8_t)s[i] < -127 ? -1.0f : (int8_t)s[i] / 127.0f; break;
        case 49: case 48: case 50: o[0] = s[2 * i] / 255.0f; o[1] = s[2 * i + 1] / 255.0f; break; /* R8G8 */
        case 51: case 52: o[0] = (int8_t)s[2 * i] / 127.0f; o[1] = (int8_t)s[2 * i + 1] / 127.0f; break;
        case 28: case 29: case 27: case 30: for (int k = 0; k < 4; k++) o[k] = s[4 * i + k] / 255.0f; break;
        case 31: case 32: for (int k = 0; k < 4; k++) o[k] = (int8_t)s[4 * i + k] / 127.0f; break;
        case 87: case 90: case 91: case 88: case 92: case 93: o[0] = s[4 * i + 2] / 255.0f; o[1] = s[4 * i + 1] / 255.0f; o[2] = s[4 * i] / 255.0f; o[3] = f == 88 || f == 92 || f == 93 ? 1.0f : s[4 * i + 3] / 255.0f; break;
        case 54: case 53: { uint16_t v; memcpy(&v, s + 2 * i, 2); o[0] = half_to_float(v); break; }   /* R16 float */
        case 34: case 33: for (int k = 0; k < 2; k++) { uint16_t v; memcpy(&v, s + 4 * i + 2 * k, 2); o[k] = half_to_float(v); } break;
        case 56: case 55: { uint16_t v; memcpy(&v, s + 2 * i, 2); o[0] = v / 65535.0f; break; }       /* R16 unorm */
        case 35: for (int k = 0; k < 2; k++) { uint16_t v; memcpy(&v, s + 4 * i + 2 * k, 2); o[k] = v / 65535.0f; } break;
        case 11: case 9: for (int k = 0; k < 4; k++) { uint16_t v; memcpy(&v, s + 8 * i + 2 * k, 2); o[k] = v / 65535.0f; } break;
        case 26: { uint32_t v; memcpy(&v, s + 4 * i, 4); o[0] = small_float(v & 0x7FF, 6); o[1] = small_float((v >> 11) & 0x7FF, 6); o[2] = small_float(v >> 22, 5); break; }
        case 24: case 23: { uint32_t v; memcpy(&v, s + 4 * i, 4); o[0] = (v & 1023) / 1023.0f; o[1] = ((v >> 10) & 1023) / 1023.0f; o[2] = ((v >> 20) & 1023) / 1023.0f; o[3] = (v >> 30) / 3.0f; break; }
        default: return 0;
        }
    }
    return 1;
}

static void bg_dead(int id);

/* GPU buffers of freed buffers, kept for a while: a game that makes a same-size buffer every frame gets the same GPU
   buffer back, so bind groups naming it are found in the cache instead of made anew each frame */
static struct { int id, usage; uint32_t width, used, serial; } bpool[256];
static int nbpool;

static void bpool_age(uint32_t serial)
{
    for (int i = 0; i < nbpool;)
        if (serial - bpool[i].serial > 60) { uewg_retire(bpool[i].id); bg_dead(bpool[i].id); bpool[i] = bpool[--nbpool]; }
        else i++;
}

static void res_create(GpuRes* r, uint64_t init)
{
    WTex* t = calloc(1, sizeof *t);
    r->backend = t;
    if (!g_ok) return;
    if (r->type == RES_BUFFER) {
        int u = 0;
        if (r->bind & 0x1) u |= 1 | 8;
        if (r->bind & 0x2) u |= 2 | 8;
        if (r->bind & 0x8 || r->bind & 0x80 || r->misc & 0x60) u |= 8;
        if (r->bind & 0x80) u |= 8;
        if (r->misc & 0x10) u |= 16;
        t->buf_usage = u;
        t->kind = 1;
        if (r->bind == 0x4) return;
        if (!init)
            for (int i = nbpool - 1; i >= 0; i--)
                if (bpool[i].width == r->width && bpool[i].usage == u) {
                    t->id = bpool[i].id;
                    t->used = g_serial;
                    bpool[i] = bpool[--nbpool];
                    /* on the encoder, after the old owner's commands */
                    uewg_clear_buffer(t->id, 0, (int)r->width);
                    PASS_BREAK();
                    return;
                }
        t->id = uewg_buffer_create((int)r->width, u);
        return;
    }
    int ds = (r->bind & 0x40) != 0;
    t->fmt = texfmt(r->format, ds || (is_ds_typeless(r->format) && !(r->bind & 0x20)));
    if (!t->fmt.name) { host_log("[webgpu] unsupported texture format %u", r->format); return; }
    if ((r->bind & 0x80) && !(t->fmt.flags & (FF_STORAGE | FF_DEPTH)) && t->fmt.block == 1) {
        /* compute shaders write this texture: switch to a float format WebGPU can store to, same channel count */
        const char* n = t->fmt.name;
        /* channels: the letters before the type suffix (r16float 1, rg8unorm 2, rg11b10ufloat 3, rgb10a2unorm and bgra8unorm 4) */
        int ch = 0;
        {
            size_t cut = strlen(n);
            static const char* sfx[] = { "unorm", "snorm", "ufloat", "float", "uint", "sint", 0 };
            for (int k = 0; sfx[k]; k++) { const char* q = strstr(n, sfx[k]); if (q && (size_t)(q - n) < cut) cut = (size_t)(q - n); }
            for (size_t k = 0; k < cut; k++) ch += n[k] == 'r' || n[k] == 'g' || n[k] == 'b' || n[k] == 'a';
        }
        t->fmt = ch <= 1 ? (TexFmt){ "r32float", 4, 1, FF_STORAGE | FF_NOBLEND, CV_SUBST } : ch == 2 ? (TexFmt){ "rg32float", 8, 1, FF_STORAGE | FF_NOBLEND, CV_SUBST }
                         : (TexFmt){ "rgba16float", 8, 1, FF_STORAGE, CV_SUBST };
    }
    uint32_t blk = t->fmt.block;
    t->w = (r->width + blk - 1) / blk * blk;
    t->h = r->type == RES_TEX1D ? 1 : (r->height + blk - 1) / blk * blk;
    t->layers = r->type == RES_TEX3D ? r->depth : r->array ? r->array : 1;
    t->mips = r->mips ? r->mips : 1;
    t->kind = 2;
    const char* dim = r->type == RES_TEX3D ? "3d" : r->type == RES_TEX1D && t->layers == 1 && t->mips == 1 ? "1d" : "2d";
    int rt = (r->bind & 0x20) && !(t->fmt.flags & FF_NORENDER) && !(t->fmt.flags & FF_DEPTH) && dim[0] != '1';
    int uav = (r->bind & 0x80) && (t->fmt.flags & FF_STORAGE);
    if ((r->bind & 0x80) && !uav) host_log("[webgpu] format %s can't be a storage texture", t->fmt.name);
    breset(&jb);
    bp(&jb, "{\"w\":%u,\"h\":%u,\"d\":%u,\"mips\":%u,\"format\":\"%s\",\"dim\":\"%s\",\"rt\":%d,\"uav\":%d", t->w, t->h, t->layers, t->mips,
       t->fmt.name, dim, rt || ds || (uav && !(t->fmt.flags & FF_NORENDER) && dim[0] != '1'), uav);
    if (t->fmt.view_pair) bp(&jb, ",\"viewFormats\":[\"%s\"]", t->fmt.view_pair);
    bp(&jb, "}");
    t->id = uewg_texture_create(jb.p);
}

static void res_destroy(GpuRes* r)
{
    WTex* t = T(r);
    if (!t) return;
    if (t->id && r->type == RES_BUFFER && nbpool < 256) {
        bpool[nbpool].id = t->id; bpool[nbpool].usage = t->buf_usage; bpool[nbpool].width = r->width;
        bpool[nbpool].used = t->used; bpool[nbpool].serial = g_serial; nbpool++;
        t->id = 0;
    }
    if (t->id) { uewg_retire(t->id); bg_dead(t->id); }
    if (t->view_all) uewg_retire(t->view_all);
    free(t->shadow);
    free(t);
    r->backend = NULL;
}

static void buffer_put(GpuRes* r, WTex* t, uint32_t off, const uint8_t* p, uint32_t n)
{
    if (!t->id || !n) return;
    /* ?bufdebug=1: every 600 frames, the buffers that took the most upload bytes */
    static int dbg = -1;
    static struct { GpuRes* r; uint64_t bytes; uint32_t writes; } top[64];
    static uint32_t last_serial;
    if (dbg < 0) dbg = host_query_flag("bufdebug");
    if (dbg) {
        int k = 0;
        while (k < 64 && top[k].r && top[k].r != r) k++;
        if (k < 64) { top[k].r = r; top[k].bytes += n; top[k].writes++; }
        if (g_serial / 600 != last_serial / 600) {
            last_serial = g_serial;
            for (int j = 0; j < 8; j++) {
                int best = -1;
                for (int i = 0; i < 64; i++) if (top[i].r && (best < 0 || top[i].bytes > top[best].bytes)) best = i;
                if (best < 0 || !top[best].bytes) break;
                GpuRes* b = top[best].r;
                host_log("[buf] %u bytes, usage %u, bind %x, misc %x, stride %u: %.0f KB/frame in %.1f writes/frame", (unsigned)b->width,
                         (unsigned)b->usage, (unsigned)b->bind, (unsigned)b->misc, (unsigned)b->stride, top[best].bytes / 600.0 / 1024.0, top[best].writes / 600.0);
                top[best].bytes = 0;
            }
            memset(top, 0, sizeof top);
        }
    }
    if (t->used == g_serial) {
        uint32_t ro = ring_put(p, n, (n + 3) & ~3u);
        uewg_buffer_copy_ring((int)ro, t->id, (int)(off & ~3u), (int)((n + 3) & ~3u), (int)ring_used);
        PASS_BREAK();
    } else uewg_buffer_write(t->id, (int)(off & ~3u), p, (int)n);
    (void)r;
}

/* a texture already used by this frame's unsubmitted commands is written through the ring, in order with them:
   queue writes land before those commands (staging textures refilled between copies would hand the copies the new data) */
static void tex_write(WTex* t, int mip, int x, int y, int z, int w, int h, int d, const uint8_t* p, int pitch, int rows, int size)
{
    uint32_t blocks = (uint32_t)(w + t->fmt.block - 1) / t->fmt.block, row = blocks * t->fmt.bpb, bpr = (row + 255) & ~255u;
    uint64_t need = (uint64_t)bpr * (uint32_t)rows * (uint32_t)d;
    if (t->used != g_serial || need > ring_size / 4) { uewg_texture_write(t->id, mip, x, y, z, w, h, d, p, pitch, rows, size); return; }
    uint32_t off = ring_put(NULL, 0, 0);
    if (off + need > ring_size) { ring_flush(); off = 0; }
    for (int k = 0; k < d; k++)
        for (int r = 0; r < rows; r++)
            memcpy(ring + off + ((uint64_t)k * rows + r) * bpr, p + ((uint64_t)k * rows + r) * (uint32_t)pitch, row);
    ring_used = off + (uint32_t)need;
    st_ring_bytes += need;
    uewg_texture_copy_ring((int)off, t->id, mip, x, y, z, w, h, d, (int)bpr, rows);
    PASS_BREAK();
}

static void res_upload(GpuRes* r, uint32_t sub, const int32_t* box, uint64_t src, uint32_t pitch, uint32_t slice)
{
    WTex* t = T(r);
    if (!t) return;
    t->version++;
    /* a UAV buffer the GPU wrote since its last upload goes up whole (and refreshes the shadow) */
    if (r->type == RES_BUFFER && !box && r->width >= 65536 && (r->bind & 0x80) && t->id && t->shadow && t->gpu_written) {
        memcpy(t->shadow, G2H(src), r->width);
        t->gpu_written = 0;
        buffer_put(r, t, 0, G2H(src), r->width);
        return;
    }
    if (r->type == RES_BUFFER && !box && r->width >= 65536 && t->id) {
        /* games remap whole dynamic buffers every frame but rewrite a little of them */
        const uint8_t* p = G2H(src);
        uint32_t n = r->width;
        if (!t->shadow) {
            t->shadow = malloc(n);
            if (t->shadow) memcpy(t->shadow, p, n);
            t->gpu_written = 0;
            buffer_put(r, t, 0, p, n);
            return;
        }
        uint32_t lo = 0, hi = n;
        while (lo + 8 <= n && !memcmp(p + lo, t->shadow + lo, 8)) lo += 8;
        while (lo < n && p[lo] == t->shadow[lo]) lo++;
        if (lo == n) return;
        while (hi >= lo + 8 && !memcmp(p + hi - 8, t->shadow + hi - 8, 8)) hi -= 8;
        while (hi > lo && p[hi - 1] == t->shadow[hi - 1]) hi--;
        lo &= ~3u;
        hi = (hi + 3) & ~3u;
        if (hi > n) hi = n;
        memcpy(t->shadow + lo, p + lo, hi - lo);
        buffer_put(r, t, lo, p + lo, hi - lo);
        return;
    }
    if (r->type == RES_BUFFER) {
        uint32_t off = box ? (uint32_t)box[0] : 0, n = box ? (uint32_t)(box[3] - box[0]) : r->width;
        buffer_put(r, t, off, G2H(src), n);
        return;
    }
    if (!t->id) return;
    uint32_t mips = t->mips, mip = sub % mips, layer = sub / mips;
    uint32_t blk = t->fmt.block;
    uint32_t mw = t->w >> mip, mh = t->h >> mip, md = r->type == RES_TEX3D ? r->depth >> mip : 1;
    if (!mw) mw = 1;
    if (!mh) mh = 1;
    if (!md) md = 1;
    uint32_t x = 0, y = 0, z = 0, w = mw, h = mh, d = md;
    if (box) { x = (uint32_t)box[0]; y = (uint32_t)box[1]; z = (uint32_t)box[2]; w = (uint32_t)(box[3] - box[0]); h = (uint32_t)(box[4] - box[1]); d = (uint32_t)(box[5] - box[2]); }
    if (blk > 1) { w = (w + blk - 1) / blk * blk; h = (h + blk - 1) / blk * blk; if (x + w > mw) w = mw - x; if (y + h > mh) h = mh - y; }
    if (r->type == RES_TEX1D) h = 1;
    if (!w || !h || !d) return;
    uint32_t rows = (h + blk - 1) / blk;
    if (!pitch) pitch = (w + blk - 1) / blk * src_bpb(r->format);
    if (pitch < (w + blk - 1) / blk * src_bpb(r->format)) { static int said; if (said++ < 8) host_log("[webgpu] upload pitch %u < row %u: format %u (%s conv %d) %ux%ux%u type %d box %d sub %u", pitch, (w + blk - 1) / blk * src_bpb(r->format), r->format, t->fmt.name, t->fmt.conv, w, h, d, r->type, box != NULL, sub); }
    if (!slice) slice = pitch * rows;
    if (r->type != RES_TEX3D) z = layer;
    if (t->fmt.conv) {
        uint32_t dp = w * t->fmt.bpb;
        uint8_t* tmp = malloc((size_t)dp * rows * d);
        float* fl = t->fmt.conv == CV_SUBST ? malloc(sizeof(float) * 4 * w) : NULL;
        for (uint32_t k = 0; k < d; k++)
            for (uint32_t row = 0; row < rows; row++) {
                const uint8_t* sp = G2H(src + (uint64_t)k * slice + (uint64_t)row * pitch);
                uint8_t* dpp = tmp + ((size_t)k * rows + row) * dp;
                if (!fl) { tex_conv(&t->fmt, sp, dpp, w); continue; }
                if (!decode_texels(r->format, sp, fl, w)) {
                    static int said;
                    if (!said++) host_log("[webgpu] no conversion from format %u to %s", r->format, t->fmt.name);
                    memset(fl, 0, sizeof(float) * 4 * w);
                }
                /* one or two channels go to float32, four to half floats */
                if (t->fmt.bpb == 4) for (uint32_t i = 0; i < w; i++) memcpy(dpp + 4 * i, &fl[4 * i], 4);
                else if (!strcmp(t->fmt.name, "rg32float")) for (uint32_t i = 0; i < w; i++) memcpy(dpp + 8 * i, &fl[4 * i], 8);
                else for (uint32_t i = 0; i < w; i++) for (int ch = 0; ch < 4; ch++) { uint16_t hv = float_to_half(fl[4 * i + ch]); memcpy(dpp + 8 * i + 2 * ch, &hv, 2); }
            }
        free(fl);
        tex_write(t, (int)mip, (int)x, (int)y, (int)z, (int)w, (int)h, (int)d, tmp, (int)dp, (int)rows, (int)(dp * rows * d));
        free(tmp);
        return;
    }
    uint32_t size = slice * (d - 1) + pitch * (rows - 1) + (w + blk - 1) / blk * t->fmt.bpb;
    tex_write(t, (int)mip, (int)x, (int)y, (int)z, (int)w, (int)h, (int)d, G2H(src), (int)pitch, (int)rows, (int)size);
}

/* ---- views ---- */

static void view_range(GpuView* v, WView* w)
{
    GpuRes* r = v->res;
    const uint32_t* d = (const uint32_t*)v->desc;
    uint32_t mips = r->mips ? r->mips : 1, layers = r->type == RES_TEX3D ? 1 : r->array ? r->array : 1;
    w->mip = 0; w->mips = mips; w->layer = 0; w->layers = layers;
    if (!v->dim) {
        if (r->type == RES_BUFFER) { w->first_elem = 0; w->num_elem = r->stride ? r->width / r->stride : r->width / 4; }
        return;
    }
    if (r->type == RES_BUFFER) { w->first_elem = d[2]; w->num_elem = d[3]; return; }
    if (v->kind == 0) {
        switch (v->dim) {
        case 2: case 4: case 8: case 9: w->mip = d[2]; w->mips = d[3]; break;
        case 3: case 5: w->mip = d[2]; w->mips = d[3]; w->layer = d[4]; w->layers = d[5]; break;
        case 6: w->mips = 1; break;
        case 7: w->mips = 1; w->layer = d[2]; w->layers = d[3]; break;
        case 10: w->mip = d[2]; w->mips = d[3]; w->layer = d[4]; w->layers = d[5] * 6; break;
        }
        if (w->mips == 0xFFFFFFFF || w->mip + w->mips > mips) w->mips = mips - w->mip;
    } else {
        int dsv = v->kind == 3;
        const uint32_t* u = dsv ? d + 3 : d + 2;
        uint32_t dim = v->dim;
        int arr = dsv ? (dim == 2 || dim == 4) : (dim == 3 || dim == 5);
        w->mip = u[0]; w->mips = 1;
        if (arr) { w->layer = u[1]; w->layers = u[2]; }
        else if (!dsv && dim == 8) { w->layer = 0; w->layers = 1; w->slice = (int)u[1]; w->wsize = u[2]; }
        else w->layers = 1;
        if (dsv) w->read_only = (int)d[2];
    }
    if (w->layer + w->layers > layers || w->layers == 0xFFFFFFFF) w->layers = layers - w->layer;
}

static const char* dim_name(int sdim)
{
    switch (sdim) {
    case 2: return "1d";
    case 5: return "3d";
    case 6: return "cube";
    case 7: case 8: case 9: return "2d-array";
    case 10: return "cube-array";
    default: return "2d";
    }
}

static int make_view(GpuView* v, WView* w, const char* dim, int force_single)
{
    WTex* t = T(v->res);
    if (!t || !t->id) return 0;
    TexFmt vf = texfmt(v->format, 0);
    const char* fmt = NULL;
    if (vf.name && t->fmt.view_pair && !strcmp(vf.name, t->fmt.view_pair)) fmt = vf.name;
    uint32_t mips = force_single ? 1 : w->mips, layers = w->layers;
    if (!strcmp(dim, "2d") || !strcmp(dim, "1d") || !strcmp(dim, "3d")) layers = 1;
    if (!strcmp(dim, "cube")) layers = 6;
    breset(&jb);
    bp(&jb, "{\"tex\":%d,\"dim\":\"%s\",\"mip\":%u,\"mips\":%u,\"layer\":%u,\"layers\":%u,\"aspect\":\"%s\"", t->id, dim, w->mip, mips, w->layer, layers,
       w->stencil_aspect ? "stencil-only" : w->depth_aspect ? "depth-only" : "all");
    if (fmt) bp(&jb, ",\"format\":\"%s\"", fmt);
    if (v->format == 65 && !w->depth_aspect) bp(&jb, ",\"swizzle\":\"000r\"");
    bp(&jb, "}");
    return uewg_view_create(jb.p);
}

static void view_create(GpuView* v)
{
    WView* w = calloc(1, sizeof *w);
    w->slice = -1;
    v->backend = w;
    if (!g_ok || !v->res) return;
    view_range(v, w);
    WTex* t = T(v->res);
    if (t && (t->fmt.flags & FF_DEPTH)) {
        uint32_t f = v->format;
        w->stencil_aspect = f == 22 || f == 47;
        w->depth_aspect = !w->stencil_aspect && v->kind == 0;
    }
    if (v->kind == 2 || v->kind == 3) {
        int saved = w->depth_aspect;
        w->depth_aspect = 0;
        w->stencil_aspect = 0;
        w->rt_view = make_view(v, w, v->res->type == RES_TEX3D ? "3d" : "2d", 1);
        w->depth_aspect = saved;
    }
}

static int view_for(GpuView* v, int sdim)
{
    WView* w = V(v);
    if (!w) return 0;
    for (int i = 0; i < w->nv; i++) if (w->v[i].dim == sdim) return w->v[i].id;
    int id = make_view(v, w, dim_name(sdim), 0);
    if (w->nv < WV_MAX) { w->v[w->nv].dim = sdim; w->v[w->nv].id = id; w->nv++; }
    return id;
}

/* ---- shaders ---- */

static void shader_create(GpuShader* s)
{
    WShader* w = calloc(1, sizeof *w);
    s->backend = w;
    if (s->stage == SH_VS || s->stage == SH_PS || s->stage == SH_CS || s->stage == SH_GS) w->refl_ok = dxbc_reflect(s->code, s->size, &w->refl);
    if (w->refl_ok) for (int i = 0; i < 128; i++) if (w->refl.srv[i].used) w->srv_ix[w->nsrv++] = (uint8_t)i;
}

static uint64_t sig_hash(const DxSig* s)
{
    if (!s) return 0;
    static struct { const DxSig* p; uint64_t h; } memo[256];
    unsigned k = (unsigned)(((uintptr_t)s >> 4) & 255);
    if (memo[k].p != s) { memo[k].p = s; memo[k].h = fnv(s, sizeof *s, 0xcbf29ce484222325ull); }
    return memo[k].h;
}

/* typed buffer SRV formats for the next variant() call (read in place by the shader), see buf_fmts */
static const uint8_t* cur_bfmt;

/* interp: for a vertex shader, the output interpolation its pixel shader asked for */
static Variant* variant_ex(GpuShader* s, const uint32_t depth[8], const DxSig* vs_out, const char* const* uav_fmt, const char* interp, int use_interp)
{
    const uint8_t* bf = cur_bfmt;
    cur_bfmt = NULL;
    WShader* w = s ? s->backend : NULL;
    if (!w || !w->refl_ok) return NULL;
    uint64_t sh = sig_hash(vs_out);
    uint64_t key = 0x9E3779B97F4A7C15ull * (uint64_t)(use_interp + 1);
    if (uav_fmt) for (int i = 0; i < 8; i++) if (uav_fmt[i]) key = (key ^ (uint64_t)(uintptr_t)uav_fmt[i] ^ (uint64_t)i << 56) * 0x100000001B3ull;
    if (interp) for (int i = 0; i < 32; i++) if (interp[i]) key = (key ^ (uint64_t)(i << 8 | interp[i])) * 0x100000001B3ull;
    if (bf) for (int i = 0; i < 128; i++) if (bf[i]) key = (key ^ ((uint64_t)i << 40 | bf[i])) * 0x100000001B3ull;
    for (Variant* v = w->vars; v; v = v->next)
        if (v->vkey == key && v->sig == sh && !memcmp(v->depth, depth, 32)) return v->failed ? NULL : v;
    Variant* v = calloc(1, sizeof *v);
    memcpy(v->depth, depth, 32);
    v->sig = sh;
    v->vkey = key;
    v->next = w->vars;
    w->vars = v;
    DxWgslOpts o = { 0 };
    { static int ms; if (!ms) ms = uewg_max_storage(); o.max_storage = ms; }
    o.vs_out = vs_out;
    o.out_interp = interp;
    o.use_interp = use_interp;
    memcpy(o.depth_srv, depth, 16);
    memcpy(o.float_srv, depth + 4, 16);
    if (uav_fmt) memcpy(o.uav_format, uav_fmt, sizeof o.uav_format);
    o.srv_buf_fmt = bf;
    if (!dxbc_to_wgsl_ex(s->code, s->size, &o, &v->w) || !v->w.wgsl) {
        static int skipped;
        if (skipped++ < 40) host_log("[webgpu] shader skipped: %s", v->w.error);
        v->failed = 1;
        return NULL;
    }
    v->module = uewg_shader_create(v->w.wgsl);
    free(v->w.wgsl);
    v->w.wgsl = NULL;
    return v;
}

/* formats of the typed buffer views a stage's shader reads, when they can be read in place (format known to the
   translator, first element at a storage-offset boundary); returns a hash for the draw memo, 0 when there are none */
static uint32_t buf_fmts(GpuCtx* c, int stage, uint8_t f[128])
{
    GpuShader* sh = c->sh[stage];
    WShader* w = sh ? sh->backend : NULL;
    uint32_t h = 0;
    memset(f, 0, 128);
    if (!w || !w->refl_ok || dbg_flag("expandbuffers", &expand_bufs)) return 0;
    for (int k = 0; k < w->nsrv; k++) {
        int i = w->srv_ix[k];
        DxSlot* s = &w->refl.srv[i];
        if (!s->buffer || s->raw || s->structured) continue;
        GpuView* v = c->srv[stage][i];
        if (!v || !v->res || v->res->type != RES_BUFFER || v->format >= 256) continue;
        int bpe = dxbc_wgsl_buf_bpe((int)v->format);
        WView* vw = V(v);
        if (!bpe || !vw || ((uint64_t)vw->first_elem * (uint32_t)bpe) & 255) continue;
        f[i] = (uint8_t)v->format;
        h = (h ^ (uint32_t)(i << 8 | f[i])) * 16777619u;
    }
    return h;
}

static Variant* variant(GpuShader* s, const uint32_t depth[8], const DxSig* vs_out, const char* const* uav_fmt)
{
    return variant_ex(s, depth, vs_out, uav_fmt, NULL, 0);
}

static int stage_vis(int stage) { return stage == SH_VS ? 1 : stage == SH_PS ? 2 : 4; }

static void layout_entries(Buf* b, Variant* v, int stage, int res, uint32_t* static_mask, int* dyn_left)
{
    int vis = stage_vis(stage), first = 1;
    DxWgsl* w = &v->w;
    if (!res) {
        for (int i = 0, k = 0; i < 16; i++) {
            if (!w->cb_size[i]) continue;
            int dyn = *dyn_left > 0;
            if (dyn) (*dyn_left)--;
            else *static_mask |= 1u << k;
            k++;
            bp(b, "%s{\"binding\":%d,\"visibility\":%d,\"buffer\":{\"type\":\"uniform\",\"hasDynamicOffset\":%s,\"minBindingSize\":%d}}", first ? "" : ",", i, vis,
               dyn ? "true" : "false", w->cb_size[i] * 16);
            first = 0;
        }
        return;
    }
    for (int i = 0; i < 128; i++) {
        DxSlot* s = &w->srv[i];
        if (!s->used || s->merged) continue;
        if (s->buffer) bp(b, "%s{\"binding\":%d,\"visibility\":%d,\"buffer\":{\"type\":\"read-only-storage\"}}", first ? "" : ",", 16 + i, vis);
        else {
            const char* st = s->compare ? "depth" : s->ret == 1 ? "sint" : s->ret == 2 ? "uint" : "float";
            bp(b, "%s{\"binding\":%d,\"visibility\":%d,\"texture\":{\"sampleType\":\"%s\",\"viewDimension\":\"%s\",\"multisampled\":%s}}", first ? "" : ",", 16 + i, vis, st,
               dim_name(s->dim), "false");
        }
        first = 0;
    }
    if (w->nmerged) { bp(b, "%s{\"binding\":176,\"visibility\":%d,\"buffer\":{\"type\":\"read-only-storage\"}}", first ? "" : ",", vis); first = 0; }
    for (int i = 0; i < 16; i++)
        if (w->samp[i].used) { bp(b, "%s{\"binding\":%d,\"visibility\":%d,\"sampler\":{\"type\":\"%s\"}}", first ? "" : ",", 160 + i, vis, w->samp[i].compare ? "comparison" : w->samp[i].nofilter ? "non-filtering" : "filtering"); first = 0; }
    for (int i = 0; i < 8; i++) {
        DxSlot* u = &w->uav[i];
        if (!u->used) continue;
        if (u->raw || u->structured || u->dim == 1) bp(b, "%s{\"binding\":%d,\"visibility\":%d,\"buffer\":{\"type\":\"storage\"}}", first ? "" : ",", 180 + i, vis);
        else bp(b, "%s{\"binding\":%d,\"visibility\":%d,\"storageTexture\":{\"access\":\"%s\",\"format\":\"%s\",\"viewDimension\":\"%s\"}}", first ? "" : ",", 180 + i, vis,
                u->written ? "write-only" : "read-write", "r32float", dim_name(u->dim));
        first = 0;
    }
}

static Program* program_ex(Variant* vs, Variant* ps, Variant* cs, const char* const* uav_fmt, int kind, const char* extra3)
{
    uint64_t xh = extra3 ? fnv(extra3, strlen(extra3), 1) : 0;
    uintptr_t key[5] = { (uintptr_t)vs, (uintptr_t)ps, (uintptr_t)cs, (uintptr_t)kind, (uintptr_t)xh };
    Program** slot = &programs[fnv(key, sizeof key, 7) & 1023];
    for (Program* p = *slot; p; p = p->next) if (p->vs == vs && p->ps == ps && p->cs == cs && p->kind == kind && p->xh == xh) return p;
    Program* p = calloc(1, sizeof *p);
    p->vs = vs; p->ps = ps; p->cs = cs;
    p->kind = kind;
    p->xh = xh;
    p->next = *slot;
    *slot = p;
    Buf b = { 0 };
    bp(&b, "[");
    Variant* st[2] = { cs ? cs : vs, cs ? NULL : ps };
    int stage[2] = { cs || kind == 1 ? SH_CS : SH_VS, kind == 1 ? SH_CS : SH_PS };
    int ngroups = cs ? 2 : 4;
    static int max_dyn = -1;
    if (max_dyn < 0) max_dyn = uewg_max_dynamic_ubo();
    int dyn_left = max_dyn;
    for (int g = 0; g < ngroups; g++) {
        Variant* v = st[g / 2];
        bp(&b, "%s[", g ? "," : "");
        if (v) {
            size_t before = b.n;
            layout_entries(&b, v, stage[g / 2], g & 1, &p->cb_static[g / 2], &dyn_left);
            if (g & 1 && uav_fmt) {
                /* storage texture formats come from the bound views */
                char* q = b.p + before;
                for (int i = 0; i < 8; i++) {
                    if (!uav_fmt[i]) continue;
                    char key[48];
                    snprintf(key, sizeof key, "\"binding\":%d,", 180 + i);
                    char* e = strstr(q, key);
                    char* f = e ? strstr(e, "\"format\":\"r32float\"") : NULL;
                    if (f && (!strchr(e, '}') || f < strstr(e, "}}"))) {
                        size_t tail = strlen(f + 19);
                        Buf nb = { 0 };
                        bp(&nb, "%.*s\"format\":\"%s\"%s", (int)(f - b.p), b.p, uav_fmt[i], f + 19);
                        (void)tail;
                        free(b.p);
                        b = nb;
                        q = b.p + before;
                    }
                }
            }
        }
        if (g == 3 && extra3) bp(&b, "%s%s", b.p[b.n - 1] == '[' ? "" : ",", extra3);
        bp(&b, "]");
    }
    bp(&b, "]");
    p->layout = uewg_layout_create(b.p);
    for (int s = 0; s < 2; s++) {
        Variant* v = st[s];
        if (!v) continue;
        breset(&b);
        bp(&b, "[");
        int first = 1;
        for (int i = 0; i < 16; i++)
            if (v->w.cb_size[i]) { bp(&b, "%s{\"b\":%d,\"ring\":1,\"size\":%d}", first ? "" : ",", i, v->w.cb_size[i] * 16); p->cb_slots[s][p->ncb[s]++] = i; first = 0; }
        bp(&b, "]");
        /* with fixed-offset buffers the group is made per draw instead (cb_bind) */
        if (!p->cb_static[s]) p->cb_group[s] = uewg_bindgroup_create(p->layout, s * 2, b.p);
    }
    free(b.p);
    return p;
}

static Program* program(Variant* vs, Variant* ps, Variant* cs, const char* const* uav_fmt) { return program_ex(vs, ps, cs, uav_fmt, 0, NULL); }

/* ---- states ---- */

static const char* cmp_name(uint32_t f)
{
    static const char* n[] = { "always", "never", "less", "equal", "less-equal", "greater", "not-equal", "greater-equal", "always" };
    return f <= 8 ? n[f] : "always";
}
static const char* blend_factor(uint32_t f, int dual)
{
    switch (f) {
    case 1: return "zero"; case 2: return "one"; case 3: return "src"; case 4: return "one-minus-src";
    case 5: return "src-alpha"; case 6: return "one-minus-src-alpha"; case 7: return "dst-alpha"; case 8: return "one-minus-dst-alpha";
    case 9: return "dst"; case 10: return "one-minus-dst"; case 11: return "src-alpha-saturated";
    case 14: return "constant"; case 15: return "one-minus-constant";
    case 16: return dual ? "src1" : "src"; case 17: return dual ? "one-minus-src1" : "one-minus-src";
    case 18: return dual ? "src1-alpha" : "src-alpha"; case 19: return dual ? "one-minus-src1-alpha" : "one-minus-src-alpha";
    }
    return "one";
}
static const char* blend_op(uint32_t o)
{
    switch (o) { case 2: return "subtract"; case 3: return "reverse-subtract"; case 4: return "min"; case 5: return "max"; }
    return "add";
}
static const char* stencil_op(uint32_t o)
{
    switch (o) {
    case 2: return "zero"; case 3: return "replace"; case 4: return "increment-clamp"; case 5: return "decrement-clamp";
    case 6: return "invert"; case 7: return "increment-wrap"; case 8: return "decrement-wrap";
    }
    return "keep";
}
static const char* address(uint32_t a)
{
    switch (a) { case 1: return "repeat"; case 2: case 5: return "mirror-repeat"; }
    return "clamp-to-edge";
}

static void state_create(GpuState* s)
{
    if (!g_ok || s->kind != 4) return;
    const uint32_t* d = (const uint32_t*)s->desc;
    uint32_t f = d[0];
    float bias, minl, maxl;
    memcpy(&bias, &d[4], 4);
    memcpy(&minl, &d[11], 4);
    memcpy(&maxl, &d[12], 4);
    int aniso = (f & 0x40) != 0;
    uint32_t an = d[5] < 1 ? 1 : d[5] > 16 ? 16 : d[5];
    if (minl < 0 || minl != minl) minl = 0;
    if (maxl > 32 || maxl != maxl) maxl = 32;
    if (maxl < minl) maxl = minl;
    breset(&jb);
    bp(&jb, "{\"addressModeU\":\"%s\",\"addressModeV\":\"%s\",\"addressModeW\":\"%s\",\"magFilter\":\"%s\",\"minFilter\":\"%s\",\"mipmapFilter\":\"%s\",\"lodMinClamp\":%g,\"lodMaxClamp\":%g",
       address(d[1]), address(d[2]), address(d[3]), aniso || (f & 4) ? "linear" : "nearest", aniso || (f & 16) ? "linear" : "nearest", aniso || (f & 1) ? "linear" : "nearest", minl, maxl);
    if (aniso && an > 1) bp(&jb, ",\"maxAnisotropy\":%u", an);
    if ((f & 0x180) == 0x80) bp(&jb, ",\"compare\":\"%s\"", cmp_name(d[6]));
    bp(&jb, "}");
    s->backend = (void*)(intptr_t)uewg_sampler_create(jb.p);
    /* the same addressing without filtering, for shaders that read depth textures through it */
    breset(&jb);
    bp(&jb, "{\"addressModeU\":\"%s\",\"addressModeV\":\"%s\",\"addressModeW\":\"%s\",\"lodMinClamp\":%g,\"lodMaxClamp\":%g}",
       address(d[1]), address(d[2]), address(d[3]), minl, maxl);
    s->backend_nearest = uewg_sampler_create(jb.p);
}

static int dummy_sampler[3];

/* mode: 0 filtering, 1 comparison, 2 non-filtering */
static int sampler_id(GpuState* s, int mode)
{
    if (s && mode == 2 && s->backend_nearest) return s->backend_nearest;
    if (s && mode != 2 && s->backend) return (int)(intptr_t)s->backend;
    if (!dummy_sampler[mode])
        dummy_sampler[mode] = uewg_sampler_create(mode == 1 ? "{\"compare\":\"less-equal\"}" : mode == 2 ? "{}" : "{\"magFilter\":\"linear\",\"minFilter\":\"linear\",\"mipmapFilter\":\"linear\"}");
    return dummy_sampler[mode];
}

/* ---- typed buffer views are expanded to 16 bytes per element on the CPU ---- */

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, f;
    if (!e) { if (!m) f = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; f = s | e << 23 | m << 13; } }
    else if (e == 31) f = s | 0x7F800000 | m << 13;
    else f = s | (e + 112) << 23 | m << 13;
    float r;
    memcpy(&r, &f, 4);
    return r;
}

static void expand_elem(uint32_t fmt, const uint8_t* s, uint32_t* o)
{
    float* fo = (float*)o;
    /* missing components read as 0, alpha as 1 */
    o[0] = o[1] = o[2] = 0;
    o[3] = 0x3F800000u;
    switch (fmt) {
    case 2: case 3: case 4: memcpy(o, s, 16); break;
    case 6: case 7: case 8: memcpy(o, s, 12); break;
    case 16: case 17: case 18: memcpy(o, s, 8); break;
    case 41: case 42: case 43: memcpy(o, s, 4); break;
    case 10: for (int i = 0; i < 4; i++) fo[i] = half_to_float((uint16_t)(s[i * 2] | s[i * 2 + 1] << 8)); break;
    case 34: for (int i = 0; i < 2; i++) fo[i] = half_to_float((uint16_t)(s[i * 2] | s[i * 2 + 1] << 8)); break;
    case 54: fo[0] = half_to_float((uint16_t)(s[0] | s[1] << 8)); break;
    case 28: for (int i = 0; i < 4; i++) fo[i] = s[i] / 255.0f; break;
    case 87: fo[0] = s[2] / 255.0f; fo[1] = s[1] / 255.0f; fo[2] = s[0] / 255.0f; fo[3] = s[3] / 255.0f; break;
    case 30: for (int i = 0; i < 4; i++) o[i] = s[i]; break;
    case 12: for (int i = 0; i < 4; i++) o[i] = (uint32_t)(s[i * 2] | s[i * 2 + 1] << 8); break;
    case 36: for (int i = 0; i < 2; i++) o[i] = (uint32_t)(s[i * 2] | s[i * 2 + 1] << 8); break;
    case 57: o[0] = (uint32_t)(s[0] | s[1] << 8); break;
    case 62: o[0] = s[0]; break;
    case 61: fo[0] = s[0] / 255.0f; break;
    case 56: fo[0] = (s[0] | s[1] << 8) / 65535.0f; break;
    case 35: for (int i = 0; i < 2; i++) fo[i] = (s[i * 2] | s[i * 2 + 1] << 8) / 65535.0f; break;
    case 11: for (int i = 0; i < 4; i++) fo[i] = (s[i * 2] | s[i * 2 + 1] << 8) / 65535.0f; break;
    case 31: for (int i = 0; i < 4; i++) fo[i] = (int8_t)s[i] < -127 ? -1.0f : (int8_t)s[i] / 127.0f; break;
    case 51: for (int i = 0; i < 2; i++) fo[i] = (int8_t)s[i] < -127 ? -1.0f : (int8_t)s[i] / 127.0f; break;
    case 63: fo[0] = (int8_t)s[0] < -127 ? -1.0f : (int8_t)s[0] / 127.0f; break;
    case 13: for (int i = 0; i < 4; i++) { int16_t v = (int16_t)(s[i * 2] | s[i * 2 + 1] << 8); fo[i] = v < -32767 ? -1.0f : v / 32767.0f; } break;
    case 37: for (int i = 0; i < 2; i++) { int16_t v = (int16_t)(s[i * 2] | s[i * 2 + 1] << 8); fo[i] = v < -32767 ? -1.0f : v / 32767.0f; } break;
    case 58: { int16_t v = (int16_t)(s[0] | s[1] << 8); fo[0] = v < -32767 ? -1.0f : v / 32767.0f; break; }
    case 29: for (int i = 0; i < 4; i++) fo[i] = s[i] / 255.0f; break;
    case 88: fo[0] = s[2] / 255.0f; fo[1] = s[1] / 255.0f; fo[2] = s[0] / 255.0f; fo[3] = 1.0f; break;
    case 49: for (int i = 0; i < 2; i++) fo[i] = s[i] / 255.0f; break;
    case 50: for (int i = 0; i < 2; i++) o[i] = s[i]; break;
    case 32: for (int i = 0; i < 4; i++) o[i] = (uint32_t)(int32_t)(int8_t)s[i]; break;
    case 52: for (int i = 0; i < 2; i++) o[i] = (uint32_t)(int32_t)(int8_t)s[i]; break;
    case 64: o[0] = (uint32_t)(int32_t)(int8_t)s[0]; break;
    case 14: for (int i = 0; i < 4; i++) o[i] = (uint32_t)(int32_t)(int16_t)(s[i * 2] | s[i * 2 + 1] << 8); break;
    case 38: for (int i = 0; i < 2; i++) o[i] = (uint32_t)(int32_t)(int16_t)(s[i * 2] | s[i * 2 + 1] << 8); break;
    case 59: o[0] = (uint32_t)(int32_t)(int16_t)(s[0] | s[1] << 8); break;
    case 24: { uint32_t v = (uint32_t)(s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24);
        fo[0] = (v & 1023) / 1023.0f; fo[1] = ((v >> 10) & 1023) / 1023.0f; fo[2] = ((v >> 20) & 1023) / 1023.0f; fo[3] = (v >> 30) / 3.0f; break; }
    case 25: { uint32_t v = (uint32_t)(s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24);
        o[0] = v & 1023; o[1] = (v >> 10) & 1023; o[2] = (v >> 20) & 1023; o[3] = v >> 30; break; }
    case 26: { uint32_t v = (uint32_t)(s[0] | s[1] << 8 | s[2] << 16 | (uint32_t)s[3] << 24);
        /* 6-bit mantissa floats (5 for blue), same exponent bias as half */
        fo[0] = half_to_float((uint16_t)((v & 2047) << 4)); fo[1] = half_to_float((uint16_t)(((v >> 11) & 2047) << 4)); fo[2] = half_to_float((uint16_t)((v >> 22) << 5)); fo[3] = 1.0f; break; }
    default: memcpy(o, s, 4); break;
    }
}

static int typed_buffer(GpuView* v, uint32_t* size)
{
    WView* w = V(v);
    GpuRes* r = v->res;
    WTex* t = T(r);
    if (!w || !t || (!r->backing && !(t->id && (r->bind & 0x80)))) return 0;
    uint32_t bpe = src_bpb(v->format);
    if (!bpe) bpe = 4;
    uint32_t n = w->num_elem;
    if ((uint64_t)(w->first_elem + n) * bpe > r->width) n = r->width / bpe > w->first_elem ? r->width / bpe - w->first_elem : 0;
    if (!n) n = 1;
    uint32_t bytes = n * 16;
    if (!w->typed_buf || w->typed_buf_size < (int)bytes) {
        if (w->typed_buf) uewg_release(w->typed_buf);
        w->typed_buf = uewg_buffer_create((int)bytes, 8);
        w->typed_buf_size = (int)bytes;
        w->typed_version = t->version - 1;
    }
    if (w->typed_version != t->version && t->id && (r->bind & 0x80)) {
        /* compute shaders may write this buffer: expand from the GPU copy, which also holds every CPU upload */
        uewg_expand_typed(t->id, w->typed_buf, (int)w->first_elem, (int)n, (int)v->format, (int)bpe);
        PASS_BREAK();
        t->used = g_serial;
        w->typed_version = t->version;
    }
    if (w->typed_version != t->version) {
        uint32_t* tmp = malloc(bytes);
        const uint8_t* s = G2H(r->backing) + (uint64_t)w->first_elem * bpe;
        for (uint32_t i = 0; i < n; i++) expand_elem(v->format, s + (uint64_t)i * bpe, tmp + i * 4);
        WTex tt = { .id = w->typed_buf, .used = t->used };
        buffer_put(NULL, &tt, 0, (const uint8_t*)tmp, bytes);
        free(tmp);
        w->typed_version = t->version;
    }
    *size = bytes;
    return w->typed_buf;
}

static int srv_buffer(GpuView* view, const DxSlot* s, uint32_t* off, uint32_t* size)
{
    WTex* t = view ? T(view->res) : NULL;
    *off = *size = 0;
    if (!t || view->res->type != RES_BUFFER) return 0;
    if (!s->raw && !s->structured) return typed_buffer(view, size);
    WView* vw = V(view);
    uint32_t stride = s->raw ? 4 : (view->res->stride ? view->res->stride : 4);
    *off = vw->first_elem * stride;
    *size = vw->num_elem * stride;
    if (*off & 255) { *off = 0; *size = 0; }
    if (!*size || *off + *size > view->res->width) *size = view->res->width - *off;
    *size &= ~3u;
    t->used = g_serial;
    return t->id;
}

static int conflicts(GpuView* v);

/* the buffers past the storage-buffer limit, copied one after another behind a header of (word offset, length) pairs */
static int merged_buffer(GpuCtx* c, Variant* v, int stage, uint32_t* total)
{
    DxWgsl* w = &v->w;
    uint32_t hdr[256] = { 0 };
    struct { int id; uint32_t off, size, dst; } cp[128];
    int ncp = 0;
    uint32_t pos = ((uint32_t)w->nmerged * 8 + 15) & ~15u;
    for (int i = 0; i < 128; i++) {
        DxSlot* s = &w->srv[i];
        if (!s->used || !s->merged) continue;
        GpuView* view = c->srv[stage][i];
        if (view && conflicts(view)) view = NULL;
        uint32_t off, size;
        int id = srv_buffer(view, s, &off, &size);
        if (!id || !size) continue;
        int h = (s->merged - 1) * 2;
        hdr[h] = pos / 4;
        hdr[h + 1] = s->raw || s->structured ? size / 4 : size / 16;
        cp[ncp].id = id; cp[ncp].off = off; cp[ncp].size = size; cp[ncp].dst = pos; ncp++;
        pos += (size + 15) & ~15u;
    }
    if (!v->mbuf || v->mbuf_size < (int)pos) {
        if (v->mbuf) uewg_release(v->mbuf);
        v->mbuf = uewg_buffer_create((int)pos, 8);
        v->mbuf_size = v->mbuf ? (int)pos : 0;
    }
    if (!v->mbuf) return 0;
    WTex tt = { .id = v->mbuf, .used = g_serial };
    buffer_put(NULL, &tt, 0, (const uint8_t*)hdr, (uint32_t)w->nmerged * 8);
    for (int k = 0; k < ncp; k++) uewg_buffer_copy(cp[k].id, (int)cp[k].off, v->mbuf, (int)cp[k].dst, (int)cp[k].size);
    if (ncp) PASS_BREAK();
    *total = pos;
    return v->mbuf;
}

/* ---- bind groups ---- */

typedef struct BgEnt { struct BgEnt* next; uint64_t h; uint64_t bloom[4]; int id; int nids; int ids[]; } BgEnt;
/* a 256-bit filter of the ids a group names: the sweep only searches groups whose filter meets the dead ids' */
static inline void bloom_add(uint64_t* b, int id) { uint32_t k = (uint32_t)id * 0x9E3779B1u >> 24; b[k >> 6] |= 1ull << (k & 63); }
/* ids of resources and views destroyed this frame: groups naming them can never be used again and are freed after the frame,
   since games that make a buffer per frame (feedback buffers) would otherwise fill the browser's descriptor heaps */
static int dead_ids[4096], ndead, dead_over;
static void bg_dead(int id) { if (!id) return; if (ndead < 4096) dead_ids[ndead++] = id; else dead_over = 1; }
static BgEnt* bg_cache[4096];
static int bg_count, bg_full;

static int cmp_int(const void* a, const void* b) { int x = *(const int*)a, y = *(const int*)b; return x < y ? -1 : x > y; }

static void bg_sweep(void);

static void bg_clear(void)
{
    bg_epoch++;
    for (int i = 0; i < 4096; i++)
        while (bg_cache[i]) { BgEnt* e = bg_cache[i]; bg_cache[i] = e->next; uewg_release(e->id); free(e); }
    bg_count = 0;
}

static void bg_sweep(void)
{
    if (dead_over) { bg_clear(); ndead = 0; dead_over = 0; return; }
    if (!ndead) return;
    qsort(dead_ids, (size_t)ndead, sizeof *dead_ids, cmp_int);
    uint64_t dead[4] = { 0 };
    for (int i = 0; i < ndead; i++) bloom_add(dead, dead_ids[i]);
    int freed = 0;
    for (int i = 0; i < 4096; i++)
        for (BgEnt** pe = &bg_cache[i]; *pe;) {
            BgEnt* e = *pe;
            if (!((e->bloom[0] & dead[0]) | (e->bloom[1] & dead[1]) | (e->bloom[2] & dead[2]) | (e->bloom[3] & dead[3]))) { pe = &e->next; continue; }
            int hit = 0;
            for (int k = 0; k < e->nids && !hit; k++) hit = bsearch(&e->ids[k], dead_ids, (size_t)ndead, sizeof *dead_ids, cmp_int) != NULL;
            if (hit) { *pe = e->next; uewg_release(e->id); free(e); bg_count--; freed++; }
            else pe = &e->next;
        }
    ndead = 0;
    /* memos may name the freed groups */
    if (freed) bg_epoch++;
}

/* a view read while the same subresources are a render target; other mips or layers of it are fine */
static int overlaps(GpuView* a, GpuView* t)
{
    WView *x = V(a), *y = V(t);
    if (!x || !y) return 1;
    if (y->mip < x->mip || y->mip >= x->mip + x->mips) return 0;
    if (t->res->type == RES_TEX3D || a->res->type == RES_TEX3D) return 1;
    return y->layer < x->layer + x->layers && x->layer < y->layer + y->layers;
}

static int conflicts(GpuView* v)
{
    GpuRes* r = v ? v->res : NULL;
    if (!r) return 0;
    for (int i = 0; i < 8; i++) if (pass_rt[i] && pass_rt[i]->res == r && overlaps(v, pass_rt[i])) return 1;
    if (pass_ds && pass_ds->res == r && !(V(pass_ds) && V(pass_ds)->read_only) && overlaps(v, pass_ds)) return 1;
    return 0;
}

static int res_group_ex(GpuCtx* c, Program* p, Variant* v, int stage, int group, const char* extra);
static int res_group(GpuCtx* c, Program* p, Variant* v, int stage, int group) { return res_group_ex(c, p, v, stage, group, NULL); }

static int temp_bg[4096], ntemp;

/* a bind group entry; groups are cached by a hash of these, JSON is only made for new ones */
typedef struct { int b, kind, id; uint32_t off, size; int dim, ret; } Ent;
enum { E_ID, E_BUF, E_DUMMY_TEX, E_DUMMY_BUF, E_DUMMY_SBUF, E_DUMMY_STEX };

static void ent_json(Buf* b, const Ent* e, int n)
{
    for (int i = 0; i < n; i++, e++) {
        const char* sep = i ? "," : "";
        switch (e->kind) {
        case E_ID: bp(b, "%s{\"b\":%d,\"id\":%d}", sep, e->b, e->id); break;
        case E_BUF: bp(b, "%s{\"b\":%d,\"buf\":%d,\"off\":%u,\"size\":%u}", sep, e->b, e->id, e->off, e->size); break;
        case E_DUMMY_TEX: bp(b, "%s{\"b\":%d,\"dummy\":\"%s:%s\"}", sep, e->b, dim_name(e->dim), e->ret == 3 ? "depth" : e->ret == 1 ? "sint" : e->ret == 2 ? "uint" : "float"); break;
        case E_DUMMY_BUF: bp(b, "%s{\"b\":%d,\"dummy\":\"buf\"}", sep, e->b); break;
        case E_DUMMY_SBUF: bp(b, "%s{\"b\":%d,\"dummy\":\"sbuf\"}", sep, e->b); break;
        case E_DUMMY_STEX: bp(b, "%s{\"b\":%d,\"dummy\":\"stex\"}", sep, e->b); break;
        }
    }
}

/* resource groups by what is bound (views, samplers, which views clash with the targets), kept across passes and frames
   until a view or resource is freed (bg_epoch) */
typedef struct RCache { uint64_t h; Variant* v; uint32_t epoch; int bg, nbufs; struct WTex* bufs[16]; } RCache;
static RCache rcache[8192];

static int res_group_ex(GpuCtx* c, Program* p, Variant* v, int stage, int group, const char* extra)
{
    DxWgsl* w = &v->w;
    /* same views and samplers as last time, same targets: same group */
    Memo* mo = &v->memo;
    variant_slots(v);
    void* refs[160];
    int nr = 0, memo_ok = !extra && !v->nuav && !w->nmerged;
    for (int k = 0; k < v->nsrv && memo_ok; k++) {
        int i = v->srv_ix[k];
        GpuView* vw = c->srv[stage][i];
        if (vw && vw->res && vw->res->type == RES_BUFFER && !w->srv[i].raw && !w->srv[i].structured) memo_ok = 0;
        refs[nr++] = vw;
    }
    for (int k = 0; k < v->nsamp && memo_ok; k++) refs[nr++] = c->samp[stage][v->samp_ix[k]];
    if (memo_ok && mo->valid && mo->layout == p->layout && mo->group == group && mo->pass_gen == pass_gen && mo->bg_epoch == bg_epoch &&
        mo->n == nr && !memcmp(mo->refs, refs, sizeof *refs * (size_t)nr)) {
        for (int i = 0; i < mo->nbufs; i++) mo->bufs[i]->used = g_serial;
        st_bg_memo++;
        return mo->bg;
    }
    RCache* rc = NULL;
    uint64_t rh = 0;
    if (memo_ok) {
        uint32_t clash[5] = { 0 };
        for (int k = 0; k < v->nsrv; k++) if (refs[k] && conflicts((GpuView*)refs[k])) clash[k >> 5] |= 1u << (k & 31);
        uint64_t seed = fnv(clash, sizeof clash, (uint64_t)(uintptr_t)v * 0x9E3779B97F4A7C15ull ^ ((uint64_t)p->layout << 40) ^ ((uint64_t)group << 32) ^ bg_epoch);
        rh = fnv(refs, sizeof *refs * (size_t)nr, seed) | 1;
        rc = &rcache[rh & 8191];
        if (!dbg_flag("norescache", &no_rcache) && rc->h == rh && rc->v == v && rc->epoch == bg_epoch) {
            for (int i = 0; i < rc->nbufs; i++) rc->bufs[i]->used = g_serial;
            mo->valid = 1; mo->layout = p->layout; mo->group = group; mo->pass_gen = pass_gen; mo->bg_epoch = bg_epoch; mo->n = nr;
            memcpy(mo->refs, refs, sizeof *refs * (size_t)nr);
            mo->nbufs = rc->nbufs;
            memcpy(mo->bufs, rc->bufs, sizeof *rc->bufs * (size_t)rc->nbufs);
            mo->bg = rc->bg;
            st_bg_rc++;
            return rc->bg;
        }
    }
    static Ent ents[160];
    int n = 0;
#define ENT(...) do { ents[n] = (Ent){ __VA_ARGS__ }; n++; } while (0)
    for (int k = 0; k < v->nsrv; k++) {
        int i = v->srv_ix[k];
        DxSlot* s = &w->srv[i];
        GpuView* view = c->srv[stage][i];
        if (view && conflicts(view)) view = NULL;
        WTex* t = view ? T(view->res) : NULL;
        if (s->merged) continue;
        if (s->buffer && s->fmt && !s->raw && !s->structured) {
            WView* vw = view ? V(view) : NULL;
            int bpe = dxbc_wgsl_buf_bpe(s->fmt);
            uint64_t off = vw ? (uint64_t)vw->first_elem * (uint32_t)bpe : 0, size = vw ? (uint64_t)vw->num_elem * (uint32_t)bpe : 0;
            if (t && t->id && view->res->type == RES_BUFFER && !(off & 255) && off < view->res->width) {
                if (!size || off + size > view->res->width) size = view->res->width - off;
                size = (size + 3) & ~3ull;
                if (off + size > view->res->width) size = (view->res->width - off) & ~3ull;
                t->used = g_serial;
                if (size) { ENT(16 + i, E_BUF, t->id, (uint32_t)off, (uint32_t)size); continue; }
            }
            ENT(16 + i, E_DUMMY_BUF);
        } else if (s->buffer) {
            uint32_t off = 0, size = 0;
            int id = srv_buffer(view, s, &off, &size);
            if (id && size) ENT(16 + i, E_BUF, id, off, size);
            else ENT(16 + i, E_DUMMY_BUF);
        } else {
            int id = 0;
            if (t && view->res->type != RES_BUFFER) {
                WView* vw = V(view);
                int want_depth = s->compare != 0;
                if (vw && vw->depth_aspect == want_depth) id = view_for(view, s->dim);
                t->used = g_serial;
            }
            if (id) ENT(16 + i, E_ID, id);
            else {
                /* why a placeholder is bound instead of the game's texture (once per reason and slot) */
                static int said, dbg = -1;
                if (dbg < 0) dbg = host_query_flag("texdebug");
                if (dbg && said < 60) {
                    GpuView* orig = c->srv[stage][i];
                    WView* ov = V(orig);
                    const char* why = !orig ? "nothing bound" : !orig->res ? "no resource" : conflicts(orig) ? "also a render target"
                                    : !T(orig->res) ? "no texture" : !ov ? "no view" : ov->depth_aspect != (s->compare != 0) ? "depth aspect" : "view_for failed";
                    host_log("[tex] stage %d slot %d: placeholder (%s) shader dim %d view dim %u fmt %u res type %d", stage, i, why, s->dim,
                             orig ? orig->dim : 0, orig ? orig->format : 0, orig && orig->res ? orig->res->type : -1);
                    said++;
                }
                ENT(16 + i, E_DUMMY_TEX, 0, 0, 0, s->dim, s->compare ? 3 : s->ret);
            }
        }
    }
    if (w->nmerged) {
        uint32_t size = 0;
        int id = merged_buffer(c, v, stage, &size);
        if (id) ENT(176, E_BUF, id, 0, size);
        else ENT(176, E_DUMMY_BUF);
    }
    for (int k = 0; k < v->nsamp; k++) {
        int i = v->samp_ix[k];
        ENT(160 + i, E_ID, sampler_id(c->samp[stage][i], w->samp[i].compare ? 1 : w->samp[i].nofilter ? 2 : 0));
    }
    for (int k = 0; k < v->nuav; k++) {
        int i = v->uav_ix[k];
        DxSlot* u = &w->uav[i];
        GpuView* view = stage == SH_PS ? c->ps_uav[i] : c->uav[i];
        WTex* t = view ? T(view->res) : NULL;
        int id = 0;
        if (t && t->id) {
            t->used = g_serial;
            t->version++;
            t->gpu_written = 1;
            if (view->res->type == RES_BUFFER) {
                WView* vw = V(view);
                uint32_t stride = view->res->stride && (u->structured) ? view->res->stride : !u->raw && src_bpb(view->format) >= 4 ? src_bpb(view->format) : 4;
                uint32_t off = vw->first_elem * stride, size = vw->num_elem * stride;
                if ((off & 255) || !size || off + size > view->res->width) { off = 0; size = view->res->width; }
                ENT(180 + i, E_BUF, t->id, off, size & ~3u);
                continue;
            }
            WView* vw = V(view);
            uint32_t save = vw->mips;
            vw->mips = 1;
            id = view_for(view, u->dim == 2 ? 2 : u->dim == 5 ? 5 : u->dim == 8 ? 8 : 3);
            vw->mips = save;
        }
        if (id) ENT(180 + i, E_ID, id);
        else ENT(180 + i, u->raw || u->structured || u->dim == 1 ? E_DUMMY_SBUF : E_DUMMY_STEX);
    }
#undef ENT
    static Buf rb;
    if (extra) {
        /* per-draw entries: not cached */
        breset(&rb);
        bp(&rb, "[");
        ent_json(&rb, ents, n);
        bp(&rb, "%s%s]", n ? "," : "", extra);
        int id = uewg_bindgroup_create(p->layout, group, rb.p);
        if (ntemp < 4096) temp_bg[ntemp++] = id;
        return id;
    }
    if (!n) return empty_bg;
    uint64_t h = fnv(ents, sizeof *ents * (size_t)n, (uint64_t)p->layout * 0x9E3779B97F4A7C15ull + (uint64_t)group);
    BgEnt** slot = &bg_cache[h & 4095];
    int found = 0;
    for (BgEnt* e = *slot; e; e = e->next) if (e->h == h) { found = e->id; break; }
    if (memo_ok) {
        /* remember it, with the buffers whose later CPU writes must stay ordered after this draw */
        mo->valid = 1; mo->layout = p->layout; mo->group = group; mo->pass_gen = pass_gen; mo->n = nr;
        memcpy(mo->refs, refs, sizeof *refs * (size_t)nr);
        mo->nbufs = 0;
        for (int i = 0; i < 128 && mo->nbufs < 16; i++) {
            GpuView* vw = w->srv[i].used ? c->srv[stage][i] : NULL;
            if (vw && vw->res && vw->res->type == RES_BUFFER && T(vw->res)) mo->bufs[mo->nbufs++] = T(vw->res);
        }
        if (mo->nbufs == 16) mo->valid = 0;
    }
#define RC_PUT(id) do { if (rc && mo->valid) { rc->h = rh; rc->v = v; rc->epoch = bg_epoch; rc->bg = (id); rc->nbufs = mo->nbufs; memcpy(rc->bufs, mo->bufs, sizeof *mo->bufs * (size_t)mo->nbufs); } } while (0)
    if (found) { st_bg_hash++; if (memo_ok && mo->valid) { mo->bg = found; mo->bg_epoch = bg_epoch; } RC_PUT(found); return found; }
    st_bg_new++;
    { static int dbg = -1, said; static uint64_t first; if (dbg < 0) dbg = host_query_flag("bgdebug");
      if (!first) first = host_ticks_ns();
      if (dbg && said < 400 && host_ticks_ns() - first > 45000000000ull) { said++; char line[600]; int o = snprintf(line, sizeof line, "[bg] new v=%p g=%d:", (void*)v, group);
        for (int k = 0; k < n && o < 560; k++) o += snprintf(line + o, sizeof line - o, " %d:%d/%d/%u", ents[k].b, ents[k].kind, ents[k].id, ents[k].off);
        host_log("%s", line); } }
    /* a full cache is emptied after the frame: groups already handed to this draw must stay alive */
    /* every live bind group holds descriptors in the browser's D3D12 heaps; too many and the device is lost */
    if (bg_count > 8192) bg_full = 1;
    BgEnt* e = malloc(sizeof *e + sizeof(int) * (size_t)n);
    e->h = h;
    e->nids = 0;
    memset(e->bloom, 0, sizeof e->bloom);
    for (int k = 0; k < n; k++) if ((ents[k].kind == E_ID && ents[k].b < 160) || ents[k].kind == E_BUF || ents[k].b >= 176) if (ents[k].id) {
        e->ids[e->nids++] = ents[k].id;
        bloom_add(e->bloom, ents[k].id);
    }
    _Static_assert(sizeof(Ent) == 7 * sizeof(int), "Ent is read as 7 ints by uewg_bindgroup_bin");
    e->id = uewg_bindgroup_bin(p->layout, group, ents, n);
    e->next = *slot;
    *slot = e;
    bg_count++;
    if (memo_ok && mo->valid) { mo->bg = e->id; mo->bg_epoch = bg_epoch; }
    RC_PUT(e->id);
#undef RC_PUT
    return e->id;
}

static int cb_offsets(GpuCtx* c, Program* p, Variant* v, int stage, int s, int32_t* out)
{
    int n = 0;
    for (int k = 0; k < p->ncb[s]; k++) {
        int i = p->cb_slots[s][k];
        uint32_t want = (uint32_t)v->w.cb_size[i] * 16;
        GpuRes* r = c->cb[stage][i];
        uint32_t first = c->cb_first[stage][i] * 16, have = 0;
        const uint8_t* src = NULL;
        if (r && r->backing) {
            src = G2H(r->backing) + first;
            have = r->width > first ? r->width - first : 0;
            if (c->cb_num[stage][i] && c->cb_num[stage][i] * 16 < have) have = c->cb_num[stage][i] * 16;
        }
        if (have > want) have = want;
        WTex* t = r ? T(r) : NULL;
        if (t && ring_live(t->cb_serial, t->cb_off) && t->cb_version == t->version && t->cb_first == first && t->cb_size == want) {
            out[n++] = (int32_t)t->cb_off;
            st_cb_hit++;
            continue;
        }
        /* rewritten with the same contents (engines refresh constants that did not change): keep the copy from this frame,
           which also keeps the dynamic offset, so the browser does not rebind */
        if (t && src && ring_live(t->cb_serial, t->cb_off) && t->cb_first == first && t->cb_size == want && !memcmp(src, ring + t->cb_off, have)) {
            t->cb_version = t->version;
            out[n++] = (int32_t)t->cb_off;
            st_cb_same++;
            continue;
        }
        /* the same contents already copied this frame from another buffer (per-draw constants often repeat): reuse that
           copy. Found by the size and first bytes, then compared whole */
        if (src && have) {
            static struct { uint64_t key; uint32_t gen, off, want, have; } seen[4096];
            uint64_t key = fnv(src, have < 64 ? have : 64, ((uint64_t)want << 32) | have);
            uint32_t slot = (uint32_t)(key >> 52) & 4095;
            if (seen[slot].key == key && seen[slot].want == want && seen[slot].have == have && ring_live(seen[slot].gen, seen[slot].off)
                && !memcmp(src, ring + seen[slot].off, have)) {
                out[n++] = (int32_t)seen[slot].off;
                st_cb_dedup++;
                if (t) { t->cb_serial = seen[slot].gen; t->cb_version = t->version; t->cb_first = first; t->cb_size = want; t->cb_off = seen[slot].off; }
                continue;
            }
            st_cb_miss++;
            out[n++] = (int32_t)ring_put(src, have, want);
            seen[slot].key = key; seen[slot].gen = ring_gen; seen[slot].off = (uint32_t)out[n - 1]; seen[slot].want = want; seen[slot].have = have;
            if (t) { t->cb_serial = ring_gen; t->cb_version = t->version; t->cb_first = first; t->cb_size = want; t->cb_off = (uint32_t)out[n - 1]; }
            continue;
        }
        st_cb_miss++;
        out[n++] = (int32_t)ring_put(src, have, want);
        if (t) { t->cb_serial = ring_gen; t->cb_version = t->version; t->cb_first = first; t->cb_size = want; t->cb_off = (uint32_t)out[n - 1]; }
    }
    return n;
}

/* temp_bg: bind groups made for a single draw, dropped at present */

/* constant buffer group for this draw: writes the dynamic offsets to dyn, returns the group */
static int cb_bind(GpuCtx* c, Program* p, Variant* v, int stage, int s, int32_t* dyn, int* ndyn)
{
    int32_t off[16];
    int n = cb_offsets(c, p, v, stage, s, off);
    if (!p->cb_static[s]) {
        memcpy(dyn, off, sizeof *off * (size_t)n);
        *ndyn = n;
        return p->cb_group[s] ? p->cb_group[s] : empty_bg;
    }
    st_static++;
    static Buf b;
    breset(&b);
    bp(&b, "[");
    int nd = 0;
    for (int k = 0; k < n; k++) {
        int i = p->cb_slots[s][k];
        if (p->cb_static[s] >> k & 1) bp(&b, "%s{\"b\":%d,\"ring\":1,\"off\":%d,\"size\":%d}", k ? "," : "", i, off[k], v->w.cb_size[i] * 16);
        else { bp(&b, "%s{\"b\":%d,\"ring\":1,\"size\":%d}", k ? "," : "", i, v->w.cb_size[i] * 16); dyn[nd++] = off[k]; }
    }
    bp(&b, "]");
    int id = uewg_bindgroup_create(p->layout, s * 2, b.p);
    if (ntemp < 4096) temp_bg[ntemp++] = id;
    *ndyn = nd;
    return id;
}

/* ---- render passes ---- */

/* D3D draws with no render target and no depth buffer (pixel shaders writing UAVs only); WebGPU passes need an
   attachment, so those passes get a shared scratch target the size of the viewport that nothing is written to */
static int null_view;
static uint32_t null_w, null_h;
static int no_targets(GpuCtx* c)
{
    for (int i = 0; i < 8; i++) if (c->rtv[i] && V(c->rtv[i]) && V(c->rtv[i])->rt_view) return 0;
    return !(c->dsv && V(c->dsv) && V(c->dsv)->rt_view);
}
static void null_target(GpuCtx* c)
{
    uint32_t w = 1, h = 1;
    for (int i = 0; i < (c->nvp ? c->nvp : 0); i++) {
        float x = c->vp[i].x + c->vp[i].w, y = c->vp[i].y + c->vp[i].h;
        if (x > (float)w) w = (uint32_t)(x + 0.999f);
        if (y > (float)h) h = (uint32_t)(y + 0.999f);
    }
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    if (null_view && w <= null_w && h <= null_h) return;
    if (w < null_w) w = null_w;
    if (h < null_h) h = null_h;
    breset(&jb);
    bp(&jb, "{\"w\":%u,\"h\":%u,\"d\":1,\"mips\":1,\"format\":\"r8unorm\",\"dim\":\"2d\",\"rt\":1,\"uav\":0}", w, h);
    int tex = uewg_texture_create(jb.p);
    breset(&jb);
    bp(&jb, "{\"tex\":%d,\"dim\":\"2d\",\"mip\":0,\"mips\":1,\"layer\":0,\"layers\":1,\"aspect\":\"all\"}", tex);
    null_view = uewg_view_create(jb.p);
    null_w = w; null_h = h;
    PASS_BREAK();
}

static void ensure_pass(GpuCtx* c, uint32_t* w, uint32_t* h)
{
    int none = no_targets(c);
    if (none) null_target(c);
    int same = pass_open && pass_ds == c->dsv;
    for (int i = 0; i < 8 && same; i++) if (pass_rt[i] != c->rtv[i]) same = 0;
    *w = 0; *h = 0;
    for (int i = 0; i < 8; i++) {
        GpuView* v = c->rtv[i];
        if (v && v->res) { WView* vw = V(v); uint32_t m = vw ? vw->mip : 0; *w = v->res->width >> m; *h = v->res->height >> m; break; }
    }
    if (!*w && c->dsv && c->dsv->res) { WView* vw = V(c->dsv); uint32_t m = vw ? vw->mip : 0; *w = c->dsv->res->width >> m; *h = c->dsv->res->height >> m; }
    if (none) { *w = null_w; *h = null_h; }
    if (!*w) *w = 1;
    if (!*h) *h = 1;
    if (same) return;
    memcpy(pass_rt, c->rtv, sizeof pass_rt);
    pass_gen++;
    pass_ds = c->dsv;
    breset(&jb);
    bp(&jb, "{\"color\":[");
    int last = -1;
    for (int i = 0; i < 8; i++) if (c->rtv[i] && V(c->rtv[i]) && V(c->rtv[i])->rt_view) last = i;
    for (int i = 0; i <= last; i++) {
        WView* vw = c->rtv[i] ? V(c->rtv[i]) : NULL;
        if (vw && vw->rt_view) bp(&jb, "%s{\"view\":%d,\"slice\":%d}", i ? "," : "", vw->rt_view, vw->slice);
        else bp(&jb, "%snull", i ? "," : "");
    }
    if (none && null_view) bp(&jb, "{\"view\":%d,\"slice\":-1}", null_view);
    bp(&jb, "]");
    WView* dv = c->dsv ? V(c->dsv) : NULL;
    if (dv && dv->rt_view) {
        WTex* t = T(c->dsv->res);
        bp(&jb, ",\"depth\":{\"view\":%d,\"stencil\":%d,\"ro\":%d}", dv->rt_view, t && (t->fmt.flags & FF_STENCIL) ? 1 : 0, dv->read_only);
    }
    bp(&jb, "}");
    uewg_begin_pass(jb.p);
    pass_open = 1;
}

static int temp_views[4096], ntemp_views;

static void clear_rtv(GpuView* v, const float* rgba)
{
    WView* w = V(v);
    if (!g_ok || !w || !w->rt_view) return;
    breset(&jb);
    bp(&jb, "{\"color\":[{\"view\":%d,\"slice\":%d,\"clear\":[%g,%g,%g,%g]}]}", w->rt_view, w->slice, rgba[0], rgba[1], rgba[2], rgba[3]);
    uewg_clear(jb.p, (int)ring_used);
    PASS_BREAK();
}

static void clear_dsv(GpuView* v, uint32_t flags, float depth, uint8_t stencil)
{
    WView* w = V(v);
    if (!g_ok || !w || !w->rt_view) return;
    WTex* t = T(v->res);
    int has_st = t && (t->fmt.flags & FF_STENCIL);
    breset(&jb);
    bp(&jb, "{\"color\":[],\"depth\":{\"view\":%d,\"stencil\":%d", w->rt_view, has_st);
    if (flags & 1) bp(&jb, ",\"dclear\":%g", depth);
    if ((flags & 2) && has_st) bp(&jb, ",\"sclear\":%u", stencil);
    bp(&jb, "}}");
    uewg_clear(jb.p, (int)ring_used);
    PASS_BREAK();
}

/* ---- pipelines ---- */

static void depth_state(Buf* b, GpuCtx* c, const char* fmt, int has_stencil, int ro)
{
    static const uint32_t def[13] = { 1, 1, 2, 0, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8 };
    const uint32_t* d = c->depth ? (const uint32_t*)c->depth->desc : def;
    int en = (int)d[0];
    int write = en && d[1] && !(ro & 1);
    static int depth_always = -1;
    if (dbg_flag("depthalways", &depth_always) && !write) en = 0;
    bp(b, ",\"depth\":{\"format\":\"%s\",\"depthWriteEnabled\":%s,\"depthCompare\":\"%s\"", fmt, write ? "true" : "false", en ? cmp_name(d[2]) : "always");
    if (has_stencil && d[3]) {
        uint8_t rm = (uint8_t)(d[4] & 0xFF), wm = (uint8_t)((d[4] >> 8) & 0xFF);
        if (ro & 2) wm = 0;
        bp(b, ",\"stencilReadMask\":%u,\"stencilWriteMask\":%u", rm, wm);
        for (int f = 0; f < 2; f++) {
            const uint32_t* s = d + 5 + f * 4;
            bp(b, ",\"stencil%s\":{\"failOp\":\"%s\",\"depthFailOp\":\"%s\",\"passOp\":\"%s\",\"compare\":\"%s\"}", f ? "Back" : "Front",
               stencil_op(s[0]), stencil_op(s[1]), stencil_op(s[2]), cmp_name(s[3]));
        }
    }
    const uint32_t* r = c->raster ? (const uint32_t*)c->raster->desc : NULL;
    if (r && c->topology >= 4) {
        float clamp, slope;
        memcpy(&clamp, &r[4], 4);
        memcpy(&slope, &r[5], 4);
        bp(b, ",\"depthBias\":%d,\"depthBiasSlopeScale\":%g,\"depthBiasClamp\":%g", (int32_t)r[3], slope, clamp);
    }
    bp(b, "}");
}

static int render_pipeline(GpuCtx* c, Program* p, PipeKey* k)
{
    uint64_t h = fnv(k, sizeof *k, 0xcbf29ce484222325ull);
    Pipe** slot = &pipes[h & 1023];
    for (Pipe* e = *slot; e; e = e->next) if (e->h == h && !memcmp(&e->k, k, sizeof *k)) return e->id;
    Buf b = { 0 };
    bp(&b, "{\"layout\":%d,\"vs\":%d", p->layout, p->vs->module);
    int any_rt = 0;
    for (int i = 0; i < 8; i++) if (k->rt_fmt[i]) any_rt = 1;
    /* D3D allows a depth-only draw with colour targets bound; WebGPU wants a fragment stage that lists them */
    static int empty_fs;
    if (!p->ps && any_rt && !empty_fs) empty_fs = uewg_shader_create("@fragment fn main() {}");
    if (p->ps) bp(&b, ",\"ps\":%d", p->ps->module);
    else if (any_rt) bp(&b, ",\"ps\":%d", empty_fs);
    bp(&b, ",\"buffers\":[");
    GpuLayout* l = p->kind == 2 ? NULL : c->layout;
    int maxslot = -1;
    if (l) for (uint32_t i = 0; i < l->count; i++) if ((int)l->el[i].slot > maxslot && l->el[i].slot < 16) maxslot = (int)l->el[i].slot;
    const DxSig* in = &p->vs->w.in;
    for (int s = 0; s <= maxslot; s++) {
        Buf a = { 0 };
        int inst = 0, any = 0;
        uint32_t auto_off = 0;
        for (uint32_t i = 0; i < l->count; i++) {
            if (l->el[i].slot != (uint32_t)s) { continue; }
            uint32_t off = l->el[i].offset;
            const char* vf = vfmt(l->el[i].format);
            uint32_t fb = src_bpb(l->el[i].format);
            if (off == 0xFFFFFFFF) off = auto_off;
            auto_off = off + fb;
            if (l->el[i].per_instance) inst = 1;
            int loc = -1;
            for (int e = 0; e < in->n; e++)
                if (!in->e[e].sysval && in->e[e].index == l->el[i].index && !strcasecmp(in->e[e].name, l->el[i].semantic)) { loc = (int)in->e[e].reg; break; }
            if (loc < 0 || !vf) continue;
            if (k->stride[s] && off + fb > k->stride[s]) continue;
            bp(&a, "%s{\"format\":\"%s\",\"offset\":%u,\"shaderLocation\":%d}", any ? "," : "", vf, off, loc);
            any = 1;
        }
        if (any) bp(&b, "%s{\"arrayStride\":%u,\"stepMode\":\"%s\",\"attributes\":[%s]}", s ? "," : "", k->stride[s], inst ? "instance" : "vertex", a.p);
        else bp(&b, "%snull", s ? "," : "");
        free(a.p);
    }
    bp(&b, "]");
    if (p->ps || any_rt) {
        bp(&b, ",\"targets\":[");
        int last = -1;
        for (int i = 0; i < 8; i++) if (k->rt_fmt[i]) last = i;
        const uint32_t* bd = k->blend ? (const uint32_t*)k->blend->desc : NULL;
        int indep = bd ? (int)bd[1] : 0;
        for (int i = 0; i <= last; i++) {
            if (!k->rt_fmt[i]) { bp(&b, "%snull", i ? "," : ""); continue; }
            TexFmt f = texfmt(k->rt_fmt[i], 0);
            int written = 0;
            if (p->ps)
                for (int e = 0; e < p->ps->w.out.n; e++) if (!strcasecmp(p->ps->w.out.e[e].name, "SV_Target") && p->ps->w.out.e[e].index == (uint32_t)i) written = 1;
            const uint32_t* rt = bd ? bd + 2 + (indep ? i : 0) * 8 : NULL;
            uint32_t mask = rt ? rt[7] & 15 : 15;
            if (!written) mask = 0;
            bp(&b, "%s{\"format\":\"%s\",\"writeMask\":%u", i ? "," : "", f.name, mask);
            if (rt && rt[0] && !(f.flags & FF_NOBLEND)) {
                int dual = 0;
                bp(&b, ",\"blend\":{\"color\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"%s\"},\"alpha\":{\"srcFactor\":\"%s\",\"dstFactor\":\"%s\",\"operation\":\"%s\"}}",
                   rt[3] >= 4 && rt[3] <= 5 ? "one" : blend_factor(rt[1], dual), rt[3] >= 4 && rt[3] <= 5 ? "one" : blend_factor(rt[2], dual), blend_op(rt[3]),
                   rt[6] >= 4 && rt[6] <= 5 ? "one" : blend_factor(rt[4], dual), rt[6] >= 4 && rt[6] <= 5 ? "one" : blend_factor(rt[5], dual), blend_op(rt[6]));
            }
            bp(&b, "}");
        }
        bp(&b, "]");
    }
    static const char* topo[] = { "triangle-list", "point-list", "line-list", "line-strip", "triangle-list", "triangle-strip" };
    const char* tp = k->topology < 6 ? topo[k->topology] : "triangle-list";
    const uint32_t* r = k->raster ? (const uint32_t*)k->raster->desc : NULL;
    uint32_t cull = r ? r[1] : 3;
    static int no_cull = -1;
    if (dbg_flag("nocull", &no_cull)) cull = 1;
    bp(&b, ",\"primitive\":{\"topology\":\"%s\",\"cullMode\":\"%s\",\"frontFace\":\"%s\"", tp, cull == 2 ? "front" : cull == 3 ? "back" : "none", r && r[2] ? "ccw" : "cw");
    if (!strcmp(tp, "triangle-strip") || !strcmp(tp, "line-strip")) bp(&b, ",\"stripIndexFormat\":\"%s\"", k->strip32 ? "uint32" : "uint16");
    if (r && !r[6]) bp(&b, ",\"unclippedDepth\":true");
    bp(&b, "}");
    if (k->ds_fmt) {
        TexFmt f = texfmt(k->ds_fmt, 1);
        depth_state(&b, c, f.name, (f.flags & FF_STENCIL) != 0, k->ds_ro);
    }
    bp(&b, ",\"sampleMask\":%u}", k->sample_mask);
    int id = uewg_render_pipeline_create(b.p);
    free(b.p);
    Pipe* e = calloc(1, sizeof *e);
    e->k = *k;
    e->h = h;
    e->id = id;
    e->next = *slot;
    *slot = e;
    return id;
}

/* which bound shader resources are depth textures: only the slots the stage's shader reads matter */
static void depth_mask(GpuCtx* c, int stage, uint32_t m[8])
{
    memset(m, 0, 32);
    WShader* ws = c->sh[stage] ? c->sh[stage]->backend : NULL;
    int all = !ws || !ws->refl_ok || dbg_flag("fulldepthmask", &full_dmask), n = all ? 128 : ws->nsrv;
    for (int k = 0; k < n; k++) {
        int i = all ? k : ws->srv_ix[k];
        GpuView* v = c->srv[stage][i];
        WView* w = V(v);
        if (w && w->depth_aspect && v->res && v->res->type != RES_BUFFER) m[i >> 5] |= 1u << (i & 31);
        /* an integer view of a 32-bit float texture (R32_TYPELESS read as R32_UINT): the shader declares it f32 */
        WTex* t = v && v->res && v->res->type != RES_BUFFER ? T(v->res) : NULL;
        if (t && t->fmt.name && !(t->fmt.flags & FF_DEPTH) && strstr(t->fmt.name, "32float") && ws && ws->refl_ok && ws->refl.srv[i].ret && !ws->refl.srv[i].compare)
            m[4 + (i >> 5)] |= 1u << (i & 31);
    }
}

static int32_t pk[4096];

/* why draws were dropped, reported every 600 frames */
static const char* drop_why[] = { "unsupported stage", "topology", "vertex shader", "pixel shader", "pipeline", "viewport", "index buffer" };
static uint32_t drops[7], drawn;
#define DROP(i) do { drops[i]++; return; } while (0)


static void key_targets(GpuCtx* c, PipeKey* k)
{
    k->blend = c->blend;
    k->depth = c->depth;
    k->raster = c->raster;
    for (int i = 0; i < 8; i++) if (c->rtv[i] && V(c->rtv[i]) && V(c->rtv[i])->rt_view) k->rt_fmt[i] = T(c->rtv[i]->res) ? c->rtv[i]->res->format : 0;
    for (int i = 0; i < 8; i++) if (k->rt_fmt[i]) { TexFmt f = texfmt(c->rtv[i]->format, 0); TexFmt rf = texfmt(k->rt_fmt[i], 0);
        if (f.name && rf.name && rf.view_pair && !strcmp(f.name, rf.view_pair)) k->rt_fmt[i] = c->rtv[i]->format;
        /* the texture may be stored in a substitute format (see res_create) */
        const char* real = T(c->rtv[i]->res)->fmt.name;
        if (rf.name && real && strcmp(rf.name, real)) k->rt_fmt[i] = !strcmp(real, "r32float") ? 41 : !strcmp(real, "rg32float") ? 16 : 10; }
    if (c->dsv && V(c->dsv) && V(c->dsv)->rt_view) { k->ds_fmt = c->dsv->res->format; k->ds_ro = (uint8_t)V(c->dsv)->read_only; }
    if (no_targets(c)) k->rt_fmt[0] = 61;
    k->sample_mask = c->sample_mask ? c->sample_mask : 0xFFFFFFFFu;
}

/* group count, then per group: id, dynamic offset count, offsets */
static int put_groups(GpuCtx* c, Program* p, Variant* vs, Variant* ps, int n)
{
    pk[n++] = 4;
    for (int s = 0; s < 2; s++) {
        Variant* v = s ? ps : vs;
        int stage = s ? SH_PS : SH_VS;
        if (!v) { pk[n++] = empty_bg; pk[n++] = 0; pk[n++] = empty_bg; pk[n++] = 0; continue; }
        int nd;
        pk[n] = cb_bind(c, p, v, stage, s, &pk[n + 2], &nd);
        pk[n + 1] = nd;
        n += 2 + nd;
        pk[n++] = res_group(c, p, v, stage, s * 2 + 1);
        pk[n++] = 0;
    }
    return n;
}

/* viewport, scissor, stencil reference, blend constant; -1 when the viewport is empty */
static int put_fixed(GpuCtx* c, uint32_t w, uint32_t h, int n)
{
    GpuViewport vp = c->nvp ? c->vp[0] : (GpuViewport){ 0, 0, (float)w, (float)h, 0, 1 };
    if (vp.x < 0) { vp.w += vp.x; vp.x = 0; }
    if (vp.y < 0) { vp.h += vp.y; vp.y = 0; }
    if (vp.x + vp.w > (float)w) vp.w = (float)w - vp.x;
    if (vp.y + vp.h > (float)h) vp.h = (float)h - vp.y;
    if (vp.w <= 0 || vp.h <= 0) return -1;
    if (vp.minz < 0) vp.minz = 0;
    if (vp.maxz > 1) vp.maxz = 1;
    if (vp.maxz < vp.minz) vp.maxz = vp.minz;
    memcpy(&pk[n], &vp, 24);
    n += 6;
    int32_t sx = 0, sy = 0, sw = (int32_t)w, sh = (int32_t)h;
    const uint32_t* rd = c->raster ? (const uint32_t*)c->raster->desc : NULL;
    if (rd && rd[7] && c->nscissor) {
        sx = c->scissor[0][0]; sy = c->scissor[0][1]; sw = c->scissor[0][2] - sx; sh = c->scissor[0][3] - sy;
        if (sx < 0) { sw += sx; sx = 0; }
        if (sy < 0) { sh += sy; sy = 0; }
        if (sx > (int32_t)w) sx = (int32_t)w;
        if (sy > (int32_t)h) sy = (int32_t)h;
        if (sx + sw > (int32_t)w) sw = (int32_t)w - sx;
        if (sy + sh > (int32_t)h) sh = (int32_t)h - sy;
        if (sw < 0) sw = 0;
        if (sh < 0) sh = 0;
    }
    pk[n++] = sx; pk[n++] = sy; pk[n++] = sw; pk[n++] = sh;
    pk[n++] = (int32_t)c->stencil_ref;
    memcpy(&pk[n], c->blend_factor, 16);
    return n + 4;
}

/* ---- geometry shaders ----
   D3D's geometry shaders here only route triangles to render target layers (volume slices, cube faces).
   A compute shader fetches the vertices itself, runs the vertex and geometry shaders per input primitive and
   sorts the emitted triangles into one bucket per layer; each layer is then drawn indirectly with a
   passthrough vertex shader and the original pixel shader. */

static Variant* lib_variant(GpuShader* s, const uint32_t depth[8], const char* prefix, int group)
{
    WShader* w = s ? s->backend : NULL;
    if (!w || !w->refl_ok) return NULL;
    char uk[64];
    snprintf(uk, sizeof uk, "lib:%s%d", prefix, group);
    for (Variant* v = w->vars; v; v = v->next)
        if (!memcmp(v->depth, depth, 32) && !strcmp(v->uav_key, uk)) return v->failed ? NULL : v;
    Variant* v = calloc(1, sizeof *v);
    memcpy(v->depth, depth, 32);
    snprintf(v->uav_key, sizeof v->uav_key, "%s", uk);
    v->next = w->vars;
    w->vars = v;
    DxWgslOpts o = { 0 };
    memcpy(o.depth_srv, depth, 16);
    memcpy(o.float_srv, depth + 4, 16);
    o.lib = 1;
    o.prefix = prefix;
    o.group = group;
    if (!dxbc_to_wgsl_ex(s->code, s->size, &o, &v->w) || !v->w.wgsl || v->w.error[0]) {
        static int skipped;
        if (skipped++ < 40) host_log("[webgpu] geometry stage skipped: %s", v->w.error);
        v->failed = 1;
        return NULL;
    }
    return v;
}

typedef struct GsProg {
    struct GsProg* next;
    Variant *vs, *gs;
    GpuLayout* layout;
    uint32_t stride[16];
    uint8_t topology, indexed, ib32;
    int ok, nv, nout, pos_reg, layer_reg, layer_comp;
    uint32_t vb_used;
    Program* prog;
    int pipe;
    Variant pt;             /* passthrough vertex shader for the layered draws */
    DxSig out;
} GsProg;

static GsProg* gs_progs;
static int gs_out_buf, gs_out_size, gs_args_buf, gs_args_size;
static int gs_old[64], ngs_old;

static const char* comp_type_kind(uint32_t fmt)
{
    switch (fmt) {
    case 3: case 7: case 12: case 17: case 25: case 30: case 36: case 42: case 50: case 57: case 62: return "u";
    case 4: case 8: case 14: case 18: case 32: case 38: case 43: case 52: case 59: case 64: return "i";
    }
    return "f";
}

/* WGSL expression decoding a vertex element at byte address A from vertex buffer slot S, as vec4<f32> bits */
static void vdecode(Buf* b, int sl, uint32_t fmt, const char* a)
{
#define W(k) "rd%d(%s + %du)", sl, a, k
    char w0[96], w1[96], w2[96], w3[96];
    snprintf(w0, sizeof w0, W(0)); snprintf(w1, sizeof w1, W(4)); snprintf(w2, sizeof w2, W(8)); snprintf(w3, sizeof w3, W(12));
#undef W
    const char* one = comp_type_kind(fmt)[0] == 'f' ? "1.0" : "bitcast<f32>(1u)";
    switch (fmt) {
    case 2: case 3: case 4: bp(b, "bitcast<vec4<f32>>(vec4<u32>(%s, %s, %s, %s))", w0, w1, w2, w3); break;
    case 6: case 7: case 8: bp(b, "vec4<f32>(bitcast<vec3<f32>>(vec3<u32>(%s, %s, %s)), %s)", w0, w1, w2, one); break;
    case 16: case 17: case 18: bp(b, "vec4<f32>(bitcast<vec2<f32>>(vec2<u32>(%s, %s)), 0.0, %s)", w0, w1, one); break;
    case 41: case 42: case 43: bp(b, "vec4<f32>(bitcast<f32>(%s), 0.0, 0.0, %s)", w0, one); break;
    case 10: bp(b, "vec4<f32>(unpack2x16float(%s), unpack2x16float(%s))", w0, w1); break;
    case 11: bp(b, "vec4<f32>(unpack2x16unorm(%s), unpack2x16unorm(%s))", w0, w1); break;
    case 13: bp(b, "vec4<f32>(unpack2x16snorm(%s), unpack2x16snorm(%s))", w0, w1); break;
    case 12: bp(b, "bitcast<vec4<f32>>(vec4<u32>(%s & 0xFFFFu, %s >> 16u, %s & 0xFFFFu, %s >> 16u))", w0, w0, w1, w1); break;
    case 14: bp(b, "bitcast<vec4<f32>>(vec4<i32>(extractBits(bitcast<i32>(%s), 0u, 16u), extractBits(bitcast<i32>(%s), 16u, 16u), extractBits(bitcast<i32>(%s), 0u, 16u), extractBits(bitcast<i32>(%s), 16u, 16u)))", w0, w0, w1, w1); break;
    case 34: bp(b, "vec4<f32>(unpack2x16float(%s), 0.0, 1.0)", w0); break;
    case 35: bp(b, "vec4<f32>(unpack2x16unorm(%s), 0.0, 1.0)", w0); break;
    case 37: bp(b, "vec4<f32>(unpack2x16snorm(%s), 0.0, 1.0)", w0); break;
    case 36: bp(b, "vec4<f32>(bitcast<vec2<f32>>(vec2<u32>(%s & 0xFFFFu, %s >> 16u)), 0.0, %s)", w0, w0, one); break;
    case 38: bp(b, "vec4<f32>(bitcast<vec2<f32>>(vec2<i32>(extractBits(bitcast<i32>(%s), 0u, 16u), extractBits(bitcast<i32>(%s), 16u, 16u))), 0.0, %s)", w0, w0, one); break;
    case 54: bp(b, "vec4<f32>(unpack2x16float(%s & 0xFFFFu).x, 0.0, 0.0, 1.0)", w0); break;
    case 56: bp(b, "vec4<f32>(f32(%s & 0xFFFFu) / 65535.0, 0.0, 0.0, 1.0)", w0); break;
    case 57: bp(b, "vec4<f32>(bitcast<f32>(%s & 0xFFFFu), 0.0, 0.0, %s)", w0, one); break;
    case 24: bp(b, "vec4<f32>(vec3<f32>(f32(%s & 1023u), f32((%s >> 10u) & 1023u), f32((%s >> 20u) & 1023u)) / 1023.0, f32(%s >> 30u) / 3.0)", w0, w0, w0, w0); break;
    case 28: case 29: bp(b, "unpack4x8unorm(%s)", w0); break;
    case 31: bp(b, "unpack4x8snorm(%s)", w0); break;
    case 30: bp(b, "bitcast<vec4<f32>>(vec4<u32>(%s & 255u, (%s >> 8u) & 255u, (%s >> 16u) & 255u, %s >> 24u))", w0, w0, w0, w0); break;
    case 32: bp(b, "bitcast<vec4<f32>>(vec4<i32>(extractBits(bitcast<i32>(%s), 0u, 8u), extractBits(bitcast<i32>(%s), 8u, 8u), extractBits(bitcast<i32>(%s), 16u, 8u), extractBits(bitcast<i32>(%s), 24u, 8u)))", w0, w0, w0, w0); break;
    case 87: case 88: bp(b, "unpack4x8unorm(%s).zyxw", w0); break;
    case 49: bp(b, "vec4<f32>(unpack4x8unorm(%s).xy, 0.0, 1.0)", w0); break;
    case 50: bp(b, "vec4<f32>(bitcast<vec2<f32>>(vec2<u32>(%s & 255u, (%s >> 8u) & 255u)), 0.0, %s)", w0, w0, one); break;
    case 61: bp(b, "vec4<f32>(unpack4x8unorm(%s).x, 0.0, 0.0, 1.0)", w0); break;
    case 62: bp(b, "vec4<f32>(bitcast<f32>(%s & 255u), 0.0, 0.0, %s)", w0, one); break;
    default: bp(b, "vec4<f32>(0.0, 0.0, 0.0, 1.0)"); break;
    }
}

static int sig_match(const DxSig* s, const DxSigElem* e)
{
    for (int i = 0; i < s->n; i++) if (!strcasecmp(s->e[i].name, e->name) && s->e[i].index == e->index) return i;
    return -1;
}

static GsProg* gs_prog(GpuCtx* c, Variant* vs, Variant* gs, int indexed)
{
    uint32_t stride[16];
    for (int i = 0; i < 16; i++) stride[i] = c->vb[i] ? c->vb_stride[i] : 0;
    int ib32 = indexed && c->ib_format == 42;
    for (GsProg* g = gs_progs; g; g = g->next)
        if (g->vs == vs && g->gs == gs && g->layout == c->layout && g->topology == c->topology && g->indexed == indexed && g->ib32 == ib32 && !memcmp(g->stride, stride, sizeof stride))
            return g->ok ? g : NULL;
    GsProg* g = calloc(1, sizeof *g);
    g->vs = vs; g->gs = gs; g->layout = c->layout; g->topology = (uint8_t)c->topology; g->indexed = (uint8_t)indexed; g->ib32 = (uint8_t)ib32;
    memcpy(g->stride, stride, sizeof stride);
    g->next = gs_progs;
    gs_progs = g;
    DxWgsl* gw = &gs->w;
    int nv = gw->gs_in ? gw->gs_in : 3;
    int want = c->topology == 1 ? 1 : c->topology == 2 || c->topology == 3 ? 2 : 3;
    if (gw->gs_topo != 5 || nv != want) {
        host_log("[webgpu] geometry shader with %d-vertex input on topology %u, output topology %d: not emulated", nv, c->topology, gw->gs_topo);
        return NULL;
    }
    g->nv = nv;
    g->nout = gw->nout ? gw->nout : 1;
    g->out = gw->out;
    g->pos_reg = -1; g->layer_reg = -1;
    for (int i = 0; i < gw->out.n; i++) {
        const DxSigElem* e = &gw->out.e[i];
        if (e->sysval == 1) g->pos_reg = (int)e->reg;
        if (e->sysval == 4) { g->layer_reg = (int)e->reg; int cc = 0; while (cc < 3 && !(e->mask >> cc & 1)) cc++; g->layer_comp = cc; }
    }
    if (g->pos_reg < 0) return NULL;

    Buf b = { 0 }, x = { 0 };
    bp(&b, "%s%s%s", dxbc_wgsl_helpers(), vs->w.wgsl, gw->wgsl);
    bp(&b, "struct GsP { vbo: array<vec4<u32>, 4>, vbs: array<vec4<u32>, 4>, m: vec4<u32>, n: vec4<u32>, q: vec4<u32> }\n"
           "@group(3) @binding(217) var<uniform> gp: GsP;\n"
           "@group(3) @binding(218) var<storage, read_write> gout: array<vec4<f32>>;\n"
           "@group(3) @binding(219) var<storage, read_write> gargs: array<atomic<u32>>;\n");
    bp(&x, "{\"binding\":217,\"visibility\":4,\"buffer\":{\"type\":\"uniform\"}},{\"binding\":218,\"visibility\":4,\"buffer\":{\"type\":\"storage\"}},"
           "{\"binding\":219,\"visibility\":4,\"buffer\":{\"type\":\"storage\"}}");
    /* vertex fetch */
    Buf fetch = { 0 };
    const DxSig* in = &vs->w.in;
    GpuLayout* l = c->layout;
    for (int e = 0; e < in->n; e++) {
        const DxSigElem* se = &in->e[e];
        if (se->sysval == 6) { bp(&fetch, "    a_v[%u].x = bitcast<f32>(vid);\n", se->reg); continue; }
        if (se->sysval == 8) { bp(&fetch, "    a_v[%u].x = bitcast<f32>(iid);\n", se->reg); continue; }
        if (se->sysval || !l) continue;
        uint32_t auto_off[16] = { 0 };
        for (uint32_t i = 0; i < l->count; i++) {
            uint32_t sl = l->el[i].slot;
            if (sl >= 16) continue;
            uint32_t off = l->el[i].offset == 0xFFFFFFFF ? auto_off[sl] : l->el[i].offset;
            auto_off[sl] = off + src_bpb(l->el[i].format);
            if (l->el[i].index != se->index || strcasecmp(l->el[i].semantic, se->name)) continue;
            g->vb_used |= 1u << sl;
            char a[160];
            if (!l->el[i].per_instance) snprintf(a, sizeof a, "gp.vbo[%u][%u] + vid * gp.vbs[%u][%u] + %uu", sl / 4, sl % 4, sl / 4, sl % 4, off);
            else if (l->el[i].step) snprintf(a, sizeof a, "gp.vbo[%u][%u] + (gp.n.x + iid / %uu) * gp.vbs[%u][%u] + %uu", sl / 4, sl % 4, l->el[i].step, sl / 4, sl % 4, off);
            else snprintf(a, sizeof a, "gp.vbo[%u][%u] + gp.n.x * gp.vbs[%u][%u] + %uu", sl / 4, sl % 4, sl / 4, sl % 4, off);
            bp(&fetch, "    a_v[%u] = ", se->reg);
            vdecode(&fetch, (int)sl, l->el[i].format, a);
            bp(&fetch, ";\n");
            break;
        }
    }
    for (int sl = 0; sl < 16; sl++) {
        if (!(g->vb_used >> sl & 1)) continue;
        bp(&b, "@group(3) @binding(%d) var<storage, read> vb%d: array<u32>;\n"
               "fn rd%d(a: u32) -> u32 { let n = arrayLength(&vb%d) - 1u; let w = a >> 2u; let s = (a & 3u) * 8u; let lo = vb%d[min(w, n)];"
               " if (s == 0u) { return lo; } return (lo >> s) | (vb%d[min(w + 1u, n)] << (32u - s)); }\n", 200 + sl, sl, sl, sl, sl, sl);
        bp(&x, ",{\"binding\":%d,\"visibility\":4,\"buffer\":{\"type\":\"read-only-storage\"}}", 200 + sl);
    }
    if (indexed) {
        bp(&b, "@group(3) @binding(216) var<storage, read> ib: array<u32>;\n");
        bp(&x, ",{\"binding\":216,\"visibility\":4,\"buffer\":{\"type\":\"read-only-storage\"}}");
        if (ib32) bp(&b, "fn index_at(e: u32) -> u32 { return ib[min((gp.q.x >> 2u) + e, arrayLength(&ib) - 1u)]; }\n");
        else bp(&b, "fn index_at(e: u32) -> u32 { let a = gp.q.x + e * 2u; return (ib[min(a >> 2u, arrayLength(&ib) - 1u)] >> ((a & 2u) * 8u)) & 0xFFFFu; }\n");
    }
    /* VS outputs into GS inputs, matched by semantic */
    Buf link = { 0 };
    const DxSig* vo = &vs->w.out;
    for (int e = 0; e < gw->in.n; e++) {
        const DxSigElem* ge = &gw->in.e[e];
        int k = sig_match(vo, ge);
        if (k < 0) continue;
        const DxSigElem* ve = &vo->e[k];
        for (int cc = 0, j = 0; cc < 4; cc++) {
            if (!(ge->mask >> cc & 1)) continue;
            int sc = cc;
            for (int q = 0, seen = 0; q < 4; q++) if (ve->mask >> q & 1) { if (seen == j) { sc = q; break; } seen++; }
            j++;
            bp(&link, "    b_v[k][%u].%c = a_o%u.%c;\n", ge->reg, "xyzw"[cc], ve->reg, "xyzw"[sc]);
        }
    }
    bp(&b, "fn vs_run(vid: u32, iid: u32, k: u32) {\n%s    a_uew_body();\n%s}\n", fetch.p ? fetch.p : "", link.p ? link.p : "");
    free(fetch.p); free(link.p);
    /* strip assembly into per-layer triangle lists */
    int N = g->nout;
    bp(&b, "var<private> gs_k: u32;\nvar<private> gs_p: array<array<vec4<f32>, %d>, 2>;\n", N);
    bp(&b, "fn gs_put(a: array<vec4<f32>, %d>, b: array<vec4<f32>, %d>, c: array<vec4<f32>, %d>) {\n", N, N, N);
    if (g->layer_reg >= 0) bp(&b, "    let l = bitcast<u32>(a[%d].%c);\n", g->layer_reg, "xyzw"[g->layer_comp]);
    else bp(&b, "    let l = 0u;\n");
    bp(&b, "    if (l >= gp.n.z) { return; }\n    let i = atomicAdd(&gargs[l * 4u], 3u);\n    if (i + 3u > gp.n.y) { return; }\n"
           "    let o = (l * gp.n.y + i) * %du;\n", N);
    for (int r = 0; r < N; r++) bp(&b, "    gout[o + %du] = a[%d]; gout[o + %du] = b[%d]; gout[o + %du] = c[%d];\n", r, r, N + r, r, 2 * N + r, r);
    bp(&b, "}\nfn uew_emit() {\n    var cur: array<vec4<f32>, %d>;\n", N);
    for (int r = 0; r < N; r++) bp(&b, "    cur[%d] = b_o%d;\n", r, r);
    bp(&b, "    if (gs_k >= 2u) { if ((gs_k & 1u) == 0u) { gs_put(gs_p[0], gs_p[1], cur); } else { gs_put(gs_p[1], gs_p[0], cur); } }\n"
           "    gs_p[0] = gs_p[1];\n    gs_p[1] = cur;\n    gs_k++;\n}\nfn uew_cut() { gs_k = 0u; }\n");
    bp(&b, "@compute @workgroup_size(64) fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
           "    let t = id.x + id.y * gp.n.w;\n    if (t >= gp.m.z * gp.m.w) { return; }\n"
           "    let inst = t / gp.m.z;\n    let prim = t - inst * gp.m.z;\n");
    for (int k = 0; k < nv; k++) {
        if (c->topology == 5) bp(&b, "    let e%d = prim + select(%du, %du, (prim & 1u) == 1u);\n", k, k, k == 0 ? 1 : k == 1 ? 0 : k);
        else if (c->topology == 3) bp(&b, "    let e%d = prim + %du;\n", k, k);
        else bp(&b, "    let e%d = prim * %du + %du;\n", k, nv, k);
        if (indexed) bp(&b, "    vs_run(index_at(gp.m.x + e%d) + gp.m.y, inst, %du);\n", k, k);
        else bp(&b, "    vs_run(gp.m.x + e%d, inst, %du);\n", k, k);
    }
    bp(&b, "    b_vprim = vec4<f32>(bitcast<f32>(prim));\n    gs_k = 0u;\n    b_uew_body();\n}\n");
    Program* p = program_ex(vs, gs, NULL, NULL, 1, x.p);
    int mod = uewg_shader_create(b.p);
    g->pipe = uewg_compute_pipeline_create(p->layout, mod);
    g->prog = p;
    free(b.p); free(x.p);
    if (!g->pipe) return NULL;
    /* passthrough vertex shader: one record of nout registers per vertex */
    Buf v = { 0 };
    int ll = dxbc_layer_loc(&g->out);
    bp(&v, "@group(1) @binding(16) var<storage, read> gin: array<vec4<f32>>;\nstruct VOut {\n    @builtin(position) @invariant pos: vec4<f32>,\n");
    int done[32] = { 0 };
    Buf epi = { 0 };
    for (int i = 0; i < g->out.n; i++) {
        const DxSigElem* e = &g->out.e[i];
        if (e->sysval || e->reg >= 32 || done[e->reg]) continue;
        done[e->reg] = 1;
        int flat = 0;
        for (int j = 0; j < g->out.n; j++) if (g->out.e[j].reg == e->reg && !g->out.e[j].sysval && g->out.e[j].comp_type != 3) flat = 1;
        bp(&v, "    @location(%u)%s l%u: vec4<f32>,\n", e->reg, flat ? " @interpolate(flat)" : "", e->reg);
        bp(&epi, "    o.l%u = gin[b + %uu];\n", e->reg, e->reg);
    }
    if (ll >= 0) {
        bp(&v, "    @location(%d) @interpolate(flat) layer: u32,\n", ll);
        bp(&epi, "    o.layer = bitcast<u32>(gin[b + %du].%c);\n", g->layer_reg, "xyzw"[g->layer_comp]);
    }
    bp(&v, "}\n@vertex fn main(@builtin(vertex_index) vid: u32) -> VOut {\n    let b = vid * %du;\n    var o: VOut;\n    o.pos = gin[b + %du];\n%s    return o;\n}\n",
       N, g->pos_reg, epi.p ? epi.p : "");
    g->pt.module = uewg_shader_create(v.p);
    g->pt.w.srv[0].used = 1;
    g->pt.w.srv[0].buffer = 1;
    g->pt.w.srv[0].raw = 1;
    g->pt.w.out = g->out;
    free(v.p); free(epi.p);
    g->ok = 1;
    return g;
}

/* single-layer attachment view i of a render target or depth view */
static int layer_view(GpuView* v, uint32_t i, int* slice)
{
    WView* w = V(v);
    *slice = -1;
    if (!w || !w->rt_view) return 0;
    if (v->res->type == RES_TEX3D) { *slice = (w->slice < 0 ? 0 : w->slice) + (int)i; return w->rt_view; }
    if (!w->layer_views) { w->nlayer_views = (int)w->layers; w->layer_views = calloc((size_t)w->layers, sizeof(int)); }
    if (i >= (uint32_t)w->nlayer_views) return 0;
    if (!w->layer_views[i]) {
        uint32_t save = w->layer, saves = w->layers;
        int da = w->depth_aspect, st = w->stencil_aspect;
        w->layer = save + i; w->layers = 1; w->depth_aspect = 0; w->stencil_aspect = 0;
        w->layer_views[i] = make_view(v, w, "2d", 1);
        w->layer = save; w->layers = saves; w->depth_aspect = da; w->stencil_aspect = st;
    }
    return w->layer_views[i];
}

static uint32_t view_layers(GpuView* v)
{
    WView* w = V(v);
    if (!w) return 0;
    if (v->res->type == RES_TEX3D) {
        uint32_t d = v->res->depth >> w->mip;
        if (!d) d = 1;
        uint32_t first = w->slice < 0 ? 0 : (uint32_t)w->slice;
        uint32_t n = d > first ? d - first : 1;
        return w->wsize && w->wsize < n ? w->wsize : n;
    }
    return w->layers;
}

static void gs_grow(int* id, int* size, int want, int usage)
{
    if (*id && *size >= want) return;
    if (*id) { if (ngs_old < 64) gs_old[ngs_old++] = *id; else uewg_release(*id); }
    *size = want < 65536 ? 65536 : want + want / 2;
    *id = uewg_buffer_create(*size, usage);
}

static void gs_draw(GpuCtx* c, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, int indexed)
{
    uint32_t dm[8];
    depth_mask(c, SH_VS, dm);
    Variant* vs = lib_variant(c->sh[SH_VS], dm, "a_", 0);
    depth_mask(c, SH_GS, dm);
    Variant* gs = lib_variant(c->sh[SH_GS], dm, "b_", 2);
    if (!vs || !gs) DROP(0);
    GsProg* g = gs_prog(c, vs, gs, indexed);
    if (!g) DROP(0);
    Variant* ps = NULL;
    if (c->sh[SH_PS]) {
        depth_mask(c, SH_PS, dm);
        ps = variant(c->sh[SH_PS], dm, &g->out, NULL);
        if (!ps) DROP(3);
    }
    GpuView* first_rt = NULL;
    for (int i = 0; i < 8 && !first_rt; i++) if (c->rtv[i] && V(c->rtv[i]) && V(c->rtv[i])->rt_view) first_rt = c->rtv[i];
    uint32_t nl = first_rt ? view_layers(first_rt) : c->dsv ? view_layers(c->dsv) : 0;
    if (!nl) DROP(0);
    if (nl > 256) nl = 256;
    uint32_t nprim = c->topology == 5 ? (count >= 3 ? count - 2 : 0) : c->topology == 3 ? (count >= 2 ? count - 1 : 0) : count / (uint32_t)g->nv;
    if (!nprim) return;
    /* each primitive lands at most once per layer */
    uint64_t cap = (uint64_t)nprim * instances * 3, rec = (uint64_t)g->nout * 16;
    if (cap * nl * rec > (64u << 20)) cap = (64u << 20) / (nl * rec);
    cap -= cap % 3;
    if (!cap) DROP(0);
    gs_grow(&gs_out_buf, &gs_out_size, (int)(cap * nl * rec), 8);
    gs_grow(&gs_args_buf, &gs_args_size, (int)(nl * 16), 8 | 16);
    /* parameters */
    uint32_t prm[44] = { 0 };
    for (int s = 0; s < 16; s++) { prm[s] = c->vb_offset[s]; prm[16 + s] = c->vb_stride[s]; }
    uint64_t total = (uint64_t)nprim * instances, groups = (total + 63) / 64;
    uint32_t wx = groups > 65535 ? 65535 : (uint32_t)groups, wy = (uint32_t)((groups + wx - 1) / wx);
    prm[32] = first; prm[33] = (uint32_t)base_vertex; prm[34] = nprim; prm[35] = instances;
    prm[36] = first_instance; prm[37] = (uint32_t)cap; prm[38] = nl; prm[39] = wx * 64;
    prm[40] = c->ib_offset;
    uint32_t po = ring_put(prm, sizeof prm, sizeof prm);
    Buf x = { 0 };
    bp(&x, "{\"b\":217,\"ring\":1,\"off\":%u,\"size\":%u},{\"b\":218,\"buf\":%d},{\"b\":219,\"buf\":%d}", po, (unsigned)sizeof prm, gs_out_buf, gs_args_buf);
    for (int s = 0; s < 16; s++) {
        if (!(g->vb_used >> s & 1)) continue;
        WTex* t = T(c->vb[s]);
        if (t && t->id) { t->used = g_serial; bp(&x, ",{\"b\":%d,\"buf\":%d}", 200 + s, t->id); }
        else bp(&x, ",{\"b\":%d,\"dummy\":\"buf\"}", 200 + s);
    }
    if (indexed) {
        WTex* t = T(c->ib);
        if (!t || !t->id) { free(x.p); DROP(6); }
        t->used = g_serial;
        bp(&x, ",{\"b\":216,\"buf\":%d}", t->id);
    }
    memcpy(pass_rt, c->rtv, sizeof pass_rt);
    pass_gen++;
    pass_ds = c->dsv;
    int n = 0, nd;
    pk[n++] = g->pipe; pk[n++] = (int32_t)wx; pk[n++] = (int32_t)wy; pk[n++] = (int32_t)nl; pk[n++] = (int32_t)cap; pk[n++] = gs_args_buf;
    Program* cp = g->prog;
    pk[n] = cb_bind(c, cp, vs, SH_VS, 0, &pk[n + 2], &nd); pk[n + 1] = nd; n += 2 + nd;
    pk[n++] = res_group(c, cp, vs, SH_VS, 1); pk[n++] = 0;
    pk[n] = cb_bind(c, cp, gs, SH_GS, 1, &pk[n + 2], &nd); pk[n + 1] = nd; n += 2 + nd;
    pk[n++] = res_group_ex(c, cp, gs, SH_GS, 3, x.p); pk[n++] = 0;
    free(x.p);
    /* the layered draws */
    Program* rp = program_ex(&g->pt, ps, NULL, NULL, 2, NULL);
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = rp;
    key_targets(c, &k);
    k.topology = 4;
    int rpipe = render_pipeline(c, rp, &k);
    if (!rpipe) DROP(4);
    pk[n++] = rpipe;
    pk[n++] = rp->cb_group[0] ? rp->cb_group[0] : empty_bg; pk[n++] = 0;
    static int pt_bg, pt_bg_buf, pt_bg_layout;
    if (pt_bg_buf != gs_out_buf || pt_bg_layout != rp->layout) {
        char j[64];
        snprintf(j, sizeof j, "[{\"b\":16,\"buf\":%d}]", gs_out_buf);
        pt_bg = uewg_bindgroup_create(rp->layout, 1, j);
        pt_bg_buf = gs_out_buf;
        pt_bg_layout = rp->layout;
    }
    pk[n++] = pt_bg; pk[n++] = 0;
    if (ps) {
        pk[n] = cb_bind(c, rp, ps, SH_PS, 1, &pk[n + 2], &nd); pk[n + 1] = nd; n += 2 + nd;
        pk[n++] = res_group(c, rp, ps, SH_PS, 3); pk[n++] = 0;
    } else { pk[n++] = empty_bg; pk[n++] = 0; pk[n++] = empty_bg; pk[n++] = 0; }
    uint32_t w = 0, h = 0;
    GpuView* sz = first_rt ? first_rt : c->dsv;
    if (sz && sz->res) { uint32_t m = V(sz)->mip; w = sz->res->width >> m; h = sz->res->height >> m; }
    if (!w) w = 1;
    if (!h) h = 1;
    int m = put_fixed(c, w, h, n);
    if (m < 0) DROP(5);
    n = m;
    int last = -1;
    for (int i = 0; i < 8; i++) if (c->rtv[i] && V(c->rtv[i]) && V(c->rtv[i])->rt_view) last = i;
    WView* dv = c->dsv ? V(c->dsv) : NULL;
    int has_depth = dv && dv->rt_view;
    WTex* dt = has_depth ? T(c->dsv->res) : NULL;
    pk[n++] = last + 1; pk[n++] = has_depth; pk[n++] = dt && (dt->fmt.flags & FF_STENCIL) ? 1 : 0; pk[n++] = has_depth ? dv->read_only : 0;
    for (uint32_t L = 0; L < nl; L++) {
        if (n + 20 > (int)(sizeof pk / sizeof *pk)) DROP(0);
        for (int i = 0; i <= last; i++) {
            int sl = -1, id = c->rtv[i] ? layer_view(c->rtv[i], L, &sl) : 0;
            pk[n++] = id; pk[n++] = sl;
        }
        if (has_depth) { int sl; pk[n++] = layer_view(c->dsv, L, &sl); }
    }
    uewg_gs(pk);
    PASS_BREAK();
    drawn++;
}

static void draw(GpuCtx* c, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, int indexed)
{
    if (!g_ok || !c->sh[SH_VS] || c->sh[SH_HS] || c->sh[SH_DS] || !count || !instances) DROP(0);
    int indirect = count == ~0u && c->indirect;
    WTex* it = indirect ? T(c->indirect) : NULL;
    if (indirect && (!it || !it->id || c->sh[SH_GS])) DROP(0);
    if (c->sh[SH_GS]) { st_gs++; gs_draw(c, count, first, base_vertex, instances, first_instance, indexed); return; }
    if (c->topology > 5) DROP(1);
    uint32_t dm[8] = { 0 }, vdm[8];
    depth_mask(c, SH_VS, vdm);
    if (c->sh[SH_PS]) depth_mask(c, SH_PS, dm);
    /* everything the variants, program and pipeline depend on; most draws repeat the previous draw's */
    static struct DrawKey {
        GpuShader *vs, *ps;
        uint32_t vdm[8], dm[8];
        GpuLayout* layout;
        GpuState *blend, *depth, *raster;
        GpuView* rtv[8];
        GpuRes* rtres[8];
        uint32_t rtfmt[8], rtview[8];
        GpuView* dsv;
        GpuRes* dsres;
        uint32_t dsfmt, dsro, dsview, stride[16], sample_mask, topology, strip32, vbf, pbf;
    } dk, last_dk;
    static struct { int valid; Variant *vs, *ps; Program* p; int pipe; } dmemo;
    memset(&dk, 0, sizeof dk);
    dk.vs = c->sh[SH_VS]; dk.ps = c->sh[SH_PS];
    memcpy(dk.vdm, vdm, 32); memcpy(dk.dm, dm, 32);
    dk.layout = c->layout; dk.blend = c->blend; dk.depth = c->depth; dk.raster = c->raster;
    for (int i = 0; i < 8; i++) {
        GpuView* r = c->rtv[i];
        if (!r) continue;
        dk.rtv[i] = r; dk.rtres[i] = r->res; dk.rtfmt[i] = r->format;
        WView* wv = V(r);
        dk.rtview[i] = wv ? (uint32_t)wv->rt_view : 0;
    }
    if (c->dsv) { dk.dsv = c->dsv; dk.dsres = c->dsv->res; dk.dsfmt = c->dsv->res ? c->dsv->res->format : 0; dk.dsro = V(c->dsv) ? (uint32_t)V(c->dsv)->read_only : 0; dk.dsview = V(c->dsv) ? (uint32_t)V(c->dsv)->rt_view : 0; }
    for (int i = 0; i < 16; i++) dk.stride[i] = c->vb[i] ? c->vb_stride[i] : 0;
    dk.sample_mask = c->sample_mask; dk.topology = c->topology; dk.strip32 = c->ib_format == 42;
    static uint8_t vbfmt[128], pbfmt[128];
    dk.vbf = buf_fmts(c, SH_VS, vbfmt);
    dk.pbf = c->sh[SH_PS] ? buf_fmts(c, SH_PS, pbfmt) : 0;
    Variant *vs, *ps = NULL;
    Program* p;
    int pipe;
    uint32_t w, h;
    if (!dbg_flag("nodrawmemo", &no_dmemo) && dmemo.valid && !memcmp(&dk, &last_dk, sizeof dk)) {
        vs = dmemo.vs; ps = dmemo.ps; p = dmemo.p; pipe = dmemo.pipe;
        ensure_pass(c, &w, &h);
    } else {
        dmemo.valid = 0;
        cur_bfmt = vbfmt;
        vs = variant(c->sh[SH_VS], vdm, NULL, NULL);
        if (!vs) DROP(2);
        if (c->sh[SH_PS]) {
            cur_bfmt = pbfmt;
            ps = variant_ex(c->sh[SH_PS], dm, &vs->w.out, NULL, NULL, 1);
            if (!ps) DROP(3);
            int any = 0;
            for (int i = 0; i < 32; i++) any |= ps->w.interp[i];
            if (any) cur_bfmt = vbfmt;
            if (any && !(vs = variant_ex(c->sh[SH_VS], vdm, NULL, NULL, ps->w.interp, 0))) DROP(2);
        }
        p = program(vs, ps, NULL, NULL);
        ensure_pass(c, &w, &h);
        PipeKey k;
        memset(&k, 0, sizeof k);
        k.prog = p;
        k.layout = c->layout;
        memcpy(k.stride, dk.stride, sizeof k.stride);
        key_targets(c, &k);
        k.topology = (uint8_t)c->topology;
        k.strip32 = c->ib_format == 42;
        pipe = render_pipeline(c, p, &k);
        if (!pipe) DROP(4);
        wait_pipe(pipe);
        last_dk = dk;
        dmemo.valid = 1; dmemo.vs = vs; dmemo.ps = ps; dmemo.p = p; dmemo.pipe = pipe;
    }
    int n = 0;
    pk[n++] = pipe;
    n = put_groups(c, p, vs, ps, n);
    int nvb = 0, at = n++;
    uint32_t want_vb = 0, inst_step[16] = { 0 };
    if (c->layout) for (uint32_t e = 0; e < c->layout->count; e++) if (c->layout->el[e].slot < 16) {
        want_vb |= 1u << c->layout->el[e].slot;
        if (c->layout->el[e].per_instance) inst_step[c->layout->el[e].slot] = c->layout->el[e].step ? c->layout->el[e].step : 1;
    }
    /* D3D's SV_InstanceID starts at 0 whatever the start instance; WebGPU's instance_index doesn't. So the start
       instance moves the per-instance vertex buffers instead and the draw itself starts at instance 0. */
    uint32_t shift = indirect ? 0 : first_instance;
    for (int i = 0; i < 16 && shift; i++)
        if (inst_step[i] && c->vb[i] && (uint64_t)c->vb_offset[i] + (uint64_t)(shift / inst_step[i]) * c->vb_stride[i] >= c->vb[i]->width) shift = 0;
    /* likewise SV_VertexID of an indexed draw is the index alone, without the base vertex (Unreal's manual vertex fetch
       adds that itself), while WebGPU's vertex_index includes it: move the per-vertex buffers instead */
    WShader* vsw = c->sh[SH_VS] ? (WShader*)c->sh[SH_VS]->backend : NULL;
    if (indirect) { st_ind++; if (vsw && vsw->refl.uses_instance_id) st_iid_ind++; }
    else if (first_instance && shift != first_instance && vsw && vsw->refl.uses_instance_id) st_iid_lost++;
    uint32_t vshift = !indirect && indexed && base_vertex > 0 && vsw && vsw->refl_ok && vsw->refl.uses_vertex_id ? (uint32_t)base_vertex : 0;
    for (int i = 0; i < 16 && vshift; i++)
        if (!inst_step[i] && c->vb[i] && dk.stride[i] && ((uint64_t)c->vb_offset[i] + (uint64_t)vshift * c->vb_stride[i] >= c->vb[i]->width || (vshift * c->vb_stride[i]) & 3)) vshift = 0;
    for (int i = 0; i < 16; i++) {
        WTex* t = T(c->vb[i]);
        if (!t || !t->id || !(want_vb >> i & 1)) {
            /* D3D reads zeros from an unbound slot; WebGPU needs a buffer there */
            if (want_vb >> i & 1) { pk[n++] = i; pk[n++] = -1; pk[n++] = 0; nvb++; }
            continue;
        }
        t->used = g_serial;
        pk[n++] = i; pk[n++] = t->id; pk[n++] = (int32_t)((c->vb_offset[i] + (inst_step[i] ? shift / inst_step[i] * c->vb_stride[i] : vshift * c->vb_stride[i])) & ~3u);
        nvb++;
    }
    pk[at] = nvb;
    int m = put_fixed(c, w, h, n);
    if (m < 0) DROP(5);
    n = m;
    if (indirect) { it->used = g_serial; pk[n++] = indexed; pk[n++] = -1; pk[n++] = it->id; pk[n++] = (int32_t)c->indirect_off; pk[n++] = 0; pk[n++] = 0; }
    else { pk[n++] = indexed; pk[n++] = (int32_t)count; pk[n++] = (int32_t)instances; pk[n++] = (int32_t)first; pk[n++] = base_vertex - (int32_t)vshift; pk[n++] = (int32_t)(first_instance - shift); }
    if (indexed) {
        WTex* t = T(c->ib);
        if (!t || !t->id) DROP(6);
        t->used = g_serial;
        pk[n++] = t->id; pk[n++] = c->ib_format == 42; pk[n++] = (int32_t)c->ib_offset;
    }
    uewg_draw(pk);
    drawn++;
}

static void dispatch(GpuCtx* c, uint32_t x, uint32_t y, uint32_t z)
{
    if (!g_ok || !c->sh[SH_CS] || !x || !y || !z) return;
    uint32_t dm[8];
    depth_mask(c, SH_CS, dm);
    const char* uf[8] = { 0 };
    WShader* ws = c->sh[SH_CS]->backend;
    if (!ws || !ws->refl_ok) return;
    static char buf_tags[128][8];
    for (int i = 0; i < 8; i++) {
        if (!ws->refl.uav[i].used || ws->refl.uav[i].raw || ws->refl.uav[i].structured) continue;
        if (ws->refl.uav[i].buffer || ws->refl.uav[i].dim == 1) {
            /* typed buffer UAVs wider or narrower than one 32-bit channel: stores pack to the view's format */
            GpuView* v = c->uav[i];
            uint32_t f = v && v->res && v->res->type == RES_BUFFER ? v->format : 0;
            if (f && f < 128 && f != 41 && f != 42 && f != 43 && src_bpb(f) >= 4) {
                if (!buf_tags[f][0]) snprintf(buf_tags[f], sizeof buf_tags[f], "buf:%u", f);
                uf[i] = buf_tags[f];
            }
            continue;
        }
        GpuView* v = c->uav[i];
        WTex* t = v ? T(v->res) : NULL;
        if (t && t->fmt.name && v->res->type != RES_BUFFER) uf[i] = t->fmt.name;
    }
    static uint8_t cbfmt[128];
    buf_fmts(c, SH_CS, cbfmt);
    cur_bfmt = cbfmt;
    Variant* cs = variant(c->sh[SH_CS], dm, NULL, uf);
    if (!cs) return;
    Program* p = program(NULL, NULL, cs, uf);
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.prog = p;
    k.topology = 255;
    uint64_t h = fnv(&k, sizeof k, 0xcbf29ce484222325ull);
    Pipe** slot = &pipes[h & 1023];
    int pipe = 0;
    for (Pipe* e = *slot; e; e = e->next) if (e->h == h && !memcmp(&e->k, &k, sizeof k)) { pipe = e->id; break; }
    if (!pipe) {
        pipe = uewg_compute_pipeline_create(p->layout, cs->module);
        Pipe* e = calloc(1, sizeof *e);
        e->k = k; e->h = h; e->id = pipe; e->next = *slot; *slot = e;
    }
    if (!pipe) return;
    wait_pipe(pipe);
    int n = 0;
    pk[n++] = pipe;
    int nd;
    pk[n] = cb_bind(c, p, cs, SH_CS, 0, &pk[n + 2], &nd);
    pk[n + 1] = nd;
    n += 2 + nd;
    memset(pass_rt, 0, sizeof pass_rt);
    pass_gen++;
    pass_ds = NULL;
    pk[n++] = res_group(c, p, cs, SH_CS, 1);
    if (x == ~0u && c->indirect && T(c->indirect) && T(c->indirect)->id) {
        T(c->indirect)->used = g_serial;
        pk[n++] = -1; pk[n++] = T(c->indirect)->id; pk[n++] = (int32_t)c->indirect_off;
    } else { pk[n++] = (int32_t)x; pk[n++] = (int32_t)y; pk[n++] = (int32_t)z; }
    pk[n++] = (int32_t)ring_used;
    uewg_dispatch(pk);
    PASS_BREAK();
}

/* ---- copies, present ---- */

static void copy(GpuRes* dst, uint32_t dsub, uint32_t dx, uint32_t dy, uint32_t dz, GpuRes* src, uint32_t ssub, const int32_t* box)
{
    WTex *d = T(dst), *s = T(src);
    if (!g_ok || !d || !s) return;
    if (dst->type == RES_BUFFER && src->type == RES_BUFFER) {
        uint32_t so = box ? (uint32_t)box[0] : 0, n = box ? (uint32_t)(box[3] - box[0]) : src->width;
        if (dx + n > dst->width) n = dst->width - dx;
        if (dst->backing && src->backing) memmove(G2H(dst->backing + dx), G2H(src->backing + so), n);
        d->version++;
        free(d->shadow);
        d->shadow = NULL;
        if (d->id && s->id && !(so & 3) && !(dx & 3)) { uewg_copy_buffer(s->id, (int)so, d->id, (int)dx, (int)n); PASS_BREAK(); d->used = s->used = g_serial; }
        return;
    }
    if (!d->id || !s->id || dst->type == RES_BUFFER || src->type == RES_BUFFER) return;
    uint32_t smip = ssub % s->mips, sl = ssub / s->mips, dmip = dsub % d->mips, dl = dsub / d->mips;
    uint32_t sx = 0, sy = 0, sz = 0, w = s->w >> smip, h = s->h >> smip, dd = src->type == RES_TEX3D ? src->depth >> smip : 1;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!dd) dd = 1;
    if (box) { sx = (uint32_t)box[0]; sy = (uint32_t)box[1]; sz = (uint32_t)box[2]; w = (uint32_t)(box[3] - box[0]); h = (uint32_t)(box[4] - box[1]); dd = (uint32_t)(box[5] - box[2]); }
    uint32_t dw = d->w >> dmip, dh = d->h >> dmip;
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    if (dx + w > dw) w = dw - dx;
    if (dy + h > dh) h = dh - dy;
    if (!w || !h || !dd) return;
    uewg_copy_texture(s->id, (int)smip, src->type == RES_TEX3D ? 0 : (int)sl, (int)sx, (int)sy, (int)sz, d->id, (int)dmip, dst->type == RES_TEX3D ? 0 : (int)dl,
                      (int)dx, (int)dy, (int)dz, (int)w, (int)h, (int)dd);
    d->used = s->used = g_serial;
    PASS_BREAK();
}

/* GPU -> CPU for Map(READ) of staging resources: submit what's pending, then wait for the copy (the import suspends) */
static void readback(GpuRes* r, uint32_t sub)
{
    WTex* t = T(r);
    if (!g_ok || !t || !t->id || !r->backing || r->usage != 3) return;
    ring_submit();
    int small = r->bytes <= (64u << 10);
    if (r->type == RES_BUFFER) {
        if (small && uewg_readback_latest(0, t->id, 0, 0, (int)r->width, 1, 1, G2H(r->backing), (int)r->width, (int)r->width, (int)r->width, 1)) return;
        uewg_readback(0, t->id, 0, 0, (int)r->width, 1, 1, G2H(r->backing), (int)r->width, (int)r->width, (int)r->width, 1);
        return;
    }
    if (t->fmt.conv) return;
    uint32_t mip = sub % t->mips, layer = sub / t->mips, blk = t->fmt.block;
    uint32_t w = r->width >> mip, h = r->type == RES_TEX1D ? 1 : r->height >> mip, d = r->type == RES_TEX3D ? r->depth >> mip : 1;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t bw = (w + blk - 1) / blk, bh = (h + blk - 1) / blk;
    if (small && uewg_readback_latest(1, t->id, (int)mip, r->type == RES_TEX3D ? 0 : (int)layer, (int)(bw * blk), (int)(bh * blk), (int)d,
                                      G2H(r->backing + r->sub_offset[sub]), (int)r->sub_pitch[sub], (int)r->sub_slice[sub], (int)(bw * t->fmt.bpb), (int)bh))
        return;
    uewg_readback(1, t->id, (int)mip, r->type == RES_TEX3D ? 0 : (int)layer, (int)(bw * blk), (int)(bh * blk), (int)d, G2H(r->backing + r->sub_offset[sub]),
                  (int)r->sub_pitch[sub], (int)r->sub_slice[sub], (int)(bw * t->fmt.bpb), (int)bh);
}

static void present(GpuRes* bb, uint32_t w, uint32_t h)
{
    (void)w; (void)h;
    if (!g_ok) return;
    WTex* t = T(bb);
    int view = 0;
    if (t && t->id) {
        if (!t->view_all) {
            breset(&jb);
            bp(&jb, "{\"tex\":%d,\"dim\":\"2d\",\"mip\":0,\"mips\":1,\"layer\":0,\"layers\":1,\"aspect\":\"all\"}", t->id);
            t->view_all = uewg_view_create(jb.p);
        }
        view = t->view_all;
    }
    static uint32_t frames;
    if (++frames % 600 == 0) {
        char m[256];
        int o = snprintf(m, sizeof m, "%u drawn", drawn), dropped = 0;
        for (int i = 0; i < 7; i++) if (drops[i]) { o += snprintf(m + o, sizeof m - o, ", %u dropped (%s)", drops[i], drop_why[i]); dropped = 1; }
        if (dropped) host_log("[webgpu] last 600 frames: %s", m);
        host_log("[webgpu] per frame: %u draws, %u GS draws, CB ring %.0f KB (%llu hits, %llu unchanged, %llu shared, %llu copies), %llu per-draw CB groups, res groups %llu memo / %llu cached / %llu hashed / %llu new, %d live",
                 drawn / 600, (unsigned)(st_gs / 600), st_ring_bytes / 600 / 1024.0, (unsigned long long)(st_cb_hit / 600), (unsigned long long)(st_cb_same / 600), (unsigned long long)(st_cb_dedup / 600), (unsigned long long)(st_cb_miss / 600),
                 (unsigned long long)(st_static / 600), (unsigned long long)(st_bg_memo / 600), (unsigned long long)(st_bg_rc / 600), (unsigned long long)(st_bg_hash / 600), (unsigned long long)(st_bg_new / 600), bg_count);
        if (st_ind || st_iid_lost) host_log("[webgpu] per frame: %.1f indirect draws (%.1f read SV_InstanceID), %.1f direct draws lost their start instance",
                                            st_ind / 600.0, st_iid_ind / 600.0, st_iid_lost / 600.0);
        st_ind = st_iid_ind = st_iid_lost = 0;
        {
            char m[512]; int o = 0;
            uint32_t total = 0;
            for (int i = 0; i < 64; i++) total += brk_n[i];
            for (int k = 0; k < 6 && total > 600 * 20; k++) {
                int best = -1;
                for (int i = 0; i < 64; i++) if (brk_n[i] && (best < 0 || brk_n[i] > brk_n[best])) best = i;
                if (best < 0) break;
                o += snprintf(m + o, sizeof m - o, " line %u: %.1f", brk_line[best], brk_n[best] / 600.0);
                brk_n[best] = 0;
            }
            if (o) host_log("[webgpu] pass breaks per frame by source line:%s", m);
            memset(brk_n, 0, sizeof brk_n);
        }
        st_bg_rc = st_ring_bytes = st_cb_hit = st_cb_same = st_cb_dedup = st_cb_miss = st_gs = st_static = st_bg_memo = st_bg_hash = st_bg_new = 0;
        memset(drops, 0, sizeof drops);
        drawn = 0;
    }
    uewg_present(view, (int)ring_used);
    if (dbg_flag("ringreset", &ring_reset)) { ring_used = 0; ring_gen++; uewg_ring_reset(); }
    for (int i = 0; i < ntemp; i++) uewg_release(temp_bg[i]);
    ntemp = 0;
    if (bg_full) { bg_clear(); bg_full = 0; ndead = 0; dead_over = 0; }
    bpool_age(g_serial);
    if (!bg_full) bg_sweep();
    for (int i = 0; i < ngs_old; i++) uewg_release(gs_old[i]);
    for (int i = 0; i < ntemp_views; i++) uewg_release(temp_views[i]);
    ntemp_views = 0;
    ngs_old = 0;
    PASS_BREAK();
    g_serial++;
    memset(pass_rt, 0, sizeof pass_rt);
    pass_gen++;
    pass_ds = NULL;
}

static void clear_uav(GpuView* v, const uint32_t* val, int is_float)
{
    WView* w = V(v);
    WTex* t = v ? T(v->res) : NULL;
    if (!g_ok || !w || !t || !t->id) return;
    t->version++;
    t->gpu_written = 1;
    if (v->res->type == RES_BUFFER) {
        uint32_t stride = v->res->stride ? v->res->stride : 4;
        uint32_t off = w->first_elem * stride, size = w->num_elem * stride;
        if (!size || off + size > v->res->width) { off = 0; size = v->res->width; }
        off &= ~3u;
        size = (size + 3) & ~3u;
        uint32_t word = val[0];
        if (is_float) { float f; memcpy(&f, &val[0], 4); word = (uint32_t)f; }
        if (!word) { uewg_clear_buffer(t->id, (int)off, (int)size); PASS_BREAK(); return; }
        /* non-zero fill: a pattern in the ring, copied in order with the other commands */
        for (uint32_t done = 0; done < size;) {
            uint32_t n = size - done > (1u << 20) ? 1u << 20 : size - done;
            uint32_t* tmp = malloc(n);
            for (uint32_t i = 0; i < n / 4; i++) tmp[i] = word;
            uint32_t ro = ring_put(tmp, n, n);
            free(tmp);
            uewg_buffer_copy_ring((int)ro, t->id, (int)(off + done), (int)n, (int)ring_used);
            done += n;
        }
        PASS_BREAK();
        t->used = g_serial;
        return;
    }
    /* textures: a clearing render pass over the view's mip, per layer or depth slice */
    float rgba[4];
    for (int i = 0; i < 4; i++) {
        if (is_float) memcpy(&rgba[i], &val[i], 4);
        else rgba[i] = (t->fmt.flags & FF_SINT) ? (float)(int32_t)val[i] : (float)val[i];
    }
    uint32_t n = v->res->type == RES_TEX3D ? (v->res->depth >> w->mip ? v->res->depth >> w->mip : 1) : w->layers;
    for (uint32_t L = 0; L < n; L++) {
        uint32_t sl = w->layer, sls = w->layers;
        if (v->res->type != RES_TEX3D) { w->layer = sl + L; w->layers = 1; }
        int id = make_view(v, w, v->res->type == RES_TEX3D ? "3d" : "2d", 1);
        w->layer = sl; w->layers = sls;
        if (!id) return;
        breset(&jb);
        bp(&jb, "{\"color\":[{\"view\":%d,\"slice\":%d,\"clear\":[%.9g,%.9g,%.9g,%.9g]}]}", id, v->res->type == RES_TEX3D ? (int)L : -1, rgba[0], rgba[1], rgba[2], rgba[3]);
        uewg_clear(jb.p, (int)ring_used);
        if (ntemp_views < 4096) temp_views[ntemp_views++] = id;
    }
    t->used = g_serial;
    PASS_BREAK();
}

static void gen_mips(GpuView* v)
{
    WTex* t = v && v->res ? T(v->res) : NULL;
    if (!g_ok || !t || !t->id || t->mips < 2 || v->res->type == RES_TEX3D) return;
    uewg_gen_mips(t->id);
    t->used = g_serial;
    PASS_BREAK();
}

static void layout_create(GpuLayout* l) { (void)l; }

static void view_destroy(GpuView* v)
{
    WView* w = V(v);
    if (!w) return;
    if (w->rt_view) uewg_retire(w->rt_view);
    for (int i = 0; i < w->nv; i++) if (w->v[i].id) { uewg_retire(w->v[i].id); bg_dead(w->v[i].id); }
    if (w->typed_buf) { uewg_retire(w->typed_buf); bg_dead(w->typed_buf); }
    free(w);
    v->backend = NULL;
}

/* freed views/resources may come back at the same address: drop pointer-keyed state */
static void forget(void)
{
    PASS_BREAK();
    memset(pass_rt, 0, sizeof pass_rt);
    pass_ds = NULL;
    pass_gen++;
    bg_epoch++;
}

/* D3D timestamps from when the GPU finished the work submitted before them: the gap between two is the GPU time of the
   work in between (engines time their GPU benchmarks this way and scale the workload by the result) */
static uint32_t timestamp(void)
{
    if (!g_ok) return 0;
    ring_submit();
    return (uint32_t)uewg_timestamp();
}
static int timestamp_get(uint32_t ticket, uint64_t* ns)
{
    double ms;
    if (!uewg_timestamp_get((int)ticket, &ms)) return 0;
    *ns = (uint64_t)(ms * 1e6);
    return 1;
}

/* WebGPU write-only storage formats: the core set, bgra8unorm with its feature, the 8/16-bit and packed ones with
   texture-formats-tier1 */
static int uav_store(uint32_t f)
{
    const char* n = texfmt(f, 0).name;
    if (!n) return 0;
    static const char* core[] = { "rgba8unorm", "rgba8snorm", "rgba8uint", "rgba8sint", "rgba16uint", "rgba16sint", "rgba16float",
        "r32uint", "r32sint", "r32float", "rg32uint", "rg32sint", "rg32float", "rgba32uint", "rgba32sint", "rgba32float" };
    static const char* tier1[] = { "r8unorm", "r8snorm", "r8uint", "r8sint", "rg8unorm", "rg8snorm", "rg8uint", "rg8sint", "r16uint",
        "r16sint", "r16float", "rg16uint", "rg16sint", "rg16float", "rgb10a2uint", "rgb10a2unorm", "rg11b10ufloat", "r16unorm",
        "r16snorm", "rg16unorm", "rg16snorm", "rgba16unorm", "rgba16snorm" };
    for (size_t i = 0; i < sizeof core / sizeof *core; i++) if (!strcmp(n, core[i])) return 1;
    if (!strcmp(n, "bgra8unorm")) return uewg_has_feature("bgra8unorm-storage");
    static int t1 = -1;
    if (t1 < 0) t1 = uewg_has_feature("texture-formats-tier1");
    if (t1) for (size_t i = 0; i < sizeof tier1 / sizeof *tier1; i++) if (!strcmp(n, tier1[i])) return 1;
    return 0;
}

static GpuBackend wgpu = { "WebGPU", res_create, res_upload, res_destroy, view_create, shader_create, state_create, layout_create,
                           clear_rtv, clear_dsv, draw, dispatch, copy, readback, present, NULL, clear_uav, gen_mips, view_destroy, forget,
                           timestamp, timestamp_get, uav_store };

GpuBackend* gpu_wgpu_backend(void)
{
    static int tried;
    if (!tried) {
        tried = 1;
        int w, h;
        host_canvas_size(&w, &h);
        ring = malloc(ring_size);
        uewg_ring_used_at(&ring_used);
        int r = ring ? uewg_init(w, h, ring, (int)ring_size) : 0;
        g_ok = r & 1;
        g_d32s8 = (r & 2) != 0;
        if (g_ok) empty_bg = uewg_bindgroup_create(-1, 0, "[]");
    }
    return g_ok ? &wgpu : NULL;
}
#endif
