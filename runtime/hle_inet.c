/* WinInet: HTTP for host names the page serves itself (Module.uewNet.http, an offline stand-in for a game's online
   services); every other host fails as on a machine without a network. Requests complete at once; games that opened
   their session asynchronously get the completion callback from a helper thread, as from WinInet's own workers. */
#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

void hle_set_error(uint32_t e);

enum { K_SESSION = 1, K_CONNECT, K_REQUEST };
typedef struct INet {
    int used, kind, async;
    int parent;
    uint64_t callback, context;
    char host[256];
    uint16_t port;
    int secure;
    char verb[16], path[2048], headers[8192];
    /* response */
    int done, status;
    char* resp_headers;     /* "Name: value\r\n" lines */
    uint8_t* body;
    uint32_t body_len, body_pos;
} INet;
enum { NNET = 512, NET_BASE = 0x7B000000 };
static INet nets[NNET];

static INet* inet(uint64_t h) { uint64_t i = h - NET_BASE; return h >= NET_BASE && i < NNET && nets[i].used ? &nets[i] : NULL; }
static uint64_t inet_new(int kind, int parent)
{
    for (int i = 1; i < NNET; i++)
        if (!nets[i].used) {
            memset(&nets[i], 0, sizeof nets[i]);
            nets[i].used = 1; nets[i].kind = kind; nets[i].parent = parent;
            if (parent) { nets[i].async = nets[parent].async; nets[i].callback = nets[parent].callback; }
            return NET_BASE + (uint64_t)i;
        }
    return 0;
}
static int idx(INet* n) { return (int)(n - nets); }

static void wstr(uint64_t p, char* out, size_t cap, int64_t len)
{
    size_t i = 0;
    for (; p && i + 1 < cap && (len < 0 || (int64_t)i < len); i++) {
        uint16_t ch = (uint16_t)LD16(p + 2 * i);
        if (!ch) break;
        out[i] = (char)(ch < 128 ? ch : '?');
    }
    out[i] = 0;
}

/* ---- the page's handler ---- */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
int inet_claims(const char* host);
/* runs the request; the answer waits in Module.uewNet.last until inet_take copies it */
int inet_request(const char* method, const char* url, const char* headers, const uint8_t* body, int len, int* status, int* hlen);
void inet_take(char* headers, uint8_t* body);
#else
static int inet_claims(const char* host) { (void)host; return 0; }
static int inet_request(const char* m, const char* u, const char* h, const uint8_t* b, int l, int* s, int* hl) { (void)m; (void)u; (void)h; (void)b; (void)l; *s = 0; *hl = 0; return 0; }
static void inet_take(char* h, uint8_t* b) { (void)h; (void)b; }
#endif

/* ---- completion callbacks, from a helper thread ---- */
typedef struct Note { uint64_t cb, h, ctx; uint32_t status; uint64_t result; uint32_t error; uint64_t info; uint32_t info_len; int raw; } Note;
static Note notes[256];
static int nnotes;
static uint64_t notify_thread;

static void notify_loop(CPU* c)
{
    uint64_t info = g_calloc(16);
    for (;;) {
        while (nnotes) {
            Note n = notes[0];
            memmove(notes, notes + 1, sizeof notes[0] * (size_t)--nnotes);
            uint64_t a[5] = { n.h, n.ctx, n.status, 0, 0 };
            if (n.raw) { a[3] = n.info; a[4] = n.info_len; }
            else if (n.status == 100 || n.status == 0x200000) { ST64(info, n.result); ST32(info + 8, n.error); a[3] = info; a[4] = 16; }
            rt_invoke(c, n.cb, 5, a);
        }
        sched_sleep(c, 1);
    }
}
static void notify(INet* n, uint64_t h, uint32_t status, uint64_t result, uint32_t error)
{
    if (!n->async || !n->callback || nnotes == 256) return;
    if (!notify_thread) {
        sched_create(rt_dyn_thunk(notify_loop, "wininet worker"), 0, 0);
        notify_thread = 1;
    }
    notes[nnotes++] = (Note){ n->callback, h, n->context, status, result, error, 0, 0, 0 };
}
static void notify_raw(INet* n, uint64_t h, uint32_t status, uint64_t info, uint32_t len)
{
    if (!n->async || !n->callback || nnotes == 256) return;
    if (!notify_thread) { sched_create(rt_dyn_thunk(notify_loop, "wininet worker"), 0, 0); notify_thread = 1; }
    notes[nnotes++] = (Note){ n->callback, h, n->context, status, 0, 0, info, len, 1 };
}

