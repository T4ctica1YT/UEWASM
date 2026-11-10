#pragma once
#include "host.h"

enum { RES_BUFFER = 1, RES_TEX1D, RES_TEX2D, RES_TEX3D };
enum { SH_VS, SH_HS, SH_DS, SH_GS, SH_PS, SH_CS, SH_COUNT };

typedef struct GpuRes {
    int type;
    uint32_t width, height, depth, mips, array, format, samples, usage, bind, cpu, misc, stride;
    uint64_t bytes, backing, backing_raw;
    uint32_t sub_offset[256];
    uint32_t sub_pitch[256];
    uint32_t sub_slice[256];
    int nsub;
    uint32_t map_type;          /* D3D11_MAP of the current Map */
    void* backend;
} GpuRes;

typedef struct GpuView {
    GpuRes* res;
    int kind;
    uint32_t format, dim, first_mip, mips, first_slice, slices;
    uint8_t desc[64];
    void* backend;
} GpuView;

typedef struct GpuShader {
    int stage;
    uint8_t* code;
    uint32_t size;
    void* backend;
} GpuShader;

typedef struct GpuState {
    int kind;
    uint8_t desc[264];
    void* backend;
    int backend_nearest;
} GpuState;

typedef struct GpuLayout {
    uint32_t count;
    struct { char semantic[32]; uint32_t index, format, slot, offset, per_instance, step; } el[32];
    GpuShader* vs_sig;
    void* backend;
} GpuLayout;

typedef struct GpuViewport { float x, y, w, h, minz, maxz; } GpuViewport;

typedef struct GpuCtx {
    GpuShader* sh[SH_COUNT];
    GpuRes* cb[SH_COUNT][14];
    uint32_t cb_first[SH_COUNT][14], cb_num[SH_COUNT][14];
    GpuView* srv[SH_COUNT][128];
    GpuState* samp[SH_COUNT][16];
    GpuView* uav[8];
    GpuView* ps_uav[8];      /* pixel-shader UAVs (OMSetRenderTargetsAndUnorderedAccessViews), apart from compute ones */
    GpuRes* vb[16];
    uint32_t vb_stride[16], vb_offset[16];
    GpuRes* ib;
    uint32_t ib_format, ib_offset, topology;
    GpuLayout* layout;
    GpuView* rtv[8];
    GpuView* dsv;
    GpuState *blend, *depth, *raster;
    float blend_factor[4];
    uint32_t sample_mask, stencil_ref;
    GpuViewport vp[16];
    uint32_t nvp;
    int32_t scissor[16][4];
    uint32_t nscissor;
    GpuRes* indirect;           /* arguments buffer of the current indirect draw or dispatch */
    uint32_t indirect_off;
} GpuCtx;

typedef struct GpuBackend {
    const char* name;
    void (*res_create)(GpuRes* r, uint64_t init);
    void (*res_upload)(GpuRes* r, uint32_t sub, const int32_t* box, uint64_t src, uint32_t pitch, uint32_t slice);
    void (*res_destroy)(GpuRes* r);
    void (*view_create)(GpuView* v);
    void (*shader_create)(GpuShader* s);
    void (*state_create)(GpuState* s);
    void (*layout_create)(GpuLayout* l);
    void (*clear_rtv)(GpuView* v, const float* rgba);
    void (*clear_dsv)(GpuView* v, uint32_t flags, float depth, uint8_t stencil);
    void (*draw)(GpuCtx* c, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, int indexed);
    void (*dispatch)(GpuCtx* c, uint32_t x, uint32_t y, uint32_t z);
    void (*copy)(GpuRes* dst, uint32_t dsub, uint32_t dx, uint32_t dy, uint32_t dz, GpuRes* src, uint32_t ssub, const int32_t* box);
    void (*readback)(GpuRes* r, uint32_t sub);
    void (*present)(GpuRes* backbuffer, uint32_t w, uint32_t h);
    void (*resize)(uint32_t w, uint32_t h);
    void (*clear_uav)(GpuView* v, const uint32_t* values, int is_float);
    void (*gen_mips)(GpuView* v);
    void (*view_destroy)(GpuView* v);
    void (*forget)(void);            /* objects were freed: drop anything that compares their pointers */
    /* GPU timestamps: submit what is pending and return a ticket; the time (ns) once the GPU has finished it, else 0 */
    uint32_t (*timestamp)(void);
    int (*timestamp_get)(uint32_t ticket, uint64_t* ns);
} GpuBackend;

extern GpuBackend* gpu;
GpuBackend* gpu_null_backend(void);
GpuBackend* gpu_web_backend(void);
uint32_t dxgi_format_bytes(uint32_t fmt, uint32_t* block);
