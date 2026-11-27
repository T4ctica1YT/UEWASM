/* Fixed-function vertex and pixel shaders for Direct3D 9, as vs_3_0/ps_3_0 bytecode.

   The device (d3d9.c) describes a draw with FfpState: the vertex declaration, the texture stage state, the lighting and
   fog render state, the vertex-blend mode and which textures the stages sample. This turns that into a shader pair the
   D3D9->WGSL translator accepts, so fixed-function draws take exactly the same path as a game's own shaders.

   Covered:
   - world/view/projection transforms, XYZRHW screen-space vertices and tweening;
   - hardware vertex blending (XYZB weights, indexed or consecutive matrices), with normals skinned the same way;
   - per-vertex lighting: up to eight point, spot or directional lights, material and global ambient, emissive,
     diffuse/ambient/specular/emissive material sources from vertex colours, LOCALVIEWER and SPECULARENABLE;
   - texture-coordinate generation (camera-space normal, position, reflection vector, sphere map), texture transforms
     with 1-4 coordinates, and projected transforms (divided per vertex);
   - fog (linear, exponential, exponential squared) as a per-vertex factor blended in the pixel shader;
   - user clip planes, tested against the world position in the pixel shader;
   - 2D, cube and volume texture stages, and the common colour/alpha stage operations including the blend and
     multiply-add ones.

   The constants each generated shader reads are listed in d3d9_ffp.h; d3d9.c fills them per draw. */

#include "d3d9_ffp.h"

#include <stdlib.h>
#include <string.h>

/* ---- source and destination tokens ---- */
#define S_T(n) (0x80E40000u | (uint32_t)(n))
#define S_TX(n) (0x80000000u | (uint32_t)(n))
#define S_TY(n) (0x80550000u | (uint32_t)(n))
#define S_TZ(n) (0x80AA0000u | (uint32_t)(n))
#define S_TW(n) (0x80FF0000u | (uint32_t)(n))
#define S_IN(n) (0x90E40000u | (uint32_t)(n))
#define S_INX(n) (0x90000000u | (uint32_t)(n))
#define S_INY(n) (0x90550000u | (uint32_t)(n))
#define S_INZ(n) (0x90AA0000u | (uint32_t)(n))
#define S_INW(n) (0x90FF0000u | (uint32_t)(n))
#define S_CONST(n) (0xA0E40000u | (uint32_t)(n))
#define S_CX(n) (0xA0000000u | (uint32_t)(n))
#define S_CY(n) (0xA0550000u | (uint32_t)(n))
#define S_CZ(n) (0xA0AA0000u | (uint32_t)(n))
#define S_CW(n) (0xA0FF0000u | (uint32_t)(n))
#define S_CONST_NEG(n) (S_CONST(n) | (1u << 24))
#define S_CONST_NEGY(n) (S_CY(n) | (1u << 24))
#define S_T_NEG(n) (S_T(n) | (1u << 24))

#define D_T(n, m) (0x80000000u | ((uint32_t)(m) << 16) | (uint32_t)(n))
#define D_SAT(n, m) (D_T(n, m) | (1u << 20))
#define D_OUT(slot, m) (0xE0000000u | ((uint32_t)(m) << 16) | (uint32_t)(slot))
#define D_OC(n) (0x80000000u | (0xFu << 16) | (uint32_t)(8 + (n)))
#define D_ADDR(m) (0x80000000u | (3u << 28) | ((uint32_t)(m) << 16) | 3u)
#define MASK_X 1u
#define MASK_Y 2u
#define MASK_Z 4u
#define MASK_W 8u
#define MASK_XY 3u
#define MASK_XYZ 7u
#define MASK_ALL 15u

/* opcodes */
enum {
    F_MOV = 1, F_ADD = 2, F_SUB = 3, F_MAD = 4, F_MUL = 5, F_RCP = 6, F_RSQ = 7, F_DP3 = 8, F_DP4 = 9,
    F_MIN = 10, F_MAX = 11, F_SLT = 12, F_EXP = 14, F_LOG = 15, F_LRP = 18,
    F_M4X3 = 21, F_MOVA = 46, F_TEXKILL = 65
};

/* literal constant registers the generated shaders define themselves */
enum {
    L_ZERO = 0, L_ONE, L_HALF, L_NEG, L_TWO, L_THREE, L_LOG2E, L_PALETTE_STEP, L_EPS
};

typedef struct { uint32_t* t; int n, cap, bad; } TB;

static void tb_op(TB* b, uint32_t v)
{
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 1024;
        uint32_t* t = realloc(b->t, (size_t)cap * 4);
        if (!t) { b->bad = 1; return; }
        b->t = t;
        b->cap = cap;
    }
    b->t[b->n++] = v;
}

/* instruction tokens: the length counts the parameters after the instruction token */
static void op2(TB* b, uint32_t op, uint32_t d, uint32_t s0) { tb_op(b, 0x02000000u | op); tb_op(b, d); tb_op(b, s0); }
static void op3(TB* b, uint32_t op, uint32_t d, uint32_t s0, uint32_t s1)
{
    tb_op(b, 0x03000000u | op); tb_op(b, d); tb_op(b, s0); tb_op(b, s1);
}
static void op4(TB* b, uint32_t op, uint32_t d, uint32_t s0, uint32_t s1, uint32_t s2)
{
    tb_op(b, 0x04000000u | op); tb_op(b, d); tb_op(b, s0); tb_op(b, s1); tb_op(b, s2);
}
/* a matrix instruction reading c[a0.x + base]; mova a0.x must have run first */
static void op_matrix(TB* b, uint32_t op, uint32_t d, uint32_t s0, uint32_t base)
{
    tb_op(b, 0x04000000u | op); tb_op(b, d); tb_op(b, s0);
    tb_op(b, S_CONST(base) | (1u << 13));
    tb_op(b, 0x80000003u);                          /* address register a0.x */
}
/* dp3 reading c[a0.x + base]: the upper 3x3 of a blending matrix, without its translation */
static void op_dp3_rel(TB* b, uint32_t d, uint32_t s0, uint32_t base)
{
    tb_op(b, 0x04000000u | F_DP3); tb_op(b, d); tb_op(b, s0);
    tb_op(b, S_CONST(base) | (1u << 13));
    tb_op(b, 0x80000003u);
}
static void dcl(TB* b, uint32_t usage, uint32_t index, uint32_t reg)
{
    tb_op(b, 0x0200001F);
    tb_op(b, 0x80000000u | (usage & 0x1F) | ((index & 0xF) << 16));
    tb_op(b, reg);
}
static void def(TB* b, uint32_t reg, float x, float y, float z, float w)
{
    float v[4] = { x, y, z, w };
    tb_op(b, 0x05000051);
    tb_op(b, 0xA00F0000u | (reg & 0xFF));
    for (int i = 0; i < 4; i++) { uint32_t u; memcpy(&u, &v[i], 4); tb_op(b, u); }
}

