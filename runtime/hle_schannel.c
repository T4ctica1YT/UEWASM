/* Schannel (secur32/sspicli): TLS for the game's own connections, as Windows' security package gives it to libcurl's
   schannel backend, the EOS SDK and HTTP stacks. A BearSSL client (third_party/bearssl) does the work; server
   certificates are checked against the root store (ca_roots.h) and the server name, unless the caller validates
   them itself (SCH_CRED_MANUAL_CRED_VALIDATION), when the result is kept for its own checks.

   Calls follow Schannel's buffer contract: InitializeSecurityContext and DecryptMessage take whole TLS records from
   the caller's buffer, report what is left as SECBUFFER_EXTRA, and answer SEC_E_INCOMPLETE_MESSAGE (taking nothing)
   when not even one record is there yet; tokens to send come back in SECBUFFER_TOKEN, allocated for the caller when it
   asks (ISC_REQ_ALLOCATE_MEMORY, freed with FreeContextBuffer). */
#include "host.h"
#include "ca_roots.h"
#include "bearssl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hle_set_error(uint32_t e);
void wstr_to_utf8(uint64_t p, char* out, size_t cap);

#define SEC_E_OK 0
#define SEC_I_CONTINUE_NEEDED 0x00090312
#define SEC_I_CONTEXT_EXPIRED 0x00090317
#define SEC_E_INCOMPLETE_MESSAGE 0x80090318
#define SEC_E_INVALID_HANDLE 0x80090301
#define SEC_E_UNSUPPORTED_FUNCTION 0x80090302
#define SEC_E_INTERNAL_ERROR 0x80090304
#define SEC_E_SECPKG_NOT_FOUND 0x80090305
#define SEC_E_INVALID_TOKEN 0x80090308
#define SEC_E_MESSAGE_ALTERED 0x8009030F
#define SEC_E_WRONG_PRINCIPAL 0x80090322
#define SEC_E_UNTRUSTED_ROOT 0x80090325
#define SEC_E_CERT_EXPIRED 0x80090328
#define SEC_E_ILLEGAL_MESSAGE 0x80090326
#define SEC_E_BUFFER_TOO_SMALL 0x80090321

enum { SECBUFFER_EMPTY = 0, SECBUFFER_DATA = 1, SECBUFFER_TOKEN = 2, SECBUFFER_MISSING = 4, SECBUFFER_EXTRA = 5,
       SECBUFFER_STREAM_TRAILER = 6, SECBUFFER_STREAM_HEADER = 7, SECBUFFER_ALERT = 17, SECBUFFER_APPLICATION_PROTOCOLS = 18 };
#define ISC_REQ_ALLOCATE_MEMORY 0x100
#define SCH_CRED_NO_SERVERNAME_CHECK 0x4
#define SCH_CRED_MANUAL_CRED_VALIDATION 0x8
#define HEADER_SIZE 13
#define TRAILER_SIZE 64
#define MAX_MESSAGE 16384

static int dbg = -1;
#define TL(...) do { if (dbg < 0) dbg = host_query_flag("tlsdebug"); if (dbg) host_log("[tls] " __VA_ARGS__); } while (0)

/* ---- the root store as BearSSL trust anchors ---- */

/* ?httpdebug=1: HTTPS requests (method, host, path; form fields by name, values only for ones that say what kind of
   request it is) and the status of each answer, with the body of failed ones: what a service answered when a game
   reports a sign-in error. Credentials, tokens and successful answers' bodies are never logged. */
