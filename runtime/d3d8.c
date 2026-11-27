/* Direct3D 8 for 32-bit Unreal Engine 2 games (UT2004's D3DDrv, Unreal II, Deus Ex: Invisible War).
   D3D8 is the same GPU model as D3D9 with different COM vtables, so this file is a forwarding layer:
   every D3D8 object owns the matching D3D9 object from d3d9.c and calls its HLE method with a synthetic
   CPU frame (d9_call). D3D8 state values that differ are translated here; the D3D9 device does the work.

   The fixed-function path is the target: caps advertise no vertex/pixel shader versions, so a game that
   honours them draws without shaders and d3d9.c generates the fixed-function shaders it needs. Declaration
   only vertex shaders (CreateVertexShader with a NULL function) are converted to D3D9 declarations; real
   D3D8 shader bytecode is handed to the D3D9 shader translator as-is. */

#include "com.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define THIS A1
#define D3D_OK 0
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_MOREDATA 0x88760869u

void d3d9_ensure(void);
uint64_t d3d9_backbuffer_raw(void);
uint64_t d3d9_autodepth_raw(void);
void hle_Direct3DCreate9(CPU* c);

static ComKind K_D8D3D, K_D8Dev, K_D8Tex, K_D8Cube, K_D8Vol, K_D8Surf, K_D8VB, K_D8IB, K_D8SC;

/* a wrapped D3D9 object: the wrapper's COM handle is what the game sees */
typedef struct { uint64_t d3d9; } D8;

static uint64_t G_d3d, G_dev;
static uint32_t G_ib_base, G_cur_vs, G_cur_ps;

/* D3D8 shader handles have bit 31 set (FVF handles leave it clear, as d3d8to9 does) */
typedef struct {
    uint32_t handle, kind;          /* 0 = vertex declaration, 1 = vertex shader, 2 = pixel shader */
    uint64_t hidden;
    uint32_t ndecl, decl[256];
} Sh8;
static Sh8 G_sh[64];
static int G_nsh;

static uint32_t d9_release(uint64_t o)
{
    if (!o) return 0;
    ComKind* k = com_kind(o);
    if (!k || !k->slots[2]) return 0;
    CPU c;
    memset(&c, 0, sizeof c);
    c.r[1] = o;
    k->slots[2](&c);
    return (uint32_t)c.r[0];
}

/* call a D3D9 HLE method: a[0] is A2, a[1] A3, a[2] A4, a[3] ARG(5), ... */
static uint64_t d9_call(uint64_t obj, int slot, const uint64_t* a, int n)
{
    ComKind* k = obj ? com_kind(obj) : NULL;
    if (!k || slot < 0 || slot >= k->nslots || !k->slots[slot]) return 0;
    CPU c;
    memset(&c, 0, sizeof c);
    c.r[1] = obj;
    if (n > 0) c.r[2] = a[0];
    if (n > 1) c.r[8] = a[1];
    if (n > 2) c.r[9] = a[2];
    uint64_t f = g_calloc(8 * 24);
    if (!f) return 0;
    for (int i = 3; i < n && i < 21; i++) ST64(f + 8ull * (i + 2), a[i]);
    c.r[4] = f;
    k->slots[slot](&c);
    g_free(f);
    return c.r[0];
}

static void d8_zero(uint64_t obj)
{
    D8* d = com_host(obj);
    if (d) {
        if (d->d3d9) d9_release(d->d3d9);
        if (obj == G_d3d) G_d3d = 0;
        if (obj == G_dev) G_dev = 0;
        free(d);
    }
    com_free(obj);
}

static uint64_t d8_wrap(ComKind* k, uint64_t hidden)
{
    D8* d = calloc(1, sizeof *d);
    if (!d) return 0;
    d->d3d9 = hidden;
    return com_new(k, d, 0);
}

static uint64_t d8_hidden(uint64_t o) { D8* d = com_host(o); return d ? d->d3d9 : 0; }
static uint32_t d9_type(uint64_t o) { return (uint32_t)d9_call(o, 10, NULL, 0); }

static uint64_t d8_wrap_tex(uint64_t o)
{
    if (!o) return 0;
    switch (d9_type(o)) {
    case 5: return d8_wrap(&K_D8Cube, o);
    case 4: return d8_wrap(&K_D8Vol, o);
    default: return d8_wrap(&K_D8Tex, o);
    }
}

static uint64_t d8_wrap_surf(uint64_t o) { return o ? d8_wrap(&K_D8Surf, o) : 0; }

/* ---------------------------------------------------------------- D3D8 caps */

static void fill_caps8(uint64_t p)
{
    if (!p) return;
    uint32_t* k = (uint32_t*)G2H(p);
    memset(k, 0, 53 * 4);
    k[0] = 1;                       /* D3DDEVTYPE_HAL */
    k[3] = 0x200A0000u;             /* CANRENDERWINDOWED | FULLSCREENGAMMA | DYNAMICTEXTURES */
    k[4] = 0x20;                    /* ALPHA_FULLSCREEN_FLIP_OR_DISCARD */
    k[5] = 0x80000001u;             /* PRESENT_INTERVAL_IMMEDIATE | ONE */
    k[6] = 0x3;
    k[7] = 0x00090400u;             /* HWTRANSFORMANDLIGHT | HWRASTERIZATION | DRAWPRIMTLVERTEX */
    k[8] = 0x00000EF2u;             /* MASKZ, CULL*, COLORWRITEENABLE, CLIPTLVERTS, TSSARGTEMP, BLENDOP */
    k[9] = 0x00373F90u;             /* ZTEST, FOG*, ANTIALIASEDGES, MIPMAPLODBIAS, ZBIAS, ANISOTROPY, WBUFFER, WFOG, ZFOG */
    k[10] = k[13] = 0xFF;           /* all depth / alpha comparisons */
    k[11] = k[12] = 0x1FFFu;        /* all blend factors */
    k[14] = 0x00084208u;            /* colour, specular, alpha and fog gouraud */
    k[15] = 0x0001EC05u;            /* perspective, alpha, projected, cube, volume, mip cube/volume */
    k[16] = k[17] = k[18] = 0x07030700u;
    k[19] = k[20] = 0x3Fu;          /* wrap, mirror, clamp, border, independent UV, mirror once */
    k[21] = 0x1Fu;
    k[22] = k[23] = 4096;
    k[24] = 2048;
    k[25] = 8192; k[26] = 8192; k[27] = 16;
    float w = 1.0e10f;
    memcpy(&k[28], &w, 4);
    k[34] = 0xFF;                   /* all stencil ops */
    k[35] = 8u | 0x00100000u;       /* 8 texture coordinates, point size */
    k[36] = 0x03FFFFFFu;            /* all texture ops through LERP */
    k[37] = 8; k[38] = 8;
    k[39] = 0x7B;                   /* texgen, material source 7, directional, positional, local viewer, tweening */
    k[40] = 8; k[41] = 6; k[42] = 4;
    k[43] = 95;                 /* MaxVertexBlendMatrixIndex: the D3D9 fixed-function palette holds 96 matrices */
    float ps = 64.0f;
    memcpy(&k[44], &ps, 4);
    k[45] = 0xFFFFF; k[46] = 0xFFFF; k[47] = 16; k[48] = 255;
    /* VertexShaderVersion, MaxVertexShaderConst, PixelShaderVersion and MaxPixelShaderValue stay zero: this
       makes the D3D8 renderer use its fixed-function path, which d3d9.c implements. */
    float pv = 1.0f;
    memcpy(&k[52], &pv, 4);
}

