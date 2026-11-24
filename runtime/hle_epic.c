/* Epic Games sign-in for games the Epic launcher installed (--epic account): the page signs the player in on Epic's
   own website and passes the code Epic gives back (or a token kept from an earlier sign-in); this file turns it into
   the one-time exchange code the launcher would hand the game, and adds it to the command line as the launcher does
   (-AUTH_PASSWORD=<code> -AUTH_TYPE=exchangecode, with the account's name and id). The game's EOS SDK signs in with it.

   The requests go to Epic's account service over TLS done here (BearSSL, the root store Schannel uses), carried by
   the page's network routes: through WISP when the build has it, so a device that can't reach Epic itself still signs
   in, and the WISP server only relays ciphertext. No token is logged; the refresh token goes back to the page, which
   keeps it only when the player asked to be remembered. */
#include "host.h"
#include "sched.h"
#include "bearssl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void process_cmdline_append(const char* s);
void host_secure_random(void* buf, size_t n);

#ifdef __EMSCRIPTEN__
int epic_start(void);
int epic_state(void);
int epic_cred(char* buf, int cap);
int epic_namespace(char* buf, int cap);
void epic_done(int ok, const char* refresh, const char* name, const char* msg);
int net_claim(const char* name);
int net_open(int route, int port, int udp);
void net_send(int id, const uint8_t* p, int n);
int net_avail(int id);
int net_recv(int id, uint8_t* p, int cap);
void net_close(int id);
size_t tls_anchors(const br_x509_trust_anchor** out);

/* the Epic Games Launcher's public OAuth client: exchange codes are made for the launcher's sessions */
#define CLIENT_ID "34a02cf8f4414e29b15921876da36f9a"
#define CLIENT_SECRET "daafbccc737745039dffe53d94fc76cf"
#define ACCOUNT_HOST "account-public-service-prod.ol.epicgames.com"
/* room for Epic's tokens (eg1 JWTs pass 2 KB; a cut-off one is refused as "Missing second delimiter") */
#define TOK 16384

static void b64(const char* in, char* out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint8_t)in[i] << 16 | (i + 1 < n ? (uint8_t)in[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < n ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static void form_escape(const char* in, char* out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 4 < cap; in++) {
        unsigned char ch = (unsigned char)*in;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out[o++] = (char)ch;
        else
            o += (size_t)snprintf(out + o, cap - o, "%%%02X", ch);
    }
    out[o] = 0;
}

/* a string field of a JSON object (no nesting needed for these answers); 0 when missing */
static int json_str(const char* j, const char* key, char* out, size_t cap)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char* p = strstr(j, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return 0;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < cap; p++) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p;
    }
    out[o] = 0;
    return *p == '"';   /* a value cut short counts as missing */
}

