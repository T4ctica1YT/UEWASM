#include "com.h"
#include "gpu.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>

GpuBackend* gpu;
static GpuCtx ctx;
static uint64_t g_device, g_context, g_factory, g_adapter, g_output, g_dxgidevice, g_swapchain, g_backbuffer;
static uint64_t sc_desc;

#define I(a, b, c_, d0, d1, d2, d3, d4, d5, d6, d7) { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, (c_) & 0xFF, ((c_) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t IID_Device[][16] = { I(0xdb6f6ddb, 0xac77, 0x4e88, 0x82, 0x53, 0x81, 0x9d, 0xf9, 0xbb, 0xf1, 0x40) };
static const uint8_t IID_Context[][16] = { I(0xc0bfa96c, 0xe089, 0x44fb, 0x8e, 0xaf, 0x26, 0xf8, 0x79, 0x61, 0x90, 0xda) };
static const uint8_t IID_DXGIDevice[][16] = { I(0x54ec77fa, 0x1377, 0x44e6, 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c),
                                              I(0x77db970f, 0x6276, 0x48ba, 0xba, 0x28, 0x07, 0x01, 0x43, 0xb4, 0x39, 0x2c) };
static const uint8_t IID_Factory[][16] = { I(0x7b7166ec, 0x21c7, 0x44ae, 0xb2, 0x1a, 0xc9, 0xae, 0x32, 0x1a, 0xe3, 0x69),
                                           I(0x770aae78, 0xf26f, 0x4dba, 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87) };
static const uint8_t IID_Adapter[][16] = { I(0x2411e7e1, 0x12ac, 0x4ccf, 0xbd, 0x14, 0x97, 0x98, 0xe8, 0x53, 0x4d, 0xc0),
                                           I(0x29038f61, 0x3839, 0x4626, 0x91, 0xfd, 0x08, 0x68, 0x79, 0x01, 0x1a, 0x05) };
static const uint8_t IID_Output[][16] = { I(0xae02eedb, 0xc735, 0x4690, 0x8d, 0x52, 0x5a, 0x8d, 0xc2, 0x02, 0x13, 0xaa) };
static const uint8_t IID_SwapChain[][16] = { I(0x310d36a0, 0xd2e7, 0x4c0a, 0xaa, 0x04, 0x6a, 0x9d, 0x23, 0xb8, 0x88, 0x6a) };
static const uint8_t IID_Tex2D[][16] = { I(0x6f15aaf2, 0xd208, 0x4e89, 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c) };
static const uint8_t IID_Resource[][16] = { I(0xdc8e63f3, 0xd12b, 0x4952, 0xb4, 0x7b, 0x5e, 0x45, 0x02, 0x6a, 0x86, 0x2d) };

static ComKind K_Device = { "ID3D11Device", 43, {0}, IID_Device, 1 };
static ComKind K_Context = { "ID3D11DeviceContext", 115, {0}, IID_Context, 1 };
static ComKind K_DXGIDevice = { "IDXGIDevice", 16, {0}, IID_DXGIDevice, 2 };
static ComKind K_Factory = { "IDXGIFactory1", 14, {0}, IID_Factory, 2 };
static ComKind K_Adapter = { "IDXGIAdapter1", 11, {0}, IID_Adapter, 2 };
static ComKind K_Output = { "IDXGIOutput", 19, {0}, IID_Output, 1 };
static ComKind K_SwapChain = { "IDXGISwapChain", 18, {0}, IID_SwapChain, 1 };
static ComKind K_Buffer = { "ID3D11Buffer", 11, {0}, IID_Resource, 1 };
static ComKind K_Tex = { "ID3D11Texture", 11, {0}, IID_Tex2D, 1 };
static ComKind K_View = { "ID3D11View", 9, {0} };
static ComKind K_Shader = { "ID3D11Shader", 7, {0} };
static ComKind K_State = { "ID3D11State", 8, {0} };
static ComKind K_Layout = { "ID3D11InputLayout", 7, {0} };
static ComKind K_Query = { "ID3D11Query", 10, {0} };

#define PAY(o) ((o) + COM_HDR)

/* ---- device children ---- */
static void child_get_device(CPU* c) { ST32(g_device + 8, LD32(g_device + 8) + 1); ST64(A2, g_device); RET(0); }
static void priv_get(CPU* c) { if (A3) ST32(A3, 0); RET(0x887A0002); }
static void priv_set(CPU* c) { RET(0); }
static void child_common(ComKind* k)
{
    com_iunknown(k);
    k->slots[3] = child_get_device;
    k->slots[4] = priv_get;
    k->slots[5] = priv_set;
    k->slots[6] = priv_set;
}

static uint32_t fmt_info(uint32_t f, uint32_t* block) { return dxgi_format_bytes(f, block); }

