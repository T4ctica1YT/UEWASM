/* Direct3D 9 shader bytecode (vs/ps 2.0, 2.x, 3.0, and vs 1.1) to WGSL.

   The output follows the conventions of the DXBC translator (dxbc_wgsl.c), so the WebGPU backend treats both alike:
   - constants: one uniform array per stage, cb0 (group 0 for vertex shaders, group 2 for pixel shaders). D3D9's float
     constants c0-c255 are its first 256 entries, integer constants i0-i15 the next 16 (as bits), booleans b0-b15 the 16
     after that, then D3D9_CB_EXTRA holds state the fixed pipeline used to apply: the half-pixel offset (vertex
     shaders) and the alpha test (pixel shaders). runtime/d3d9.c fills it in that layout.
   - textures and samplers: sampler sN is texture binding 16+N and sampler binding 160+N (group 1 or 3).
   - vertex inputs: location = input register; varyings: a location per output, matched by semantic (vs_out).

   A vertex shader built for a vertex declaration with integer elements read as floats (UBYTE4, SHORT2, SHORT4) gets
   a trailer after the END token: D3D9_INT_MAGIC, a mask of unsigned input registers and a mask of signed ones. */
#include "d3d9_wgsl.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char* p; size_t n, cap; } SB;
static void sp(SB* b, const char* f, ...)
{
    va_list a;
    for (;;) {
        va_start(a, f);
        int k = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, f, a);
        va_end(a);
        if (k < 0) return;
        if (b->p && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        size_t want = (b->n + (size_t)k + 1) * 2;
        if (want < 4096) want = 4096;
        char* q = realloc(b->p, want);
        if (!q) return;
        b->p = q;
        b->cap = want;
        if (!b->n) b->p[0] = 0;
    }
}

enum {
    RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_TEXTURE = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_TEXCRDOUT = 6, RT_OUTPUT = 6,
    RT_CONSTINT = 7, RT_COLOROUT = 8, RT_DEPTHOUT = 9, RT_SAMPLER = 10, RT_CONST2 = 11, RT_CONST3 = 12, RT_CONST4 = 13, RT_CONSTBOOL = 14,
    RT_LOOP = 15, RT_TEMPF16 = 16, RT_MISC = 17, RT_LABEL = 18, RT_PRED = 19
};
enum {
    OP_NOP = 0, OP_MOV, OP_ADD, OP_SUB, OP_MAD, OP_MUL, OP_RCP, OP_RSQ, OP_DP3, OP_DP4, OP_MIN, OP_MAX, OP_SLT, OP_SGE, OP_EXP, OP_LOG,
    OP_LIT, OP_DST, OP_LRP, OP_FRC, OP_M4x4, OP_M4x3, OP_M3x4, OP_M3x3, OP_M3x2, OP_CALL, OP_CALLNZ, OP_LOOP, OP_RET, OP_ENDLOOP,
    OP_LABEL, OP_DCL, OP_POW, OP_CRS, OP_SGN, OP_ABS, OP_NRM, OP_SINCOS, OP_REP, OP_ENDREP, OP_IF, OP_IFC, OP_ELSE, OP_ENDIF,
    OP_BREAK, OP_BREAKC, OP_MOVA, OP_DEFB, OP_DEFI,
    OP_TEXCOORD = 64, OP_TEXKILL, OP_TEX, OP_EXPP = 78, OP_LOGP, OP_CND, OP_DEF, OP_CMP = 88, OP_BEM, OP_DP2ADD, OP_DSX, OP_DSY,
    OP_TEXLDD, OP_SETP, OP_TEXLDL, OP_BREAKP, OP_PHASE = 0xFFFD, OP_COMMENT = 0xFFFE, OP_END = 0xFFFF
};

static const char* const USAGE[] = { "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT", "BINORMAL",
                                     "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE" };
static const char* usage_name(uint32_t u) { return u < 14 ? USAGE[u] : "UNKNOWN"; }

/* a parameter token, decoded; rel*: the relative-address register that follows it, if any */
typedef struct {
    uint32_t type, num, mask, mod, swz, shift;
    int rel;
    uint32_t rel_type, rel_num, rel_comp;
} Param;

/* outputs of a vertex shader / inputs of a pixel shader, by array slot (o[]/v[] index) */
typedef struct { int used; uint32_t usage, index; } Slot;

typedef struct {
    const uint32_t* t;
    size_t n;
    int ps, major, minor;
    DxWgsl* out;
    const DxWgslOpts* opt;
    /* defined constants */
    uint8_t cdef[256], idef[16], bdef[16];
    uint32_t cval[256][4];
    int32_t ival[16][4];
    uint32_t bval[16];
    int crel;               /* float constants read with relative addressing */
    /* declarations */
    Slot vin[16], vout[16];
    uint8_t samp_dim[16];   /* 0 = unused, else 3 (2d), 6 (cube), 5 (3d) */
    int ntemp, uses_pred, uses_a0, uses_aL, uses_vpos, uses_vface, odepth, ocol;
    uint32_t written_out;   /* vertex shader output slots written (SM2 fixed registers mapped to slots) */
    uint32_t int_u, int_s;  /* vertex inputs that are integers (from the trailer) */
    SB body, funcs;
    SB* cur;
    int depth, loopn, in_label, done_main;
    int lstack[32], lsp;
    int err;
    char why[200];
} Tr;

static void fail(Tr* t, const char* f, ...)
{
    if (t->err) return;
    t->err = 1;
    va_list a;
    va_start(a, f);
    vsnprintf(t->why, sizeof t->why, f, a);
    va_end(a);
}

static uint32_t reg_type(uint32_t tok) { return ((tok >> 28) & 7) | ((tok >> 8) & 0x18); }

/* reads a destination or source parameter (and its relative-address token) at *i */
static Param param(Tr* t, size_t* i, int src)
{
    Param p = { 0 };
    if (*i >= t->n) { fail(t, "truncated shader"); return p; }
    uint32_t tok = t->t[(*i)++];
    p.type = reg_type(tok);
    p.num = tok & 0x7FF;
    if (src) {
        p.swz = (tok >> 16) & 0xFF;
        p.mod = (tok >> 24) & 0xF;
        p.mask = 0xF;
    } else {
        p.mask = (tok >> 16) & 0xF;
        p.mod = (tok >> 20) & 0xF;
        p.shift = (tok >> 24) & 0xF;
        p.swz = 0xE4;
    }
    if (tok & (1u << 13)) {
        p.rel = 1;
        /* vs_1_1 / vs_2_0 sources: a0.x implied when no extra token (1.x); 2.0+ always has the token */
        if (t->major >= 2 && *i < t->n) {
            uint32_t r = t->t[(*i)++];
            p.rel_type = reg_type(r);
            p.rel_num = r & 0x7FF;
            p.rel_comp = (r >> 16) & 3;
        } else {
            p.rel_type = RT_ADDR;
            p.rel_comp = 0;
        }
    }
    return p;
}