/* one HTTPS request (Connection: close) to Epic's account service: the HTTP status, the body in out */
static int https(CPU* c, const char* req, char* out, size_t cap)
{
    int route = net_claim(ACCOUNT_HOST);
    int id = route ? net_open(route, 443, 0) : 0;
    if (!id) { snprintf(out, cap, "no network route to Epic (the build needs WISP, or the page a connection)"); return -1; }
    static br_ssl_client_context sc;
    static br_x509_minimal_context xc;
    static unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
    const br_x509_trust_anchor* ta;
    size_t nta = tls_anchors(&ta);
    br_ssl_client_init_full(&sc, &xc, ta, nta);
    br_ssl_engine_set_buffer(&sc.eng, iobuf, sizeof iobuf, 1);
    unsigned char seed[32];
    host_secure_random(seed, sizeof seed);
    br_ssl_engine_inject_entropy(&sc.eng, seed, sizeof seed);
    br_ssl_client_reset(&sc, ACCOUNT_HOST, 0);
    size_t reqlen = strlen(req), sent = 0, got = 0;
    char* resp = malloc(1 << 16);
    size_t rcap = (1 << 16) - 1;
    uint64_t deadline = host_ticks_ns() + 30000000000ull;
    for (;;) {
        unsigned st = br_ssl_engine_current_state(&sc.eng);
        size_t len;
        if (st & BR_SSL_CLOSED) break;
        if (st & BR_SSL_SENDREC) {
            unsigned char* b = br_ssl_engine_sendrec_buf(&sc.eng, &len);
            net_send(id, b, (int)len);
            br_ssl_engine_sendrec_ack(&sc.eng, len);
            continue;
        }
        if (st & BR_SSL_RECVAPP) {
            unsigned char* b = br_ssl_engine_recvapp_buf(&sc.eng, &len);
            size_t k = len < rcap - got ? len : rcap - got;
            memcpy(resp + got, b, k);
            got += k;
            br_ssl_engine_recvapp_ack(&sc.eng, len);
            continue;
        }
        if ((st & BR_SSL_SENDAPP) && sent < reqlen) {
            unsigned char* b = br_ssl_engine_sendapp_buf(&sc.eng, &len);
            size_t k = len < reqlen - sent ? len : reqlen - sent;
            memcpy(b, req + sent, k);
            br_ssl_engine_sendapp_ack(&sc.eng, k);
            sent += k;
            if (sent == reqlen) br_ssl_engine_flush(&sc.eng, 0);
            continue;
        }
        if (st & BR_SSL_RECVREC) {
            int a = net_avail(id);
            if (a < 0) break;
            if (a == 0) {
                if (host_ticks_ns() > deadline) break;
                sched_sleep(c, 4);
                continue;
            }
            unsigned char* b = br_ssl_engine_recvrec_buf(&sc.eng, &len);
            int n = net_recv(id, b, (int)len);
            if (n > 0) br_ssl_engine_recvrec_ack(&sc.eng, (size_t)n);
            continue;
        }
        sched_sleep(c, 4);
        if (host_ticks_ns() > deadline) break;
    }
    net_close(id);
    resp[got] = 0;
    int err = br_ssl_engine_last_error(&sc.eng);
    int status = 0;
    char* body = strstr(resp, "\r\n\r\n");
    if (!got || !body || sscanf(resp, "HTTP/%*s %d", &status) != 1) {
        snprintf(out, cap, err ? "TLS error %d talking to Epic" : "no answer from Epic", err);
        free(resp);
        return -1;
    }
    *body = 0;
    body += 4;
    /* chunked answers: join the chunks */
    if (strstr(resp, "hunked")) {
        char *r = body, *w = body;
        for (;;) {
            unsigned long n = strtoul(r, &r, 16);
            r = strstr(r, "\r\n");
            if (!n || !r) break;
            r += 2;
            memmove(w, r, n);
            w += n;
            r += n + 2;
        }
        *w = 0;
    }
    snprintf(out, cap, "%s", body);
    free(resp);
    return status;
}

/* a token from Epic's account service: grant is "authorization_code" (with code=) or "refresh_token" */
static int token(CPU* c, const char* grant, const char* value, char* out, size_t cap)
{
    static char basic[96], esc[3 * TOK], body[3 * TOK + 256], req[3 * TOK + 1024];
    b64(CLIENT_ID ":" CLIENT_SECRET, basic);
    form_escape(value, esc, sizeof esc);
    snprintf(body, sizeof body, "grant_type=%s&%s=%s&token_type=eg1", grant, strcmp(grant, "authorization_code") ? grant : "code", esc);
    snprintf(req, sizeof req,
             "POST /account/api/oauth/token HTTP/1.1\r\nHost: " ACCOUNT_HOST "\r\nAuthorization: basic %s\r\n"
             "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %zu\r\nUser-Agent: UEWASM\r\nConnection: close\r\n\r\n%s",
             basic, strlen(body), body);
    return https(c, req, out, cap);
}