static int httpdbg = -1;
static int http_debug(void) { if (httpdbg < 0) httpdbg = host_query_flag("httpdebug"); return httpdbg; }
static void http_log_request(const char* host, const unsigned char* p, size_t n)
{
    static const char* methods[] = { "GET ", "POST ", "PUT ", "PATCH ", "DELETE ", 0 };
    int m = 0;
    while (methods[m] && (n < strlen(methods[m]) || memcmp(p, methods[m], strlen(methods[m])))) m++;
    if (!methods[m]) return;
    size_t q = strlen(methods[m]);
    while (q < n && q < 300 && p[q] != '\r' && p[q] != '?' && p[q] != ' ') q++;
    char line[1024];
    int o = snprintf(line, sizeof line, "%.*s%s%.*s", (int)strlen(methods[m]), (const char*)p, host, (int)(q - strlen(methods[m])), (const char*)p + strlen(methods[m]));
    const unsigned char* body = NULL;
    for (size_t i = 0; i + 3 < n; i++) if (!memcmp(p + i, "\r\n\r\n", 4)) { body = p + i + 4; break; }
    if (body && body < p + n && *body != '{') {
        static const char* shown[] = { "grant_type", "external_auth_type", "token_type", "scope", 0 };
        const unsigned char* b = body, *end = p + n;
        while (b < end && o < (int)sizeof line - 80) {
            const unsigned char* eq = b, *amp;
            while (eq < end && *eq != '=' && *eq != '&') eq++;
            amp = eq;
            while (amp < end && *amp != '&') amp++;
            int show = 0;
            for (int k = 0; shown[k]; k++) if ((size_t)(eq - b) == strlen(shown[k]) && !memcmp(b, shown[k], eq - b)) show = 1;
            size_t vl = eq < amp ? (size_t)(amp - eq - 1) : 0;
            if (show && vl < 64) o += snprintf(line + o, sizeof line - o, " %.*s", (int)(amp - b), (const char*)b);
            else o += snprintf(line + o, sizeof line - o, " %.*s=(%u chars)", (int)(eq - b) < 40 ? (int)(eq - b) : 40, (const char*)b, (unsigned)vl);
            b = amp < end ? amp + 1 : end;
        }
    }
    host_log("[http] %s", line);
}
static void http_log_response(const char* host, const unsigned char* p, size_t n)
{
    if (n < 12 || memcmp(p, "HTTP/1.", 7)) return;
    int status = atoi((const char*)p + 9);
    size_t e = 0;
    while (e < n && e < 120 && p[e] != '\r') e++;
    const unsigned char* body = NULL;
    for (size_t i = 0; i + 3 < n; i++) if (!memcmp(p + i, "\r\n\r\n", 4)) { body = p + i + 4; break; }
    if ((status >= 200 && status < 300) || !body) { host_log("[http] %s: %.*s", host, (int)e, (const char*)p); return; }
    size_t bl = (size_t)(p + n - body);
    host_log("[http] %s: %.*s: %.*s", host, (int)e, (const char*)p, (int)(bl < 800 ? bl : 800), (const char*)body);
}

static br_x509_trust_anchor* anchors;
static size_t nanchors;

typedef struct { unsigned char* p; size_t n, cap; } Bytes;
static void bytes_add(void* ctx, const void* buf, size_t len)
{
    Bytes* b = ctx;
    if (b->n + len > b->cap) { b->cap = (b->n + len) * 2 + 64; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, buf, len);
    b->n += len;
}
static void* dup_mem(const void* p, size_t n) { void* q = malloc(n ? n : 1); memcpy(q, p, n); return q; }
static int copy_pkey(br_x509_pkey* dst, const br_x509_pkey* src)
{
    dst->key_type = src->key_type;
    if (src->key_type == BR_KEYTYPE_RSA) {
        dst->key.rsa.n = dup_mem(src->key.rsa.n, src->key.rsa.nlen); dst->key.rsa.nlen = src->key.rsa.nlen;
        dst->key.rsa.e = dup_mem(src->key.rsa.e, src->key.rsa.elen); dst->key.rsa.elen = src->key.rsa.elen;
        return 1;
    }
    if (src->key_type == BR_KEYTYPE_EC) {
        dst->key.ec.curve = src->key.ec.curve;
        dst->key.ec.q = dup_mem(src->key.ec.q, src->key.ec.qlen); dst->key.ec.qlen = src->key.ec.qlen;
        return 1;
    }
    return 0;
}
static void load_anchors(void)
{
    if (anchors) return;
    anchors = calloc(CA_ROOT_COUNT, sizeof *anchors);
    for (int i = 0; i < CA_ROOT_COUNT; i++) {
        br_x509_decoder_context dc;
        Bytes dn = { 0 };
        br_x509_decoder_init(&dc, bytes_add, &dn);
        br_x509_decoder_push(&dc, ca_roots[i].der, ca_roots[i].size);
        const br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);
        if (!pk || !copy_pkey(&anchors[nanchors].pkey, pk)) { free(dn.p); continue; }
        anchors[nanchors].dn.data = dn.p;
        anchors[nanchors].dn.len = dn.n;
        anchors[nanchors].flags = BR_X509_TA_CA;
        nanchors++;
    }
}

/* ---- credentials and contexts ---- */

typedef struct { int used; uint32_t flags; } Cred;
static Cred creds[64];

typedef struct Ctx Ctx;
/* wraps BearSSL's chain validation: keeps the server's certificate, and in manual mode lets the handshake go on
   whatever the result, so the caller's own checks decide */
typedef struct {
    const br_x509_class* vtable;
    br_x509_minimal_context minimal;
    Ctx* owner;
    int depth;
} Wrap;

struct Ctx {
    int used, done, closing, closed, manual, noname;
    br_ssl_client_context sc;
    Wrap x;
    unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    char host[256];
    Bytes leaf;                  /* the server's certificate */
    Bytes chain[12];             /* every certificate it sent, leaf first */
    int nchain;
    unsigned verify_err;         /* BearSSL's verdict on the chain */
    br_x509_pkey leaf_key;       /* the leaf's key, for a manual-mode handshake that failed validation */
    int have_leaf_key;
    const char* alpn[8];
    char alpn_store[8][32];
    int nalpn;
    uint64_t cert_ctx;           /* the CERT_CONTEXT handed out for the leaf */
};
static Ctx* ctxs[64];