/* ---- sessions and connections ---- */
HLE(InternetOpenW)
{
    uint64_t h = inet_new(K_SESSION, 0);
    if (!h) { hle_set_error(8); RET(0); return; }
    inet(h)->async = (ARG(5) & 0x10000000) != 0;    /* INTERNET_FLAG_ASYNC */
    RET(h);
}
HLE(InternetOpenA) { hle_InternetOpenW(c); }
HLE(InternetSetStatusCallbackW) { INet* n = inet(A1); uint64_t old = n ? n->callback : 0; if (n) n->callback = A2; RET(old); }
HLE(InternetSetStatusCallbackA) { hle_InternetSetStatusCallbackW(c); }
static void connect_w(CPU* c, int wide)
{
    INet* s = inet(A1);
    if (!s) { hle_set_error(6); RET(0); return; }
    uint64_t h = inet_new(K_CONNECT, idx(s));
    if (!h) { hle_set_error(8); RET(0); return; }
    INet* n = inet(h);
    if (wide) wstr(A2, n->host, sizeof n->host, -1);
    else { size_t i = 0; for (; i + 1 < sizeof n->host && LD8(A2 + i); i++) n->host[i] = (char)LD8(A2 + i); n->host[i] = 0; }
    n->port = (uint16_t)A3;
    n->context = ARG(8);
    RET(h);
}
HLE(InternetConnectW) { connect_w(c, 1); }
HLE(InternetConnectA) { connect_w(c, 0); }

static void astr(uint64_t p, char* out, size_t cap, int64_t len)
{
    size_t i = 0;
    for (; p && i + 1 < cap && (len < 0 || (int64_t)i < len); i++) { out[i] = (char)LD8(p + i); if (!out[i]) return; }
    out[i] = 0;
}
static void gstr(int wide, uint64_t p, char* out, size_t cap, int64_t len) { if (wide) wstr(p, out, cap, len); else astr(p, out, cap, len); }

static void open_request(CPU* c, int wide)
{
    INet* cn = inet(A1);
    if (!cn || cn->kind != K_CONNECT) { hle_set_error(6); RET(0); return; }
    uint64_t h = inet_new(K_REQUEST, idx(cn));
    if (!h) { hle_set_error(8); RET(0); return; }
    INet* r = inet(h);
    snprintf(r->host, sizeof r->host, "%s", cn->host);
    r->port = cn->port;
    if (A2) gstr(wide, A2, r->verb, sizeof r->verb, -1); else strcpy(r->verb, "GET");
    if (A3) gstr(wide, A3, r->path, sizeof r->path, -1); else strcpy(r->path, "/");
    r->secure = (ARG(7) & 0x00800000) != 0 || cn->port == 443;   /* INTERNET_FLAG_SECURE */
    r->context = ARG(8);
    RET(h);
}
HLE(HttpOpenRequestW) { open_request(c, 1); }
HLE(HttpOpenRequestA) { open_request(c, 0); }

