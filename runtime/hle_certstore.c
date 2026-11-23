/* crypt32 certificate stores: the system ROOT store holds real root certificates (ca_roots.h, Mozilla's), so a game's
   own TLS (OpenSSL in Unreal's libcurl loads the Windows ROOT store; the EOS SDK) can check the servers it reaches
   through WISP. Other system stores and memory stores start empty. Contexts are kept for the life of the page.

   CERT_CONTEXT: dwCertEncodingType, pbCertEncoded, cbCertEncoded, pCertInfo, hCertStore (pointer-sized fields after
   the first DWORD are aligned: 40 bytes on 64-bit, 20 on 32-bit). */
#include "host.h"
#include "ca_roots.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

void hle_set_error(uint32_t e);

#define STORE_MAGIC 0x5EEDCE57u
#define CRYPT_E_NOT_FOUND 0x80092004u

static uint64_t root_ctx[CA_ROOT_COUNT];

static uint32_t ctx_size(void) { return RT_G32 ? 20 : 40; }
static void ctx_set(uint64_t c, int i, uint64_t v)
{
    static const uint8_t o64[] = { 0, 8, 16, 24, 32 }, o32[] = { 0, 4, 8, 12, 16 };
    uint32_t o = RT_G32 ? o32[i] : o64[i];
    if (i == 0 || i == 2) ST32(c + o, (uint32_t)v);
    else STP(c + o, v);
}

/* a context holding a copy of the DER bytes, with an empty CERT_INFO for callers that peek at it */
static uint64_t new_ctx(const uint8_t* der, uint32_t n, uint64_t store)
{
    uint64_t c = g_calloc(ctx_size()), b = g_calloc(n ? n : 1), info = g_calloc(256);
    memcpy(G2H(b), der, n);
    ctx_set(c, 0, 1);                      /* X509_ASN_ENCODING */
    ctx_set(c, 1, b);
    ctx_set(c, 2, n);
    ctx_set(c, 3, info);
    ctx_set(c, 4, store);
    return c;
}

/* store: magic, kind (1 = ROOT, 0 = empty) */
static uint64_t new_store(int root)
{
    uint64_t s = g_calloc(16);   /* magic, kind, list index */
    ST32(s, STORE_MAGIC);
    ST32(s + 4, (uint32_t)root);
    return s;
}
static int is_root_name(const char* n)
{
    return !strcasecmp(n, "ROOT") || !strcasecmp(n, "AuthRoot") || !strcasecmp(n, "CA");
}
static void wide_name(uint64_t p, char* out, size_t cap)
{
    size_t i = 0;
    for (; p && i + 1 < cap; i++) {
        uint16_t ch = (uint16_t)LD16(p + 2 * i);
        if (!ch) break;
        out[i] = (char)(ch < 128 ? ch : '?');
    }
    out[i] = 0;
}

HLE(CertOpenSystemStoreW) { char n[32]; wide_name(A2, n, sizeof n); RET(new_store(is_root_name(n))); }
HLE(CertOpenSystemStoreA) { RET(new_store(A2 && is_root_name(GSTR(A2)))); }

/* CertOpenStore(provider, encoding, hCryptProv, flags, pvPara): system stores by name; anything else is an empty store
   (memory stores, collections) the game fills itself */
HLE(CertOpenStore)
{
    uint64_t prov = A1, para = ARG(5);
    char n[32] = "";
    if (prov == 10 && para) wide_name(para, n, sizeof n);           /* CERT_STORE_PROV_SYSTEM_W */
    else if (prov == 9 && para) snprintf(n, sizeof n, "%s", GSTR(para));   /* CERT_STORE_PROV_SYSTEM_A */
    RET(new_store(n[0] && is_root_name(n)));
}
HLE(CertCloseStore) { RET(1); }

/* lists: the certificates a server sent, in its order (store kind 2, list index at +8) */
static struct { uint64_t ctx[12]; int n; } lists[64];
static int nlists;

HLE(CertEnumCertificatesInStore)
{
    uint64_t store = A1, prev = A2;
    if (store && LD32(store) == STORE_MAGIC && LD32(store + 4) == 2) {
        uint32_t li = LD32(store + 8);
        int next = 0;
        if (prev) { next = -1; for (int i = 0; li < 64 && i < lists[li].n; i++) if (lists[li].ctx[i] == prev) { next = i + 1; break; } }
        if (li >= 64 || next < 0 || next >= lists[li].n) { hle_set_error(CRYPT_E_NOT_FOUND); RET(0); return; }
        RET(lists[li].ctx[next]);
        return;
    }
    if (!store || LD32(store) != STORE_MAGIC || !LD32(store + 4)) { hle_set_error(CRYPT_E_NOT_FOUND); RET(0); return; }
    int next = 0;
    if (prev) {
        next = -1;
        for (int i = 0; i < CA_ROOT_COUNT; i++) if (root_ctx[i] == prev) { next = i + 1; break; }
        if (next < 0) { hle_set_error(CRYPT_E_NOT_FOUND); RET(0); return; }
    }
    if (next >= CA_ROOT_COUNT) { hle_set_error(CRYPT_E_NOT_FOUND); RET(0); return; }
    if (!root_ctx[next]) root_ctx[next] = new_ctx(ca_roots[next].der, ca_roots[next].size, store);
    RET(root_ctx[next]);
}