static void w_start_chain(const br_x509_class** c, const char* name)
{
    Wrap* w = (Wrap*)c;
    w->depth = 0;
    w->owner->leaf.n = 0;
    for (int i = 0; i < w->owner->nchain; i++) w->owner->chain[i].n = 0;
    w->owner->nchain = 0;
    w->minimal.vtable->start_chain(&w->minimal.vtable, name);
}
static void w_start_cert(const br_x509_class** c, uint32_t len)
{
    Wrap* w = (Wrap*)c;
    w->depth++;
    if (w->owner->nchain < 12) w->owner->nchain++;
    w->minimal.vtable->start_cert(&w->minimal.vtable, len);
}
static void w_append(const br_x509_class** c, const unsigned char* buf, size_t len)
{
    Wrap* w = (Wrap*)c;
    if (w->depth == 1) bytes_add(&w->owner->leaf, buf, len);
    if (w->depth <= 12) bytes_add(&w->owner->chain[w->depth - 1], buf, len);
    w->minimal.vtable->append(&w->minimal.vtable, buf, len);
}
static void w_end_cert(const br_x509_class** c) { Wrap* w = (Wrap*)c; w->minimal.vtable->end_cert(&w->minimal.vtable); }
static unsigned w_end_chain(const br_x509_class** c)
{
    Wrap* w = (Wrap*)c;
    Ctx* k = w->owner;
    unsigned err = w->minimal.vtable->end_chain(&w->minimal.vtable);
    k->verify_err = err;
    TL("%s: certificate chain %s (%u)", k->host, err ? "not trusted" : "trusted", err);
    if (err && (k->manual || (k->noname && err == BR_ERR_X509_BAD_SERVER_NAME)) && k->leaf.n) {
        br_x509_decoder_context dc;
        br_x509_decoder_init(&dc, NULL, NULL);
        br_x509_decoder_push(&dc, k->leaf.p, k->leaf.n);
        const br_x509_pkey* pk = br_x509_decoder_get_pkey(&dc);
        if (pk && copy_pkey(&k->leaf_key, pk)) { k->have_leaf_key = 1; return 0; }
    }
    return err;
}
static const br_x509_pkey* w_get_pkey(const br_x509_class* const* c, unsigned* usages)
{
    Wrap* w = (Wrap*)c;
    if (w->owner->verify_err && w->owner->have_leaf_key) {
        if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
        return &w->owner->leaf_key;
    }
    return w->minimal.vtable->get_pkey(&w->minimal.vtable, usages);
}
static const br_x509_class wrap_vtable = { sizeof(Wrap), w_start_chain, w_start_cert, w_append, w_end_cert, w_end_chain, w_get_pkey };

void host_secure_random(void* buf, size_t n);
static void entropy(unsigned char* p, size_t n) { host_secure_random(p, n); }

/* SecHandle: dwLower, dwUpper (pointer-sized) */
static void put_handle(uint64_t h, uint32_t kind, int i) { if (h) { STP(h, (uint64_t)i + 1); STP(h + GP, kind); } }
static int get_handle(uint64_t h, uint32_t kind) { if (!h || LDP(h + GP) != kind) return -1; uint64_t i = LDP(h); return i >= 1 && i <= 64 ? (int)i - 1 : -1; }
#define K_CRED 0x5EEDC4ED
#define K_CTX 0x5EEDC7C7
static Ctx* ctx_of(uint64_t h) { int i = get_handle(h, K_CTX); return i >= 0 ? ctxs[i] : NULL; }

/* SecBufferDesc: ulVersion, cBuffers, pBuffers (+8); SecBuffer: cbBuffer, BufferType, pvBuffer (+8); 16 or 12 bytes */
static uint32_t sb_size(void) { return RT_G32 ? 12 : 16; }
static uint32_t desc_count(uint64_t d) { return d ? (uint32_t)LD32(d + 4) : 0; }
static uint64_t desc_buf(uint64_t d, uint32_t i) { return LDP(d + 8) + (uint64_t)sb_size() * i; }
static uint64_t find_buf(uint64_t d, uint32_t type)
{
    for (uint32_t i = 0; i < desc_count(d); i++) if ((LD32(desc_buf(d, i) + 4) & 0x0FFFFFFF) == type) return desc_buf(d, i);
    return 0;
}
static void set_buf(uint64_t b, uint32_t type, uint64_t pv, uint32_t cb) { ST32(b, cb); ST32(b + 4, type); STP(b + 8, pv); }

