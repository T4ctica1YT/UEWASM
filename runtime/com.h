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
extern GuestFn com_generic[160];

#define THIS A1