/* server certificates from Schannel connections, with what the handshake established about them: whether BearSSL
   found a chain to a root for this server name, and the name. Chain and name queries answer from this, so a caller's
   own validation (libcurl's manual mode) sees the handshake's result; other certificates are reported untrusted. */
static struct { uint64_t ctx; int trusted; char name[256]; } server[64];
static int nserver;
/* the certificates a server sent (leaf first), each a context in one store as Schannel gives them; the leaf's */
uint64_t certstore_server_chain(const uint8_t* const* der, const uint32_t* len, int n, int trusted, const char* name)
{
    uint64_t store = new_store(2);
    int li = nlists < 64 ? nlists++ : 63;
    lists[li].n = 0;
    ST32(store + 8, (uint32_t)li);
    for (int k = 0; k < n && k < 12; k++) lists[li].ctx[lists[li].n++] = new_ctx(der[k], len[k], store);
    uint64_t c = lists[li].ctx[0];
    int i = nserver < 64 ? nserver++ : (int)(c % 64);
    server[i].ctx = c;
    server[i].trusted = trusted;
    snprintf(server[i].name, sizeof server[i].name, "%s", name ? name : "");
    return c;
}
static int server_of(uint64_t ctx) { for (int i = 0; i < nserver; i++) if (server[i].ctx == ctx) return i; return -1; }

/* CertGetNameString(context, type, flags, para, out, cchName): the server name a handshake checked (DNS, simple
   display and attribute types alike); with CERT_NAME_SEARCH_ALL_NAMES_FLAG a double-NUL list. Others: empty */
static uint32_t name_of(uint64_t ctx, uint32_t flags, uint64_t out, uint32_t cap, int wide)
{
    int i = server_of(ctx);
    const char* n = i >= 0 ? server[i].name : "";
    size_t len = strlen(n), need = len + 1 + ((flags & 2) && len ? 1 : 0);
    if (!out || !cap) return (uint32_t)need;
    size_t k = need <= cap ? need : cap;
    for (size_t j = 0; j < k; j++) {
        uint8_t ch = j < len ? (uint8_t)n[j] : 0;
        if (wide) ST16(out + 2 * j, ch); else ST8(out + j, ch);
    }
    if (wide) ST16(out + 2 * (k - 1), 0); else ST8(out + k - 1, 0);
    return (uint32_t)k;
}
HLE(CertGetNameStringW) { RET(name_of(A1, (uint32_t)A3, ARG(5), (uint32_t)ARG(6), 1)); }
HLE(CertGetNameStringA) { RET(name_of(A1, (uint32_t)A3, ARG(5), (uint32_t)ARG(6), 0)); }

/* chains: one element, the certificate itself, with the handshake's verdict (CERT_TRUST_IS_UNTRUSTED_ROOT otherwise).
   CERT_CHAIN_CONTEXT / CERT_SIMPLE_CHAIN / CERT_CHAIN_ELEMENT: cbSize, then the trust status (two DWORDs), then
   pointers at 64-bit or 32-bit alignment */
#define CERT_TRUST_IS_UNTRUSTED_ROOT 0x20
/* CryptQueryObject(type, object, content flags, format flags, flags, encoding out, content type out, format out, store,
   message, context out): a certificate in memory (CRYPT_DATA_BLOB) or a file, DER or PEM, as a certificate context */
