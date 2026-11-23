// Real connections from a recompiled program: TCP by host name, TCP to a raw address, UDP (a DNS query), and HTTPS through
// Schannel the way libcurl's schannel backend drives it. In the browser these go through a WISP server (uewasm-cli serve
// --wisp) and the runtime's Schannel; natively, straight out and Windows' own. Prints OK lines and DONE.
#define SECURITY_WIN32
#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <bcrypt.h>
#include <wincrypt.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

static int http_status(SOCKET s, const char* host, char* line, int cap)
{
    char req[256], buf[2048];
    int n = snprintf(req, sizeof req, "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    if (send(s, req, n, 0) != n) return 0;
    int got = 0, r;
    while (got < (int)sizeof buf - 1 && (r = recv(s, buf + got, sizeof buf - 1 - got, 0)) > 0) {
        got += r;
        if (memchr(buf, '\n', got)) break;
    }
    buf[got] = 0;
    char* e = strpbrk(buf, "\r\n");
    if (e) *e = 0;
    snprintf(line, cap, "%s", buf);
    return strncmp(buf, "HTTP/1.", 7) == 0;
}

static SOCKET connect_to(const char* host, const char* port)
{
    struct addrinfo hints = { 0 }, *ai = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai)) return INVALID_SOCKET;
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(s, ai->ai_addr, (int)ai->ai_addrlen)) { closesocket(s); s = INVALID_SOCKET; }
    freeaddrinfo(ai);
    return s;
}