static void lit(char* b, size_t n, uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    if (!isfinite(f)) { snprintf(b, n, "bitcast<f32>(%uu)", bits); return; }
    if (f != 0.0f && fabsf(f) < 1.1754944e-38f) { snprintf(b, n, "0.0"); return; }
    char t[48];
    snprintf(t, sizeof t, "%.9g", f);
    if (!strpbrk(t, ".en")) snprintf(b, n, "%s.0", t);
    else snprintf(b, n, "%s", t);
}

/* ---- register expressions ---- */

static const char* comp = "xyzw";

static void sm2_out_slot(uint32_t type, uint32_t num, int* slot, uint32_t* usage, uint32_t* index)
{
    /* fixed SM1/2 vertex outputs on fixed slots: TEXCOORD0-7 on 0-7, COLOR0-1 on 10-11, FOG 12, PSIZE 13 */
    if (type == RT_TEXCRDOUT) { *slot = (int)num; *usage = 5; *index = num; }
    else if (type == RT_ATTROUT) { *slot = 10 + (int)(num & 1); *usage = 10; *index = num; }
    else if (num == 1) { *slot = 12; *usage = 11; *index = 0; }
    else { *slot = 13; *usage = 4; *index = 0; }
}

static void rel_expr(Tr* t, const Param* p, char* b, size_t n, int base)
{
    if (p->rel_type == RT_LOOP) { t->uses_aL = 1; snprintf(b, n, "(aL + %d)", base); }
    else { t->uses_a0 = 1; snprintf(b, n, "(a0.%c + %d)", comp[p->rel_comp], base); }
}

/* the register as a vec4<f32> expression, before swizzle and modifiers; off moves along its file (matrix rows) */
static void reg_expr(Tr* t, const Param* p, int off, char* b, size_t n)
{
    int num = (int)p->num + off;
    char r[64];
    switch (p->type) {
    case RT_TEMP: case RT_TEMPF16:
        if (num >= 32) { fail(t, "temp r%d", num); }
        if (num + 1 > t->ntemp) t->ntemp = num + 1;
        snprintf(b, n, "r%d", num);
        return;
    case RT_INPUT:
        if (p->rel) { rel_expr(t, p, r, sizeof r, num); snprintf(b, n, "v[clamp(%s, 0, 15)]", r); }
        else snprintf(b, n, "v[%d]", num & 15);
        return;
    case RT_CONST: case RT_CONST2: case RT_CONST3: case RT_CONST4:
        if (p->type != RT_CONST) num += 2048 * (int)(p->type - RT_CONST2 + 1);
        if (p->rel) {
            rel_expr(t, p, r, sizeof r, num);
            t->crel = 1;
            snprintf(b, n, "uew_c(%s)", r);
        } else if (num < 256 && t->cdef[num]) {
            char x[4][48];
            for (int k = 0; k < 4; k++) lit(x[k], sizeof x[k], t->cval[num][k]);
            snprintf(b, n, "vec4<f32>(%s, %s, %s, %s)", x[0], x[1], x[2], x[3]);
        } else snprintf(b, n, "cb0[%d]", num & 255);
        return;
    case RT_ADDR:   /* = RT_TEXTURE */
        if (t->ps) snprintf(b, n, "tc[%d]", num & 7);
        else { t->uses_a0 = 1; snprintf(b, n, "vec4<f32>(a0)"); }
        return;
    case RT_CONSTINT:
        if (num < 16 && t->idef[num]) snprintf(b, n, "vec4<f32>(vec4<i32>(%di, %di, %di, %di))", t->ival[num][0], t->ival[num][1], t->ival[num][2], t->ival[num][3]);
        else snprintf(b, n, "vec4<f32>(bitcast<vec4<i32>>(cb0[%d]))", D3D9_CB_INT + (num & 15));
        return;
    case RT_CONSTBOOL:
        if (num < 16 && t->bdef[num]) snprintf(b, n, "vec4<f32>(%s)", t->bval[num] ? "1.0" : "0.0");
        else snprintf(b, n, "vec4<f32>(select(0.0, 1.0, bitcast<u32>(cb0[%d].x) != 0u))", D3D9_CB_BOOL + (num & 15));
        return;
    case RT_LOOP: t->uses_aL = 1; snprintf(b, n, "vec4<f32>(f32(aL))"); return;
    case RT_MISC:
        if (num == 0) { t->uses_vpos = 1; snprintf(b, n, "vpos"); }
        else { t->uses_vface = 1; snprintf(b, n, "vface"); }
        return;
    case RT_PRED: t->uses_pred = 1; snprintf(b, n, "select(vec4<f32>(0.0), vec4<f32>(1.0), p0)"); return;
    case RT_RASTOUT: case RT_ATTROUT: case RT_TEXCRDOUT: case RT_COLOROUT: case RT_DEPTHOUT:
        /* outputs read back (allowed in vs 3.0) */
        break;
    default:
        fail(t, "source register type %u", p->type);
        snprintf(b, n, "vec4<f32>(0.0)");
        return;
    }
    snprintf(b, n, "vec4<f32>(0.0)");
}

/* destination: the lvalue, and whether it is a scalar slot */
static void dst_lvalue(Tr* t, const Param* p, char* b, size_t n)
{
    int num = (int)p->num;
    char r[64];
    switch (p->type) {
    case RT_TEMP: case RT_TEMPF16:
        if (num + 1 > t->ntemp) t->ntemp = num + 1;
        snprintf(b, n, "r%d", num);
        return;
    case RT_ADDR:
        if (t->ps) { snprintf(b, n, "tc[%d]", num & 7); return; }
        t->uses_a0 = 1;
        snprintf(b, n, "a0");
        return;
    case RT_RASTOUT:
        if (num == 0) { snprintf(b, n, "opos"); return; }
        /* fall through: fog and point size go to slots */
    case RT_ATTROUT:
    case RT_TEXCRDOUT:
        if (t->major >= 3) {
            if (p->rel) { rel_expr(t, p, r, sizeof r, num); snprintf(b, n, "o[clamp(%s, 0, 15)]", r); t->written_out = 0xFFFF; }
            else { snprintf(b, n, "o[%d]", num & 15); t->written_out |= 1u << (num & 15); }
            return;
        } else {
            int slot; uint32_t u, x;
            sm2_out_slot(p->type, (uint32_t)num, &slot, &u, &x);
            t->vout[slot].used = 1; t->vout[slot].usage = u; t->vout[slot].index = x;
            t->written_out |= 1u << slot;
            snprintf(b, n, "o[%d]", slot);
            return;
        }
    case RT_COLOROUT:
        if (num < 4 && num + 1 > t->ocol) t->ocol = num + 1;
        snprintf(b, n, "oc[%d]", num & 3);
        return;
    case RT_DEPTHOUT: t->odepth = 1; snprintf(b, n, "odepth"); return;
    case RT_PRED: t->uses_pred = 1; snprintf(b, n, "p0"); return;
    case RT_LOOP: snprintf(b, n, "aLf"); return;
    default:
        fail(t, "destination register type %u", p->type);
        snprintf(b, n, "rx");
    }
}

