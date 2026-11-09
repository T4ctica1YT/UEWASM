#include "dxbc_parse.h"

#include <math.h>
#define NL "\n"

enum { K_F, K_I, K_U };
static const char* C4 = "xyzw";

typedef struct {
    DxWgsl* sh;
    Str body;
    int stage, indent, temps;
    int tex_dim[128], tex_ret[128];
    int uav_dim[8], uav_ret[8], uav_raw[8], uav_stride[8], uav_atomic[8], uav_read[8];
    int g_size[32], g_atomic[32], g_stride[32];
    int idx_size[16];
    int switch_open[32], switch_depth, case_open[32];
    int nloops;
    int ps_depth;
    int sat;   /* _sat on the instruction being translated: clamps every float result it writes */
    char in_interp[32];
    /* two-result instructions (udiv, imul, umul, sincos, swapc) read all sources before writing either result */
    int defer, ndefer;
    Op* defer_dst[2];
} Tw;

/* D3D interpolation mode (dcl_input_ps bits 11-14) as a WGSL attribute */
static const char* interp_attr(int m)
{
    switch (m) {
    case 1: return " @interpolate(flat)";
    case 3: return " @interpolate(perspective, centroid)";
    case 4: return " @interpolate(linear)";
    case 5: return " @interpolate(linear, centroid)";
    case 6: return " @interpolate(perspective, sample)";
    case 7: return " @interpolate(linear, sample)";
    default: return "";
    }
}

static const char* vt(int kind, int n)
{
    static const char* t[3][5] = { { "", "f32", "vec2<f32>", "vec3<f32>", "vec4<f32>" },
                                   { "", "i32", "vec2<i32>", "vec3<i32>", "vec4<i32>" },
                                   { "", "u32", "vec2<u32>", "vec3<u32>", "vec4<u32>" } };
    return t[kind][n];
}

static void flit(char* b, uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    uint32_t e = (bits >> 23) & 0xFF;
    if (e == 0xFF) { sprintf(b, "uew_bits(%uu)", bits); return; }
    if (e == 0 && (bits & 0x7FFFFF)) { sprintf(b, "bitcast<f32>(%uu)", bits); return; }
    char t[48];
    sprintf(t, "%.9g", f);
    float back = strtof(t, NULL);
    uint32_t bb;
    memcpy(&bb, &back, 4);
    if (bb != bits) { sprintf(b, "bitcast<f32>(%uu)", bits); return; }
    if (!strchr(t, '.') && !strchr(t, 'e')) strcat(t, ".0");
    sprintf(b, "f32(%s)", t);
}

static void lit(char* b, uint32_t v, int kind)
{
    if (kind == K_F) flit(b, v);
    else if (kind == K_I) { if (v == 0x80000000u) sprintf(b, "bitcast<i32>(0x80000000u)"); else sprintf(b, "%di", (int32_t)v); }
    else sprintf(b, "%uu", v);
}

static void base(Tw* tr, Str* s, Op* o);

static void idx(Tw* tr, Str* s, Op* o, int i)
{
    if (o->idx[i].rel) {
        Op* rel = o->idx[i].rel;
        sp(s, "(bitcast<i32>(");
        base(tr, s, rel);
        sp(s, ".%c)", C4[rel->swz[0]]);
        if (o->idx[i].imm) sp(s, " + %di", (int)o->idx[i].imm);
        sp(s, ")");
    } else sp(s, "%u", o->idx[i].imm);
}

static void base(Tw* tr, Str* s, Op* o)
{
    switch (o->type) {
    case T_TEMP: sp(s, "r%u", o->idx[0].imm); break;
    case T_INPUT:
        if (tr->stage == 2 && o->dims == 2) { sp(s, "v["); idx(tr, s, o, 0); sp(s, "]["); idx(tr, s, o, 1); sp(s, "]"); }
        else { sp(s, "v["); idx(tr, s, o, o->dims == 2 ? 1 : 0); sp(s, "]"); }
        break;
    case T_PRIMID: sp(s, "vprim"); break;
    case T_GSINST: sp(s, "vgsi"); break;
    case T_OUTPUT: sp(s, "o%u", o->idx[0].imm); break;
    case T_INDEXABLE: sp(s, "x%u[", o->idx[0].imm); idx(tr, s, o, 1); sp(s, "]"); break;
    case T_CB: sp(s, "cb%u[", o->idx[0].imm); idx(tr, s, o, 1); sp(s, "]"); break;
    case T_ICB: sp(s, "bitcast<vec4<f32>>(icb["); idx(tr, s, o, 0); sp(s, "])"); break;
    case T_ODEPTH: case T_ODEPTH_GE: case T_ODEPTH_LE: sp(s, "vec4<f32>(odepth)"); break;
    case T_TID: sp(s, "vtid"); break;
    case T_GID: sp(s, "vgid"); break;
    case T_TIDG: sp(s, "vtidg"); break;
    case T_TIDGF: sp(s, "vtidf"); break;
    case T_COVERAGE: sp(s, "vec4<f32>(bitcast<f32>(0xFFFFFFFFu))"); break;
    default: sp(s, "vec4<f32>(0.0)"); break;
    }
}

static void conv_from_f(Str* s, int kind, int n, const char* e)
{
    if (kind == K_F) sp(s, "%s", e);
    else sp(s, "bitcast<%s>(%s)", vt(kind, n), e);
}

static void src(Tw* tr, Str* s, Op* o, int dmask, int nfixed, int kind)
{
    int sel[4], n = 0;
    if (dmask >= 0) { for (int i = 0; i < 4; i++) if (dmask & (1 << i)) sel[n++] = o->swz[i]; }
    else { for (int i = 0; i < nfixed; i++) sel[n++] = o->swz[i]; }
    if (!n) { sel[0] = 0; n = 1; }
    if (o->type == T_IMM32 || o->type == T_IMM64) {
        char b[64];
        int neg = o->neg && kind != K_U;
        if (neg) sp(s, "(-");
        if (n > 1) sp(s, "%s(", vt(kind, n));
        for (int i = 0; i < n; i++) { lit(b, o->imm[sel[i] & 3], kind); sp(s, "%s%s", i ? ", " : "", b); }
        if (n > 1) sp(s, ")");
        if (neg) sp(s, ")");
        return;
    }
    Str t = { 0 };
    base(tr, &t, o);
    if (o->type == T_ODEPTH || o->type == T_ODEPTH_GE || o->type == T_ODEPTH_LE) { sp(s, "%s", n == 1 ? "odepth" : "vec4<f32>(odepth)"); free(t.p); return; }
    sp(&t, ".");
    for (int i = 0; i < n; i++) sp(&t, "%c", C4[sel[i]]);
    Str c = { 0 };
    conv_from_f(&c, kind, n, t.p);
    if (o->absm) { Str a = { 0 }; sp(&a, "abs(%s)", c.p); free(c.p); c = a; }
    if (o->neg) {
        if (kind == K_U) sp(s, "(%s(0u) - %s)", vt(K_U, n), c.p);
        else sp(s, "(-%s)", c.p);
    } else sp(s, "%s", c.p);
    free(t.p);
    free(c.p);
}

static void ind(Tw* tr) { for (int i = 0; i <= tr->indent; i++) sp(&tr->body, "    "); }

static int dm(Op* d) { return d->type == T_NULL ? 1 : (d->mask ? d->mask : 1); }

static void assign(Tw* tr, Op* d, int kind, int sat, const char* expr)
{
    if (d->type == T_NULL || d->type == T_OMASK) return;
    int m = dm(d), n = popc4(m);
    Str v = { 0 };
    if ((sat || tr->sat) && kind == K_F) sp(&v, "saturate(%s)", expr);
    else sp(&v, "%s", expr);
    Str f = { 0 };
    if (kind == K_F) sp(&f, "%s", v.p);
    else sp(&f, "bitcast<%s>(%s)", vt(K_F, n), v.p);
    if (tr->defer && tr->ndefer < 2) {
        ind(tr);
        sp(&tr->body, "let q%d_ = %s;\n", tr->ndefer, f.p);
        tr->defer_dst[tr->ndefer++] = d;
        free(v.p);
        free(f.p);
        return;
    }
    ind(tr);
    if (d->type == T_ODEPTH || d->type == T_ODEPTH_GE || d->type == T_ODEPTH_LE) {
        sp(&tr->body, "odepth = %s%s;\n", f.p, n > 1 ? ".x" : "");
    } else {
        Str b = { 0 };
        base(tr, &b, d);
        if (n == 1) {
            int c = 0;
            while (!(m & (1 << c))) c++;
            sp(&tr->body, "%s.%c = %s;\n", b.p, C4[c], f.p);
        } else if (n == 4) {
            sp(&tr->body, "%s = %s;\n", b.p, f.p);
        } else {
            sp(&tr->body, "{ let t_ = %s;", f.p);
            for (int c = 0, j = 0; c < 4; c++) if (m & (1 << c)) sp(&tr->body, " %s.%c = t_.%c;", b.p, C4[c], C4[j++]);
            sp(&tr->body, " }\n");
        }
        free(b.p);
    }
    free(v.p);
    free(f.p);
}

static void bin(Tw* tr, Op** o, int sat, int kind, const char* op, int fn)
{
    int m = dm(o[0]);
    Str e = { 0 };
    if (fn) { sp(&e, "%s(", op); src(tr, &e, o[1], m, 0, kind); sp(&e, ", "); src(tr, &e, o[2], m, 0, kind); sp(&e, ")"); }
    else { sp(&e, "("); src(tr, &e, o[1], m, 0, kind); sp(&e, " %s ", op); src(tr, &e, o[2], m, 0, kind); sp(&e, ")"); }
    assign(tr, o[0], kind, sat, e.p);
    free(e.p);
}

static void un(Tw* tr, Op** o, int sat, int kin, int kout, const char* fn)
{
    int m = dm(o[0]);
    Str e = { 0 };
    sp(&e, "%s(", fn);
    src(tr, &e, o[1], m, 0, kin);
    sp(&e, ")");
    assign(tr, o[0], kout, sat, e.p);
    free(e.p);
}

static void defer_begin(Tw* tr)
{
    ind(tr);
    sp(&tr->body, "{\n");
    tr->indent++;
    tr->defer = 1;
    tr->ndefer = 0;
}

static void defer_end(Tw* tr)
{
    int sat = tr->sat;
    tr->defer = 0;
    tr->sat = 0;
    for (int i = 0; i < tr->ndefer; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "q%d_", i);
        assign(tr, tr->defer_dst[i], K_F, 0, nm);
    }
    tr->sat = sat;
    tr->indent--;
    ind(tr);
    sp(&tr->body, "}\n");
}