/* everything the engine has to send, appended to out */
static void drain(Ctx* k, Bytes* out)
{
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&k->sc.eng);
        if (!(st & BR_SSL_SENDREC)) return;
        size_t len;
        unsigned char* p = br_ssl_engine_sendrec_buf(&k->sc.eng, &len);
        bytes_add(out, p, len);
        br_ssl_engine_sendrec_ack(&k->sc.eng, len);
    }
}
/* one whole TLS record into the engine; 0 when the engine closed */
static int feed(Ctx* k, const unsigned char* p, size_t n)
{
    while (n) {
        unsigned st = br_ssl_engine_current_state(&k->sc.eng);
        if (st & BR_SSL_CLOSED) return 0;
        if (!(st & BR_SSL_RECVREC)) return 1;
        size_t len;
        unsigned char* q = br_ssl_engine_recvrec_buf(&k->sc.eng, &len);
        if (len > n) len = n;
        memcpy(q, p, len);
        br_ssl_engine_recvrec_ack(&k->sc.eng, len);
        p += len; n -= len;
    }
    return !(br_ssl_engine_current_state(&k->sc.eng) & BR_SSL_CLOSED);
}
/* the length of the first whole record in p (0 when there isn't one yet) */
static size_t record_len(const unsigned char* p, size_t n)
{
    if (n < 5) return 0;
    size_t len = 5 + ((size_t)p[3] << 8 | p[4]);
    return len <= n ? len : 0;
}
static uint32_t bearssl_status(Ctx* k)
{
    int e = br_ssl_engine_last_error(&k->sc.eng);
    TL("%s: closed, BearSSL error %d", k->host, e);
    if (e == BR_ERR_OK) return SEC_I_CONTEXT_EXPIRED;
    if (e == BR_ERR_X509_BAD_SERVER_NAME) return SEC_E_WRONG_PRINCIPAL;
    if (e == BR_ERR_X509_EXPIRED) return SEC_E_CERT_EXPIRED;
    if (e >= BR_ERR_X509_OK && e < BR_ERR_X509_OK + 64) return SEC_E_UNTRUSTED_ROOT;
    return SEC_E_ILLEGAL_MESSAGE;
}

/* the output token: in the caller's buffer, or allocated for it */
static uint32_t put_token(uint64_t out, uint32_t req, const Bytes* b)
{
    /* callers name it SECBUFFER_TOKEN or leave it SECBUFFER_EMPTY for Schannel to fill (libcurl does) */
    uint64_t tb = find_buf(out, SECBUFFER_TOKEN);
    if (!tb) tb = find_buf(out, SECBUFFER_EMPTY);
    if (!tb) return b->n ? SEC_E_INTERNAL_ERROR : SEC_E_OK;
    if (req & ISC_REQ_ALLOCATE_MEMORY) {
        uint64_t p = b->n ? g_alloc(b->n) : 0;
        if (b->n) memcpy(G2H(p), b->p, b->n);
        set_buf(tb, SECBUFFER_TOKEN, p, (uint32_t)b->n);
    } else {
        if (b->n > LD32(tb)) return SEC_E_BUFFER_TOO_SMALL;
        if (b->n) memcpy(G2H(LDP(tb + 8)), b->p, b->n);
        ST32(tb, (uint32_t)b->n);
        ST32(tb + 4, SECBUFFER_TOKEN);
    }
    return SEC_E_OK;
}

