/* Windows cryptography a game uses for itself: random numbers (CryptGenRandom, BCryptGenRandom, RtlGenRandom), all from
   the browser's cryptographic source (crypto.getRandomValues, through getentropy), and hashing through CryptoAPI
   (CryptCreateHash...) and CNG (BCryptCreateHash...): MD5, SHA-1, SHA-256/384/512 and their HMACs, on BearSSL's
   implementations. Libraries check downloads and pin server keys with these (libcurl's certificate pinning hashes the
   server's public key with SHA-256), and seed their own TLS from the random functions. */
#include "host.h"
#include "bearssl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <unistd.h>
#else
#include <time.h>
#endif

void hle_set_error(uint32_t e);
void wstr_to_utf8(uint64_t p, char* out, size_t cap);

void host_secure_random(void* buf, size_t n)
{
    unsigned char* p = buf;
#ifdef __EMSCRIPTEN__
    while (n) { size_t k = n > 256 ? 256 : n; if (getentropy(p, k)) break; p += k; n -= k; }
#else
    /* native verification builds only */
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)(rand() ^ (clock() >> (i & 7)));
#endif
}
static void random_to_guest(uint64_t p, uint64_t n)
{
    unsigned char tmp[256];
    while (n) {
        size_t k = n > sizeof tmp ? sizeof tmp : (size_t)n;
        host_secure_random(tmp, k);
        memcpy(G2H(p), tmp, k);
        p += k; n -= k;
    }
}
HLE(CryptGenRandom) { if (A3 && A2) random_to_guest(A3, (uint32_t)A2); RET(1); }
HLE(SystemFunction036) { if (A1 && A2) random_to_guest(A1, (uint32_t)A2); RET(1); }   /* RtlGenRandom */
/* (hAlgorithm, pbBuffer, cbBuffer, dwFlags) */
HLE(BCryptGenRandom) { if (A2 && A3) random_to_guest(A2, (uint32_t)A3); RET(0); }
HLE(ProcessPrng) { if (A1 && A2) random_to_guest(A1, A2); RET(1); }

/* ---- hashes ---- */

typedef struct {
    int used, hmac;
    const br_hash_class* vt;
    br_hash_compat_context h;
    br_hmac_key_context hk;
    br_hmac_context hm;
} Hash;
static Hash hashes[256];
#define HASH_MAGIC 0x5EED4A00u

static const br_hash_class* by_algid(uint32_t a)
{
    switch (a) {
    case 0x8003: return &br_md5_vtable;      /* CALG_MD5 */
    case 0x8004: return &br_sha1_vtable;     /* CALG_SHA1 */
    case 0x800C: return &br_sha256_vtable;   /* CALG_SHA_256 */
    case 0x800D: return &br_sha384_vtable;   /* CALG_SHA_384 */
    case 0x800E: return &br_sha512_vtable;   /* CALG_SHA_512 */
    }
    return NULL;
}
static const br_hash_class* by_name(const char* n)
{
    if (!strcmp(n, "MD5")) return &br_md5_vtable;
    if (!strcmp(n, "SHA1")) return &br_sha1_vtable;
    if (!strcmp(n, "SHA256")) return &br_sha256_vtable;
    if (!strcmp(n, "SHA384")) return &br_sha384_vtable;
    if (!strcmp(n, "SHA512")) return &br_sha512_vtable;
    return NULL;
}
static size_t out_size(const br_hash_class* vt) { return (vt->desc >> BR_HASHDESC_OUT_OFF) & BR_HASHDESC_OUT_MASK; }
static size_t block_size(const br_hash_class* vt) { return (size_t)1 << ((vt->desc >> BR_HASHDESC_LBLEN_OFF) & BR_HASHDESC_LBLEN_MASK); }

static int new_hash(const br_hash_class* vt, const void* key, size_t klen, int hmac)
{
    for (int i = 0; i < 256; i++)
        if (!hashes[i].used) {
            Hash* h = &hashes[i];
            memset(h, 0, sizeof *h);
            h->used = 1;
            h->vt = vt;
            h->hmac = hmac;
            if (hmac) { br_hmac_key_init(&h->hk, vt, key, klen); br_hmac_init(&h->hm, &h->hk, 0); }
            else vt->init(&h->h.vtable);
            return i;
        }
    return -1;
}
static Hash* hash_of(uint64_t handle)
{
    uint32_t v = (uint32_t)handle;
    if ((v & 0xFFFFFF00u) != HASH_MAGIC) return NULL;
    Hash* h = &hashes[v & 0xFF];
    return h->used ? h : NULL;
}
static void update(Hash* h, uint64_t p, uint64_t n)
{
    while (n) {
        size_t k = n > 65536 ? 65536 : (size_t)n;
        if (h->hmac) br_hmac_update(&h->hm, G2H(p), k);
        else h->vt->update(&h->h.vtable, G2H(p), k);
        p += k; n -= k;
    }
}
/* the digest so far (the hash itself goes on unchanged) */
static size_t digest(Hash* h, unsigned char* out)
{
    if (h->hmac) return br_hmac_out(&h->hm, out);
    h->vt->out(&h->h.vtable, out);
    return out_size(h->vt);
}

