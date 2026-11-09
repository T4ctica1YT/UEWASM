#include "dxbc.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char* p; size_t n, cap; } Str;

static void sp(Str* s, const char* fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(s->p ? s->p + s->n : NULL, s->p ? s->cap - s->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (s->p && s->n + (size_t)k < s->cap) { s->n += (size_t)k; return; }
        s->cap = (s->cap + (size_t)k + 1) * 2;
        s->p = realloc(s->p, s->cap);
    }
}

enum { T_TEMP, T_INPUT, T_OUTPUT, T_INDEXABLE, T_IMM32, T_IMM64, T_SAMPLER, T_RESOURCE, T_CB, T_ICB, T_LABEL, T_PRIMID, T_ODEPTH,
       T_NULL, T_RAST, T_OMASK, T_UAV = 30, T_TID = 32, T_GID = 33, T_TIDG = 34, T_COVERAGE = 35, T_TIDGF = 36, T_GSINST = 37,
       T_ODEPTH_GE = 38, T_ODEPTH_LE = 39 };

typedef struct Op {
    int type, ncomp, sel, dims, neg, absm;
    uint8_t mask, swz[4];
    uint32_t imm[4];
    struct { uint32_t imm; struct Op* rel; } idx[3];
} Op;

typedef struct {
    const uint32_t* t;
    size_t n, pos;
    Op pool[4096];
    int npool;
} Rd;

static Op* new_op(Rd* r) { return r->npool < 4096 ? &r->pool[r->npool++] : &r->pool[4095]; }

static Op* read_op(Rd* r);

static void read_index(Rd* r, Op* o, int i, int repr)
{
    o->idx[i].rel = NULL;
    o->idx[i].imm = 0;
    if (repr == 0 || repr == 3) o->idx[i].imm = r->t[r->pos++];
    else if (repr == 1 || repr == 4) { o->idx[i].imm = r->t[r->pos]; r->pos += 2; }
    if (repr >= 2) o->idx[i].rel = read_op(r);
}

static Op* read_op(Rd* r)
{
    Op* o = new_op(r);
    memset(o, 0, sizeof *o);
    uint32_t tok = r->t[r->pos++];
    int nc = tok & 3;
    o->ncomp = nc == 0 ? 0 : nc == 1 ? 1 : 4;
    o->sel = (tok >> 2) & 3;
    o->type = (int)((tok >> 12) & 0xFF);
    o->dims = (int)((tok >> 20) & 3);
    o->mask = 0xF;
    for (int i = 0; i < 4; i++) o->swz[i] = (uint8_t)i;
    if (o->ncomp == 4) {
        if (o->sel == 0) o->mask = (uint8_t)((tok >> 4) & 0xF);
        else if (o->sel == 1) for (int i = 0; i < 4; i++) o->swz[i] = (uint8_t)((tok >> (4 + 2 * i)) & 3);
        else for (int i = 0; i < 4; i++) o->swz[i] = (uint8_t)((tok >> 4) & 3);
    } else if (o->ncomp == 1) {
        for (int i = 0; i < 4; i++) o->swz[i] = 0;
        o->mask = 1;
    } else {
        o->mask = 0;
    }
    if (tok >> 31) {
        uint32_t ext = r->t[r->pos++];
        if ((ext & 0x3F) == 1) {
            uint32_t mod = (ext >> 6) & 0xFF;
            o->neg = (mod & 1) != 0;
            o->absm = (mod & 2) != 0;
        }
    }
    if (o->type == T_IMM32) {
        for (int i = 0; i < o->ncomp; i++) o->imm[i] = r->t[r->pos++];
        if (o->ncomp == 1) o->imm[1] = o->imm[2] = o->imm[3] = o->imm[0];
    } else if (o->type == T_IMM64) {
        for (int i = 0; i < o->ncomp; i++) { o->imm[i] = r->t[r->pos]; r->pos += 2; }
    } else {
        for (int i = 0; i < o->dims; i++) read_index(r, o, i, (int)((tok >> (22 + 3 * i)) & 7));
    }
    return o;
}

typedef struct {
    DxShader* sh;
    Str body, decl;
    int stage, indent, temps;
    int samp_cmp[16];
    int tex_dim[128], tex_ret[128], tex_buf[128], tex_raw[128], tex_stride[128];
    int nidx_temps;
    int out_regs[64];
    int ps_depth;
} Tr;

enum { K_F, K_I, K_U };

static const char* comp = "xyzw";

static int popc(int m) { int n = 0; for (int i = 0; i < 4; i++) n += (m >> i) & 1; return n; }

static void fmt_float(char* b, uint32_t bits)
{
    float f;
    memcpy(&f, &bits, 4);
    uint32_t e = (bits >> 23) & 0xFF;
    if (e == 0xFF || (e == 0 && (bits & 0x7FFFFF))) { sprintf(b, "uintBitsToFloat(%uu)", bits); return; }
    char t[48];
    sprintf(t, "%.9g", f);
    float back = strtof(t, NULL);
    uint32_t bb;
    memcpy(&bb, &back, 4);
    if (bb != bits) { sprintf(b, "uintBitsToFloat(%uu)", bits); return; }
    if (!strchr(t, '.') && !strchr(t, 'e') && !strchr(t, 'n') && !strchr(t, 'i')) strcat(t, ".0");
    sprintf(b, "%s", t);
}

static void imm_comp(char* b, uint32_t v, int kind)
{
    if (kind == K_F) fmt_float(b, v);
    else if (kind == K_I) { if (v == 0x80000000u) sprintf(b, "int(0x80000000u)"); else sprintf(b, "%d", (int32_t)v); }
    else sprintf(b, "%uu", v);
}

static void base_reg(Tr* tr, Str* s, Op* o);

static void idx_expr(Tr* tr, Str* s, Op* o, int i)
{
    if (o->idx[i].rel) {
        Op* rel = o->idx[i].rel;
        sp(s, "(floatBitsToInt(");
        base_reg(tr, s, rel);
        sp(s, ".%c)", comp[rel->swz[0]]);
        if (o->idx[i].imm) sp(s, " + %d", (int)o->idx[i].imm);
        sp(s, ")");
    } else sp(s, "%u", o->idx[i].imm);
}

static void base_reg(Tr* tr, Str* s, Op* o)
{
    switch (o->type) {
    case T_TEMP: sp(s, "r%u", o->idx[0].imm); break;
    case T_INPUT:
        sp(s, "v[");
        idx_expr(tr, s, o, o->dims == 2 ? 1 : 0);
        sp(s, "]");
        break;
    case T_OUTPUT: sp(s, "o%u", o->idx[0].imm); break;
    case T_INDEXABLE: sp(s, "x%u[", o->idx[0].imm); idx_expr(tr, s, o, 1); sp(s, "]"); break;
    case T_CB: sp(s, "%scb%u[", tr->stage == 0 ? "ps_" : "vs_", o->idx[0].imm); idx_expr(tr, s, o, 1); sp(s, "]"); break;
    case T_ICB: sp(s, "icb["); idx_expr(tr, s, o, 0); sp(s, "]"); break;
    case T_ODEPTH: case T_ODEPTH_GE: case T_ODEPTH_LE: sp(s, "odepth"); break;
    case T_PRIMID: sp(s, "vec4(0.0)"); break;
    case T_COVERAGE: sp(s, "vec4(uintBitsToFloat(0xFFFFFFFFu))"); break;
    case T_GSINST: sp(s, "vec4(0.0)"); break;
    default: sp(s, "vec4(0.0)"); break;
    }
    (void)tr;
}