/* a constant register's component, broadcast */
static uint32_t c_comp(uint32_t reg, int k)
{
    static const uint32_t sw[4] = { 0xA0000000u, 0xA0550000u, 0xA0AA0000u, 0xA0FF0000u };
    return sw[k & 3] | (reg & 0xFF);
}
static uint32_t t_comp(uint32_t reg, int k)
{
    static const uint32_t sw[4] = { 0x80000000u, 0x80550000u, 0x80AA0000u, 0x80FF0000u };
    return sw[k & 3] | (reg & 0xFF);
}
static uint32_t in_comp(uint32_t reg, int k)
{
    static const uint32_t sw[4] = { 0x90000000u, 0x90550000u, 0x90AA0000u, 0x90FF0000u };
    return sw[k & 3] | (reg & 0xFF);
}

/* ---- the draw, as the generator sees it ---- */

typedef struct {
    int pos, pos_usage, normal, col[2], tc[8], weight, index;
    int out_tc[8];
    int out_col0, out_col1, out_fog, out_clip;
    int lit, skin, weights, blend_mats, tween, xyzrhw;
} FF;

static void scan(const FfpState* f, FF* ff)
{
    memset(ff, 0, sizeof *ff);
    ff->pos = ff->normal = ff->weight = ff->index = -1;
    ff->col[0] = ff->col[1] = -1;
    for (int i = 0; i < 8; i++) ff->tc[i] = -1;
    for (int i = 0; i < 8; i++) ff->out_tc[i] = -1;
    for (int i = 0; i < f->n; i++) {
        const FfpElem* e = &f->e[i];
        if ((e->usage == 0 || e->usage == 9) && ff->pos < 0) { ff->pos = i; ff->pos_usage = e->usage; }
        else if (e->usage == 3 && ff->normal < 0) ff->normal = i;
        else if (e->usage == 5 && e->index < 8 && ff->tc[e->index] < 0) ff->tc[e->index] = i;
        else if (e->usage == 10 && e->index < 2 && ff->col[e->index] < 0) ff->col[e->index] = i;
        else if (e->usage == 1 && ff->weight < 0) ff->weight = i;
        else if (e->usage == 2 && ff->index < 0) ff->index = i;
    }
    ff->xyzrhw = ff->pos_usage == 9;
    ff->tween = f->blend == 255 && !ff->xyzrhw;
    ff->skin = !ff->xyzrhw && !ff->tween && f->blend > 0 && f->blend != 255 && ff->weight >= 0;
    /* D3DVBF_1WEIGHTS blends two matrices with one weight, 2WEIGHTS three, 3WEIGHTS four (the last weight is
       1 - the sum of the others); 0WEIGHTS uses one matrix with a weight of 1 */
    ff->weights = f->blend >= 1 && f->blend <= 3 ? f->blend : 0;
    ff->blend_mats = ff->weights > 0 ? ff->weights + 1 : 1;   /* 0WEIGHTS uses one matrix with a weight of 1 */
    ff->lit = f->lighting && !ff->xyzrhw && ff->normal >= 0;
    /* the vertex and pixel shaders must number their output/input registers the same way: include only the texture
       coordinates and colours the declaration really has */
    int used[8];
    for (int i = 0; i < 8; i++) used[i] = ff->tc[i] >= 0;
    int slot = 1;
    for (int i = 0; i < 8; i++) if (used[i]) ff->out_tc[i] = slot++;
    ff->out_col0 = ((f->color_vertex && ff->col[0] >= 0) || ff->lit) ? slot++ : -1;
    ff->out_col1 = ((f->color_vertex && ff->col[1] >= 0) || (ff->lit && f->specular)) ? slot++ : -1;
    ff->out_fog = f->fog ? slot++ : -1;
    ff->out_clip = f->clip_planes ? slot++ : -1;
}

/* the texture-coordinate index and generated-coordinate mode of a stage */
static int stage_tc(const FfpState* f, int s, int* gen)
{
    uint32_t v = f->tss[s][11];
    int idx = (int)(v & 0xF);
    *gen = (int)((v >> 16) & 0xF);
    if (idx > 7) idx = 0;
    return idx;
}

/* ================================================================ vertex shader */

static void vs_normalize(TB* b, int r)
{
    op3(b, F_DP3, D_T(r, MASK_W), S_T(r), S_T(r));
    op2(b, F_RSQ, D_T(r, MASK_W), S_TW(r));
    op3(b, F_MUL, D_T(r, MASK_XYZ), S_T(r), S_TW(r));
}

/* the blend weights in r8: the vertex's explicit ones, then the derived last one (1 - their sum) */
static void vs_blend_weights(TB* b, const FF* ff)
{
    int w = ff->weights;
    if (w <= 0) { op2(b, F_MOV, D_T(8, MASK_X), S_CX(L_ONE)); return; }
    for (int k = 0; k < w; k++) op2(b, F_MOV, D_T(8, 1u << k), in_comp((uint32_t)ff->weight, k));
    op2(b, F_MOV, D_T(8, 1u << w), S_CX(L_ONE));
    for (int k = 0; k < w; k++) op3(b, F_SUB, D_T(8, 1u << w), t_comp(8, w), t_comp(8, k));
}

