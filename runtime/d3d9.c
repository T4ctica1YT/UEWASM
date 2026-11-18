/* Direct3D 9 on the same GPU backend as Direct3D 11 (gpu.h), the way DXVK maps D3D9 onto a modern API:
   - D3D9 textures, surfaces and buffers become GpuRes objects in the matching DXGI format (a few D3D9-only formats,
     such as L8 and A8L8, are converted to RGBA8 when uploaded);
   - the render, sampler and texture-stage state becomes D3D11-style state objects, made when a draw needs them and
     cached by content;
   - shaders stay D3D9 bytecode: the backend's shader translator hands them to d3d9_wgsl.c, and their constants live
     in one uniform buffer per stage (layout in d3d9_wgsl.h);
   - the fixed-function pieces D3D9 shaders still rely on are emulated: alpha test in the pixel shader, the half-pixel
     offset in the vertex shader, partial clears and scaled StretchRect with built-in shaders.
   Fixed-function vertex/pixel processing itself is not implemented (shader-based engines such as Unreal Engine 3 don't
   use it); such draws are skipped and logged once.

   COM methods are written for both 32-bit (stdcall, arguments widened by rt_hle32) and 64-bit guests: pointers in guest
   structures go through LDP/STP and their offsets depend on GP. */
#include "com.h"
#include "d3d9_wgsl.h"
#include "gpu.h"
#include "sched.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THIS A1
#define PAY(o) ((o) + COM_HDR)
#define D3D_OK 0
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define E_NOINTERFACE 0x80004002u
#define S_FALSE 1