static int exchange(CPU* c, const char* access, char* out, size_t cap)
{
    static char req[TOK + 1024];
    snprintf(req, sizeof req, "GET /account/api/oauth/exchange HTTP/1.1\r\nHost: " ACCOUNT_HOST "\r\nAuthorization: bearer %s\r\n"
             "User-Agent: UEWASM\r\nConnection: close\r\n\r\n", access);
    return https(c, req, out, cap);
}

static int epic_ready(GThread* t) { (void)t; return epic_state() != 0; }

/* why Epic refused: its errorMessage, or the status. Epic quotes the code or token it was sent ('...'), which must not
   reach the screen or the log, so quoted text is left out */
static void reason(int status, const char* body, char* msg, size_t cap)
{
    static char m[2 * TOK], clean[512];
    if (status > 0 && json_str(body, "errorMessage", m, sizeof m)) {
        size_t o = 0;
        int quoted = 0;
        for (const char* p = m; *p && o + 1 < sizeof clean; p++) {
            if (*p == '\'') { quoted = !quoted; if (!quoted && o + 1 < sizeof clean) clean[o++] = '-'; continue; }
            if (!quoted) clean[o++] = *p;
        }
        clean[o] = 0;
        snprintf(msg, cap, "Epic: %s", clean);
    }
    else if (status > 0) snprintf(msg, cap, "Epic answered HTTP %d", status);
    else snprintf(msg, cap, "%s", body);
}

void epic_signin(CPU* c)
{
    if (!epic_start()) return;
    static char ans[4 * TOK];
    static char cred[TOK], access[TOK], refresh[TOK];
    char acct[64], name[128], code[128], ns[128], msg[300], arg[512];
    for (int tries = 0; tries < 8; tries++) {
        if (epic_state() == 0) sched_block_ext(c, epic_ready, host_ticks_ns() + 3600000000000ull);
        if (epic_state() != 1) { host_log("[epic] playing without an Epic account"); return; }
        epic_cred(cred, sizeof cred);
        char* nl = strchr(cred, '\n');
        if (!nl) { epic_done(0, NULL, NULL, "no sign-in code"); continue; }
        *nl = 0;
        int st = token(c, cred, nl + 1, ans, sizeof ans);
        memset(cred, 0, sizeof cred);
        if (st != 200 || !json_str(ans, "access_token", access, sizeof access)) { reason(st, ans, msg, sizeof msg); epic_done(0, NULL, NULL, msg); continue; }
        json_str(ans, "refresh_token", refresh, sizeof refresh);
        if (!json_str(ans, "account_id", acct, sizeof acct)) acct[0] = 0;
        if (!json_str(ans, "displayName", name, sizeof name)) snprintf(name, sizeof name, "Player");
        st = exchange(c, access, ans, sizeof ans);
        memset(access, 0, sizeof access);
        if (st != 200 || !json_str(ans, "code", code, sizeof code)) { reason(st, ans, msg, sizeof msg); epic_done(0, NULL, NULL, msg); continue; }
        for (char* p = name; *p; p++) if (*p == '"') *p = '\'';
        snprintf(arg, sizeof arg, "-AUTH_LOGIN=unused -AUTH_PASSWORD=%s -AUTH_TYPE=exchangecode -epicusername=\"%s\" -epicuserid=%s", code, name, acct);
        process_cmdline_append(arg);
        if (epic_namespace(ns, sizeof ns) && ns[0]) { snprintf(arg, sizeof arg, "-epicsandboxid=%s", ns); process_cmdline_append(arg); }
        memset(code, 0, sizeof code);
        epic_done(1, refresh, name, NULL);
        memset(refresh, 0, sizeof refresh);
        host_log("[epic] signed in to Epic Games as %s", name);
        return;
    }
    host_log("[epic] Epic sign-in failed; playing without an Epic account");
}
#else
void epic_signin(CPU* c) { (void)c; }
#endif