/* point a0.x at the k-th blending matrix (3 rows per matrix); the index comes from the vertex when indexed */
static void vs_blend_matrix(TB* b, const FF* ff, int k)
{
    if (ff->index >= 0 && k < 4) {
        op2(b, F_MOV, D_T(9, MASK_X), in_comp((uint32_t)ff->index, k));
        op3(b, F_MUL, D_T(9, MASK_Y), S_TX(9), S_CX(L_THREE));
    } else op2(b, F_MOV, D_T(9, MASK_Y), c_comp(L_PALETTE_STEP, k));
    op2(b, F_MOVA, D_ADDR(MASK_X), S_TY(9));
}

static void vs_world_position(TB* b, const FF* ff)
{
    if (ff->tween) {
        op_matrix(b, F_M4X3, D_T(0, MASK_XYZ), S_IN(ff->pos), FFP_VS_WORLD);
        op_matrix(b, F_M4X3, D_T(9, MASK_XYZ), S_IN(ff->pos), FFP_VS_WORLD + 3);
        op3(b, F_SUB, D_T(9, MASK_XYZ), S_T(9), S_T(0));
        op4(b, F_MAD, D_T(0, MASK_XYZ), S_T(9), S_CX(FFP_VS_TWEEN), S_T(0));
    } else if (ff->skin) {
        vs_blend_weights(b, ff);
        op2(b, F_MOV, D_T(0, MASK_ALL), S_CX(L_ZERO));
        for (int k = 0; k < ff->blend_mats; k++) {
            vs_blend_matrix(b, ff, k);
            op_matrix(b, F_M4X3, D_T(10, MASK_XYZ), S_IN(ff->pos), FFP_VS_PALETTE);
            op3(b, F_MUL, D_T(10, MASK_XYZ), S_T(10), t_comp(8, k));
            op3(b, F_ADD, D_T(0, MASK_XYZ), S_T(0), S_T(10));
        }
        op2(b, F_MOV, D_T(0, MASK_W), S_CX(L_ONE));
    } else {
        op3(b, F_DP4, D_T(0, MASK_X), S_IN(ff->pos), S_CONST(FFP_VS_WORLD));
        op3(b, F_DP4, D_T(0, MASK_Y), S_IN(ff->pos), S_CONST(FFP_VS_WORLD + 1));
        op3(b, F_DP4, D_T(0, MASK_Z), S_IN(ff->pos), S_CONST(FFP_VS_WORLD + 2));
        op2(b, F_MOV, D_T(0, MASK_W), S_CX(L_ONE));
    }
}

/* world-space normal in r1, normalised */
static void vs_normal(TB* b, const FF* ff)
{
    if (ff->skin) {
        vs_blend_weights(b, ff);
        op2(b, F_MOV, D_T(1, MASK_ALL), S_CX(L_ZERO));
        for (int k = 0; k < ff->blend_mats; k++) {
            vs_blend_matrix(b, ff, k);
            op_dp3_rel(b, D_T(10, MASK_X), S_IN(ff->normal), FFP_VS_PALETTE);
            op_dp3_rel(b, D_T(10, MASK_Y), S_IN(ff->normal), FFP_VS_PALETTE + 1);
            op_dp3_rel(b, D_T(10, MASK_Z), S_IN(ff->normal), FFP_VS_PALETTE + 2);
            op3(b, F_MUL, D_T(10, MASK_XYZ), S_T(10), t_comp(8, k));
            op3(b, F_ADD, D_T(1, MASK_XYZ), S_T(1), S_T(10));
        }
    } else {
        op3(b, F_DP3, D_T(1, MASK_X), S_IN(ff->normal), S_CONST(FFP_VS_WORLD_IT));
        op3(b, F_DP3, D_T(1, MASK_Y), S_IN(ff->normal), S_CONST(FFP_VS_WORLD_IT + 1));
        op3(b, F_DP3, D_T(1, MASK_Z), S_IN(ff->normal), S_CONST(FFP_VS_WORLD_IT + 2));
    }
    vs_normalize(b, 1);
}