/* ---------------------------------------------------------------- argument counts (32-bit stdcall, this included) */

static const uint8_t A_D3D[16] = { 3, 1, 1, 2, 1, 4, 2, 4, 3, 6, 7, 6, 6, 4, 2, 7 };
static const uint8_t A_DEV[97] = {
    3, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 2, 3, 2, 5, 4, 2, 3, 2,
    8, 9, 7, 6, 6, 7, 6, 5, 6, 3, 2, 3, 2, 2, 1, 1, 7, 3, 3, 3,
    2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 1, 2, 2, 2, 2, 3, 2, 2,
    3, 3, 4, 4, 2, 4, 3, 3, 2, 2, 4, 6, 5, 9, 6, 5, 2, 2, 2, 4,
    4, 4, 4, 4, 4, 3, 3, 3, 2, 2, 2, 4, 4, 4, 4, 4, 2
};
static const uint8_t A_TEX[19] = { 3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 2, 1, 1, 3, 3, 5, 2, 2 };
static const uint8_t A_CUBE[19] = { 3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 2, 1, 1, 3, 4, 6, 3, 3 };
static const uint8_t A_VOL[18] = { 3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 2, 1, 1, 3, 5, 2, 2 };
static const uint8_t A_SURF[17] = { 3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 3, 2, 4, 1, 2, 2 };
static const uint8_t A_BUF[14] = { 3, 1, 1, 2, 5, 4, 2, 2, 1, 1, 1, 5, 1, 2 };
static const uint8_t A_SC[9] = { 3, 1, 1, 5, 4, 3, 2, 2, 2 };

/* ---------------------------------------------------------------- IUnknown / resource base */

