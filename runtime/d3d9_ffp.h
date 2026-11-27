#pragma once
#include <stdint.h>

/* Fixed-function shader generation for Direct3D 9 (Unreal Engine 2's renderers when they bind no shaders).

   d3d9.c keeps the device state and fills the constants listed here; d3d9_ffp.c turns a description of the draw into a
   vs_3_0/ps_3_0 pair, which goes through the same D3D9->WGSL translator as game shaders. Keeping the generator apart
   means it can be built and exercised without the rest of the runtime. */

typedef struct {
    uint16_t stream, offset;
    uint8_t type, method, usage, index;     /* D3DDECLTYPE, D3DDECLMETHOD, D3DDECLUSAGE */
} FfpElem;

typedef struct {
    int type;                               /* D3DLIGHTTYPE: 1 point, 2 spot, 3 directional */
    float diffuse[4], specular[4], ambient[4];
    float position[3], direction[3];
    float range, falloff, atten[3], theta, phi;
} FfpLight;

#define FFP_MAX_LIGHTS 8
#define FFP_MAX_STAGES 8

typedef struct {
    int n;
    FfpElem e[64];
    uint32_t tss[FFP_MAX_STAGES][33];       /* D3D9 texture stage state */
    uint8_t texkind[FFP_MAX_STAGES];        /* 0 none, 3 2D, 6 cube, 5 volume */
    /* lighting */
    int lighting, specular, color_vertex, local_viewer, normalize_normals;
    int diffuse_src, ambient_src, specular_src, emissive_src;   /* 0 material, 1 colour0, 2 colour1 */
    int nlights;
    FfpLight light[FFP_MAX_LIGHTS];
    /* fog: 0 none, 1 exp, 2 exp2, 3 linear */
    int fog, fog_radial;
    /* vertex blending: 0 off, 1/2/3 weights, 5 four weights, 255 tweening */
    int blend, blend_indexed;
    int clip_planes;                        /* bitmask of enabled user clip planes */
} FfpState;

/* constant registers the generated shaders read (vec4 units), filled by update_ffp() in d3d9.c */
#define FFP_VS_VIEWPROJ 128
#define FFP_VS_VIEW_FWD 134
#define FFP_VS_EYE 135
#define FFP_VS_LIGHTS 136
#define FFP_VS_LIGHT_STRIDE 7
#define FFP_VS_TEXMAT 192
#define FFP_VS_VIEW 232                 /* the view matrix's rows, for camera-space coordinates and fog depth */
#define FFP_VS_TWEEN 235
#define FFP_VS_FOG 236
#define FFP_VS_AMBIENT 237
#define FFP_VS_WORLD 240
#define FFP_VS_WORLD_IT 243
#define FFP_VS_MAT_DIFFUSE 246
#define FFP_VS_MAT_AMBIENT 247
#define FFP_VS_MAT_SPECULAR 248
#define FFP_VS_MAT_EMISSIVE 249
#define FFP_VS_MVP 250
#define FFP_VS_SCREEN 254
#define FFP_VS_PALETTE 296              /* 96 world matrices of 3 rows, for vertex blending */
#define FFP_VS_PALETTE_MATRICES 96
#define FFP_PS_STAGE 240
#define FFP_PS_MATERIAL 248
#define FFP_PS_FOG 249
#define FFP_PS_CLIP 250                 /* six user clip planes at c250-c255 */

/* build the pair for this draw; the caller frees both and owns the tokens until make_sh copied them */
uint32_t* ffp_build_vs(const FfpState* f, uint32_t* bytes);
uint32_t* ffp_build_ps(const FfpState* f, uint32_t* bytes);
/* texture stages that sample a texture, for the startup log line */
int ffp_texture_stages(const FfpState* f);