/* one light: adds its diffuse, ambient and specular contributions to r2/r3 */
static void vs_light(TB* b, int C, const FfpLight* L)
{
    if (L->type == 3) {
        op2(b, F_MOV, D_T(4, MASK_XYZ), S_CONST_NEG(C + 4));
        op2(b, F_MOV, D_T(5, MASK_X), S_CX(L_ONE));
    } else {
        op3(b, F_SUB, D_T(4, MASK_XYZ), S_CONST(C + 3), S_T(0));
        op3(b, F_DP3, D_T(5, MASK_Y), S_T(4), S_T(4));
        op2(b, F_RSQ, D_T(5, MASK_Z), S_TY(5));
        op3(b, F_MUL, D_T(4, MASK_XYZ), S_T(4), S_TZ(5));
        op3(b, F_MUL, D_T(5, MASK_W), S_TY(5), S_TZ(5));              /* d */
        op3(b, F_MUL, D_T(5, MASK_X), S_TY(5), S_CW(C + 4));          /* a2*d^2 */
        op4(b, F_MAD, D_T(5, MASK_X), S_TW(5), S_CW(C + 3), S_TX(5)); /* a1*d */
        op3(b, F_ADD, D_T(5, MASK_X), S_TX(5), S_CW(C + 2));          /* a0 */
        op2(b, F_RCP, D_T(5, MASK_X), S_TX(5));
        op3(b, F_SLT, D_T(6, MASK_X), S_TW(5), S_CW(C + 1));          /* d < Range */
        op3(b, F_SUB, D_T(6, MASK_Y), S_CX(L_ONE), S_CW(C + 5));
        op4(b, F_MAD, D_T(6, MASK_X), S_TX(6), S_CW(C + 5), S_TY(6));
        op3(b, F_MUL, D_T(5, MASK_X), S_TX(5), S_TX(6));
        if (L->type == 2) {
            op3(b, F_DP3, D_T(6, MASK_X), S_T(4), S_CONST(C + 4));
            op3(b, F_MUL, D_T(6, MASK_X), S_TX(6), S_CX(L_NEG));      /* -N.L */
            op3(b, F_MAX, D_T(6, MASK_X), S_TX(6), S_CX(L_ZERO));
            op3(b, F_ADD, D_T(6, MASK_X), S_TX(6), S_CONST_NEGY(C + 5));
            op3(b, F_MUL, D_T(6, MASK_X), S_TX(6), S_CZ(C + 5));
            op3(b, F_MAX, D_T(6, MASK_X), S_TX(6), S_CX(L_ZERO));
            op2(b, F_LOG, D_T(6, MASK_Y), S_TX(6));
            op3(b, F_MUL, D_T(6, MASK_Y), S_TY(6), S_CX(C + 6));
            op2(b, F_EXP, D_T(6, MASK_X), S_TY(6));
            op3(b, F_MUL, D_T(5, MASK_X), S_TX(5), S_TX(6));
        }
    }
    op3(b, F_DP3, D_T(6, MASK_W), S_T(1), S_T(4));
    op3(b, F_MAX, D_T(6, MASK_W), S_TW(6), S_CX(L_ZERO));
    op3(b, F_MUL, D_T(6, MASK_XYZ), S_TW(6), S_CONST(C + 0));
    op3(b, F_MUL, D_T(6, MASK_XYZ), S_T(6), S_TX(5));
    op3(b, F_MUL, D_T(6, MASK_XYZ), S_T(6), S_T(8));
    op3(b, F_ADD, D_T(2, MASK_XYZ), S_T(2), S_T(6));
    op3(b, F_MUL, D_T(11, MASK_XYZ), S_CONST(C + 2), S_TX(5));
    op3(b, F_MUL, D_T(11, MASK_XYZ), S_T(11), S_T(10));
    op3(b, F_ADD, D_T(2, MASK_XYZ), S_T(2), S_T(11));
    op3(b, F_ADD, D_T(6, MASK_XYZ), S_T(4), S_T(7));
    op3(b, F_DP3, D_T(6, MASK_W), S_T(6), S_T(6));
    op2(b, F_RSQ, D_T(6, MASK_W), S_TW(6));
    op3(b, F_MUL, D_T(6, MASK_XYZ), S_T(6), S_TW(6));
    op3(b, F_DP3, D_T(6, MASK_W), S_T(1), S_T(6));
    op3(b, F_MAX, D_T(6, MASK_W), S_TW(6), S_CX(L_ZERO));
    op2(b, F_LOG, D_T(6, MASK_Y), S_TW(6));
    op3(b, F_MUL, D_T(6, MASK_Y), S_TY(6), S_CW(FFP_VS_MAT_DIFFUSE));
    op2(b, F_EXP, D_T(6, MASK_W), S_TY(6));
    op3(b, F_MUL, D_T(11, MASK_XYZ), S_TW(6), S_CONST(C + 1));
    op3(b, F_MUL, D_T(11, MASK_XYZ), S_T(11), S_TX(5));
    op3(b, F_MUL, D_T(11, MASK_XYZ), S_T(11), S_T(9));
    op3(b, F_ADD, D_T(3, MASK_XYZ), S_T(3), S_T(11));
}

/* writes texture-coordinate output `slot` for index `i` */
static void vs_texcoord(TB* b, const FfpState* f, const FF* ff, int i, int slot)
{
    int gen = 0, stage = -1;
    for (int s = 0; s < FFP_MAX_STAGES; s++) {
        if (f->tss[s][1] == 1) continue;
        int gi;
        if (stage_tc(f, s, &gi) == i) { stage = s; gen = gi; }
    }
    if (gen == 0) {
        if (ff->tc[i] >= 0) op2(b, F_MOV, D_OUT(slot, MASK_ALL), S_IN(ff->tc[i]));
        else op2(b, F_MOV, D_OUT(slot, MASK_ALL), S_CX(L_ZERO));
        return;
    }
    if (gen >= 3) {
        /* reflection: camera-space position in r5, camera-space normal in r4 */
        op3(b, F_DP4, D_T(5, MASK_X), S_T(0), S_CONST(FFP_VS_VIEW));
        op3(b, F_DP4, D_T(5, MASK_Y), S_T(0), S_CONST(FFP_VS_VIEW + 1));
        op3(b, F_DP4, D_T(5, MASK_Z), S_T(0), S_CONST(FFP_VS_VIEW + 2));
        op2(b, F_MOV, D_T(5, MASK_W), S_CX(L_ONE));
        vs_normalize(b, 5);
        op3(b, F_DP3, D_T(4, MASK_X), S_T(1), S_CONST(FFP_VS_VIEW));
        op3(b, F_DP3, D_T(4, MASK_Y), S_T(1), S_CONST(FFP_VS_VIEW + 1));
        op3(b, F_DP3, D_T(4, MASK_Z), S_T(1), S_CONST(FFP_VS_VIEW + 2));
        vs_normalize(b, 4);
        op3(b, F_DP3, D_T(6, MASK_W), S_T(4), S_T(5));
        op3(b, F_MUL, D_T(6, MASK_W), S_TW(6), S_CX(L_TWO));
        op3(b, F_MUL, D_T(6, MASK_XYZ), S_T(4), S_TW(6));
        op3(b, F_SUB, D_T(4, MASK_XYZ), S_T(5), S_T(6));             /* R = I - 2(N.I)N */
    } else if (gen == 2) {
        op3(b, F_DP4, D_T(4, MASK_X), S_T(0), S_CONST(FFP_VS_VIEW));
        op3(b, F_DP4, D_T(4, MASK_Y), S_T(0), S_CONST(FFP_VS_VIEW + 1));
        op3(b, F_DP4, D_T(4, MASK_Z), S_T(0), S_CONST(FFP_VS_VIEW + 2));
        op2(b, F_MOV, D_T(4, MASK_W), S_CX(L_ONE));
    } else {
        op3(b, F_DP3, D_T(4, MASK_X), S_T(1), S_CONST(FFP_VS_VIEW));
        op3(b, F_DP3, D_T(4, MASK_Y), S_T(1), S_CONST(FFP_VS_VIEW + 1));
        op3(b, F_DP3, D_T(4, MASK_Z), S_T(1), S_CONST(FFP_VS_VIEW + 2));
        vs_normalize(b, 4);
    }
    if (gen == 4) {
        op4(b, F_MAD, D_T(5, MASK_X), S_TX(4), S_CX(L_HALF), S_CX(L_HALF));
        op4(b, F_MAD, D_T(5, MASK_Y), S_TY(4), S_CX(L_NEG), S_CX(L_HALF));
        op2(b, F_MOV, D_T(5, MASK_Z), S_CX(L_ZERO));
        op2(b, F_MOV, D_T(5, MASK_W), S_CX(L_ONE));
        op2(b, F_MOV, D_T(4, MASK_ALL), S_T(5));
    }
    uint32_t tt = stage >= 0 ? f->tss[stage][24] : 0;
    int count = (int)(tt & 0xFF), proj = (tt & 0x100) != 0;
    if (count > 0) {
        op3(b, F_DP4, D_T(5, MASK_X), S_T(4), S_CONST(FFP_VS_TEXMAT + 4 * i));
        op3(b, F_DP4, D_T(5, MASK_Y), S_T(4), S_CONST(FFP_VS_TEXMAT + 4 * i + 1));
        if (count >= 3) op3(b, F_DP4, D_T(5, MASK_Z), S_T(4), S_CONST(FFP_VS_TEXMAT + 4 * i + 2));
        if (count >= 4) op3(b, F_DP4, D_T(5, MASK_W), S_T(4), S_CONST(FFP_VS_TEXMAT + 4 * i + 3));
        if (proj) {
            op2(b, F_RCP, D_T(5, MASK_W), S_TW(5));
            op3(b, F_MUL, D_T(5, MASK_XYZ), S_T(5), S_TW(5));
        }
        op2(b, F_MOV, D_OUT(slot, MASK_ALL), S_T(5));
    } else op2(b, F_MOV, D_OUT(slot, MASK_ALL), S_T(4));
}