/* full source expression: register, swizzle, modifier */
static void src_expr(Tr* t, const Param* p, int off, char* b, size_t n)
{
    char r[200], s[220];
    reg_expr(t, p, off, r, sizeof r);
    if (p->swz != 0xE4) {
        char sw[5];
        for (int k = 0; k < 4; k++) sw[k] = comp[(p->swz >> (2 * k)) & 3];
        sw[4] = 0;
        snprintf(s, sizeof s, "%s.%s", r, sw);
    } else snprintf(s, sizeof s, "%s", r);
    switch (p->mod) {
    case 0: snprintf(b, n, "%s", s); break;
    case 1: snprintf(b, n, "(-%s)", s); break;
    case 2: snprintf(b, n, "(%s - vec4<f32>(0.5))", s); break;
    case 3: snprintf(b, n, "(vec4<f32>(0.5) - %s)", s); break;
    case 4: snprintf(b, n, "(%s * 2.0 - vec4<f32>(1.0))", s); break;
    case 5: snprintf(b, n, "(vec4<f32>(1.0) - %s * 2.0)", s); break;
    case 6: snprintf(b, n, "(vec4<f32>(1.0) - %s)", s); break;
    case 7: snprintf(b, n, "(%s * 2.0)", s); break;
    case 8: snprintf(b, n, "(%s * -2.0)", s); break;
    case 9: snprintf(b, n, "(%s / %s.z)", s, s); break;
    case 10: snprintf(b, n, "(%s / %s.w)", s, s); break;
    case 11: snprintf(b, n, "abs(%s)", s); break;
    case 12: snprintf(b, n, "(-abs(%s))", s); break;
    default: snprintf(b, n, "%s", s); break;
    }
}

static void line(Tr* t, const char* f, ...)
{
    char buf[2048];
    va_list a;
    va_start(a, f);
    vsnprintf(buf, sizeof buf, f, a);
    va_end(a);
    for (int i = 0; i < t->depth + 1; i++) sp(t->cur, "    ");
    sp(t->cur, "%s\n", buf);
}

/* writes a vec4<f32> expression to a destination, honouring mask, saturate and predication */
static void write_dst(Tr* t, const Param* d, const char* e, int pred, const Param* pp)
{
    char lv[96], ex[1400];
    dst_lvalue(t, d, lv, sizeof lv);
    if (d->type == RT_ADDR && !t->ps) {
        /* address register: round to nearest (vs 2.0+) or floor (mov in vs 1.x) */
        snprintf(ex, sizeof ex, "vec4<i32>(%s(%s))", t->major >= 2 ? "round" : "floor", e);
    } else if (d->type == RT_DEPTHOUT) {
        snprintf(ex, sizeof ex, "%s", e);
    } else {
        const char* sh = "";
        char shb[32] = "";
        if (d->shift) {
            int s = d->shift >= 8 ? (int)d->shift - 16 : (int)d->shift;
            snprintf(shb, sizeof shb, " * %s", s > 0 ? (s == 1 ? "2.0" : s == 2 ? "4.0" : "8.0") : (s == -1 ? "0.5" : s == -2 ? "0.25" : "0.125"));
            sh = shb;
        }
        if (d->mod & 1) snprintf(ex, sizeof ex, "clamp((%s)%s, vec4<f32>(0.0), vec4<f32>(1.0))", e, sh);
        else snprintf(ex, sizeof ex, "(%s)%s", e, sh);
    }
    if (d->type == RT_DEPTHOUT) { line(t, "odepth = (%s).x;", ex); return; }
    uint32_t m = d->mask ? d->mask : 0xF;
    if (!pred && m == 0xF) { line(t, "%s = %s;", lv, ex); return; }
    char b[2048];
    int o = snprintf(b, sizeof b, "{ let t_ = %s;", ex);
    for (int k = 0; k < 4; k++) {
        if (!(m >> k & 1)) continue;
        if (pred) {
            int pc = (int)((pp->swz >> (2 * k)) & 3);
            o += snprintf(b + o, sizeof b - o, " if (%sp0.%c) { %s.%c = t_.%c; }", pp->mod == 13 ? "!" : "", comp[pc], lv, comp[k], comp[k]);
        } else o += snprintf(b + o, sizeof b - o, " %s.%c = t_.%c;", lv, comp[k], comp[k]);
    }
    snprintf(b + o, sizeof b - o, " }");
    line(t, "%s", b);
}

static const char* cmp_op(uint32_t c)
{
    switch (c & 7) {
    case 1: return ">";
    case 2: return "==";
    case 3: return ">=";
    case 4: return "<";
    case 5: return "!=";
    case 6: return "<=";
    }
    return "==";
}

static void int_expr(Tr* t, uint32_t num, char* b, size_t n)
{
    num &= 15;
    if (t->idef[num]) snprintf(b, n, "vec4<i32>(%di, %di, %di, %di)", t->ival[num][0], t->ival[num][1], t->ival[num][2], t->ival[num][3]);
    else snprintf(b, n, "bitcast<vec4<i32>>(cb0[%d])", D3D9_CB_INT + (int)num);
}

static void bool_expr(Tr* t, const Param* p, char* b, size_t n)
{
    if (p->type == RT_PRED) {
        t->uses_pred = 1;
        snprintf(b, n, "%sp0.%c", p->mod == 13 ? "!" : "", comp[p->swz & 3]);
        return;
    }
    uint32_t num = p->num & 15;
    const char* neg = p->mod == 13 ? "!" : "";
    if (t->bdef[num]) snprintf(b, n, "%s%s", neg, t->bval[num] ? "true" : "false");
    else snprintf(b, n, "%s(bitcast<u32>(cb0[%d].x) != 0u)", neg, D3D9_CB_BOOL + (int)num);
}