static void d8_qi(CPU* c)
{
    ST32(THIS + 8, LD32(THIS + 8) + 1);
    STP(A3, THIS);
    RET(D3D_OK);
}
static void d8_get_device(CPU* c)
{
    if (G_dev) ST32(G_dev + 8, LD32(G_dev + 8) + 1);
    STP(A2, G_dev);
    RET(D3D_OK);
}
static void d8_private_ok(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_private_get(CPU* c) { (void)c; RET(0x88760866u); }
static void d8_priority(CPU* c) { (void)c; RET(0); }
static void d8_preload(CPU* c) { (void)c; RET(0); }
static void d8_type(CPU* c)
{
    ComKind* k = com_kind(THIS);
    uint32_t t = 0;
    if (k == &K_D8Tex) t = 3;
    else if (k == &K_D8Cube) t = 5;
    else if (k == &K_D8Vol) t = 4;
    else if (k == &K_D8Surf) t = 1;
    else if (k == &K_D8VB) t = 6;
    else if (k == &K_D8IB) t = 7;
    RET(t);
}
static void d8_res_base(ComKind* k)
{
    k->slots[0] = d8_qi;
    k->slots[3] = d8_get_device;
    k->slots[4] = d8_private_ok;
    k->slots[5] = d8_private_get;
    k->slots[6] = d8_private_ok;
    k->slots[7] = d8_priority;
    k->slots[8] = d8_priority;
    k->slots[9] = d8_preload;
    k->slots[10] = d8_type;
}

/* ---------------------------------------------------------------- IDirect3D8 */

static void d8_d3d_register(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_d3d_count(CPU* c) { (void)c; RET(1); }
static void d8_d3d_identifier(CPU* c)
{
    /* D3D8's identifier has no DeviceName[32], so D3D9's structure cannot be copied over it */
    if (!A4) { RET(D3DERR_INVALIDCALL); return; }
    uint8_t* p = (uint8_t*)G2H(A4);
    memset(p, 0, 1068);
    strcpy((char*)p, "nvd3dum.dll");
    strcpy((char*)p + 512, "NVIDIA GeForce GTX 660 (UEWASM WebGPU)");
    ST32(A4 + 1024, 0x0010000Cu);
    ST32(A4 + 1028, 0x00090012u);
    ST32(A4 + 1032, 0x10DEu);
    ST32(A4 + 1036, 0x11C0u);
    ST32(A4 + 1040, 0);
    ST32(A4 + 1044, 0xA1u);
    for (int i = 0; i < 16; i++) p[1048 + i] = (uint8_t)(0x5A + i * 7);
    ST32(A4 + 1064, 1);
    RET(D3D_OK);
}
static void d8_d3d_mode_count(CPU* c)
{
    uint64_t a[2] = { A2, 22 };     /* D3D8 has no format here; ask D3D9 for X8R8G8B8 modes */
    RET(d9_call(d8_hidden(THIS), 6, a, 2));
}
static void d8_d3d_enum(CPU* c)
{
    uint64_t a[4] = { A2, 22, A3, A4 };
    RET(d9_call(d8_hidden(THIS), 7, a, 4));
}
static void d8_d3d_display(CPU* c)
{
    uint64_t a[2] = { A2, A3 };
    RET(d9_call(d8_hidden(THIS), 8, a, 2));
}
static void d8_d3d_check_type(CPU* c)
{
    uint64_t a[5] = { A2, A3, A4, ARG(5), ARG(6) };
    RET(d9_call(d8_hidden(THIS), 9, a, 5));
}
static void d8_d3d_check_format(CPU* c)
{
    uint64_t a[6] = { A2, A3, A4, ARG(5), ARG(6), ARG(7) };
    RET(d9_call(d8_hidden(THIS), 10, a, 6));
}
static void d8_d3d_check_ms(CPU* c)
{
    /* D3D8 has no quality-levels output; D3D9's sixth argument is optional and null here */
    uint64_t a[6] = { A2, A3, A4, ARG(5), ARG(6), 0 };
    RET(d9_call(d8_hidden(THIS), 11, a, 6));
}
static void d8_d3d_check_ds(CPU* c)
{
    uint64_t a[5] = { A2, A3, A4, ARG(5), ARG(6) };
    RET(d9_call(d8_hidden(THIS), 12, a, 5));
}
static void d8_d3d_caps(CPU* c) { fill_caps8(A4); RET(D3D_OK); }
static void d8_d3d_monitor(CPU* c) { (void)c; RET(0x10001); }

/* D3D8 presentation parameters have no MultiSampleQuality: 5 DWORDs, then 8 more */
static uint64_t pp8_to_pp9(uint64_t p8)
{
    if (!p8) return 0;
    uint64_t p9 = g_calloc(56);
    if (!p9) return 0;
    for (int i = 0; i < 5; i++) ST32(p9 + 4ull * i, LD32(p8 + 4ull * i));
    ST32(p9 + 20, 0);
    for (int i = 0; i < 8; i++) ST32(p9 + 24ull + 4ull * i, LD32(p8 + 20ull + 4ull * i));
    return p9;
}

static void d8_d3d_create(CPU* c)
{
    uint64_t pp8 = ARG(6), out8 = ARG(7);
    if (!pp8 || !out8) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t pp9 = pp8_to_pp9(pp8);
    uint64_t out9 = g_alloc(4);
    if (!pp9 || !out9) { g_free(pp9); g_free(out9); RET(0x8007000Eu); return; }
    ST32(out9, 0);
    uint64_t a[6] = { A2, A3, A4, ARG(5), pp9, out9 };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 16, a, 6);
    uint64_t dev9 = LD32(out9);
    if (hr == D3D_OK && dev9) {
        G_dev = d8_wrap(&K_D8Dev, dev9);
        STP(out8, G_dev);
    } else {
        STP(out8, 0);
    }
    g_free(pp9);
    g_free(out9);
    RET(hr);
}

/* ---------------------------------------------------------------- IDirect3DDevice8 */

static void d8_dev_test(CPU* c) { RET(d9_call(d8_hidden(THIS), 3, NULL, 0)); }
static void d8_dev_mem(CPU* c) { RET(d9_call(d8_hidden(THIS), 4, NULL, 0)); }
static void d8_dev_discard(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_get_d3d(CPU* c)
{
    if (G_d3d) ST32(G_d3d + 8, LD32(G_d3d + 8) + 1);
    STP(A2, G_d3d);
    RET(D3D_OK);
}
static void d8_dev_caps(CPU* c) { fill_caps8(A2); RET(D3D_OK); }
static void d8_dev_display(CPU* c)
{
    uint64_t a[2] = { 0, A2 };
    RET(d9_call(d8_hidden(THIS), 8, a, 2));
}
static void d8_dev_creation(CPU* c)
{
    if (!A2) { RET(D3DERR_INVALIDCALL); return; }
    ST32(A2, 0);                /* AdapterOrdinal */
    ST32(A2 + 4, 1);            /* D3DDEVTYPE_HAL */
    ST32(A2 + 8, 0);            /* hFocusWindow */
    ST32(A2 + 12, 0);           /* BehaviorFlags */
    RET(D3D_OK);
}
static void d8_dev_cursor(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_cursor_pos(CPU* c) { (void)c; RET(0); }
static void d8_dev_show_cursor(CPU* c) { (void)c; RET(0); }

static void d8_dev_additional(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (A3) STP(A3, 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[2] = { A2, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 13, a, 2);
    uint64_t sc = LD32(out);
    if (hr == D3D_OK && sc) {
        uint64_t o = d8_wrap(&K_D8SC, sc);
        if (A3) STP(A3, o);
    } else if (A3) STP(A3, 0);
    g_free(out);
    RET(hr);
}

static void d8_dev_reset(CPU* c)
{
    uint64_t pp9 = pp8_to_pp9(A2);
    uint64_t a[1] = { pp9 };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 16, a, 1);
    g_free(pp9);
    G_ib_base = 0; G_cur_vs = G_cur_ps = 0;
    RET(hr);
}
static void d8_dev_present(CPU* c) { (void)c; RET(d9_call(d8_hidden(THIS), 17, NULL, 0)); }
static void d8_dev_backbuffer(CPU* c)
{
    if (!A4) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t out = g_alloc(4);
    if (!out) { STP(A4, 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[4] = { 0, A2, A3, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 18, a, 4);
    uint64_t hidden = LD32(out);
    STP(A4, hr == D3D_OK ? d8_wrap_surf(hidden) : 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_raster(CPU* c)
{
    uint64_t a[2] = { 0, A2 };
    RET(d9_call(d8_hidden(THIS), 19, a, 2));
}
static void d8_dev_gamma(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_get_gamma(CPU* c) { if (A2) memset(G2H(A2), 0, 3 * 256 * 2); RET(D3D_OK); }

static void d8_dev_create_texture(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(8)) STP(ARG(8), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[7] = { A2, A3, A4, ARG(5), ARG(6), ARG(7), out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 23, a, 7);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(8), d8_wrap_tex(hidden));
    else STP(ARG(8), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_volume(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(9)) STP(ARG(9), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[8] = { A2, A3, A4, ARG(5), ARG(6), ARG(7), ARG(8), out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 24, a, 8);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(9), d8_wrap(&K_D8Vol, hidden));
    else STP(ARG(9), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_cube(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(7)) STP(ARG(7), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[6] = { A2, A3, A4, ARG(5), ARG(6), out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 25, a, 6);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(7), d8_wrap(&K_D8Cube, hidden));
    else STP(ARG(7), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_vb(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(6)) STP(ARG(6), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[6] = { A2, A3, A4, ARG(5), out, 0 };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 26, a, 6);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(6), d8_wrap(&K_D8VB, hidden));
    else STP(ARG(6), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_ib(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(6)) STP(ARG(6), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[6] = { A2, A3, A4, ARG(5), out, 0 };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 27, a, 6);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(6), d8_wrap(&K_D8IB, hidden));
    else STP(ARG(6), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_rt(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(7)) STP(ARG(7), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[8] = { A2, A3, A4, ARG(5), 0, ARG(6), out, 0 };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 28, a, 8);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(7), d8_wrap_surf(hidden));
    else STP(ARG(7), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_ds(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(6)) STP(ARG(6), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[8] = { A2, A3, A4, ARG(5), 0, 1, out, 0 };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 29, a, 8);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(6), d8_wrap_surf(hidden));
    else STP(ARG(6), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_create_image(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!out) { if (ARG(5)) STP(ARG(5), 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[5] = { A2, A3, A4, 2, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 36, a, 5);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) STP(ARG(5), d8_wrap_surf(hidden));
    else STP(ARG(5), 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_copy_rects(CPU* c)
{
    uint64_t src = d8_hidden(A2), dst = d8_hidden(ARG(5));
    uint32_t n = (uint32_t)A4;
    if (!src || !dst) { RET(D3DERR_INVALIDCALL); return; }
    if (!n) { RET(D3D_OK); return; }
    for (uint32_t i = 0; i < n; i++) {
        uint64_t rect = A3 ? A3 + 16ull * i : 0;
        uint64_t pt = ARG(6) ? ARG(6) + 8ull * i : 0;
        uint64_t a[4] = { src, rect, dst, pt };
        d9_call(d8_hidden(THIS), 30, a, 4);
    }
    RET(D3D_OK);
}
static void d8_dev_update_texture(CPU* c)
{
    uint64_t a[2] = { d8_hidden(A2), d8_hidden(A3) };
    RET(d9_call(d8_hidden(THIS), 31, a, 2));
}
static void d8_dev_front(CPU* c) { (void)c; RET(D3DERR_INVALIDCALL); }

static void d8_dev_set_rt(CPU* c)
{
    uint64_t color = A2 ? d8_hidden(A2) : d3d9_backbuffer_raw();
    uint64_t depth = A3 ? d8_hidden(A3) : d3d9_autodepth_raw();
    uint64_t a[2] = { 0, color };
    d9_call(d8_hidden(THIS), 37, a, 2);
    uint64_t d[1] = { depth };
    d9_call(d8_hidden(THIS), 39, d, 1);
    RET(D3D_OK);
}
static void d8_dev_get_rt(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A2 || !out) { if (A2) STP(A2, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[2] = { 0, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 38, a, 2);
    uint64_t hidden = LD32(out);
    STP(A2, hr == D3D_OK ? d8_wrap_surf(hidden) : 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_get_ds(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A2 || !out) { if (A2) STP(A2, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[1] = { out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 40, a, 1);
    uint64_t hidden = LD32(out);
    STP(A2, hr == D3D_OK ? d8_wrap_surf(hidden) : 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_begin(CPU* c) { (void)c; RET(d9_call(d8_hidden(THIS), 41, NULL, 0)); }
static void d8_dev_end(CPU* c) { (void)c; RET(d9_call(d8_hidden(THIS), 42, NULL, 0)); }
static void d8_dev_clear(CPU* c)
{
    uint64_t a[6] = { A2, A3, A4, ARG(5), ARG(6), ARG(7) };
    RET(d9_call(d8_hidden(THIS), 43, a, 6));
}
static void d8_dev_set_transform(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 44, a, 2)); }
static void d8_dev_get_transform(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 45, a, 2)); }
static void d8_dev_mult_transform(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 46, a, 2)); }
static void d8_dev_set_vp(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 47, a, 1)); }
static void d8_dev_get_vp(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 48, a, 1)); }
static void d8_dev_set_mat(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 49, a, 1)); }
static void d8_dev_get_mat(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 50, a, 1)); }
static void d8_dev_set_light(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 51, a, 2)); }
static void d8_dev_get_light(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 52, a, 2)); }
static void d8_dev_light_enable(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 53, a, 2)); }
static void d8_dev_get_light_enable(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 54, a, 2)); }
static void d8_dev_set_clip_plane(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 55, a, 2)); }
static void d8_dev_get_clip_plane(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 56, a, 2)); }
static void d8_dev_set_rs(CPU* c)
{
    uint32_t s = (uint32_t)A2;
    if (s == 40) s = 176;           /* EDGEANTIALIAS -> ANTIALIASEDLINEENABLE */
    uint64_t a[2] = { s, A3 };
    RET(d9_call(d8_hidden(THIS), 57, a, 2));
}
static void d8_dev_get_rs(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 58, a, 2)); }
static void d8_dev_begin_state(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_end_state(CPU* c) { if (A2) ST32(A2, 1); RET(D3D_OK); }
static void d8_dev_apply_state(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_capture_state(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_delete_state(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_create_state(CPU* c) { if (A3) ST32(A3, 1); RET(D3D_OK); }
static void d8_dev_set_clip_status(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_get_clip_status(CPU* c) { if (A2) memset(G2H(A2), 0, 8); RET(D3D_OK); }

static int sampler_state(uint32_t ty)
{
    switch (ty) {
    case 13: return 1;              /* ADDRESSU */
    case 14: return 2;              /* ADDRESSV */
    case 15: return 4;              /* BORDERCOLOR */
    case 16: return 5;              /* MAGFILTER */
    case 17: return 6;              /* MINFILTER */
    case 18: return 7;              /* MIPFILTER */
    case 19: return 8;              /* MIPMAPLODBIAS */
    case 20: return 9;              /* MAXMIPLEVEL */
    case 21: return 10;             /* MAXANISOTROPY */
    case 25: return 3;              /* ADDRESSW */
    default: return -1;
    }
}
static uint32_t stage_state(uint32_t ty)
{
    if (ty == 26) return 25;        /* COLORARG0 */
    if (ty == 27) return 26;        /* ALPHAARG0 */
    if (ty == 28) return 27;        /* RESULTARG */
    return ty;
}

static void d8_dev_get_texture(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A3 || !out) { if (A3) STP(A3, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[2] = { A2, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 64, a, 2);
    uint64_t hidden = LD32(out);
    STP(A3, hr == D3D_OK ? d8_wrap_tex(hidden) : 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_set_texture(CPU* c)
{
    uint64_t a[2] = { A2, A3 ? d8_hidden(A3) : 0 };
    RET(d9_call(d8_hidden(THIS), 65, a, 2));
}
static void d8_dev_get_tss(CPU* c)
{
    int ss = sampler_state((uint32_t)A3);
    uint64_t a[3] = { A2, ss >= 0 ? (uint64_t)ss : A3, A4 };
    if (ss < 0) a[1] = stage_state((uint32_t)A3);
    RET(d9_call(d8_hidden(THIS), ss >= 0 ? 68 : 66, a, 3));
}
static void d8_dev_set_tss(CPU* c)
{
    int ss = sampler_state((uint32_t)A3);
    uint64_t a[3] = { A2, ss >= 0 ? (uint64_t)ss : A3, A4 };
    if (ss < 0) a[1] = stage_state((uint32_t)A3);
    RET(d9_call(d8_hidden(THIS), ss >= 0 ? 69 : 67, a, 3));
}
static void d8_dev_validate(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 70, a, 1)); }
static void d8_dev_get_info(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_set_palette(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_get_palette(CPU* c) { if (A3) memset(G2H(A3), 0, 256 * 4); RET(D3D_OK); }
static void d8_dev_set_palette_num(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_get_palette_num(CPU* c) { if (A2) ST32(A2, 0); RET(D3D_OK); }

static void d8_dev_draw(CPU* c)
{
    uint64_t a[3] = { A2, A3, A4 };
    RET(d9_call(d8_hidden(THIS), 81, a, 3));
}
static void d8_dev_draw_indexed(CPU* c)
{
    /* D3D9: type, BaseVertexIndex, MinVertexIndex, NumVertices, StartIndex, PrimitiveCount */
    uint64_t a[6] = { A2, 0, A3, A4, ARG(5), ARG(6) };
    RET(d9_call(d8_hidden(THIS), 82, a, 6));
}
static void d8_dev_draw_up(CPU* c)
{
    uint64_t a[4] = { A2, A3, A4, ARG(5) };
    RET(d9_call(d8_hidden(THIS), 83, a, 4));
}
static void d8_dev_draw_indexed_up(CPU* c)
{
    uint64_t a[8] = { A2, A3, A4, ARG(5), ARG(6), ARG(7), ARG(8), ARG(9) };
    RET(d9_call(d8_hidden(THIS), 84, a, 8));
}
static void d8_dev_process(CPU* c) { (void)c; RET(D3DERR_INVALIDCALL); }

/* ---------------------------------------------------------------- D3D8 shaders */

static Sh8* find_sh(uint32_t h)
{
    for (int i = 0; i < G_nsh; i++) if (G_sh[i].handle == h) return &G_sh[i];
    return NULL;
}

static uint32_t new_sh(uint32_t kind, uint64_t hidden)
{
    if (G_nsh >= 63) { d9_release(hidden); return 0; }
    Sh8* s = &G_sh[G_nsh++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->hidden = hidden;
    s->handle = 0x80000000u | (uint32_t)G_nsh;
    return s->handle;
}

static void del_sh(uint32_t h)
{
    Sh8* s = find_sh(h);
    if (!s) return;
    if (s->hidden) d9_release(s->hidden);
    *s = G_sh[--G_nsh];
}

/* D3D8 vertex declaration tokens -> D3D9 elements (stream, offset, type, method, usage, index) */
static uint32_t elem_size(uint32_t t)
{
    static const uint8_t sz[7] = { 4, 8, 12, 16, 4, 4, 4 };
    return t < 7 ? sz[t] : 0;
}
static uint32_t usage_of(uint32_t reg, uint32_t* index)
{
    *index = 0;
    if (reg == 0) return 0;         /* POSITION */
    if (reg == 1) return 1;         /* BLENDWEIGHT */
    if (reg == 2) return 2;         /* BLENDINDICES */
    if (reg == 3) return 3;         /* NORMAL */
    if (reg == 4) return 4;         /* PSIZE */
    if (reg == 5) return 10;        /* COLOR0 */
    if (reg == 6) { *index = 1; return 10; }
    if (reg >= 7 && reg <= 14) { *index = reg - 7; return 5; } /* TEXCOORD0-7 */
    return 5;                       /* POSITION2/other: keep it as a texcoord */
}

static int parse_decl(uint64_t p, uint8_t* e, uint32_t* count, uint32_t* toks, uint32_t* ntoks)
{
    uint32_t stream = 0, off[16];
    int n = 0, nt = 0;
    memset(off, 0, sizeof off);
    if (!p) return 0;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t t = LD32(p + 4ull * i);
        uint32_t ty = t >> 29;
        if (t == 0xFFFFFFFFu || ty == 7) break;
        if (nt < 256) toks[nt++] = t;
        if (ty == 1) { stream = t & 0xF; continue; }
        if (ty == 2) {
            if (t & 0x10000000u) { off[stream] += ((t >> 16) & 0xF) * 4u; continue; }
            uint32_t dt = (t >> 16) & 0xF, reg = t & 0xF;
            if (dt > 6 || n >= 63) continue;
            uint32_t idx, use = usage_of(reg, &idx);
            uint8_t* q = e + 8 * n++;
            /* D3DVERTEXELEMENT9: WORD stream, WORD offset, BYTE type, BYTE method, BYTE usage, BYTE index */
            q[0] = (uint8_t)(stream & 0xFF); q[1] = (uint8_t)(stream >> 8);
            q[2] = (uint8_t)(off[stream] & 0xFF); q[3] = (uint8_t)(off[stream] >> 8);
            q[4] = (uint8_t)dt; q[5] = 0; q[6] = (uint8_t)use; q[7] = (uint8_t)idx;
            off[stream] += elem_size(dt);
            continue;
        }
        if (ty == 4) { i += 4 * ((t >> 25) & 0xF); continue; } /* constant memory */
        if (ty == 5) { i += (t >> 24) & 0x1F; continue; }        /* extension */
    }
    e[8 * n] = 0xFF; e[8 * n + 1] = 0; e[8 * n + 2] = 0; e[8 * n + 3] = 0;
    e[8 * n + 4] = 0; e[8 * n + 5] = 0; e[8 * n + 6] = 0; e[8 * n + 7] = 0;
    *count = (uint32_t)n;
    *ntoks = (uint32_t)nt;
    return n > 0;
}

static void d8_dev_create_vs(CPU* c)
{
    uint64_t out = A4;
    if (!out) { RET(D3DERR_INVALIDCALL); return; }
    (void)ARG(5);                   /* D3D8 Usage; the D3D9 object has no software flag */
    uint64_t hidden = 0;
    uint32_t kind = 1;
    uint32_t toks[256];
    uint32_t ntoks = 0;
    if (A3) {
        uint64_t o = g_alloc(4);
        if (!o) { ST32(out, 0); RET(0x8007000Eu); return; }
        ST32(o, 0);
        uint64_t a[2] = { A3, o };
        uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 91, a, 2);
        hidden = LD32(o);
        g_free(o);
        if (hr != D3D_OK || !hidden) { ST32(out, 0); RET(hr); }
    } else {
        if (!A2) { ST32(out, 0); RET(D3DERR_INVALIDCALL); return; }
        uint8_t* elems = (uint8_t*)calloc(64, 8);
        if (!elems) { ST32(out, 0); RET(0x8007000Eu); return; }
        uint32_t n = 0;
        if (!parse_decl(A2, elems, &n, toks, &ntoks)) { free(elems); ST32(out, 0); RET(D3DERR_INVALIDCALL); }
        uint64_t ep = g_alloc(64 * 8);
        if (!ep) { free(elems); ST32(out, 0); RET(0x8007000Eu); return; }
        memcpy(G2H(ep), elems, 64 * 8);
        free(elems);
        uint64_t o = g_alloc(4);
        if (!o) { g_free(ep); ST32(out, 0); RET(0x8007000Eu); return; }
        ST32(o, 0);
        uint64_t a[2] = { ep, o };
        uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 86, a, 2);
        hidden = LD32(o);
        g_free(ep);
        g_free(o);
        if (hr != D3D_OK || !hidden) { ST32(out, 0); RET(hr); }
        kind = 0;
    }
    uint32_t h = new_sh(kind, hidden);
    if (!h) { ST32(out, 0); RET(D3DERR_INVALIDCALL); }
    if (kind == 0) {
        Sh8* s = find_sh(h);
        s->ndecl = ntoks;
        memcpy(s->decl, toks, sizeof(uint32_t) * ntoks);
    }
    ST32(out, h);
    RET(D3D_OK);
}

static void d8_dev_set_vs(CPU* c)
{
    uint32_t h = (uint32_t)A2;
    if (!(h & 0x80000000u)) {
        uint64_t z[1] = { 0 };
        d9_call(d8_hidden(THIS), 92, z, 1);      /* SetFVF replaces a shader */
        uint64_t a[1] = { h };
        G_cur_vs = h;
        RET(d9_call(d8_hidden(THIS), 89, a, 1));
    }
    Sh8* s = find_sh(h);
    if (!s || s->kind == 2) { RET(D3DERR_INVALIDCALL); }
    if (s->kind == 0) {
        uint64_t z[1] = { 0 };
        d9_call(d8_hidden(THIS), 92, z, 1);      /* a declaration replaces a shader */
    }
    uint64_t a[1] = { s->hidden };
    G_cur_vs = h;
    RET(d9_call(d8_hidden(THIS), s->kind == 0 ? 87 : 92, a, 1));
}
static void d8_dev_get_vs(CPU* c) { if (A2) ST32(A2, G_cur_vs); RET(D3D_OK); }
static void d8_dev_del_vs(CPU* c) { if ((uint32_t)A2 == G_cur_vs) G_cur_vs = 0; del_sh((uint32_t)A2); RET(D3D_OK); }
static void d8_dev_set_vsc(CPU* c) { uint64_t a[3] = { A2, A3, A4 }; RET(d9_call(d8_hidden(THIS), 94, a, 3)); }
static void d8_dev_get_vsc(CPU* c) { uint64_t a[3] = { A2, A3, A4 }; RET(d9_call(d8_hidden(THIS), 95, a, 3)); }
static void d8_dev_get_vs_decl(CPU* c)
{
    Sh8* s = find_sh((uint32_t)A2);
    if (!s || s->kind != 0 || !A4) { RET(D3DERR_INVALIDCALL); return; }
    uint32_t bytes = (s->ndecl + 1) * 4;
    uint32_t cap = (uint32_t)LD32(A4);
    ST32(A4, bytes);
    if (!A3) { RET(D3D_OK); return; }
    if (cap < bytes) { RET(D3DERR_MOREDATA); return; }
    if (s->ndecl) memcpy(G2H(A3), s->decl, (size_t)s->ndecl * 4);
    ST32(A3 + 4ull * s->ndecl, 0xFFFFFFFFu);
    RET(D3D_OK);
}
static void d8_dev_get_vs_func(CPU* c) { (void)c; RET(D3DERR_INVALIDCALL); }

static void d8_dev_set_stream(CPU* c)
{
    uint64_t a[4] = { A2, d8_hidden(A3), 0, A4 };
    RET(d9_call(d8_hidden(THIS), 100, a, 4));
}
static void d8_dev_get_stream(CPU* c)
{
    uint64_t out = g_alloc(4), stride = g_alloc(4);
    if (!out || !stride) { g_free(out); g_free(stride); RET(0x8007000Eu); }
    ST32(out, 0); ST32(stride, 0);
    uint64_t a[4] = { A2, out, 0, stride };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 101, a, 4);
    uint64_t hidden = LD32(out);
    if (A3) STP(A3, hidden ? d8_wrap(&K_D8VB, hidden) : 0);
    if (A4) ST32(A4, LD32(stride));
    g_free(out); g_free(stride);
    RET(hr);
}
static void d8_dev_set_indices(CPU* c)
{
    G_ib_base = (uint32_t)A3;
    uint64_t a[2] = { d8_hidden(A2), A3 };
    RET(d9_call(d8_hidden(THIS), 104, a, 2));
}
static void d8_dev_get_indices(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A2 || !out) { if (A2) STP(A2, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[1] = { out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 105, a, 1);
    uint64_t hidden = LD32(out);
    STP(A2, hidden ? d8_wrap(&K_D8IB, hidden) : 0);
    if (A3) ST32(A3, G_ib_base);
    g_free(out);
    RET(hr);
}

static void d8_dev_create_ps(CPU* c)
{
    if (!A3) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t out = g_alloc(4);
    if (!out) { ST32(A3, 0); RET(0x8007000Eu); return; }
    ST32(out, 0);
    uint64_t a[2] = { A2, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 106, a, 2);
    uint64_t hidden = LD32(out);
    if (hr == D3D_OK && hidden) {
        uint32_t h = new_sh(2, hidden);
        ST32(A3, h);
    } else ST32(A3, 0);
    g_free(out);
    RET(hr);
}
static void d8_dev_set_ps(CPU* c)
{
    uint32_t h = (uint32_t)A2;
    if (!h) { G_cur_ps = 0; RET(d9_call(d8_hidden(THIS), 107, NULL, 0)); }
    Sh8* s = find_sh(h);
    if (!s || s->kind != 2) { RET(D3DERR_INVALIDCALL); }
    uint64_t a[1] = { s->hidden };
    G_cur_ps = h;
    RET(d9_call(d8_hidden(THIS), 107, a, 1));
}
static void d8_dev_get_ps(CPU* c) { if (A2) ST32(A2, G_cur_ps); RET(D3D_OK); }
static void d8_dev_del_ps(CPU* c) { if ((uint32_t)A2 == G_cur_ps) G_cur_ps = 0; del_sh((uint32_t)A2); RET(D3D_OK); }
static void d8_dev_set_psc(CPU* c) { uint64_t a[3] = { A2, A3, A4 }; RET(d9_call(d8_hidden(THIS), 109, a, 3)); }
static void d8_dev_get_psc(CPU* c) { uint64_t a[3] = { A2, A3, A4 }; RET(d9_call(d8_hidden(THIS), 110, a, 3)); }
static void d8_dev_get_ps_func(CPU* c) { (void)c; RET(D3DERR_INVALIDCALL); }

static void d8_dev_draw_rect_patch(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_draw_tri_patch(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_dev_delete_patch(CPU* c) { (void)c; RET(D3D_OK); }

/* ---------------------------------------------------------------- texture / cube / volume / surface / buffer methods */

static void d8_tex_lod(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 11, a, 1)); }
static void d8_tex_get_lod(CPU* c) { RET(d9_call(d8_hidden(THIS), 12, NULL, 0)); }
static void d8_tex_levels(CPU* c) { RET(d9_call(d8_hidden(THIS), 13, NULL, 0)); }

static void d8_surf_desc(uint64_t src, uint64_t dst)
{
    uint32_t fmt = LD32(src), type = LD32(src + 4), usage = LD32(src + 8), pool = LD32(src + 12);
    uint32_t ms = LD32(src + 16), w = LD32(src + 24), h = LD32(src + 28);
    ST32(dst, fmt); ST32(dst + 4, type); ST32(dst + 8, usage); ST32(dst + 12, pool);
    ST32(dst + 16, w * h * 4u); ST32(dst + 20, ms); ST32(dst + 24, w); ST32(dst + 28, h);
}
static void d8_vol_desc(uint64_t src, uint64_t dst)
{
    uint32_t fmt = LD32(src), type = LD32(src + 4), usage = LD32(src + 8), pool = LD32(src + 12);
    uint32_t w = LD32(src + 16), h = LD32(src + 20), d = LD32(src + 24);
    ST32(dst, fmt); ST32(dst + 4, type); ST32(dst + 8, usage); ST32(dst + 12, pool);
    ST32(dst + 16, w * h * d * 4u); ST32(dst + 20, w); ST32(dst + 24, h); ST32(dst + 28, d);
}

static void d8_tex_level_desc(CPU* c)
{
    if (!A3) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t tmp = g_calloc(32);
    if (!tmp) { RET(0x8007000Eu); return; }
    uint64_t a[2] = { A2, tmp };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 17, a, 2);
    if (hr == D3D_OK) {
        if (com_kind(THIS) == &K_D8Vol) d8_vol_desc(tmp, A3);
        else d8_surf_desc(tmp, A3);
    }
    g_free(tmp);
    RET(hr);
}
static void d8_tex_surface(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A3 || !out) { if (A3) STP(A3, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[2] = { A2, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 18, a, 2);
    uint64_t hidden = LD32(out);
    STP(A3, hr == D3D_OK ? d8_wrap_surf(hidden) : 0);
    g_free(out);
    RET(hr);
}
static void d8_cube_surface(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A4 || !out) { if (A4) STP(A4, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[3] = { A2, A3, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 18, a, 3);
    uint64_t hidden = LD32(out);
    STP(A4, hr == D3D_OK ? d8_wrap_surf(hidden) : 0);
    g_free(out);
    RET(hr);
}
static void d8_tex_lock(CPU* c)
{
    uint64_t a[4] = { A2, A3, A4, ARG(5) };
    RET(d9_call(d8_hidden(THIS), 19, a, 4));
}
static void d8_cube_lock(CPU* c)
{
    uint64_t a[5] = { A2, A3, A4, ARG(5), ARG(6) };
    RET(d9_call(d8_hidden(THIS), 19, a, 5));
}
static void d8_tex_unlock(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 20, a, 1)); }
static void d8_cube_unlock(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 20, a, 2)); }
static void d8_tex_dirty(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_cube_dirty(CPU* c) { (void)c; RET(D3D_OK); }
static void d8_vol_lock(CPU* c)
{
    uint64_t a[4] = { A2, A3, A4, ARG(5) };
    RET(d9_call(d8_hidden(THIS), 19, a, 4));
}
static void d8_vol_unlock(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 20, a, 1)); }

static void d8_surf_get_desc(CPU* c)
{
    if (!A2) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t tmp = g_calloc(32);
    if (!tmp) { RET(0x8007000Eu); return; }
    uint64_t a[1] = { tmp };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 12, a, 1);
    if (hr == D3D_OK) d8_surf_desc(tmp, A2);
    g_free(tmp);
    RET(hr);
}
static void d8_surf_lock(CPU* c) { uint64_t a[3] = { A2, A3, A4 }; RET(d9_call(d8_hidden(THIS), 13, a, 3)); }
static void d8_surf_unlock(CPU* c) { RET(d9_call(d8_hidden(THIS), 14, NULL, 0)); }
static void d8_surf_container(CPU* c) { uint64_t a[2] = { A2, A3 }; RET(d9_call(d8_hidden(THIS), 11, a, 2)); }
static void d8_surf_dc(CPU* c) { if (A2) ST32(A2, 0); RET(D3DERR_INVALIDCALL); }
static void d8_surf_release_dc(CPU* c) { (void)c; RET(D3D_OK); }

static void d8_buf_lock(CPU* c) { uint64_t a[4] = { A2, A3, A4, ARG(5) }; RET(d9_call(d8_hidden(THIS), 11, a, 4)); }
static void d8_buf_unlock(CPU* c) { RET(d9_call(d8_hidden(THIS), 12, NULL, 0)); }
static void d8_buf_desc(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 13, a, 1)); }