/* writes the fog amount (0 = clear, 1 = fog colour) to output `slot` */
static void vs_fog(TB* b, const FfpState* f, int slot)
{
    if (f->fog_radial) {
        op3(b, F_DP4, D_T(4, MASK_X), S_T(0), S_CONST(FFP_VS_VIEW));
        op3(b, F_DP4, D_T(4, MASK_Y), S_T(0), S_CONST(FFP_VS_VIEW + 1));
        op3(b, F_DP4, D_T(4, MASK_Z), S_T(0), S_CONST(FFP_VS_VIEW + 2));
        op3(b, F_DP3, D_T(4, MASK_W), S_T(4), S_T(4));
        op2(b, F_RSQ, D_T(4, MASK_W), S_TW(4));
        op2(b, F_RCP, D_T(4, MASK_W), S_TW(4));                       /* sqrt(d^2) */
    } else {
        op3(b, F_DP4, D_T(4, MASK_W), S_T(0), S_CONST(FFP_VS_VIEW + 2));
        op3(b, F_MUL, D_T(6, MASK_X), S_TW(4), S_CX(L_NEG));
        op3(b, F_MAX, D_T(4, MASK_W), S_TW(4), S_TX(6));               /* |view z| */
    }
    if (f->fog == 3) {
        op3(b, F_SUB, D_T(5, MASK_X), S_CY(FFP_VS_FOG), S_TW(4));     /* end - d */
        op3(b, F_SUB, D_T(5, MASK_Y), S_CY(FFP_VS_FOG), S_CX(FFP_VS_FOG));
        op2(b, F_RCP, D_T(5, MASK_Y), S_TY(5));
        op3(b, F_MUL, D_T(5, MASK_X), S_TX(5), S_TY(5));               /* visibility */
    } else {
        op3(b, F_MUL, D_T(5, MASK_X), S_TW(4), S_CZ(FFP_VS_FOG));
        if (f->fog == 2) op3(b, F_MUL, D_T(5, MASK_X), S_TX(5), S_TX(5));
        op3(b, F_MUL, D_T(5, MASK_X), S_TX(5), S_CY(L_LOG2E));
        op2(b, F_EXP, D_T(5, MASK_X), S_TX(5));
    }
    op3(b, F_SUB, D_T(5, MASK_X), S_CX(L_ONE), S_TX(5));
    op3(b, F_MAX, D_T(5, MASK_X), S_TX(5), S_CX(L_ZERO));
    op3(b, F_MIN, D_T(5, MASK_X), S_TX(5), S_CX(L_ONE));
    op2(b, F_MOV, D_OUT(slot, MASK_ALL), S_TX(5));
}