static float f32of(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

/* ---------------------------------------------------------------- formats */

enum { CVN = 0, CV_L8, CV_A8L8, CV_A8, CV_L16, CV_X1555, CV_A2R10G10B10, CV_X8B8G8R8 };
enum { FF_RT = 1, FF_DS = 2, FF_SRGB = 4, FF_VTX = 8 };
typedef struct { uint32_t d3d, dxgi, bpb, block, conv, flags, srgb; } Fmt;

#define FOURCC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
static const Fmt FMTS[] = {
    { 21, 87, 4, 1, CVN, FF_RT | FF_SRGB | FF_VTX, 91 },          /* A8R8G8B8 */
    { 22, 88, 4, 1, CVN, FF_RT | FF_SRGB, 93 },                   /* X8R8G8B8 */
    { 23, 85, 2, 1, CVN, 0, 0 },                                  /* R5G6B5 */
    { 24, 86, 2, 1, CV_X1555, 0, 0 },                             /* X1R5G5B5 */
    { 25, 86, 2, 1, CVN, 0, 0 },                                  /* A1R5G5B5 */
    { 26, 115, 2, 1, CVN, 0, 0 },                                 /* A4R4G4B4 */
    { 28, 28, 1, 1, CV_A8, 0, 0 },                                /* A8 */
    { 31, 24, 4, 1, CVN, FF_RT, 0 },                              /* A2B10G10R10 */
    { 32, 28, 4, 1, CVN, FF_RT | FF_SRGB, 29 },                   /* A8B8G8R8 */
    { 33, 28, 4, 1, CV_X8B8G8R8, 0, 0 },                          /* X8B8G8R8 */
    { 34, 35, 4, 1, CVN, FF_RT | FF_VTX, 0 },                     /* G16R16 */
    { 35, 24, 4, 1, CV_A2R10G10B10, 0, 0 },                       /* A2R10G10B10 */
    { 36, 11, 8, 1, CVN, FF_RT | FF_VTX, 0 },                     /* A16B16G16R16 */
    { 50, 28, 1, 1, CV_L8, 0, 0 },                                /* L8 */
    { 51, 28, 2, 1, CV_A8L8, 0, 0 },                              /* A8L8 */
    { 60, 51, 2, 1, CVN, 0, 0 },                                  /* V8U8 */
    { 63, 31, 4, 1, CVN, 0, 0 },                                  /* Q8W8V8U8 */
    { 81, 28, 2, 1, CV_L16, 0, 0 },                               /* L16 */
    { 111, 54, 2, 1, CVN, FF_RT | FF_VTX, 0 },                    /* R16F */
    { 112, 34, 4, 1, CVN, FF_RT | FF_VTX, 0 },                    /* G16R16F */
    { 113, 10, 8, 1, CVN, FF_RT | FF_VTX, 0 },                    /* A16B16G16R16F */
    { 114, 41, 4, 1, CVN, FF_RT | FF_VTX, 0 },                    /* R32F */
    { 115, 16, 8, 1, CVN, FF_RT | FF_VTX, 0 },                    /* G32R32F */
    { 116, 2, 16, 1, CVN, FF_RT | FF_VTX, 0 },                    /* A32B32G32R32F */
    { FOURCC('D', 'X', 'T', '1'), 71, 8, 4, CVN, FF_SRGB, 72 },
    { FOURCC('D', 'X', 'T', '2'), 74, 16, 4, CVN, FF_SRGB, 75 },
    { FOURCC('D', 'X', 'T', '3'), 74, 16, 4, CVN, FF_SRGB, 75 },
    { FOURCC('D', 'X', 'T', '4'), 77, 16, 4, CVN, FF_SRGB, 78 },
    { FOURCC('D', 'X', 'T', '5'), 77, 16, 4, CVN, FF_SRGB, 78 },
    { FOURCC('A', 'T', 'I', '1'), 80, 8, 4, CVN, 0, 0 },
    { FOURCC('A', 'T', 'I', '2'), 83, 16, 4, CVN, 0, 0 },
    /* depth: surfaces only (not readable as textures here, so engines fall back to colour shadow maps) */
    { 70, 55, 2, 1, CVN, FF_DS, 0 },                              /* D16_LOCKABLE */
    { 71, 40, 4, 1, CVN, FF_DS, 0 },                              /* D32 */
    { 73, 45, 4, 1, CVN, FF_DS, 0 },                              /* D15S1 */
    { 75, 45, 4, 1, CVN, FF_DS, 0 },                              /* D24S8 */
    { 77, 45, 4, 1, CVN, FF_DS, 0 },                              /* D24X8 */
    { 79, 45, 4, 1, CVN, FF_DS, 0 },                              /* D24X4S4 */
    { 80, 55, 2, 1, CVN, FF_DS, 0 },                              /* D16 */
    { 82, 40, 4, 1, CVN, FF_DS, 0 },                              /* D32F_LOCKABLE */
    { 83, 45, 4, 1, CVN, FF_DS, 0 },                              /* D24FS8 */
};
static const Fmt* fmt_of(uint32_t d3d)
{
    for (size_t i = 0; i < sizeof FMTS / sizeof *FMTS; i++) if (FMTS[i].d3d == d3d) return &FMTS[i];
    return NULL;
}

/* ---------------------------------------------------------------- objects */

enum { O_TEX = 1, O_CUBE, O_VOL, O_SURF, O_VB, O_IB };
enum { POOL_DEFAULT = 0, POOL_MANAGED, POOL_SYSTEMMEM, POOL_SCRATCH };

typedef struct D9 {
    int kind;
    uint64_t obj;
    uint32_t fmt, usage, pool, fvf;
    uint32_t w, h, d, levels, faces;
    const Fmt* f;
    GpuRes* r;                  /* NULL for system-memory resources */
    uint64_t lock;              /* guest memory in D3D9 layout (textures: all faces and levels) */
    uint64_t lock_raw;
    uint32_t* loff;             /* offset of each sub (face * levels + level) in lock */
    uint32_t lock_lo, lock_hi;  /* buffers: range written since the last upload */
    int locked_sub, lock_ro;
    /* surfaces */
    struct D9* parent;
    uint32_t level, face;
    GpuView *rtv[2], *dsv, *srv_sub;
    /* textures */
    uint64_t* surfs;            /* surface objects, made on first use */
    GpuView* srv[2];            /* plain and sRGB */
    int dead;
} D9;

static ComKind K_D3D, K_Dev, K_Tex, K_Cube, K_Vol, K_Surf, K_VB, K_IB, K_Decl, K_VS, K_PS, K_Query, K_SC, K_SB;

static uint32_t sub_dims(const D9* t, uint32_t level, uint32_t* w, uint32_t* h, uint32_t* d)
{
    *w = t->w >> level; *h = t->h >> level; *d = t->kind == O_VOL ? t->d >> level : 1;
    if (!*w) *w = 1;
    if (!*h) *h = 1;
    if (!*d) *d = 1;
    return 0;
}
static uint32_t d9_pitch(const D9* t, uint32_t level)
{
    uint32_t w, h, d;
    sub_dims(t, level, &w, &h, &d);
    uint32_t b = t->f ? t->f->block : 1;
    return (w + b - 1) / b * (t->f ? t->f->bpb : 4);
}
static uint32_t d9_rows(const D9* t, uint32_t level)
{
    uint32_t w, h, d;
    sub_dims(t, level, &w, &h, &d);
    uint32_t b = t->f ? t->f->block : 1;
    return (h + b - 1) / b;
}

static uint64_t res_backing(GpuRes* r);
static uint64_t lock_mem(D9* t)
{
    if (t->lock) return t->lock;
    if ((t->kind == O_VB || t->kind == O_IB) && t->r) return t->lock = res_backing(t->r);
    if (t->kind == O_VB || t->kind == O_IB) {
        t->lock_raw = g_calloc((uint64_t)t->w + 64);
        t->lock = t->lock_raw ? (t->lock_raw + 15) & ~15ull : 0;
        return t->lock;
    }
    uint32_t n = t->faces * t->levels;
    t->loff = calloc(n + 1, sizeof *t->loff);
    uint64_t off = 0;
    for (uint32_t f = 0; f < t->faces; f++)
        for (uint32_t l = 0; l < t->levels; l++) {
            uint32_t w, h, d;
            sub_dims(t, l, &w, &h, &d);
            t->loff[f * t->levels + l] = (uint32_t)off;
            off += (uint64_t)d9_pitch(t, l) * d9_rows(t, l) * d;
            off = (off + 15) & ~15ull;
        }
    t->loff[n] = (uint32_t)off;
    t->lock_raw = g_calloc(off + 64);
    t->lock = t->lock_raw ? (t->lock_raw + 15) & ~15ull : 0;
    return t->lock;
}

/* DXGI layout of a GPU resource's subresources (as d3d11.c lays them out) */
static void layout_subresources(GpuRes* r)
{
    uint32_t block, bpb = dxgi_format_bytes(r->format, &block);
    uint64_t off = 0;
    int n = 0;
    uint32_t layers = r->type == RES_TEX3D ? 1 : r->array;
    for (uint32_t a = 0; a < layers; a++)
        for (uint32_t m = 0; m < r->mips && n < 256; m++, n++) {
            uint32_t w = r->width >> m, h = r->height >> m, d = r->type == RES_TEX3D ? r->depth >> m : 1;
            if (!w) w = 1;
            if (!h) h = 1;
            if (!d) d = 1;
            uint32_t bw = (w + block - 1) / block, bh = (h + block - 1) / block;
            r->sub_offset[n] = (uint32_t)off;
            r->sub_pitch[n] = bw * bpb;
            r->sub_slice[n] = bw * bpb * bh;
            off += (uint64_t)r->sub_slice[n] * d;
            off = (off + 15) & ~15ull;
        }
    r->nsub = n;
    r->bytes = off;
}
static uint64_t res_backing(GpuRes* r)
{
    if (!r->backing) {
        r->backing_raw = g_calloc(r->bytes + 128);
        r->backing = r->backing_raw ? (r->backing_raw + 63) & ~63ull : 0;
    }
    return r->backing;
}

static GpuBackend* backend(void)
{
    if (!gpu) gpu = gpu_web_backend();
    if (!gpu) gpu = gpu_null_backend();
    return gpu;
}

static GpuRes* new_tex(int type, uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t layers, uint32_t dxgi, uint32_t bind, int cube)
{
    GpuRes* r = calloc(1, sizeof *r);
    r->type = type;
    r->width = w; r->height = h; r->depth = d ? d : 1;
    r->mips = mips ? mips : 1;
    r->array = layers ? layers : 1;
    r->format = dxgi;
    r->samples = 1;
    r->bind = bind;
    r->misc = cube ? 0x4 : 0;
    layout_subresources(r);
    if (backend()->res_create) gpu->res_create(r, 0);
    return r;
}

static GpuRes* new_buf(uint32_t size, uint32_t bind)
{
    GpuRes* r = calloc(1, sizeof *r);
    r->type = RES_BUFFER;
    r->width = size;
    r->bytes = size;
    r->usage = 2;
    r->bind = bind;
    r->cpu = 0x10000;
    r->mips = r->array = r->depth = r->height = 1;
    r->nsub = 1;
    r->sub_pitch[0] = r->sub_slice[0] = size;
    if (backend()->res_create) gpu->res_create(r, 0);
    /* buffers keep their contents in the backing copy, which the backend reads (as d3d11.c does) */
    res_backing(r);
    return r;
}

static GpuView* new_view(GpuRes* r, int kind, uint32_t format, uint32_t dim, const uint32_t* u, int nu)
{
    GpuView* v = calloc(1, sizeof *v);
    v->res = r;
    v->kind = kind;
    v->format = format;
    v->dim = dim;
    uint32_t desc[10] = { format, dim };
    for (int i = 0; i < nu && i < 8; i++) desc[2 + i] = u[i];
    memcpy(v->desc, desc, sizeof desc);
    if (backend()->view_create) gpu->view_create(v);
    return v;
}
static void free_view(GpuView* v) { if (v) { if (gpu && gpu->view_destroy) gpu->view_destroy(v); free(v); } }

/* ---------------------------------------------------------------- device state */

typedef struct {
    uint64_t obj;
    uint32_t n;
    struct { uint16_t stream, offset; uint8_t type, method, usage, index; } e[64];
    struct { uint32_t inst; GpuLayout* l; } var[8];
    int nvar;
} Decl;

typedef struct VsVar { uint32_t um, sm; GpuShader* s; struct VsVar* next; } VsVar;
typedef struct {
    uint64_t obj;
    int ps;
    uint8_t* code;
    uint32_t size;
    GpuShader* base;
    VsVar* vars;
    uint8_t usage[16], index[16];
} Sh;

typedef struct { uint64_t obj; uint32_t type; int issued; } Query;

#define NRS 256
static struct {
    uint64_t d3d, dev, sc, backbuf, autodepth;
    uint32_t bb_w, bb_h, bb_fmt, ds_fmt;
    uint32_t pp[16];                /* present parameters (as given) */
    uint32_t rs[NRS];
    uint32_t ss[21][14];            /* samplers 0-15, dmap 16, vertex 17-20 */
    uint64_t tex[21];
    uint64_t rt[4], ds;
    uint32_t vp[6];
    int32_t scissor[4];
    uint64_t vb[16];
    uint32_t vb_off[16], vb_stride[16], vb_freq[16];
    uint64_t ib;
    Decl* decl;
    Sh *vs, *ps;
    uint32_t fvf;
    uint64_t vcb, pcb;              /* constants in guest memory, D3D9_CB_VEC4S vec4s each */
    GpuRes *vcb_r, *pcb_r;
    int vcb_dirty, pcb_dirty;
    int in_scene;
    GpuCtx ctx;
    /* ring for DrawPrimitiveUP and the built-in draws */
    GpuRes *up_vb, *up_ib;
    uint32_t up_vb_at, up_ib_at;
    uint32_t frames;
} S;

#define UP_SIZE (4u << 20)

static void set_cb_extra(void)
{
    /* vertex shaders: the half-pixel offset in clip space for the current viewport */
    float* v = (float*)G2H(S.vcb + 16ull * D3D9_CB_EXTRA);
    v[0] = S.vp[2] ? -1.0f / (float)S.vp[2] : 0.0f;
    v[1] = S.vp[3] ? 1.0f / (float)S.vp[3] : 0.0f;
    S.vcb_dirty = 1;
    /* pixel shaders: the alpha test */
    float* p = (float*)G2H(S.pcb + 16ull * D3D9_CB_EXTRA);
    p[0] = S.rs[15] ? (float)(S.rs[25] & 15) : 8.0f;
    p[1] = (float)(S.rs[24] & 0xFF) / 255.0f;
    S.pcb_dirty = 1;
}

static void default_state(void)
{
    memset(S.rs, 0, sizeof S.rs);
    S.rs[7] = S.ds ? 1 : 0;     /* ZENABLE */
    S.rs[8] = 3;                /* FILLMODE solid */
    S.rs[9] = 2;                /* SHADEMODE gouraud */
    S.rs[14] = 1;               /* ZWRITEENABLE */
    S.rs[16] = 1;               /* LASTPIXEL */
    S.rs[19] = 2;               /* SRCBLEND one */
    S.rs[20] = 1;               /* DESTBLEND zero */
    S.rs[22] = 3;               /* CULLMODE ccw */
    S.rs[23] = 4;               /* ZFUNC lessequal */
    S.rs[25] = 8;               /* ALPHAFUNC always */
    S.rs[53] = S.rs[54] = S.rs[55] = 1;
    S.rs[56] = 8;
    S.rs[58] = S.rs[59] = 0xFFFFFFFF;
    S.rs[60] = 0xFFFFFFFF;
    S.rs[136] = 1;              /* CLIPPING */
    S.rs[137] = 1;              /* LIGHTING */
    S.rs[154] = 0x3F800000;     /* POINTSIZE 1.0 */
    S.rs[162] = 0xFFFFFFFF;     /* MULTISAMPLEMASK */
    S.rs[168] = S.rs[190] = S.rs[191] = S.rs[192] = 0xF;
    S.rs[171] = 1;              /* BLENDOP add */
    S.rs[186] = S.rs[187] = S.rs[188] = 1;
    S.rs[189] = 8;
    S.rs[193] = 0xFFFFFFFF;     /* BLENDFACTOR */
    S.rs[207] = 2; S.rs[208] = 1; S.rs[209] = 1;
    for (int s = 0; s < 21; s++) {
        memset(S.ss[s], 0, sizeof S.ss[s]);
        S.ss[s][1] = S.ss[s][2] = S.ss[s][3] = 1;
        S.ss[s][5] = S.ss[s][6] = 1;
        S.ss[s][10] = 1;
    }
    for (int i = 0; i < 16; i++) S.vb_freq[i] = 1;
    set_cb_extra();
}

static int samp_slot(uint32_t s)
{
    if (s < 16) return (int)s;
    if (s == 256) return 16;
    if (s >= 257 && s <= 260) return 17 + (int)(s - 257);
    return -1;
}

/* ---------------------------------------------------------------- state objects, cached by content */

typedef struct SCache { int kind; uint8_t desc[264]; GpuState* s; struct SCache* next; } SCache;
static SCache* scache[256];
static GpuState* state_of(int kind, const void* desc, uint32_t size)
{
    uint32_t h = 2166136261u ^ (uint32_t)kind;
    for (uint32_t i = 0; i < size; i++) h = (h ^ ((const uint8_t*)desc)[i]) * 16777619u;
    for (SCache* e = scache[h & 255]; e; e = e->next) if (e->kind == kind && !memcmp(e->desc, desc, size)) return e->s;
    SCache* e = calloc(1, sizeof *e);
    e->kind = kind;
    memcpy(e->desc, desc, size);
    e->s = calloc(1, sizeof *e->s);
    e->s->kind = kind;
    memcpy(e->s->desc, desc, size);
    if (backend()->state_create) gpu->state_create(e->s);
    e->next = scache[h & 255];
    scache[h & 255] = e;
    return e->s;
}

static uint32_t alpha_factor(uint32_t b)
{
    /* D3D11 rejects colour factors for alpha; D3D9 uses their alpha there */
    switch (b) {
    case 3: return 5; case 4: return 6; case 9: return 7; case 10: return 8;
    case 12: return 5; case 13: return 6;
    }
    return b;
}
static uint32_t colour_factor(uint32_t b) { return b == 12 ? 5 : b == 13 ? 6 : b ? b : 2; }

static GpuState* blend_state(void)
{
    uint32_t d[66];
    memset(d, 0, sizeof d);
    int on = S.rs[27] != 0;
    d[1] = 1;   /* independent: per-target write masks */
    uint32_t wm[4] = { S.rs[168], S.rs[190], S.rs[191], S.rs[192] };
    uint32_t src = colour_factor(S.rs[19]), dst = colour_factor(S.rs[20]), op = S.rs[171] ? S.rs[171] : 1;
    /* BOTHSRCALPHA: source alpha and inverse source alpha for the destination */
    if (S.rs[19] == 12) dst = 6;
    if (S.rs[19] == 13) { src = 6; dst = 5; }
    uint32_t sa = alpha_factor(src), da = alpha_factor(dst), opa = op;
    if (S.rs[206]) { sa = alpha_factor(colour_factor(S.rs[207])); da = alpha_factor(colour_factor(S.rs[208])); opa = S.rs[209] ? S.rs[209] : 1; }
    for (int i = 0; i < 8; i++) {
        uint32_t* rt = d + 2 + 8 * i;
        rt[0] = (uint32_t)on;
        rt[1] = src; rt[2] = dst; rt[3] = op;
        rt[4] = sa; rt[5] = da; rt[6] = opa;
        rt[7] = (i < 4 ? wm[i] : 0xF) & 0xF;
    }
    return state_of(1, d, 264);
}

static GpuState* depth_state(void)
{
    uint32_t d[13];
    memset(d, 0, sizeof d);
    d[0] = S.rs[7] != 0;
    d[1] = S.rs[14] != 0;
    d[2] = S.rs[23] ? S.rs[23] : 4;
    d[3] = S.rs[52] != 0;
    d[4] = (S.rs[58] & 0xFF) | (S.rs[59] & 0xFF) << 8;
    d[5] = S.rs[53]; d[6] = S.rs[54]; d[7] = S.rs[55]; d[8] = S.rs[56];
    if (S.rs[185]) { d[9] = S.rs[186]; d[10] = S.rs[187]; d[11] = S.rs[188]; d[12] = S.rs[189]; }
    else { d[9] = d[5]; d[10] = d[6]; d[11] = d[7]; d[12] = d[8]; }
    for (int i = 5; i < 13; i++) if (!d[i]) d[i] = i == 8 || i == 12 ? 8 : 1;
    return state_of(2, d, 52);
}

static GpuState* raster_state(void)
{
    uint32_t d[10];
    memset(d, 0, sizeof d);
    d[0] = S.rs[8] == 2 ? 2 : 3;
    /* front faces are clockwise in D3D9: CCW culls the back, CW the front */
    d[1] = S.rs[22] == 1 ? 1 : S.rs[22] == 2 ? 2 : 3;
    d[2] = 0;
    /* D3D9's depth bias is in depth units; D3D11's in steps of a 24-bit depth buffer */
    float bias = f32of(S.rs[195]), slope = f32of(S.rs[175]);
    int32_t ib = (int32_t)lrintf(bias * 16777215.0f);
    memcpy(&d[3], &ib, 4);
    memcpy(&d[5], &slope, 4);
    d[6] = 1;
    d[7] = S.rs[174] != 0;
    return state_of(3, d, 40);
}

static GpuState* sampler_state(int slot)
{
    const uint32_t* s = S.ss[slot];
    uint32_t d[13];
    memset(d, 0, sizeof d);
    uint32_t mag = s[5], min = s[6], mip = s[7];
    uint32_t filter;
    if (mag == 3 || min == 3) filter = 0x55;
    else filter = (min >= 2 ? 0x10 : 0) | (mag >= 2 ? 0x4 : 0) | (mip == 2 ? 0x1 : 0);
    d[0] = filter;
    for (int i = 0; i < 3; i++) d[1 + i] = s[1 + i] >= 1 && s[1 + i] <= 5 ? s[1 + i] : 1;
    d[4] = s[8];                                    /* MIPMAPLODBIAS (float bits) */
    d[5] = s[10] ? (s[10] > 16 ? 16 : s[10]) : 1;
    d[6] = 8;
    uint32_t bc = s[4];
    float border[4] = { ((bc >> 16) & 0xFF) / 255.0f, ((bc >> 8) & 0xFF) / 255.0f, (bc & 0xFF) / 255.0f, (bc >> 24) / 255.0f };
    memcpy(&d[7], border, 16);
    float minlod = (float)s[9], maxlod = mip == 0 ? (float)s[9] : 1000.0f;
    memcpy(&d[11], &minlod, 4);
    memcpy(&d[12], &maxlod, 4);
    return state_of(4, d, 52);
}

/* ---------------------------------------------------------------- resources */

static D9* D(uint64_t o) { return o ? (D9*)com_host(o) : NULL; }

static void unbind(uint64_t o)
{
    for (int i = 0; i < 21; i++) if (S.tex[i] == o) S.tex[i] = 0;
    for (int i = 0; i < 4; i++) if (S.rt[i] == o) S.rt[i] = 0;
    if (S.ds == o) S.ds = 0;
    for (int i = 0; i < 16; i++) if (S.vb[i] == o) S.vb[i] = 0;
    if (S.ib == o) S.ib = 0;
}

/* freed after the frame is submitted, once nothing references them (as d3d11.c does) */
static uint64_t* doomed;
static size_t ndoomed, cap_doomed;
static void doom(uint64_t o)
{
    if (ndoomed == cap_doomed) { cap_doomed = cap_doomed ? cap_doomed * 2 : 1024; doomed = realloc(doomed, cap_doomed * sizeof *doomed); }
    doomed[ndoomed++] = o;
}
static void free_d9(D9* t)
{
    if (t->r) {
        if (gpu->res_destroy) gpu->res_destroy(t->r);
        if (t->r->backing_raw) g_free(t->r->backing_raw);
        free(t->r);
    }
    for (int i = 0; i < 2; i++) { free_view(t->srv[i]); free_view(t->rtv[i]); }
    free_view(t->dsv);
    free_view(t->srv_sub);
    if (t->lock_raw) g_free(t->lock_raw);
    free(t->loff);
    if (t->surfs) {
        for (uint32_t i = 0; i < t->faces * t->levels; i++)
            if (t->surfs[i]) {
                D9* s = D(t->surfs[i]);
                unbind(t->surfs[i]);
                if (s) { free_d9(s); }
                com_free(t->surfs[i]);
            }
        free(t->surfs);
    }
    free(t);
}
static void reap(void)
{
    size_t freed = 0;
    for (size_t i = 0; i < ndoomed; i++) {
        uint64_t o = doomed[i];
        if (LD32(o + 8)) continue;
        D9* t = D(o);
        if (t) { unbind(o); free_d9(t); }
        com_free(o);
        freed++;
    }
    ndoomed = 0;
    if (freed && gpu->forget) gpu->forget();
}


/* surfaces of a texture share its reference count, as in D3D9 */
static void surf_addref(CPU* c)
{
    D9* s = D(THIS);
    uint64_t o = s && s->parent ? s->parent->obj : THIS;
    uint32_t n = (uint32_t)LD32(o + 8) + 1;
    ST32(o + 8, n);
    RET(n);
}
static void res_release(CPU* c)
{
    D9* s = D(THIS);
    uint64_t o = s && s->parent ? s->parent->obj : THIS;
    uint32_t n = (uint32_t)LD32(o + 8);
    if (!n) { RET(0); return; }
    ST32(o + 8, --n);
    if (!n) doom(o);
    RET(n);
}

static uint64_t surface_of(D9* t, uint32_t face, uint32_t level)
{
    if (face >= t->faces || level >= t->levels) return 0;
    if (!t->surfs) t->surfs = calloc(t->faces * t->levels, sizeof *t->surfs);
    uint32_t i = face * t->levels + level;
    if (!t->surfs[i]) {
        D9* s = calloc(1, sizeof *s);
        s->kind = O_SURF;
        s->parent = t;
        s->face = face;
        s->level = level;
        s->fmt = t->fmt; s->f = t->f; s->usage = t->usage; s->pool = t->pool;
        sub_dims(t, level, &s->w, &s->h, &s->d);
        s->levels = 1; s->faces = 1;
        s->r = NULL;
        s->obj = com_new(&K_Surf, s, 0);
        t->surfs[i] = s->obj;
    }
    return t->surfs[i];
}

/* the GPU resource and subresource behind a surface or texture level */
static GpuRes* gres(D9* s, uint32_t* sub)
{
    if (!s) return NULL;
    if (s->kind == O_SURF && s->parent) {
        D9* t = s->parent;
        *sub = s->face * t->levels + s->level;
        return t->r;
    }
    *sub = 0;
    return s->r;
}

static void convert_rows(const D9* t, const uint8_t* src, uint32_t src_pitch, uint8_t* dst, uint32_t dst_pitch, uint32_t w, uint32_t rows)
{
    for (uint32_t y = 0; y < rows; y++) {
        const uint8_t* s = src + (size_t)y * src_pitch;
        uint8_t* d = dst + (size_t)y * dst_pitch;
        for (uint32_t x = 0; x < w; x++) {
            switch (t->f->conv) {
            case CV_L8: d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = s[x]; d[4 * x + 3] = 255; break;
            case CV_A8L8: d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = s[2 * x]; d[4 * x + 3] = s[2 * x + 1]; break;
            case CV_A8: d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = 0; d[4 * x + 3] = s[x]; break;
            case CV_L16: d[4 * x] = d[4 * x + 1] = d[4 * x + 2] = s[2 * x + 1]; d[4 * x + 3] = 255; break;
            case CV_X8B8G8R8: memcpy(d + 4 * x, s + 4 * x, 3); d[4 * x + 3] = 255; break;
            case CV_X1555: { uint16_t v; memcpy(&v, s + 2 * x, 2); v |= 0x8000; memcpy(d + 2 * x, &v, 2); break; }
            case CV_A2R10G10B10: {
                uint32_t v; memcpy(&v, s + 4 * x, 4);
                v = (v & 0xC00FFC00u) | ((v >> 20) & 0x3FF) | ((v & 0x3FF) << 20);
                memcpy(d + 4 * x, &v, 4);
                break;
            }
            }
        }
    }
}

/* uploads a rectangle of a texture level from guest memory in D3D9 layout (src: its top-left), converting if needed */
static void upload(D9* t, uint32_t face, uint32_t level, const uint32_t* rect, uint64_t src, uint32_t src_pitch)
{
    if (!t->r || !gpu->res_upload || !src) return;
    uint32_t w, h, d;
    sub_dims(t, level, &w, &h, &d);
    uint32_t x0 = 0, y0 = 0, x1 = w, y1 = h;
    if (rect) { x0 = rect[0]; y0 = rect[1]; x1 = rect[2]; y1 = rect[3]; }
    if (x1 > w) x1 = w;
    if (y1 > h) y1 = h;
    if (x0 >= x1 || y0 >= y1) return;
    uint32_t b = t->f->block;
    uint32_t sub = face * t->levels + level;
    int32_t box[6] = { (int32_t)x0, (int32_t)y0, 0, (int32_t)x1, (int32_t)y1, (int32_t)d };
    int whole = !rect || (x0 == 0 && y0 == 0 && x1 == w && y1 == h);
    uint32_t rows = (y1 - y0 + b - 1) / b;
    if (t->f->conv) {
        uint32_t cw = x1 - x0;
        uint32_t dbpp = t->f->conv == CV_X1555 ? 2 : 4;
        uint32_t dp = cw * dbpp;
        uint64_t tmp = g_alloc((uint64_t)dp * rows * d + 16);
        if (!tmp) return;
        for (uint32_t z = 0; z < d; z++)
            convert_rows(t, G2H(src + (uint64_t)z * src_pitch * rows), src_pitch, G2H(tmp + (uint64_t)z * dp * rows), dp, cw, rows);
        gpu->res_upload(t->r, sub, whole ? NULL : box, tmp, dp, dp * rows);
        g_free(tmp);
        return;
    }
    gpu->res_upload(t->r, sub, whole ? NULL : box, src, src_pitch, src_pitch * rows);
}

static GpuView* tex_srv(D9* t, int srgb)
{
    if (!t || !t->r) return NULL;
    srgb = srgb && t->f->srgb;
    if (!t->srv[srgb]) {
        uint32_t fmt = srgb ? t->f->srgb : t->f->dxgi;
        uint32_t u[2] = { 0, t->levels };
        t->srv[srgb] = new_view(t->r, 0, fmt, t->kind == O_CUBE ? 9 : t->kind == O_VOL ? 8 : 4, u, 2);
    }
    return t->srv[srgb];
}

static GpuView* surf_rtv(D9* s, int srgb)
{
    uint32_t sub;
    GpuRes* r = gres(s, &sub);
    if (!r) return NULL;
    srgb = srgb && s->f->srgb && (s->f->flags & FF_SRGB);
    if (!s->rtv[srgb]) {
        uint32_t fmt = srgb ? s->f->srgb : s->f->dxgi;
        if (s->parent && s->parent->kind == O_CUBE) {
            uint32_t u[3] = { s->level, s->face, 1 };
            s->rtv[srgb] = new_view(r, 2, fmt, 5, u, 3);
        } else {
            uint32_t u[1] = { s->parent ? s->level : 0 };
            s->rtv[srgb] = new_view(r, 2, fmt, 4, u, 1);
        }
    }
    return s->rtv[srgb];
}

static GpuView* surf_dsv(D9* s)
{
    uint32_t sub;
    GpuRes* r = gres(s, &sub);
    if (!r) return NULL;
    if (!s->dsv) {
        uint32_t u[2] = { 0, s->parent ? s->level : 0 };
        s->dsv = new_view(r, 3, s->f->dxgi, 3, u, 2);
    }
    return s->dsv;
}

/* a texture view of just this surface (for StretchRect) */
static GpuView* surf_srv(D9* s)
{
    uint32_t sub;
    GpuRes* r = gres(s, &sub);
    if (!r) return NULL;
    if (!s->srv_sub) {
        if (s->parent && s->parent->kind == O_CUBE) {
            uint32_t u[4] = { s->level, 1, s->face, 1 };
            s->srv_sub = new_view(r, 0, s->f->dxgi, 5, u, 4);
        } else {
            uint32_t u[2] = { s->parent ? s->level : 0, 1 };
            s->srv_sub = new_view(r, 0, s->f->dxgi, 4, u, 2);
        }
    }
    return s->srv_sub;
}

static D9* make_res(int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool)
{
    const Fmt* f = fmt_of(fmt);
    if (!f) return NULL;
    D9* t = calloc(1, sizeof *t);
    t->kind = kind;
    t->fmt = fmt; t->f = f; t->usage = usage; t->pool = pool;
    t->w = w ? w : 1; t->h = h ? h : 1; t->d = d ? d : 1;
    t->faces = kind == O_CUBE ? 6 : 1;
    if (!levels) {
        uint32_t m = t->w > t->h ? t->w : t->h;
        if (kind == O_VOL && t->d > m) m = t->d;
        levels = 1;
        while (m > 1) { m >>= 1; levels++; }
    }
    t->levels = levels;
    t->locked_sub = -1;
    if (pool == POOL_SYSTEMMEM || pool == POOL_SCRATCH) return t;
    uint32_t bind = 0x8;
    if (usage & 1) bind |= 0x20;
    if (f->flags & FF_DS) bind = 0x40;
    t->r = new_tex(kind == O_VOL ? RES_TEX3D : RES_TEX2D, t->w, t->h, t->d, levels, t->faces, f->dxgi, bind, kind == O_CUBE);
    return t;
}

/* ---------------------------------------------------------------- IDirect3D9 */

static void d3d_qi(CPU* c) { ST32(THIS + 8, LD32(THIS + 8) + 1); STP(A3, THIS); RET(D3D_OK); }
static void d3d_adapter_count(CPU* c) { RET(1); }
static void d3d_identifier(CPU* c)
{
    uint64_t p = A4;
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    memset(G2H(p), 0, 1100);
    strcpy((char*)G2H(p), "nvd3dum.dll");
    strcpy((char*)G2H(p + 512), "NVIDIA GeForce GTX 660 (UEWASM WebGPU)");
    strcpy((char*)G2H(p + 1024), "\\\\.\\DISPLAY1");
    ST32(p + 1056, 0x0010000Cu);        /* driver version 9.18.13.... */
    ST32(p + 1060, 0x00090012u);
    ST32(p + 1064, 0x10DE);             /* NVIDIA */
    ST32(p + 1068, 0x11C0);
    ST32(p + 1072, 0);
    ST32(p + 1076, 0xA1);
    for (int i = 0; i < 16; i++) *(uint8_t*)G2H(p + 1080 + i) = (uint8_t)(0x5A + i * 7);
    ST32(p + 1096, 1);
    RET(D3D_OK);
}

static const uint32_t MODES[][2] = { { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 } };
static void d3d_mode_count(CPU* c) { uint32_t f = (uint32_t)A3; RET(f == 22 || f == 21 ? 4 : 0); }
static void d3d_enum_modes(CPU* c)
{
    uint32_t i = (uint32_t)A4;
    uint64_t p = ARG(5);
    if (i >= 4 || !p) { RET(D3DERR_INVALIDCALL); return; }
    ST32(p, MODES[i][0]); ST32(p + 4, MODES[i][1]); ST32(p + 8, 60); ST32(p + 12, 22);
    RET(D3D_OK);
}
static void display_mode(uint64_t p)
{
    int w = 1280, h = 720;
    host_canvas_size(&w, &h);
    ST32(p, (uint32_t)w); ST32(p + 4, (uint32_t)h); ST32(p + 8, 60); ST32(p + 12, 22);
}
static void d3d_display_mode(CPU* c) { if (A3) display_mode(A3); RET(D3D_OK); }
static void d3d_check_type(CPU* c) { RET(D3D_OK); }

static void d3d_check_format(CPU* c)
{
    uint32_t usage = (uint32_t)ARG(5), rtype = (uint32_t)ARG(6), fmt = (uint32_t)ARG(7);
    const Fmt* f = fmt_of(fmt);
    uint32_t ok = D3D_OK;
    if (!f) ok = D3DERR_NOTAVAILABLE;
    else if (f->flags & FF_DS) {
        /* depth formats only as depth surfaces: shaders can't read them back here */
        if (!(usage & 2) || rtype == 3) ok = D3DERR_NOTAVAILABLE;
    } else {
        if (usage & 2) ok = D3DERR_NOTAVAILABLE;
        if ((usage & 1) && !(f->flags & FF_RT)) ok = D3DERR_NOTAVAILABLE;
        if ((usage & 0x40000) && !((f->flags & FF_SRGB) && (f->flags & FF_RT))) ok = D3DERR_NOTAVAILABLE;   /* SRGBWRITE */
        if ((usage & 0x10000) && !(f->flags & FF_SRGB)) ok = D3DERR_NOTAVAILABLE;   /* SRGBREAD */
        if ((usage & 0x100000) && !(f->flags & FF_VTX)) ok = D3DERR_NOTAVAILABLE;  /* VERTEXTEXTURE */
        if ((usage & 0x80000) && f->dxgi == 2) ok = D3DERR_NOTAVAILABLE;          /* blending into 32-bit float x4 */
        if (rtype == 4 && f->block > 1) ok = D3DERR_NOTAVAILABLE;                 /* compressed volume textures */
    }
    RET(ok);
}
static void d3d_check_ms(CPU* c)
{
    uint32_t ms = (uint32_t)ARG(6);
    uint64_t q = ARG(7);
    if (q) ST32(q, 1);
    RET(ms <= 1 ? D3D_OK : D3DERR_NOTAVAILABLE);
}
static void d3d_check_ds_match(CPU* c) { RET(D3D_OK); }
static void d3d_check_conv(CPU* c) { RET(D3D_OK); }

static void fill_caps(uint64_t p)
{
    uint32_t* k = (uint32_t*)G2H(p);
    memset(k, 0, 304);
    float fv;
    k[0] = 1;                   /* HAL */
    k[2] = 0x20000;
    k[3] = 0x20000 | 0x100000 | 0x20000000 | 0x40000000 | 0x80000000;
    k[4] = 0x20 | 0x80 | 0x100 | 0x200;
    k[5] = 0x80000000 | 0xF;
    k[6] = 3;
    k[7] = 0x10 | 0x20 | 0x40 | 0x80 | 0x200 | 0x400 | 0x800 | 0x1000 | 0x2000 | 0x8000 | 0x10000 | 0x20000 | 0x80000 | 0x100000;
    k[8] = 0x2 | 0x10 | 0x20 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x40000 | 0x80000 | 0x100000 | 0x200000;
    k[9] = 0x1 | 0x10 | 0x80 | 0x100 | 0x2000 | 0x10000 | 0x20000 | 0x100000 | 0x200000 | 0x400000 | 0x1000000 | 0x2000000 | 0x4000000 | 0x8000000;
    k[10] = 0xFF;
    k[11] = k[12] = 0x3FFF;
    k[13] = 0xFF;
    k[14] = 0x8 | 0x200 | 0x4000 | 0x80000;
    k[15] = 0x1 | 0x4 | 0x400 | 0x800 | 0x2000 | 0x4000 | 0x8000 | 0x10000;
    k[16] = k[17] = 0x07030700;
    k[18] = 0x03030300;
    k[19] = k[20] = 0x3F;
    k[21] = 0x1F;
    k[22] = k[23] = 8192;
    k[24] = 2048;
    k[25] = k[26] = 8192;
    k[27] = 16;
    fv = 1e10f; memcpy(&k[28], &fv, 4);
    fv = -32768.0f; memcpy(&k[29], &fv, 4); memcpy(&k[30], &fv, 4);
    fv = 32768.0f; memcpy(&k[31], &fv, 4); memcpy(&k[32], &fv, 4);
    k[34] = 0x1FF;
    k[35] = 0x00100008;
    k[36] = 0x3FFFFFF;
    k[37] = 8;
    k[38] = 8;
    k[39] = 0x17B;
    k[40] = 8;
    k[41] = 6;
    k[42] = 4;
    k[43] = 0;
    fv = 256.0f; memcpy(&k[44], &fv, 4);
    k[45] = 0xFFFFFF;
    k[46] = 0xFFFFFF;
    k[47] = 16;
    k[48] = 508;
    k[49] = 0xFFFE0300;
    k[50] = 256;
    k[51] = 0xFFFF0300;
    fv = 65504.0f; memcpy(&k[52], &fv, 4);
    k[53] = 0x51;
    k[56] = 0; k[57] = 0; k[58] = 1;
    k[59] = 0x33F;
    k[60] = 4;
    k[61] = 0x03000300;
    k[62] = 1; k[63] = 24; k[64] = 32; k[65] = 4;
    k[66] = 0x1F; k[67] = 24; k[68] = 32; k[69] = 4; k[70] = 512;
    k[71] = 0x03000300;
    k[72] = 0xFFFF; k[73] = 0xFFFF;
    k[74] = 32768; k[75] = 32768;
}
static void d3d_caps(CPU* c) { if (A4) fill_caps(A4); RET(D3D_OK); }
static void d3d_monitor(CPU* c) { RET(0x10001); }

static void make_backbuffer(void);
static void d3d_create_device(CPU* c);

/* ---------------------------------------------------------------- device: helpers */

static void dev_ok(CPU* c) { RET(D3D_OK); }
static void dev_zero(CPU* c) { RET(0); }

static uint64_t addref(uint64_t o)
{
    if (!o) return 0;
    D9* s = D(o);
    uint64_t r = s && s->parent ? s->parent->obj : o;
    ST32(r + 8, LD32(r + 8) + 1);
    return o;
}

static void present_params(uint64_t pp)
{
    uint32_t hw = RT_G32 ? 28 : 32;
    S.bb_w = (uint32_t)LD32(pp);
    S.bb_h = (uint32_t)LD32(pp + 4);
    S.bb_fmt = (uint32_t)LD32(pp + 8);
    if (!S.bb_w || !S.bb_h) {
        int w = 1280, h = 720;
        host_canvas_size(&w, &h);
        S.bb_w = (uint32_t)w; S.bb_h = (uint32_t)h;
        ST32(pp, S.bb_w); ST32(pp + 4, S.bb_h);
    }
    if (!fmt_of(S.bb_fmt) || S.bb_fmt == 0) { S.bb_fmt = 22; ST32(pp + 8, 22); }
    if (!LD32(pp + 12)) ST32(pp + 12, 1);
    uint32_t auto_ds = (uint32_t)LD32(pp + hw + GP + 4);
    S.ds_fmt = auto_ds ? (uint32_t)LD32(pp + hw + GP + 8) : 0;
    memcpy(S.pp, G2H(pp), RT_G32 ? 56 : 64);
}

static uint64_t new_surface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t usage, uint32_t pool)
{
    D9* s = make_res(O_SURF, w, h, 1, 1, usage, fmt, pool);
    if (!s) return 0;
    s->obj = com_new(&K_Surf, s, 0);
    return s->obj;
}