static void finish(INet* r, uint64_t h, int ok, uint32_t err)
{
    r->done = 1;
    notify(r, h, 100, ok ? 1 : 0, ok ? 0 : err);   /* INTERNET_STATUS_REQUEST_COMPLETE */
}
static void send_request_w(CPU* c, int wide, uint64_t h, uint64_t hdrs, int64_t hlen, uint64_t body, uint32_t blen)
{
    INet* r = inet(h);
    if (!r || r->kind != K_REQUEST) { hle_set_error(6); RET(0); return; }
    if (hdrs) gstr(wide, hdrs, r->headers + strlen(r->headers), sizeof r->headers - strlen(r->headers), hlen == -1 || hlen == 0xFFFFFFFF ? -1 : hlen);
    free(r->body); r->body = NULL; free(r->resp_headers); r->resp_headers = NULL;
    r->body_len = r->body_pos = 0; r->done = 0;
    if (!inet_claims(r->host)) {
        /* ERROR_INTERNET_NAME_NOT_RESOLVED */
        if (r->async) { finish(r, h, 0, 12007); hle_set_error(997); RET(0); return; }
        hle_set_error(12007);
        RET(0);
        return;
    }
    char url[2400];
    int defport = r->secure ? 443 : 80;
    if (r->port && r->port != defport) snprintf(url, sizeof url, "%s://%s:%u%s", r->secure ? "https" : "http", r->host, r->port, r->path);
    else snprintf(url, sizeof url, "%s://%s%s", r->secure ? "https" : "http", r->host, r->path);
    int status = 0, hl = 0;
    int n = inet_request(r->verb, url, r->headers, body ? G2H(body) : (const uint8_t*)"", body ? (int)blen : 0, &status, &hl);
    r->resp_headers = calloc(1, (size_t)hl + 1);
    r->body = malloc((size_t)n + 1);
    inet_take(r->resp_headers, r->body);
    r->body_len = (uint32_t)n;
    r->status = status;
    if (r->async) { finish(r, h, 1, 0); hle_set_error(997); RET(0); return; }   /* ERROR_IO_PENDING, then the callback */
    r->done = 1;
    RET(1);
}
static void send_request(CPU* c, uint64_t h, uint64_t hdrs, int64_t hlen, uint64_t body, uint32_t blen) { send_request_w(c, 1, h, hdrs, hlen, body, blen); }
HLE(HttpSendRequestW) { send_request(c, A1, A2, (int64_t)(int32_t)A3, A4, (uint32_t)ARG(5)); }
HLE(HttpSendRequestA) { send_request_w(c, 0, A1, A2, (int64_t)(int32_t)A3, A4, (uint32_t)ARG(5)); }
static void add_headers(CPU* c, int wide)
{
    INet* r = inet(A1);
    if (!r) { hle_set_error(6); RET(0); return; }
    gstr(wide, A2, r->headers + strlen(r->headers), sizeof r->headers - strlen(r->headers), (int32_t)A3 == -1 ? -1 : (int64_t)(uint32_t)A3);
    size_t l = strlen(r->headers);
    if (l >= 2 && strcmp(r->headers + l - 2, "\r\n") && l + 2 < sizeof r->headers) strcat(r->headers, "\r\n");
    RET(1);
}
HLE(HttpAddRequestHeadersW) { add_headers(c, 1); }
HLE(HttpAddRequestHeadersA) { add_headers(c, 0); }