// HTTPS GET / through SSPI's Schannel package; the status line in line
static int https_status(const char* host, int curl_style, char* line, int cap)
{
    PSecurityFunctionTableA sspi = InitSecurityInterfaceA();
    SCHANNEL_CRED sc = { 0 };
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    // libcurl validates the server certificate itself (manual mode); others let Schannel do it
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | (curl_style ? SCH_CRED_MANUAL_CRED_VALIDATION : SCH_CRED_AUTO_CRED_VALIDATION);
    CredHandle cred;
    TimeStamp ts;
    if (sspi->AcquireCredentialsHandleA(NULL, (char*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &cred, &ts) != SEC_E_OK) {
        snprintf(line, cap, "AcquireCredentialsHandle failed");
        return 0;
    }
    SOCKET s = connect_to(host, "443");
    if (s == INVALID_SOCKET) { snprintf(line, cap, "connect failed"); return 0; }
    DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM, attrs;
    CtxtHandle ctx;
    // libcurl leaves the output buffers SECBUFFER_EMPTY for Schannel to fill; others name them SECBUFFER_TOKEN
    SecBuffer ob = { 0, curl_style ? SECBUFFER_EMPTY : SECBUFFER_TOKEN, NULL };
    SecBufferDesc od = { SECBUFFER_VERSION, 1, &ob };
    SECURITY_STATUS st = sspi->InitializeSecurityContextA(&cred, NULL, (char*)host, req, 0, 0, NULL, 0, &ctx, &od, &attrs, &ts);
    static char in[65536];
    int have = 0;
    for (;;) {
        if (ob.cbBuffer) { send(s, (char*)ob.pvBuffer, ob.cbBuffer, 0); sspi->FreeContextBuffer(ob.pvBuffer); ob.cbBuffer = 0; ob.pvBuffer = NULL; }
        if (st == SEC_E_OK) break;
        if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_INCOMPLETE_MESSAGE) { snprintf(line, cap, "handshake failed %08lx", (unsigned long)st); return 0; }
        if (st == SEC_E_INCOMPLETE_MESSAGE || have == 0) {
            int r = recv(s, in + have, (int)sizeof in - have, 0);
            if (r <= 0) { snprintf(line, cap, "connection closed in the handshake"); return 0; }
            have += r;
        }
        SecBuffer ib[2] = { { (unsigned long)have, SECBUFFER_TOKEN, in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc id = { SECBUFFER_VERSION, 2, ib };
        ob.BufferType = curl_style ? SECBUFFER_EMPTY : SECBUFFER_TOKEN; ob.cbBuffer = 0; ob.pvBuffer = NULL;
        st = sspi->InitializeSecurityContextA(&cred, &ctx, (char*)host, req, 0, 0, &id, 0, NULL, &od, &attrs, &ts);
        if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) { memmove(in, in + have - ib[1].cbBuffer, ib[1].cbBuffer); have = ib[1].cbBuffer; }
        else have = 0;
    }
    if (curl_style) {
        // libcurl's manual check: the server certificate, a chain to a trusted root, and the host among its names
        PCCERT_CONTEXT cert = NULL;
        PCCERT_CHAIN_CONTEXT ch = NULL;
        CERT_CHAIN_PARA cp = { sizeof cp };
        if (sspi->QueryContextAttributesA(&ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) != SEC_E_OK || !cert) { snprintf(line, cap, "no server certificate"); return 0; }
        if (!CertGetCertificateChain(NULL, cert, NULL, cert->hCertStore, &cp, 0, NULL, &ch) || ch->TrustStatus.dwErrorStatus) {
            snprintf(line, cap, "chain not trusted (%08lx)", ch ? (unsigned long)ch->TrustStatus.dwErrorStatus : 0ul);
            return 0;
        }
        CERT_CHAIN_POLICY_PARA pp = { sizeof pp };
        CERT_CHAIN_POLICY_STATUS ps = { sizeof ps };
        if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, ch, &pp, &ps) || ps.dwError) { snprintf(line, cap, "policy error %08lx", (unsigned long)ps.dwError); return 0; }
        CertFreeCertificateChain(ch);
        char names[4096];
        DWORD k = CertGetNameStringA(cert, CERT_NAME_DNS_TYPE, CERT_NAME_SEARCH_ALL_NAMES_FLAG, NULL, names, sizeof names);
        int match = 0;
        for (char* p = names; k > 1 && *p; p += strlen(p) + 1) {
            const char* dot = strchr(host, '.');
            if (!_stricmp(p, host) || (p[0] == '*' && p[1] == '.' && dot && !_stricmp(p + 2, dot + 1))) match = 1;
        }
        if (!match) { snprintf(line, cap, "host not among the certificate's names"); return 0; }
        // libcurl's certificate info walks the store the server certificate comes in: the chain the server sent
        int count = 0;
        for (PCCERT_CONTEXT e = CertEnumCertificatesInStore(cert->hCertStore, NULL); e; e = CertEnumCertificatesInStore(cert->hCertStore, e)) count++;
        if (count < 1) { snprintf(line, cap, "no certificates in the server's store"); return 0; }
        CertFreeCertificateContext(cert);
    }
    // libcurl insists the context attributes Schannel returns are the ones it asked for
    if (curl_style && attrs != req) { snprintf(line, cap, "context attributes %08lx, asked for %08lx", (unsigned long)attrs, (unsigned long)req); return 0; }
    SecPkgContext_StreamSizes sz;
    sspi->QueryContextAttributesA(&ctx, SECPKG_ATTR_STREAM_SIZES, &sz);
    static char msg[20000];
    int n = snprintf(msg + sz.cbHeader, sizeof msg - sz.cbHeader - sz.cbTrailer, "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host);
    SecBuffer eb[4] = { { sz.cbHeader, SECBUFFER_STREAM_HEADER, msg }, { (unsigned long)n, SECBUFFER_DATA, msg + sz.cbHeader },
                        { sz.cbTrailer, SECBUFFER_STREAM_TRAILER, msg + sz.cbHeader + n }, { 0, SECBUFFER_EMPTY, NULL } };
    SecBufferDesc ed = { SECBUFFER_VERSION, 4, eb };
    if (sspi->EncryptMessage(&ctx, 0, &ed, 0) != SEC_E_OK) { snprintf(line, cap, "EncryptMessage failed"); return 0; }
    send(s, msg, eb[0].cbBuffer + eb[1].cbBuffer + eb[2].cbBuffer, 0);
    char plain[4096];
    int got = 0;
    while (got < (int)sizeof plain - 1 && !memchr(plain, '\n', got)) {
        SecBuffer db[4] = { { (unsigned long)have, SECBUFFER_DATA, in }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc dd = { SECBUFFER_VERSION, 4, db };
        SECURITY_STATUS r = have ? sspi->DecryptMessage(&ctx, &dd, 0, NULL) : SEC_E_INCOMPLETE_MESSAGE;
        if (r == SEC_E_INCOMPLETE_MESSAGE) {
            int k = recv(s, in + have, (int)sizeof in - have, 0);
            if (k <= 0) break;
            have += k;
            continue;
        }
        if (r != SEC_E_OK) break;
        int extra = 0;
        for (int i = 0; i < 4; i++) {
            if (db[i].BufferType == SECBUFFER_DATA && db[i].cbBuffer) {
                int m = (int)db[i].cbBuffer < (int)sizeof plain - 1 - got ? (int)db[i].cbBuffer : (int)sizeof plain - 1 - got;
                memcpy(plain + got, db[i].pvBuffer, m);
                got += m;
            }
            if (db[i].BufferType == SECBUFFER_EXTRA) extra = (int)db[i].cbBuffer;
        }
        memmove(in, in + have - extra, extra);
        have = extra;
    }
    plain[got] = 0;
    char* e = strpbrk(plain, "\r\n");
    if (e) *e = 0;
    snprintf(line, cap, "%s", plain);
    sspi->DeleteSecurityContext(&ctx);
    sspi->FreeCredentialsHandle(&cred);
    closesocket(s);
    return strncmp(plain, "HTTP/1.", 7) == 0;
}

static void hex(const unsigned char* d, int n, char* out) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]); }