static void make_backbuffer(void)
{
    if (S.backbuf) { ST32(S.backbuf + 8, 0); doom(S.backbuf); }
    if (S.autodepth) { ST32(S.autodepth + 8, 0); doom(S.autodepth); }
    S.backbuf = new_surface(S.bb_w, S.bb_h, S.bb_fmt, 1, POOL_DEFAULT);
    S.autodepth = S.ds_fmt ? new_surface(S.bb_w, S.bb_h, S.ds_fmt, 2, POOL_DEFAULT) : 0;
    /* the device keeps its own references (internal, never reaching zero from game releases) */
    if (S.backbuf) ST32(S.backbuf + 8, 1000000);
    if (S.autodepth) ST32(S.autodepth + 8, 1000000);
    memset(S.rt, 0, sizeof S.rt);
    S.rt[0] = S.backbuf;
    S.ds = S.autodepth;
    S.vp[0] = S.vp[1] = 0; S.vp[2] = S.bb_w; S.vp[3] = S.bb_h;
    float z0 = 0.0f, z1 = 1.0f;
    memcpy(&S.vp[4], &z0, 4); memcpy(&S.vp[5], &z1, 4);
    S.scissor[0] = S.scissor[1] = 0; S.scissor[2] = (int32_t)S.bb_w; S.scissor[3] = (int32_t)S.bb_h;
    host_log("[d3d9] back buffer %ux%u format %u, depth format %u", S.bb_w, S.bb_h, S.bb_fmt, S.ds_fmt);
}

/* ---------------------------------------------------------------- draws */

static Sh* builtin_vs(int tex);
static Sh* builtin_ps(int tex);

static GpuShader* vs_variant(Sh* vs, uint32_t um, uint32_t sm)
{
    if (!um && !sm) return vs->base;
    for (VsVar* v = vs->vars; v; v = v->next) if (v->um == um && v->sm == sm) return v->s;
    VsVar* v = calloc(1, sizeof *v);
    v->um = um; v->sm = sm;
    GpuShader* s = calloc(1, sizeof *s);
    s->stage = SH_VS;
    s->size = vs->size + 12;
    s->code = malloc(s->size);
    memcpy(s->code, vs->code, vs->size);
    uint32_t tr[3] = { D3D9_INT_MAGIC, um, sm };
    memcpy(s->code + vs->size, tr, 12);
    if (gpu->shader_create) gpu->shader_create(s);
    v->s = s;
    v->next = vs->vars;
    vs->vars = v;
    return s;
}

static const uint32_t DECL_DXGI[17] = { 41, 16, 6, 2, 87, 30, 38, 14, 28, 37, 13, 35, 11, 0, 0, 34, 10 };