/* ---- texture sampling ---- */
static void sample_expr(Tr* t, uint32_t s, const char* coord, const char* mode, const char* x1, const char* x2, char* b, size_t n)
{
    s &= 15;
    if (!t->samp_dim[s]) t->samp_dim[s] = 3;     /* SM1/2 pixel shaders without dcl: 2D */
    int dim = t->samp_dim[s];
    const char* c = dim == 3 ? ".xy" : ".xyz";
    if (!t->ps && !strcmp(mode, "s")) mode = "l0";
    if (!strcmp(mode, "s")) snprintf(b, n, "textureSample(t%u, s%u, (%s)%s)", s, s, coord, c);
    else if (!strcmp(mode, "p")) {
        /* projected: divide by w (2D/3D); cube maps ignore projection */
        if (dim == 6) snprintf(b, n, "textureSample(t%u, s%u, (%s).xyz)", s, s, coord);
        else snprintf(b, n, "textureSample(t%u, s%u, ((%s)%s / (%s).w))", s, s, coord, c, coord);
    } else if (!strcmp(mode, "b")) snprintf(b, n, "textureSampleBias(t%u, s%u, (%s)%s, clamp((%s).w, -16.0, 15.99))", s, s, coord, c, coord);
    else if (!strcmp(mode, "l")) snprintf(b, n, "textureSampleLevel(t%u, s%u, (%s)%s, (%s).w)", s, s, coord, c, coord);
    else if (!strcmp(mode, "l0")) snprintf(b, n, "textureSampleLevel(t%u, s%u, (%s)%s, 0.0)", s, s, coord, c);
    else snprintf(b, n, "textureSampleGrad(t%u, s%u, (%s)%s, (%s)%s, (%s)%s)", s, s, coord, c, x1, c, x2, c);
}

/* ---- the instruction stream ---- */

static int param_count_sm1(uint32_t op)
{
    switch (op) {
    case OP_NOP: return 0;
    case OP_MOV: case OP_RCP: case OP_RSQ: case OP_EXP: case OP_LOG: case OP_LIT: case OP_FRC: case OP_EXPP: case OP_LOGP:
    case OP_MOVA: case OP_ABS: case OP_SGN: case OP_NRM: case OP_DCL: return 2;
    case OP_ADD: case OP_SUB: case OP_MUL: case OP_DP3: case OP_DP4: case OP_MIN: case OP_MAX: case OP_SLT: case OP_SGE: case OP_DST:
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: case OP_POW: case OP_CRS: return 3;
    case OP_MAD: case OP_LRP: case OP_CND: case OP_CMP: return 4;
    case OP_DEF: return 5;
    }
    return -1;
}

