#pragma once
#include "dxbc.h"

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
       T_NULL, T_RAST, T_OMASK, T_UAV = 30, T_TGSM = 31, T_TID = 32, T_GID = 33, T_TIDG = 34, T_COVERAGE = 35, T_TIDGF = 36,
       T_GSINST = 37, T_ODEPTH_GE = 38, T_ODEPTH_LE = 39 };

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

static int popc4(int m) { int n = 0; for (int i = 0; i < 4; i++) n += (m >> i) & 1; return n; }

static int parse_signature(const uint8_t* d, size_t len, int big, DxSig* out)
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

static const uint32_t* dxbc_chunks(const uint8_t* code, size_t size, DxSig* in, DxSig* out, size_t* ntok)
{
    const uint32_t* tokens = NULL;
    if (size < 32 || memcmp(code, "DXBC", 4)) return NULL;
    uint32_t nchunks = *(const uint32_t*)(code + 28);
    for (uint32_t i = 0; i < nchunks; i++) {
        uint32_t off = *(const uint32_t*)(code + 32 + 4 * i);
        if (off + 8 > size) continue;
        const uint8_t* ch = code + off;
        uint32_t len = *(const uint32_t*)(ch + 4);
        const uint8_t* data = ch + 8;
        if (!memcmp(ch, "ISGN", 4)) parse_signature(data, len, 0, in);
        else if (!memcmp(ch, "ISG1", 4)) parse_signature(data, len, 1, in);
        else if (!memcmp(ch, "OSGN", 4)) parse_signature(data, len, 0, out);
        else if (!memcmp(ch, "OSG1", 4)) parse_signature(data, len, 1, out);
        else if (!memcmp(ch, "OSG5", 4)) parse_signature(data, len, 2, out);
        else if (!memcmp(ch, "SHDR", 4) || !memcmp(ch, "SHEX", 4)) { tokens = (const uint32_t*)data; *ntok = len / 4; }
    }
    return tokens;
}