/* CryptoAPI: (hProv, Algid, hKey, dwFlags, phHash); HMAC goes through keys, which aren't here */
HLE(CryptAcquireContextW) { if (A1) STP(A1, 0x7C000001); RET(1); }
HLE(CryptAcquireContextA) { if (A1) STP(A1, 0x7C000001); RET(1); }
HLE(CryptReleaseContext) { RET(1); }
HLE(CryptCreateHash)
{
    const br_hash_class* vt = by_algid((uint32_t)A2);
    int i = vt ? new_hash(vt, NULL, 0, 0) : -1;
    if (i < 0) { hle_set_error(vt ? 8 : 0x80090008); RET(0); return; }   /* NTE_BAD_ALGID */
    if (ARG(5)) STP(ARG(5), HASH_MAGIC | (uint32_t)i);
    RET(1);
}
HLE(CryptHashData) { Hash* h = hash_of(A1); if (!h) { hle_set_error(6); RET(0); return; } if (A2 && A3) update(h, A2, (uint32_t)A3); RET(1); }
/* (hHash, dwParam, pbData, pdwDataLen, dwFlags): HP_ALGID 1, HP_HASHVAL 2, HP_HASHSIZE 4 */
HLE(CryptGetHashParam)
{
    Hash* h = hash_of(A1);
    if (!h || !A4) { hle_set_error(h ? 87 : 6); RET(0); return; }
    unsigned char d[64];
    uint32_t need, cap = LD32(A4);
    if (A2 == 2) need = (uint32_t)digest(h, d);
    else if (A2 == 4 || A2 == 1) need = 4;
    else { hle_set_error(0x80090009); RET(0); return; }   /* NTE_BAD_TYPE */
    ST32(A4, need);
    if (!A3) { RET(1); return; }
    if (cap < need) { hle_set_error(234); RET(0); return; }   /* ERROR_MORE_DATA */
    if (A2 == 2) memcpy(G2H(A3), d, need);
    else ST32(A3, A2 == 4 ? (uint32_t)out_size(h->vt) : 0);
    RET(1);
}
HLE(CryptDestroyHash) { Hash* h = hash_of(A1); if (h) h->used = 0; RET(1); }
HLE(CryptDuplicateHash)
{
    Hash* h = hash_of(A1);
    if (!h || !ARG(4)) { hle_set_error(6); RET(0); return; }
    int i = new_hash(h->vt, NULL, 0, 0);
    if (i < 0) { hle_set_error(8); RET(0); return; }
    hashes[i] = *h;
    STP(ARG(4), HASH_MAGIC | (uint32_t)i);
    RET(1);
}

/* ---- CNG: algorithm handles name a hash (and HMAC with BCRYPT_ALG_HANDLE_HMAC_FLAG), or the RNG ---- */

typedef struct { int used, hmac, rng; const br_hash_class* vt; } Alg;
static Alg algs[64];
#define ALG_MAGIC 0x5EED4B00u
#define STATUS_NOT_SUPPORTED 0xC00000BB
#define STATUS_INVALID_HANDLE 0xC0000008
#define STATUS_BUFFER_TOO_SMALL 0xC0000023
static Alg* alg_of(uint64_t h) { uint32_t v = (uint32_t)h; return (v & 0xFFFFFF00u) == ALG_MAGIC && (v & 0xFF) < 64 && algs[v & 0xFF].used ? &algs[v & 0xFF] : NULL; }