static int pem_to_der(const uint8_t* p, size_t n, uint8_t** out, size_t* outn)
{
    const char* b = memchr(p, '-', n) ? strstr((const char*)p, "-----BEGIN CERTIFICATE-----") : NULL;
    if (!b) return 0;
    const char* e = strstr(b + 27, "-----END");
    if (!e) return 0;
    uint8_t* d = malloc((size_t)(e - b));
    size_t m = 0;
    uint32_t acc = 0, bits = 0;
    for (const char* q = b + 27; q < e; q++) {
        int c = *q, v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; d[m++] = (uint8_t)(acc >> bits); }
    }
    *out = d;
    *outn = m;
    return 1;
}
void guest_to_host(const char* guest, char* out, size_t cap);
FILE* host_fopen(const char* path, const char* mode);
HLE(CryptQueryObject)
{
    uint64_t type = A1, obj = A2, ctx_out = ARG(11);
    uint8_t* data = NULL;
    size_t n = 0;
    if (type == 2 && obj) {   /* CERT_QUERY_OBJECT_BLOB */
        n = LD32(obj);
        uint64_t p = LDP(obj + (RT_G32 ? 4 : 8));
        if (p && n) { data = malloc(n); memcpy(data, G2H(p), n); }
    } else if (type == 1 && obj) {   /* CERT_QUERY_OBJECT_FILE: a wide path */
        char g[520], h[1200];
        wide_name(obj, g, sizeof g);
        guest_to_host(g, h, sizeof h);
        FILE* f = host_fopen(h, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long k = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (k > 0 && (data = malloc((size_t)k)) && fread(data, 1, (size_t)k, f) == (size_t)k) n = (size_t)k;
            fclose(f);
        }
    }
    if (data && n && data[0] != 0x30) {
        uint8_t* der;
        size_t dn;
        uint8_t* z = realloc(data, n + 1);
        if (z) { data = z; data[n] = 0; }
        if (pem_to_der(data, n, &der, &dn)) { free(data); data = der; n = dn; }
    }
    if (!data || n < 4 || data[0] != 0x30) { free(data); hle_set_error(CRYPT_E_NOT_FOUND); RET(0); return; }
    if (ARG(6)) ST32(ARG(6), 0x10001);   /* X509_ASN_ENCODING | PKCS_7_ASN_ENCODING */
    if (ARG(7)) ST32(ARG(7), 1);         /* CERT_QUERY_CONTENT_CERT */
    if (ARG(8)) ST32(ARG(8), 1);         /* CERT_QUERY_FORMAT_BINARY */
    if (ARG(9)) STP(ARG(9), 0);
    if (ARG(10)) STP(ARG(10), 0);
    if (ctx_out) STP(ctx_out, new_ctx(data, (uint32_t)n, 0));
    free(data);
    RET(1);
}

HLE(CertCreateCertificateChainEngine) { if (A2) STP(A2, 0x5EEDE7E7); RET(1); }
HLE(CertFreeCertificateChainEngine) { RET(0); }
/* (engine, context, time, extra store, para, flags, reserved, ppChainContext) */
HLE(CertGetCertificateChain)
{
    uint64_t ctx = A2, out = ARG(8);
    if (!ctx || !out) { hle_set_error(0x80070057); RET(0); return; }
    int i = server_of(ctx);
    uint32_t err = i >= 0 && server[i].trusted ? 0 : CERT_TRUST_IS_UNTRUSTED_ROOT;
    int w = RT_G32 ? 4 : 8;
    uint64_t el = g_calloc(64), elv = g_calloc(8), sc = g_calloc(64), scv = g_calloc(8), cc = g_calloc(96);
    ST32(el, RT_G32 ? 32 : 56); STP(el + w, ctx); ST32(el + 2 * w, err);
    STP(elv, el);
    ST32(sc, RT_G32 ? 28 : 48); ST32(sc + 4, err); ST32(sc + 12, 1); STP(sc + 16, elv);
    STP(scv, sc);
    ST32(cc, RT_G32 ? 52 : 88); ST32(cc + 4, err); ST32(cc + 12, 1); STP(cc + 16, scv);
    STP(out, cc);
    RET(1);
}
HLE(CertFreeCertificateChain) { RET(0); }
/* (policy, chain, para, status): CERT_CHAIN_POLICY_STATUS { cbSize, dwError, lChainIndex, lElementIndex, pvExtra } */
HLE(CertVerifyCertificateChainPolicy)
{
    uint64_t chain = A2, st = A4;
    uint32_t err = chain ? (uint32_t)LD32(chain + 4) : CERT_TRUST_IS_UNTRUSTED_ROOT;
    if (st) { ST32(st + 4, err ? 0x800B0109 : 0); ST32(st + 8, err ? 0 : 0xFFFFFFFF); ST32(st + 12, err ? 0 : 0xFFFFFFFF); }   /* CERT_E_UNTRUSTEDROOT */
    RET(1);
}

HLE(CertCreateCertificateContext)
{
    if (!A2 || !A3) { hle_set_error(0x80070057); RET(0); return; }
    RET(new_ctx(G2H(A2), (uint32_t)A3, 0));
}
HLE(CertDuplicateCertificateContext) { RET(A1); }
HLE(CertFreeCertificateContext) { RET(1); }
HLE(CertFindCertificateInStore) { hle_set_error(CRYPT_E_NOT_FOUND); RET(0); }
/* (store, context, disposition, out context): accepted; the out context is the same certificate */
HLE(CertAddCertificateContextToStore) { if (A4) STP(A4, A2); RET(1); }
HLE(CertAddEncodedCertificateToStore)
{
    /* (store, encoding, bytes, size, disposition, out context) */
    uint64_t out = ARG(6);
    if (out) STP(out, A3 && A4 ? new_ctx(G2H(A3), (uint32_t)A4, A1) : 0);
    RET(1);
}
HLE(CertGetCertificateContextProperty) { hle_set_error(CRYPT_E_NOT_FOUND); RET(0); }