static uint32_t isc(uint64_t cred, uint64_t ctxh, const char* target, uint32_t req, uint64_t in, uint64_t newctx, uint64_t out, uint64_t attrs)
{
    Ctx* k = ctx_of(ctxh);
    Bytes o = { 0 };
    uint32_t r;
    /* every attribute asked for is given (sequence and replay detection, confidentiality, stream mode, allocated
       memory...); callers such as libcurl check that the returned set matches the requested one */
    if (attrs) ST32(attrs, req);
    if (!k) {
        int ci = get_handle(cred, K_CRED);
        if (ci < 0) return SEC_E_INVALID_HANDLE;
        int i = 0;
        while (i < 64 && ctxs[i]) i++;
        if (i == 64) return SEC_E_INTERNAL_ERROR;
        k = ctxs[i] = calloc(1, sizeof *k);
        k->used = 1;
        k->manual = (creds[ci].flags & SCH_CRED_MANUAL_CRED_VALIDATION) != 0;
        snprintf(k->host, sizeof k->host, "%s", target ? target : "");
        load_anchors();
        br_ssl_client_init_full(&k->sc, &k->x.minimal, anchors, nanchors);
        k->x.vtable = &wrap_vtable;
        k->x.owner = k;
        br_ssl_engine_set_x509(&k->sc.eng, &k->x.vtable);
        br_ssl_engine_set_buffer(&k->sc.eng, k->iobuf, sizeof k->iobuf, 1);
        unsigned char seed[32];
        entropy(seed, sizeof seed);
        br_ssl_engine_inject_entropy(&k->sc.eng, seed, sizeof seed);
        /* application protocols (ALPN) the caller offered */
        uint64_t ap = find_buf(in, SECBUFFER_APPLICATION_PROTOCOLS);
        if (ap && LD32(ap) >= 10) {
            /* SEC_APPLICATION_PROTOCOLS: ProtocolListsSize u32, then lists: ext u32, size u16, then u8-prefixed names */
            uint64_t p = LDP(ap + 8);
            uint32_t ext = LD32(p + 4), size = LD16(p + 8);
            for (uint32_t o2 = 0; ext == 2 && o2 < size && k->nalpn < 8;) {
                uint32_t n = LD8(p + 10 + o2);
                if (n >= sizeof k->alpn_store[0] || o2 + 1 + n > size) break;
                memcpy(k->alpn_store[k->nalpn], G2H(p + 11 + o2), n);
                k->alpn_store[k->nalpn][n] = 0;
                if (http_debug() && !strcmp(k->alpn_store[k->nalpn], "h2")) { o2 += 1 + n; continue; }   /* readable HTTP/1.1 */
                k->alpn[k->nalpn] = k->alpn_store[k->nalpn];
                k->nalpn++;
                o2 += 1 + n;
            }
            if (k->nalpn) br_ssl_engine_set_protocol_names(&k->sc.eng, k->alpn, (size_t)k->nalpn);
        }
        /* the name always goes out (SNI: servers behind CDNs need it); without the check a mismatch is only recorded */
        k->noname = (creds[ci].flags & SCH_CRED_NO_SERVERNAME_CHECK) != 0;
        if (!br_ssl_client_reset(&k->sc, k->host[0] ? k->host : NULL, 0)) return SEC_E_INTERNAL_ERROR;
        put_handle(newctx, K_CTX, i);
        TL("%s: handshake starts", k->host);
        drain(k, &o);
        r = put_token(out, req, &o);
        free(o.p);
        return r ? r : SEC_I_CONTINUE_NEEDED;
    }
    if (newctx && newctx != ctxh) { STP(newctx, LDP(ctxh)); STP(newctx + GP, LDP(ctxh + GP)); }
    if (k->closing) {
        /* after ApplyControlToken(SCHANNEL_SHUTDOWN): the close_notify to send */
        br_ssl_engine_close(&k->sc.eng);
        drain(k, &o);
        r = put_token(out, req, &o);
        free(o.p);
        return r ? r : SEC_E_OK;
    }
    uint64_t tb = find_buf(in, SECBUFFER_TOKEN);
    const unsigned char* p = tb ? G2H(LDP(tb + 8)) : NULL;
    size_t n = tb ? LD32(tb) : 0, used = 0, rl;
    if (!record_len(p, n)) return SEC_E_INCOMPLETE_MESSAGE;
    while (!k->done && (rl = record_len(p + used, n - used))) {
        if (!feed(k, p + used, rl)) { free(o.p); return bearssl_status(k); }
        used += rl;
        drain(k, &o);
        unsigned st = br_ssl_engine_current_state(&k->sc.eng);
        if (st & BR_SSL_SENDAPP) k->done = 1;
    }
    /* what wasn't taken goes back as SECBUFFER_EXTRA (the second input buffer, as Schannel reports it) */
    if (desc_count(in) >= 2) {
        uint64_t eb = desc_buf(in, 1);
        if (used < n) set_buf(eb, SECBUFFER_EXTRA, 0, (uint32_t)(n - used));
        else set_buf(eb, SECBUFFER_EMPTY, 0, 0);
    }
    r = put_token(out, req, &o);
    free(o.p);
    if (r) return r;
    if (k->done) TL("%s: handshake done (TLS %x%s%s)", k->host, br_ssl_engine_get_version(&k->sc.eng),
                    br_ssl_engine_get_selected_protocol(&k->sc.eng) ? ", " : "",
                    br_ssl_engine_get_selected_protocol(&k->sc.eng) ? br_ssl_engine_get_selected_protocol(&k->sc.eng) : "");
    return k->done ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
}

/* (phCredential, phContext, pszTargetName, fContextReq, Reserved1, TargetDataRep, pInput, Reserved2, phNewContext,
    pOutput, pfContextAttr, ptsExpiry) */
HLE(InitializeSecurityContextW)
{
    char t[256] = "";
    if (A3) wstr_to_utf8(A3, t, sizeof t);
    RET(isc(A1, A2, t, (uint32_t)A4, ARG(7), ARG(9), ARG(10), ARG(11)));
}
HLE(InitializeSecurityContextA) { RET(isc(A1, A2, A3 ? GSTR(A3) : "", (uint32_t)A4, ARG(7), ARG(9), ARG(10), ARG(11))); }

