#include "com.h"

#include <stdio.h>
#include <stdlib.h>

static ComKind* kinds[64];
static int nkinds;
#define MAX_HOSTS (1u << 18)
static void* hosts[MAX_HOSTS];
static uint32_t nhosts = 1;
static uint32_t free_hosts[MAX_HOSTS];
static uint32_t nfree;

#define GEN(n) static void gen_##n(CPU* c) { com_unimpl(c, n); }
#define GEN10(n) GEN(n##0) GEN(n##1) GEN(n##2) GEN(n##3) GEN(n##4) GEN(n##5) GEN(n##6) GEN(n##7) GEN(n##8) GEN(n##9)
GEN(0) GEN(1) GEN(2) GEN(3) GEN(4) GEN(5) GEN(6) GEN(7) GEN(8) GEN(9)
GEN10(1) GEN10(2) GEN10(3) GEN10(4) GEN10(5) GEN10(6) GEN10(7) GEN10(8) GEN10(9) GEN10(10) GEN10(11) GEN10(12) GEN10(13) GEN10(14) GEN10(15)
#define REF(n) gen_##n
#define REF10(n) REF(n##0), REF(n##1), REF(n##2), REF(n##3), REF(n##4), REF(n##5), REF(n##6), REF(n##7), REF(n##8), REF(n##9)
GuestFn com_generic[160] = { REF(0), REF(1), REF(2), REF(3), REF(4), REF(5), REF(6), REF(7), REF(8), REF(9), REF10(1), REF10(2), REF10(3),
    REF10(4), REF10(5), REF10(6), REF10(7), REF10(8), REF10(9), REF10(10), REF10(11), REF10(12), REF10(13), REF10(14), REF10(15) };

void com_unimpl(CPU* c, int slot)
{
    ComKind* k = com_kind(THIS);
    static char seen[64][160];
    int ki = (int)LD32(THIS + 16);
    if (ki >= 0 && ki < 64 && !seen[ki][slot]) {
        seen[ki][slot] = 1;
        host_log("[com] %s slot %d not implemented", k ? k->name : "?", slot);
    }
    RET(0);
}

static void qi(CPU* c)
{
    ComKind* k = com_kind(THIS);
    uint64_t iid = A2, out = A3;
    static const uint8_t unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    int ok = iid_eq(iid, unk);
    for (int i = 0; k && !ok && i < k->niids; i++) ok = iid_eq(iid, k->iids[i]);
    if (!ok) {
        ST64(out, 0);
        RET(0x80004002);
        return;
    }
    ST32(THIS + 8, LD32(THIS + 8) + 1);
    ST64(out, THIS);
    RET(0);
}
static void addref(CPU* c) { uint32_t n = (uint32_t)LD32(THIS + 8) + 1; ST32(THIS + 8, n); RET(n); }
static void release(CPU* c)
{
    uint32_t n = (uint32_t)LD32(THIS + 8);
    if (!n) { RET(0); return; }
    n--;
    ST32(THIS + 8, n);
    ComKind* k = com_kind(THIS);
    if (!n && k && k->on_zero) k->on_zero(THIS);
    RET(n);
}

void com_iunknown(ComKind* k)
{
    k->slots[0] = qi;
    k->slots[1] = addref;
    k->slots[2] = release;
}

void com_finalize(ComKind* k)
{
    if (k->vtbl) return;
    kinds[nkinds++] = k;
    k->vtbl = g_calloc(8ull * (uint64_t)k->nslots);
    for (int i = 0; i < k->nslots; i++) {
        GuestFn f = k->slots[i] ? k->slots[i] : com_generic[i];
        ST64(k->vtbl + 8ull * i, rt_dyn_thunk(f, k->name));
    }
}

uint64_t com_new(ComKind* k, void* host, uint64_t extra)
{
    com_finalize(k);
    uint64_t o = g_calloc(COM_HDR + extra);
    int ki = 0;
    for (; ki < nkinds; ki++) if (kinds[ki] == k) break;
    ST64(o, k->vtbl);
    ST32(o + 8, 1);
    ST32(o + 16, (uint64_t)ki);
    uint32_t h = nfree ? free_hosts[--nfree] : nhosts < MAX_HOSTS ? nhosts++ : 0;
    if (!h) host_log("[com] out of object slots");
    hosts[h] = host;
    ST32(o + 24, h);
    return o;
}

void com_free(uint64_t obj)
{
    uint32_t h = (uint32_t)LD32(obj + 24);
    if (h && h < MAX_HOSTS && hosts[h]) { hosts[h] = NULL; free_hosts[nfree++] = h; }
    g_free(obj);
}

void* com_host(uint64_t obj)
{
    if (!obj) return NULL;
    uint32_t h = (uint32_t)LD32(obj + 24);
    return h < MAX_HOSTS ? hosts[h] : NULL;
}

ComKind* com_kind(uint64_t obj)
{
    if (!obj) return NULL;
    uint32_t i = (uint32_t)LD32(obj + 16);
    return i < (uint32_t)nkinds ? kinds[i] : NULL;
}

int iid_eq(uint64_t guest_iid, const uint8_t* iid) { return memcmp(G2H(guest_iid), iid, 16) == 0; }