static void cmpop(Tw* tr, Op** o, int kind, const char* op)
{
    int m = dm(o[0]), n = popc4(m);
    Str e = { 0 };
    if (n == 1) sp(&e, "select(0u, 0xFFFFFFFFu, ");
    else sp(&e, "select(%s(0u), %s(0xFFFFFFFFu), ", vt(K_U, n), vt(K_U, n));
    src(tr, &e, o[1], m, 0, kind);
    sp(&e, " %s ", op);
    src(tr, &e, o[2], m, 0, kind);
    sp(&e, ")");
    assign(tr, o[0], K_U, 0, e.p);
    free(e.p);
}

static void shift(Tw* tr, Op** o, int kind, const char* op)
{
    int m = dm(o[0]), n = popc4(m);
    Str e = { 0 };
    sp(&e, "(");
    src(tr, &e, o[1], m, 0, kind);
    sp(&e, " %s (", op);
    src(tr, &e, o[2], m, 0, K_U);
    sp(&e, " & %s(31u)))", n == 1 ? "u32" : vt(K_U, n));
    assign(tr, o[0], kind, 0, e.p);
    free(e.p);
}

static void per_comp(Tw* tr, Op** o, const char* fn, const int* kinds, int nargs, int kout, const char* suffix)
{
    int m = dm(o[0]);
    Str e = { 0 };
    int n = popc4(m), first = 1;
    if (n > 1) sp(&e, "%s(", vt(kout, n));
    for (int i = 0; i < 4; i++) {
        if (!(m & (1 << i))) continue;
        if (!first) sp(&e, ", ");
        first = 0;
        sp(&e, "%s(", fn);
        for (int a = 0; a < nargs; a++) { if (a) sp(&e, ", "); src(tr, &e, o[1 + a], 1 << i, 0, kinds[a]); }
        sp(&e, ")%s", suffix ? suffix : "");
    }
    if (n > 1) sp(&e, ")");
    assign(tr, o[0], kout, 0, e.p);
    free(e.p);
}

/* D3D resource dimension: 1 buffer, 2 1d, 3 2d, 4 2dms, 5 3d, 6 cube, 7 1darray, 8 2darray, 9 2dmsarray, 10 cubearray */
static int is_array(int dim) { return dim == 7 || dim == 8 || dim == 9 || dim == 10; }
static int base_coords(int dim) { switch (dim) { case 1: case 2: case 7: return 1; case 5: case 6: case 10: return 3; default: return 2; } }

static void coords(Tw* tr, Str* s, Op* addr, int dim, int kind)
{
    int n = base_coords(dim);
    if (dim == 7) { sp(s, "vec2<f32>("); src(tr, s, addr, -1, 1, kind); sp(s, ", 0.5)"); return; }
    src(tr, s, addr, -1, n, kind);
}

static void layer(Tw* tr, Str* s, Op* addr, int dim, int as_float)
{
    Op t = *addr;
    t.swz[0] = addr->swz[base_coords(dim)];
    if (as_float) { sp(s, "i32(round("); src(tr, s, &t, -1, 1, K_F); sp(s, "))"); }
    else src(tr, s, &t, -1, 1, K_I);
}

static void offs(Str* s, int dim, int off[3])
{
    if (dim == 6 || dim == 10) return;
    if (dim == 5) sp(s, ", vec3<i32>(%d, %d, %d)", off[0], off[1], off[2]);
    else if (dim == 2) return;
    else sp(s, ", vec2<i32>(%d, %d)", off[0], off[1]);
}

static void tex_out(Tw* tr, Op* d, Op* res, const char* call, int depth)
{
    int m = dm(d);
    Str e = { 0 };
    if (depth) sp(&e, "vec4<f32>(%s, 0.0, 0.0, 1.0).", call);
    else {
        int ret = tr->tex_ret[res->idx[0].imm & 127];
        if (ret) sp(&e, "bitcast<vec4<f32>>(%s).", call);
        else sp(&e, "(%s).", call);
    }
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&e, "%c", C4[res->swz[i]]);
    assign(tr, d, K_F, 0, e.p);
    free(e.p);
}

static void emit_sample(Tw* tr, int opc, Op** o, int off[3], int has_off)
{
    Op *d = o[0], *addr = o[1], *res = o[2], *smp = o[3];
    int t = (int)res->idx[0].imm & 127, s = (int)smp->idx[0].imm & 15;
    int dim = tr->tex_dim[t];
    int cmp = opc == 70 || opc == 71;
    tr->sh->srv[t].used = 1;
    tr->sh->samp[s].used = 1;
    if (cmp) { tr->sh->srv[t].compare = 1; tr->sh->samp[s].compare = 1; }
    /* WebGPU only pairs depth textures with non-filtering samplers outside comparisons */
    if (tr->sh->srv[t].compare && !cmp) tr->sh->samp[s].nofilter = 1;
    int depth = tr->sh->srv[t].compare;
    int frag = tr->stage == 0;
    Str c = { 0 }, a = { 0 }, call = { 0 };
    coords(tr, &c, addr, dim, K_F);
    if (is_array(dim)) { sp(&a, ", "); layer(tr, &a, addr, dim, 1); }
    char of[64] = "";
    if (has_off) { Str x = { 0 }; offs(&x, dim, off); if (x.p) snprintf(of, sizeof of, "%s", x.p); free(x.p); }
    if (depth && !cmp && dim != 6 && dim != 10) {
        /* WebGPU won't filter depth textures, and D3D reads them unfiltered anyway: load the texel */
        Str l = { 0 };
        if (opc == 72) { Str x = { 0 }; src(tr, &x, o[4], -1, 1, K_F); sp(&l, "i32(%s)", x.p); free(x.p); }
        else sp(&l, "0");
        sp(&call, "textureLoad(t%d, vec2<i32>(floor((%s) * vec2<f32>(textureDimensions(t%d, %s))))%s%s%s, %s)", t, c.p, t, l.p,
           has_off && of[0] ? " + " : "", has_off && of[0] ? of + 2 : "", a.p ? a.p : "", l.p);
        free(l.p);
        tex_out(tr, d, res, call.p, depth);
        free(c.p); free(a.p); free(call.p);
        return;
    }
    switch (opc) {
    case 69:
        if (frag) sp(&call, "textureSample(t%d, s%d, %s%s%s)", t, s, c.p, a.p ? a.p : "", of);
        else sp(&call, "textureSampleLevel(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", depth ? "0" : "0.0", of);
        break;
    case 74: {
        Str b = { 0 };
        src(tr, &b, o[4], -1, 1, K_F);
        if (frag && !depth) sp(&call, "textureSampleBias(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", b.p, of);
        else sp(&call, "textureSampleLevel(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", depth ? "0" : "0.0", of);
        free(b.p);
        break;
    }
    case 72: {
        Str l = { 0 };
        src(tr, &l, o[4], -1, 1, K_F);
        if (depth) sp(&call, "textureSampleLevel(t%d, s%d, %s%s, i32(%s)%s)", t, s, c.p, a.p ? a.p : "", l.p, of);
        else sp(&call, "textureSampleLevel(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", l.p, of);
        free(l.p);
        break;
    }
    case 73: {
        int n = base_coords(dim);
        Str dx = { 0 }, dy = { 0 };
        src(tr, &dx, o[4], -1, n, K_F);
        src(tr, &dy, o[5], -1, n, K_F);
        if (depth) sp(&call, "textureSampleLevel(t%d, s%d, %s%s, 0%s)", t, s, c.p, a.p ? a.p : "", of);
        else sp(&call, "textureSampleGrad(t%d, s%d, %s%s, %s, %s%s)", t, s, c.p, a.p ? a.p : "", dx.p, dy.p, of);
        free(dx.p); free(dy.p);
        break;
    }
    case 70: case 71: {
        Str r = { 0 };
        src(tr, &r, o[4], -1, 1, K_F);
        if (opc == 70 && frag) sp(&call, "textureSampleCompare(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", r.p, of);
        else sp(&call, "textureSampleCompareLevel(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", r.p, of);
        free(r.p);
        int m = dm(d), n = popc4(m);
        Str e = { 0 };
        if (n == 1) sp(&e, "%s", call.p);
        else sp(&e, "%s(%s)", vt(K_F, n), call.p);
        assign(tr, d, K_F, 0, e.p);
        free(e.p); free(c.p); free(a.p); free(call.p);
        return;
    }
    }
    tex_out(tr, d, res, call.p, depth);
    free(c.p); free(a.p); free(call.p);
}

static void emit_gather(Tw* tr, int opc, Op** o, int off[3], int has_off)
{
    int po = opc == 127 || opc == 128, cmp = opc == 126 || opc == 128;
    Op *d = o[0], *addr = o[1], *res = o[po ? 3 : 2], *smp = o[po ? 4 : 3];
    int t = (int)res->idx[0].imm & 127, s = (int)smp->idx[0].imm & 15;
    int dim = tr->tex_dim[t];
    tr->sh->srv[t].used = 1;
    tr->sh->samp[s].used = 1;
    if (cmp) { tr->sh->srv[t].compare = 1; tr->sh->samp[s].compare = 1; }
    /* WebGPU only pairs depth textures with non-filtering samplers outside comparisons */
    if (tr->sh->srv[t].compare && !cmp) tr->sh->samp[s].nofilter = 1;
    Str c = { 0 }, a = { 0 }, call = { 0 };
    coords(tr, &c, addr, dim, K_F);
    if (po) {
        Str p = { 0 };
        sp(&p, "(%s + vec2<f32>(", c.p);
        src(tr, &p, o[2], -1, 2, K_I);
        sp(&p, ") / vec2<f32>(textureDimensions(t%d)))", t);
        free(c.p);
        c = p;
    }
    if (is_array(dim)) { sp(&a, ", "); layer(tr, &a, addr, dim, 1); }
    char of[64] = "";
    if (has_off && !po) { Str x = { 0 }; offs(&x, dim, off); if (x.p) snprintf(of, sizeof of, "%s", x.p); free(x.p); }
    if (cmp) {
        Str r = { 0 };
        src(tr, &r, o[po ? 5 : 4], -1, 1, K_F);
        sp(&call, "textureGatherCompare(t%d, s%d, %s%s, %s%s)", t, s, c.p, a.p ? a.p : "", r.p, of);
        free(r.p);
    } else if (tr->sh->srv[t].compare) sp(&call, "textureGather(t%d, s%d, %s%s%s)", t, s, c.p, a.p ? a.p : "", of);
    else sp(&call, "textureGather(%d, t%d, s%d, %s%s%s)", smp->swz[0], t, s, c.p, a.p ? a.p : "", of);
    int m = dm(d);
    Str e = { 0 };
    int ret = tr->tex_ret[t];
    if (ret && !cmp) sp(&e, "bitcast<vec4<f32>>(%s).", call.p);
    else sp(&e, "(%s).", call.p);
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&e, "%c", C4[res->swz[i]]);
    assign(tr, d, K_F, 0, e.p);
    free(e.p); free(c.p); free(a.p); free(call.p);
}