/* the input layout for a declaration with the current per-instance streams; the integer-input masks for the shader */
static GpuLayout* decl_layout(Decl* dl, Sh* vs, uint32_t inst, uint32_t* um, uint32_t* sm)
{
    *um = *sm = 0;
    for (uint32_t i = 0; i < dl->n; i++) {
        uint8_t ty = dl->e[i].type;
        if (ty != 5 && ty != 6 && ty != 7) continue;
        for (int r = 0; r < 16; r++)
            if (vs->usage[r] == dl->e[i].usage && vs->index[r] == dl->e[i].index) { if (ty == 5) *um |= 1u << r; else *sm |= 1u << r; }
    }
    for (int k = 0; k < dl->nvar; k++) if (dl->var[k].inst == inst) return dl->var[k].l;
    GpuLayout* l = calloc(1, sizeof *l);
    static const char* const NAMES[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
                                         "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
    for (uint32_t i = 0; i < dl->n && l->count < 32; i++) {
        uint8_t ty = dl->e[i].type, us = dl->e[i].usage;
        if (ty > 16 || !DECL_DXGI[ty] || us > 13) continue;
        uint32_t k = l->count++;
        snprintf(l->el[k].semantic, sizeof l->el[k].semantic, "%s", NAMES[us]);
        l->el[k].index = dl->e[i].index;
        l->el[k].format = DECL_DXGI[ty];
        l->el[k].slot = dl->e[i].stream & 15;
        l->el[k].offset = dl->e[i].offset;
        l->el[k].per_instance = (inst >> (dl->e[i].stream & 15)) & 1;
        l->el[k].step = l->el[k].per_instance ? 1 : 0;
    }
    if (gpu->layout_create) gpu->layout_create(l);
    if (dl->nvar < 8) { dl->var[dl->nvar].inst = inst; dl->var[dl->nvar].l = l; dl->nvar++; }
    return l;
}

static uint32_t prim_count(uint32_t type, uint32_t n)
{
    switch (type) {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5: case 6: return n + 2;
    }
    return 0;
}

static void upload_constants(void)
{
    if (S.vcb_dirty && gpu->res_upload) { gpu->res_upload(S.vcb_r, 0, NULL, S.vcb, D3D9_CB_VEC4S * 16, D3D9_CB_VEC4S * 16); S.vcb_dirty = 0; }
    if (S.pcb_dirty && gpu->res_upload) { gpu->res_upload(S.pcb_r, 0, NULL, S.pcb, D3D9_CB_VEC4S * 16, D3D9_CB_VEC4S * 16); S.pcb_dirty = 0; }
}

/* render targets and depth buffer into the context; 0 if nothing can be drawn */
static int bind_targets(GpuCtx* x)
{
    memset(x->rtv, 0, sizeof x->rtv);
    x->dsv = NULL;
    uint32_t w = 0, h = 0;
    for (int i = 0; i < 4; i++) {
        D9* s = D(S.rt[i]);
        if (!s) continue;
        x->rtv[i] = surf_rtv(s, S.rs[194] != 0);
        if (!w) { w = s->w; h = s->h; }
    }
    D9* ds = D(S.ds);
    if (ds) {
        /* D3D9 allows a depth buffer larger than the colour target; WebGPU wants them the same size */
        if (!w || (ds->w == w && ds->h == h)) x->dsv = surf_dsv(ds);
        else {
            static int said;
            if (said++ < 4) host_log("[d3d9] depth buffer %ux%u doesn't match render target %ux%u: drawn without depth", ds->w, ds->h, w, h);
        }
    }
    return x->rtv[0] || x->dsv;
}

static void bind_viewport(GpuCtx* x)
{
    x->vp[0].x = (float)S.vp[0]; x->vp[0].y = (float)S.vp[1];
    x->vp[0].w = (float)S.vp[2]; x->vp[0].h = (float)S.vp[3];
    x->vp[0].minz = f32of(S.vp[4]); x->vp[0].maxz = f32of(S.vp[5]);
    x->nvp = 1;
    memcpy(x->scissor[0], S.scissor, sizeof S.scissor);
    x->nscissor = 1;
}

static int draw_setup(uint32_t* instances)
{
    GpuCtx* x = &S.ctx;
    if (!S.vs || !S.ps) {
        static int said;
        if (said++ < 4) host_log("[d3d9] fixed-function draw skipped (vertex shader %s, pixel shader %s)", S.vs ? "set" : "none", S.ps ? "set" : "none");
        return 0;
    }
    if (!S.decl) return 0;
    if (!bind_targets(x)) return 0;
    bind_viewport(x);
    x->blend = blend_state();
    x->depth = depth_state();
    x->raster = raster_state();
    uint32_t bf = S.rs[193];
    x->blend_factor[0] = ((bf >> 16) & 0xFF) / 255.0f; x->blend_factor[1] = ((bf >> 8) & 0xFF) / 255.0f;
    x->blend_factor[2] = (bf & 0xFF) / 255.0f; x->blend_factor[3] = (bf >> 24) / 255.0f;
    x->sample_mask = 0xFFFFFFFF;
    x->stencil_ref = S.rs[57] & 0xFF;
    x->topology = 4;
    /* textures and samplers: pixel 0-15, vertex 0-3 */
    memset(x->srv, 0, sizeof x->srv);
    memset(x->samp, 0, sizeof x->samp);
    for (int i = 0; i < 16; i++) {
        D9* t = D(S.tex[i]);
        if (!t) continue;
        x->srv[SH_PS][i] = t->kind == O_SURF ? surf_srv(t) : tex_srv(t, S.ss[i][11] != 0);
        x->samp[SH_PS][i] = sampler_state(i);
    }
    for (int i = 0; i < 4; i++) {
        D9* t = D(S.tex[17 + i]);
        if (!t) continue;
        x->srv[SH_VS][i] = tex_srv(t, 0);
        x->samp[SH_VS][i] = sampler_state(17 + i);
    }
    /* instancing: stream 0's frequency (INDEXEDDATA) is the instance count; INSTANCEDATA streams step per instance */
    uint32_t inst = 0;
    *instances = 1;
    if (S.vb_freq[0] & 0x40000000u) *instances = S.vb_freq[0] & 0x3FFFFFFF;
    if (!*instances) *instances = 1;
    for (int i = 1; i < 16; i++) if (S.vb_freq[i] & 0x80000000u) inst |= 1u << i;
    if (*instances <= 1) inst = 0;
    uint32_t um, sm;
    x->layout = decl_layout(S.decl, S.vs, inst, &um, &sm);
    x->sh[SH_VS] = vs_variant(S.vs, um, sm);
    x->sh[SH_PS] = S.ps->base;
    x->cb[SH_VS][0] = S.vcb_r;
    x->cb[SH_PS][0] = S.pcb_r;
    for (int i = 0; i < 16; i++) {
        D9* b = D(S.vb[i]);
        x->vb[i] = b ? b->r : NULL;
        x->vb_stride[i] = S.vb_stride[i];
        x->vb_offset[i] = S.vb_off[i];
    }
    D9* ib = D(S.ib);
    x->ib = ib ? ib->r : NULL;
    x->ib_format = ib && ib->fmt == 102 ? 42 : 57;
    x->ib_offset = 0;
    upload_constants();
    return 1;
}

/* buffers written since their last upload go up before a draw reads them */
static void flush_buffer(D9* b)
{
    if (!b || !b->r || !b->lock || b->lock_hi <= b->lock_lo) return;
    int32_t box[6] = { (int32_t)b->lock_lo, 0, 0, (int32_t)b->lock_hi, 1, 1 };
    if (gpu->res_upload) gpu->res_upload(b->r, 0, box, b->lock + b->lock_lo, b->lock_hi - b->lock_lo, 0);
    b->lock_lo = b->lock_hi = 0;
}

static uint32_t ring_put(GpuRes* r, uint32_t* at, uint64_t src, uint32_t n, uint32_t align)
{
    uint32_t off = (*at + align - 1) / align * align;
    if (off + n > r->width) off = 0;
    if (n > r->width) return 0xFFFFFFFF;
    memcpy(G2H(res_backing(r) + off), G2H(src), n);
    int32_t box[6] = { (int32_t)off, 0, 0, (int32_t)(off + n), 1, 1 };
    if (gpu->res_upload) gpu->res_upload(r, 0, box, r->backing + off, n, 0);
    *at = off + n;
    return off;
}

/* a triangle fan as an indexed triangle list (WebGPU has no fans) */
static uint32_t fan_indices(uint32_t first, uint32_t prims, uint64_t src_idx, int idx32)
{
    uint32_t n = prims * 3;
    uint64_t tmp = g_alloc((uint64_t)n * 4 + 16);
    if (!tmp) return 0xFFFFFFFF;
    uint32_t* d = (uint32_t*)G2H(tmp);
    for (uint32_t i = 0; i < prims; i++) {
        uint32_t a = 0, b = i + 1, c3 = i + 2;
        if (src_idx) {
            a = idx32 ? LD32(src_idx) : (uint32_t)(*(uint16_t*)G2H(src_idx));
            b = idx32 ? LD32(src_idx + 4ull * (i + 1)) : (uint32_t)(*(uint16_t*)G2H(src_idx + 2ull * (i + 1)));
            c3 = idx32 ? LD32(src_idx + 4ull * (i + 2)) : (uint32_t)(*(uint16_t*)G2H(src_idx + 2ull * (i + 2)));
        } else { a += first; b += first; c3 += first; }
        d[3 * i] = a; d[3 * i + 1] = b; d[3 * i + 2] = c3;
    }
    uint32_t off = ring_put(S.up_ib, &S.up_ib_at, tmp, n * 4, 4);
    g_free(tmp);
    return off;
}

/* ?d3d9trace=<frame>: every draw of that frame, with what it reads (texture formats and sizes, sampler addressing),
   its blend states and the first vertex and pixel shader constants, for rendering bugs a frame capture can't explain.
   Module._uew_d3d9_trace() from the page traces the next frame instead */
static int trace_want = -1;
#if defined(__EMSCRIPTEN__) && defined(UEW_DEBUG_TOOLS)
__attribute__((used, visibility("default")))
#endif
void uew_d3d9_trace(void) { trace_want = (int)S.frames + 1; }

static void trace_draw(uint32_t type, uint32_t count, uint32_t first, int32_t base, int indexed)
{
    if (trace_want < 0) trace_want = host_query_int("d3d9trace");
    if (trace_want <= 0 || S.frames != (uint32_t)trace_want) return;
    static int n;
    char tx[400] = "";
    int k = 0;
    for (int i = 0; i < 4 && k < 200; i++) {
        D9* t = D(S.tex[i]);
        if (t) k += snprintf(tx + k, sizeof tx - k, " t%d=fmt%u %ux%u lv%u addr%u/%u filt%u/%u/%u mem %llx", i, t->fmt, t->w, t->h, t->levels,
                             S.ss[i][1], S.ss[i][2], S.ss[i][5], S.ss[i][6], S.ss[i][7], (unsigned long long)t->lock);
    }
    const float* pc = (const float*)G2H(S.pcb);
    const float* vc = (const float*)G2H(S.vcb);
    host_log("[d3d9t] #%d type %u n %u first %d base %d%s vs %p ps %p blend %u src %u dst %u sep %u %u/%u rt %llx%s", n++, type, count, (int)first,
             base, indexed ? " indexed" : "", (void*)S.vs, (void*)S.ps, S.rs[27], S.rs[19], S.rs[20], S.rs[206], S.rs[207], S.rs[208],
             (unsigned long long)S.rt[0], tx);
    host_log("[d3d9t]   pc0 %g %g %g %g | pc6 %g %g %g %g | pc7 %g %g %g %g | vc10 %g %g %g %g | vc11 %g %g %g %g",
             pc[0], pc[1], pc[2], pc[3], pc[24], pc[25], pc[26], pc[27], pc[28], pc[29], pc[30], pc[31],
             vc[40], vc[41], vc[42], vc[43], vc[44], vc[45], vc[46], vc[47]);
    D9* dsd = D(S.ds);
    host_log("[d3d9t]   ds %llx fmt %u %ux%u, stencil %u func %u ref %u, colorwrite %x", (unsigned long long)S.ds, dsd ? dsd->fmt : 0,
             dsd ? dsd->w : 0, dsd ? dsd->h : 0, S.rs[52], S.rs[56], S.rs[57], S.rs[168]);
    host_log("[d3d9t]   vc0 %g %g %g %g | vc1 %g %g %g %g | vc2 %g %g %g %g | vc3 %g %g %g %g", vc[0], vc[1], vc[2], vc[3], vc[4],
             vc[5], vc[6], vc[7], vc[8], vc[9], vc[10], vc[11], vc[12], vc[13], vc[14], vc[15]);
    /* each shader once, as translated */
    static const void* shown[2048];
    static int nshown;
    const Sh* shs[2] = { S.vs, S.ps };
    for (int i = 0; i < 2; i++) {
        int seen = !shs[i];
        for (int j = 0; j < nshown && !seen; j++) seen = shown[j] == shs[i];
        if (seen || nshown == 2048) continue;
        shown[nshown++] = shs[i];
        DxWgsl w;
        if (d3d9_to_wgsl(shs[i]->code, shs[i]->size, NULL, &w)) { host_log("[d3d9t]   shader %p:\n%s", (const void*)shs[i], w.wgsl); dxbc_wgsl_free(&w); }
    }
    host_log("[d3d9t]   vc6 %g %g %g %g | vc7 %g %g %g %g | vc9 %g %g %g %g | vc13 %g %g %g %g", vc[24], vc[25], vc[26], vc[27],
             vc[28], vc[29], vc[30], vc[31], vc[36], vc[37], vc[38], vc[39], vc[52], vc[53], vc[54], vc[55]);
}

static void do_draw(uint32_t type, uint32_t count, uint32_t first, int32_t base, int indexed, uint32_t instances)
{
    trace_draw(type, count, first, base, indexed);
    S.ctx.topology = type >= 1 && type <= 5 ? type : 4;
    if (gpu->draw && count) gpu->draw(&S.ctx, count, first, base, instances, 0, indexed);
}

static void dev_draw(CPU* c)
{
    uint32_t type = (uint32_t)A2, start = (uint32_t)A3, prims = (uint32_t)A4, inst;
    if (!prims || !draw_setup(&inst)) { RET(D3D_OK); return; }
    for (int i = 0; i < 16; i++) flush_buffer(D(S.vb[i]));
    if (type == 6) {
        uint32_t off = fan_indices(start, prims, 0, 0);
        if (off != 0xFFFFFFFF) { S.ctx.ib = S.up_ib; S.ctx.ib_format = 42; S.ctx.ib_offset = off; do_draw(4, prims * 3, 0, 0, 1, inst); }
    } else do_draw(type, prim_count(type, prims), start, 0, 0, inst);
    RET(D3D_OK);
}

static void dev_draw_indexed(CPU* c)
{
    uint32_t type = (uint32_t)A2;
    int32_t base = (int32_t)A3;
    uint32_t start = (uint32_t)ARG(6), prims = (uint32_t)ARG(7), inst;
    if (!prims || !draw_setup(&inst)) { RET(D3D_OK); return; }
    for (int i = 0; i < 16; i++) flush_buffer(D(S.vb[i]));
    D9* ib = D(S.ib);
    flush_buffer(ib);
    if (!ib) { RET(D3D_OK); return; }
    if (type == 6) {
        int i32 = ib->fmt == 102;
        uint32_t off = fan_indices(0, prims, lock_mem(ib) + (uint64_t)start * (i32 ? 4 : 2), i32);
        if (off != 0xFFFFFFFF) { S.ctx.ib = S.up_ib; S.ctx.ib_format = 42; S.ctx.ib_offset = off; do_draw(4, prims * 3, 0, base, 1, inst); }
    } else do_draw(type, prim_count(type, prims), start, base, 1, inst);
    RET(D3D_OK);
}

static void dev_draw_up(CPU* c)
{
    uint32_t type = (uint32_t)A2, prims = (uint32_t)A3, stride = (uint32_t)ARG(5), inst;
    uint64_t data = A4;
    uint32_t nv = type == 6 ? prims + 2 : prim_count(type, prims);
    if (prims && data && stride && draw_setup(&inst)) {
        uint32_t off = ring_put(S.up_vb, &S.up_vb_at, data, nv * stride, stride);
        if (off != 0xFFFFFFFF) {
            S.ctx.vb[0] = S.up_vb; S.ctx.vb_stride[0] = stride; S.ctx.vb_offset[0] = off;
            if (type == 6) {
                uint32_t io = fan_indices(0, prims, 0, 0);
                if (io != 0xFFFFFFFF) { S.ctx.ib = S.up_ib; S.ctx.ib_format = 42; S.ctx.ib_offset = io; do_draw(4, prims * 3, 0, 0, 1, inst); }
            } else do_draw(type, nv, 0, 0, 0, inst);
        }
    }
    /* D3D9 clears stream 0 and the indices after a UP draw */
    S.vb[0] = 0; S.vb_off[0] = S.vb_stride[0] = 0;
    S.ib = 0;
    RET(D3D_OK);
}

static void dev_draw_indexed_up(CPU* c)
{
    uint32_t type = (uint32_t)A2, minv = (uint32_t)A3, nverts = (uint32_t)A4, prims = (uint32_t)ARG(5);
    uint64_t idx = ARG(6), vdata = ARG(8);
    uint32_t ifmt = (uint32_t)ARG(7), stride = (uint32_t)ARG(9), inst;
    if (prims && idx && vdata && stride && draw_setup(&inst)) {
        int i32 = ifmt == 102;
        uint32_t nv = minv + nverts;
        uint32_t voff = ring_put(S.up_vb, &S.up_vb_at, vdata, nv * stride, stride);
        if (voff != 0xFFFFFFFF) {
            S.ctx.vb[0] = S.up_vb; S.ctx.vb_stride[0] = stride; S.ctx.vb_offset[0] = voff;
            if (type == 6) {
                uint32_t io = fan_indices(0, prims, idx, i32);
                if (io != 0xFFFFFFFF) { S.ctx.ib = S.up_ib; S.ctx.ib_format = 42; S.ctx.ib_offset = io; do_draw(4, prims * 3, 0, 0, 1, inst); }
            } else {
                uint32_t ni = prim_count(type, prims);
                uint32_t io = ring_put(S.up_ib, &S.up_ib_at, idx, ni * (i32 ? 4 : 2), 4);
                if (io != 0xFFFFFFFF) { S.ctx.ib = S.up_ib; S.ctx.ib_format = i32 ? 42 : 57; S.ctx.ib_offset = io; do_draw(type, ni, 0, 0, 1, inst); }
            }
        }
    }
    S.vb[0] = 0; S.vb_off[0] = S.vb_stride[0] = 0;
    S.ib = 0;
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- built-in draws: clears of part of a target, scaled copies */

/* hand-assembled vs_3_0 / ps_3_0:
   vs: dcl_position v0; dcl_texcoord v1; dcl_position o0; dcl_texcoord o1; mov o0, v0; mov o1, v1
   ps (colour): mov oC0, c0          ps (texture): dcl_texcoord v0.xy; dcl_2d s0; texld r0, v0, s0; mov oC0, r0 */
static const uint32_t VS_BLIT[] = { 0xFFFE0300, 0x0200001F, 0x80000000, 0x900F0000, 0x0200001F, 0x80000005, 0x900F0001,
                                    0x0200001F, 0x80000000, 0xE00F0000, 0x0200001F, 0x80000005, 0xE00F0001,
                                    0x02000001, 0xE00F0000, 0x90E40000, 0x02000001, 0xE00F0001, 0x90E40001, 0x0000FFFF };
static const uint32_t PS_FILL[] = { 0xFFFF0300, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF };
static const uint32_t PS_TEX[] = { 0xFFFF0300, 0x0200001F, 0x80000005, 0x90030000, 0x0200001F, 0x90000000, 0xA00F0800,
                                   0x03000042, 0x800F0000, 0x90E40000, 0xA0E40800, 0x02000001, 0x800F0800, 0x80E40000, 0x0000FFFF };

static Sh* make_sh(const uint32_t* code, uint32_t size, int ps)
{
    Sh* s = calloc(1, sizeof *s);
    s->ps = ps;
    s->size = size;
    s->code = malloc(size);
    memcpy(s->code, code, size);
    s->base = calloc(1, sizeof *s->base);
    s->base->stage = ps ? SH_PS : SH_VS;
    s->base->code = s->code;
    s->base->size = size;
    if (!ps) d3d9_vs_inputs(s->code, size, s->usage, s->index);
    if (gpu->shader_create) gpu->shader_create(s->base);
    return s;
}
static Sh* builtin_vs(int tex) { (void)tex; static Sh* s; if (!s) s = make_sh(VS_BLIT, sizeof VS_BLIT, 0); return s; }
static Sh* builtin_ps(int tex)
{
    static Sh *f, *t;
    if (tex) { if (!t) t = make_sh(PS_TEX, sizeof PS_TEX, 1); return t; }
    if (!f) f = make_sh(PS_FILL, sizeof PS_FILL, 1);
    return f;
}

static struct { GpuRes *vb, *vcb, *pcb; uint64_t vcb_m, pcb_m; GpuLayout* layout; } B;

static void builtin_init(void)
{
    if (B.vb) return;
    B.vb = new_buf(64 * 1024, 0x1);
    B.vcb = new_buf(D3D9_CB_VEC4S * 16, 0x4);
    B.pcb = new_buf(D3D9_CB_VEC4S * 16, 0x4);
    B.vcb_m = B.vcb->backing;
    B.pcb_m = B.pcb->backing;
    GpuLayout* l = calloc(1, sizeof *l);
    l->count = 2;
    strcpy(l->el[0].semantic, "POSITION"); l->el[0].format = 2; l->el[0].offset = 0;
    strcpy(l->el[1].semantic, "TEXCOORD"); l->el[1].format = 2; l->el[1].offset = 16;
    if (gpu->layout_create) gpu->layout_create(l);
    B.layout = l;
}
static uint32_t B_at;

/* one quad: clip-space rect (x0,y0)-(x1,y1) at depth z, texture coordinates (u0,v0)-(u1,v1) */
static void builtin_quad(GpuCtx* x, float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1, float v1)
{
    float q[6][8] = {
        { x0, y0, z, 1, u0, v0, 0, 0 }, { x1, y0, z, 1, u1, v0, 0, 0 }, { x0, y1, z, 1, u0, v1, 0, 0 },
        { x1, y0, z, 1, u1, v0, 0, 0 }, { x1, y1, z, 1, u1, v1, 0, 0 }, { x0, y1, z, 1, u0, v1, 0, 0 },
    };
    uint64_t tmp = g_alloc(sizeof q);
    if (!tmp) return;
    memcpy(G2H(tmp), q, sizeof q);
    uint32_t off = ring_put(B.vb, &B_at, tmp, sizeof q, 32);
    g_free(tmp);
    if (off == 0xFFFFFFFF) return;
    x->vb[0] = B.vb; x->vb_stride[0] = 32; x->vb_offset[0] = off;
    x->topology = 4;
    if (gpu->draw) gpu->draw(x, 6, 0, 0, 1, 0, 0);
}

/* fills rectangles of the current targets: colour (masks per flags), depth and stencil */
static void builtin_clear(uint32_t flags, const float rgba[4], float z, uint32_t stencil, const int32_t* rects, uint32_t nrect, GpuView* only_rtv,
                          uint32_t tw, uint32_t th)
{
    builtin_init();
    GpuCtx x;
    memset(&x, 0, sizeof x);
    if (only_rtv) x.rtv[0] = only_rtv;
    else {
        bind_targets(&x);
        if (!(flags & 1)) memset(x.rtv, 0, sizeof x.rtv);
        if (!(flags & 6)) x.dsv = NULL;
    }
    if (!x.rtv[0] && !x.dsv) return;
    float* pc = (float*)G2H(B.pcb_m);
    memcpy(pc, rgba, 16);
    pc[4 * D3D9_CB_EXTRA] = 8.0f;
    memset(G2H(B.vcb_m), 0, D3D9_CB_VEC4S * 16);
    gpu->res_upload(B.vcb, 0, NULL, B.vcb_m, D3D9_CB_VEC4S * 16, 0);
    gpu->res_upload(B.pcb, 0, NULL, B.pcb_m, D3D9_CB_VEC4S * 16, 0);
    x.sh[SH_VS] = builtin_vs(0)->base;
    x.sh[SH_PS] = builtin_ps(0)->base;
    x.cb[SH_VS][0] = B.vcb; x.cb[SH_PS][0] = B.pcb;
    x.layout = B.layout;
    uint32_t bd[66] = { 0 };
    bd[1] = 1;
    for (int i = 0; i < 8; i++) { uint32_t* r = bd + 2 + 8 * i; r[1] = 2; r[2] = 1; r[3] = 1; r[4] = 2; r[5] = 1; r[6] = 1; r[7] = (flags & 1) ? 0xF : 0; }
    x.blend = state_of(1, bd, 264);
    uint32_t dd[13] = { (flags & 2) ? 1u : 0u, (flags & 2) ? 1u : 0u, 8, (flags & 4) ? 1u : 0u, 0xFF00, 1, 1, 3, 8, 1, 1, 3, 8 };
    if (flags & 4) dd[4] = 0xFF | 0xFF00;
    x.depth = state_of(2, dd, 52);
    uint32_t rd[10] = { 3, 1, 0, 0, 0, 0, 1, 1, 0, 0 };
    x.raster = state_of(3, rd, 40);
    x.stencil_ref = stencil & 0xFF;
    x.sample_mask = 0xFFFFFFFF;
    x.vp[0].x = 0; x.vp[0].y = 0; x.vp[0].w = (float)tw; x.vp[0].h = (float)th; x.vp[0].minz = 0; x.vp[0].maxz = 1;
    x.nvp = 1;
    x.nscissor = 1;
    for (uint32_t i = 0; i < nrect; i++) {
        const int32_t* r = rects + 4 * i;
        int32_t sx0 = r[0] < 0 ? 0 : r[0], sy0 = r[1] < 0 ? 0 : r[1];
        int32_t sx1 = r[2] > (int32_t)tw ? (int32_t)tw : r[2], sy1 = r[3] > (int32_t)th ? (int32_t)th : r[3];
        if (sx0 >= sx1 || sy0 >= sy1) continue;
        x.scissor[0][0] = sx0; x.scissor[0][1] = sy0; x.scissor[0][2] = sx1; x.scissor[0][3] = sy1;
        builtin_quad(&x, -1, -1, 1, 1, z, 0, 0, 0, 0);
    }
}

/* scaled or converting copy between surfaces, drawn as a textured quad */
static void builtin_stretch(D9* src, const int32_t* sr, D9* dst, const int32_t* dr, int linear)
{
    builtin_init();
    GpuCtx x;
    memset(&x, 0, sizeof x);
    x.rtv[0] = surf_rtv(dst, 0);
    GpuView* sv = surf_srv(src);
    if (!x.rtv[0] || !sv) return;
    float* pc = (float*)G2H(B.pcb_m);
    memset(pc, 0, 16);
    pc[4 * D3D9_CB_EXTRA] = 8.0f;
    memset(G2H(B.vcb_m), 0, D3D9_CB_VEC4S * 16);
    gpu->res_upload(B.vcb, 0, NULL, B.vcb_m, D3D9_CB_VEC4S * 16, 0);
    gpu->res_upload(B.pcb, 0, NULL, B.pcb_m, D3D9_CB_VEC4S * 16, 0);
    x.sh[SH_VS] = builtin_vs(1)->base;
    x.sh[SH_PS] = builtin_ps(1)->base;
    x.cb[SH_VS][0] = B.vcb; x.cb[SH_PS][0] = B.pcb;
    x.layout = B.layout;
    x.srv[SH_PS][0] = sv;
    uint32_t sd[13] = { linear ? 0x15u : 0u, 3, 3, 3, 0, 1, 8, 0, 0, 0, 0, 0, 0 };
    float big = 1000.0f;
    memcpy(&sd[12], &big, 4);
    x.samp[SH_PS][0] = state_of(4, sd, 52);
    uint32_t bd[66] = { 0 };
    bd[1] = 1;
    for (int i = 0; i < 8; i++) { uint32_t* r = bd + 2 + 8 * i; r[1] = 2; r[2] = 1; r[3] = 1; r[4] = 2; r[5] = 1; r[6] = 1; r[7] = 0xF; }
    x.blend = state_of(1, bd, 264);
    uint32_t dd[13] = { 0, 0, 8, 0, 0xFFFF, 1, 1, 1, 8, 1, 1, 1, 8 };
    x.depth = state_of(2, dd, 52);
    uint32_t rd[10] = { 3, 1, 0, 0, 0, 0, 1, 1, 0, 0 };
    x.raster = state_of(3, rd, 40);
    x.sample_mask = 0xFFFFFFFF;
    x.vp[0].x = 0; x.vp[0].y = 0; x.vp[0].w = (float)dst->w; x.vp[0].h = (float)dst->h; x.vp[0].minz = 0; x.vp[0].maxz = 1;
    x.nvp = 1;
    x.scissor[0][0] = dr[0]; x.scissor[0][1] = dr[1]; x.scissor[0][2] = dr[2]; x.scissor[0][3] = dr[3];
    x.nscissor = 1;
    float X0 = 2.0f * dr[0] / dst->w - 1, X1 = 2.0f * dr[2] / dst->w - 1;
    float Y0 = 1 - 2.0f * dr[1] / dst->h, Y1 = 1 - 2.0f * dr[3] / dst->h;
    float u0 = (float)sr[0] / src->w, u1 = (float)sr[2] / src->w, v0 = (float)sr[1] / src->h, v1 = (float)sr[3] / src->h;
    builtin_quad(&x, X0, Y0, X1, Y1, 0, u0, v0, u1, v1);
}

/* ---------------------------------------------------------------- IDirect3DDevice9 */

static void dev_qi(CPU* c) { ST32(THIS + 8, LD32(THIS + 8) + 1); STP(A3, THIS); RET(0); }
static void dev_test_coop(CPU* c) { RET(D3D_OK); }
static void dev_tex_mem(CPU* c) { RET(0x7FF00000u); }
static void dev_get_d3d(CPU* c) { STP(A2, S.d3d); ST32(S.d3d + 8, LD32(S.d3d + 8) + 1); RET(D3D_OK); }
static void dev_get_caps(CPU* c) { if (A2) fill_caps(A2); RET(D3D_OK); }
static void dev_display_mode(CPU* c) { if (A3) display_mode(A3); RET(D3D_OK); }
static void dev_creation_params(CPU* c)
{
    uint64_t p = A2;
    if (!p) { RET(D3DERR_INVALIDCALL); return; }
    ST32(p, 0); ST32(p + 4, 1);
    STP(p + 8, 0);
    ST32(p + 8 + GP, 0x40);
    RET(D3D_OK);
}
static void dev_show_cursor(CPU* c) { RET(0); }
static void dev_get_swapchain(CPU* c) { STP(A3, S.sc); ST32(S.sc + 8, LD32(S.sc + 8) + 1); RET(D3D_OK); }
static void dev_num_swapchains(CPU* c) { RET(1); }
static void dev_reset(CPU* c)
{
    if (A2) present_params(A2);
    make_backbuffer();
    default_state();
    for (int i = 0; i < 21; i++) S.tex[i] = 0;
    for (int i = 0; i < 16; i++) S.vb[i] = 0;
    S.ib = 0;
    RET(D3D_OK);
}

static void dev_present(CPU* c)
{
    D9* bb = D(S.backbuf);
    if (gpu->present) gpu->present(bb ? bb->r : NULL, S.bb_w, S.bb_h);
    reap();
    S.frames++;
    sched_frame_report();
    sched_wait_frame(c);
    RET(D3D_OK);
}
static void dev_get_backbuffer(CPU* c) { STP(ARG(5), addref(S.backbuf)); RET(S.backbuf ? D3D_OK : D3DERR_INVALIDCALL); }
static void dev_raster_status(CPU* c) { if (A3) { ST32(A3, 0); ST32(A3 + 4, 0); } RET(D3D_OK); }

static void out_obj(uint64_t pp, uint64_t o) { if (pp) STP(pp, o); }

static void dev_create_texture(CPU* c)
{
    D9* t = make_res(O_TEX, (uint32_t)A2, (uint32_t)A3, 1, (uint32_t)A4, (uint32_t)ARG(5), (uint32_t)ARG(6), (uint32_t)ARG(7));
    if (!t) {
        static int said;
        if (said++ < 16) host_log("[d3d9] CreateTexture: format %u (0x%x) not supported", (unsigned)ARG(6), (unsigned)ARG(6));
        out_obj(ARG(8), 0);
        RET(D3DERR_INVALIDCALL);
        return;
    }
    t->obj = com_new(&K_Tex, t, 0);
    out_obj(ARG(8), t->obj);
    RET(D3D_OK);
}
static void dev_create_volume(CPU* c)
{
    D9* t = make_res(O_VOL, (uint32_t)A2, (uint32_t)A3, (uint32_t)A4, (uint32_t)ARG(5), (uint32_t)ARG(6), (uint32_t)ARG(7), (uint32_t)ARG(8));
    if (!t) { out_obj(ARG(9), 0); RET(D3DERR_INVALIDCALL); return; }
    t->obj = com_new(&K_Vol, t, 0);
    out_obj(ARG(9), t->obj);
    RET(D3D_OK);
}
static void dev_create_cube(CPU* c)
{
    D9* t = make_res(O_CUBE, (uint32_t)A2, (uint32_t)A2, 1, (uint32_t)A3, (uint32_t)A4, (uint32_t)ARG(5), (uint32_t)ARG(6));
    if (!t) { out_obj(ARG(7), 0); RET(D3DERR_INVALIDCALL); return; }
    t->obj = com_new(&K_Cube, t, 0);
    out_obj(ARG(7), t->obj);
    RET(D3D_OK);
}
static void make_buffer(CPU* c, int kind)
{
    D9* b = calloc(1, sizeof *b);
    b->kind = kind;
    b->w = (uint32_t)A2;
    b->usage = (uint32_t)A3;
    if (kind == O_VB) { b->fvf = (uint32_t)A4; b->fmt = 100; }
    else b->fmt = (uint32_t)A4 == 102 ? 102 : 101;
    b->pool = (uint32_t)ARG(5);
    b->h = b->d = b->levels = b->faces = 1;
    if (b->pool != POOL_SYSTEMMEM && b->pool != POOL_SCRATCH) b->r = new_buf((b->w + 3) & ~3u, kind == O_VB ? 0x1 : 0x2);
    lock_mem(b);
    b->obj = com_new(kind == O_VB ? &K_VB : &K_IB, b, 0);
    out_obj(ARG(6), b->obj);
    RET(D3D_OK);
}
static void dev_create_vb(CPU* c) { make_buffer(c, O_VB); }
static void dev_create_ib(CPU* c) { make_buffer(c, O_IB); }
static void dev_create_rt(CPU* c)
{
    uint64_t o = new_surface((uint32_t)A2, (uint32_t)A3, (uint32_t)A4, 1, POOL_DEFAULT);
    out_obj(ARG(8), o);
    RET(o ? D3D_OK : D3DERR_INVALIDCALL);
}
static void dev_create_ds(CPU* c)
{
    uint64_t o = new_surface((uint32_t)A2, (uint32_t)A3, (uint32_t)A4, 2, POOL_DEFAULT);
    out_obj(ARG(8), o);
    RET(o ? D3D_OK : D3DERR_INVALIDCALL);
}

static void dev_create_offscreen(CPU* c)
{
    uint64_t o = new_surface((uint32_t)A2, (uint32_t)A3, (uint32_t)A4, 0, (uint32_t)ARG(5));
    out_obj(ARG(6), o);
    RET(o ? D3D_OK : D3DERR_INVALIDCALL);
}

/* ---------------------------------------------------------------- locking and copies */

static D9* owner(D9* s, uint32_t* face, uint32_t* level)
{
    if (s->parent) { *face = s->face; *level = s->level; return s->parent; }
    *face = 0; *level = 0;
    return s;
}
static uint64_t sub_mem(D9* t, uint32_t face, uint32_t level)
{
    uint64_t m = lock_mem(t);
    return m ? m + t->loff[face * t->levels + level] : 0;
}

/* GPU contents of a level into guest memory in D3D9 layout (render targets read back by the game) */
static void read_sub(D9* t, uint32_t face, uint32_t level, uint64_t dst, uint32_t dpitch)
{
    if (!t->r || !gpu->readback || t->f->conv || !dst) return;
    uint32_t sub = face * t->levels + level;
    if (!res_backing(t->r)) return;
    gpu->readback(t->r, sub);
    uint32_t rows = d9_rows(t, level), sp = t->r->sub_pitch[sub], n = d9_pitch(t, level);
    for (uint32_t y = 0; y < rows; y++)
        memcpy(G2H(dst + (uint64_t)y * dpitch), G2H(t->r->backing + t->r->sub_offset[sub] + (uint64_t)y * sp), n < sp ? n : sp);
}

static void lock_rect(CPU* c, D9* t, uint32_t face, uint32_t level, uint64_t out, uint64_t rect, uint32_t flags)
{
    if (!t || !out || level >= t->levels || face >= t->faces) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t p = sub_mem(t, face, level);
    if (!p) { RET(0x8007000Eu); return; }
    uint32_t pitch = d9_pitch(t, level);
    if ((t->usage & 1) && t->r && !(flags & 0x2000)) read_sub(t, face, level, p, pitch);
    if (rect) {
        uint32_t b = t->f->block;
        p += (uint64_t)(LD32(rect + 4) / b) * pitch + (uint64_t)(LD32(rect) / b) * t->f->bpb;
    }
    ST32(out, pitch);
    STP(out + GP, p);
    t->lock_ro = (flags & 0x10) != 0;
    RET(D3D_OK);
}
static void unlock_rect(CPU* c, D9* t, uint32_t face, uint32_t level)
{
    if (t && t->lock && face < t->faces && level < t->levels && !t->lock_ro) upload(t, face, level, NULL, sub_mem(t, face, level), d9_pitch(t, level));
    if (t) t->lock_ro = 0;
    RET(D3D_OK);
}

static void dev_update_surface(CPU* c)
{
    D9 *s = D(A2), *d = D(A4);
    if (!s || !d) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t sf, sl, df, dl;
    D9 *so = owner(s, &sf, &sl), *dw = owner(d, &df, &dl);
    int32_t r[4] = { 0, 0, (int32_t)s->w, (int32_t)s->h };
    if (A3) for (int i = 0; i < 4; i++) r[i] = (int32_t)LD32(A3 + 4ull * i);
    uint32_t dx = ARG(5) ? (uint32_t)LD32(ARG(5)) : 0, dy = ARG(5) ? (uint32_t)LD32(ARG(5) + 4) : 0;
    uint32_t pitch = d9_pitch(so, sl), b = so->f->block;
    uint64_t src = sub_mem(so, sf, sl) + (uint64_t)(r[1] / (int32_t)b) * pitch + (uint64_t)(r[0] / (int32_t)b) * so->f->bpb;
    uint32_t dr[4] = { dx, dy, dx + (uint32_t)(r[2] - r[0]), dy + (uint32_t)(r[3] - r[1]) };
    upload(dw, df, dl, dr, src, pitch);
    RET(D3D_OK);
}
static void dev_update_texture(CPU* c)
{
    D9 *s = D(A2), *d = D(A3);
    if (!s || !d || s->kind != d->kind) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t off = 0;
    while (off + 1 < s->levels && (s->w >> off) > d->w) off++;
    for (uint32_t f = 0; f < d->faces; f++)
        for (uint32_t l = 0; l < d->levels && l + off < s->levels; l++)
            upload(d, f, l, NULL, sub_mem(s, f, l + off), d9_pitch(s, l + off));
    RET(D3D_OK);
}
static void dev_get_rt_data(CPU* c)
{
    D9 *s = D(A2), *d = D(A3);
    if (!s || !d) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t sf, sl, df, dl;
    D9 *so = owner(s, &sf, &sl), *dw = owner(d, &df, &dl);
    read_sub(so, sf, sl, sub_mem(dw, df, dl), d9_pitch(dw, dl));
    RET(D3D_OK);
}

static void rect_or_full(uint64_t p, D9* s, int32_t* r)
{
    r[0] = 0; r[1] = 0; r[2] = (int32_t)s->w; r[3] = (int32_t)s->h;
    if (p) for (int i = 0; i < 4; i++) r[i] = (int32_t)LD32(p + 4ull * i);
}
static int same_storage(uint32_t a, uint32_t b) { return a == b || ((a == 87 || a == 88) && (b == 87 || b == 88)); }

static void dev_stretch(CPU* c)
{
    D9 *s = D(A2), *d = D(A4);
    if (!s || !d) { RET(D3DERR_INVALIDCALL); return; }
    int32_t sr[4], dr[4];
    rect_or_full(A3, s, sr);
    rect_or_full(ARG(5), d, dr);
    uint32_t ss, ds;
    GpuRes *a = gres(s, &ss), *b = gres(d, &ds);
    if (!a || !b) { RET(D3D_OK); return; }
    if (sr[2] - sr[0] == dr[2] - dr[0] && sr[3] - sr[1] == dr[3] - dr[1] && same_storage(s->f->dxgi, d->f->dxgi)) {
        int32_t box[6] = { sr[0], sr[1], 0, sr[2], sr[3], 1 };
        if (gpu->copy) gpu->copy(b, ds, (uint32_t)dr[0], (uint32_t)dr[1], 0, a, ss, box);
    } else if (s->f->flags & FF_DS) {
        static int said;
        if (said++ < 4) host_log("[d3d9] StretchRect between depth surfaces of different sizes skipped");
    } else builtin_stretch(s, sr, d, dr, (uint32_t)ARG(6) == 2);
    RET(D3D_OK);
}

static void argb(uint32_t c, float* f) { f[0] = ((c >> 16) & 0xFF) / 255.0f; f[1] = ((c >> 8) & 0xFF) / 255.0f; f[2] = (c & 0xFF) / 255.0f; f[3] = (c >> 24) / 255.0f; }

static void dev_color_fill(CPU* c)
{
    D9* s = D(A2);
    if (!s) { RET(D3DERR_INVALIDCALL); return; }
    float col[4];
    argb((uint32_t)A4, col);
    GpuView* v = surf_rtv(s, 0);
    if (!v) { RET(D3D_OK); return; }
    if (!A3) { if (gpu->clear_rtv) gpu->clear_rtv(v, col); }
    else {
        int32_t r[4];
        rect_or_full(A3, s, r);
        builtin_clear(1, col, 0, 0, r, 1, v, s->w, s->h);
    }
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- targets, clears, state */

static void dev_set_rt(CPU* c)
{
    uint32_t i = (uint32_t)A2;
    if (i >= 4 || (i == 0 && !A3)) { RET(D3DERR_INVALIDCALL); return; }
    S.rt[i] = A3;
    D9* s = D(A3);
    if (i == 0 && s) {
        /* D3D9 resets the viewport and scissor rectangle to the new target */
        S.vp[0] = S.vp[1] = 0; S.vp[2] = s->w; S.vp[3] = s->h;
        float z0 = 0, z1 = 1;
        memcpy(&S.vp[4], &z0, 4); memcpy(&S.vp[5], &z1, 4);
        S.scissor[0] = S.scissor[1] = 0; S.scissor[2] = (int32_t)s->w; S.scissor[3] = (int32_t)s->h;
        set_cb_extra();
    }
    RET(D3D_OK);
}
static void dev_get_rt(CPU* c)
{
    uint32_t i = (uint32_t)A2;
    uint64_t o = i < 4 ? S.rt[i] : 0;
    STP(A3, addref(o));
    RET(o ? D3D_OK : 0x88760866u);
}
static void dev_set_ds(CPU* c) { S.ds = A2; RET(D3D_OK); }
static void dev_get_ds(CPU* c) { STP(A2, addref(S.ds)); RET(S.ds ? D3D_OK : 0x88760866u); }

static void dev_clear(CPU* c)
{
    uint32_t n = (uint32_t)A2, flags = (uint32_t)A4 & 7, stencil = (uint32_t)ARG(7);
    uint64_t rects = A3;
    float z = f32of(ARG(6)), col[4];
    argb((uint32_t)ARG(5), col);
    D9* t = D(S.rt[0]);
    D9* ds = D(S.ds);
    if (!t) t = ds;
    if (!t) { RET(D3D_OK); return; }
    uint32_t tw = t->w, th = t->h;
    int32_t area[4] = { (int32_t)S.vp[0], (int32_t)S.vp[1], (int32_t)(S.vp[0] + S.vp[2]), (int32_t)(S.vp[1] + S.vp[3]) };
    if (S.rs[174]) {
        if (S.scissor[0] > area[0]) area[0] = S.scissor[0];
        if (S.scissor[1] > area[1]) area[1] = S.scissor[1];
        if (S.scissor[2] < area[2]) area[2] = S.scissor[2];
        if (S.scissor[3] < area[3]) area[3] = S.scissor[3];
    }
    int full = !(n && rects) && area[0] <= 0 && area[1] <= 0 && area[2] >= (int32_t)tw && area[3] >= (int32_t)th;
    if (full) {
        if ((flags & 1) && gpu->clear_rtv)
            for (int i = 0; i < 4; i++) { D9* s = D(S.rt[i]); GpuView* v = s ? surf_rtv(s, S.rs[194] != 0) : NULL; if (v) gpu->clear_rtv(v, col); }
        if ((flags & 6) && ds && gpu->clear_dsv) { GpuView* v = surf_dsv(ds); if (v) gpu->clear_dsv(v, (flags >> 1) & 3, z, (uint8_t)stencil); }
        RET(D3D_OK);
        return;
    }
    int32_t* rl = calloc(n && rects ? n : 1, 16);
    uint32_t k = 0;
    if (n && rects) {
        for (uint32_t i = 0; i < n; i++) {
            int32_t r[4];
            for (int j = 0; j < 4; j++) r[j] = (int32_t)LD32(rects + 16ull * i + 4ull * j);
            if (r[0] < area[0]) r[0] = area[0];
            if (r[1] < area[1]) r[1] = area[1];
            if (r[2] > area[2]) r[2] = area[2];
            if (r[3] > area[3]) r[3] = area[3];
            memcpy(rl + 4 * k++, r, 16);
        }
    } else { memcpy(rl, area, 16); k = 1; }
    builtin_clear(flags, col, z, stencil, rl, k, NULL, tw, th);
    free(rl);
    RET(D3D_OK);
}

static void dev_get_transform(CPU* c)
{
    float* m = A3 ? (float*)G2H(A3) : NULL;
    if (m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1.0f; }
    RET(D3D_OK);
}
static void dev_set_viewport(CPU* c) { if (A2) { memcpy(S.vp, G2H(A2), 24); set_cb_extra(); } RET(D3D_OK); }
static void dev_get_viewport(CPU* c) { if (A2) memcpy(G2H(A2), S.vp, 24); RET(D3D_OK); }
static void dev_set_rs(CPU* c)
{
    uint32_t s = (uint32_t)A2;
    if (s < NRS) {
        S.rs[s] = (uint32_t)A3;
        if (s == 15 || s == 24 || s == 25) set_cb_extra();
    }
    RET(D3D_OK);
}
static void dev_get_rs(CPU* c) { uint32_t s = (uint32_t)A2; if (A3) ST32(A3, s < NRS ? S.rs[s] : 0); RET(D3D_OK); }

static void dev_state_block(CPU* c)
{
    static int said;
    if (said++ < 2) host_log("[d3d9] state blocks are not implemented (capture and apply do nothing)");
    STP(A3, com_new(&K_SB, NULL, 0));
    RET(D3D_OK);
}
static void dev_end_state_block(CPU* c) { STP(A2, com_new(&K_SB, NULL, 0)); RET(D3D_OK); }

static void dev_get_texture(CPU* c) { int s = samp_slot((uint32_t)A2); uint64_t o = s >= 0 ? S.tex[s] : 0; STP(A3, addref(o)); RET(D3D_OK); }
static void dev_set_texture(CPU* c) { int s = samp_slot((uint32_t)A2); if (s >= 0) S.tex[s] = A3; RET(D3D_OK); }
static void dev_get_tss(CPU* c) { if (A4) ST32(A4, 0); RET(D3D_OK); }
static void dev_get_ss(CPU* c)
{
    int s = samp_slot((uint32_t)A2);
    uint32_t ty = (uint32_t)A3;
    if (A4) ST32(A4, s >= 0 && ty < 14 ? S.ss[s][ty] : 0);
    RET(D3D_OK);
}
static void dev_set_ss(CPU* c)
{
    int s = samp_slot((uint32_t)A2);
    uint32_t ty = (uint32_t)A3;
    if (s >= 0 && ty < 14) S.ss[s][ty] = (uint32_t)A4;
    RET(D3D_OK);
}
static void dev_validate(CPU* c) { if (A2) ST32(A2, 1); RET(D3D_OK); }
static void dev_set_scissor(CPU* c) { if (A2) memcpy(S.scissor, G2H(A2), 16); RET(D3D_OK); }
static void dev_get_scissor(CPU* c) { if (A2) memcpy(G2H(A2), S.scissor, 16); RET(D3D_OK); }

/* vertex declarations, from elements or from an FVF code */
static Decl* new_decl(void) { Decl* d = calloc(1, sizeof *d); d->obj = com_new(&K_Decl, d, 0); return d; }
static void dev_create_decl(CPU* c)
{
    Decl* d = new_decl();
    uint64_t e = A2;
    for (uint32_t i = 0; i < 64; i++, e += 8) {
        uint16_t st = *(uint16_t*)G2H(e);
        if (st == 0xFF || st == 0xFFFF) break;
        d->e[d->n].stream = st;
        d->e[d->n].offset = *(uint16_t*)G2H(e + 2);
        d->e[d->n].type = *(uint8_t*)G2H(e + 4);
        d->e[d->n].method = *(uint8_t*)G2H(e + 5);
        d->e[d->n].usage = *(uint8_t*)G2H(e + 6);
        d->e[d->n].index = *(uint8_t*)G2H(e + 7);
        d->n++;
    }
    STP(A3, d->obj);
    RET(D3D_OK);
}
static void dev_set_decl(CPU* c) { S.decl = A2 ? com_host(A2) : NULL; RET(D3D_OK); }
static void dev_get_decl(CPU* c) { uint64_t o = S.decl ? S.decl->obj : 0; if (o) ST32(o + 8, LD32(o + 8) + 1); STP(A2, o); RET(D3D_OK); }

static void dev_set_fvf(CPU* c)
{
    static struct { uint32_t fvf; Decl* d; } cache[64];
    static int n;
    uint32_t f = (uint32_t)A2;
    S.fvf = f;
    if (!f) { RET(D3D_OK); return; }
    for (int i = 0; i < n; i++) if (cache[i].fvf == f) { S.decl = cache[i].d; RET(D3D_OK); return; }
    Decl* d = new_decl();
    ST32(d->obj + 8, 1000000);
    uint16_t off = 0;
#define EL(ty, us, ix, sz) do { d->e[d->n].offset = off; d->e[d->n].type = (ty); d->e[d->n].usage = (us); d->e[d->n].index = (ix); d->n++; off += (sz); } while (0)
    uint32_t pos = f & 0x400E;
    if (pos == 0x2) EL(2, 0, 0, 12);
    else if (pos == 0x4) EL(3, 9, 0, 16);
    else if (pos >= 0x6 && pos <= 0xE) { EL(2, 0, 0, 12); off += (uint16_t)(4 * ((pos - 0x4) / 2)); }
    if (f & 0x10) EL(2, 3, 0, 12);
    if (f & 0x20) EL(0, 4, 0, 4);
    if (f & 0x40) EL(4, 10, 0, 4);
    if (f & 0x80) EL(4, 10, 1, 4);
    uint32_t ntex = (f >> 8) & 15;
    for (uint32_t i = 0; i < ntex; i++) {
        uint32_t k = (f >> (16 + 2 * i)) & 3;
        static const uint8_t ty[4] = { 1, 2, 3, 0 }, sz[4] = { 8, 12, 16, 4 };
        EL(ty[k], 5, i, sz[k]);
    }
#undef EL
    if (n < 64) { cache[n].fvf = f; cache[n].d = d; n++; }
    S.decl = d;
    RET(D3D_OK);
}
static void dev_get_fvf(CPU* c) { if (A2) ST32(A2, S.fvf); RET(D3D_OK); }

static uint32_t sh_size(uint64_t p)
{
    if (!p) return 0;
    uint32_t v = (uint32_t)LD32(p);
    if ((v & 0xFFFE0000u) != 0xFFFE0000u) return 0;
    int major = (int)((v >> 8) & 0xFF);
    for (uint32_t i = 1; i < (1u << 20);) {
        uint32_t tok = (uint32_t)LD32(p + 4ull * i), op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu) return (i + 1) * 4;
        if (op == 0xFFFE) i += 1 + ((tok >> 16) & 0x7FFF);
        else i += major >= 2 && !(tok & 0x80000000u) ? 1 + ((tok >> 24) & 0xF) : 1;
    }
    return 0;
}
static void make_shader_obj(CPU* c, int ps)
{
    uint32_t size = sh_size(A2);
    if (!size) { STP(A3, 0); RET(D3DERR_INVALIDCALL); return; }
    Sh* s = make_sh((const uint32_t*)G2H(A2), size, ps);
    s->obj = com_new(ps ? &K_PS : &K_VS, s, 0);
    STP(A3, s->obj);
    RET(D3D_OK);
}
static void dev_create_vs(CPU* c) { make_shader_obj(c, 0); }
static void dev_create_ps(CPU* c) { make_shader_obj(c, 1); }
static void dev_set_vs(CPU* c) { S.vs = A2 ? com_host(A2) : NULL; RET(D3D_OK); }
static void dev_set_ps(CPU* c) { S.ps = A2 ? com_host(A2) : NULL; RET(D3D_OK); }
static void dev_get_vs(CPU* c) { uint64_t o = S.vs ? S.vs->obj : 0; if (o) ST32(o + 8, LD32(o + 8) + 1); STP(A2, o); RET(D3D_OK); }
static void dev_get_ps(CPU* c) { uint64_t o = S.ps ? S.ps->obj : 0; if (o) ST32(o + 8, LD32(o + 8) + 1); STP(A2, o); RET(D3D_OK); }

/* constants: kind 0 float (c0-255), 1 int (i0-15), 2 bool (b0-15) */
static void consts(CPU* c, int ps, int kind, int set)
{
    uint32_t start = (uint32_t)A2, n = (uint32_t)A4;
    uint64_t data = A3, cb = ps ? S.pcb : S.vcb;
    uint32_t lim = kind ? 16 : 256, base = kind == 1 ? D3D9_CB_INT : kind == 2 ? D3D9_CB_BOOL : 0;
    if (!data || start >= lim) { RET(D3DERR_INVALIDCALL); return; }
    if (start + n > lim) n = lim - start;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t slot = cb + 16ull * (base + start + i);
        if (kind == 2) { if (set) ST32(slot, LD32(data + 4ull * i)); else ST32(data + 4ull * i, LD32(slot)); }
        else if (set) memcpy(G2H(slot), G2H(data + 16ull * i), 16);
        else memcpy(G2H(data + 16ull * i), G2H(slot), 16);
    }
    if (set) { if (ps) S.pcb_dirty = 1; else S.vcb_dirty = 1; }
    RET(D3D_OK);
}
static void vsf(CPU* c) { consts(c, 0, 0, 1); }
static void vsfg(CPU* c) { consts(c, 0, 0, 0); }
static void vsi(CPU* c) { consts(c, 0, 1, 1); }
static void vsig(CPU* c) { consts(c, 0, 1, 0); }
static void vsb(CPU* c) { consts(c, 0, 2, 1); }
static void vsbg(CPU* c) { consts(c, 0, 2, 0); }
static void psf(CPU* c) { consts(c, 1, 0, 1); }
static void psfg(CPU* c) { consts(c, 1, 0, 0); }
static void psi(CPU* c) { consts(c, 1, 1, 1); }
static void psig(CPU* c) { consts(c, 1, 1, 0); }
static void psb(CPU* c) { consts(c, 1, 2, 1); }
static void psbg(CPU* c) { consts(c, 1, 2, 0); }

static void dev_set_stream(CPU* c)
{
    uint32_t i = (uint32_t)A2;
    if (i >= 16) { RET(D3DERR_INVALIDCALL); return; }
    S.vb[i] = A3; S.vb_off[i] = (uint32_t)A4; S.vb_stride[i] = (uint32_t)ARG(5);
    RET(D3D_OK);
}
static void dev_get_stream(CPU* c)
{
    uint32_t i = (uint32_t)A2 & 15;
    STP(A3, addref(S.vb[i]));
    if (A4) ST32(A4, S.vb_off[i]);
    if (ARG(5)) ST32(ARG(5), S.vb_stride[i]);
    RET(D3D_OK);
}
static void dev_set_freq(CPU* c) { uint32_t i = (uint32_t)A2; if (i < 16) S.vb_freq[i] = (uint32_t)A3; RET(D3D_OK); }
static void dev_get_freq(CPU* c) { uint32_t i = (uint32_t)A2; if (A3) ST32(A3, i < 16 ? S.vb_freq[i] : 1); RET(D3D_OK); }
static void dev_set_indices(CPU* c) { S.ib = A2; RET(D3D_OK); }
static void dev_get_indices(CPU* c) { STP(A2, addref(S.ib)); RET(D3D_OK); }
static void dev_invalid(CPU* c) { RET(D3DERR_INVALIDCALL); }

static void dev_create_query(CPU* c)
{
    uint32_t ty = (uint32_t)A2;
    if (ty != 8 && ty != 9) { if (A3) STP(A3, 0); RET(D3DERR_NOTAVAILABLE); return; }
    if (!A3) { RET(D3D_OK); return; }
    Query* q = calloc(1, sizeof *q);
    q->type = ty;
    q->obj = com_new(&K_Query, q, 0);
    STP(A3, q->obj);
    RET(D3D_OK);
}
static void q_type(CPU* c) { Query* q = com_host(THIS); RET(q ? q->type : 0); }
static void q_size(CPU* c) { RET(4); }
static void q_issue(CPU* c) { Query* q = com_host(THIS); if (q) q->issued = 1; RET(D3D_OK); }
static void q_data(CPU* c)
{
    Query* q = com_host(THIS);
    /* events are always done; occlusion queries report everything visible (nothing is culled wrongly) */
    if (A2 && (uint32_t)A3 >= 4) ST32(A2, q && q->type == 9 ? 100000 : 1);
    RET(D3D_OK);
}

/* ---------------------------------------------------------------- resource methods */

static void child_device(CPU* c) { ST32(S.dev + 8, LD32(S.dev + 8) + 1); STP(A2, S.dev); RET(D3D_OK); }
static void priv_get(CPU* c) { RET(0x88760866u); }
static void perm_qi(CPU* c)
{
    uint64_t o = THIS;
    D9* s = com_kind(o) == &K_Surf ? D(o) : NULL;
    uint64_t r = s && s->parent ? s->parent->obj : o;
    ST32(r + 8, LD32(r + 8) + 1);
    STP(A3, o);
    RET(D3D_OK);
}
static void res_type(CPU* c)
{
    D9* t = D(THIS);
    static const uint32_t T[] = { 0, 3, 5, 4, 1, 6, 7 };
    RET(t && t->kind <= 6 ? T[t->kind] : 0);
}
static void tex_levels(CPU* c) { D9* t = D(THIS); RET(t ? t->levels : 0); }
static void tex_genmips(CPU* c) { D9* t = D(THIS); GpuView* v = tex_srv(t, 0); if (v && gpu->gen_mips) gpu->gen_mips(v); RET(0); }
static void surf_desc(D9* t, uint32_t level, uint64_t p)
{
    uint32_t w, h, d;
    sub_dims(t, level, &w, &h, &d);
    uint32_t v[8] = { t->fmt, 1, t->usage, t->pool, 0, 0, w, h };
    memcpy(G2H(p), v, 32);
}
static void tex_level_desc(CPU* c)
{
    D9* t = D(THIS);
    uint32_t l = (uint32_t)A2;
    if (!t || l >= t->levels || !A3) { RET(D3DERR_INVALIDCALL); return; }
    if (t->kind == O_VOL) {
        uint32_t w, h, d;
        sub_dims(t, l, &w, &h, &d);
        uint32_t v[7] = { t->fmt, 2, t->usage, t->pool, w, h, d };
        memcpy(G2H(A3), v, 28);
    } else surf_desc(t, l, A3);
    RET(D3D_OK);
}
static void tex_surface(CPU* c) { D9* t = D(THIS); uint64_t o = t ? surface_of(t, 0, (uint32_t)A2) : 0; STP(A3, addref(o)); RET(o ? D3D_OK : D3DERR_INVALIDCALL); }
static void cube_surface(CPU* c) { D9* t = D(THIS); uint64_t o = t ? surface_of(t, (uint32_t)A2, (uint32_t)A3) : 0; STP(A4, addref(o)); RET(o ? D3D_OK : D3DERR_INVALIDCALL); }
static void tex_lock(CPU* c) { lock_rect(c, D(THIS), 0, (uint32_t)A2, A3, A4, (uint32_t)ARG(5)); }
static void tex_unlock(CPU* c) { unlock_rect(c, D(THIS), 0, (uint32_t)A2); }
static void cube_lock(CPU* c) { lock_rect(c, D(THIS), (uint32_t)A2, (uint32_t)A3, A4, ARG(5), (uint32_t)ARG(6)); }
static void cube_unlock(CPU* c) { unlock_rect(c, D(THIS), (uint32_t)A2, (uint32_t)A3); }
static void vol_lock(CPU* c)
{
    D9* t = D(THIS);
    uint32_t l = (uint32_t)A2;
    uint64_t out = A3;
    if (!t || !out || l >= t->levels) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t p = sub_mem(t, 0, l);
    if (!p) { RET(0x8007000Eu); return; }
    uint32_t pitch = d9_pitch(t, l), slice = pitch * d9_rows(t, l);
    if (A4) p += (uint64_t)LD32(A4 + 8) * slice + (uint64_t)LD32(A4 + 4) * pitch + (uint64_t)LD32(A4) * t->f->bpb;
    ST32(out, pitch); ST32(out + 4, slice); STP(out + 8, p);
    t->lock_ro = ((uint32_t)ARG(5) & 0x10) != 0;
    RET(D3D_OK);
}
static void vol_unlock(CPU* c) { unlock_rect(c, D(THIS), 0, (uint32_t)A2); }
static void surf_container(CPU* c)
{
    D9* s = D(THIS);
    uint64_t o = s && s->parent ? s->parent->obj : S.dev;
    ST32(o + 8, LD32(o + 8) + 1);
    STP(A3, o);
    RET(D3D_OK);
}
static void surf_get_desc(CPU* c) { D9* s = D(THIS); if (s && A2) { uint32_t f, l; D9* o = owner(s, &f, &l); surf_desc(o, l, A2); } RET(D3D_OK); }
static void surf_lock(CPU* c)
{
    D9* s = D(THIS);
    if (!s) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t f, l;
    D9* o = owner(s, &f, &l);
    lock_rect(c, o, f, l, A2, A3, (uint32_t)A4);
}
static void surf_unlock(CPU* c)
{
    D9* s = D(THIS);
    if (!s) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t f, l;
    D9* o = owner(s, &f, &l);
    unlock_rect(c, o, f, l);
}
static void buf_lock(CPU* c)
{
    D9* b = D(THIS);
    if (!b || !A4) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t off = (uint32_t)A2, n = (uint32_t)A3, fl = (uint32_t)ARG(5);
    if (off > b->w) off = b->w;
    if (!n || off + n > b->w) n = b->w - off;
    uint64_t m = lock_mem(b);
    STP(A4, m + off);
    if (!(fl & 0x10)) {
        if (b->lock_hi <= b->lock_lo) { b->lock_lo = off; b->lock_hi = off + n; }
        else { if (off < b->lock_lo) b->lock_lo = off; if (off + n > b->lock_hi) b->lock_hi = off + n; }
        b->lock_lo &= ~3u;
        b->lock_hi = (b->lock_hi + 3) & ~3u;
        if (b->lock_hi > ((b->w + 3) & ~3u)) b->lock_hi = (b->w + 3) & ~3u;
    }
    RET(D3D_OK);
}
static void buf_unlock(CPU* c) { flush_buffer(D(THIS)); RET(D3D_OK); }
static void buf_desc(CPU* c)
{
    D9* b = D(THIS);
    if (!b || !A2) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t v[6] = { b->fmt, b->kind == O_VB ? 6u : 7u, b->usage, b->pool, b->w, b->fvf };
    memcpy(G2H(A2), v, b->kind == O_VB ? 24 : 20);
    RET(D3D_OK);
}
static void decl_get(CPU* c)
{
    Decl* d = com_host(THIS);
    if (!d) { RET(D3DERR_INVALIDCALL); return; }
    if (A3) ST32(A3, d->n + 1);
    if (A2) {
        for (uint32_t i = 0; i <= d->n; i++) {
            uint64_t e = A2 + 8ull * i;
            int end = i == d->n;
            *(uint16_t*)G2H(e) = end ? 0xFF : d->e[i].stream; *(uint16_t*)G2H(e + 2) = end ? 0 : d->e[i].offset;
            *(uint8_t*)G2H(e + 4) = end ? 17 : d->e[i].type; *(uint8_t*)G2H(e + 5) = end ? 0 : d->e[i].method;
            *(uint8_t*)G2H(e + 6) = end ? 0 : d->e[i].usage; *(uint8_t*)G2H(e + 7) = end ? 0 : d->e[i].index;
        }
    }
    RET(D3D_OK);
}
static void sh_function(CPU* c)
{
    Sh* s = com_host(THIS);
    if (!s || !A3) { RET(D3DERR_INVALIDCALL); return; }
    if (A2) memcpy(G2H(A2), s->code, s->size);
    ST32(A3, s->size);
    RET(D3D_OK);
}

/* swap chain */
static void sc_backbuffer(CPU* c) { STP(A4, addref(S.backbuf)); RET(D3D_OK); }
static void sc_raster(CPU* c) { if (A2) { ST32(A2, 0); ST32(A2 + 4, 0); } RET(D3D_OK); }
static void sc_mode(CPU* c) { if (A2) display_mode(A2); RET(D3D_OK); }
static void sc_params(CPU* c) { if (A2) memcpy(G2H(A2), S.pp, RT_G32 ? 56 : 64); RET(D3D_OK); }

/* ---------------------------------------------------------------- setup */

/* stack argument counts per method, `this` included, one character each (0-9, then a = 10) */
static void slots(ComKind* k, const char* n)
{
    for (int i = 0; n[i] && i < k->nslots; i++) k->nargs[i] = (uint8_t)(n[i] >= 'a' ? n[i] - 'a' + 10 : n[i] - '0');
}
#define SET(k, i, f) ((k).slots[i] = (f))

static void res_base(ComKind* k)
{
    SET(*k, 0, perm_qi); SET(*k, 1, surf_addref); SET(*k, 2, res_release);
    SET(*k, 3, child_device); SET(*k, 4, dev_ok); SET(*k, 5, priv_get); SET(*k, 6, dev_ok);
    SET(*k, 7, dev_zero); SET(*k, 8, dev_zero); SET(*k, 9, dev_zero); SET(*k, 10, res_type);
}

static void setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    backend();
    ComKind* all[] = { &K_D3D, &K_Dev, &K_Tex, &K_Cube, &K_Vol, &K_Surf, &K_VB, &K_IB, &K_Decl, &K_VS, &K_PS, &K_Query, &K_SC, &K_SB };
    static const char* names[] = { "IDirect3D9", "IDirect3DDevice9", "IDirect3DTexture9", "IDirect3DCubeTexture9", "IDirect3DVolumeTexture9",
                                   "IDirect3DSurface9", "IDirect3DVertexBuffer9", "IDirect3DIndexBuffer9", "IDirect3DVertexDeclaration9",
                                   "IDirect3DVertexShader9", "IDirect3DPixelShader9", "IDirect3DQuery9", "IDirect3DSwapChain9", "IDirect3DStateBlock9" };
    static const int counts[] = { 17, 119, 22, 22, 22, 17, 14, 14, 5, 5, 5, 8, 10, 6 };
    for (int i = 0; i < 14; i++) { all[i]->name = names[i]; all[i]->nslots = counts[i]; com_iunknown(all[i]); all[i]->slots[0] = perm_qi; }

    slots(&K_D3D, "31121435367765427");
    SET(K_D3D, 4, d3d_adapter_count); SET(K_D3D, 5, d3d_identifier); SET(K_D3D, 6, d3d_mode_count);
    SET(K_D3D, 7, d3d_enum_modes); SET(K_D3D, 8, d3d_display_mode); SET(K_D3D, 9, d3d_check_type); SET(K_D3D, 10, d3d_check_format);
    SET(K_D3D, 11, d3d_check_ms); SET(K_D3D, 12, d3d_check_ds_match); SET(K_D3D, 13, d3d_check_conv); SET(K_D3D, 14, d3d_caps);
    SET(K_D3D, 15, d3d_monitor); SET(K_D3D, 16, d3d_create_device);

    /* IDirect3DDevice9, methods 0-118 */
    slots(&K_Dev, "311111223244233125532439a8779953336473322117333222233333333312223344442332222212147597322223224444445533223224444444423");                         /* 118 */
    SET(K_Dev, 3, dev_test_coop); SET(K_Dev, 4, dev_tex_mem); SET(K_Dev, 5, dev_ok); SET(K_Dev, 6, dev_get_d3d);
    SET(K_Dev, 7, dev_get_caps); SET(K_Dev, 8, dev_display_mode); SET(K_Dev, 9, dev_creation_params); SET(K_Dev, 10, dev_ok);
    SET(K_Dev, 11, dev_ok); SET(K_Dev, 12, dev_show_cursor); SET(K_Dev, 13, dev_invalid); SET(K_Dev, 14, dev_get_swapchain);
    SET(K_Dev, 15, dev_num_swapchains); SET(K_Dev, 16, dev_reset); SET(K_Dev, 17, dev_present); SET(K_Dev, 18, dev_get_backbuffer);
    SET(K_Dev, 19, dev_raster_status); SET(K_Dev, 20, dev_ok); SET(K_Dev, 21, dev_ok); SET(K_Dev, 22, dev_ok);
    SET(K_Dev, 23, dev_create_texture); SET(K_Dev, 24, dev_create_volume); SET(K_Dev, 25, dev_create_cube); SET(K_Dev, 26, dev_create_vb);
    SET(K_Dev, 27, dev_create_ib); SET(K_Dev, 28, dev_create_rt); SET(K_Dev, 29, dev_create_ds); SET(K_Dev, 30, dev_update_surface);
    SET(K_Dev, 31, dev_update_texture); SET(K_Dev, 32, dev_get_rt_data); SET(K_Dev, 33, dev_ok); SET(K_Dev, 34, dev_stretch);
    SET(K_Dev, 35, dev_color_fill); SET(K_Dev, 36, dev_create_offscreen); SET(K_Dev, 37, dev_set_rt); SET(K_Dev, 38, dev_get_rt);
    SET(K_Dev, 39, dev_set_ds); SET(K_Dev, 40, dev_get_ds); SET(K_Dev, 41, dev_ok); SET(K_Dev, 42, dev_ok); SET(K_Dev, 43, dev_clear);
    SET(K_Dev, 44, dev_ok); SET(K_Dev, 45, dev_get_transform); SET(K_Dev, 46, dev_ok); SET(K_Dev, 47, dev_set_viewport);
    SET(K_Dev, 48, dev_get_viewport);
    for (int i = 49; i <= 56; i++) SET(K_Dev, i, dev_ok);   /* materials, lights, clip planes: not used with shaders */
    SET(K_Dev, 57, dev_set_rs); SET(K_Dev, 58, dev_get_rs); SET(K_Dev, 59, dev_state_block); SET(K_Dev, 60, dev_ok);
    SET(K_Dev, 61, dev_end_state_block); SET(K_Dev, 62, dev_ok); SET(K_Dev, 63, dev_ok); SET(K_Dev, 64, dev_get_texture);
    SET(K_Dev, 65, dev_set_texture); SET(K_Dev, 66, dev_get_tss); SET(K_Dev, 67, dev_ok); SET(K_Dev, 68, dev_get_ss);
    SET(K_Dev, 69, dev_set_ss); SET(K_Dev, 70, dev_validate);
    for (int i = 71; i <= 74; i++) SET(K_Dev, i, dev_ok);
    SET(K_Dev, 75, dev_set_scissor); SET(K_Dev, 76, dev_get_scissor); SET(K_Dev, 77, dev_ok); SET(K_Dev, 78, dev_zero);
    SET(K_Dev, 79, dev_ok); SET(K_Dev, 80, dev_zero); SET(K_Dev, 81, dev_draw); SET(K_Dev, 82, dev_draw_indexed);
    SET(K_Dev, 83, dev_draw_up); SET(K_Dev, 84, dev_draw_indexed_up); SET(K_Dev, 85, dev_invalid); SET(K_Dev, 86, dev_create_decl);
    SET(K_Dev, 87, dev_set_decl); SET(K_Dev, 88, dev_get_decl); SET(K_Dev, 89, dev_set_fvf); SET(K_Dev, 90, dev_get_fvf);
    SET(K_Dev, 91, dev_create_vs); SET(K_Dev, 92, dev_set_vs); SET(K_Dev, 93, dev_get_vs);
    SET(K_Dev, 94, vsf); SET(K_Dev, 95, vsfg); SET(K_Dev, 96, vsi); SET(K_Dev, 97, vsig); SET(K_Dev, 98, vsb); SET(K_Dev, 99, vsbg);
    SET(K_Dev, 100, dev_set_stream); SET(K_Dev, 101, dev_get_stream); SET(K_Dev, 102, dev_set_freq); SET(K_Dev, 103, dev_get_freq);
    SET(K_Dev, 104, dev_set_indices); SET(K_Dev, 105, dev_get_indices); SET(K_Dev, 106, dev_create_ps); SET(K_Dev, 107, dev_set_ps);
    SET(K_Dev, 108, dev_get_ps);
    SET(K_Dev, 109, psf); SET(K_Dev, 110, psfg); SET(K_Dev, 111, psi); SET(K_Dev, 112, psig); SET(K_Dev, 113, psb); SET(K_Dev, 114, psbg);
    SET(K_Dev, 115, dev_invalid); SET(K_Dev, 116, dev_invalid); SET(K_Dev, 117, dev_ok); SET(K_Dev, 118, dev_create_query);

    ComKind* res[] = { &K_Tex, &K_Cube, &K_Vol, &K_Surf, &K_VB, &K_IB };
    for (int i = 0; i < 6; i++) res_base(res[i]);
    slots(&K_Tex, "3112542211121121133522"); slots(&K_Cube, "3112542211121121134633"); slots(&K_Vol, "3112542211121121133522");
    slots(&K_Surf, "31125422111324122"); slots(&K_VB, "31125422111512"); slots(&K_IB, "31125422111512");
    ComKind* texs[] = { &K_Tex, &K_Cube, &K_Vol };
    for (int i = 0; i < 3; i++) {
        SET(*texs[i], 11, dev_zero); SET(*texs[i], 12, dev_zero); SET(*texs[i], 13, tex_levels); SET(*texs[i], 14, dev_ok);
        SET(*texs[i], 15, dev_zero); SET(*texs[i], 16, tex_genmips); SET(*texs[i], 17, tex_level_desc); SET(*texs[i], 21, dev_ok);
    }
    SET(K_Tex, 18, tex_surface); SET(K_Tex, 19, tex_lock); SET(K_Tex, 20, tex_unlock);
    SET(K_Cube, 18, cube_surface); SET(K_Cube, 19, cube_lock); SET(K_Cube, 20, cube_unlock);
    SET(K_Vol, 18, dev_invalid); SET(K_Vol, 19, vol_lock); SET(K_Vol, 20, vol_unlock);
    SET(K_Surf, 11, surf_container); SET(K_Surf, 12, surf_get_desc); SET(K_Surf, 13, surf_lock); SET(K_Surf, 14, surf_unlock);
    SET(K_Surf, 15, dev_invalid); SET(K_Surf, 16, dev_ok);
    SET(K_VB, 11, buf_lock); SET(K_VB, 12, buf_unlock); SET(K_VB, 13, buf_desc);
    SET(K_IB, 11, buf_lock); SET(K_IB, 12, buf_unlock); SET(K_IB, 13, buf_desc);
    slots(&K_Decl, "31123"); SET(K_Decl, 3, child_device); SET(K_Decl, 4, decl_get);
    slots(&K_VS, "31123"); SET(K_VS, 3, child_device); SET(K_VS, 4, sh_function);
    slots(&K_PS, "31123"); SET(K_PS, 3, child_device); SET(K_PS, 4, sh_function);
    slots(&K_Query, "31121124"); SET(K_Query, 3, child_device); SET(K_Query, 4, q_type); SET(K_Query, 5, q_size);
    SET(K_Query, 6, q_issue); SET(K_Query, 7, q_data);
    slots(&K_SC, "3116242222"); SET(K_SC, 3, dev_present); SET(K_SC, 4, dev_ok); SET(K_SC, 5, sc_backbuffer); SET(K_SC, 6, sc_raster);
    SET(K_SC, 7, sc_mode); SET(K_SC, 8, child_device); SET(K_SC, 9, sc_params);
    slots(&K_SB, "311211"); SET(K_SB, 3, child_device); SET(K_SB, 4, dev_ok); SET(K_SB, 5, dev_ok);

    S.d3d = com_new(&K_D3D, NULL, 0);
    S.dev = com_new(&K_Dev, NULL, 0);
    S.sc = com_new(&K_SC, NULL, 0);
    ST32(S.d3d + 8, 1000000); ST32(S.dev + 8, 1000000); ST32(S.sc + 8, 1000000);
    S.vcb_r = new_buf(D3D9_CB_VEC4S * 16, 0x4);
    S.pcb_r = new_buf(D3D9_CB_VEC4S * 16, 0x4);
    S.vcb = S.vcb_r->backing;
    S.pcb = S.pcb_r->backing;
    S.up_vb = new_buf(UP_SIZE, 0x1);
    S.up_ib = new_buf(1u << 20, 0x2);
    host_log("[d3d9] Direct3D 9 ready (%s backend)", gpu->name);
}

static void d3d_create_device(CPU* c)
{
    if (!ARG(6) || !ARG(7)) { RET(D3DERR_INVALIDCALL); return; }
    present_params(ARG(6));
    make_backbuffer();
    default_state();
    if (gpu->resize) gpu->resize(S.bb_w, S.bb_h);
    STP(ARG(7), S.dev);
    RET(D3D_OK);
}

HLE(Direct3DCreate9) { setup(); RET(S.d3d); }
HLE(Direct3DCreate9Ex) { if (A2) STP(A2, 0); RET(D3DERR_NOTAVAILABLE); }
HLE(D3DPERF_SetOptions) { RET(0); }
HLE(D3DPERF_SetRegion) { RET(0); }
HLE(D3DPERF_QueryRepeatFrame) { RET(0); }