// a whole HTTPS download in libcurl's pattern (16 KB reads, records split across reads): body size and SHA-256
static int https_download(const char* host, const char* path, char* line, int cap)
{
    PSecurityFunctionTableA sspi = InitSecurityInterfaceA();
    SCHANNEL_CRED sc = { 0 };
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION;
    CredHandle cred;
    TimeStamp ts;
    if (sspi->AcquireCredentialsHandleA(NULL, (char*)UNISP_NAME_A, SECPKG_CRED_OUTBOUND, NULL, &sc, NULL, NULL, &cred, &ts) != SEC_E_OK) { snprintf(line, cap, "credentials"); return 0; }
    SOCKET s = connect_to(host, "443");
    if (s == INVALID_SOCKET) { snprintf(line, cap, "connect failed"); return 0; }
    DWORD req = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM, attrs;
    CtxtHandle ctx;
    SecBuffer ob = { 0, SECBUFFER_EMPTY, NULL };
    SecBufferDesc od = { SECBUFFER_VERSION, 1, &ob };
    SECURITY_STATUS st = sspi->InitializeSecurityContextA(&cred, NULL, (char*)host, req, 0, 0, NULL, 0, &ctx, &od, &attrs, &ts);
    static char in[65536];
    int have = 0;
    for (;;) {
        if (ob.cbBuffer) { send(s, (char*)ob.pvBuffer, ob.cbBuffer, 0); sspi->FreeContextBuffer(ob.pvBuffer); ob.cbBuffer = 0; ob.pvBuffer = NULL; }
        if (st == SEC_E_OK) break;
        if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_INCOMPLETE_MESSAGE) { snprintf(line, cap, "handshake failed %08lx", (unsigned long)st); return 0; }
        if (st == SEC_E_INCOMPLETE_MESSAGE || have == 0) {
            int r = recv(s, in + have, (int)sizeof in - have, 0);
            if (r <= 0) { snprintf(line, cap, "closed in the handshake"); return 0; }
            have += r;
        }
        SecBuffer ib[2] = { { (unsigned long)have, SECBUFFER_TOKEN, in }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc id = { SECBUFFER_VERSION, 2, ib };
        ob.BufferType = SECBUFFER_EMPTY; ob.cbBuffer = 0; ob.pvBuffer = NULL;
        st = sspi->InitializeSecurityContextA(&cred, &ctx, (char*)host, req, 0, 0, &id, 0, NULL, &od, &attrs, &ts);
        if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
        if (ib[1].BufferType == SECBUFFER_EXTRA && ib[1].cbBuffer) { memmove(in, in + have - ib[1].cbBuffer, ib[1].cbBuffer); have = ib[1].cbBuffer; }
        else have = 0;
    }
    SecPkgContext_StreamSizes sz;
    sspi->QueryContextAttributesA(&ctx, SECPKG_ATTR_STREAM_SIZES, &sz);
    static char msg[4096];
    int n = snprintf(msg + sz.cbHeader, sizeof msg - sz.cbHeader - sz.cbTrailer, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: nettest\r\nConnection: close\r\n\r\n", path, host);
    SecBuffer eb[4] = { { sz.cbHeader, SECBUFFER_STREAM_HEADER, msg }, { (unsigned long)n, SECBUFFER_DATA, msg + sz.cbHeader },
                        { sz.cbTrailer, SECBUFFER_STREAM_TRAILER, msg + sz.cbHeader + n }, { 0, SECBUFFER_EMPTY, NULL } };
    SecBufferDesc ed = { SECBUFFER_VERSION, 4, eb };
    if (sspi->EncryptMessage(&ctx, 0, &ed, 0) != SEC_E_OK) { snprintf(line, cap, "EncryptMessage failed"); return 0; }
    send(s, msg, eb[0].cbBuffer + eb[1].cbBuffer + eb[2].cbBuffer, 0);
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hh;
    BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0);
    static char head[8192];
    int hl = 0, in_body = 0;
    long long body = 0;
    for (;;) {
        SecBuffer db[4] = { { (unsigned long)have, SECBUFFER_DATA, in }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL }, { 0, SECBUFFER_EMPTY, NULL } };
        SecBufferDesc dd = { SECBUFFER_VERSION, 4, db };
        SECURITY_STATUS r = have ? sspi->DecryptMessage(&ctx, &dd, 0, NULL) : SEC_E_INCOMPLETE_MESSAGE;
        if (r == SEC_E_INCOMPLETE_MESSAGE) {
            int want = (int)sizeof in - have < 16384 ? (int)sizeof in - have : 16384;
            int k = recv(s, in + have, want, 0);
            if (k <= 0) break;
            have += k;
            continue;
        }
        if (r != SEC_E_OK) break;
        int extra = 0;
        for (int i = 0; i < 4; i++) {
            if (db[i].BufferType == SECBUFFER_DATA && db[i].cbBuffer) {
                const char* p = (const char*)db[i].pvBuffer;
                int m = (int)db[i].cbBuffer;
                if (!in_body) {
                    int take = m < (int)sizeof head - hl ? m : (int)sizeof head - hl;
                    memcpy(head + hl, p, take);
                    hl += take;
                    char* e = NULL;
                    for (int j = 3; j < hl; j++) if (!memcmp(head + j - 3, "\r\n\r\n", 4)) { e = head + j + 1; break; }
                    if (!e) continue;
                    in_body = 1;
                    int before = hl - take, hdr = (int)(e - head);
                    p += hdr - before; m -= hdr - before;
                }
                BCryptHashData(hh, (PUCHAR)p, m, 0);
                body += m;
            }
            if (db[i].BufferType == SECBUFFER_EXTRA) extra = (int)db[i].cbBuffer;
        }
        memmove(in, in + have - extra, extra);
        have = extra;
    }
    unsigned char d[32];
    char h[65];
    BCryptFinishHash(hh, d, 32, 0);
    BCryptDestroyHash(hh);
    BCryptCloseAlgorithmProvider(alg, 0);
    hex(d, 32, h);
    char* e = strpbrk(head, "\r\n");
    if (e) *e = 0;
    snprintf(line, cap, "%s, %lld bytes, sha256 %s", head, body, h);
    sspi->DeleteSecurityContext(&ctx);
    sspi->FreeCredentialsHandle(&cred);
    closesocket(s);
    return strncmp(head, "HTTP/1.1 200", 12) == 0;
}