static void emit_ld(Tw* tr, Op** o, int off[3], int has_off, int ms)
{
    Op *d = o[0], *addr = o[1], *res = o[2];
    int t = (int)res->idx[0].imm & 127;
    int dim = tr->tex_dim[t];
    tr->sh->srv[t].used = 1;
    Str call = { 0 };
    if (dim == 1) {
        tr->sh->srv[t].buffer = 1;
        Str i = { 0 };
        src(tr, &i, addr, -1, 1, K_I);
        sp(&call, "t%d[%s]", t, i.p);
        free(i.p);
        int m = dm(d);
        Str e = { 0 };
        sp(&e, "bitcast<vec4<f32>>(%s).", call.p);
        for (int k = 0; k < 4; k++) if (m & (1 << k)) sp(&e, "%c", C4[res->swz[k]]);
        assign(tr, d, K_F, 0, e.p);
        free(e.p); free(call.p);
        return;
    }
    int n = base_coords(dim);
    Str c = { 0 }, a = { 0 }, l = { 0 };
    if (dim == 7) { sp(&c, "vec2<i32>("); src(tr, &c, addr, -1, 1, K_I); sp(&c, ", 0)"); }
    else src(tr, &c, addr, -1, n, K_I);
    if (has_off) {
        Str x = { 0 };
        if (n == 1) sp(&x, "(%s + %d)", c.p, off[0]);
        else if (n == 2) sp(&x, "(%s + vec2<i32>(%d, %d))", c.p, off[0], off[1]);
        else sp(&x, "(%s + vec3<i32>(%d, %d, %d))", c.p, off[0], off[1], off[2]);
        free(c.p);
        c = x;
    }
    if (is_array(dim)) { sp(&a, ", "); layer(tr, &a, addr, dim, 0); }
    /* render targets are single-sampled here, so a multisampled texture is read as its one sample */
    if (ms) sp(&l, "0");
    else { Op lo = *addr; lo.swz[0] = addr->swz[n + (is_array(dim) ? 1 : 0) < 4 ? n + (is_array(dim) ? 1 : 0) : 3]; src(tr, &l, &lo, -1, 1, K_I); }
    sp(&call, "textureLoad(t%d, %s%s, %s)", t, c.p, a.p ? a.p : "", l.p);
    tex_out(tr, d, res, call.p, tr->sh->srv[t].compare);
    free(c.p); free(a.p); free(l.p); free(call.p);
}

static void emit_resinfo(Tw* tr, Op** o, int rtype)
{
    Op *d = o[0], *res = o[2];
    int t = (int)res->idx[0].imm & 127;
    int dim = tr->tex_dim[t];
    tr->sh->srv[t].used = 1;
    Str l = { 0 }, e = { 0 };
    src(tr, &l, o[1], -1, 1, K_U);
    const char* lv = dim == 4 || dim == 9 ? "" : l.p;
    const char* sep = *lv ? ", " : "";
    switch (dim) {
    case 2: sp(&e, "vec4<u32>(textureDimensions(t%d%s%s), 0u, 0u, textureNumLevels(t%d))", t, sep, lv, t); break;
    case 7: sp(&e, "vec4<u32>(textureDimensions(t%d%s%s).x, textureNumLayers(t%d), 0u, textureNumLevels(t%d))", t, sep, lv, t, t); break;
    case 5: sp(&e, "vec4<u32>(textureDimensions(t%d%s%s), textureNumLevels(t%d))", t, sep, lv, t); break;
    case 8: case 10: sp(&e, "vec4<u32>(textureDimensions(t%d%s%s), textureNumLayers(t%d), textureNumLevels(t%d))", t, sep, lv, t, t); break;
    case 4: case 9: sp(&e, "vec4<u32>(textureDimensions(t%d), 1u, 1u)", t); break;
    default: sp(&e, "vec4<u32>(textureDimensions(t%d%s%s), 1u, textureNumLevels(t%d))", t, sep, lv, t); break;
    }
    Str full = { 0 };
    int m = dm(d), n = popc4(m);
    if (rtype == 2) sp(&full, "(%s).", e.p);
    else if (rtype == 1) sp(&full, "(vec4<f32>(1.0) / vec4<f32>(%s)).", e.p);
    else sp(&full, "vec4<f32>(%s).", e.p);
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&full, "%c", C4[res->swz[i]]);
    (void)n;
    assign(tr, d, rtype == 2 ? K_U : K_F, 0, full.p);
    free(l.p); free(e.p); free(full.p);
}

static void buf_name(Str* s, Op* r) { sp(s, "%c%u", r->type == T_UAV ? 'u' : r->type == T_TGSM ? 'g' : 't', r->idx[0].imm); }

static int buf_atomic(Tw* tr, Op* r)
{
    if (r->type == T_UAV) return tr->uav_atomic[r->idx[0].imm & 7];
    if (r->type == T_TGSM) return tr->g_atomic[r->idx[0].imm & 31];
    return 0;
}

static void word_addr(Tw* tr, Str* s, Op** o, int structured, int ai, Op* r)
{
    int stride = r->type == T_UAV ? tr->uav_stride[r->idx[0].imm & 7] : r->type == T_TGSM ? tr->g_stride[r->idx[0].imm & 31] : tr->sh->srv[r->idx[0].imm & 127].stride;
    if (structured) { sp(s, "((u32("); src(tr, s, o[ai], -1, 1, K_I); sp(s, ") * %du + u32(", stride); src(tr, s, o[ai + 1], -1, 1, K_I); sp(s, ")) >> 2u)"); }
    else { sp(s, "(u32("); src(tr, s, o[ai], -1, 1, K_I); sp(s, ") >> 2u)"); }
}

static void mark_buf(Tw* tr, Op* r, int write)
{
    if (r->type == T_UAV) { tr->sh->uav[r->idx[0].imm & 7].used = 1; if (write) tr->sh->uav[r->idx[0].imm & 7].written = 1; }
    else if (r->type == T_RESOURCE) tr->sh->srv[r->idx[0].imm & 127].used = 1;
}

static void emit_ld_buf(Tw* tr, Op** o, int structured)
{
    Op *d = o[0], *r = o[structured ? 3 : 2];
    mark_buf(tr, r, 0);
    Str a = { 0 }, nm = { 0 }, e = { 0 };
    word_addr(tr, &a, o, structured, 1, r);
    buf_name(&nm, r);
    int m = dm(d), n = popc4(m), first = 1, at = buf_atomic(tr, r);
    if (n > 1) sp(&e, "vec%d<u32>(", n);
    for (int i = 0; i < 4; i++) {
        if (!(m & (1 << i))) continue;
        if (!first) sp(&e, ", ");
        first = 0;
        if (at) sp(&e, "atomicLoad(&%s[%s + %du])", nm.p, a.p, r->swz[i]);
        else sp(&e, "%s[%s + %du]", nm.p, a.p, r->swz[i]);
    }
    if (n > 1) sp(&e, ")");
    assign(tr, d, K_U, 0, e.p);
    free(a.p); free(nm.p); free(e.p);
}

static void emit_store_buf(Tw* tr, Op** o, int structured)
{
    Op* r = o[0];
    mark_buf(tr, r, 1);
    Str a = { 0 }, nm = { 0 };
    word_addr(tr, &a, o, structured, 1, r);
    buf_name(&nm, r);
    Op* v = o[structured ? 3 : 2];
    int m = r->mask ? r->mask : 1, at = buf_atomic(tr, r);
    for (int i = 0, j = 0; i < 4; i++) {
        if (!(m & (1 << i))) continue;
        Str x = { 0 };
        src(tr, &x, v, 1 << i, 0, K_U);
        ind(tr);
        if (at) sp(&tr->body, "atomicStore(&%s[%s + %du], %s);\n", nm.p, a.p, j, x.p);
        else sp(&tr->body, "%s[%s + %du] = %s;\n", nm.p, a.p, j, x.p);
        j++;
        free(x.p);
    }
    free(a.p); free(nm.p);
}

static const char* atomic_fn(int opc)
{
    switch (opc) {
    case 169: case 181: return "atomicAnd";
    case 170: case 182: return "atomicOr";
    case 171: case 183: return "atomicXor";
    case 173: case 180: return "atomicAdd";
    case 174: case 186: return "atomicMax";
    case 175: case 187: return "atomicMin";
    case 176: case 188: return "atomicMax";
    case 177: case 189: return "atomicMin";
    case 184: return "atomicExchange";
    default: return NULL;
    }
}

static void emit_atomic(Tw* tr, int opc, Op** o)
{
    int imm = opc >= 178;
    Op* r = o[imm ? 1 : 0];
    mark_buf(tr, r, 1);
    Str a = { 0 }, nm = { 0 }, call = { 0 };
    int structured = (r->type == T_UAV && tr->uav_stride[r->idx[0].imm & 7]) || (r->type == T_TGSM && tr->g_stride[r->idx[0].imm & 31]);
    Op* ao = o[imm ? 2 : 1];
    if (structured) {
        Op ix = *ao, of = *ao;
        of.swz[0] = ao->swz[1];
        Op* tmp[2] = { &ix, &of };
        int stride = r->type == T_UAV ? tr->uav_stride[r->idx[0].imm & 7] : tr->g_stride[r->idx[0].imm & 31];
        sp(&a, "((u32("); src(tr, &a, tmp[0], -1, 1, K_I); sp(&a, ") * %du + u32(", stride); src(tr, &a, tmp[1], -1, 1, K_I); sp(&a, ")) >> 2u)");
    } else if (r->type == T_UAV && !tr->uav_raw[r->idx[0].imm & 7]) {
        if (tr->uav_dim[r->idx[0].imm & 7] != 1) {
            snprintf(tr->sh->error, sizeof tr->sh->error, "atomics on UAV textures are not supported by WebGPU");
        }
        sp(&a, "u32("); src(tr, &a, ao, -1, 1, K_I); sp(&a, ")");
    } else { sp(&a, "(u32("); src(tr, &a, ao, -1, 1, K_I); sp(&a, ") >> 2u)"); }
    buf_name(&nm, r);
    int signed_op = opc == 174 || opc == 175 || opc == 186 || opc == 187;
    Op* val = o[imm ? 3 : 2];
    Str v = { 0 };
    if (opc == 172 || opc == 185) {
        Str cmpv = { 0 };
        src(tr, &cmpv, val, -1, 1, K_U);
        src(tr, &v, o[imm ? 4 : 3], -1, 1, K_U);
        sp(&call, "atomicCompareExchangeWeak(&%s[%s], %s, %s).old_value", nm.p, a.p, cmpv.p, v.p);
        free(cmpv.p);
    } else if (opc == 178 || opc == 179) {
        sp(&call, "atomicAdd(&%s[0], %s)", nm.p, opc == 178 ? "1u" : "0xFFFFFFFFu");
    } else {
        src(tr, &v, val, -1, 1, signed_op ? K_I : K_U);
        if (signed_op) sp(&call, "u32(%s(&%s[%s], bitcast<u32>(%s)))", atomic_fn(opc), nm.p, a.p, v.p);
        else sp(&call, "%s(&%s[%s], %s)", atomic_fn(opc), nm.p, a.p, v.p);
    }
    if (imm) assign(tr, o[0], K_U, 0, call.p);
    else { ind(tr); sp(&tr->body, "_ = %s;\n", call.p); }
    free(a.p); free(nm.p); free(call.p); free(v.p);
}