static const char* vec_t(int kind, int n)
{
    static const char* t[3][5] = { { "", "float", "vec2", "vec3", "vec4" }, { "", "int", "ivec2", "ivec3", "ivec4" }, { "", "uint", "uvec2", "uvec3", "uvec4" } };
    return t[kind][n];
}

/* Read operand o as `n` components chosen by dmask (component-wise) or by first n swizzle entries (dmask < 0). */
static void src(Tr* tr, Str* s, Op* o, int dmask, int nfixed, int kind)
{
    int sel[4], n = 0;
    if (dmask >= 0) { for (int i = 0; i < 4; i++) if (dmask & (1 << i)) sel[n++] = o->swz[i]; }
    else { for (int i = 0; i < nfixed; i++) sel[n++] = o->swz[i]; }
    if (o->type == T_IMM32 || o->type == T_IMM64) {
        char b[64];
        if (o->neg) sp(s, "-");
        sp(s, "%s(", vec_t(kind, n));
        for (int i = 0; i < n; i++) {
            imm_comp(b, o->imm[sel[i] & 3], kind);
            sp(s, "%s%s", i ? ", " : "", b);
        }
        sp(s, ")");
        return;
    }
    Str t = { 0 };
    base_reg(tr, &t, o);
    sp(&t, ".");
    for (int i = 0; i < n; i++) sp(&t, "%c", comp[sel[i]]);
    const char* conv = kind == K_F ? "" : kind == K_I ? "floatBitsToInt" : "floatBitsToUint";
    if (o->neg) sp(s, "-");
    if (o->absm) sp(s, "abs(");
    sp(s, "%s(%s)", conv, t.p);
    if (o->absm) sp(s, ")");
    free(t.p);
}

static void line(Tr* tr) { for (int i = 0; i < tr->indent + 1; i++) sp(&tr->body, "    "); }

/* Assign `expr` (of kind, with popc(mask) components) to destination operand d. */
static void assign(Tr* tr, Op* d, int kind, int sat, const char* expr)
{
    if (d->type == T_NULL) return;
    int m = d->mask ? d->mask : 1;
    int n = popc(m);
    line(tr);
    if (d->type == T_ODEPTH || d->type == T_ODEPTH_GE || d->type == T_ODEPTH_LE) {
        sp(&tr->body, "odepth = %s%s%s;\n", sat ? "clamp(" : "", expr, sat ? ", 0.0, 1.0)" : "");
        return;
    }
    Str b = { 0 };
    base_reg(tr, &b, d);
    sp(&tr->body, "%s.", b.p);
    free(b.p);
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&tr->body, "%c", comp[i]);
    const char* conv = kind == K_F ? "" : kind == K_I ? "intBitsToFloat" : "uintBitsToFloat";
    if (sat && kind == K_F) sp(&tr->body, " = clamp(%s(%s), 0.0, 1.0);\n", vec_t(K_F, n), expr);
    else sp(&tr->body, " = %s(%s(%s));\n", conv, vec_t(kind, n), expr);
}

static int dmask(Op* d) { return d->type == T_NULL ? 1 : (d->mask ? d->mask : 1); }

static void bin(Tr* tr, Op** o, int sat, int kind, const char* op, int func)
{
    int m = dmask(o[0]);
    Str e = { 0 };
    if (func) { sp(&e, "%s(", op); src(tr, &e, o[1], m, 0, kind); sp(&e, ", "); src(tr, &e, o[2], m, 0, kind); sp(&e, ")"); }
    else { sp(&e, "("); src(tr, &e, o[1], m, 0, kind); sp(&e, " %s ", op); src(tr, &e, o[2], m, 0, kind); sp(&e, ")"); }
    assign(tr, o[0], kind, sat, e.p);
    free(e.p);
}

static void un(Tr* tr, Op** o, int sat, int kind_in, int kind_out, const char* fn)
{
    int m = dmask(o[0]);
    Str e = { 0 };
    sp(&e, "%s(", fn);
    src(tr, &e, o[1], m, 0, kind_in);
    sp(&e, ")");
    assign(tr, o[0], kind_out, sat, e.p);
    free(e.p);
}

static void cmp(Tr* tr, Op** o, int kind, const char* vecfn, const char* op)
{
    int m = dmask(o[0]), n = popc(m);
    Str e = { 0 };
    if (n == 1) { sp(&e, "(("); src(tr, &e, o[1], m, 0, kind); sp(&e, ") %s (", op); src(tr, &e, o[2], m, 0, kind); sp(&e, ") ? 0xFFFFFFFFu : 0u)"); }
    else { sp(&e, "%s(%s(", vec_t(K_U, n), vecfn); src(tr, &e, o[1], m, 0, kind); sp(&e, ", "); src(tr, &e, o[2], m, 0, kind); sp(&e, ")) * 0xFFFFFFFFu"); }
    assign(tr, o[0], K_U, 0, e.p);
    free(e.p);
}

static void dot(Tr* tr, Op** o, int sat, int n)
{
    Str e = { 0 };
    int m = dmask(o[0]);
    sp(&e, "%s(dot(", vec_t(K_F, popc(m)));
    src(tr, &e, o[1], -1, n, K_F);
    sp(&e, ", ");
    src(tr, &e, o[2], -1, n, K_F);
    sp(&e, "))");
    assign(tr, o[0], K_F, sat, e.p);
    free(e.p);
}

static int coord_n(int dim) { return dim == 2 || dim == 1 ? 2 : dim == 5 ? 2 : dim == 3 || dim == 4 ? 2 : dim == 9 ? 4 : dim == 6 ? 3 : 3; }

/* D3D resource dimension: 1 buffer, 2 tex1d, 3 tex2d, 4 tex2dms, 5 tex3d, 6 cube, 7 tex1darray, 8 tex2darray, 9 tex2dmsarray, 10 cubearray */
static int gl_coords(int dim) { switch (dim) { case 2: return 1; case 3: case 4: return 2; case 5: return 3; case 6: return 3; case 7: return 2; case 8: case 9: return 3; case 10: return 3; default: return 1; } }

static int pair_index(Tr* tr, int t, int s, int shadow)
{
    DxShader* sh = tr->sh;
    for (int i = 0; i < sh->npairs; i++)
        if (sh->pairs[i].tex == t && sh->pairs[i].samp == s && sh->pairs[i].shadow == shadow) return i;
    if (sh->npairs == 64) return 0;
    DxPair* p = &sh->pairs[sh->npairs];
    p->tex = t;
    p->samp = s;
    p->shadow = shadow;
    p->dim = tr->tex_dim[t & 127];
    p->ret_type = tr->tex_ret[t & 127];
    return sh->npairs++;
}