/* HttpQueryInfo levels: STATUS_CODE 19, RAW_HEADERS_CRLF 22, CONTENT_LENGTH 5, CONTENT_TYPE 1, CUSTOM 65535; FLAG_NUMBER 0x20000000 */
static const char* header_value(INet* r, const char* name, char* out, size_t cap)
{
    if (!r->resp_headers) return NULL;
    size_t nl = strlen(name);
    for (const char* p = r->resp_headers; *p;) {
        const char* e = strstr(p, "\r\n");
        if (!e) break;
        if ((size_t)(e - p) > nl && p[nl] == ':' && !strncasecmp(p, name, nl)) {
            const char* v = p + nl + 1;
            while (*v == ' ') v++;
            size_t n = (size_t)(e - v) < cap - 1 ? (size_t)(e - v) : cap - 1;
            memcpy(out, v, n); out[n] = 0;
            return out;
        }
        p = e + 2;
    }
    return NULL;
}
static void query_info(CPU* c, int wide)
{
    INet* r = inet(A1);
    uint32_t level = (uint32_t)A2, base = level & 0xFFFF, number = (level & 0x20000000) != 0;
    uint64_t buf = A3, lenp = A4;
    if (!r || !r->done) { hle_set_error(12019); RET(0); return; }   /* ERROR_INTERNET_INCORRECT_HANDLE_STATE */
    char val[8192];
    const char* v = NULL;
    if (base == 19) { snprintf(val, sizeof val, "%d", r->status); v = val; }
    else if (base == 20) { snprintf(val, sizeof val, "%s", r->status < 300 ? "OK" : r->status == 404 ? "Not Found" : "Error"); v = val; }
    else if (base == 22 || base == 21) {
        snprintf(val, sizeof val, "HTTP/1.1 %d %s\r\n%s\r\n", r->status, r->status < 300 ? "OK" : "Error", r->resp_headers ? r->resp_headers : "");
        if (base == 21) for (char* p = val; *p; p++) if (*p == '\r' && p[1] == '\n') { *p = 0; memmove(p + 1, p + 2, strlen(p + 2) + 1); }
        v = val;
    }
    else if (base == 5) v = header_value(r, "Content-Length", val, sizeof val);
    else if (base == 1) v = header_value(r, "Content-Type", val, sizeof val);
    else if (base == 65535) { char name[256]; gstr(wide, buf, name, sizeof name, -1); v = header_value(r, name, val, sizeof val); }
    if (!v) { hle_set_error(12150); RET(0); return; }   /* ERROR_HTTP_HEADER_NOT_FOUND */
    uint32_t cap = lenp ? (uint32_t)LD32(lenp) : 0;
    if (number) {
        if (cap < 4) { ST32(lenp, 4); hle_set_error(122); RET(0); return; }
        ST32(buf, (uint32_t)strtoul(v, NULL, 10));
        ST32(lenp, 4);
        RET(1);
        return;
    }
    size_t n = strlen(v);
    if (base == 21) { n = 0; while (v[n] || v[n + 1]) n++; n++; }   /* NUL-separated, double-NUL terminated */
    uint32_t cw = wide ? 2 : 1, need = (uint32_t)(cw * (n + 1));
    if (cap < need || !buf) { ST32(lenp, need); hle_set_error(122); RET(0); return; }
    for (size_t i = 0; i <= n; i++) { if (wide) ST16(buf + 2 * i, (uint8_t)v[i]); else ST8(buf + i, (uint8_t)v[i]); }
    ST32(lenp, (uint32_t)(cw * n));
    RET(1);
}
HLE(HttpQueryInfoW) { query_info(c, 1); }
HLE(HttpQueryInfoA) { query_info(c, 0); }
HLE(InternetReadFile)
{
    INet* r = inet(A1);
    if (!r || !r->done) { hle_set_error(6); RET(0); return; }
    uint32_t want = (uint32_t)A3, left = r->body_len - r->body_pos, n = want < left ? want : left;
    if (n) memcpy(G2H(A2), r->body + r->body_pos, n);
    r->body_pos += n;
    if (A4) ST32(A4, n);
    RET(1);
}
HLE(InternetQueryDataAvailable) { INet* r = inet(A1); if (A2) ST32(A2, r && r->done ? r->body_len - r->body_pos : 0); RET(r != NULL); }
HLE(InternetCloseHandle)
{
    INet* n = inet(A1);
    if (!n) { RET(0); return; }
    notify(n, A1, 70, 0, 0);   /* INTERNET_STATUS_HANDLE_CLOSING */
    free(n->body); free(n->resp_headers);
    n->used = 0;
    RET(1);
}
HLE(InternetQueryOptionW) { if (A4) { uint32_t cap = (uint32_t)LD32(A4); if (A3 && cap >= 4) ST32(A3, 0); ST32(A4, 4); } RET(1); }
HLE(InternetSetOptionW) { RET(1); }
HLE(InternetSetOptionA) { RET(1); }
HLE(InternetGetConnectedState) { if (A1) ST32(A1, 2); RET(1); }   /* INTERNET_CONNECTION_LAN */
HLE(InternetAttemptConnect) { RET(0); }
HLE(InternetGetLastResponseInfoW) { if (A1) ST32(A1, 0); if (A2 && A3 && LD32(A3)) ST16(A2, 0); if (A3) ST32(A3, 0); RET(1); }