/* (phAlgorithm, pszAlgId, pszImplementation, dwFlags) */
HLE(BCryptOpenAlgorithmProvider)
{
    char n[32] = "";
    if (A2) wstr_to_utf8(A2, n, sizeof n);
    const br_hash_class* vt = by_name(n);
    int rng = !strcmp(n, "RNG");
    if (!vt && !rng) { RET(STATUS_NOT_SUPPORTED); return; }
    for (int i = 0; i < 64; i++)
        if (!algs[i].used) {
            algs[i] = (Alg){ 1, (A4 & 8) != 0, rng, vt };
            if (A1) STP(A1, ALG_MAGIC | (uint32_t)i);
            RET(0);
            return;
        }
    RET(0xC0000017);   /* STATUS_NO_MEMORY */
}
HLE(BCryptCloseAlgorithmProvider) { Alg* a = alg_of(A1); if (a) a->used = 0; RET(0); }
/* (hObject, pszProperty, pbOutput, cbOutput, pcbResult, dwFlags) on algorithms and hashes */
HLE(BCryptGetProperty)
{
    char n[48] = "";
    if (A2) wstr_to_utf8(A2, n, sizeof n);
    Alg* a = alg_of(A1);
    Hash* h = hash_of(A1);
    const br_hash_class* vt = a ? a->vt : h ? h->vt : NULL;
    if (!vt) { RET(a ? STATUS_NOT_SUPPORTED : STATUS_INVALID_HANDLE); return; }
    uint32_t v;
    if (!strcmp(n, "ObjectLength")) v = 0;          /* the hash lives here, so callers' object buffers go unused */
    else if (!strcmp(n, "HashDigestLength")) v = (uint32_t)out_size(vt);
    else if (!strcmp(n, "HashBlockLength")) v = (uint32_t)block_size(vt);
    else { RET(STATUS_NOT_SUPPORTED); return; }
    if (ARG(5)) ST32(ARG(5), 4);
    if (!A3) { RET(0); return; }
    if ((uint32_t)A4 < 4) { RET(STATUS_BUFFER_TOO_SMALL); return; }
    ST32(A3, v);
    RET(0);
}
/* (hAlgorithm, phHash, pbHashObject, cbHashObject, pbSecret, cbSecret, dwFlags) */
HLE(BCryptCreateHash)
{
    Alg* a = alg_of(A1);
    if (!a || !a->vt) { RET(STATUS_INVALID_HANDLE); return; }
    uint64_t key = ARG(5);
    uint32_t klen = (uint32_t)ARG(6);
    int i = new_hash(a->vt, key ? G2H(key) : NULL, klen, a->hmac);
    if (i < 0) { RET(0xC0000017); return; }
    if (A2) STP(A2, HASH_MAGIC | (uint32_t)i);
    RET(0);
}
HLE(BCryptHashData) { Hash* h = hash_of(A1); if (!h) { RET(STATUS_INVALID_HANDLE); return; } if (A2 && A3) update(h, A2, (uint32_t)A3); RET(0); }
/* (hHash, pbOutput, cbOutput, dwFlags): the hash is done after this */
HLE(BCryptFinishHash)
{
    Hash* h = hash_of(A1);
    if (!h) { RET(STATUS_INVALID_HANDLE); return; }
    unsigned char d[64];
    size_t n = digest(h, d);
    if ((uint32_t)A3 != n) { RET(0xC000000D); return; }   /* STATUS_INVALID_PARAMETER: the size must match */
    memcpy(G2H(A2), d, n);
    RET(0);
}
HLE(BCryptDestroyHash) { Hash* h = hash_of(A1); if (h) h->used = 0; RET(0); }
/* (hHash, phNewHash, pbHashObject, cbHashObject, dwFlags) */
HLE(BCryptDuplicateHash)
{
    Hash* h = hash_of(A1);
    if (!h) { RET(STATUS_INVALID_HANDLE); return; }
    int i = new_hash(h->vt, NULL, 0, 0);
    if (i < 0) { RET(0xC0000017); return; }
    hashes[i] = *h;
    if (A2) STP(A2, HASH_MAGIC | (uint32_t)i);
    RET(0);
}
/* one-shot: (hAlgorithm, pbSecret, cbSecret, pbInput, cbInput, pbOutput, cbOutput) */
HLE(BCryptHash)
{
    Alg* a = alg_of(A1);
    if (!a || !a->vt) { RET(STATUS_INVALID_HANDLE); return; }
    int i = new_hash(a->vt, A2 ? G2H(A2) : NULL, (uint32_t)A3, a->hmac);
    if (i < 0) { RET(0xC0000017); return; }
    if (A4 && ARG(5)) update(&hashes[i], A4, (uint32_t)ARG(5));
    unsigned char d[64];
    size_t n = digest(&hashes[i], d);
    hashes[i].used = 0;
    if ((uint32_t)ARG(7) != n) { RET(0xC000000D); return; }
    memcpy(G2H(ARG(6)), d, n);
    RET(0);
}