/* ---------------------------------------------------------------- IDirect3DSwapChain8 */

static void d8_sc_present(CPU* c) { (void)c; RET(d9_call(d8_hidden(THIS), 3, NULL, 0)); }
static void d8_sc_raster(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 6, a, 1)); }
static void d8_sc_display(CPU* c) { uint64_t a[1] = { A2 }; RET(d9_call(d8_hidden(THIS), 7, a, 1)); }
static void d8_sc_device(CPU* c)
{
    if (G_dev) ST32(G_dev + 8, LD32(G_dev + 8) + 1);
    STP(A2, G_dev);
    RET(D3D_OK);
}
static void d8_sc_params(CPU* c)
{
    if (!A2) { RET(D3DERR_INVALIDCALL); return; }
    uint64_t tmp = g_calloc(56);
    if (!tmp) { RET(0x8007000Eu); return; }
    uint64_t a[1] = { tmp };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 9, a, 1);
    if (hr == D3D_OK) {
        for (int i = 0; i < 5; i++) ST32(A2 + 4ull * i, LD32(tmp + 4ull * i));
        for (int i = 0; i < 8; i++) ST32(A2 + 20 + 4ull * i, LD32(tmp + 24 + 4ull * i));
    }
    g_free(tmp);
    RET(hr);
}
static void d8_sc_backbuffer(CPU* c)
{
    uint64_t out = g_alloc(4);
    if (!A4 || !out) { if (A4) STP(A4, 0); g_free(out); RET(D3DERR_INVALIDCALL); return; }
    ST32(out, 0);
    uint64_t a[3] = { A2, A3, out };
    uint32_t hr = (uint32_t)d9_call(d8_hidden(THIS), 5, a, 3);
    uint64_t hidden = LD32(out);
    STP(A4, hr == D3D_OK ? d8_wrap_surf(hidden) : 0);
    g_free(out);
    RET(hr);
}