/* (pszPrincipal, pszPackage, fCredentialUse, pvLogonId, pAuthData, pGetKeyFn, pvGetKeyArgument, phCredential, ptsExpiry) */
static uint32_t acquire(uint64_t auth, uint64_t phcred)
{
    int i = 0;
    while (i < 64 && creds[i].used) i++;
    if (i == 64) return SEC_E_INTERNAL_ERROR;
    creds[i].used = 1;
    /* SCHANNEL_CRED (version 4) and SCH_CREDENTIALS (version 5) both keep dwFlags late in the structure */
    creds[i].flags = 0;
    if (auth) {
        uint32_t ver = LD32(auth);
        if (ver == 4) creds[i].flags = LD32(auth + (RT_G32 ? 48 : 72));          /* SCHANNEL_CRED.dwFlags */
        else if (ver == 5) creds[i].flags = LD32(auth + (RT_G32 ? 32 : 52));     /* SCH_CREDENTIALS.dwFlags */
    }
    put_handle(phcred, K_CRED, i);
    TL("credentials %d (flags %x)", i, creds[i].flags);
    return SEC_E_OK;
}
HLE(AcquireCredentialsHandleW) { RET(acquire(ARG(5), ARG(8))); }
HLE(AcquireCredentialsHandleA) { RET(acquire(ARG(5), ARG(8))); }
HLE(FreeCredentialsHandle) { int i = get_handle(A1, K_CRED); if (i >= 0) creds[i].used = 0; RET(i >= 0 ? SEC_E_OK : SEC_E_INVALID_HANDLE); }

static void free_key(br_x509_pkey* k)
{
    if (k->key_type == BR_KEYTYPE_RSA) { free(k->key.rsa.n); free(k->key.rsa.e); }
    else if (k->key_type == BR_KEYTYPE_EC) free(k->key.ec.q);
}
HLE(DeleteSecurityContext)
{
    int i = get_handle(A1, K_CTX);
    if (i < 0 || !ctxs[i]) { RET(SEC_E_INVALID_HANDLE); return; }
    free(ctxs[i]->leaf.p);
    for (int j = 0; j < 12; j++) free(ctxs[i]->chain[j].p);
    if (ctxs[i]->have_leaf_key) free_key(&ctxs[i]->leaf_key);
    free(ctxs[i]);
    ctxs[i] = NULL;
    RET(SEC_E_OK);
}
/* (phContext, pInput): SCHANNEL_SHUTDOWN in a token buffer starts the close */
HLE(ApplyControlToken)
{
    Ctx* k = ctx_of(A1);
    if (!k) { RET(SEC_E_INVALID_HANDLE); return; }
    uint64_t tb = find_buf(A2, SECBUFFER_TOKEN);
    if (tb && LD32(tb) >= 4 && LD32(LDP(tb + 8)) == 1) k->closing = 1;
    RET(SEC_E_OK);
}
HLE(CompleteAuthToken) { RET(SEC_E_UNSUPPORTED_FUNCTION); }

/* (phContext, fQOP, pMessage, MessageSeqNo): header, data, trailer buffers, contiguous as the caller lays them out */
HLE(EncryptMessage)
{
    Ctx* k = ctx_of(A1);
    if (!k || !k->done) { RET(SEC_E_INVALID_HANDLE); return; }
    uint64_t hb = find_buf(A3, SECBUFFER_STREAM_HEADER), db = find_buf(A3, SECBUFFER_DATA), tb = find_buf(A3, SECBUFFER_STREAM_TRAILER);
    if (!hb || !db || !tb) { RET(SEC_E_INVALID_TOKEN); return; }
    const unsigned char* p = G2H(LDP(db + 8));
    size_t n = LD32(db), sent = 0;
    Bytes o = { 0 };
    if (http_debug()) http_log_request(k->host, p, n);
    while (sent < n) {
        unsigned st = br_ssl_engine_current_state(&k->sc.eng);
        if (st & BR_SSL_CLOSED) { free(o.p); RET(bearssl_status(k)); return; }
        if (st & BR_SSL_SENDREC) { drain(k, &o); continue; }
        size_t len;
        unsigned char* q = br_ssl_engine_sendapp_buf(&k->sc.eng, &len);
        if (!q) { drain(k, &o); continue; }
        if (len > n - sent) len = n - sent;
        memcpy(q, p + sent, len);
        br_ssl_engine_sendapp_ack(&k->sc.eng, len);
        sent += len;
    }
    br_ssl_engine_flush(&k->sc.eng, 0);
    drain(k, &o);
    uint32_t hcap = LD32(hb), dcap = LD32(db), tcap = LD32(tb);
    if (o.n > (size_t)hcap + dcap + tcap) { free(o.p); RET(SEC_E_BUFFER_TOO_SMALL); return; }
    memcpy(G2H(LDP(hb + 8)), o.p, o.n);
    uint32_t h = (uint32_t)(o.n < hcap ? o.n : hcap), d = (uint32_t)(o.n - h < dcap ? o.n - h : dcap);
    ST32(hb, h); ST32(db, d); ST32(tb, (uint32_t)(o.n - h - d));
    free(o.p);
    RET(SEC_E_OK);
}