// hashes through CNG (BCrypt, with HMAC) and CryptoAPI, and random numbers from both
static void hashes(void)
{
    const char* msg = "The quick brown fox jumps over the lazy dog";
    unsigned char d[64];
    char h[129];
    BCRYPT_ALG_HANDLE alg;
    BCRYPT_HASH_HANDLE hh;
    DWORD len = 0, got = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) == 0 &&
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&len, sizeof len, &got, 0) == 0 &&
        BCryptCreateHash(alg, &hh, NULL, 0, NULL, 0, 0) == 0 && BCryptHashData(hh, (PUCHAR)msg, (ULONG)strlen(msg), 0) == 0 &&
        BCryptFinishHash(hh, d, len, 0) == 0) { hex(d, (int)len, h); printf("OK hash bcrypt sha256: %s\n", h); BCryptDestroyHash(hh); }
    else printf("FAIL hash bcrypt sha256\n");
    BCryptCloseAlgorithmProvider(alg, 0);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG) == 0 &&
        BCryptHash(alg, (PUCHAR)"key", 3, (PUCHAR)msg, (ULONG)strlen(msg), d, 32) == 0) { hex(d, 32, h); printf("OK hash bcrypt hmac-sha256: %s\n", h); }
    else printf("FAIL hash bcrypt hmac-sha256\n");
    BCryptCloseAlgorithmProvider(alg, 0);
    HCRYPTPROV prov;
    HCRYPTHASH ch;
    const ALG_ID ids[2] = { CALG_MD5, CALG_SHA1 };
    const char* names[2] = { "md5", "sha1" };
    if (CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        for (int i = 0; i < 2; i++) {
            DWORD n = sizeof d;
            if (CryptCreateHash(prov, ids[i], 0, 0, &ch) && CryptHashData(ch, (const BYTE*)msg, (DWORD)strlen(msg), 0) &&
                CryptGetHashParam(ch, HP_HASHVAL, d, &n, 0)) { hex(d, (int)n, h); printf("OK hash cryptoapi %s: %s\n", names[i], h); CryptDestroyHash(ch); }
            else printf("FAIL hash cryptoapi %s\n", names[i]);
        }
    static const unsigned char zero[16];
    unsigned char r1[16] = { 0 }, r2[16] = { 0 };
    BCryptGenRandom(NULL, r1, sizeof r1, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    CryptGenRandom(prov, sizeof r2, r2);
    printf(memcmp(r1, r2, 16) && memcmp(r1, zero, 16) ? "OK random: differs\n" : "FAIL random: same bytes\n");
    CryptReleaseContext(prov, 0);
}

int main(void)
{
    hashes();
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
    char line[256];

    SOCKET s = connect_to("example.com", "80");
    if (s != INVALID_SOCKET && http_status(s, "example.com", line, sizeof line)) printf("OK tcp by name: %s\n", line);
    else printf("FAIL tcp by name (error %d)\n", WSAGetLastError());
    if (s != INVALID_SOCKET) closesocket(s);

    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons(80);
    inet_pton(AF_INET, "1.1.1.1", &a.sin_addr);
    SOCKET t = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(t, (struct sockaddr*)&a, sizeof a) == 0 && http_status(t, "one.one.one.one", line, sizeof line))
        printf("OK tcp by address: %s\n", line);
    else
        printf("FAIL tcp by address (error %d)\n", WSAGetLastError());
    closesocket(t);

    // a DNS question for example.com (A), to 1.1.1.1:53
    unsigned char q[64] = { 0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    int n = 12;
    const char* labels[] = { "example", "com" };
    for (int i = 0; i < 2; i++) { q[n++] = (unsigned char)strlen(labels[i]); memcpy(q + n, labels[i], strlen(labels[i])); n += (int)strlen(labels[i]); }
    q[n++] = 0; q[n++] = 0; q[n++] = 1; q[n++] = 0; q[n++] = 1;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, 0);
    a.sin_port = htons(53);
    unsigned char ans[512];
    struct sockaddr_in from;
    int fl = sizeof from, r = -1;
    if (sendto(u, (const char*)q, n, 0, (struct sockaddr*)&a, sizeof a) == n)
        r = recvfrom(u, (char*)ans, sizeof ans, 0, (struct sockaddr*)&from, &fl);
    if (r >= 12 && ans[0] == 0x12 && ans[1] == 0x34) {
        char ip[32];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        printf("OK udp: %d answers from %s:%d\n", ans[6] << 8 | ans[7], ip, ntohs(from.sin_port));
    } else
        printf("FAIL udp (got %d, error %d)\n", r, WSAGetLastError());
    closesocket(u);

    if (https_status("example.com", 0, line, sizeof line)) printf("OK https (schannel): %s\n", line);
    else printf("FAIL https (schannel): %s\n", line);
    if (https_status("api.epicgames.dev", 1, line, sizeof line)) printf("OK https epic (schannel): %s\n", line);
    else printf("FAIL https epic (schannel): %s\n", line);
    if (https_download("raw.githubusercontent.com", "/torvalds/linux/v6.0/MAINTAINERS", line, sizeof line)) printf("OK https download: %s\n", line);
    else printf("FAIL https download: %s\n", line);
    printf("DONE\n");
    WSACleanup();
    return 0;
}