/* ---------------------------------------------------------------- setup */

static void d8_setup(void)
{
    static int done;
    if (done) return;
    done = 1;

    ComKind* all[] = { &K_D8D3D, &K_D8Dev, &K_D8Tex, &K_D8Cube, &K_D8Vol, &K_D8Surf, &K_D8VB, &K_D8IB, &K_D8SC };
    static const char* names[] = { "IDirect3D8", "IDirect3DDevice8", "IDirect3DTexture8", "IDirect3DCubeTexture8",
                                   "IDirect3DVolumeTexture8", "IDirect3DSurface8", "IDirect3DVertexBuffer8",
                                   "IDirect3DIndexBuffer8", "IDirect3DSwapChain8" };
    static const int counts[] = { 16, 97, 19, 19, 18, 17, 14, 14, 9 };
    static const uint8_t* args[] = { A_D3D, A_DEV, A_TEX, A_CUBE, A_VOL, A_SURF, A_BUF, A_BUF, A_SC };
    for (int i = 0; i < 9; i++) {
        all[i]->name = names[i];
        all[i]->nslots = counts[i];
        all[i]->on_zero = d8_zero;
        com_iunknown(all[i]);
        memcpy(all[i]->nargs, args[i], (size_t)counts[i]);
    }
    for (int i = 2; i < 8; i++) d8_res_base(all[i]);

    K_D8D3D.slots[0] = d8_qi;
    K_D8D3D.slots[3] = d8_d3d_register; K_D8D3D.slots[4] = d8_d3d_count; K_D8D3D.slots[5] = d8_d3d_identifier;
    K_D8D3D.slots[6] = d8_d3d_mode_count; K_D8D3D.slots[7] = d8_d3d_enum; K_D8D3D.slots[8] = d8_d3d_display;
    K_D8D3D.slots[9] = d8_d3d_check_type; K_D8D3D.slots[10] = d8_d3d_check_format; K_D8D3D.slots[11] = d8_d3d_check_ms;
    K_D8D3D.slots[12] = d8_d3d_check_ds; K_D8D3D.slots[13] = d8_d3d_caps; K_D8D3D.slots[14] = d8_d3d_monitor;
    K_D8D3D.slots[15] = d8_d3d_create;

    K_D8Dev.slots[0] = d8_qi;
    K_D8Dev.slots[3] = d8_dev_test; K_D8Dev.slots[4] = d8_dev_mem; K_D8Dev.slots[5] = d8_dev_discard;
    K_D8Dev.slots[6] = d8_dev_get_d3d; K_D8Dev.slots[7] = d8_dev_caps; K_D8Dev.slots[8] = d8_dev_display;
    K_D8Dev.slots[9] = d8_dev_creation; K_D8Dev.slots[10] = d8_dev_cursor; K_D8Dev.slots[11] = d8_dev_cursor_pos;
    K_D8Dev.slots[12] = d8_dev_show_cursor; K_D8Dev.slots[13] = d8_dev_additional; K_D8Dev.slots[14] = d8_dev_reset;
    K_D8Dev.slots[15] = d8_dev_present; K_D8Dev.slots[16] = d8_dev_backbuffer; K_D8Dev.slots[17] = d8_dev_raster;
    K_D8Dev.slots[18] = d8_dev_gamma; K_D8Dev.slots[19] = d8_dev_get_gamma;
    K_D8Dev.slots[20] = d8_dev_create_texture; K_D8Dev.slots[21] = d8_dev_create_volume; K_D8Dev.slots[22] = d8_dev_create_cube;
    K_D8Dev.slots[23] = d8_dev_create_vb; K_D8Dev.slots[24] = d8_dev_create_ib; K_D8Dev.slots[25] = d8_dev_create_rt;
    K_D8Dev.slots[26] = d8_dev_create_ds; K_D8Dev.slots[27] = d8_dev_create_image; K_D8Dev.slots[28] = d8_dev_copy_rects;
    K_D8Dev.slots[29] = d8_dev_update_texture; K_D8Dev.slots[30] = d8_dev_front; K_D8Dev.slots[31] = d8_dev_set_rt;
    K_D8Dev.slots[32] = d8_dev_get_rt; K_D8Dev.slots[33] = d8_dev_get_ds; K_D8Dev.slots[34] = d8_dev_begin;
    K_D8Dev.slots[35] = d8_dev_end; K_D8Dev.slots[36] = d8_dev_clear; K_D8Dev.slots[37] = d8_dev_set_transform;
    K_D8Dev.slots[38] = d8_dev_get_transform; K_D8Dev.slots[39] = d8_dev_mult_transform; K_D8Dev.slots[40] = d8_dev_set_vp;
    K_D8Dev.slots[41] = d8_dev_get_vp; K_D8Dev.slots[42] = d8_dev_set_mat; K_D8Dev.slots[43] = d8_dev_get_mat;
    K_D8Dev.slots[44] = d8_dev_set_light; K_D8Dev.slots[45] = d8_dev_get_light; K_D8Dev.slots[46] = d8_dev_light_enable;
    K_D8Dev.slots[47] = d8_dev_get_light_enable; K_D8Dev.slots[48] = d8_dev_set_clip_plane; K_D8Dev.slots[49] = d8_dev_get_clip_plane;
    K_D8Dev.slots[50] = d8_dev_set_rs; K_D8Dev.slots[51] = d8_dev_get_rs; K_D8Dev.slots[52] = d8_dev_begin_state;
    K_D8Dev.slots[53] = d8_dev_end_state; K_D8Dev.slots[54] = d8_dev_apply_state; K_D8Dev.slots[55] = d8_dev_capture_state;
    K_D8Dev.slots[56] = d8_dev_delete_state; K_D8Dev.slots[57] = d8_dev_create_state; K_D8Dev.slots[58] = d8_dev_set_clip_status;
    K_D8Dev.slots[59] = d8_dev_get_clip_status; K_D8Dev.slots[60] = d8_dev_get_texture; K_D8Dev.slots[61] = d8_dev_set_texture;
    K_D8Dev.slots[62] = d8_dev_get_tss; K_D8Dev.slots[63] = d8_dev_set_tss; K_D8Dev.slots[64] = d8_dev_validate;
    K_D8Dev.slots[65] = d8_dev_get_info; K_D8Dev.slots[66] = d8_dev_set_palette; K_D8Dev.slots[67] = d8_dev_get_palette;
    K_D8Dev.slots[68] = d8_dev_set_palette_num; K_D8Dev.slots[69] = d8_dev_get_palette_num;
    K_D8Dev.slots[70] = d8_dev_draw; K_D8Dev.slots[71] = d8_dev_draw_indexed; K_D8Dev.slots[72] = d8_dev_draw_up;
    K_D8Dev.slots[73] = d8_dev_draw_indexed_up; K_D8Dev.slots[74] = d8_dev_process; K_D8Dev.slots[75] = d8_dev_create_vs;
    K_D8Dev.slots[76] = d8_dev_set_vs; K_D8Dev.slots[77] = d8_dev_get_vs; K_D8Dev.slots[78] = d8_dev_del_vs;
    K_D8Dev.slots[79] = d8_dev_set_vsc; K_D8Dev.slots[80] = d8_dev_get_vsc; K_D8Dev.slots[81] = d8_dev_get_vs_decl;
    K_D8Dev.slots[82] = d8_dev_get_vs_func; K_D8Dev.slots[83] = d8_dev_set_stream; K_D8Dev.slots[84] = d8_dev_get_stream;
    K_D8Dev.slots[85] = d8_dev_set_indices; K_D8Dev.slots[86] = d8_dev_get_indices; K_D8Dev.slots[87] = d8_dev_create_ps;
    K_D8Dev.slots[88] = d8_dev_set_ps; K_D8Dev.slots[89] = d8_dev_get_ps; K_D8Dev.slots[90] = d8_dev_del_ps;
    K_D8Dev.slots[91] = d8_dev_set_psc; K_D8Dev.slots[92] = d8_dev_get_psc; K_D8Dev.slots[93] = d8_dev_get_ps_func;
    K_D8Dev.slots[94] = d8_dev_draw_rect_patch; K_D8Dev.slots[95] = d8_dev_draw_tri_patch; K_D8Dev.slots[96] = d8_dev_delete_patch;

    for (int i = 0; i < 3; i++) {
        ComKind* k = i == 0 ? &K_D8Tex : i == 1 ? &K_D8Cube : &K_D8Vol;
        k->slots[11] = d8_tex_lod; k->slots[12] = d8_tex_get_lod; k->slots[13] = d8_tex_levels;
        k->slots[14] = d8_tex_level_desc;
    }
    K_D8Tex.slots[15] = d8_tex_surface; K_D8Tex.slots[16] = d8_tex_lock; K_D8Tex.slots[17] = d8_tex_unlock; K_D8Tex.slots[18] = d8_tex_dirty;
    K_D8Cube.slots[15] = d8_cube_surface; K_D8Cube.slots[16] = d8_cube_lock; K_D8Cube.slots[17] = d8_cube_unlock; K_D8Cube.slots[18] = d8_cube_dirty;
    K_D8Vol.slots[15] = d8_vol_lock; K_D8Vol.slots[16] = d8_vol_unlock; K_D8Vol.slots[17] = d8_tex_dirty;
    K_D8Surf.slots[11] = d8_surf_container; K_D8Surf.slots[12] = d8_surf_get_desc; K_D8Surf.slots[13] = d8_surf_lock;
    K_D8Surf.slots[14] = d8_surf_unlock; K_D8Surf.slots[15] = d8_surf_dc; K_D8Surf.slots[16] = d8_surf_release_dc;
    K_D8VB.slots[11] = d8_buf_lock; K_D8VB.slots[12] = d8_buf_unlock; K_D8VB.slots[13] = d8_buf_desc;
    K_D8IB.slots[11] = d8_buf_lock; K_D8IB.slots[12] = d8_buf_unlock; K_D8IB.slots[13] = d8_buf_desc;
    K_D8SC.slots[3] = d8_sc_present; K_D8SC.slots[4] = d8_sc_backbuffer; K_D8SC.slots[5] = d8_sc_raster;
    K_D8SC.slots[6] = d8_sc_display; K_D8SC.slots[7] = d8_sc_device; K_D8SC.slots[8] = d8_sc_params;
}

HLE(Direct3DCreate8)
{
    d8_setup();
    if (G_d3d) {
        ST32(G_d3d + 8, LD32(G_d3d + 8) + 1);
        RET(G_d3d);
    }
    d3d9_ensure();
    CPU c9;
    memset(&c9, 0, sizeof c9);
    hle_Direct3DCreate9(&c9);
    G_d3d = d8_wrap(&K_D8D3D, c9.r[0]);
    host_log("[d3d8] Direct3D 8 compatibility layer ready (D3D9 fixed-function backend)");
    RET(G_d3d);
}

HLE(DebugSetMute) { (void)c; RET(0); }
HLE(D3D8GetSWInfo) { (void)c; RET(0); }
HLE(ValidateVertexShader) { (void)c; RET(D3D_OK); }
HLE(ValidatePixelShader) { (void)c; RET(D3D_OK); }