/* (phContext, pMessage, MessageSeqNo, pfQOP): one record from the data buffer, decrypted in place */
HLE(DecryptMessage)
{
    Ctx* k = ctx_of(A1);
    if (!k) { RET(SEC_E_INVALID_HANDLE); return; }
    uint64_t d = A2, db = find_buf(d, SECBUFFER_DATA);
    if (!db || desc_count(d) < 4) { RET(SEC_E_INVALID_TOKEN); return; }
    uint64_t base = LDP(db + 8);
    unsigned char* p = G2H(base);
    size_t n = LD32(db), rl = record_len(p, n);
    if (!rl) {
        uint64_t mb = desc_buf(d, 1);
        set_buf(mb, SECBUFFER_MISSING, 0, (uint32_t)(n < 5 ? 5 - n : 5 + ((size_t)p[3] << 8 | p[4]) - n));
        RET(SEC_E_INCOMPLETE_MESSAGE);
        return;
    }
    if (!feed(k, p, rl)) {
        int e = br_ssl_engine_last_error(&k->sc.eng);
        for (uint32_t i = 0; i < 4; i++) set_buf(desc_buf(d, i), SECBUFFER_EMPTY, 0, 0);
        RET(e == BR_ERR_OK ? SEC_I_CONTEXT_EXPIRED : SEC_E_MESSAGE_ALTERED);   /* close_notify: the peer ended it */
        return;
    }
    size_t got = 0;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&k->sc.eng);
        if (!(st & BR_SSL_RECVAPP)) break;
        size_t len;
        unsigned char* q = br_ssl_engine_recvapp_buf(&k->sc.eng, &len);
        if (got + len > rl) len = rl - got;
        memmove(p + 5 + got, q, len);
        br_ssl_engine_recvapp_ack(&k->sc.eng, len);
        got += len;
        if (got >= rl - 5) break;
    }
    /* anything the record made the engine want to send (a key update, an alert) is dropped: Schannel would hand
       it back for the caller to renegotiate, which these callers never do */
    if (br_ssl_engine_current_state(&k->sc.eng) & BR_SSL_SENDREC) { Bytes o = { 0 }; drain(k, &o); free(o.p); }
    set_buf(desc_buf(d, 0), SECBUFFER_STREAM_HEADER, base, 5);
    set_buf(desc_buf(d, 1), SECBUFFER_DATA, base + 5, (uint32_t)got);
    set_buf(desc_buf(d, 2), SECBUFFER_STREAM_TRAILER, base + 5 + got, (uint32_t)(rl - 5 - got));
    if (rl < n) set_buf(desc_buf(d, 3), SECBUFFER_EXTRA, base + rl, (uint32_t)(n - rl));
    else set_buf(desc_buf(d, 3), SECBUFFER_EMPTY, 0, 0);
    if (A4) ST32(A4, 0);
    if (http_debug()) http_log_response(k->host, p + 5, got);
    RET(SEC_E_OK);
}

/* the leaf as a CERT_CONTEXT (crypt32's layout), made once per context */
uint64_t certstore_server_chain(const uint8_t* const* der, const uint32_t* len, int n, int trusted, const char* name);
static uint32_t query(uint64_t h, uint32_t attr, uint64_t buf)
{
    Ctx* k = ctx_of(h);
    if (!k) return SEC_E_INVALID_HANDLE;
    TL("%s: QueryContextAttributes %u", k->host, attr);
    switch (attr) {
    case 4:   /* SECPKG_ATTR_STREAM_SIZES */
        ST32(buf, HEADER_SIZE); ST32(buf + 4, TRAILER_SIZE); ST32(buf + 8, MAX_MESSAGE); ST32(buf + 12, 4); ST32(buf + 16, 16);
        return SEC_E_OK;
    case 0x53:   /* SECPKG_ATTR_REMOTE_CERT_CONTEXT */
        if (!k->leaf.n) return SEC_E_INTERNAL_ERROR;
        /* trusted when BearSSL checked the chain against the roots and the server name (it did, in auto mode or not) */
        if (!k->cert_ctx) {
            const uint8_t* ders[12];
            uint32_t lens[12];
            for (int i = 0; i < k->nchain; i++) { ders[i] = k->chain[i].p; lens[i] = (uint32_t)k->chain[i].n; }
            k->cert_ctx = certstore_server_chain(ders, lens, k->nchain, k->verify_err == 0, k->host);
        }
        STP(buf, k->cert_ctx);
        return SEC_E_OK;
    case 0x5a:   /* SECPKG_ATTR_CONNECTION_INFO: TLS 1.2 client, AES, SHA-256, ECDHE */
        ST32(buf, br_ssl_engine_get_version(&k->sc.eng) >= BR_TLS12 ? 0x800 : 0x200);
        ST32(buf + 4, 0x6610); ST32(buf + 8, 128); ST32(buf + 12, 0x800C); ST32(buf + 16, 256); ST32(buf + 20, 0xAE06); ST32(buf + 24, 256);
        return SEC_E_OK;
    case 35: {   /* SECPKG_ATTR_APPLICATION_PROTOCOL */
        const char* sel = br_ssl_engine_get_selected_protocol(&k->sc.eng);
        memset(G2H(buf), 0, 264);
        ST32(buf, sel ? 1 : 2);   /* SecApplicationProtocolNegotiationStatus_Success / _None */
        ST32(buf + 4, 2);         /* ALPN */
        if (sel) { size_t l = strlen(sel); ST8(buf + 8, (uint8_t)l); memcpy(G2H(buf + 9), sel, l); }
        return SEC_E_OK;
    }
    }
    TL("QueryContextAttributes %u: not supported", attr);
    return SEC_E_UNSUPPORTED_FUNCTION;
}
HLE(QueryContextAttributesW) { RET(query(A1, (uint32_t)A2, A3)); }
HLE(QueryContextAttributesA) { RET(query(A1, (uint32_t)A2, A3)); }