uint32_t* ffp_build_vs(const FfpState* f, uint32_t* bytes)
{
    FF ff;
    scan(f, &ff);
    *bytes = 0;
    if (ff.pos < 0) return NULL;
    TB b;
    memset(&b, 0, sizeof b);
    tb_op(&b, 0xFFFE0300);                     /* vs_3_0 */
    def(&b, L_ZERO, 0, 0, 0, 0);
    def(&b, L_ONE, 1, 1, 1, 1);
    def(&b, L_HALF, 0.5f, 0.5f, 0.5f, 0.5f);
    def(&b, L_NEG, -1, -1, -1, -1);
    def(&b, L_TWO, 2, 2, 2, 2);
    def(&b, L_THREE, 3, 3, 3, 3);
    def(&b, L_LOG2E, 1.44269504f, -1.44269504f, 0, 0);
    def(&b, L_PALETTE_STEP, 0, 3, 6, 9);
    def(&b, L_EPS, 1.0e-6f, 0, 0, 0);
    dcl(&b, (uint32_t)ff.pos_usage, 0, 0x900F0000u | (uint32_t)ff.pos);
    if (ff.normal >= 0) dcl(&b, 3, 0, 0x900F0000u | (uint32_t)ff.normal);
    for (int i = 0; i < 8; i++) if (ff.tc[i] >= 0) dcl(&b, 5, (uint32_t)i, 0x900F0000u | (uint32_t)ff.tc[i]);
    for (int i = 0; i < 2; i++) if (ff.col[i] >= 0) dcl(&b, 10, (uint32_t)i, 0x900F0000u | (uint32_t)ff.col[i]);
    if (ff.skin && ff.weight >= 0) dcl(&b, 1, 0, 0x900F0000u | (uint32_t)ff.weight);
    if (ff.skin && ff.index >= 0) dcl(&b, 2, 0, 0x900F0000u | (uint32_t)ff.index);
    dcl(&b, (uint32_t)ff.pos_usage, 0, 0xE00F0000u);
    for (int i = 0; i < 8; i++) if (ff.out_tc[i] >= 0) dcl(&b, 5, (uint32_t)i, 0xE00F0000u | (uint32_t)ff.out_tc[i]);
    if (ff.out_col0 >= 0) dcl(&b, 10, 0, 0xE00F0000u | (uint32_t)ff.out_col0);
    if (ff.out_col1 >= 0) dcl(&b, 10, 1, 0xE00F0000u | (uint32_t)ff.out_col1);
    if (ff.out_fog >= 0) dcl(&b, 11, 0, 0xE00F0000u | (uint32_t)ff.out_fog);
    if (ff.out_clip >= 0) dcl(&b, 13, 0, 0xE00F0000u | (uint32_t)ff.out_clip);

    if (ff.xyzrhw) {
        op2(&b, F_MOV, D_T(0, MASK_ALL), S_IN(ff.pos));
        op3(&b, F_MUL, D_T(0, MASK_XY), S_T(0), S_CONST(FFP_VS_SCREEN));
        op3(&b, F_ADD, D_T(0, MASK_XY), S_T(0), S_CONST(FFP_VS_SCREEN + 1));
        op3(&b, F_MUL, D_T(0, MASK_Z), S_T(0), S_CZ(FFP_VS_SCREEN));
        op3(&b, F_ADD, D_T(0, MASK_Z), S_T(0), S_CZ(FFP_VS_SCREEN + 1));
        op2(&b, F_MOV, D_T(0, MASK_W), S_CW(FFP_VS_SCREEN + 1));
        op2(&b, F_MOV, D_OUT(0, MASK_ALL), S_T(0));
    } else {
        vs_world_position(&b, &ff);
        op3(&b, F_DP4, D_OUT(0, MASK_X), S_T(0), S_CONST(FFP_VS_VIEWPROJ));
        op3(&b, F_DP4, D_OUT(0, MASK_Y), S_T(0), S_CONST(FFP_VS_VIEWPROJ + 1));
        op3(&b, F_DP4, D_OUT(0, MASK_Z), S_T(0), S_CONST(FFP_VS_VIEWPROJ + 2));
        op3(&b, F_DP4, D_OUT(0, MASK_W), S_T(0), S_CONST(FFP_VS_VIEWPROJ + 3));
        if (ff.lit) vs_normal(&b, &ff);
    }

    if (ff.lit) {
        op2(&b, F_MOV, D_T(8, MASK_ALL), S_CONST(FFP_VS_MAT_DIFFUSE));
        op2(&b, F_MOV, D_T(9, MASK_ALL), S_CONST(FFP_VS_MAT_SPECULAR));
        op2(&b, F_MOV, D_T(10, MASK_ALL), S_CONST(FFP_VS_MAT_AMBIENT));
        op2(&b, F_MOV, D_T(11, MASK_ALL), S_CONST(FFP_VS_MAT_EMISSIVE));
        if (f->color_vertex && ff.col[0] >= 0) {
            if (f->diffuse_src == 1) op2(&b, F_MOV, D_T(8, MASK_ALL), S_IN(ff.col[0]));
            if (f->ambient_src == 1) op2(&b, F_MOV, D_T(10, MASK_ALL), S_IN(ff.col[0]));
            if (f->specular_src == 1) op2(&b, F_MOV, D_T(9, MASK_ALL), S_IN(ff.col[0]));
            if (f->emissive_src == 1) op2(&b, F_MOV, D_T(11, MASK_ALL), S_IN(ff.col[0]));
        }
        if (f->color_vertex && ff.col[1] >= 0) {
            if (f->diffuse_src == 2) op2(&b, F_MOV, D_T(8, MASK_ALL), S_IN(ff.col[1]));
            if (f->ambient_src == 2) op2(&b, F_MOV, D_T(10, MASK_ALL), S_IN(ff.col[1]));
            if (f->specular_src == 2) op2(&b, F_MOV, D_T(9, MASK_ALL), S_IN(ff.col[1]));
            if (f->emissive_src == 2) op2(&b, F_MOV, D_T(11, MASK_ALL), S_IN(ff.col[1]));
        }
        op3(&b, F_MUL, D_T(2, MASK_ALL), S_T(10), S_CONST(FFP_VS_AMBIENT));
        op3(&b, F_ADD, D_T(2, MASK_XYZ), S_T(2), S_T(11));
        op2(&b, F_MOV, D_T(3, MASK_ALL), S_CX(L_ZERO));
        if (f->local_viewer) {
            op3(&b, F_SUB, D_T(7, MASK_XYZ), S_CONST(FFP_VS_EYE), S_T(0));
            vs_normalize(&b, 7);
        } else {
            op2(&b, F_MOV, D_T(7, MASK_XYZ), S_CONST(FFP_VS_VIEW_FWD));
        }
        for (int i = 0; i < f->nlights && i < FFP_MAX_LIGHTS; i++)
            vs_light(&b, FFP_VS_LIGHTS + FFP_VS_LIGHT_STRIDE * i, &f->light[i]);
        op2(&b, F_MOV, D_T(2, MASK_W), S_TW(8));
        op2(&b, F_MOV, D_OUT(ff.out_col0, MASK_ALL), S_T(2));
        if (ff.out_col1 >= 0) op2(&b, F_MOV, D_OUT(ff.out_col1, MASK_XYZ), S_T(3));
    } else {
        if (f->color_vertex && ff.col[0] >= 0) op2(&b, F_MOV, D_OUT(ff.out_col0, MASK_ALL), S_IN(ff.col[0]));
        if (f->color_vertex && ff.col[1] >= 0) op2(&b, F_MOV, D_OUT(ff.out_col1, MASK_ALL), S_IN(ff.col[1]));
    }
    for (int i = 0; i < 8; i++) if (ff.out_tc[i] >= 0) vs_texcoord(&b, f, &ff, i, ff.out_tc[i]);
    if (ff.out_fog >= 0) {
        if (ff.xyzrhw) op2(&b, F_MOV, D_OUT(ff.out_fog, MASK_ALL), S_CX(L_ZERO));
        else vs_fog(&b, f, ff.out_fog);
    }
    if (ff.out_clip >= 0) op2(&b, F_MOV, D_OUT(ff.out_clip, MASK_ALL), S_T(0));
    tb_op(&b, 0x0000FFFF);
    *bytes = (uint32_t)b.n * 4;
    return b.t;
}