static void emit_uav_typed(Tw* tr, int opc, Op** o)
{
    if (opc == 164) {
        Op* u = o[0];
        int k = (int)u->idx[0].imm & 7;
        tr->sh->uav[k].used = 1;
        tr->sh->uav[k].written = 1;
        int dim = tr->uav_dim[k], n = base_coords(dim);
        Str c = { 0 }, a = { 0 }, v = { 0 };
        src(tr, &c, o[1], -1, n, K_U);
        if (is_array(dim)) { sp(&a, ", "); Op t = *o[1]; t.swz[0] = o[1]->swz[n]; src(tr, &a, &t, -1, 1, K_U); }
        int ret = tr->uav_ret[k];
        src(tr, &v, o[2], -1, 4, ret == 1 ? K_I : ret == 2 ? K_U : K_F);
        ind(tr);
        if (dim == 1 && tr->uav_atomic[k]) sp(&tr->body, "atomicStore(&u%d[%s], bitcast<vec4<u32>>(%s).x);\n", k, c.p, v.p);
        else if (dim == 1) sp(&tr->body, "u%d[%s] = bitcast<vec4<u32>>(%s).x;\n", k, c.p, v.p);
        else sp(&tr->body, "textureStore(u%d, %s%s, %s);\n", k, c.p, a.p ? a.p : "", v.p);
        free(c.p); free(a.p); free(v.p);
        return;
    }
    Op *d = o[0], *u = o[2];
    int k = (int)u->idx[0].imm & 7;
    tr->sh->uav[k].used = 1;
    tr->uav_read[k] = 1;
    int dim = tr->uav_dim[k], n = base_coords(dim);
    Str c = { 0 }, a = { 0 }, call = { 0 };
    src(tr, &c, o[1], -1, n, K_U);
    if (is_array(dim)) { sp(&a, ", "); Op t = *o[1]; t.swz[0] = o[1]->swz[n]; src(tr, &a, &t, -1, 1, K_U); }
    if (dim == 1) sp(&call, tr->uav_atomic[k] ? "vec4<u32>(atomicLoad(&u%d[%s]), 0u, 0u, 0u)" : "vec4<u32>(u%d[%s], 0u, 0u, 0u)", k, c.p);
    else sp(&call, "textureLoad(u%d, %s%s)", k, c.p, a.p ? a.p : "");
    int m = dm(d);
    Str e = { 0 };
    sp(&e, "bitcast<vec4<f32>>(%s).", call.p);
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&e, "%c", C4[u->swz[i]]);
    assign(tr, d, K_F, 0, e.p);
    free(c.p); free(a.p); free(call.p); free(e.p);
}

static const char* tex_type(int dim, int ret, int depth, int ms)
{
    static char b[64];
    const char* t = ret == 1 ? "i32" : ret == 2 ? "u32" : "f32";
    if (depth) {
        switch (dim) {
        case 6: return "texture_depth_cube";
        case 8: return "texture_depth_2d_array";
        case 10: return "texture_depth_cube_array";
        default: return "texture_depth_2d";
        }
    }
    (void)ms;
    switch (dim) {
    case 2: snprintf(b, sizeof b, "texture_1d<%s>", t); break;
    case 5: snprintf(b, sizeof b, "texture_3d<%s>", t); break;
    case 6: snprintf(b, sizeof b, "texture_cube<%s>", t); break;
    case 7: case 8: case 9: snprintf(b, sizeof b, "texture_2d_array<%s>", t); break;
    case 10: snprintf(b, sizeof b, "texture_cube_array<%s>", t); break;
    default: snprintf(b, sizeof b, "texture_2d<%s>", t); break;
    }
    return b;
}

static const char* uav_fmt(int ret) { return ret == 1 ? "r32sint" : ret == 2 ? "r32uint" : "rgba16float"; }

static void prescan(Tw* tr, const uint32_t* t, size_t n)
{
    Rd* rd = calloc(1, sizeof *rd);
    rd->t = t;
    rd->n = n;
    rd->pos = 2;
    while (rd->pos < rd->n) {
        rd->npool = 0;
        uint32_t tok = t[rd->pos];
        int opc = (int)(tok & 0x7FF);
        size_t len = opc == 53 ? t[rd->pos + 1] : (tok >> 24) & 0x7F;
        if (!len) break;
        size_t start = rd->pos++;
        uint32_t ext = tok;
        while (ext >> 31 && rd->pos < start + len) ext = t[rd->pos++];
        if (opc >= 169 && opc <= 189 && opc != 178 && opc != 179) {
            Op* r = read_op(rd);
            if (opc >= 178) r = read_op(rd);
            if (r->type == T_UAV) tr->uav_atomic[r->idx[0].imm & 7] = 1;
            if (r->type == T_TGSM) tr->g_atomic[r->idx[0].imm & 31] = 1;
        } else if (opc == 178 || opc == 179) {
            read_op(rd);
            Op* r = read_op(rd);
            if (r->type == T_UAV) tr->uav_atomic[r->idx[0].imm & 7] = 1;
        }
        rd->pos = start + len;
    }
    free(rd);
}

static int sem_eq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? *a - 32 : *a, y = *b >= 'a' && *b <= 'z' ? *b - 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

static int reg_is_int(const DxSig* s, uint32_t reg)
{
    for (int i = 0; i < s->n; i++)
        if (s->e[i].reg == reg && !s->e[i].sysval && s->e[i].comp_type != 3) return 1;
    return 0;
}

/* stop WGSL from constant-folding bitcasts of NaN/Inf bit patterns, which is a compile error */
static char* no_fold(char* s)
{
    static const struct { const char *from, *to; } rep[] = {
        { "bitcast<f32>(", "uew_bits(" }, { "bitcast<vec2<f32>>(vec2<u32>(", "uew_bits2(vec2<u32>(" },
        { "bitcast<vec3<f32>>(vec3<u32>(", "uew_bits3(vec3<u32>(" }, { "bitcast<vec4<f32>>(vec4<u32>(", "uew_bits4(vec4<u32>(" },
    };
    Str o = { 0 };
    const char* p = s;
    while (*p) {
        int hit = -1;
        for (int k = 0; k < 4 && hit < 0; k++)
            if (!strncmp(p, rep[k].from, strlen(rep[k].from))) hit = k;
        if (hit == 0) {
            const char* q = p + 13;
            while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f') || (*q >= 'A' && *q <= 'F') || *q == 'x') q++;
            if (q == p + 13 || q[0] != 'u' || q[1] != ')') hit = -1;
        }
        if (hit >= 0) { sp(&o, "%s", rep[hit].to); p += strlen(rep[hit].from); continue; }
        size_t n = strcspn(p + 1, "b") + 1;
        sp(&o, "%.*s", (int)n, p);
        p += n;
    }
    free(s);
    return o.p;
}

/* D3D lets barriers sit behind branches that are uniform in practice; WGSL wants proof, so those
   branch conditions go through workgroupUniformLoad. */
typedef struct { int start, loop, barrier, exit, ret, is_if, is_loop; } Blk;

static char* uniform_barriers(char* body, int* changed)
{
    *changed = 0;
    if (!body || (!strstr(body, "Barrier()"))) return body;
    int nl = 0;
    for (char* p = body; *p; p++) if (*p == '\n') nl++;
    char** ln = calloc((size_t)nl + 1, sizeof *ln);
    int* lloop = calloc((size_t)nl + 1, sizeof *lloop);
    int* lblk = calloc((size_t)nl + 1, sizeof *lblk);
    Blk* bl = calloc((size_t)nl + 1, sizeof *bl);
    int n = 0, nb = 0, last_barrier = -1;
    for (char* p = body; *p;) {
        char* e = strchr(p, '\n');
        ln[n++] = p;
        if (!e) break;
        *e = 0;
        p = e + 1;
    }
    int stack[256], sp_ = 0;
    for (int i = 0; i < n; i++) {
        const char* t = ln[i];
        while (*t == ' ') t++;
        size_t L = strlen(t);
        int loop = -1;
        for (int k = sp_ - 1; k >= 0; k--) if (bl[stack[k]].is_loop) { loop = stack[k]; break; }
        lloop[i] = loop;
        lblk[i] = -1;
        if (strstr(t, "Barrier()")) { for (int k = 0; k < sp_; k++) bl[stack[k]].barrier = 1; last_barrier = i; }
        int single = !strncmp(t, "if (", 4) && L > 2 && t[L - 1] == '}';
        if (single || !strcmp(t, "break;") || !strcmp(t, "continue;")) {
            int ret = strstr(t, "return;") != NULL;
            for (int k = sp_ - 1; k >= 0; k--) {
                if (ret) bl[stack[k]].ret = 1;
                else { bl[stack[k]].exit = 1; if (bl[stack[k]].is_loop) break; }
            }
        } else if (!strcmp(t, "return;")) for (int k = 0; k < sp_; k++) bl[stack[k]].ret = 1;
        if (t[0] == '}' && strcmp(t, "} else {")) { if (sp_) sp_--; continue; }
        if (L && t[L - 1] == '{' && strcmp(t, "} else {") && sp_ < 256) {
            Blk* b = &bl[nb];
            b->start = i;
            b->loop = loop;
            b->is_if = !strncmp(t, "if (", 4);
            b->is_loop = !strcmp(t, "loop {");
            lblk[i] = nb;
            stack[sp_++] = nb++;
        }
    }
    Str o = { 0 };
    for (int i = 0; i < n; i++) {
        const char* t = ln[i];
        int ind = 0;
        while (t[ind] == ' ') ind++;
        int conv = 0;
        if (!strncmp(t + ind, "if (", 4)) {
            if (lblk[i] >= 0) {
                Blk* b = &bl[lblk[i]];
                conv = b->barrier || (b->exit && b->loop >= 0 && bl[b->loop].barrier) || (b->ret && last_barrier > i);
            } else {
                int ret = strstr(t, "return;") != NULL;
                conv = ret ? last_barrier > i : (lloop[i] >= 0 && bl[lloop[i]].barrier);
            }
        }
        if (conv) {
            const char* c = t + ind + 4;
            int depth = 1;
            const char* q = c;
            for (; *q && depth; q++) { if (*q == '(') depth++; else if (*q == ')') depth--; }
            if (!depth) {
                sp(&o, "%.*suew_c = u32(%.*s);\n", ind, t, (int)(q - 1 - c), c);
                sp(&o, "%.*sif (workgroupUniformLoad(&uew_c) != 0u)%s\n", ind, t, q);
                *changed = 1;
                continue;
            }
        }
        sp(&o, "%s\n", t);
    }
    free(ln); free(lloop); free(lblk); free(bl);
    free(body);
    return o.p;
}