/* SecPkgInfo for "Microsoft Unified Security Protocol Provider": capabilities, version, RPC id, max token, name, comment */
static uint64_t pkg_info(int wide)
{
    static uint64_t p[2];
    if (p[wide]) return p[wide];
    uint32_t sz = RT_G32 ? 20 : 32;
    uint64_t s = g_calloc(sz + 256);
    ST32(s, 0x107B3); ST16(s + 4, 1); ST16(s + 6, 14); ST32(s + 8, 24576);
    const char* nm = "Microsoft Unified Security Protocol Provider";
    uint64_t name = s + sz;
    if (wide) for (size_t i = 0; i <= strlen(nm); i++) ST16(name + 2 * i, (uint8_t)nm[i]);
    else memcpy(G2H(name), nm, strlen(nm) + 1);
    STP(s + (RT_G32 ? 12 : 16), name);
    STP(s + (RT_G32 ? 16 : 24), name);
    return p[wide] = s;
}
HLE(QuerySecurityPackageInfoW) { if (A2) STP(A2, pkg_info(1)); RET(SEC_E_OK); }
HLE(QuerySecurityPackageInfoA) { if (A2) STP(A2, pkg_info(0)); RET(SEC_E_OK); }
/* one package: Schannel */
static void enum_packages(CPU* c, int wide) { if (A1) ST32(A1, 1); if (A2) STP(A2, pkg_info(wide)); RET(SEC_E_OK); }
HLE(EnumerateSecurityPackagesW) { enum_packages(c, 1); }
HLE(EnumerateSecurityPackagesA) { enum_packages(c, 0); }

/* ---- the SecurityFunctionTable libcurl and HTTP stacks take from InitSecurityInterface ---- */

void hle_FreeContextBuffer(CPU* c);
static void unsupported(CPU* c) { RET(SEC_E_UNSUPPORTED_FUNCTION); }
static void enum_w(CPU* c) { enum_packages(c, 1); }
static void enum_a(CPU* c) { enum_packages(c, 0); }
static uint64_t table(int wide)
{
    static uint64_t t[2];
    static const int8_t nargs[] = { 2, 3, 9, 1, 0, 12, 9, 2, 1, 2, 3, 1, 1, 4, 4, 1, 2, 0, 0, 4, 4, 8, 0, 2, 4, 4, 4, 4, 8, 4, 4 };
    if (t[wide]) return t[wide];
    int n = (int)sizeof nargs;
    uint64_t tb = t[wide] = g_calloc((uint64_t)GP * (n + 1));
    ST32(tb, 4);   /* SECURITY_SUPPORT_PROVIDER_INTERFACE_VERSION_4 */
    for (int i = 0; i < n; i++) {
        GuestFn f = unsupported;
        switch (i) {
        case 0: f = wide ? enum_w : enum_a; break;
        case 2: f = wide ? hle_AcquireCredentialsHandleW : hle_AcquireCredentialsHandleA; break;
        case 3: f = hle_FreeCredentialsHandle; break;
        case 5: f = wide ? hle_InitializeSecurityContextW : hle_InitializeSecurityContextA; break;
        case 7: f = hle_CompleteAuthToken; break;
        case 8: f = hle_DeleteSecurityContext; break;
        case 9: f = hle_ApplyControlToken; break;
        case 10: f = wide ? hle_QueryContextAttributesW : hle_QueryContextAttributesA; break;
        case 15: f = hle_FreeContextBuffer; break;
        case 16: f = wide ? hle_QuerySecurityPackageInfoW : hle_QuerySecurityPackageInfoA; break;
        case 24: f = hle_EncryptMessage; break;
        case 25: f = hle_DecryptMessage; break;
        }
        STP(tb + (uint64_t)GP * (i + 1), rt_dyn_thunk_n(f, "schannel", nargs[i]));
    }
    return tb;
}
HLE(InitSecurityInterfaceW) { RET(table(1)); }
HLE(InitSecurityInterfaceA) { RET(table(0)); }

/* crypt32: CryptMsgGetParam has nothing to read here */
HLE(CryptMsgGetParam) { hle_set_error(0x8009200C); RET(0); }