static void sampler_name(Tr* tr, Str* s, int pi) { sp(s, "%s_tex%d", tr->stage == 0 ? "ps" : "vs", pi); }

static void coord_expr(Tr* tr, Str* e, Op* addr, int dim, int kind)
{
    int n = gl_coords(dim);
    if (dim == 2) { sp(e, "vec2("); src(tr, e, addr, -1, 1, kind); sp(e, ", 0.5)"); return; }
    if (dim == 7) { sp(e, "vec3("); src(tr, e, addr, -1, 1, kind); sp(e, ", 0.5, "); Op t = *addr; t.swz[0] = addr->swz[1]; src(tr, e, &t, -1, 1, kind); sp(e, ")"); return; }
    src(tr, e, addr, -1, n, kind);
}

static void tex_result(Tr* tr, Op* d, Op* res, const char* call, int ret)
{
    int m = dmask(d);
    Str e = { 0 };
    const char* conv = ret == 1 ? "intBitsToFloat" : ret == 2 ? "uintBitsToFloat" : "";
    sp(&e, "%s(%s).", conv, call);
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&e, "%c", comp[res->swz[i]]);
    assign(tr, d, K_F, 0, e.p);
    free(e.p);
}

static void emit_sample(Tr* tr, int opc, Op** o, int nops, int sat, int off[3], int has_off)
{
    (void)nops;
    (void)sat;
    Op *d = o[0], *addr = o[1], *res = o[2], *smp = o[3];
    int t = (int)res->idx[0].imm, s = (int)smp->idx[0].imm;
    int dim = tr->tex_dim[t & 127];
    int shadow = opc == 70 || opc == 71;
    int pi = pair_index(tr, t, s, shadow);
    Str c = { 0 };
    sampler_name(tr, &c, pi);
    Str co = { 0 };
    if (shadow) {
        int n = gl_coords(dim);
        sp(&co, "vec%d(", n + 1);
        coord_expr(tr, &co, addr, dim, K_F);
        sp(&co, ", ");
        src(tr, &co, o[4], -1, 1, K_F);
        sp(&co, ")");
    } else coord_expr(tr, &co, addr, dim, K_F);
    Str call = { 0 };
    char offs[64] = "";
    if (has_off && dim != 6) {
        int n = gl_coords(dim) - (dim == 8 ? 1 : 0);
        if (n == 1) n = 2;
        sprintf(offs, ", ivec%d(%d, %d%s)", n, off[0], off[1], n == 3 ? (off[2] ? ", 1" : ", 0") : "");
    }
    switch (opc) {
    case 69: sp(&call, has_off && *offs ? "textureOffset(%s, %s%s)" : "texture(%s, %s)", c.p, co.p, offs); break;
    case 70: sp(&call, "vec4(texture(%s, %s))", c.p, co.p); break;
    case 71: sp(&call, dim == 8 ? "vec4(texture(%s, %s))" : "vec4(textureLod(%s, %s, 0.0))", c.p, co.p); break;
    case 72: { Str l = { 0 }; src(tr, &l, o[4], -1, 1, K_F); sp(&call, *offs ? "textureLodOffset(%s, %s, %s%s)" : "textureLod(%s, %s, %s)", c.p, co.p, l.p, offs); free(l.p); break; }
    case 74: { Str l = { 0 }; src(tr, &l, o[4], -1, 1, K_F); sp(&call, "texture(%s, %s, %s)", c.p, co.p, l.p); free(l.p); break; }
    case 73: {
        int n = gl_coords(dim) - (dim == 8 ? 1 : 0);
        if (n < 2) n = 2;
        Str dx = { 0 }, dy = { 0 };
        src(tr, &dx, o[4], -1, n, K_F);
        src(tr, &dy, o[5], -1, n, K_F);
        sp(&call, "textureGrad(%s, %s, %s, %s)", c.p, co.p, dx.p, dy.p);
        free(dx.p); free(dy.p);
        break;
    }
    }
    if (shadow) {
        Str e = { 0 };
        sp(&e, "%s.x", call.p);
        Str full = { 0 };
        int m = dmask(d);
        sp(&full, "%s(%s)", vec_t(K_F, popc(m)), e.p);
        assign(tr, d, K_F, 0, full.p);
        free(e.p); free(full.p);
    } else tex_result(tr, d, res, call.p, tr->tex_ret[t & 127]);
    free(c.p); free(co.p); free(call.p);
}

static void emit_ld(Tr* tr, Op** o, int off[3], int has_off)
{
    Op *d = o[0], *addr = o[1], *res = o[2];
    int t = (int)res->idx[0].imm;
    int dim = tr->tex_dim[t & 127];
    int pi = pair_index(tr, t, -1, 0);
    Str c = { 0 }, call = { 0 }, co = { 0 }, lod = { 0 };
    sampler_name(tr, &c, pi);
    if (dim == 1) {
        sp(&co, "uew_buf_coord(");
        src(tr, &co, addr, -1, 1, K_I);
        sp(&co, ")");
        sp(&call, "texelFetch(%s, %s, 0)", c.p, co.p);
    } else {
        int n = gl_coords(dim);
        if (dim == 2) { sp(&co, "ivec2("); src(tr, &co, addr, -1, 1, K_I); sp(&co, ", 0)"); }
        else src(tr, &co, addr, -1, n, K_I);
        if (dim == 4 || dim == 9) sp(&lod, "0");
        else { Op l = *addr; l.swz[0] = addr->swz[n == 1 ? 1 : n]; src(tr, &lod, &l, -1, 1, K_I); }
        if (has_off) sp(&call, "texelFetchOffset(%s, %s, %s, ivec%d(%d, %d%s))", c.p, co.p, lod.p, n < 2 ? 2 : n, off[0], off[1], n == 3 ? ", 0" : "");
        else sp(&call, "texelFetch(%s, %s, %s)", c.p, co.p, lod.p);
    }
    tex_result(tr, d, res, call.p, tr->tex_ret[t & 127]);
    free(c.p); free(call.p); free(co.p); free(lod.p);
}