/* URL_COMPONENTSW (x64): size, scheme ptr/len, nScheme, host ptr/len, port, user, password, path, extra */
static void crack_part(int cw, uint64_t comp, int ptr_off, int len_off, uint64_t url, size_t from, size_t n)
{
    uint64_t p = LD64(comp + ptr_off);
    uint32_t cap = (uint32_t)LD32(comp + len_off);
    if (!p && !cap) return;
    if (!p) { ST64(comp + ptr_off, url + cw * from); ST32(comp + len_off, (uint32_t)n); return; }
    if (cap <= n) { ST32(comp + len_off, (uint32_t)n + 1); return; }
    if (cw == 2) { for (size_t i = 0; i < n; i++) ST16(p + 2 * i, LD16(url + 2 * (from + i))); ST16(p + 2 * n, 0); }
    else { for (size_t i = 0; i < n; i++) ST8(p + i, LD8(url + from + i)); ST8(p + n, 0); }
    ST32(comp + len_off, (uint32_t)n);
}
static void crack_url(CPU* c, int wide)
{
    uint64_t url = A1, comp = A4;
    int cw = wide ? 2 : 1;
    size_t len = (uint32_t)A2 ? (uint32_t)A2 : 0;
    if (!len) while (wide ? LD16(url + 2 * len) : LD8(url + len)) len++;
    char s[4096];
    gstr(wide, url, s, sizeof s, (int64_t)len);
    char* se = strstr(s, "://");
    if (!se) { hle_set_error(12006); RET(0); return; }   /* ERROR_INTERNET_UNRECOGNIZED_SCHEME */
    size_t scheme_n = (size_t)(se - s), host_from = scheme_n + 3, host_end = host_from;
    while (s[host_end] && s[host_end] != '/' && s[host_end] != ':' && s[host_end] != '?' && s[host_end] != '#') host_end++;
    int https = scheme_n == 5 && !strncasecmp(s, "https", 5);
    unsigned port = https ? 443 : 80;
    size_t path_from = host_end;
    if (s[host_end] == ':') { port = (unsigned)strtoul(s + host_end + 1, NULL, 10); while (s[path_from] && s[path_from] != '/' && s[path_from] != '?') path_from++; }
    size_t extra_from = path_from;
    while (s[extra_from] && s[extra_from] != '?' && s[extra_from] != '#') extra_from++;
    size_t total = strlen(s);
    crack_part(cw, comp, 8, 16, url, 0, scheme_n);
    ST32(comp + 20, https ? 4 : 3);   /* INTERNET_SCHEME_HTTPS / HTTP */
    crack_part(cw, comp, 24, 32, url, host_from, host_end - host_from);
    ST16(comp + 36, (uint16_t)port);
    crack_part(cw, comp, 40, 48, url, 0, 0);
    crack_part(cw, comp, 56, 64, url, 0, 0);
    crack_part(cw, comp, 72, 80, url, path_from, extra_from - path_from);
    crack_part(cw, comp, 88, 96, url, extra_from, total - extra_from);
    RET(1);
}
HLE(InternetCrackUrlW) { crack_url(c, 1); }
HLE(InternetCrackUrlA) { crack_url(c, 0); }