static Str helpers_txt;

const char* dxbc_wgsl_helpers(void)
{
    if (helpers_txt.p) return helpers_txt.p;
    Str g = { 0 };
    sp(&g, "diagnostic(off, derivative_uniformity);\n");
    sp(&g, "fn uew_bits2(v: vec2<u32>) -> vec2<f32> { return bitcast<vec2<f32>>(v); }" NL "fn uew_bits3(v: vec3<u32>) -> vec3<f32> { return bitcast<vec3<f32>>(v); }" NL
           "fn uew_bits4(v: vec4<u32>) -> vec4<f32> { return bitcast<vec4<f32>>(v); }" NL);
    sp(&g, "fn uew_bits(v: u32) -> f32 { return bitcast<f32>(v); }\n");
    sp(&g, "fn uew_f32tof16(v: f32) -> u32 { return pack2x16float(vec2<f32>(v, 0.0)) & 0xFFFFu; }\n"
           "fn uew_firstbit_hi(v: u32) -> u32 { return select(31u - firstLeadingBit(v), 0xFFFFFFFFu, v == 0u); }\n"
           "fn uew_firstbit_shi(v: i32) -> u32 { let l = firstLeadingBit(v); return select(31u - bitcast<u32>(l), 0xFFFFFFFFu, l == -1i); }\n"
           "fn uew_ubfe(w: u32, o: u32, v: u32) -> u32 { return extractBits(v, o & 31u, w & 31u); }\n"
           "fn uew_ibfe(w: u32, o: u32, v: i32) -> i32 { return extractBits(v, o & 31u, w & 31u); }\n"
           "fn uew_bfi(w: u32, o: u32, i: u32, b: u32) -> u32 { return insertBits(b, i, o & 31u, w & 31u); }\n"
           "fn uew_umulhi(a: u32, b: u32) -> u32 { let al = a & 0xFFFFu; let ah = a >> 16u; let bl = b & 0xFFFFu; let bh = b >> 16u;"
           " let lo = al * bl; let m1 = ah * bl; let m2 = al * bh; let mid = (lo >> 16u) + (m1 & 0xFFFFu) + (m2 & 0xFFFFu);"
           " return ah * bh + (m1 >> 16u) + (m2 >> 16u) + (mid >> 16u); }\n"
           "fn uew_imulhi(a: i32, b: i32) -> i32 { var h = bitcast<i32>(uew_umulhi(bitcast<u32>(a), bitcast<u32>(b))); if (a < 0) { h -= b; } if (b < 0) { h -= a; } return h; }\n");
    helpers_txt = g;
    return g.p;
}

int dxbc_layer_loc(const DxSig* s)
{
    if (!s) return -1;
    int has = 0, top = -1;
    for (int i = 0; i < s->n; i++) {
        if (s->e[i].sysval == 4) has = 1;
        if ((int)s->e[i].reg > top) top = (int)s->e[i].reg;
    }
    return has ? top + 1 : -1;
}

/* the shader's own identifiers (registers, bindings, uew_body) get a prefix so two shaders can share a module */
static int own_name(const char* p, size_t n)
{
    static const char* fixed[] = { "v", "icb", "odepth", "uew_body", "vtid", "vgid", "vtidg", "vtidf", "uew_c", "vprim", "vgsi" };
    for (size_t k = 0; k < sizeof fixed / sizeof *fixed; k++) if (strlen(fixed[k]) == n && !strncmp(p, fixed[k], n)) return 1;
    if (n == 3 && (!strncmp(p, "u32", 3) || !strncmp(p, "f32", 3) || !strncmp(p, "i32", 3))) return 0;
    size_t a = !strncmp(p, "cb", 2) ? 2 : (strchr("rotsugx", p[0]) ? 1 : 0);
    if (!a || a >= n) return 0;
    for (size_t i = a; i < n; i++) if (p[i] < '0' || p[i] > '9') return 0;
    return 1;
}

static char* prefix_names(char* s, const char* pre)
{
    Str o = { 0 };
    const char* p = s;
    char prev = 0;
    while (*p) {
        int id0 = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_';
        if (!id0) {
            const char* q = p;
            while (*q && !((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '_')) {
                if (*q >= '0' && *q <= '9') { while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || *q == '.') q++; continue; }
                q++;
            }
            sp(&o, "%.*s", (int)(q - p), p);
            prev = q[-1];
            p = q;
            continue;
        }
        const char* q = p;
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '_') q++;
        size_t n = (size_t)(q - p);
        if (prev != '.' && prev != '@' && own_name(p, n)) sp(&o, "%s", pre);
        sp(&o, "%.*s", (int)n, p);
        prev = q[-1];
        p = q;
    }
    free(s);
    return o.p;
}