static void emit_resinfo(Tr* tr, Op** o, int rtype)
{
    Op *d = o[0], *res = o[2];
    int t = (int)res->idx[0].imm;
    int dim = tr->tex_dim[t & 127];
    int pi = pair_index(tr, t, -1, 0);
    Str c = { 0 }, lod = { 0 }, e = { 0 };
    sampler_name(tr, &c, pi);
    src(tr, &lod, o[1], -1, 1, K_I);
    const char* k = rtype == 2 ? "uint" : "float";
    const char* vk = rtype == 2 ? "uvec4" : "vec4";
    if (dim == 5 || dim == 8) sp(&e, "%s(%s(textureSize(%s, %s)), %s(%s_mips[%d]))", vk, rtype == 2 ? "uvec3" : "vec3", c.p, lod.p, k, tr->stage == 0 ? "ps" : "vs", pi);
    else sp(&e, "%s(%s(textureSize(%s, %s)), 0, %s(%s_mips[%d]))", vk, rtype == 2 ? "uvec2" : "vec2", c.p, lod.p, k, tr->stage == 0 ? "ps" : "vs", pi);
    if (rtype == 1) {
        Str r = { 0 };
        sp(&r, "(vec4(1.0) / %s)", e.p);
        free(e.p);
        e = r;
    }
    Str full = { 0 };
    int m = dmask(d);
    sp(&full, "(%s).", e.p);
    for (int i = 0; i < 4; i++) if (m & (1 << i)) sp(&full, "%c", comp[res->swz[i]]);
    assign(tr, d, rtype == 2 ? K_U : K_F, 0, full.p);
    free(c.p); free(lod.p); free(e.p); free(full.p);
}

static const char* sanitize(const char* s, char* buf)
{
    int i = 0;
    for (; s[i] && i < 30; i++) buf[i] = (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= '0' && s[i] <= '9') ? s[i] : '_';
    buf[i] = 0;
    return buf;
}

static int parse_sig(const uint8_t* d, size_t len, int big, DxSig* out)
{
    if (len < 8) return 0;
    uint32_t n = *(const uint32_t*)d;
    size_t esz = big == 2 ? 28 : big ? 32 : 24;
    out->n = 0;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        const uint8_t* e = d + 8 + esz * i;
        if (big) e += 4;
        uint32_t name_off = *(const uint32_t*)e;
        DxSigElem* x = &out->e[out->n++];
        snprintf(x->name, sizeof x->name, "%s", name_off < len ? (const char*)d + name_off : "X");
        x->index = *(const uint32_t*)(e + 4);
        x->sysval = *(const uint32_t*)(e + 8);
        x->comp_type = *(const uint32_t*)(e + 12);
        x->reg = *(const uint32_t*)(e + 16);
        x->mask = e[20];
        x->rw_mask = e[21];
    }
    return 1;
}

static const char* HELPERS =
    "ivec2 uew_buf_coord(int i) { return ivec2(i & 4095, i >> 12); }\n"
    "float uew_f16tof32(uint v) { return unpackHalf2x16(v).x; }\n"
    "uint uew_f32tof16(float v) { return packHalf2x16(vec2(v, 0.0)) & 0xFFFFu; }\n"
    "uint uew_bitcount(uint v) { v = v - ((v >> 1u) & 0x55555555u); v = (v & 0x33333333u) + ((v >> 2u) & 0x33333333u); return (((v + (v >> 4u)) & 0x0F0F0F0Fu) * 0x01010101u) >> 24u; }\n"
    "uvec2 uew_bitcount(uvec2 v) { return uvec2(uew_bitcount(v.x), uew_bitcount(v.y)); }\n"
    "uvec3 uew_bitcount(uvec3 v) { return uvec3(uew_bitcount(v.x), uew_bitcount(v.y), uew_bitcount(v.z)); }\n"
    "uvec4 uew_bitcount(uvec4 v) { return uvec4(uew_bitcount(v.x), uew_bitcount(v.y), uew_bitcount(v.z), uew_bitcount(v.w)); }\n"
    "uint uew_msb(uint v) { if (v == 0u) return 0xFFFFFFFFu; uint r = 0u; for (int i = 16; i > 0; i >>= 1) if (v >= (1u << uint(i))) { v >>= uint(i); r += uint(i); } return r; }\n"
    "uint uew_lsb(uint v) { if (v == 0u) return 0xFFFFFFFFu; uint r = 0u; while ((v & 1u) == 0u) { v >>= 1u; r++; } return r; }\n"
    "uint uew_ubfe(uint w, uint o, uint v) { w &= 31u; o &= 31u; if (w == 0u) return 0u; if (w + o < 32u) return (v << (32u - w - o)) >> (32u - w); return v >> o; }\n"
    "int uew_ibfe(uint w, uint o, int v) { w &= 31u; o &= 31u; if (w == 0u) return 0; if (w + o < 32u) return (v << int(32u - w - o)) >> int(32u - w); return v >> int(o); }\n"
    "uint uew_bfi(uint w, uint o, uint ins, uint b) { w &= 31u; o &= 31u; uint m = (w == 32u ? 0xFFFFFFFFu : ((1u << w) - 1u)) << o; return ((ins << o) & m) | (b & ~m); }\n"
    "uint uew_bfrev(uint v) { v = ((v >> 1u) & 0x55555555u) | ((v & 0x55555555u) << 1u); v = ((v >> 2u) & 0x33333333u) | ((v & 0x33333333u) << 2u); v = ((v >> 4u) & 0x0F0F0F0Fu) | ((v & 0x0F0F0F0Fu) << 4u); v = ((v >> 8u) & 0x00FF00FFu) | ((v & 0x00FF00FFu) << 8u); return (v >> 16u) | (v << 16u); }\n"
    "uint uew_umulhi(uint a, uint b) { uint al = a & 0xFFFFu, ah = a >> 16u, bl = b & 0xFFFFu, bh = b >> 16u; uint lo = al * bl, m1 = ah * bl, m2 = al * bh; uint mid = (lo >> 16u) + (m1 & 0xFFFFu) + (m2 & 0xFFFFu); return ah * bh + (m1 >> 16u) + (m2 >> 16u) + (mid >> 16u); }\n";

static void scalarized(Tr* tr, Op** o, const char* fn, int k1, int k2, int k3, int nargs, int kout)
{
    int m = dmask(o[0]);
    Str e = { 0 };
    int n = popc(m);
    sp(&e, "%s(", vec_t(kout, n));
    int first = 1;
    for (int i = 0; i < 4; i++) {
        if (!(m & (1 << i))) continue;
        if (!first) sp(&e, ", ");
        first = 0;
        sp(&e, "%s(", fn);
        int kinds[3] = { k1, k2, k3 };
        for (int a = 0; a < nargs; a++) {
            if (a) sp(&e, ", ");
            src(tr, &e, o[1 + a], 1 << i, 0, kinds[a]);
        }
        sp(&e, ")");
    }
    sp(&e, ")");
    assign(tr, o[0], kout, 0, e.p);
    free(e.p);
}