/* ---- WinHTTP, on the same requests ---- */
HLE(WinHttpOpen) { uint64_t h = inet_new(K_SESSION, 0); if (!h) { hle_set_error(8); RET(0); return; } inet(h)->async = (ARG(5) & 0x10000000) != 0; RET(h); }
HLE(WinHttpSetStatusCallback) { INet* n = inet(A1); uint64_t old = n ? n->callback : 0; if (n) n->callback = A2; RET(old); }
HLE(WinHttpConnect)
{
    INet* s = inet(A1);
    if (!s) { hle_set_error(6); RET(0); return; }
    uint64_t h = inet_new(K_CONNECT, idx(s));
    if (!h) { hle_set_error(8); RET(0); return; }
    wstr(A2, inet(h)->host, sizeof inet(h)->host, -1);
    inet(h)->port = (uint16_t)A3;
    RET(h);
}
HLE(WinHttpOpenRequest)
{
    INet* cn = inet(A1);
    if (!cn || cn->kind != K_CONNECT) { hle_set_error(6); RET(0); return; }
    uint64_t h = inet_new(K_REQUEST, idx(cn));
    if (!h) { hle_set_error(8); RET(0); return; }
    INet* r = inet(h);
    snprintf(r->host, sizeof r->host, "%s", cn->host);
    r->port = cn->port;
    if (A2) wstr(A2, r->verb, sizeof r->verb, -1); else strcpy(r->verb, "GET");
    if (A3) wstr(A3, r->path, sizeof r->path, -1); else strcpy(r->path, "/");
    r->secure = (ARG(7) & 0x00800000) != 0 || cn->port == 443;
    RET(h);
}
HLE(WinHttpAddRequestHeaders) { hle_HttpAddRequestHeadersW(c); }
HLE(WinHttpSendRequest)
{
    INet* r = inet(A1);
    if (!r) { hle_set_error(6); RET(0); return; }
    r->context = ARG(7);
    int async = r->async;
    r->async = 0;     /* done in place; WinHTTP reports completion through the callback even so */
    send_request(c, A1, A2, (int64_t)(int32_t)A3, A4, (uint32_t)ARG(5));
    r->async = async;
    if (!R(0)) {
        if (async) { notify(r, A1, 0x200000, 3, (uint32_t)LD32(g_teb + TEB_ERR)); hle_set_error(997); }   /* REQUEST_ERROR (API_SEND_REQUEST) */
        return;
    }
    notify(r, A1, 0x400000, 0, 0);   /* SENDREQUEST_COMPLETE */
    RET(1);
}
HLE(WinHttpWriteData) { if (A4) ST32(A4, (uint32_t)A3); RET(1); }
HLE(WinHttpReceiveResponse) { INet* r = inet(A1); if (!r || !r->done) { hle_set_error(12019); RET(0); return; } notify(r, A1, 0x20000, 0, 0); RET(1); }   /* HEADERS_AVAILABLE */
/* WinHttpQueryHeaders(h, level, name, buffer, &len, &index): levels as WinInet's, the custom name given apart */
HLE(WinHttpQueryHeaders)
{
    uint64_t save8 = R(8), save9 = R(9);
    uint32_t level = (uint32_t)A2;
    INet* r = inet(A1);
    if (!r) { hle_set_error(6); RET(0); return; }
    if ((level & 0xFFFF) == 65535 && A3) {
        /* the custom header name goes in the buffer for the WinInet path */
        char name[256]; wstr(A3, name, sizeof name, -1);
        char val[4096];
        const char* v = header_value(r, name, val, sizeof val);
        uint64_t buf = A4, lenp = ARG(5);
        if (!v) { hle_set_error(12150); RET(0); return; }
        uint32_t need = (uint32_t)(2 * (strlen(v) + 1)), cap = lenp ? (uint32_t)LD32(lenp) : 0;
        if (!buf || cap < need) { ST32(lenp, need); hle_set_error(122); RET(0); return; }
        for (size_t i = 0; i <= strlen(v); i++) ST16(buf + 2 * i, (uint8_t)v[i]);
        ST32(lenp, need - 2);
        RET(1);
        return;
    }
    /* HttpQueryInfoW(h, level, buffer, &len): buffer and length move into its argument places */
    uint64_t buf = A4, lenp = ARG(5);
    R(8) = buf;
    R(9) = lenp;
    hle_HttpQueryInfoW(c);
    R(8) = save8; R(9) = save9;
}
HLE(WinHttpQueryDataAvailable)
{
    INet* r = inet(A1);
    if (!r || !r->done) { hle_set_error(12019); RET(0); return; }
    uint32_t left = r->body_len - r->body_pos;
    if (A2) ST32(A2, left);
    if (r->async) {
        static uint64_t avail;
        if (!avail) avail = g_calloc(8);
        ST32(avail, left);
        notify_raw(r, A1, 0x40000, avail, 4);   /* DATA_AVAILABLE */
    }
    RET(1);
}
HLE(WinHttpReadData)
{
    INet* r = inet(A1);
    if (!r || !r->done) { hle_set_error(12019); RET(0); return; }
    uint32_t want = (uint32_t)A3, left = r->body_len - r->body_pos, n = want < left ? want : left;
    if (n) memcpy(G2H(A2), r->body + r->body_pos, n);
    r->body_pos += n;
    if (A4) ST32(A4, n);
    notify_raw(r, A1, 0x80000, A2, n);   /* READ_COMPLETE */
    RET(1);
}
HLE(WinHttpCloseHandle)
{
    INet* n = inet(A1);
    if (!n) { RET(0); return; }
    notify(n, A1, 0x800, 0, 0);   /* HANDLE_CLOSING */
    free(n->body); free(n->resp_headers);
    n->used = 0;
    RET(1);
}
HLE(WinHttpCrackUrl) { hle_InternetCrackUrlW(c); }
HLE(WinHttpSetOption) { RET(1); }
HLE(WinHttpQueryOption) { if (A4) ST32(A4, 0); RET(0); }
HLE(WinHttpSetTimeouts) { RET(1); }
HLE(WinHttpSetCredentials) { RET(1); }
HLE(WinHttpQueryAuthSchemes) { hle_set_error(12150); RET(0); }
HLE(WinHttpGetProxyForUrl) { hle_set_error(12180); RET(0); }   /* ERROR_WINHTTP_AUTODETECTION_FAILED */
HLE(WinHttpGetIEProxyConfigForCurrentUser) { if (A1) memset(G2H(A1), 0, 32); RET(1); }
HLE(WinHttpGetDefaultProxyConfiguration) { if (A1) { memset(G2H(A1), 0, 24); ST32(A1, 1); } RET(1); }   /* NO_PROXY */
HLE(WinHttpCheckPlatform) { RET(1); }
HLE(WinHttpWebSocketCompleteUpgrade) { hle_set_error(12150); RET(0); }