/* ================================================================ pixel shader */

static uint32_t ps_arg_src(uint32_t a, int stage, int tex_ok, int col0, int col1)
{
    switch (a & 0xF) {
    case 0: return col0 >= 0 ? S_IN(col0) : S_CONST(FFP_PS_MATERIAL);   /* diffuse */
    case 1: return S_T(0);                                             /* current */
    case 2: return tex_ok ? S_T(1) : S_CX(L_ONE);                      /* texture */
    case 3: return S_CONST(289);                                       /* TFactor */
    case 4: return col1 >= 0 ? S_IN(col1) : S_CX(L_ZERO);              /* specular */
    case 5: return S_T(2);                                             /* temp */
    default: return S_CONST(FFP_PS_STAGE + stage);                     /* stage constant */
    }
}

/* materialise an argument into a register, applying D3DTA_ALPHAREPLICATE and D3DTA_COMPLEMENT */
static void ps_arg(TB* b, uint32_t a, int stage, int tex_ok, int col0, int col1, int reg)
{
    op2(b, F_MOV, D_T(reg, MASK_ALL), ps_arg_src(a, stage, tex_ok, col0, col1));
    if (a & 0x20) op2(b, F_MOV, D_T(reg, MASK_ALL), S_TW(reg));
    if (a & 0x10) op3(b, F_SUB, D_T(reg, MASK_ALL), S_CX(L_ONE), S_T(reg));
}

/* colour operation from r3/r4/r5 into r0.rgb */
static void ps_color_op(TB* b, uint32_t op, int col0)
{
    const uint32_t d = D_T(0, MASK_XYZ);
    const uint32_t alpha = col0 >= 0 ? S_INW((uint32_t)col0) : S_CW(FFP_PS_MATERIAL);
    switch (op) {
    case 1: break;
    case 2: op2(b, F_MOV, d, S_T(4)); break;
    case 3: op2(b, F_MOV, d, S_T(5)); break;
    case 5: op3(b, F_MUL, d, S_T(4), S_T(5)); op3(b, F_ADD, d, S_T(0), S_T(0)); break;
    case 6: op3(b, F_MUL, d, S_T(4), S_T(5)); op3(b, F_ADD, d, S_T(0), S_T(0));
            op3(b, F_ADD, d, S_T(0), S_T(0)); break;
    case 7: op3(b, F_ADD, d, S_T(4), S_T(5)); break;
    case 8: op3(b, F_ADD, d, S_T(4), S_T(5)); op3(b, F_SUB, d, S_T(0), S_CX(L_HALF)); break;
    case 9: op3(b, F_ADD, d, S_T(4), S_T(5)); op3(b, F_SUB, d, S_T(0), S_CX(L_HALF));
            op3(b, F_ADD, d, S_T(0), S_T(0)); break;
    case 10: op3(b, F_SUB, d, S_T(4), S_T(5)); break;
    case 11: op3(b, F_MUL, D_T(6, MASK_XYZ), S_T(4), S_T(5));
             op3(b, F_ADD, d, S_T(4), S_T(5));
             op3(b, F_SUB, d, S_T(0), S_T(6)); break;
    case 12: case 13: case 14: case 16: {
        /* blend: arg1 + alpha * (arg2 - arg1), with that operation's alpha source */
        uint32_t a = op == 12 ? alpha : op == 13 ? S_TW(1) : op == 14 ? S_CW(289) : S_TW(0);
        op2(b, F_MOV, D_T(6, MASK_X), a);
        op3(b, F_SUB, D_T(6, MASK_XYZ), S_T(5), S_T(4));
        op4(b, F_MAD, d, S_TX(6), S_T(6), S_T(4));
        break;
    }
    case 15: {                                     /* blend texture alpha PM: arg1 + arg2 * (1 - alpha) */
        op3(b, F_SUB, D_T(6, MASK_X), S_CX(L_ONE), S_TW(1));
        op4(b, F_MAD, d, S_TX(6), S_T(5), S_T(4));
        break;
    }
    case 24: op3(b, F_DP3, d, S_T(4), S_T(5)); break;
    case 25: op4(b, F_MAD, d, S_T(5), S_T(3), S_T(4)); break;
    case 26: op4(b, F_LRP, d, S_T(3), S_T(5), S_T(4)); break;
    default: op3(b, F_MUL, d, S_T(4), S_T(5)); break;
    }
}

