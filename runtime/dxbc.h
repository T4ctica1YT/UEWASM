#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct DxSigElem {
    char name[32];
    uint32_t index, sysval, comp_type, reg, mask, rw_mask;
} DxSigElem;

typedef struct DxSig {
    DxSigElem e[32];
    int n;
} DxSig;

typedef struct DxResource {
    int slot, dim, ret_type, is_buffer, is_uav, structured, raw, stride;
} DxResource;

typedef struct DxPair {
    int tex, samp, dim, ret_type, shadow;
} DxPair;

typedef struct DxShader {
    int stage;
    char* glsl;
    DxSig in, out;
    DxResource res[128];
    int nres;
    int cb_size[16];
    DxPair pairs[64];
    int npairs;
    int uses_vertex_id, uses_instance_id;
    int ps_outputs;
    char error[256];
} DxShader;

int dxbc_translate(const uint8_t* code, size_t size, DxShader* out);
void dxbc_free(DxShader* s);

typedef struct DxSlot { int used, dim, ret, compare, buffer, raw, structured, stride, atomic, written, nofilter; } DxSlot;

typedef struct DxWgsl {
    int stage;
    char* wgsl;
    DxSig in, out;
    int cb_size[16];
    DxSlot srv[128];
    DxSlot samp[16];
    DxSlot uav[8];
    int cs[3];
    int ps_depth;
    int uses_vertex_id, uses_instance_id;
    int nout;                               /* output registers */
    int gs_in, gs_maxout, gs_topo, gs_inst; /* geometry shaders: input vertices, max output, output topology, instances */
    char interp[32];                        /* pixel shaders: D3D interpolation mode per input location (0 = linear) */
    char error[256];
} DxWgsl;

/* vs_out: for pixel shaders, the paired vertex shader's output signature (locations follow its registers).
   Bind groups: VS/CS constants 0, resources 1; PS constants 2, resources 3. */
typedef struct DxWgslOpts {
    const DxSig* vs_out;
    uint32_t depth_srv[4];
    const char* uav_format[8];
    /* lib: only declarations and <prefix>uew_body(), names prefixed, bindings from group `group`, for splicing into a compute shader */
    const char* prefix;
    int lib, group;
    /* vertex shaders: interpolation of each output location, from the pixel shader they feed (WGSL needs both to agree) */
    const char* out_interp;
    /* pixel shaders: honour the declared interpolation modes (the vertex side must then be built with out_interp) */
    int use_interp;
} DxWgslOpts;

const char* dxbc_wgsl_helpers(void);
/* location of the flat render-target-layer varying that follows a signature with SV_RenderTargetArrayIndex, or -1 */
int dxbc_layer_loc(const DxSig* s);

int dxbc_to_wgsl_ex(const uint8_t* code, size_t size, const DxWgslOpts* opt, DxWgsl* out);
int dxbc_to_wgsl(const uint8_t* code, size_t size, const DxSig* vs_out, DxWgsl* out);
int dxbc_reflect(const uint8_t* code, size_t size, DxWgsl* out);
void dxbc_wgsl_free(DxWgsl* s);