int dxbc_to_wgsl_ex(const uint8_t* code, size_t size, const DxWgslOpts* opt, DxWgsl* out)
{
    const DxSig* vs_out = opt ? opt->vs_out : NULL;
    memset(out, 0, sizeof *out);
    if (opt) for (int i = 0; i < 128; i++) if (opt->depth_srv[i >> 5] >> (i & 31) & 1) out->srv[i].compare = 1;
    size_t ntok = 0;
    const uint32_t* tokens = dxbc_chunks(code, size, &out->in, &out->out, &ntok);
    if (!tokens) { snprintf(out->error, sizeof out->error, "not a DXBC shader"); return 0; }
    int prog = (int)(tokens[0] >> 16);
    out->stage = prog;
    int lib = opt && opt->lib;
    if (prog != 0 && prog != 1 && prog != 5 && prog != 2) {
        snprintf(out->error, sizeof out->error, "%s shaders are not supported by WebGPU", prog == 3 ? "hull" : "domain");
        return 0;
    }
    Tw* tr = calloc(1, sizeof *tr);
    tr->sh = out;
    tr->stage = prog;
    size_t n = tokens[1] < ntok ? tokens[1] : ntok;
    prescan(tr, tokens, n);
    Rd* rd = calloc(1, sizeof *rd);
    rd->t = tokens;
    rd->n = n;
    rd->pos = 2;
    Str decl = { 0 }, icb = { 0 };
    int max_in = -1, max_out = -1, ok = 1;

    while (rd->pos < rd->n) {
        rd->npool = 0;
        uint32_t tok = tokens[rd->pos];
        int opc = (int)(tok & 0x7FF);
        size_t len = (tok >> 24) & 0x7F;
        size_t start = rd->pos;
        if (opc == 53) {
            len = tokens[rd->pos + 1];
            if (((tok >> 11) & 0x1FFFFF) == 3) {
                int cnt = (int)((len - 2) / 4);
                sp(&icb, "const icb = array<vec4<u32>, %d>(", cnt);
                for (int i = 0; i < cnt; i++) {
                    const uint32_t* q = &tokens[rd->pos + 2 + 4 * i];
                    sp(&icb, "%svec4<u32>(%uu, %uu, %uu, %uu)", i ? ", " : "", q[0], q[1], q[2], q[3]);
                }
                sp(&icb, ");\n");
            }
            rd->pos = start + len;
            continue;
        }
        if (!len) { snprintf(out->error, sizeof out->error, "bad token stream"); ok = 0; break; }
        rd->pos++;
        int sat = (tok >> 13) & 1, nz = (tok >> 18) & 1;
        tr->sat = sat;
        int off[3] = { 0, 0, 0 }, has_off = 0;
        uint32_t ext = tok;
        while (ext >> 31) {
            ext = tokens[rd->pos++];
            if ((ext & 0x3F) == 1) {
                has_off = 1;
                off[0] = ((int32_t)(ext << 19)) >> 28;
                off[1] = ((int32_t)(ext << 15)) >> 28;
                off[2] = ((int32_t)(ext << 11)) >> 28;
            }
        }
        Op* o[8] = { 0 };
        int nops = 0;
        switch (opc) {
        case 88: {
            Op* r = read_op(rd);
            int slot = (int)r->idx[0].imm & 127;
            int k = (int)(tokens[rd->pos++] & 0xF);
            tr->tex_dim[slot] = (int)((tok >> 11) & 0x1F);
            tr->tex_ret[slot] = k == 3 ? 1 : k == 4 ? 2 : 0;
            out->srv[slot].dim = tr->tex_dim[slot];
            out->srv[slot].ret = tr->tex_ret[slot];
            out->srv[slot].buffer = tr->tex_dim[slot] == 1;
            break;
        }
        case 161: case 162: {
            Op* r = read_op(rd);
            int slot = (int)r->idx[0].imm & 127;
            tr->tex_dim[slot] = 1;
            out->srv[slot].dim = 1;
            out->srv[slot].buffer = 1;
            out->srv[slot].raw = opc == 161;
            out->srv[slot].structured = opc == 162;
            if (opc == 162) out->srv[slot].stride = (int)tokens[rd->pos++];
            break;
        }
        case 156: case 157: case 158: {
            Op* r = read_op(rd);
            int k = (int)r->idx[0].imm & 7;
            if (opc == 156) { tr->uav_dim[k] = (int)((tok >> 11) & 0x1F); tr->uav_ret[k] = (int)(tokens[rd->pos] & 0xF) == 3 ? 1 : (int)(tokens[rd->pos] & 0xF) == 4 ? 2 : 0; }
            else { tr->uav_dim[k] = 1; tr->uav_raw[k] = 1; if (opc == 158) tr->uav_stride[k] = (int)tokens[rd->pos]; }
            out->uav[k].dim = tr->uav_dim[k];
            out->uav[k].ret = tr->uav_ret[k];
            out->uav[k].raw = tr->uav_raw[k];
            out->uav[k].structured = opc == 158;
            out->uav[k].stride = tr->uav_stride[k];
            out->uav[k].atomic = tr->uav_atomic[k];
            break;
        }
        case 159: case 160: {
            Op* r = read_op(rd);
            int k = (int)r->idx[0].imm & 31;
            if (opc == 159) tr->g_size[k] = (int)tokens[rd->pos] / 4;
            else { tr->g_stride[k] = (int)tokens[rd->pos]; tr->g_size[k] = (int)(tokens[rd->pos] * tokens[rd->pos + 1] / 4); }
            break;
        }
        case 89: {
            Op* r = read_op(rd);
            int slot = (int)r->idx[0].imm & 15, cnt = (int)r->idx[1].imm;
            if (cnt > out->cb_size[slot]) out->cb_size[slot] = cnt;
            break;
        }
        case 90: {
            Op* r = read_op(rd);
            int slot = (int)r->idx[0].imm & 15;
            if (((tok >> 11) & 0xF) == 1) out->samp[slot].compare = 1;
            break;
        }
        case 104: tr->temps = (int)tokens[rd->pos++]; break;
        case 105: {
            uint32_t reg = tokens[rd->pos], cnt = tokens[rd->pos + 1];
            sp(&decl, "var<private> x%u: array<vec4<f32>, %u>;\n", reg, cnt);
            break;
        }
        case 155: out->cs[0] = (int)tokens[rd->pos]; out->cs[1] = (int)tokens[rd->pos + 1]; out->cs[2] = (int)tokens[rd->pos + 2]; break;
        case 95: case 96: case 97: case 98: case 99: case 100: {
            Op* r = read_op(rd);
            int reg = r->dims == 2 ? (int)r->idx[1].imm : (int)r->idx[0].imm;
            if (r->type == T_INPUT && reg > max_in) max_in = reg;
            if (opc == 98 && r->type == T_INPUT && reg >= 0 && reg < 32) tr->in_interp[reg] = (char)((tok >> 11) & 15);
            break;
        }
        case 101: case 102: case 103: {
            Op* r = read_op(rd);
            if (r->type == T_OUTPUT && (int)r->idx[0].imm > max_out) max_out = (int)r->idx[0].imm;
            if (r->type == T_ODEPTH || r->type == T_ODEPTH_GE || r->type == T_ODEPTH_LE) tr->ps_depth = 1;
            break;
        }
        case 92: out->gs_topo = (int)((tok >> 11) & 0x3F); break;
        case 93: { int pr = (int)((tok >> 11) & 0x3F); out->gs_in = pr == 1 ? 1 : pr == 2 ? 2 : pr == 3 ? 3 : pr == 6 ? 4 : pr == 7 ? 6 : 3; break; }
        case 94: out->gs_maxout = (int)tokens[rd->pos]; break;
        case 206: out->gs_inst = (int)tokens[rd->pos]; break;
        case 91: case 106: case 143: case 144: case 145: case 146: case 147: case 148: case 149: case 150:
        case 151: case 152: case 153: case 154:
            break;
        default: {
            while (rd->pos < start + len && nops < 8) o[nops++] = read_op(rd);
            int m = nops ? dm(o[0]) : 1, nn = popc4(m);
            Str e = { 0 };
            static const int KU1[] = { K_U }, KUUU[] = { K_U, K_U, K_U }, KUUI[] = { K_U, K_U, K_I }, KUUUU[] = { K_U, K_U, K_U, K_U }, KF1[] = { K_F }, KUU[] = { K_U, K_U };
            switch (opc) {
            case 0: bin(tr, o, sat, K_F, "+", 0); break;
            case 56: bin(tr, o, sat, K_F, "*", 0); break;
            case 14: bin(tr, o, sat, K_F, "/", 0); break;
            case 51: bin(tr, o, sat, K_F, "min", 1); break;
            case 52: bin(tr, o, sat, K_F, "max", 1); break;
            case 50:
                sp(&e, "fma("); src(tr, &e, o[1], m, 0, K_F); sp(&e, ", "); src(tr, &e, o[2], m, 0, K_F); sp(&e, ", "); src(tr, &e, o[3], m, 0, K_F); sp(&e, ")");
                assign(tr, o[0], K_F, sat, e.p);
                break;
            case 15: case 16: case 17: {
                int k = opc - 13;
                sp(&e, nn > 1 ? "%s(dot(" : "%s(dot(", vt(K_F, nn));
                src(tr, &e, o[1], -1, k, K_F); sp(&e, ", "); src(tr, &e, o[2], -1, k, K_F); sp(&e, "))");
                assign(tr, o[0], K_F, sat, e.p);
                break;
            }
            case 54: {
                int k = o[1]->type == T_IMM32 && !sat ? K_U : K_F;
                src(tr, &e, o[1], m, 0, k);
                assign(tr, o[0], k, sat, e.p);
                break;
            }
            case 55:
                sp(&e, "select("); src(tr, &e, o[3], m, 0, K_F); sp(&e, ", "); src(tr, &e, o[2], m, 0, K_F); sp(&e, ", ");
                src(tr, &e, o[1], m, 0, K_U); sp(&e, " != %s(0u))", nn == 1 ? "u32" : vt(K_U, nn));
                assign(tr, o[0], K_F, 0, e.p);
                break;
            case 26: un(tr, o, sat, K_F, K_F, "fract"); break;
            case 64: un(tr, o, sat, K_F, K_F, "round"); break;
            case 65: un(tr, o, sat, K_F, K_F, "floor"); break;
            case 66: un(tr, o, sat, K_F, K_F, "ceil"); break;
            case 67: un(tr, o, sat, K_F, K_F, "trunc"); break;
            case 68: un(tr, o, sat, K_F, K_F, "inverseSqrt"); break;
            case 75: un(tr, o, sat, K_F, K_F, "sqrt"); break;
            case 25: un(tr, o, sat, K_F, K_F, "exp2"); break;
            case 47: un(tr, o, sat, K_F, K_F, "log2"); break;
            case 129: sp(&e, "(1.0 / "); src(tr, &e, o[1], m, 0, K_F); sp(&e, ")"); assign(tr, o[0], K_F, sat, e.p); break;
            case 11: case 122: case 123: un(tr, o, sat, K_F, K_F, tr->stage == 0 ? (opc == 123 ? "dpdxFine" : opc == 122 ? "dpdxCoarse" : "dpdx") : "fract(0.0)*"); break;
            case 12: case 124: case 125: un(tr, o, sat, K_F, K_F, tr->stage == 0 ? (opc == 125 ? "dpdyFine" : opc == 124 ? "dpdyCoarse" : "dpdy") : "fract(0.0)*"); break;
            case 77:
                defer_begin(tr);
                if (o[0]->type != T_NULL) { Str a = { 0 }; sp(&a, "sin("); src(tr, &a, o[2], dm(o[0]), 0, K_F); sp(&a, ")"); assign(tr, o[0], K_F, sat, a.p); free(a.p); }
                if (o[1]->type != T_NULL) { Str a = { 0 }; sp(&a, "cos("); src(tr, &a, o[2], dm(o[1]), 0, K_F); sp(&a, ")"); assign(tr, o[1], K_F, sat, a.p); free(a.p); }
                defer_end(tr);
                break;
            case 24: cmpop(tr, o, K_F, "=="); break;
            case 57: cmpop(tr, o, K_F, "!="); break;
            case 49: cmpop(tr, o, K_F, "<"); break;
            case 29: cmpop(tr, o, K_F, ">="); break;
            case 32: cmpop(tr, o, K_I, "=="); break;
            case 39: cmpop(tr, o, K_I, "!="); break;
            case 34: cmpop(tr, o, K_I, "<"); break;
            case 33: cmpop(tr, o, K_I, ">="); break;
            case 79: cmpop(tr, o, K_U, "<"); break;
            case 80: cmpop(tr, o, K_U, ">="); break;
            case 30: bin(tr, o, 0, K_I, "+", 0); break;
            case 38:
                defer_begin(tr);
                if (o[0]->type != T_NULL) { Op* t[3] = { o[0], o[2], o[3] }; per_comp(tr, t, "uew_imulhi", (const int[]){ K_I, K_I }, 2, K_I, NULL); }
                if (o[1]->type != T_NULL) { Op* t[3] = { o[1], o[2], o[3] }; bin(tr, t, 0, K_I, "*", 0); }
                defer_end(tr);
                break;
            case 81:
                defer_begin(tr);
                if (o[0]->type != T_NULL) { Op* t[3] = { o[0], o[2], o[3] }; per_comp(tr, t, "uew_umulhi", KUU, 2, K_U, NULL); }
                if (o[1]->type != T_NULL) { Op* t[3] = { o[1], o[2], o[3] }; bin(tr, t, 0, K_U, "*", 0); }
                defer_end(tr);
                break;
            case 78:
                defer_begin(tr);
                if (o[0]->type != T_NULL) { Op* t[3] = { o[0], o[2], o[3] }; bin(tr, t, 0, K_U, "/", 0); }
                if (o[1]->type != T_NULL) { Op* t[3] = { o[1], o[2], o[3] }; bin(tr, t, 0, K_U, "%", 0); }
                defer_end(tr);
                break;
            case 35: case 82: {
                int k = opc == 35 ? K_I : K_U;
                sp(&e, "("); src(tr, &e, o[1], m, 0, k); sp(&e, " * "); src(tr, &e, o[2], m, 0, k); sp(&e, " + "); src(tr, &e, o[3], m, 0, k); sp(&e, ")");
                assign(tr, o[0], k, 0, e.p);
                break;
            }
            case 36: bin(tr, o, 0, K_I, "max", 1); break;
            case 37: bin(tr, o, 0, K_I, "min", 1); break;
            case 83: bin(tr, o, 0, K_U, "max", 1); break;
            case 84: bin(tr, o, 0, K_U, "min", 1); break;
            case 40: sp(&e, "(-"); src(tr, &e, o[1], m, 0, K_I); sp(&e, ")"); assign(tr, o[0], K_I, 0, e.p); break;
            case 1: bin(tr, o, 0, K_U, "&", 0); break;
            case 60: bin(tr, o, 0, K_U, "|", 0); break;
            case 87: bin(tr, o, 0, K_U, "^", 0); break;
            case 59: sp(&e, "(~"); src(tr, &e, o[1], m, 0, K_U); sp(&e, ")"); assign(tr, o[0], K_U, 0, e.p); break;
            case 41: shift(tr, o, K_I, "<<"); break;
            case 42: shift(tr, o, K_I, ">>"); break;
            case 85: shift(tr, o, K_U, ">>"); break;
            case 27: un(tr, o, 0, K_F, K_I, vt(K_I, nn)); break;
            case 28: un(tr, o, 0, K_F, K_U, vt(K_U, nn)); break;
            case 43: un(tr, o, sat, K_I, K_F, vt(K_F, nn)); break;
            case 86: un(tr, o, sat, K_U, K_F, vt(K_F, nn)); break;
            case 131: per_comp(tr, o, "unpack2x16float", KU1, 1, K_F, ".x"); break;
            case 130: per_comp(tr, o, "uew_f32tof16", KF1, 1, K_U, NULL); break;
            case 134: un(tr, o, 0, K_U, K_U, "countOneBits"); break;
            case 135: per_comp(tr, o, "uew_firstbit_hi", KU1, 1, K_U, NULL); break;
            case 136: un(tr, o, 0, K_U, K_U, "firstTrailingBit"); break;
            case 137: per_comp(tr, o, "uew_firstbit_shi", (const int[]){ K_I }, 1, K_U, NULL); break;
            case 138: per_comp(tr, o, "uew_ubfe", KUUU, 3, K_U, NULL); break;
            case 139: per_comp(tr, o, "uew_ibfe", KUUI, 3, K_I, NULL); break;
            case 140: per_comp(tr, o, "uew_bfi", KUUUU, 4, K_U, NULL); break;
            case 141: un(tr, o, 0, K_U, K_U, "reverseBits"); break;
            case 142:
                defer_begin(tr);
                for (int dd = 0; dd < 2; dd++) {
                    if (o[dd]->type == T_NULL) continue;
                    Str x = { 0 };
                    int mm = dm(o[dd]), k2 = popc4(mm);
                    Op* hi = dd == 0 ? o[4] : o[3];
                    Op* lo = dd == 0 ? o[3] : o[4];
                    sp(&x, "select("); src(tr, &x, lo, mm, 0, K_F); sp(&x, ", "); src(tr, &x, hi, mm, 0, K_F); sp(&x, ", ");
                    src(tr, &x, o[2], mm, 0, K_U); sp(&x, " != %s(0u))", k2 == 1 ? "u32" : vt(K_U, k2));
                    assign(tr, o[dd], K_F, 0, x.p);
                    free(x.p);
                }
                defer_end(tr);
                break;
            case 31: { Str b = { 0 }; src(tr, &b, o[0], -1, 1, K_U); ind(tr); sp(&tr->body, "if (%s %s 0u) {\n", b.p, nz ? "!=" : "=="); free(b.p); tr->indent++; break; }
            case 18: tr->indent--; ind(tr); sp(&tr->body, "} else {\n"); tr->indent++; break;
            case 21: case 22: tr->indent--; ind(tr); sp(&tr->body, "}\n"); break;
            case 23:
                if (tr->switch_depth > 0 && tr->case_open[tr->switch_depth - 1]) { tr->indent--; ind(tr); sp(&tr->body, "}\n"); }
                if (tr->switch_depth > 0 && !tr->switch_open[tr->switch_depth - 1]) { ind(tr); sp(&tr->body, "default: {}\n"); }
                if (tr->switch_depth > 0) tr->switch_depth--;
                tr->indent--; ind(tr); sp(&tr->body, "}\n");
                break;
            case 48: {
                /* every loop gets an iteration cap: a loop running away on bad data would hang the GPU, and the browser then
                   loses the device for good (black screen); real shader loops stay far below it */
                int n = tr->nloops++;
                ind(tr); sp(&tr->body, "var uew_lc%d: u32 = 0u;\n", n);
                ind(tr); sp(&tr->body, "loop {\n"); tr->indent++;
                ind(tr); sp(&tr->body, "uew_lc%d += 1u; if (uew_lc%d > 65536u) { break; }\n", n, n);
                break;
            }
            case 2: ind(tr); sp(&tr->body, "break;\n"); break;
            case 7: ind(tr); sp(&tr->body, "continue;\n"); break;
            case 3: case 8: case 63: case 13: {
                Str b = { 0 };
                src(tr, &b, o[0], -1, 1, K_U);
                ind(tr);
                sp(&tr->body, "if (%s %s 0u) { %s; }\n", b.p, nz ? "!=" : "==", opc == 3 ? "break" : opc == 8 ? "continue" : opc == 63 ? "return" : "discard");
                free(b.p);
                break;
            }
            case 62: ind(tr); sp(&tr->body, "return;\n"); break;
            case 76: {
                Str b = { 0 };
                src(tr, &b, o[0], -1, 1, K_I);
                ind(tr); sp(&tr->body, "switch (%s) {\n", b.p); free(b.p);
                tr->indent++;
                if (tr->switch_depth < 32) { tr->switch_open[tr->switch_depth] = 0; tr->case_open[tr->switch_depth] = 0; tr->switch_depth++; }
                break;
            }
            case 6: case 10: {
                int sd = tr->switch_depth - 1;
                int prev_label_only = sd >= 0 && tr->case_open[sd] == 2;
                if (sd >= 0 && tr->case_open[sd] == 1) { tr->indent--; ind(tr); sp(&tr->body, "}\n"); }
                if (prev_label_only) {
                    tr->body.n -= 4;
                    sp(&tr->body, opc == 6 ? ", %di: {\n" : ", default: {\n", opc == 6 ? (int)o[0]->imm[0] : 0);
                } else {
                    ind(tr);
                    if (opc == 6) sp(&tr->body, "case %di: {\n", (int)o[0]->imm[0]);
                    else sp(&tr->body, "default: {\n");
                    tr->indent++;
                }
                if (opc == 10 && sd >= 0) tr->switch_open[sd] = 1;
                if (sd >= 0) tr->case_open[sd] = 2;
                break;
            }
            case 69: case 70: case 71: case 72: case 73: case 74: emit_sample(tr, opc, o, off, has_off); break;
            case 45: emit_ld(tr, o, off, has_off, 0); break;
            case 46: emit_ld(tr, o, off, has_off, 1); break;
            case 61: emit_resinfo(tr, o, (int)((tok >> 11) & 3)); break;
            case 109: case 126: case 127: case 128: emit_gather(tr, opc, o, off, has_off); break;
            case 108: sp(&e, "%s(0.0)", vt(K_F, nn)); assign(tr, o[0], K_F, 0, e.p); break;
            case 121: {
                Op* r = o[1];
                mark_buf(tr, r, 0);
                Str nm = { 0 };
                buf_name(&nm, r);
                int stride = r->type == T_UAV ? tr->uav_stride[r->idx[0].imm & 7] : out->srv[r->idx[0].imm & 127].stride;
                if (stride) sp(&e, "%s(arrayLength(&%s) * 4u / %du)", nn == 1 ? "u32" : vt(K_U, nn), nm.p, stride);
                else if (r->type == T_UAV ? tr->uav_raw[r->idx[0].imm & 7] : out->srv[r->idx[0].imm & 127].raw) sp(&e, "%s(arrayLength(&%s) * 4u)", nn == 1 ? "u32" : vt(K_U, nn), nm.p);
                else sp(&e, "%s(arrayLength(&%s))", nn == 1 ? "u32" : vt(K_U, nn), nm.p);
                assign(tr, o[0], K_U, 0, e.p);
                free(nm.p);
                break;
            }
            case 165: emit_ld_buf(tr, o, 0); break;
            case 167: emit_ld_buf(tr, o, 1); break;
            case 166: emit_store_buf(tr, o, 0); break;
            case 168: emit_store_buf(tr, o, 1); break;
            case 163: case 164: emit_uav_typed(tr, opc, o); break;
            case 190: {
                uint32_t f = (tok >> 11) & 0xF;
                ind(tr);
                if (f & 2) sp(&tr->body, "workgroupBarrier();\n");
                else if (f & 4) sp(&tr->body, "storageBarrier();\n");
                else sp(&tr->body, "workgroupBarrier();\n");
                break;
            }
            case 19: case 117: ind(tr); sp(&tr->body, "uew_emit();\n"); break;
            case 9: case 118: ind(tr); sp(&tr->body, "uew_cut();\n"); break;
            case 20: case 119: ind(tr); sp(&tr->body, "uew_emit();\n"); ind(tr); sp(&tr->body, "uew_cut();\n"); break;
            case 58: case 110: case 111: break;
            default:
                if (opc >= 169 && opc <= 189) { emit_atomic(tr, opc, o); break; }
                ind(tr);
                sp(&tr->body, "// unsupported opcode %d\n", opc);
                if (!out->error[0]) snprintf(out->error, sizeof out->error, "unsupported opcode %d", opc);
                break;
            }
            if (tr->switch_depth > 0 && tr->case_open[tr->switch_depth - 1] == 2 && opc != 6 && opc != 10) tr->case_open[tr->switch_depth - 1] = 1;
            free(e.p);
            break;
        }
        }
        rd->pos = start + len;
    }

    Str g = { 0 };
    if (!lib && prog != 2) sp(&g, "%s", dxbc_wgsl_helpers());
    int grp = lib ? opt->group : prog == 0 ? 2 : 0, rgrp = grp + 1;
    for (int i = 0; i < 16; i++)
        if (out->cb_size[i]) sp(&g, "@group(%d) @binding(%d) var<uniform> cb%d: array<vec4<f32>, %d>;\n", grp, i, i, out->cb_size[i]);
    for (int i = 0; i < 128; i++) {
        DxSlot* s = &out->srv[i];
        if (!s->used) continue;
        if (s->buffer) {
            if (s->raw || s->structured) sp(&g, "@group(%d) @binding(%d) var<storage, read> t%d: array<u32>;\n", rgrp, 16 + i, i);
            else sp(&g, "@group(%d) @binding(%d) var<storage, read> t%d: array<vec4<u32>>;\n", rgrp, 16 + i, i);
        } else sp(&g, "@group(%d) @binding(%d) var t%d: %s;\n", rgrp, 16 + i, i, tex_type(s->dim, s->ret, s->compare, 0));
    }
    for (int i = 0; i < 16; i++)
        if (out->samp[i].used) sp(&g, "@group(%d) @binding(%d) var s%d: %s;\n", rgrp, 160 + i, i, out->samp[i].compare ? "sampler_comparison" : "sampler");
    for (int i = 0; i < 8; i++) {
        DxSlot* u = &out->uav[i];
        if (!u->used) continue;
        if (u->raw || u->structured) sp(&g, "@group(%d) @binding(%d) var<storage, read_write> u%d: array<%s>;\n", rgrp, 180 + i, i, tr->uav_atomic[i] ? "atomic<u32>" : "u32");
        else if (u->dim == 1) sp(&g, "@group(%d) @binding(%d) var<storage, read_write> u%d: array<%s>;\n", rgrp, 180 + i, i, tr->uav_atomic[i] ? "atomic<u32>" : "u32");
        else {
            const char* tt = u->dim == 8 ? "texture_storage_2d_array" : u->dim == 5 ? "texture_storage_3d" : u->dim == 2 ? "texture_storage_1d" : "texture_storage_2d";
            const char* fmt = opt && opt->uav_format[i] ? opt->uav_format[i] : uav_fmt(u->ret);
            out->uav[i].written = !tr->uav_read[i];
            sp(&g, "@group(%d) @binding(%d) var u%d: %s<%s, %s>;\n", rgrp, 180 + i, i, tt, fmt, tr->uav_read[i] ? "read_write" : "write");
        }
    }
    for (int i = 0; i < 32; i++)
        if (tr->g_size[i]) sp(&g, "var<workgroup> g%d: array<%s, %d>;\n", i, tr->g_atomic[i] ? "atomic<u32>" : "u32", tr->g_size[i]);
    int uni = 0;
    if (prog == 5) tr->body.p = uniform_barriers(tr->body.p, &uni);
    if (uni) sp(&g, "var<workgroup> uew_c: u32;\n");
    if (icb.p) sp(&g, "%s", icb.p);
    for (int i = 0; i < tr->temps; i++) sp(&g, "var<private> r%d: vec4<f32>;\n", i);
    int nin = max_in + 1;
    for (int i = 0; i < out->in.n; i++) if ((int)out->in.e[i].reg + 1 > nin && out->in.e[i].reg < 32) nin = (int)out->in.e[i].reg + 1;
    if (prog == 2) {
        if (!out->gs_in) out->gs_in = 3;
        sp(&g, "var<private> v: array<array<vec4<f32>, %d>, %d>;\nvar<private> vprim: vec4<f32>;\nvar<private> vgsi: vec4<f32>;\n", nin > 0 ? nin : 1, out->gs_in);
    } else sp(&g, "var<private> v: array<vec4<f32>, %d>;\n", nin > 0 ? nin : 1);
    int nout = max_out + 1;
    for (int i = 0; i < out->out.n; i++) if ((int)out->out.e[i].reg + 1 > nout && out->out.e[i].reg < 32) nout = (int)out->out.e[i].reg + 1;
    out->nout = nout;
    for (int i = 0; i < nout; i++) sp(&g, "var<private> o%d: vec4<f32>;\n", i);
    if (prog == 0) sp(&g, "var<private> odepth: f32;\n");
    if (prog == 5) sp(&g, "var<private> vtid: vec4<f32>;\nvar<private> vgid: vec4<f32>;\nvar<private> vtidg: vec4<f32>;\nvar<private> vtidf: vec4<f32>;\n");
    if (decl.p) sp(&g, "%s", decl.p);
    sp(&g, "fn uew_body() {\n%s}\n", tr->body.p ? tr->body.p : "");

    if (lib || prog == 2) {
    } else if (prog == 1) {
        sp(&g, "struct VIn {\n    @builtin(vertex_index) vid: u32,\n    @builtin(instance_index) iid: u32,\n");
        Str pro = { 0 };
        for (int i = 0; i < out->in.n; i++) {
            DxSigElem* e = &out->in.e[i];
            if (e->sysval == 6) { sp(&pro, "    v[%u].x = bitcast<f32>(i.vid);\n", e->reg); out->uses_vertex_id = 1; continue; }
            if (e->sysval == 8) { sp(&pro, "    v[%u].x = bitcast<f32>(i.iid);\n", e->reg); out->uses_instance_id = 1; continue; }
            if (e->sysval) continue;
            int k = e->comp_type == 1 ? K_U : e->comp_type == 2 ? K_I : K_F;
            sp(&g, "    @location(%u) a%u: %s,\n", e->reg, e->reg, vt(k, 4));
            if (k == K_F) sp(&pro, "    v[%u] = i.a%u;\n", e->reg, e->reg);
            else sp(&pro, "    v[%u] = bitcast<vec4<f32>>(i.a%u);\n", e->reg, e->reg);
        }
        sp(&g, "}\nstruct VOut {\n    @builtin(position) @invariant pos: vec4<f32>,\n");
        Str epi = { 0 };
        int done[32] = { 0 };
        for (int i = 0; i < out->out.n; i++) {
            DxSigElem* e = &out->out.e[i];
            if (e->sysval == 1) { sp(&epi, "    r_.pos = o%u;\n", e->reg); continue; }
            if (e->sysval || e->reg >= 32 || done[e->reg]) continue;
            done[e->reg] = 1;
            sp(&g, "    @location(%u)%s l%u: vec4<f32>,\n", e->reg, reg_is_int(&out->out, e->reg) ? " @interpolate(flat)" :
               opt && opt->out_interp ? interp_attr(opt->out_interp[e->reg]) : "", e->reg);
            sp(&epi, "    r_.l%u = o%u;\n", e->reg, e->reg);
        }
        sp(&g, "}\n@vertex fn main(i: VIn) -> VOut {\n%s    uew_body();\n    var r_: VOut;\n%s    return r_;\n}\n", pro.p ? pro.p : "", epi.p ? epi.p : "");
        free(pro.p); free(epi.p);
    } else if (prog == 0) {
        sp(&g, "struct FIn {\n    @builtin(position) pos: vec4<f32>,\n    @builtin(front_facing) ff: bool,\n");
        Str pro = { 0 };
        int declared[32] = { 0 };
        for (int i = 0; i < out->in.n; i++) {
            DxSigElem* e = &out->in.e[i];
            if (e->sysval == 1) { sp(&pro, "    v[%u] = i.pos;\n", e->reg); continue; }
            if (e->sysval == 9) { sp(&pro, "    v[%u].x = bitcast<f32>(select(0u, 0xFFFFFFFFu, i.ff));\n", e->reg); continue; }
            if (e->sysval == 4) {
                int ll = dxbc_layer_loc(vs_out);
                if (ll >= 0) {
                    sp(&g, "    @location(%d) @interpolate(flat) layer: u32,\n", ll);
                    for (int c = 0; c < 4; c++) if (e->mask & (1 << c)) sp(&pro, "    v[%u].%c = bitcast<f32>(i.layer);\n", e->reg, C4[c]);
                }
                continue;
            }
            if (e->sysval) continue;
            int loc = (int)e->reg, src_mask = (int)e->mask, flat = reg_is_int(&out->in, e->reg);
            if (vs_out) {
                loc = -1;
                for (int k = 0; k < vs_out->n; k++)
                    if (!vs_out->e[k].sysval && sem_eq(vs_out->e[k].name, e->name) && vs_out->e[k].index == e->index) {
                        loc = (int)vs_out->e[k].reg;
                        src_mask = (int)vs_out->e[k].mask;
                        flat = reg_is_int(vs_out, vs_out->e[k].reg);
                    }
            }
            if (loc < 0 || loc >= 32) continue;
            if (!declared[loc]) {
                declared[loc] = 1;
                int mode = opt && opt->use_interp && !flat ? tr->in_interp[e->reg] : 0;
                if (mode == 2) mode = 0;
                out->interp[loc] = (char)mode;
                sp(&g, "    @location(%d)%s l%d: vec4<f32>,\n", loc, flat ? " @interpolate(flat)" : interp_attr(mode), loc);
            }
            char dst[5] = { 0 }, srcm[5] = { 0 };
            for (int c = 0, j = 0; c < 4; c++) if (e->mask & (1 << c)) dst[j++] = C4[c];
            for (int c = 0, j = 0; c < 4 && j < (int)strlen(dst); c++) if (src_mask & (1 << c)) srcm[j++] = C4[c];
            if (strlen(srcm) < strlen(dst)) snprintf(srcm, sizeof srcm, "%s", dst);
            for (size_t c = 0; c < strlen(dst); c++) sp(&pro, "    v[%u].%c = i.l%d.%c;\n", e->reg, dst[c], loc, srcm[c]);
        }
        sp(&g, "}\nstruct FOut {\n");
        Str epi = { 0 };
        int any = 0;
        for (int i = 0; i < out->out.n; i++) {
            DxSigElem* e = &out->out.e[i];
            if (sem_eq(e->name, "SV_Target")) {
                int k = e->comp_type == 1 ? K_U : e->comp_type == 2 ? K_I : K_F;
                sp(&g, "    @location(%u) c%u: %s,\n", e->index, e->index, vt(k, 4));
                if (k == K_F) sp(&epi, "    r_.c%u = o%u;\n", e->index, e->reg);
                else sp(&epi, "    r_.c%u = bitcast<%s>(o%u);\n", e->index, vt(k, 4), e->reg);
                any = 1;
            }
        }
        if (tr->ps_depth) { sp(&g, "    @builtin(frag_depth) depth: f32,\n"); sp(&epi, "    r_.depth = odepth;\n"); out->ps_depth = 1; any = 1; }
        if (!any) sp(&g, "    @location(0) c0: vec4<f32>,\n");
        sp(&g, "}\n@fragment fn main(i: FIn) -> FOut {\n%s    uew_body();\n    var r_: FOut;\n%s    return r_;\n}\n", pro.p ? pro.p : "", epi.p ? epi.p : "");
        free(pro.p); free(epi.p);
    } else {
        int x = out->cs[0] ? out->cs[0] : 1, y = out->cs[1] ? out->cs[1] : 1, z = out->cs[2] ? out->cs[2] : 1;
        sp(&g, "@compute @workgroup_size(%d, %d, %d) fn main(@builtin(global_invocation_id) gid: vec3<u32>, @builtin(workgroup_id) wid: vec3<u32>,"
               " @builtin(local_invocation_id) lid: vec3<u32>, @builtin(local_invocation_index) lix: u32) {\n"
               "    vtid = vec4<f32>(bitcast<vec3<f32>>(gid), 0.0);\n    vgid = vec4<f32>(bitcast<vec3<f32>>(wid), 0.0);\n"
               "    vtidg = vec4<f32>(bitcast<vec3<f32>>(lid), 0.0);\n    vtidf = vec4<f32>(bitcast<f32>(lix));\n    uew_body();\n}\n", x, y, z);
    }
    out->wgsl = no_fold(g.p);
    if (lib && opt->prefix) out->wgsl = prefix_names(out->wgsl, opt->prefix);
    for (int i = 0; i < 8; i++) out->uav[i].atomic = tr->uav_atomic[i];
    free(tr->body.p); free(decl.p); free(icb.p);
    free(tr); free(rd);
    return ok;
}

int dxbc_to_wgsl(const uint8_t* code, size_t size, const DxSig* vs_out, DxWgsl* out)
{
    DxWgslOpts o = { 0 };
    o.vs_out = vs_out;
    return dxbc_to_wgsl_ex(code, size, &o, out);
}

int dxbc_reflect(const uint8_t* code, size_t size, DxWgsl* out)
{
    int r = dxbc_to_wgsl(code, size, NULL, out);
    free(out->wgsl);
    out->wgsl = NULL;
    return r;
}

void dxbc_wgsl_free(DxWgsl* s)
{
    free(s->wgsl);
    s->wgsl = NULL;
}