/* alpha operation from r6/r7/r8 into r0.w */
static void ps_alpha_op(TB* b, uint32_t op)
{
    const uint32_t d = D_T(0, MASK_W);
    switch (op) {
    case 1: break;
    case 2: op2(b, F_MOV, d, S_T(6)); break;
    case 3: op2(b, F_MOV, d, S_T(7)); break;
    case 4: op3(b, F_MUL, d, S_T(6), S_T(7)); break;
    case 5: op3(b, F_MUL, d, S_T(6), S_T(7)); op3(b, F_ADD, d, S_T(0), S_T(0)); break;
    case 6: op3(b, F_MUL, d, S_T(6), S_T(7)); op3(b, F_ADD, d, S_T(0), S_T(0));
            op3(b, F_ADD, d, S_T(0), S_T(0)); break;
    case 7: op3(b, F_ADD, d, S_T(6), S_T(7)); break;
    case 8: op3(b, F_ADD, d, S_T(6), S_T(7)); op3(b, F_SUB, d, S_T(0), S_CX(L_HALF)); break;
    case 9: op3(b, F_ADD, d, S_T(6), S_T(7)); op3(b, F_SUB, d, S_T(0), S_CX(L_HALF));
            op3(b, F_ADD, d, S_T(0), S_T(0)); break;
    case 10: op3(b, F_SUB, d, S_T(6), S_T(7)); break;
    case 11: op3(b, F_MUL, D_T(5, MASK_W), S_T(6), S_T(7));
             op3(b, F_ADD, d, S_T(6), S_T(7));
             op3(b, F_SUB, d, S_T(0), S_T(5)); break;
    case 26: op4(b, F_LRP, d, S_T(8), S_T(7), S_T(6)); break;
    default: break;
    }
}

uint32_t* ffp_build_ps(const FfpState* f, uint32_t* bytes)
{
    FF ff;
    scan(f, &ff);
    *bytes = 0;
    TB b;
    memset(&b, 0, sizeof b);
    tb_op(&b, 0xFFFF0300);                     /* ps_3_0 */
    def(&b, L_ZERO, 0, 0, 0, 0);
    def(&b, L_ONE, 1, 1, 1, 1);
    def(&b, L_HALF, 0.5f, 0.5f, 0.5f, 0.5f);
    int tex_ok[FFP_MAX_STAGES];
    for (int s = 0; s < FFP_MAX_STAGES; s++) tex_ok[s] = f->tss[s][1] != 1 && f->texkind[s] != 0;
    for (int i = 0; i < 8; i++) if (ff.out_tc[i] >= 0) dcl(&b, 5, (uint32_t)i, 0x900F0000u | (uint32_t)ff.out_tc[i]);
    if (ff.out_col0 >= 0) dcl(&b, 10, 0, 0x900F0000u | (uint32_t)ff.out_col0);
    if (ff.out_col1 >= 0) dcl(&b, 10, 1, 0x900F0000u | (uint32_t)ff.out_col1);
    if (ff.out_fog >= 0) dcl(&b, 11, 0, 0x900F0000u | (uint32_t)ff.out_fog);
    if (ff.out_clip >= 0) dcl(&b, 13, 0, 0x900F0000u | (uint32_t)ff.out_clip);
    for (int s = 0; s < FFP_MAX_STAGES; s++) {
        if (!tex_ok[s]) continue;
        uint32_t ty = f->texkind[s] == 6 ? 3u : f->texkind[s] == 5 ? 4u : 2u;
        tb_op(&b, 0x0200001F);
        tb_op(&b, 0x90000000u | (ty << 27) | (uint32_t)s);
        tb_op(&b, 0xA00F0800u | (uint32_t)s);
    }
    op2(&b, F_MOV, D_T(0, MASK_ALL), ff.out_col0 >= 0 ? S_IN(ff.out_col0) : S_CONST(FFP_PS_MATERIAL));
    op2(&b, F_MOV, D_T(2, MASK_ALL), S_CX(L_ZERO));
    for (int s = 0; s < FFP_MAX_STAGES; s++) {
        uint32_t cop = f->tss[s][1];
        if (cop == 1) break;
        int gen = 0;
        int tc = stage_tc(f, s, &gen);
        if (tex_ok[s]) {
            uint32_t coord = ff.out_tc[tc] >= 0 ? S_IN(ff.out_tc[tc]) : S_CX(L_ZERO);
            tb_op(&b, 0x03000042);
            tb_op(&b, D_T(1, MASK_ALL));
            tb_op(&b, coord);
            tb_op(&b, 0xA0E40800u | (uint32_t)s);
        }
        ps_arg(&b, f->tss[s][25], s, tex_ok[s], ff.out_col0, ff.out_col1, 3);
        ps_arg(&b, f->tss[s][2], s, tex_ok[s], ff.out_col0, ff.out_col1, 4);
        ps_arg(&b, f->tss[s][3], s, tex_ok[s], ff.out_col0, ff.out_col1, 5);
        ps_color_op(&b, cop, ff.out_col0);
        uint32_t aop = f->tss[s][4];
        if (aop != 1) {
            ps_arg(&b, f->tss[s][5], s, tex_ok[s], ff.out_col0, ff.out_col1, 8);
            ps_arg(&b, f->tss[s][6], s, tex_ok[s], ff.out_col0, ff.out_col1, 6);
            ps_arg(&b, f->tss[s][7], s, tex_ok[s], ff.out_col0, ff.out_col1, 7);
            ps_alpha_op(&b, aop);
        }
    }
    for (int i = 0; i < 6; i++) {
        if (!(f->clip_planes & (1 << i)) || ff.out_clip < 0) continue;
        op3(&b, F_DP4, D_T(3, MASK_X), S_IN(ff.out_clip), S_CONST(FFP_PS_CLIP + i));
        tb_op(&b, 0x01000041);                                        /* texkill r3.x */
        tb_op(&b, D_T(3, MASK_X));
    }
    if (ff.out_fog >= 0) {
        op2(&b, F_MOV, D_T(3, MASK_X), S_INX(ff.out_fog));
        op4(&b, F_LRP, D_T(0, MASK_XYZ), S_TX(3), S_CONST(FFP_PS_FOG), S_T(0));
    }
    op2(&b, F_MOV, D_OC(0), S_T(0));
    tb_op(&b, 0x0000FFFF);
    *bytes = (uint32_t)b.n * 4;
    return b.t;
}

int ffp_texture_stages(const FfpState* f)
{
    int n = 0;
    for (int s = 0; s < FFP_MAX_STAGES; s++) if (f->tss[s][1] != 1 && f->texkind[s]) n++;
    return n;
}