int dxbc_translate(const uint8_t* code, size_t size, DxShader* out)
{
    memset(out, 0, sizeof *out);
    if (size < 32 || memcmp(code, "DXBC", 4)) { snprintf(out->error, sizeof out->error, "not DXBC"); return 0; }
    uint32_t nchunks = *(const uint32_t*)(code + 28);
    const uint32_t* tokens = NULL;
    size_t ntok = 0;
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t off = *(const uint32_t*)(code + 32 + 4 * i);
        if (off + 8 > size) continue;
        const uint8_t* ch = code + off;
        uint32_t len = *(const uint32_t*)(ch + 4);
        const uint8_t* data = ch + 8;
        if (!memcmp(ch, "ISGN", 4)) parse_sig(data, len, 0, &out->in);
        else if (!memcmp(ch, "ISG1", 4)) parse_sig(data, len, 1, &out->in);
        else if (!memcmp(ch, "OSGN", 4)) parse_sig(data, len, 0, &out->out);
        else if (!memcmp(ch, "OSG1", 4)) parse_sig(data, len, 1, &out->out);
        else if (!memcmp(ch, "OSG5", 4)) parse_sig(data, len, 2, &out->out);
        else if (!memcmp(ch, "SHDR", 4) || !memcmp(ch, "SHEX", 4)) { tokens = (const uint32_t*)data; ntok = len / 4; }
    }
    if (!tokens) { snprintf(out->error, sizeof out->error, "no shader code"); return 0; }

    Tr* tr = calloc(1, sizeof *tr);
    Rd* rd = calloc(1, sizeof *rd);
    tr->sh = out;
    uint32_t ver = tokens[0];
    int prog = (int)(ver >> 16);
    tr->stage = prog;
    out->stage = prog;
    if (prog != 0 && prog != 1) {
        snprintf(out->error, sizeof out->error, "%s shaders need WebGPU", prog == 2 ? "geometry" : prog == 3 ? "hull" : prog == 4 ? "domain" : "compute");
        free(tr); free(rd);
        return 0;
    }
    rd->t = tokens;
    rd->n = tokens[1] < ntok ? tokens[1] : ntok;
    rd->pos = 2;
    int ok = 1;
    int max_out = -1, max_in = -1;
    Str icb = { 0 };
    int icb_n = 0;

    while (rd->pos < rd->n && ok) {
        rd->npool = 0;
        uint32_t tok = tokens[rd->pos];
        int opc = (int)(tok & 0x7FF);
        size_t len = (tok >> 24) & 0x7F;
        size_t start = rd->pos;
        if (opc == 53) {
            uint32_t cls = (tok >> 11) & 0x1FFFFF;
            len = tokens[rd->pos + 1];
            if (cls == 3) {
                icb_n = (int)((len - 2) / 4);
                sp(&icb, "const vec4 icb[%d] = vec4[%d](", icb_n, icb_n);
                for (int i = 0; i < icb_n; i++) {
                    char b[4][48];
                    for (int k = 0; k < 4; k++) fmt_float(b[k], tokens[rd->pos + 2 + 4 * i + k]);
                    sp(&icb, "%svec4(%s, %s, %s, %s)", i ? ", " : "", b[0], b[1], b[2], b[3]);
                }
                sp(&icb, ");\n");
            }
            rd->pos = start + len;
            continue;
        }
        if (!len) { ok = 0; snprintf(out->error, sizeof out->error, "zero-length opcode %d", opc); break; }
        rd->pos++;
        int sat = (tok >> 13) & 1;
        int test_nz = (tok >> 18) & 1;
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
            int dim = (int)((tok >> 11) & 0x1F);
            uint32_t rt = tokens[rd->pos++];
            int k = (int)(rt & 0xF);
            tr->tex_dim[slot] = dim;
            tr->tex_ret[slot] = k == 3 ? 1 : k == 4 ? 2 : 0;
            tr->tex_buf[slot] = dim == 1;
            DxResource* x = &out->res[out->nres < 128 ? out->nres++ : 127];
            x->slot = slot; x->dim = dim; x->ret_type = tr->tex_ret[slot]; x->is_buffer = dim == 1;
            break;
        }
        case 161: case 162: {
            Op* r = read_op(rd);
            int slot = (int)r->idx[0].imm & 127;
            tr->tex_dim[slot] = 1;
            tr->tex_ret[slot] = 2;
            tr->tex_buf[slot] = 1;
            tr->tex_raw[slot] = 1;
            if (opc == 162) tr->tex_stride[slot] = (int)tokens[rd->pos++];
            DxResource* x = &out->res[out->nres < 128 ? out->nres++ : 127];
            x->slot = slot; x->dim = 1; x->ret_type = 2; x->is_buffer = 1; x->raw = 1; x->structured = opc == 162; x->stride = tr->tex_stride[slot];
            break;
        }
        case 89: {
            Op* r = read_op(rd);
            int slot = (int)r->idx[0].imm & 15;
            int n = (int)r->idx[1].imm;
            if (n > out->cb_size[slot]) out->cb_size[slot] = n;
            break;
        }
        case 90: read_op(rd); break;
        case 104: tr->temps = (int)tokens[rd->pos++]; break;
        case 105: {
            uint32_t reg = tokens[rd->pos], n = tokens[rd->pos + 1];
            rd->pos += 3;
            sp(&tr->decl, "vec4 x%u[%u];\n", reg, n);
            break;
        }
        case 95: case 96: case 97: case 98: case 99: case 100: {
            Op* r = read_op(rd);
            int reg = r->dims == 2 ? (int)r->idx[1].imm : (int)r->idx[0].imm;
            if (r->type == T_INPUT && reg > max_in) max_in = reg;
            if (opc == 96 || opc == 97 || opc == 99 || opc == 100) {
                uint32_t sv = tokens[rd->pos++];
                if (sv == 6) out->uses_vertex_id = 1;
                if (sv == 8) out->uses_instance_id = 1;
            }
            break;
        }
        case 101: case 102: case 103: {
            Op* r = read_op(rd);
            if (r->type == T_OUTPUT && (int)r->idx[0].imm > max_out) max_out = (int)r->idx[0].imm;
            if (r->type == T_ODEPTH || r->type == T_ODEPTH_GE || r->type == T_ODEPTH_LE) tr->ps_depth = 1;
            if (opc != 101) rd->pos++;
            break;
        }
        case 91: case 92: case 93: case 94: case 106: case 143: case 144: case 145: case 146: case 147: case 148: case 149: case 150:
        case 151: case 152: case 153: case 154: case 155: case 156: case 157: case 158: case 159: case 160: case 206:
            break;
        default: {
            while (rd->pos < start + len && nops < 8) o[nops++] = read_op(rd);
            int m = nops ? dmask(o[0]) : 1;
            Str e = { 0 };
            switch (opc) {
            case 0: bin(tr, o, sat, K_F, "+", 0); break;
            case 56: bin(tr, o, sat, K_F, "*", 0); break;
            case 14: bin(tr, o, sat, K_F, "/", 0); break;
            case 51: bin(tr, o, sat, K_F, "min", 1); break;
            case 52: bin(tr, o, sat, K_F, "max", 1); break;
            case 50:
                sp(&e, "("); src(tr, &e, o[1], m, 0, K_F); sp(&e, " * "); src(tr, &e, o[2], m, 0, K_F); sp(&e, " + "); src(tr, &e, o[3], m, 0, K_F); sp(&e, ")");
                assign(tr, o[0], K_F, sat, e.p);
                break;
            case 15: dot(tr, o, sat, 2); break;
            case 16: dot(tr, o, sat, 3); break;
            case 17: dot(tr, o, sat, 4); break;
            case 54: {
                int k = (o[1]->type == T_IMM32) ? K_U : K_F;
                if (sat) k = K_F;
                src(tr, &e, o[1], m, 0, k);
                assign(tr, o[0], k, sat, e.p);
                break;
            }
            case 55: {
                int n = popc(m);
                if (n == 1) { sp(&e, "(("); src(tr, &e, o[1], m, 0, K_U); sp(&e, ") != 0u ? "); src(tr, &e, o[2], m, 0, K_F); sp(&e, " : "); src(tr, &e, o[3], m, 0, K_F); sp(&e, ")"); }
                else { sp(&e, "mix("); src(tr, &e, o[3], m, 0, K_F); sp(&e, ", "); src(tr, &e, o[2], m, 0, K_F); sp(&e, ", notEqual("); src(tr, &e, o[1], m, 0, K_U); sp(&e, ", %s(0u)))", vec_t(K_U, n)); }
                assign(tr, o[0], K_F, 0, e.p);
                break;
            }
            case 26: un(tr, o, sat, K_F, K_F, "fract"); break;
            case 64: un(tr, o, sat, K_F, K_F, "roundEven"); break;
            case 65: un(tr, o, sat, K_F, K_F, "floor"); break;
            case 66: un(tr, o, sat, K_F, K_F, "ceil"); break;
            case 67: un(tr, o, sat, K_F, K_F, "trunc"); break;
            case 68: un(tr, o, sat, K_F, K_F, "inversesqrt"); break;
            case 75: un(tr, o, sat, K_F, K_F, "sqrt"); break;
            case 25: un(tr, o, sat, K_F, K_F, "exp2"); break;
            case 47: un(tr, o, sat, K_F, K_F, "log2"); break;
            case 129: sp(&e, "(1.0 / "); src(tr, &e, o[1], m, 0, K_F); sp(&e, ")"); assign(tr, o[0], K_F, sat, e.p); break;
            case 11: case 122: case 123: un(tr, o, sat, K_F, K_F, "dFdx"); break;
            case 12: case 124: case 125: un(tr, o, sat, K_F, K_F, "dFdy"); break;
            case 77: {
                if (o[0]->type != T_NULL) { Str a = { 0 }; sp(&a, "sin("); src(tr, &a, o[2], dmask(o[0]), 0, K_F); sp(&a, ")"); assign(tr, o[0], K_F, sat, a.p); free(a.p); }
                if (o[1]->type != T_NULL) { Str a = { 0 }; sp(&a, "cos("); src(tr, &a, o[2], dmask(o[1]), 0, K_F); sp(&a, ")"); assign(tr, o[1], K_F, sat, a.p); free(a.p); }
                break;
            }
            case 24: cmp(tr, o, K_F, "equal", "=="); break;
            case 57: cmp(tr, o, K_F, "notEqual", "!="); break;
            case 49: cmp(tr, o, K_F, "lessThan", "<"); break;
            case 29: cmp(tr, o, K_F, "greaterThanEqual", ">="); break;
            case 32: cmp(tr, o, K_I, "equal", "=="); break;
            case 39: cmp(tr, o, K_I, "notEqual", "!="); break;
            case 34: cmp(tr, o, K_I, "lessThan", "<"); break;
            case 33: cmp(tr, o, K_I, "greaterThanEqual", ">="); break;
            case 79: cmp(tr, o, K_U, "lessThan", "<"); break;
            case 80: cmp(tr, o, K_U, "greaterThanEqual", ">="); break;
            case 30: bin(tr, o, 0, K_I, "+", 0); break;
            case 38:
                if (o[0]->type != T_NULL) { Str a = { 0 }; sp(&a, "%s(0)", vec_t(K_I, popc(dmask(o[0])))); assign(tr, o[0], K_I, 0, a.p); free(a.p); }
                if (o[1]->type != T_NULL) { Op* t[3] = { o[1], o[2], o[3] }; bin(tr, t, 0, K_I, "*", 0); }
                break;
            case 81:
                if (o[0]->type != T_NULL) scalarized(tr, (Op*[]){ o[0], o[2], o[3] }, "uew_umulhi", K_U, K_U, 0, 2, K_U);
                if (o[1]->type != T_NULL) { Op* t[3] = { o[1], o[2], o[3] }; bin(tr, t, 0, K_U, "*", 0); }
                break;
            case 78:
                if (o[0]->type != T_NULL) { Op* t[3] = { o[0], o[2], o[3] }; bin(tr, t, 0, K_U, "/", 0); }
                if (o[1]->type != T_NULL) { Op* t[3] = { o[1], o[2], o[3] }; bin(tr, t, 0, K_U, "%", 0); }
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
            case 41: sp(&e, "("); src(tr, &e, o[1], m, 0, K_I); sp(&e, " << ("); src(tr, &e, o[2], m, 0, K_I); sp(&e, " & 31))"); assign(tr, o[0], K_I, 0, e.p); break;
            case 42: sp(&e, "("); src(tr, &e, o[1], m, 0, K_I); sp(&e, " >> ("); src(tr, &e, o[2], m, 0, K_I); sp(&e, " & 31))"); assign(tr, o[0], K_I, 0, e.p); break;
            case 85: sp(&e, "("); src(tr, &e, o[1], m, 0, K_U); sp(&e, " >> ("); src(tr, &e, o[2], m, 0, K_U); sp(&e, " & 31u))"); assign(tr, o[0], K_U, 0, e.p); break;
            case 27: un(tr, o, 0, K_F, K_I, vec_t(K_I, popc(m))); break;
            case 28: sp(&e, "%s(max(", vec_t(K_U, popc(m))); src(tr, &e, o[1], m, 0, K_F); sp(&e, ", 0.0))"); assign(tr, o[0], K_U, 0, e.p); break;
            case 43: un(tr, o, sat, K_I, K_F, vec_t(K_F, popc(m))); break;
            case 86: un(tr, o, sat, K_U, K_F, vec_t(K_F, popc(m))); break;
            case 131: scalarized(tr, o, "uew_f16tof32", K_U, 0, 0, 1, K_F); break;
            case 130: scalarized(tr, o, "uew_f32tof16", K_F, 0, 0, 1, K_U); break;
            case 134: un(tr, o, 0, K_U, K_U, "uew_bitcount"); break;
            case 135: case 137: scalarized(tr, o, "uew_msb", K_U, 0, 0, 1, K_U); break;
            case 136: scalarized(tr, o, "uew_lsb", K_U, 0, 0, 1, K_U); break;
            case 138: scalarized(tr, o, "uew_ubfe", K_U, K_U, K_U, 3, K_U); break;
            case 139: scalarized(tr, o, "uew_ibfe", K_U, K_U, K_I, 3, K_I); break;
            case 140: {
                int n = popc(m), first = 1;
                sp(&e, "%s(", vec_t(K_U, n));
                for (int i = 0; i < 4; i++) {
                    if (!(m & (1 << i))) continue;
                    if (!first) sp(&e, ", ");
                    first = 0;
                    sp(&e, "uew_bfi(");
                    for (int a = 1; a <= 4; a++) { if (a > 1) sp(&e, ", "); src(tr, &e, o[a], 1 << i, 0, K_U); }
                    sp(&e, ")");
                }
                sp(&e, ")");
                assign(tr, o[0], K_U, 0, e.p);
                break;
            }
            case 141: scalarized(tr, o, "uew_bfrev", K_U, 0, 0, 1, K_U); break;
            case 142: {
                for (int d = 0; d < 2; d++) {
                    if (o[d]->type == T_NULL) continue;
                    Str x = { 0 };
                    int mm = dmask(o[d]), n = popc(mm);
                    Op* hi = d == 0 ? o[4] : o[3];
                    Op* lo = d == 0 ? o[3] : o[4];
                    if (n == 1) { sp(&x, "(("); src(tr, &x, o[2], mm, 0, K_U); sp(&x, ") != 0u ? "); src(tr, &x, hi, mm, 0, K_F); sp(&x, " : "); src(tr, &x, lo, mm, 0, K_F); sp(&x, ")"); }
                    else { sp(&x, "mix("); src(tr, &x, lo, mm, 0, K_F); sp(&x, ", "); src(tr, &x, hi, mm, 0, K_F); sp(&x, ", notEqual("); src(tr, &x, o[2], mm, 0, K_U); sp(&x, ", %s(0u)))", vec_t(K_U, n)); }
                    assign(tr, o[d], K_F, 0, x.p);
                    free(x.p);
                }
                break;
            }
            case 31: line(tr); sp(&tr->body, "if (floatBitsToUint("); { Str b = { 0 }; base_reg(tr, &b, o[0]); sp(&tr->body, "%s.%c) %s 0u) {\n", b.p, comp[o[0]->swz[0]], test_nz ? "!=" : "=="); free(b.p); } tr->indent++; break;
            case 18: tr->indent--; line(tr); sp(&tr->body, "} else {\n"); tr->indent++; break;
            case 21: case 22: case 23: tr->indent--; line(tr); sp(&tr->body, "}\n"); break;
            case 48: line(tr); sp(&tr->body, "for (int uew_guard = 0; uew_guard < 65536; uew_guard++) {\n"); tr->indent++; break;
            case 2: line(tr); sp(&tr->body, "break;\n"); break;
            case 7: line(tr); sp(&tr->body, "continue;\n"); break;
            case 3: case 8: case 63: case 13: {
                Str b = { 0 };
                src(tr, &b, o[0], -1, 1, K_U);
                line(tr);
                sp(&tr->body, "if ((%s) %s 0u) %s;\n", b.p, test_nz ? "!=" : "==", opc == 3 ? "break" : opc == 8 ? "continue" : opc == 63 ? "return" : "discard");
                free(b.p);
                break;
            }
            case 62: line(tr); sp(&tr->body, "return;\n"); break;
            case 76: { Str b = { 0 }; src(tr, &b, o[0], -1, 1, K_I); line(tr); sp(&tr->body, "switch (%s) {\n", b.p); free(b.p); tr->indent++; break; }
            case 6: line(tr); sp(&tr->body, "case %d:\n", (int)o[0]->imm[0]); break;
            case 10: line(tr); sp(&tr->body, "default:\n"); break;
            case 69: case 70: case 71: case 72: case 73: case 74: emit_sample(tr, opc, o, nops, sat, off, has_off); break;
            case 45: case 46: emit_ld(tr, o, off, has_off); break;
            case 61: emit_resinfo(tr, o, (int)((tok >> 11) & 3)); break;
            case 109: case 126: {
                Op *d = o[0], *res = o[2], *smp = o[3];
                int t = (int)res->idx[0].imm;
                int pi = pair_index(tr, t, (int)smp->idx[0].imm, 0);
                Str cn = { 0 }, co = { 0 }, call = { 0 };
                sampler_name(tr, &cn, pi);
                coord_expr(tr, &co, o[1], tr->tex_dim[t & 127], K_F);
                int ch = (int)smp->swz[0];
                sp(&call, "uew_gather_%d(%s, %s)", ch, cn.p, co.p);
                Str full = { 0 };
                sp(&full, "vec4(texture(%s, %s).%c)", cn.p, co.p, comp[ch]);
                tex_result(tr, d, res, full.p, tr->tex_ret[t & 127]);
                free(cn.p); free(co.p); free(call.p); free(full.p);
                break;
            }
            case 165: case 167: {
                int structured = opc == 167;
                Op *d = o[0], *res = o[structured ? 3 : 2];
                int t = (int)res->idx[0].imm;
                int pi = pair_index(tr, t, -1, 0);
                Str cn = { 0 }, addr = { 0 }, full = { 0 };
                sampler_name(tr, &cn, pi);
                if (structured) { sp(&addr, "(("); src(tr, &addr, o[1], -1, 1, K_I); sp(&addr, ") * %d + (", tr->tex_stride[t & 127]); src(tr, &addr, o[2], -1, 1, K_I); sp(&addr, ")) >> 2"); }
                else { sp(&addr, "("); src(tr, &addr, o[1], -1, 1, K_I); sp(&addr, ") >> 2"); }
                int mm = dmask(d), first = 1;
                sp(&full, "%s(", vec_t(K_U, popc(mm)));
                for (int i = 0; i < 4; i++) {
                    if (!(mm & (1 << i))) continue;
                    if (!first) sp(&full, ", ");
                    first = 0;
                    sp(&full, "texelFetch(%s, uew_buf_coord((%s) + %d), 0).x", cn.p, addr.p, res->swz[i]);
                }
                sp(&full, ")");
                assign(tr, d, K_U, 0, full.p);
                free(cn.p); free(addr.p); free(full.p);
                break;
            }
            case 121: {
                Op* res = o[1];
                int t = (int)res->idx[0].imm;
                int pi = pair_index(tr, t, -1, 0);
                Str cn = { 0 };
                sampler_name(tr, &cn, pi);
                sp(&e, "%s(uint(%s_bufsize[%d]))", vec_t(K_U, popc(m)), tr->stage == 0 ? "ps" : "vs", pi);
                assign(tr, o[0], K_U, 0, e.p);
                free(cn.p);
                break;
            }
            case 58: case 9: case 19: case 20: case 190: break;
            default:
                line(tr);
                sp(&tr->body, "/* unsupported opcode %d */\n", opc);
                if (!out->error[0]) snprintf(out->error, sizeof out->error, "unsupported opcode %d", opc);
                break;
            }
            free(e.p);
            break;
        }
        }
        rd->pos = start + len;
    }

    Str g = { 0 };
    sp(&g, "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler3D;\n"
           "precision highp samplerCube;\nprecision highp sampler2DArray;\nprecision highp isampler2D;\nprecision highp usampler2D;\n"
           "precision highp isampler3D;\nprecision highp usampler3D;\nprecision highp isampler2DArray;\nprecision highp usampler2DArray;\n"
           "precision highp sampler2DShadow;\nprecision highp sampler2DArrayShadow;\nprecision highp samplerCubeShadow;\n");
    sp(&g, "%s", HELPERS);
    const char* pre = prog == 0 ? "ps" : "vs";
    for (int i = 0; i < 16; i++)
        if (out->cb_size[i]) sp(&g, "layout(std140) uniform %s_CB%d { vec4 %s_cb%d[%d]; };\n", pre, i, pre, i, out->cb_size[i]);
    for (int i = 0; i < out->npairs; i++) {
        DxPair* p = &out->pairs[i];
        const char* pfx = p->ret_type == 1 ? "i" : p->ret_type == 2 ? "u" : "";
        const char* t;
        switch (p->dim) {
        case 5: t = "sampler3D"; break;
        case 6: case 10: t = "samplerCube"; break;
        case 7: case 8: case 9: t = "sampler2DArray"; break;
        default: t = "sampler2D"; break;
        }
        if (p->shadow) sp(&g, "uniform %s%s %s_tex%d;\n", t, "Shadow", pre, i);
        else sp(&g, "uniform %s%s %s_tex%d;\n", pfx, t, pre, i);
    }
    if (out->npairs) sp(&g, "uniform int %s_mips[%d];\nuniform int %s_bufsize[%d];\n", pre, out->npairs, pre, out->npairs);
    if (icb.p) sp(&g, "%s", icb.p);
    for (int i = 0; i < tr->temps; i++) sp(&g, "vec4 r%d = vec4(0.0);\n", i);
    int nin = max_in + 1;
    for (int i = 0; i < out->in.n; i++) if ((int)out->in.e[i].reg + 1 > nin && out->in.e[i].reg < 64) nin = (int)out->in.e[i].reg + 1;
    sp(&g, "vec4 v[%d];\n", nin > 0 ? nin : 1);
    int nout = max_out + 1;
    for (int i = 0; i < out->out.n; i++) if ((int)out->out.e[i].reg + 1 > nout && out->out.e[i].reg < 64) nout = (int)out->out.e[i].reg + 1;
    for (int i = 0; i < nout; i++) sp(&g, "vec4 o%d = vec4(0.0);\n", i);
    if (prog == 0) sp(&g, "float odepth = 0.0;\n");
    if (tr->decl.p) sp(&g, "%s", tr->decl.p);

    Str pro = { 0 }, epi = { 0 };
    char nb[40];
    if (prog == 1) {
        for (int i = 0; i < out->in.n; i++) {
            DxSigElem* e = &out->in.e[i];
            if (e->sysval == 6) { sp(&pro, "    v[%u].x = intBitsToFloat(gl_VertexID);\n", e->reg); continue; }
            if (e->sysval == 8) { sp(&pro, "    v[%u].x = intBitsToFloat(gl_InstanceID);\n", e->reg); continue; }
            int k = e->comp_type == 1 ? K_U : e->comp_type == 2 ? K_I : K_F;
            sp(&g, "layout(location = %u) in %s in_%u;\n", e->reg, vec_t(k, 4), e->reg);
            sp(&pro, "    v[%u] = %s(in_%u);\n", e->reg, k == K_F ? "" : k == K_I ? "intBitsToFloat" : "uintBitsToFloat", e->reg);
        }
        for (int i = 0; i < out->out.n; i++) {
            DxSigElem* e = &out->out.e[i];
            if (e->sysval == 1) {
                sp(&epi, "    gl_Position = o%u;\n    gl_Position.y = -gl_Position.y;\n    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;\n", e->reg);
                continue;
            }
            if (e->sysval) continue;
            int n = popc((int)e->mask), k = e->comp_type == 1 ? K_U : e->comp_type == 2 ? K_I : K_F;
            char sw[5] = { 0 };
            for (int c = 0, j = 0; c < 4; c++) if (e->mask & (1 << c)) sw[j++] = comp[c];
            sp(&g, "%sout %s vary_%s%u;\n", k == K_F ? "" : "flat ", vec_t(k, n), sanitize(e->name, nb), e->index);
            sp(&epi, "    vary_%s%u = %s(o%u.%s);\n", nb, e->index, k == K_F ? "" : k == K_I ? "floatBitsToInt" : "floatBitsToUint", e->reg, sw);
        }
    } else {
        for (int i = 0; i < out->in.n; i++) {
            DxSigElem* e = &out->in.e[i];
            if (e->sysval == 1) { sp(&pro, "    v[%u] = vec4(gl_FragCoord.xy, gl_FragCoord.z, 1.0 / gl_FragCoord.w);\n", e->reg); continue; }
            if (e->sysval == 9) { sp(&pro, "    v[%u].x = uintBitsToFloat(gl_FrontFacing ? 0xFFFFFFFFu : 0u);\n", e->reg); continue; }
            if (e->sysval) continue;
            int n = popc((int)e->mask), k = e->comp_type == 1 ? K_U : e->comp_type == 2 ? K_I : K_F;
            char sw[5] = { 0 };
            for (int c = 0, j = 0; c < 4; c++) if (e->mask & (1 << c)) sw[j++] = comp[c];
            sp(&g, "%sin %s vary_%s%u;\n", k == K_F ? "" : "flat ", vec_t(k, n), sanitize(e->name, nb), e->index);
            sp(&pro, "    v[%u].%s = %s(vary_%s%u);\n", e->reg, sw, k == K_F ? "" : k == K_I ? "intBitsToFloat" : "uintBitsToFloat", nb, e->index);
        }
        for (int i = 0; i < out->out.n; i++) {
            DxSigElem* e = &out->out.e[i];
            if (!strcmp(e->name, "SV_Target") || !strcmp(e->name, "SV_TARGET")) {
                int k = e->comp_type == 1 ? K_U : e->comp_type == 2 ? K_I : K_F;
                sp(&g, "layout(location = %u) out %s out_%u;\n", e->index, vec_t(k, 4), e->index);
                sp(&epi, "    out_%u = %s(o%u);\n", e->index, k == K_F ? "" : k == K_I ? "floatBitsToInt" : "floatBitsToUint", e->reg);
                if ((int)e->index + 1 > out->ps_outputs) out->ps_outputs = (int)e->index + 1;
            }
        }
        if (tr->ps_depth) sp(&epi, "    gl_FragDepth = odepth;\n");
    }
    sp(&g, "void uew_main() {\n%s}\nvoid main() {\n%s    uew_main();\n%s}\n", tr->body.p ? tr->body.p : "", pro.p ? pro.p : "", epi.p ? epi.p : "");
    out->glsl = g.p;
    free(tr->body.p); free(tr->decl.p); free(icb.p); free(pro.p); free(epi.p);
    free(tr); free(rd);
    return ok;
}

void dxbc_free(DxShader* s)
{
    free(s->glsl);
    s->glsl = NULL;
}
