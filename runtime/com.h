#pragma once
#include "host.h"

typedef struct ComKind {
    const char* name;
    int nslots;
    GuestFn slots[160];
    const uint8_t (*iids)[16];
    int niids;
    uint64_t vtbl;
    void (*on_zero)(uint64_t obj);   /* last reference released (the object is still valid until com_free) */
    /* 32-bit guests: stack arguments of each method including `this` (stdcall: the callee pops them); 0 = 1 */
    uint8_t nargs[160];
} ComKind;

/* guest object layout: +0 vtbl, +8 refcount, +16 kind index, +24 host pointer, +32 payload */
#define COM_HDR 32
uint64_t com_new(ComKind* k, void* host, uint64_t extra);
void* com_host(uint64_t obj);
void com_free(uint64_t obj);
ComKind* com_kind(uint64_t obj);
void com_finalize(ComKind* k);
int iid_eq(uint64_t guest_iid, const uint8_t* iid);
void com_unimpl(CPU* c, int slot);
void com_iunknown(ComKind* k);
/* 32-bit guests: stack arguments of each method from slot 0 on, `this` included, one character each (0-9, then a = 10) */
void com_nargs(ComKind* k, const char* n);
/* a float argument (1-based, `this` included): Win64 passes the first four in xmm registers, x86 on the stack */
#define FARG(n) (RT_G32 ? com_fbits(ARG(n)) : c->x[(n) - 1].f[0])
float com_fbits(uint64_t v);
extern GuestFn com_generic[160];

#define THIS A1