static void emit(Tr* t, uint32_t op, uint32_t ctrl, int pred, Param* pp, Param* d, Param* s, int ns)
{
    char a[1024], b2[1024], c3[1024], e[3400];
    a[0] = b2[0] = c3[0] = 0;
    /* texture instructions read their operands themselves (the second one is a sampler) */
    int tex = op == OP_TEX || op == OP_TEXLDL || op == OP_TEXLDD;
    if (!tex) {
        if (ns > 0) src_expr(t, &s[0], 0, a, sizeof a);
        if (ns > 1 && !(op >= OP_M4x4 && op <= OP_M3x2)) src_expr(t, &s[1], 0, b2, sizeof b2);
        if (ns > 2) src_expr(t, &s[2], 0, c3, sizeof c3);
    }
    switch (op) {
    case OP_MOV: snprintf(e, sizeof e, "%s", a); break;
    case OP_MOVA: snprintf(e, sizeof e, "%s", a); break;
    case OP_ADD: snprintf(e, sizeof e, "(%s + %s)", a, b2); break;
    case OP_SUB: snprintf(e, sizeof e, "(%s - %s)", a, b2); break;
    case OP_MUL: snprintf(e, sizeof e, "(%s * %s)", a, b2); break;
    case OP_MAD: snprintf(e, sizeof e, "(%s * %s + %s)", a, b2, c3); break;
    case OP_RCP: snprintf(e, sizeof e, "vec4<f32>(1.0 / (%s).x)", a); break;
    case OP_RSQ: snprintf(e, sizeof e, "vec4<f32>(inverseSqrt(abs((%s).x)))", a); break;
    case OP_DP3: snprintf(e, sizeof e, "vec4<f32>(dot((%s).xyz, (%s).xyz))", a, b2); break;
    case OP_DP4: snprintf(e, sizeof e, "vec4<f32>(dot(%s, %s))", a, b2); break;
    case OP_MIN: snprintf(e, sizeof e, "min(%s, %s)", a, b2); break;
    case OP_MAX: snprintf(e, sizeof e, "max(%s, %s)", a, b2); break;
    case OP_SLT: snprintf(e, sizeof e, "select(vec4<f32>(0.0), vec4<f32>(1.0), %s < %s)", a, b2); break;
    case OP_SGE: snprintf(e, sizeof e, "select(vec4<f32>(0.0), vec4<f32>(1.0), %s >= %s)", a, b2); break;
    case OP_EXP: case OP_EXPP: snprintf(e, sizeof e, "vec4<f32>(exp2((%s).x))", a); break;
    case OP_LOG: case OP_LOGP: snprintf(e, sizeof e, "vec4<f32>(log2(abs((%s).x)))", a); break;
    case OP_LIT: snprintf(e, sizeof e, "uew_lit(%s)", a); break;
    case OP_DST: snprintf(e, sizeof e, "vec4<f32>(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b2, a, b2); break;
    case OP_LRP: snprintf(e, sizeof e, "mix(%s, %s, %s)", c3, b2, a); break;
    case OP_FRC: snprintf(e, sizeof e, "fract(%s)", a); break;
    case OP_POW: snprintf(e, sizeof e, "vec4<f32>(uew_pow((%s).x, (%s).x))", a, b2); break;
    case OP_CRS: snprintf(e, sizeof e, "vec4<f32>(cross((%s).xyz, (%s).xyz), 0.0)", a, b2); break;
    case OP_SGN: snprintf(e, sizeof e, "sign(%s)", a); break;
    case OP_ABS: snprintf(e, sizeof e, "abs(%s)", a); break;
    case OP_NRM: snprintf(e, sizeof e, "uew_nrm(%s)", a); break;
    case OP_SINCOS: snprintf(e, sizeof e, "vec4<f32>(cos((%s).x), sin((%s).x), 0.0, 0.0)", a, a); break;
    case OP_CMP: snprintf(e, sizeof e, "select(%s, %s, %s >= vec4<f32>(0.0))", c3, b2, a); break;
    case OP_CND: snprintf(e, sizeof e, "select(%s, %s, %s > vec4<f32>(0.5))", c3, b2, a); break;
    case OP_DP2ADD: snprintf(e, sizeof e, "vec4<f32>(dot((%s).xy, (%s).xy) + (%s).x)", a, b2, c3); break;
    case OP_DSX: snprintf(e, sizeof e, "dpdx(%s)", a); break;
    case OP_DSY: snprintf(e, sizeof e, "dpdy(%s)", a); break;
    case OP_M4x4: case OP_M4x3: case OP_M3x4: case OP_M3x3: case OP_M3x2: {
        int rows = op == OP_M4x4 || op == OP_M3x4 ? 4 : op == OP_M3x2 ? 2 : 3;
        int four = op == OP_M4x4 || op == OP_M4x3;
        int o = snprintf(e, sizeof e, "vec4<f32>(");
        for (int k = 0; k < 4; k++) {
            if (k < rows) {
                char row[600];
                src_expr(t, &s[1], k, row, sizeof row);
                o += snprintf(e + o, sizeof e - o, "%sdot((%s)%s, (%s)%s)", k ? ", " : "", a, four ? "" : ".xyz", row, four ? "" : ".xyz");
            } else o += snprintf(e + o, sizeof e - o, ", 0.0");
        }
        snprintf(e + o, sizeof e - o, ")");
        break;
    }
    case OP_TEX: case OP_TEXLDL: case OP_TEXLDD: {
        char coord[600], x1[600] = "", x2[600] = "";
        if (t->major < 2) { fail(t, "pixel shader 1.x texture instructions"); return; }
        src_expr(t, &s[0], 0, coord, sizeof coord);
        if (op == OP_TEXLDD) { src_expr(t, &s[2], 0, x1, sizeof x1); src_expr(t, &s[3], 0, x2, sizeof x2); }
        const char* mode = op == OP_TEXLDL ? "l" : op == OP_TEXLDD ? "d" : (ctrl & 3) == 1 ? "p" : (ctrl & 3) == 2 ? "b" : "s";
        char smp[1700];
        sample_expr(t, s[1].num, coord, mode, x1, x2, smp, sizeof smp);
        if (s[1].swz != 0xE4) {
            char sw[5];
            for (int k = 0; k < 4; k++) sw[k] = comp[(s[1].swz >> (2 * k)) & 3];
            sw[4] = 0;
            snprintf(e, sizeof e, "(%s).%s", smp, sw);
        } else snprintf(e, sizeof e, "%s", smp);
        break;
    }
    default:
        fail(t, "opcode %u", op);
        return;
    }
    write_dst(t, d, e, pred, pp);
}

static void open_fn(Tr* t, const char* name)
{
    t->cur = &t->funcs;
    sp(t->cur, "fn %s() {\n", name);
    t->depth = 0;
}

static int run(Tr* t, int emit_code)
{
    size_t i = 1;
    t->cur = &t->body;
    t->depth = 0;
    t->in_label = 0;
    t->done_main = 0;
    t->loopn = 0;
    t->lsp = 0;
    while (i < t->n && !t->err) {
        uint32_t tok = t->t[i];
        uint32_t op = tok & 0xFFFF;
        if (op == OP_END) break;
        if (op == OP_COMMENT) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        i++;
        size_t len;
        if (t->major >= 2) len = (tok >> 24) & 0xF;
        else {
            int pc = param_count_sm1(op);
            if (pc < 0) { fail(t, "vs 1.x opcode %u", op); break; }
            len = (size_t)pc;
            if (op == OP_DCL) len = 2;
        }
        size_t end = i + len;
        if (end > t->n) { fail(t, "truncated instruction"); break; }
        uint32_t ctrl = (tok >> 16) & 0xFF;
        int pred = (tok >> 28) & 1;
        Param pp = { 0 };

        if (op == OP_DEF || op == OP_DEFI || op == OP_DEFB) {
            Param d = param(t, &i, 0);
            if (!emit_code) {
                if (op == OP_DEF && d.num < 256) { t->cdef[d.num] = 1; memcpy(t->cval[d.num], &t->t[i], 16); }
                if (op == OP_DEFI && d.num < 16) { t->idef[d.num] = 1; memcpy(t->ival[d.num], &t->t[i], 16); }
                if (op == OP_DEFB && d.num < 16) { t->bdef[d.num] = 1; t->bval[d.num] = t->t[i]; }
            }
            i = end;
            continue;
        }
        if (op == OP_DCL) {
            uint32_t u = t->t[i++];
            Param d = param(t, &i, 0);
            if (!emit_code) {
                if (d.type == RT_SAMPLER) {
                    uint32_t ty = (u >> 27) & 0xF;
                    t->samp_dim[d.num & 15] = ty == 3 ? 6 : ty == 4 ? 5 : 3;
                } else if (d.type == RT_INPUT) {
                    Slot* sl = &t->vin[d.num & 15];
                    sl->used = 1;
                    if (t->ps && t->major < 3) { sl->usage = 10; sl->index = d.num; }    /* ps 2.x v0/v1: colours */
                    else { sl->usage = u & 0x1F; sl->index = (u >> 16) & 0xF; }
                } else if (d.type == RT_ADDR && t->ps) {
                    /* ps 2.x t#: texture coordinates, recorded on input slots 8+ */
                    Slot* sl = &t->vin[8 + (d.num & 7)];
                    sl->used = 1; sl->usage = 5; sl->index = d.num;
                } else if (d.type == RT_OUTPUT && !t->ps) {
                    Slot* sl = &t->vout[d.num & 15];
                    sl->used = 1; sl->usage = u & 0x1F; sl->index = (u >> 16) & 0xF;
                } else if (d.type == RT_MISC) {
                    if (d.num == 0) t->uses_vpos = 1; else t->uses_vface = 1;
                }
            }
            i = end;
            continue;
        }
        if (op == OP_PHASE) { i = end; continue; }

        Param d = { 0 }, s[4];
        memset(s, 0, sizeof s);
        int ns = 0;
        int has_dst = !(op == OP_CALL || op == OP_CALLNZ || op == OP_LOOP || op == OP_RET || op == OP_ENDLOOP || op == OP_LABEL || op == OP_REP ||
                        op == OP_ENDREP || op == OP_IF || op == OP_IFC || op == OP_ELSE || op == OP_ENDIF || op == OP_BREAK || op == OP_BREAKC ||
                        op == OP_BREAKP || op == OP_NOP);
        if (op == OP_LOOP) has_dst = 0;
        if (has_dst) d = param(t, &i, 0);
        if (pred && i < end) pp = param(t, &i, 1);
        while (i < end && ns < 4) s[ns++] = param(t, &i, 1);
        i = end;
        if (!emit_code || t->err) continue;
        if (t->done_main && op != OP_LABEL && !t->in_label) continue;

        switch (op) {
        case OP_NOP: break;
        case OP_LABEL: {
            if (t->in_label || !t->done_main) {
                /* code after the main body without ret: close what is open */
                while (t->depth > 0) { t->depth--; line(t, "}"); }
                sp(t->cur, "}\n");
            }
            char nm[24];
            snprintf(nm, sizeof nm, "uew_l%u", s[0].num);
            open_fn(t, nm);
            t->in_label = 1;
            t->done_main = 1;
            break;
        }
        case OP_RET:
            if (t->depth == 0) {
                sp(t->cur, "}\n");
                if (!t->in_label) t->done_main = 1;
                t->in_label = 0;
            } else line(t, "return;");
            break;
        case OP_CALL: line(t, "uew_l%u();", s[0].num); break;
        case OP_CALLNZ: {
            char cnd[200];
            bool_expr(t, &s[1], cnd, sizeof cnd);
            line(t, "if (%s) { uew_l%u(); }", cnd, s[0].num);
            break;
        }
        case OP_IF: {
            char cnd[200];
            bool_expr(t, &s[0], cnd, sizeof cnd);
            line(t, "if (%s) {", cnd);
            t->depth++;
            break;
        }
        case OP_IFC: case OP_BREAKC: {
            char x[1024], y[1024];
            src_expr(t, &s[0], 0, x, sizeof x);
            src_expr(t, &s[1], 0, y, sizeof y);
            if (op == OP_IFC) { line(t, "if ((%s).x %s (%s).x) {", x, cmp_op(ctrl), y); t->depth++; }
            else line(t, "if ((%s).x %s (%s).x) { break; }", x, cmp_op(ctrl), y);
            break;
        }
        case OP_ELSE: t->depth--; line(t, "} else {"); t->depth++; break;
        case OP_ENDIF: t->depth--; line(t, "}"); break;
        case OP_BREAK: line(t, "break;"); break;
        case OP_BREAKP: {
            char cnd[200];
            bool_expr(t, &s[0], cnd, sizeof cnd);
            line(t, "if (%s) { break; }", cnd);
            break;
        }
        case OP_REP: case OP_LOOP: {
            /* loop aL, i#: i.x iterations, aL from i.y in steps of i.z; rep i#: i.x iterations. D3D9 caps both at 255 */
            const Param* ip = op == OP_LOOP ? &s[1] : &s[0];
            char iv[160];
            int_expr(t, ip->num, iv, sizeof iv);
            int k = t->loopn++;
            if (t->lsp >= 32) { fail(t, "loops nested too deep"); break; }
            t->lstack[t->lsp++] = k;
            line(t, "{");
            t->depth++;
            line(t, "let li%d_ = %s;", k, iv);
            if (op == OP_LOOP) { t->uses_aL = 1; line(t, "let al%d_ = aL;", k); line(t, "aL = li%d_.y;", k); }
            line(t, "for (var n%d_ = 0; n%d_ < min(li%d_.x, 255); n%d_++) {", k, k, k, k);
            t->depth++;
            break;
        }
        case OP_ENDLOOP: case OP_ENDREP: {
            if (t->lsp <= 0) { fail(t, "unbalanced loop"); break; }
            int k = t->lstack[--t->lsp];
            if (op == OP_ENDLOOP) line(t, "aL += li%d_.z;", k);
            t->depth--;
            line(t, "}");
            if (op == OP_ENDLOOP) line(t, "aL = al%d_;", k);
            t->depth--;
            line(t, "}");
            break;
        }
        case OP_TEXKILL: {
            char lv[96];
            Param q = d;
            dst_lvalue(t, &q, lv, sizeof lv);
            uint32_t m = d.mask ? d.mask : 0xF;
            if (t->major < 2) m &= 7;
            char cond[400];
            int o = 0;
            cond[0] = 0;
            for (int k = 0; k < 4; k++) if (m >> k & 1) o += snprintf(cond + o, sizeof cond - o, "%s%s.%c < 0.0", o ? " || " : "", lv, comp[k]);
            if (o) line(t, "if (%s) { discard; }", cond);
            break;
        }
        case OP_SETP: {
            char x[1024], y[1024], ex[2200], b[2600];
            src_expr(t, &s[0], 0, x, sizeof x);
            src_expr(t, &s[1], 0, y, sizeof y);
            snprintf(ex, sizeof ex, "(%s %s %s)", x, cmp_op(ctrl), y);
            t->uses_pred = 1;
            uint32_t m = d.mask ? d.mask : 0xF;
            int o = snprintf(b, sizeof b, "{ let t_ = %s;", ex);
            for (int k = 0; k < 4; k++) if (m >> k & 1) o += snprintf(b + o, sizeof b - o, " p0.%c = t_.%c;", comp[k], comp[k]);
            snprintf(b + o, sizeof b - o, " }");
            line(t, "%s", b);
            break;
        }
        case OP_TEXCOORD: case OP_BEM: case 67: case 68: case 69: case 70: case 71: case 72: case 73: case 74: case 76: case 77:
        case 82: case 83: case 84: case 85: case 86: case 87:
            fail(t, "pixel shader 1.x instruction %u", op);
            break;
        default:
            /* sincos in 2.x carries two constant sources that only the hardware of the day needed */
            if (op == OP_SINCOS && ns > 1) ns = 1;
            emit(t, op, ctrl, pred, &pp, &d, s, ns);
            break;
        }
    }
    if (emit_code && !t->err) {
        if (t->in_label || !t->done_main) {
            while (t->depth > 0) { t->depth--; line(t, "}"); }
            sp(t->cur, "}\n");
        }
    }
    return !t->err;
}

static int parse_header(Tr* t, const uint8_t* code, size_t size)
{
    if (size < 8 || (size & 3)) { fail(t, "not a D3D9 shader"); return 0; }
    t->t = (const uint32_t*)code;
    t->n = size / 4;
    uint32_t v = t->t[0];
    if ((v & 0xFFFE0000u) != 0xFFFE0000u) { fail(t, "not a D3D9 shader"); return 0; }
    t->ps = (v & 0xFFFF0000u) == 0xFFFF0000u;
    t->major = (int)((v >> 8) & 0xFF);
    t->minor = (int)(v & 0xFF);
    if (t->ps && t->major < 2) { fail(t, "pixel shader 1.%d is not supported", t->minor); return 0; }
    /* the integer-input trailer after END */
    for (size_t i = 1; i + 2 < t->n; i++)
        if (t->t[i] == 0x0000FFFFu && t->t[i + 1] == D3D9_INT_MAGIC) { t->int_u = t->t[i + 2]; t->int_s = i + 3 < t->n ? t->t[i + 3] : 0; break; }
    return 1;
}

static void sig_add(DxSig* s, const char* name, uint32_t index, uint32_t reg, uint32_t sysval)
{
    if (s->n >= 32) return;
    DxSigElem* e = &s->e[s->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->index = index;
    e->reg = reg;
    e->sysval = sysval;
    e->comp_type = 3;
    e->mask = 0xF;
    e->rw_mask = 0xF;
}

/* the location of a pixel shader input: where the paired vertex shader writes the same semantic */
static int ps_loc(const Tr* t, const Slot* sl, int fallback)
{
    const DxSig* vo = t->opt ? t->opt->vs_out : NULL;
    if (!vo) return fallback;
    const char* nm = usage_name(sl->usage);
    for (int k = 0; k < vo->n; k++)
        if (!vo->e[k].sysval && vo->e[k].index == sl->index && !strcmp(vo->e[k].name, nm)) return (int)vo->e[k].reg;
    return -1;
}

static const char HELPERS[] =
    "diagnostic(off, derivative_uniformity);\n"
    "fn uew_pow(x: f32, y: f32) -> f32 { return select(exp2(y * log2(abs(x))), 1.0, y == 0.0); }\n"
    "fn uew_nrm(v: vec4<f32>) -> vec4<f32> { let l = dot(v.xyz, v.xyz); return select(v * inverseSqrt(l), vec4<f32>(0.0), l == 0.0); }\n"
    "fn uew_lit(s: vec4<f32>) -> vec4<f32> { let d = max(s.x, 0.0); let sp = select(0.0, uew_pow(max(s.y, 0.0), clamp(s.w, -127.9961, 127.9961)), s.x > 0.0); return vec4<f32>(1.0, d, sp, 1.0); }\n";

int d3d9_to_wgsl(const uint8_t* code, size_t size, const DxWgslOpts* opt, DxWgsl* out)
{
    memset(out, 0, sizeof *out);
    Tr* t = calloc(1, sizeof *t);
    if (!t) return 0;
    t->out = out;
    t->opt = opt;
    int ok = parse_header(t, code, size) && run(t, 0) && run(t, 1);
    if (!ok) {
        snprintf(out->error, sizeof out->error, "D3D9 %s %d.%d: %s", t->ps ? "ps" : "vs", t->major, t->minor, t->why[0] ? t->why : "translation failed");
        free(t->body.p); free(t->funcs.p); free(t);
        return 0;
    }
    out->stage = t->ps ? 4 : 0;
    out->cb_size[0] = D3D9_CB_VEC4S;
    for (int s = 0; s < 16; s++)
        if (t->samp_dim[s]) {
            out->srv[s].used = 1;
            out->srv[s].dim = t->samp_dim[s];
            out->samp[s].used = 1;
        }

    SB w = { 0 };
    sp(&w, "%s", HELPERS);
    int grp = t->ps ? 2 : 0;
    sp(&w, "@group(%d) @binding(0) var<uniform> cb0: array<vec4<f32>, %d>;\n", grp, D3D9_CB_VEC4S);
    for (int s = 0; s < 16; s++) {
        if (!t->samp_dim[s]) continue;
        const char* ty = t->samp_dim[s] == 6 ? "texture_cube<f32>" : t->samp_dim[s] == 5 ? "texture_3d<f32>" : "texture_2d<f32>";
        sp(&w, "@group(%d) @binding(%d) var t%d: %s;\n@group(%d) @binding(%d) var s%d: sampler;\n", grp + 1, 16 + s, s, ty, grp + 1, 160 + s, s);
    }
    if (t->crel) {
        sp(&w, "fn uew_c(i: i32) -> vec4<f32> {\n    switch (i) {\n");
        for (int k = 0; k < 256; k++)
            if (t->cdef[k]) {
                char x[4][48];
                for (int j = 0; j < 4; j++) lit(x[j], sizeof x[j], t->cval[k][j]);
                sp(&w, "        case %d: { return vec4<f32>(%s, %s, %s, %s); }\n", k, x[0], x[1], x[2], x[3]);
            }
        sp(&w, "        default: { return cb0[clamp(i, 0, 255)]; }\n    }\n}\n");
    }
    for (int k = 0; k < (t->ntemp > 0 ? t->ntemp : 1); k++) sp(&w, "var<private> r%d: vec4<f32>;\n", k);
    sp(&w, "var<private> v: array<vec4<f32>, 16>;\n");
    if (t->ps) sp(&w, "var<private> tc: array<vec4<f32>, 8>;\nvar<private> oc: array<vec4<f32>, 4>;\nvar<private> odepth: f32;\n"
                     "var<private> vpos: vec4<f32>;\nvar<private> vface: vec4<f32>;\n");
    else sp(&w, "var<private> o: array<vec4<f32>, 16>;\nvar<private> opos: vec4<f32>;\n");
    sp(&w, "var<private> a0: vec4<i32>;\nvar<private> aL: i32;\nvar<private> aLf: vec4<f32>;\nvar<private> p0: vec4<bool>;\n");
    if (t->funcs.p) sp(&w, "%s", t->funcs.p);
    sp(&w, "fn uew_body() {\n%s", t->body.p ? t->body.p : "}\n");

    if (!t->ps) {
        /* SM3 position: the o register declared POSITION0; SM2 writes oPos (opos) */
        int pos_slot = -1;
        if (t->major >= 3)
            for (int k = 0; k < 16; k++) if (t->vout[k].used && (t->vout[k].usage == 0 || t->vout[k].usage == 9) && t->vout[k].index == 0) { pos_slot = k; break; }
        sp(&w, "struct VIn {\n");
        int nin = 0;
        for (int k = 0; k < 16; k++) {
            if (!t->vin[k].used) continue;
            const char* ty = (t->int_u >> k & 1) ? "vec4<u32>" : (t->int_s >> k & 1) ? "vec4<i32>" : "vec4<f32>";
            sp(&w, "    @location(%d) a%d: %s,\n", k, k, ty);
            sig_add(&out->in, usage_name(t->vin[k].usage), t->vin[k].index, (uint32_t)k, 0);
            nin++;
        }
        if (!nin) sp(&w, "    @builtin(vertex_index) vid: u32,\n");
        sp(&w, "}\nstruct VOut {\n    @builtin(position) @invariant pos: vec4<f32>,\n");
        sig_add(&out->out, "POSITION", 0, 0, 1);
        for (int k = 0; k < 16; k++) {
            if (k == pos_slot) continue;
            int used = t->major >= 3 ? t->vout[k].used : (int)(t->written_out >> k & 1);
            if (!used || t->vout[k].usage == 4) continue;     /* point size has no varying */
            sp(&w, "    @location(%d) l%d: vec4<f32>,\n", k, k);
            sig_add(&out->out, usage_name(t->vout[k].usage), t->vout[k].index, (uint32_t)k, 0);
        }
        sp(&w, "}\n@vertex fn main(i: VIn) -> VOut {\n");
        for (int k = 0; k < 16; k++) if (t->vin[k].used) sp(&w, "    v[%d] = vec4<f32>(i.a%d);\n", k, k);
        sp(&w, "    uew_body();\n    var r_: VOut;\n");
        if (pos_slot >= 0) sp(&w, "    let p_ = o[%d];\n", pos_slot);
        else sp(&w, "    let p_ = opos;\n");
        /* D3D9 puts pixel centres on integer coordinates: shift by half a pixel (cb extra .xy = -1/width, 1/height) */
        sp(&w, "    r_.pos = vec4<f32>(p_.xy + cb0[%d].xy * p_.w, p_.zw);\n", D3D9_CB_EXTRA);
        for (int k = 0; k < 16; k++) {
            if (k == pos_slot) continue;
            int used = t->major >= 3 ? t->vout[k].used : (int)(t->written_out >> k & 1);
            if (!used || t->vout[k].usage == 4) continue;
            sp(&w, "    r_.l%d = o[%d];\n", k, k);
        }
        sp(&w, "    return r_;\n}\n");
    } else {
        sp(&w, "struct FIn {\n    @builtin(position) pos: vec4<f32>,\n    @builtin(front_facing) ff: bool,\n");
        int loc[16];
        for (int k = 0; k < 16; k++) {
            loc[k] = -1;
            if (!t->vin[k].used) continue;
            /* fallback slots match what an SM2 vertex shader writes (see sm2_out_slot) */
            int fb = t->major >= 3 ? k : k >= 8 ? k - 8 : 10 + k;
            loc[k] = ps_loc(t, &t->vin[k], fb);
            if (loc[k] < 0) continue;
            int dup = 0;
            for (int j = 0; j < k; j++) if (loc[j] == loc[k]) dup = 1;
            if (!dup) sp(&w, "    @location(%d) l%d: vec4<f32>,\n", loc[k], loc[k]);
            sig_add(&out->in, usage_name(t->vin[k].usage), t->vin[k].index, (uint32_t)loc[k], 0);
        }
        sp(&w, "}\nstruct FOut {\n");
        int nc = t->ocol ? t->ocol : 1;
        for (int k = 0; k < nc; k++) {
            sp(&w, "    @location(%d) c%d: vec4<f32>,\n", k, k);
            sig_add(&out->out, "SV_Target", (uint32_t)k, (uint32_t)k, 0);
        }
        if (t->odepth) { sp(&w, "    @builtin(frag_depth) d: f32,\n"); out->ps_depth = 1; }
        sp(&w, "}\n@fragment fn main(i: FIn) -> FOut {\n");
        sp(&w, "    vpos = vec4<f32>(i.pos.xy - vec2<f32>(0.5), 0.0, 0.0);\n    vface = vec4<f32>(select(-1.0, 1.0, i.ff));\n");
        for (int k = 0; k < 16; k++) {
            if (loc[k] < 0) continue;
            int is_tc = t->major < 3 && k >= 8;
            /* ps 2.x colour inputs arrive clamped to [0, 1] */
            if (is_tc) sp(&w, "    tc[%d] = i.l%d;\n", k - 8, loc[k]);
            else if (t->major < 3) sp(&w, "    v[%d] = clamp(i.l%d, vec4<f32>(0.0), vec4<f32>(1.0));\n", k, loc[k]);
            else sp(&w, "    v[%d] = i.l%d;\n", k, loc[k]);
        }
        sp(&w, "    odepth = i.pos.z;\n    uew_body();\n");
        /* alpha test, which D3D9 applies after the shader: cb extra .x = D3DCMPFUNC (8 = always), .y = reference */
        sp(&w, "    let af_ = i32(cb0[%d].x); let ar_ = cb0[%d].y; let aa_ = oc[0].w;\n", D3D9_CB_EXTRA, D3D9_CB_EXTRA);
        sp(&w, "    if (af_ != 8 && !((af_ == 2 && aa_ < ar_) || (af_ == 3 && aa_ == ar_) || (af_ == 4 && aa_ <= ar_) || (af_ == 5 && aa_ > ar_) || "
               "(af_ == 6 && aa_ != ar_) || (af_ == 7 && aa_ >= ar_))) { discard; }\n");
        sp(&w, "    var r_: FOut;\n");
        for (int k = 0; k < nc; k++) sp(&w, "    r_.c%d = oc[%d];\n", k, k);
        if (t->odepth) sp(&w, "    r_.d = clamp(odepth, 0.0, 1.0);\n");
        sp(&w, "    return r_;\n}\n");
    }
    out->nout = t->ps ? (t->ocol ? t->ocol : 1) : out->out.n;
    out->wgsl = w.p;
    free(t->body.p);
    free(t->funcs.p);
    free(t);
    return 1;
}

int d3d9_reflect(const uint8_t* code, size_t size, DxWgsl* out)
{
    int r = d3d9_to_wgsl(code, size, NULL, out);
    free(out->wgsl);
    out->wgsl = NULL;
    return r;
}

/* the usage and usage index each input register of a vertex shader is declared with (for vertex declarations) */
int d3d9_vs_inputs(const uint8_t* code, size_t size, uint8_t usage[16], uint8_t index[16])
{
    memset(usage, 0xFF, 16);
    memset(index, 0, 16);
    if (size < 8 || (size & 3)) return 0;
    const uint32_t* t = (const uint32_t*)code;
    size_t n = size / 4;
    if ((t[0] & 0xFFFF0000u) != 0xFFFE0000u) return 0;
    int major = (int)((t[0] >> 8) & 0xFF);
    for (size_t i = 1; i < n;) {
        uint32_t tok = t[i], op = tok & 0xFFFF;
        if (op == OP_END) break;
        if (op == OP_COMMENT) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        size_t len = major >= 2 ? (tok >> 24) & 0xF : (size_t)(op == OP_DCL ? 2 : param_count_sm1(op) < 0 ? 0 : param_count_sm1(op));
        if (op == OP_DCL && i + 2 < n && reg_type(t[i + 2]) == RT_INPUT) {
            uint32_t r = t[i + 2] & 15;
            usage[r] = (uint8_t)(t[i + 1] & 0x1F);
            index[r] = (uint8_t)((t[i + 1] >> 16) & 0xF);
        }
        i += 1 + len;
    }
    return 1;
}