static void layout_subresources(GpuRes* r)
{
    uint32_t block;
    uint32_t bpb = fmt_info(r->format, &block);
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

static uint64_t backing(GpuRes* r)
{
    if (!r->backing) {
        r->backing_raw = g_calloc(r->bytes + 128);
        r->backing = r->backing_raw ? (r->backing_raw + 63) & ~63ull : 0;
    }
    return r->backing;
}

static ComKind* res_kind(GpuRes* r) { return r->type == RES_BUFFER ? &K_Buffer : &K_Tex; }

static uint64_t new_res(GpuRes* r, uint64_t desc, uint32_t desc_size, uint64_t init)
{
    uint64_t o = com_new(res_kind(r), r, 64);
    memcpy(G2H(PAY(o)), G2H(desc), desc_size);
    if (gpu->res_create) gpu->res_create(r, init);
    if (init) {
        /* a CPU copy only where something reads it back: buffers (typed and merged views are built from it) and staging
           resources. Other textures go straight to the GPU; a guest-memory mirror of every texture ran big games out of
           memory, and D3D11 can't map them anyway (dynamic ones get memory when first mapped, for writing) */
        if (r->type == RES_BUFFER || r->usage == 3) backing(r);
        for (int s = 0; s < (r->type == RES_BUFFER ? 1 : r->nsub); s++) {
            uint64_t src = LD64(init + 16ull * s);
            if (!src) continue;
            uint32_t pitch = (uint32_t)LD32(init + 16ull * s + 8), slice = (uint32_t)LD32(init + 16ull * s + 12);
            if (!r->backing) { if (gpu->res_upload) gpu->res_upload(r, (uint32_t)s, NULL, src, pitch, slice); continue; }
            if (r->type == RES_BUFFER) memcpy(G2H(r->backing), G2H(src), (size_t)r->width);
            else {
                uint32_t rows = r->sub_slice[s] / (r->sub_pitch[s] ? r->sub_pitch[s] : 1);
                for (uint32_t y = 0; y < rows; y++)
                    memcpy(G2H(r->backing + r->sub_offset[s] + (uint64_t)y * r->sub_pitch[s]), G2H(src + (uint64_t)y * pitch), r->sub_pitch[s]);
            }
            if (gpu->res_upload) gpu->res_upload(r, (uint32_t)s, NULL, src, pitch, slice);
        }
    }
    return o;
}

static void res_get_type(CPU* c) { GpuRes* r = com_host(THIS); ST32(A2, r ? (uint64_t)r->type : 0); RET(0); }
static void res_evict_set(CPU* c) { RET(0); }
static void res_evict_get(CPU* c) { RET(0); }
static void buf_get_desc(CPU* c) { memcpy(G2H(A2), G2H(PAY(THIS)), 24); RET(0); }
static void tex_get_desc(CPU* c)
{
    GpuRes* r = com_host(THIS);
    memcpy(G2H(A2), G2H(PAY(THIS)), r->type == RES_TEX1D ? 32 : r->type == RES_TEX3D ? 36 : 44);
    RET(0);
}
static void view_get_resource(CPU* c) { uint64_t r = LD64(PAY(THIS)); if (r) ST32(r + 8, LD32(r + 8) + 1); ST64(A2, r); RET(0); }
static void view_get_desc(CPU* c) { memcpy(G2H(A2), G2H(PAY(THIS) + 8), 40); RET(0); }
static void state_get_desc(CPU* c) { GpuState* s = com_host(THIS); memcpy(G2H(A2), s->desc, s->kind == 3 ? 52 : s->kind == 2 ? 52 : s->kind == 1 ? 264 : 40); RET(0); }

/* ---- device ---- */
static void dev_create_buffer(CPU* c)
{
    uint64_t d = A2;
    GpuRes* r = calloc(1, sizeof *r);
    r->type = RES_BUFFER;
    r->width = (uint32_t)LD32(d);
    r->usage = (uint32_t)LD32(d + 4);
    r->bind = (uint32_t)LD32(d + 8);
    r->cpu = (uint32_t)LD32(d + 12);
    r->misc = (uint32_t)LD32(d + 16);
    r->stride = (uint32_t)LD32(d + 20);
    r->bytes = r->width;
    r->mips = r->array = r->depth = r->height = 1;
    r->nsub = 1;
    r->sub_pitch[0] = r->sub_slice[0] = r->width;
    if (!A4) { RET(0); return; }
    ST64(A4, new_res(r, d, 24, A3));
    RET(0);
}

static uint64_t make_tex(int type, uint64_t d, uint64_t init)
{
    GpuRes* r = calloc(1, sizeof *r);
    r->type = type;
    r->width = (uint32_t)LD32(d);
    uint32_t sz;
    if (type == RES_TEX1D) {
        r->height = r->depth = 1;
        r->mips = (uint32_t)LD32(d + 4); r->array = (uint32_t)LD32(d + 8); r->format = (uint32_t)LD32(d + 12);
        r->usage = (uint32_t)LD32(d + 16); r->bind = (uint32_t)LD32(d + 20); r->cpu = (uint32_t)LD32(d + 24); r->misc = (uint32_t)LD32(d + 28);
        r->samples = 1; sz = 32;
    } else if (type == RES_TEX2D) {
        r->height = (uint32_t)LD32(d + 4); r->depth = 1;
        r->mips = (uint32_t)LD32(d + 8); r->array = (uint32_t)LD32(d + 12); r->format = (uint32_t)LD32(d + 16);
        r->samples = (uint32_t)LD32(d + 20); r->usage = (uint32_t)LD32(d + 28); r->bind = (uint32_t)LD32(d + 32);
        r->cpu = (uint32_t)LD32(d + 36); r->misc = (uint32_t)LD32(d + 40); sz = 44;
    } else {
        r->height = (uint32_t)LD32(d + 4); r->depth = (uint32_t)LD32(d + 8);
        r->mips = (uint32_t)LD32(d + 12); r->array = 1; r->format = (uint32_t)LD32(d + 16);
        r->usage = (uint32_t)LD32(d + 20); r->bind = (uint32_t)LD32(d + 24); r->cpu = (uint32_t)LD32(d + 28); r->misc = (uint32_t)LD32(d + 32);
        r->samples = 1; sz = 36;
    }
    if (!r->mips) {
        uint32_t m = r->width > r->height ? r->width : r->height;
        if (r->depth > m) m = r->depth;
        r->mips = 1;
        while (m > 1) { m >>= 1; r->mips++; }
        ST32(d + (type == RES_TEX1D ? 4 : type == RES_TEX2D ? 8 : 12), r->mips);
    }
    if (!r->array) r->array = 1;
    layout_subresources(r);
    return new_res(r, d, sz, init);
}

static void dev_create_tex1d(CPU* c) { if (A4) ST64(A4, make_tex(RES_TEX1D, A2, A3)); RET(0); }
static void dev_create_tex2d(CPU* c) { if (A4) ST64(A4, make_tex(RES_TEX2D, A2, A3)); RET(0); }
static void dev_create_tex3d(CPU* c) { if (A4) ST64(A4, make_tex(RES_TEX3D, A2, A3)); RET(0); }

static void make_view(CPU* c, int kind, uint64_t out)
{
    uint64_t res = A2, desc = A3;
    GpuView* v = calloc(1, sizeof *v);
    v->res = com_host(res);
    v->kind = kind;
    if (desc) {
        memcpy(v->desc, G2H(desc), 40);
        v->format = (uint32_t)LD32(desc);
        v->dim = (uint32_t)LD32(desc + 4);
    } else if (v->res) {
        v->format = v->res->format;
        v->dim = 0;
    }
    if (gpu->view_create) gpu->view_create(v);
    if (!out) { RET(0); return; }
    uint64_t o = com_new(&K_View, v, 56);
    ST64(PAY(o), res);
    /* a view keeps its resource alive, as in D3D */
    if (res) ST32(res + 8, LD32(res + 8) + 1);
    if (desc) memcpy(G2H(PAY(o) + 8), G2H(desc), 40);
    ST64(out, o);
    RET(0);
}
static void dev_create_srv(CPU* c) { make_view(c, 0, A4); }
static void dev_create_uav(CPU* c) { make_view(c, 1, A4); }
static void dev_create_rtv(CPU* c) { make_view(c, 2, A4); }
static void dev_create_dsv(CPU* c) { make_view(c, 3, A4); }

static void dev_create_layout(CPU* c)
{
    GpuLayout* l = calloc(1, sizeof *l);
    l->count = (uint32_t)A3 < 32 ? (uint32_t)A3 : 32;
    for (uint32_t i = 0; i < l->count; i++) {
        uint64_t e = A2 + 32ull * i;
        snprintf(l->el[i].semantic, sizeof l->el[i].semantic, "%s", GSTR(LD64(e)));
        l->el[i].index = (uint32_t)LD32(e + 8);
        l->el[i].format = (uint32_t)LD32(e + 12);
        l->el[i].slot = (uint32_t)LD32(e + 16);
        l->el[i].offset = (uint32_t)LD32(e + 20);
        l->el[i].per_instance = (uint32_t)LD32(e + 24);
        l->el[i].step = (uint32_t)LD32(e + 28);
    }
    GpuShader* sig = calloc(1, sizeof *sig);
    sig->size = (uint32_t)ARG(5);
    sig->code = malloc(sig->size);
    memcpy(sig->code, G2H(A4), sig->size);
    l->vs_sig = sig;
    if (gpu->layout_create) gpu->layout_create(l);
    if (ARG(6)) ST64(ARG(6), com_new(&K_Layout, l, 0));
    RET(0);
}

static void make_shader(CPU* c, int stage, uint64_t out)
{
    GpuShader* s = calloc(1, sizeof *s);
    s->stage = stage;
    s->size = (uint32_t)A3;
    s->code = malloc(s->size);
    memcpy(s->code, G2H(A2), s->size);
    if (gpu->shader_create) gpu->shader_create(s);
    if (out) ST64(out, com_new(&K_Shader, s, 0));
    RET(0);
}
static void dev_create_vs(CPU* c) { make_shader(c, SH_VS, ARG(5)); }
static void dev_create_gs(CPU* c) { make_shader(c, SH_GS, ARG(5)); }
static void dev_create_gs_so(CPU* c) { make_shader(c, SH_GS, ARG(10)); }
static void dev_create_ps(CPU* c) { make_shader(c, SH_PS, ARG(5)); }
static void dev_create_hs(CPU* c) { make_shader(c, SH_HS, ARG(5)); }
static void dev_create_ds(CPU* c) { make_shader(c, SH_DS, ARG(5)); }
static void dev_create_cs(CPU* c) { make_shader(c, SH_CS, ARG(5)); }

static void make_state(CPU* c, int kind, uint32_t size)
{
    GpuState* s = calloc(1, sizeof *s);
    s->kind = kind;
    memcpy(s->desc, G2H(A2), size);
    if (gpu->state_create) gpu->state_create(s);
    if (A3) ST64(A3, com_new(&K_State, s, 0));
    RET(0);
}
static void dev_create_blend(CPU* c) { make_state(c, 1, 264); }
static void dev_create_depth(CPU* c) { make_state(c, 2, 52); }
static void dev_create_raster(CPU* c) { make_state(c, 3, 40); }
static void dev_create_sampler(CPU* c) { make_state(c, 4, 52); }
static void dev_create_query(CPU* c)
{
    uint64_t o = com_new(&K_Query, NULL, 8);
    ST32(PAY(o), LD32(A2));
    if (A3) ST64(A3, o);
    RET(0);
}
/* FORMAT_SUPPORT bit 23 (typed UAV) only where the backend can write the format */
static void dev_check_format(CPU* c)
{
    uint32_t s = 0x0FFFFFFF;
    if (gpu && gpu->uav_store && !gpu->uav_store((uint32_t)A2)) s &= ~0x800000u;
    ST32(A3, s);
    RET(0);
}
static void dev_check_msaa(CPU* c) { ST32(A4, (uint32_t)A3 <= 1 ? 1 : (uint32_t)A3 <= 8 ? 1 : 0); RET(0); }
static void dev_check_feature(CPU* c)
{
    uint32_t feature = (uint32_t)A2;
    uint64_t d = A3;
    if (feature == 2 || feature == 3) {
        /* FORMAT_SUPPORT / FORMAT_SUPPORT2 for the format in the first field: typed UAV, typed store, and on tier-2
           devices typed load (bit 0x40) */
        uint32_t f = (uint32_t)LD32(d), ok = gpu && gpu->uav_store ? (uint32_t)gpu->uav_store(f) : 0;
        if (feature == 2) ST32(d + 4, ok ? 0x0FFFFFFFu : 0x0F7FFFFFu);
        else {
            uint32_t atom = (f == 42 || f == 43) ? 0x3Fu : 0, load = (f >= 41 && f <= 43) ? 0x40u : 0;
            ST32(d + 4, ok ? (0x80u | load | atom) : atom);
        }
        RET(0);
        return;
    }
    memset(G2H(A3), 0, (size_t)A4);
    if (feature == 1 && A4 >= 4) ST32(A3, 1);
    RET(0);
}
static void dev_feature_level(CPU* c) { RET(0xB000); }
static void dev_flags(CPU* c) { RET(0); }
static void dev_removed(CPU* c) { RET(0); }
static void dev_get_context(CPU* c) { ST32(g_context + 8, LD32(g_context + 8) + 1); ST64(A2, g_context); RET(0); }
static void dev_exc(CPU* c) { RET(0); }
static void dev_qi(CPU* c)
{
    if (iid_eq(A2, IID_DXGIDevice[0]) || iid_eq(A2, IID_DXGIDevice[1])) { ST64(A3, g_dxgidevice); RET(0); return; }
    static const uint8_t unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    if (iid_eq(A2, IID_Device[0]) || iid_eq(A2, unk)) { ST64(A3, THIS); RET(0); return; }
    ST64(A3, 0);
    RET(0x80004002);
}

/* ---- immediate context ---- */
#define CTX_SET_SHADER(name, st) static void name(CPU* c) { ctx.sh[st] = com_host(A2); RET(0); }
CTX_SET_SHADER(ctx_vs, SH_VS) CTX_SET_SHADER(ctx_ps, SH_PS) CTX_SET_SHADER(ctx_gs, SH_GS) CTX_SET_SHADER(ctx_hs, SH_HS)
CTX_SET_SHADER(ctx_ds, SH_DS) CTX_SET_SHADER(ctx_cs, SH_CS)

static void set_cbs(CPU* c, int st)
{
    uint32_t start = (uint32_t)A2, n = (uint32_t)A3;
    for (uint32_t i = 0; i < n && start + i < 14; i++) {
        ctx.cb[st][start + i] = com_host(LD64(A4 + 8ull * i));
        ctx.cb_first[st][start + i] = 0;
        ctx.cb_num[st][start + i] = 0;
    }
    RET(0);
}
static void set_srvs(CPU* c, int st)
{
    uint32_t start = (uint32_t)A2, n = (uint32_t)A3;
    for (uint32_t i = 0; i < n && start + i < 128; i++) ctx.srv[st][start + i] = com_host(LD64(A4 + 8ull * i));
    RET(0);
}
static void set_samplers(CPU* c, int st)
{
    uint32_t start = (uint32_t)A2, n = (uint32_t)A3;
    for (uint32_t i = 0; i < n && start + i < 16; i++) ctx.samp[st][start + i] = com_host(LD64(A4 + 8ull * i));
    RET(0);
}
#define STAGE_SETTERS(P, st) \
    static void P##_cb(CPU* c) { set_cbs(c, st); } \
    static void P##_srv(CPU* c) { set_srvs(c, st); } \
    static void P##_samp(CPU* c) { set_samplers(c, st); }
STAGE_SETTERS(vs, SH_VS) STAGE_SETTERS(ps, SH_PS) STAGE_SETTERS(gs, SH_GS) STAGE_SETTERS(hs, SH_HS) STAGE_SETTERS(ds, SH_DS) STAGE_SETTERS(cs, SH_CS)

static void ctx_draw_indexed(CPU* c) { if (gpu->draw) gpu->draw(&ctx, (uint32_t)A2, (uint32_t)A3, (int32_t)A4, 1, 0, 1); RET(0); }
static void ctx_draw(CPU* c) { if (gpu->draw) gpu->draw(&ctx, (uint32_t)A2, (uint32_t)A3, 0, 1, 0, 0); RET(0); }
static void ctx_draw_indexed_inst(CPU* c) { if (gpu->draw) gpu->draw(&ctx, (uint32_t)A2, (uint32_t)A4, (int32_t)ARG(5), (uint32_t)A3, (uint32_t)ARG(6), 1); RET(0); }
static void ctx_draw_inst(CPU* c) { if (gpu->draw) gpu->draw(&ctx, (uint32_t)A2, (uint32_t)A4, 0, (uint32_t)A3, (uint32_t)ARG(5), 0); RET(0); }
static void ctx_dispatch(CPU* c) { if (gpu->dispatch) gpu->dispatch(&ctx, (uint32_t)A2, (uint32_t)A3, (uint32_t)A4); RET(0); }
/* indirect: the counts live in a GPU buffer; ~0 tells the backend to read them from ctx.indirect */
static void indirect(CPU* c, int kind)
{
    ctx.indirect = com_host(A2);
    ctx.indirect_off = (uint32_t)A3;
    if (ctx.indirect) {
        if (kind == 2) { if (gpu->dispatch) gpu->dispatch(&ctx, ~0u, 1, 1); }
        else if (gpu->draw) gpu->draw(&ctx, ~0u, 0, 0, 1, 0, kind);
    }
    ctx.indirect = NULL;
    RET(0);
}
static void ctx_draw_indexed_indirect(CPU* c) { indirect(c, 1); }
static void ctx_draw_indirect(CPU* c) { indirect(c, 0); }
static void ctx_dispatch_indirect(CPU* c) { indirect(c, 2); }
static void ctx_gen_mips(CPU* c) { if (gpu->gen_mips && com_host(A2)) gpu->gen_mips(com_host(A2)); RET(0); }

static void ctx_map(CPU* c)
{
    GpuRes* r = com_host(A2);
    uint32_t sub = (uint32_t)A3, type = (uint32_t)A4;
    uint64_t out = ARG(6);
    if (!r || sub >= (uint32_t)(r->nsub ? r->nsub : 1)) {
        static int warned;
        if (warned++ < 8) host_log("[d3d11] Map failed: object %llx (%s), subresource %u of %u", (unsigned long long)A2, r ? "known" : "unknown", sub, r ? r->nsub : 0);
        RET(0x80070057);
        return;
    }
    uint64_t b = backing(r);
    if (!b) {
        host_log("[d3d11] Map: no memory for a %u byte resource", (unsigned)r->bytes);
        RET(0x8007000E);
        return;
    }
    r->map_type = type;
    if ((type == 1 || type == 3) && gpu->readback) gpu->readback(r, sub);
    ST64(out, b + r->sub_offset[sub]);
    ST32(out + 8, r->sub_pitch[sub]);
    ST32(out + 12, r->sub_slice[sub]);
    RET(0);
}
static void ctx_unmap(CPU* c)
{
    GpuRes* r = com_host(A2);
    uint32_t sub = (uint32_t)A3;
    /* staging memory only goes to the GPU when it was mapped for writing */
    if (r && r->backing && gpu->res_upload && (r->usage != 3 || r->map_type == 2 || r->map_type == 3))
        gpu->res_upload(r, sub, NULL, r->backing + r->sub_offset[sub], r->sub_pitch[sub], r->sub_slice[sub]);
    RET(0);
}
static void ctx_update(CPU* c)
{
    GpuRes* r = com_host(A2);
    uint32_t sub = (uint32_t)A3;
    uint64_t box = A4, src = ARG(5);
    uint32_t pitch = (uint32_t)ARG(6), slice = (uint32_t)ARG(7);
    if (!r) { RET(0); return; }
    int32_t b[6];
    if (box) for (int i = 0; i < 6; i++) b[i] = (int32_t)LD32(box + 4ull * i);
    if (r->type == RES_BUFFER) {
        uint64_t dst = backing(r) + (box ? (uint32_t)b[0] : 0);
        uint32_t n = box ? (uint32_t)(b[3] - b[0]) : r->width;
        memcpy(G2H(dst), G2H(src), n);
    }
    if (gpu->res_upload) gpu->res_upload(r, sub, box ? b : NULL, src, pitch, slice);
    RET(0);
}

static void ctx_ia_layout(CPU* c) { ctx.layout = com_host(A2); RET(0); }
static void ctx_ia_vb(CPU* c)
{
    uint32_t start = (uint32_t)A2, n = (uint32_t)A3;
    for (uint32_t i = 0; i < n && start + i < 16; i++) {
        ctx.vb[start + i] = com_host(LD64(A4 + 8ull * i));
        ctx.vb_stride[start + i] = (uint32_t)LD32(ARG(5) + 4ull * i);
        ctx.vb_offset[start + i] = (uint32_t)LD32(ARG(6) + 4ull * i);
    }
    RET(0);
}
static void ctx_ia_ib(CPU* c) { ctx.ib = com_host(A2); ctx.ib_format = (uint32_t)A3; ctx.ib_offset = (uint32_t)A4; RET(0); }
static void ctx_ia_topology(CPU* c) { ctx.topology = (uint32_t)A2; RET(0); }

static void ctx_begin(CPU* c) { RET(0); }
/* Flush: work is submitted at present and before readbacks */
static void ctx_flush(CPU* c) { RET(0); }
/* End: a timestamp query takes a ticket for "when the GPU has done everything so far" */
static void ctx_end(CPU* c)
{
    uint64_t q = A2;
    if (q && LD32(PAY(q)) == 2 && gpu && gpu->timestamp) ST32(PAY(q) + 4, gpu->timestamp());
    RET(0);
}
static void ctx_get_data(CPU* c)
{
    uint64_t q = A2, data = A3, size = A4;
    uint32_t type = (uint32_t)LD32(PAY(q));
    if (data && size) {
        memset(G2H(data), 0, (size_t)size);
        if (type == 0 && size >= 4) ST32(data, 1);
        else if (type == 2 && size >= 8) {
            uint32_t ticket = (uint32_t)LD32(PAY(q) + 4);
            uint64_t ns = 0;
            static uint32_t last, polls, fresh;
            if (ticket && gpu && gpu->timestamp_get && !gpu->timestamp_get(ticket, &ns)) {
                /* not finished yet: S_FALSE, as D3D answers. A caller polling the same query in a loop is waiting for it:
                   then give the browser time to report the GPU's progress. One that asks each new query once (UE5's
                   clock calibration, which gives up after ten) gets the CPU clock instead, as a finished query would. */
                polls = ticket == last ? polls + 1 : 0;
                if (ticket != last) fresh++;
                last = ticket;
                if (fresh < 3) {
                    if (polls > 2) sched_sleep(c, 1);
                    RET(1);
                    return;
                }
            }
            fresh = 0;
            ST64(data, ns ? ns : host_ticks_ns());
        }
        /* timestamps are in ns (see ctx_end). They used to be the time of reading, so UE's GPU benchmark saw a zero-time
           GPU and scaled its workload up until the GPU hung and the browser lost the device (black screen). */
        else if (type == 3 && size >= 16) { ST64(data, 1000000000ull); ST32(data + 8, 0); }
        else if (type == 1 && size >= 8) ST64(data, 1000);
        else if (type == 6 && size >= 4) ST32(data, 1);
    }
    RET(0);
}
/* the guest objects behind the bound targets, for OMGetRenderTargets (UE4 asks before clearing) */
static uint64_t om_rtv[8], om_dsv;

static void set_targets(CPU* c, uint32_t n)
{
    for (uint32_t i = 0; i < 8; i++) {
        om_rtv[i] = i < n && A3 ? LD64(A3 + 8ull * i) : 0;
        ctx.rtv[i] = com_host(om_rtv[i]);
    }
    om_dsv = A4;
    ctx.dsv = com_host(A4);
}
static void ctx_om_targets(CPU* c) { set_targets(c, (uint32_t)A2); RET(0); }
static uint64_t om_uav[8];
/* OMSetRenderTargetsAndUnorderedAccessViews(n, rtvs, dsv, uav start, uav count, uavs, counts): the UAVs go to slots
   start..start+count of the pixel stage, separate from the compute stage's */
static void ctx_om_targets_uav(CPU* c)
{
    uint32_t n = (uint32_t)A2, start = (uint32_t)ARG(5), nu = (uint32_t)ARG(6);
    uint64_t uavs = ARG(7);
    if (n != 0xFFFFFFFF) set_targets(c, n);
    if (nu != 0xFFFFFFFF) {
        for (int i = 0; i < 8; i++) { om_uav[i] = 0; ctx.ps_uav[i] = NULL; }
        for (uint32_t i = 0; i < nu && start + i < 8; i++) {
            uint64_t o = uavs ? LD64(uavs + 8ull * i) : 0;
            om_uav[start + i] = o;
            ctx.ps_uav[start + i] = com_host(o);
        }
    }
    RET(0);
}
static uint64_t addref_obj(uint64_t o) { if (o) ST32(o + 8, LD32(o + 8) + 1); return o; }
static void get_targets(CPU* c)
{
    uint32_t n = (uint32_t)A2;
    for (uint32_t i = 0; i < n && A3; i++) ST64(A3 + 8ull * i, i < 8 ? addref_obj(om_rtv[i]) : 0);
    if (A4) ST64(A4, addref_obj(om_dsv));
}
static void ctx_om_get_targets(CPU* c) { get_targets(c); RET(0); }
static void ctx_om_get_targets_uav(CPU* c)
{
    get_targets(c);
    uint64_t uavs = ARG(7);
    uint32_t start = (uint32_t)ARG(5);
    for (uint32_t i = 0; i < (uint32_t)ARG(6) && uavs; i++) ST64(uavs + 8ull * i, start + i < 8 ? addref_obj(om_uav[start + i]) : 0);
    RET(0);
}
static void ctx_rs_get_viewports(CPU* c)
{
    uint32_t have = A2 ? LD32(A2) : 0;
    if (A3) { uint32_t n = have < ctx.nvp ? have : ctx.nvp; memcpy(G2H(A3), ctx.vp, 24 * n); }
    if (A2) ST32(A2, ctx.nvp);
    RET(0);
}
static void ctx_rs_get_scissors(CPU* c)
{
    uint32_t have = A2 ? LD32(A2) : 0;
    if (A3) { uint32_t n = have < ctx.nscissor ? have : ctx.nscissor; memcpy(G2H(A3), ctx.scissor, 16 * n); }
    if (A2) ST32(A2, ctx.nscissor);
    RET(0);
}
static void ctx_om_blend(CPU* c) { ctx.blend = com_host(A2); if (A3) memcpy(ctx.blend_factor, G2H(A3), 16); ctx.sample_mask = (uint32_t)A4; RET(0); }
static void ctx_om_depth(CPU* c) { ctx.depth = com_host(A2); ctx.stencil_ref = (uint32_t)A3; RET(0); }
static void ctx_rs_state(CPU* c) { ctx.raster = com_host(A2); RET(0); }
static void ctx_rs_viewports(CPU* c) { ctx.nvp = (uint32_t)A2 < 16 ? (uint32_t)A2 : 16; memcpy(ctx.vp, G2H(A3), 24 * ctx.nvp); RET(0); }
static void ctx_rs_scissors(CPU* c) { ctx.nscissor = (uint32_t)A2 < 16 ? (uint32_t)A2 : 16; memcpy(ctx.scissor, G2H(A3), 16 * ctx.nscissor); RET(0); }
static void ctx_copy_region(CPU* c)
{
    int32_t b[6];
    uint64_t box = ARG(9);
    if (box) for (int i = 0; i < 6; i++) b[i] = (int32_t)LD32(box + 4ull * i);
    if (gpu->copy) gpu->copy(com_host(A2), (uint32_t)A3, (uint32_t)A4, (uint32_t)ARG(5), (uint32_t)ARG(6), com_host(ARG(7)), (uint32_t)ARG(8), box ? b : NULL);
    RET(0);
}
static void ctx_copy(CPU* c)
{
    GpuRes *d = com_host(A2), *s = com_host(A3);
    if (d && s && s->backing) { backing(d); memcpy(G2H(d->backing), G2H(s->backing), (size_t)(d->bytes < s->bytes ? d->bytes : s->bytes)); }
    if (gpu->copy && d && s) for (int i = 0; i < (d->nsub ? d->nsub : 1); i++) gpu->copy(d, (uint32_t)i, 0, 0, 0, s, (uint32_t)i, NULL);
    RET(0);
}
/* multisampled targets are single-sample here, so resolving is a plain copy */
static void ctx_resolve(CPU* c)
{
    GpuRes *d = com_host(A2), *s = com_host(A4);
    static int off = -1;
    if (off < 0) off = host_query_flag("noresolve");
    if (off) { RET(0); return; }
    if (gpu->copy && d && s) gpu->copy(d, (uint32_t)A3, 0, 0, 0, s, (uint32_t)ARG(5), NULL);
    RET(0);
}
static void ctx_clear_rtv(CPU* c) { if (gpu->clear_rtv) gpu->clear_rtv(com_host(A2), (const float*)G2H(A3)); RET(0); }
static void ctx_clear_dsv(CPU* c) { float d; uint32_t bits = (uint32_t)c->x[3].d[0]; memcpy(&d, &bits, 4); if (gpu->clear_dsv) gpu->clear_dsv(com_host(A2), (uint32_t)A3, d, (uint8_t)ARG(5)); RET(0); }
static void ctx_clear_uav_u(CPU* c) { uint32_t v[4]; for (int i = 0; i < 4; i++) v[i] = LD32(A3 + 4ull * i); if (gpu->clear_uav) gpu->clear_uav(com_host(A2), v, 0); RET(0); }
static void ctx_clear_uav_f(CPU* c) { uint32_t v[4]; for (int i = 0; i < 4; i++) v[i] = LD32(A3 + 4ull * i); if (gpu->clear_uav) gpu->clear_uav(com_host(A2), v, 1); RET(0); }
static void ctx_noop(CPU* c) { RET(0); }
static void ctx_cs_uav(CPU* c) { uint32_t start = (uint32_t)A2, n = (uint32_t)A3; for (uint32_t i = 0; i < n && start + i < 8; i++) ctx.uav[start + i] = com_host(LD64(A4 + 8ull * i)); RET(0); }
static void ctx_get_null_out(CPU* c) { RET(0); }
/* getters we don't track still have to write their outputs: no objects bound */
static void zero_ptrs(uint64_t p, uint32_t n) { for (uint32_t i = 0; p && i < n && i < 128; i++) ST64(p + 8ull * i, 0); }
static void ctx_get_slots(CPU* c) { zero_ptrs(A4, (uint32_t)A3); RET(0); }
static void ctx_get_shader(CPU* c) { zero_ptrs(A2, 1); if (A4) ST32(A4, 0); RET(0); }
static void ctx_get_one(CPU* c) { zero_ptrs(A2, 1); RET(0); }
static void ctx_get_vbs(CPU* c) { uint32_t n = (uint32_t)A3; zero_ptrs(A4, n); for (uint32_t i = 0; i < n && i < 32; i++) { if (ARG(5)) ST32(ARG(5) + 4ull * i, 0); if (ARG(6)) ST32(ARG(6) + 4ull * i, 0); } RET(0); }
static void ctx_get_ib(CPU* c) { zero_ptrs(A2, 1); if (A3) ST32(A3, 0); if (A4) ST32(A4, 0); RET(0); }
static void ctx_get_topology(CPU* c) { if (A2) ST32(A2, ctx.topology); RET(0); }
static void ctx_get_blend(CPU* c) { zero_ptrs(A2, 1); if (A3) for (int i = 0; i < 4; i++) ST32(A3 + 4ull * i, 0x3F800000); if (A4) ST32(A4, 0xFFFFFFFF); RET(0); }
static void ctx_get_depth(CPU* c) { zero_ptrs(A2, 1); if (A3) ST32(A3, 0); RET(0); }
static void ctx_get_so(CPU* c) { zero_ptrs(A3, (uint32_t)A2); RET(0); }
static void ctx_clear_state(CPU* c) { memset(&ctx, 0, sizeof ctx); memset(om_rtv, 0, sizeof om_rtv); om_dsv = 0; RET(0); }
static void ctx_get_type(CPU* c) { RET(0); }

/* ---- DXGI ---- */
static void dxgi_get_parent(CPU* c) { ST32(g_factory + 8, LD32(g_factory + 8) + 1); ST64(A3, g_factory); RET(0); }
static void dxgidev_get_adapter(CPU* c) { ST32(g_adapter + 8, LD32(g_adapter + 8) + 1); ST64(A2, g_adapter); RET(0); }
static void factory_enum_adapters(CPU* c) { if (A2) { ST64(A3, 0); RET(0x887A0002); return; } ST32(g_adapter + 8, LD32(g_adapter + 8) + 1); ST64(A3, g_adapter); RET(0); }
static void factory_window_assoc(CPU* c) { RET(0); }
static void factory_get_window_assoc(CPU* c) { ST64(A2, 0); RET(0); }
static void factory_is_current(CPU* c) { RET(1); }
static void sc_make_backbuffer(uint32_t w, uint32_t h, uint32_t fmt);
static void factory_create_swapchain(CPU* c)
{
    uint64_t d = A3;
    sc_desc = g_calloc(80);
    memcpy(G2H(sc_desc), G2H(d), 72);
    uint32_t w = (uint32_t)LD32(d), h = (uint32_t)LD32(d + 4);
    if (!w || !h) { int cw, ch; host_canvas_size(&cw, &ch); w = (uint32_t)cw; h = (uint32_t)ch; ST32(sc_desc, w); ST32(sc_desc + 4, h); }
    host_log("[d3d11] swapchain %ux%u", w, h);
    sc_make_backbuffer(w, h, (uint32_t)LD32(d + 16));
    g_swapchain = com_new(&K_SwapChain, NULL, 0);
    ST64(A4, g_swapchain);
    RET(0);
}
static void adapter_enum_outputs(CPU* c) { if (A2) { ST64(A3, 0); RET(0x887A0002); return; } ST32(g_output + 8, LD32(g_output + 8) + 1); ST64(A3, g_output); RET(0); }
static void adapter_desc(CPU* c, int v1)
{
    uint64_t d = A2;
    memset(G2H(d), 0, v1 ? 312 : 304);
    utf8_to_wstr(gpu && gpu->name ? gpu->name : "Web GPU", d, 128);
    ST32(d + 256, 0x1234);
    ST32(d + 260, 0x5678);
    /* video memory from the build's memory limit (UE sizes its texture pool from it); 2 GB without one */
    extern int host_page_int(const char* name);
    uint64_t gpu_mb = host_page_int("uewGpuMB");
    if (!gpu_mb) gpu_mb = 2048;
    ST64(d + 272, gpu_mb << 20);
    ST64(d + 288, gpu_mb << 20);
    ST32(d + 296, 1);
    RET(0);
}
static void adapter_get_desc(CPU* c) { adapter_desc(c, 0); }
static void adapter_get_desc1(CPU* c) { adapter_desc(c, 1); }
static void adapter_check_iface(CPU* c) { if (A3) ST64(A3, 0x000B000000000000ull); RET(0); }
static void output_get_desc(CPU* c)
{
    uint64_t d = A2;
    memset(G2H(d), 0, 96);
    utf8_to_wstr("\\\\.\\DISPLAY1", d, 32);
    int w, h;
    host_canvas_size(&w, &h);
    ST32(d + 72, (uint32_t)w);
    ST32(d + 76, (uint32_t)h);
    ST32(d + 80, 1);
    ST32(d + 84, 1);
    ST64(d + 88, 0x30001);
    RET(0);
}
static const uint32_t std_modes[][2] = { { 640, 360 }, { 854, 480 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } };
static void output_modes(CPU* c)
{
    uint64_t num = A4, desc = ARG(5);
    /* standard sizes up to the canvas, then the canvas itself */
    uint32_t out_modes[8][2], n = 0;
    int cw, ch;
    host_canvas_size(&cw, &ch);
    for (int i = 0; i < 6; i++)
        if (std_modes[i][0] < (uint32_t)cw && std_modes[i][1] <= (uint32_t)ch) { out_modes[n][0] = std_modes[i][0]; out_modes[n][1] = std_modes[i][1]; n++; }
    out_modes[n][0] = (uint32_t)cw; out_modes[n][1] = (uint32_t)ch; n++;
    if (!desc) { ST32(num, n); RET(0); return; }
    uint32_t cap = (uint32_t)LD32(num);
    if (cap > n) cap = n;
    for (uint32_t i = 0; i < cap; i++) {
        uint64_t m = desc + 28ull * i;
        ST32(m, out_modes[i][0]); ST32(m + 4, out_modes[i][1]); ST32(m + 8, 60); ST32(m + 12, 1);
        ST32(m + 16, (uint32_t)A2); ST32(m + 20, 0); ST32(m + 24, 0);
    }
    ST32(num, cap);
    RET(0);
}
static void output_closest(CPU* c) { memcpy(G2H(A3), G2H(A2), 28); if (!LD32(A3)) { int w, h; host_canvas_size(&w, &h); ST32(A3, w); ST32(A3 + 4, h); } ST32(A3 + 8, 60); ST32(A3 + 12, 1); RET(0); }
static void output_vblank(CPU* c) { RET(0); }

static void sc_make_backbuffer(uint32_t w, uint32_t h, uint32_t fmt)
{
    uint64_t d = g_calloc(48);
    ST32(d, w); ST32(d + 4, h); ST32(d + 8, 1); ST32(d + 12, 1); ST32(d + 16, fmt); ST32(d + 20, 1);
    ST32(d + 28, 0); ST32(d + 32, 0x20 | 0x8);
    g_backbuffer = make_tex(RES_TEX2D, d, 0);
    ST32(g_backbuffer + 8, 1000000);
}
/* ---- object lifetime: the last Release queues buffers, textures and views; after the frame is submitted the ones
   no longer bound to the context are freed (D3D keeps bound objects alive) */
static uint64_t* doomed;
static size_t ndoomed, cap_doomed;
static void doom(uint64_t obj)
{
    if (ndoomed == cap_doomed) { cap_doomed = cap_doomed ? cap_doomed * 2 : 1024; doomed = realloc(doomed, cap_doomed * sizeof *doomed); }
    doomed[ndoomed++] = obj;
}
static int res_bound(GpuRes* r)
{
    for (int s = 0; s < SH_COUNT; s++) for (int i = 0; i < 14; i++) if (ctx.cb[s][i] == r) return 1;
    for (int i = 0; i < 16; i++) if (ctx.vb[i] == r) return 1;
    return ctx.ib == r || ctx.indirect == r;
}
static int view_bound(GpuView* v)
{
    for (int s = 0; s < SH_COUNT; s++) for (int i = 0; i < 128; i++) if (ctx.srv[s][i] == v) return 1;
    for (int i = 0; i < 8; i++) if (ctx.uav[i] == v || ctx.ps_uav[i] == v || ctx.rtv[i] == v) return 1;
    return ctx.dsv == v;
}
static void reap(void)
{
    size_t keep = 0, freed = 0;
    for (size_t i = 0; i < ndoomed; i++) {
        uint64_t o = doomed[i];
        if (LD32(o + 8)) continue;                         /* referenced again */
        ComKind* k = com_kind(o);
        if (k == &K_View) {
            GpuView* v = com_host(o);
            if (v && view_bound(v)) { doomed[keep++] = o; continue; }
            uint64_t res = LD64(PAY(o));
            if (v) { if (gpu->view_destroy) gpu->view_destroy(v); free(v); }
            com_free(o);
            freed++;
            if (res && LD32(res + 8)) { uint32_t n = LD32(res + 8) - 1; ST32(res + 8, n); if (!n) doom(res); }
        } else {
            GpuRes* r = com_host(o);
            if (r && res_bound(r)) { doomed[keep++] = o; continue; }
            if (r) {
                if (gpu->res_destroy) gpu->res_destroy(r);
                if (r->backing_raw) g_free(r->backing_raw);
                free(r);
            }
            com_free(o);
            freed++;
        }
    }
    /* resources doomed by views freed above were appended and handled in this same pass */
    ndoomed = keep;
    if (freed && gpu->forget) gpu->forget();
}

static void sc_present(CPU* c)
{
    GpuRes* bb = com_host(g_backbuffer);
    if (gpu->present) gpu->present(bb, bb ? bb->width : 0, bb ? bb->height : 0);
    reap();
    sched_frame_report();
    sched_wait_frame(c);
    RET(0);
}
static void sc_get_buffer(CPU* c) { ST32(g_backbuffer + 8, LD32(g_backbuffer + 8) + 1); ST64(A4, g_backbuffer); RET(0); }
static void sc_fullscreen(CPU* c) { RET(0); }
static void sc_get_fullscreen(CPU* c) { if (A2) ST32(A2, 0); if (A3) ST64(A3, 0); RET(0); }
static void sc_get_desc(CPU* c) { memcpy(G2H(A2), G2H(sc_desc), 72); RET(0); }
static void sc_resize(CPU* c)
{
    uint32_t w = (uint32_t)A3, h = (uint32_t)A4, fmt = (uint32_t)ARG(5);
    if (!w || !h) { int cw, ch; host_canvas_size(&cw, &ch); w = (uint32_t)cw; h = (uint32_t)ch; }
    if (!fmt) fmt = (uint32_t)LD32(sc_desc + 16);
    ST32(sc_desc, w); ST32(sc_desc + 4, h); ST32(sc_desc + 16, fmt);
    host_log("[d3d11] swapchain %ux%u", w, h);
    sc_make_backbuffer(w, h, fmt);
    if (gpu->resize) gpu->resize(w, h);
    RET(0);
}
static void sc_get_output(CPU* c) { ST32(g_output + 8, LD32(g_output + 8) + 1); ST64(A2, g_output); RET(0); }
static void sc_stats(CPU* c) { memset(G2H(A2), 0, 32); RET(0); }
static void sc_get_device(CPU* c) { ST64(A3, g_device); RET(0); }

static void setup_kinds(void)
{
    static int done;
    if (done) return;
    done = 1;
    gpu = gpu_web_backend();
    if (!gpu) gpu = gpu_null_backend();

    ComKind* k = &K_Device;
    com_iunknown(k);
    GuestFn dev[43] = { dev_qi, 0, 0, dev_create_buffer, dev_create_tex1d, dev_create_tex2d, dev_create_tex3d, dev_create_srv,
        dev_create_uav, dev_create_rtv, dev_create_dsv, dev_create_layout, dev_create_vs, dev_create_gs, dev_create_gs_so,
        dev_create_ps, dev_create_hs, dev_create_ds, dev_create_cs, 0, dev_create_blend, dev_create_depth, dev_create_raster,
        dev_create_sampler, dev_create_query, dev_create_query, 0, 0, 0, dev_check_format, dev_check_msaa, 0, 0,
        dev_check_feature, priv_get, priv_set, priv_set, dev_feature_level, dev_flags, dev_removed, dev_get_context, dev_exc, dev_exc };
    for (int i = 0; i < 43; i++) if (dev[i]) k->slots[i] = dev[i];

    k = &K_Context;
    child_common(k);
    GuestFn cx[115] = { 0 };
    cx[7] = vs_cb; cx[8] = ps_srv; cx[9] = ctx_ps; cx[10] = ps_samp; cx[11] = ctx_vs; cx[12] = ctx_draw_indexed; cx[13] = ctx_draw;
    cx[14] = ctx_map; cx[15] = ctx_unmap; cx[16] = ps_cb; cx[17] = ctx_ia_layout; cx[18] = ctx_ia_vb; cx[19] = ctx_ia_ib;
    cx[20] = ctx_draw_indexed_inst; cx[21] = ctx_draw_inst; cx[22] = gs_cb; cx[23] = ctx_gs; cx[24] = ctx_ia_topology;
    cx[25] = vs_srv; cx[26] = vs_samp; cx[27] = ctx_begin; cx[28] = ctx_end; cx[29] = ctx_get_data; cx[30] = ctx_noop;
    cx[31] = gs_srv; cx[32] = gs_samp; cx[33] = ctx_om_targets; cx[34] = ctx_om_targets_uav; cx[35] = ctx_om_blend;
    cx[36] = ctx_om_depth; cx[37] = ctx_noop; cx[38] = ctx_noop; cx[39] = ctx_draw_indexed_indirect; cx[40] = ctx_draw_indirect; cx[41] = ctx_dispatch;
    cx[42] = ctx_dispatch_indirect; cx[43] = ctx_rs_state; cx[44] = ctx_rs_viewports; cx[45] = ctx_rs_scissors; cx[46] = ctx_copy_region;
    cx[47] = ctx_copy; cx[48] = ctx_update; cx[49] = ctx_noop; cx[50] = ctx_clear_rtv; cx[51] = ctx_clear_uav_u; cx[52] = ctx_clear_uav_f;
    cx[53] = ctx_clear_dsv; cx[54] = ctx_gen_mips; cx[55] = ctx_noop; cx[56] = ctx_noop; cx[57] = ctx_resolve; cx[58] = ctx_noop;
    cx[59] = hs_srv; cx[60] = ctx_hs; cx[61] = hs_samp; cx[62] = hs_cb; cx[63] = ds_srv; cx[64] = ctx_ds; cx[65] = ds_samp;
    cx[66] = ds_cb; cx[67] = cs_srv; cx[68] = ctx_cs_uav; cx[69] = ctx_cs; cx[70] = cs_samp; cx[71] = cs_cb;
    /* 72-109: getters; 110 ClearState, 111 Flush, 112 GetType, 113 GetContextFlags, 114 FinishCommandList */
    for (int i = 72; i < 110; i++) cx[i] = ctx_get_slots;
    cx[74] = cx[76] = cx[82] = cx[98] = cx[102] = cx[107] = ctx_get_shader;
    cx[78] = cx[86] = cx[94] = ctx_get_one;
    cx[79] = ctx_get_vbs; cx[80] = ctx_get_ib; cx[83] = ctx_get_topology; cx[91] = ctx_get_blend; cx[92] = ctx_get_depth; cx[93] = ctx_get_so;
    cx[89] = ctx_om_get_targets; cx[90] = ctx_om_get_targets_uav; cx[95] = ctx_rs_get_viewports; cx[96] = ctx_rs_get_scissors;
    cx[110] = ctx_clear_state; cx[111] = ctx_flush; cx[112] = ctx_get_type; cx[113] = ctx_get_type; cx[114] = ctx_noop;
    /* unimplemented methods fall through to com_unimpl, which logs each one once */
    for (int i = 7; i < 115; i++) if (cx[i] && cx[i] != ctx_noop) k->slots[i] = cx[i];

    k = &K_DXGIDevice;
    com_iunknown(k);
    k->slots[3] = priv_set; k->slots[4] = priv_set; k->slots[5] = priv_get; k->slots[6] = dxgi_get_parent; k->slots[7] = dxgidev_get_adapter;
    for (int i = 8; i < 16; i++) k->slots[i] = ctx_noop;

    k = &K_Factory;
    com_iunknown(k);
    k->slots[3] = priv_set; k->slots[4] = priv_set; k->slots[5] = priv_get; k->slots[6] = dxgi_get_parent;
    k->slots[7] = factory_enum_adapters; k->slots[8] = factory_window_assoc; k->slots[9] = factory_get_window_assoc;
    k->slots[10] = factory_create_swapchain; k->slots[12] = factory_enum_adapters; k->slots[13] = factory_is_current;

    k = &K_Adapter;
    com_iunknown(k);
    k->slots[3] = priv_set; k->slots[4] = priv_set; k->slots[5] = priv_get; k->slots[6] = dxgi_get_parent;
    k->slots[7] = adapter_enum_outputs; k->slots[8] = adapter_get_desc; k->slots[9] = adapter_check_iface; k->slots[10] = adapter_get_desc1;

    k = &K_Output;
    com_iunknown(k);
    k->slots[3] = priv_set; k->slots[4] = priv_set; k->slots[5] = priv_get; k->slots[6] = dxgi_get_parent;
    k->slots[7] = output_get_desc; k->slots[8] = output_modes; k->slots[9] = output_closest; k->slots[10] = output_vblank;
    for (int i = 11; i < 19; i++) k->slots[i] = ctx_noop;

    k = &K_SwapChain;
    com_iunknown(k);
    k->slots[3] = priv_set; k->slots[4] = priv_set; k->slots[5] = priv_get; k->slots[6] = dxgi_get_parent; k->slots[7] = sc_get_device;
    k->slots[8] = sc_present; k->slots[9] = sc_get_buffer; k->slots[10] = sc_fullscreen; k->slots[11] = sc_get_fullscreen;
    k->slots[12] = sc_get_desc; k->slots[13] = sc_resize; k->slots[14] = ctx_noop; k->slots[15] = sc_get_output; k->slots[16] = sc_stats;
    k->slots[17] = ctx_noop;

    ComKind* children[] = { &K_Buffer, &K_Tex, &K_View, &K_Shader, &K_State, &K_Layout, &K_Query };
    for (int i = 0; i < 7; i++) child_common(children[i]);
    K_Buffer.on_zero = K_Tex.on_zero = K_View.on_zero = doom;
    K_Buffer.slots[7] = res_get_type; K_Buffer.slots[8] = res_evict_set; K_Buffer.slots[9] = res_evict_get; K_Buffer.slots[10] = buf_get_desc;
    K_Tex.slots[7] = res_get_type; K_Tex.slots[8] = res_evict_set; K_Tex.slots[9] = res_evict_get; K_Tex.slots[10] = tex_get_desc;
    K_View.slots[7] = view_get_resource; K_View.slots[8] = view_get_desc;
    K_State.slots[7] = state_get_desc;
    K_Query.slots[7] = ctx_get_type; K_Query.slots[8] = ctx_get_type; K_Query.slots[9] = ctx_noop;

    g_factory = com_new(&K_Factory, NULL, 0);
    g_adapter = com_new(&K_Adapter, NULL, 0);
    g_output = com_new(&K_Output, NULL, 0);
    g_device = com_new(&K_Device, NULL, 0);
    g_context = com_new(&K_Context, &ctx, 0);
    g_dxgidevice = com_new(&K_DXGIDevice, NULL, 0);
    ST32(g_factory + 8, 1000000);
    ST32(g_adapter + 8, 1000000);
    ST32(g_device + 8, 1000000);
    ST32(g_context + 8, 1000000);
    host_log("[d3d11] device ready (%s backend)", gpu->name);
}

HLE(CreateDXGIFactory1) { setup_kinds(); ST64(A2, g_factory); RET(0); }
HLE(CreateDXGIFactory) { setup_kinds(); ST64(A2, g_factory); RET(0); }
HLE(CreateDXGIFactory2) { setup_kinds(); ST64(A3, g_factory); RET(0); }
HLE(D3D11CreateDevice)
{
    setup_kinds();
    uint64_t pp_dev = ARG(8), p_fl = ARG(9), pp_ctx = ARG(10);
    if (pp_dev) ST64(pp_dev, g_device);
    if (p_fl) ST32(p_fl, 0xB000);
    if (pp_ctx) ST64(pp_ctx, g_context);
    RET(0);
}
HLE(D3D11CreateDeviceAndSwapChain)
{
    setup_kinds();
    uint64_t desc = ARG(8), pp_sc = ARG(9), pp_dev = ARG(10), p_fl = ARG(11), pp_ctx = ARG(12);
    if (desc && pp_sc) {
        uint64_t save[4] = { R(1), R(2), R(8), R(9) };
        R(1) = g_factory; R(2) = g_device; R(8) = desc; R(9) = pp_sc;
        factory_create_swapchain(c);
        R(1) = save[0]; R(2) = save[1]; R(8) = save[2]; R(9) = save[3];
    }
    if (pp_dev) ST64(pp_dev, g_device);
    if (p_fl) ST32(p_fl, 0xB000);
    if (pp_ctx) ST64(pp_ctx, g_context);
    RET(0);
}
