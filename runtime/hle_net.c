/* Winsock inside the page: TCP and UDP between sockets of this process over 127.0.0.1 (libraries such as libcurl
   make a loopback socket pair to wake themselves), and an offline machine for every other address. */
#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { NSOCK = 256, SOCK_BASE = 0x5000, BUFSZ = 1 << 16, QMAX = 16 };
enum { E_WOULDBLOCK = 10035, E_NOTSOCK = 10038, E_INVAL = 10022, E_NOTCONN = 10057, E_REFUSED = 10061, E_UNREACH = 10051,
       E_AFNOSUPPORT = 10047, E_ADDRINUSE = 10048, E_ISCONN = 10056 };

typedef struct Sock {
    int used, type, family, nonblock, listening, connected, shut_rd, shut_wr, peer_closed;
    uint16_t port;                  /* bound port, host order */
    int peer;                       /* connected stream: the other socket */
    int queue[QMAX], nq;            /* listening: accepted-to-be sockets */
    uint8_t* buf; uint32_t head, len;
    struct { uint16_t from; uint32_t n; uint8_t* p; } dgram[QMAX]; int nd;
    int bridge;                     /* a connection the page serves (Module.uewNet): its id there */
} Sock;

/* The page may serve some host names itself (an offline stand-in for a game's online services): Module.uewNet.claim(name)
   takes a name, getaddrinfo gives it an address 127.1.x.y, and connections there exchange bytes with
   Module.uewNet.open(name, port) -> { send(Uint8Array), close() } which answers through Module.uewNetPush */
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_JS(int, net_claim, (const char* name), {
    const n = Module.uewNet;
    if (!n || !n.claim) return 0;
    const s = UTF8ToString(name >>> 0);
    if (!n.claim(s)) return 0;
    n.names = n.names || [""];
    let i = n.names.indexOf(s);
    if (i < 0) { i = n.names.length; n.names.push(s); }
    return i;
});
EM_JS(int, net_open, (int route, int port), {
    const n = Module.uewNet;
    const c = n && n.open ? n.open(n.names[route], port) : null;
    if (!c) return 0;
    n.conns = n.conns || [null];
    const id = n.conns.length;
    c.q = []; c.qlen = 0; c.closed = false;
    c.push = data => { if (data && data.length) { c.q.push(data); c.qlen += data.length; } };
    c.end = () => { c.closed = true; };
    n.conns.push(c);
    return id;
});
EM_JS(void, net_send, (int id, const uint8_t* p, int n), {
    const c = Module.uewNet.conns[id];
    if (c && c.send) c.send(HEAPU8.slice(p >>> 0, (p >>> 0) + n));
});
EM_JS(int, net_avail, (int id), {
    const c = Module.uewNet && Module.uewNet.conns ? Module.uewNet.conns[id] : null;
    if (!c) return -1;
    return c.qlen ? c.qlen : c.closed ? -1 : 0;
});
EM_JS(int, net_recv, (int id, uint8_t* p, int cap), {
    const c = Module.uewNet.conns[id];
    if (!c) return -1;
    if (!c.qlen) return c.closed ? -1 : 0;
    let n = 0;
    while (n < cap && c.q.length) {
        const d = c.q[0], k = Math.min(cap - n, d.length);
        HEAPU8.set(d.subarray(0, k), (p >>> 0) + n);
        n += k;
        if (k == d.length) c.q.shift(); else c.q[0] = d.subarray(k);
    }
    c.qlen -= n;
    return n;
});
EM_JS(void, net_close, (int id), {
    const c = Module.uewNet && Module.uewNet.conns ? Module.uewNet.conns[id] : null;
    if (c) { if (c.close) c.close(); Module.uewNet.conns[id] = null; }
});
#else
static int net_claim(const char* name) { (void)name; return 0; }
static int net_open(int route, int port) { (void)route; (void)port; return 0; }
static void net_send(int id, const uint8_t* p, int n) { (void)id; (void)p; (void)n; }
static int net_avail(int id) { (void)id; return -1; }
static int net_recv(int id, uint8_t* p, int cap) { (void)id; (void)p; (void)cap; return -1; }
static void net_close(int id) { (void)id; }
#endif

static Sock socks[NSOCK];
static struct { int ev; uint32_t mask, seen; } sel[NSOCK];   /* WSAEventSelect: event + 1, mask, events already reported */
static uint32_t wsa_err;
static uint16_t next_port = 49152;

static int net_dbg = -1;
#define NL(...) do { if (net_dbg < 0) net_dbg = host_query_flag("netdebug"); if (net_dbg) host_log("[net] " __VA_ARGS__); } while (0)
static void err(uint32_t e) { wsa_err = e; NL("error %u", e); }
static Sock* get(uint64_t s) { uint64_t i = s - SOCK_BASE; return s >= SOCK_BASE && i < NSOCK && socks[i].used ? &socks[i] : NULL; }
static int idx(Sock* k) { return (int)(k - socks); }

HLE(WSAStartup)
{
    if (A2) { memset(G2H(A2), 0, 408); ST16(A2, (uint16_t)A1); ST16(A2 + 2, 0x0202); ST16(A2 + 4, 32767); memcpy(G2H(A2 + 16), "WinSock 2.0", 12); memcpy(G2H(A2 + 273), "Running", 8); }
    RET(0);
}
STUB(WSACleanup, 0)
HLE(WSAGetLastError) { RET(wsa_err); }
HLE(WSASetLastError) { wsa_err = (uint32_t)A1; RET(0); }

static uint64_t new_sock(uint32_t af, uint32_t type)
{
    if (af != 2 && af != 23) { err(E_AFNOSUPPORT); return (uint64_t)-1; }
    for (int i = 0; i < NSOCK; i++)
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].used = 1; socks[i].type = (int)type; socks[i].family = (int)af; socks[i].peer = -1;
            return SOCK_BASE + (uint64_t)i;
        }
    err(10024);
    return (uint64_t)-1;
}
HLE(socket) { NL("socket %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3); RET(new_sock((uint32_t)A1, (uint32_t)A2)); }
HLE(WSASocketW) { NL("WSASocketW %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3); RET(new_sock((uint32_t)A1, (uint32_t)A2)); }
HLE(WSASocketA) { RET(new_sock((uint32_t)A1, (uint32_t)A2)); }

static void free_buf(Sock* k)
{
    free(k->buf); k->buf = NULL; k->len = k->head = 0;
    for (int i = 0; i < k->nd; i++) free(k->dgram[i].p);
    k->nd = 0;
}
HLE(closesocket)
{
    NL("closesocket %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (k->bridge) net_close(k->bridge);
    if (k->peer >= 0 && socks[k->peer].used) { socks[k->peer].peer_closed = 1; socks[k->peer].peer = -1; }
    for (int i = 0; i < k->nq; i++) { Sock* q = &socks[k->queue[i]]; free_buf(q); q->used = 0; }
    free_buf(k);
    sel[idx(k)].ev = 0;
    k->used = 0;
    RET(0);
}

/* sockaddr: family at +0; IPv4 port at +2 (network order), address at +4; IPv6 port at +2, address at +8 */
static int is_loopback(uint64_t sa)
{
    uint16_t fam = (uint16_t)LD16(sa);
    if (fam == 2) { uint8_t a = (uint8_t)LD8(sa + 4); uint32_t v = (uint32_t)LD32(sa + 4); return a == 127 || v == 0; }
    if (fam == 23) { for (int i = 0; i < 15; i++) if (LD8(sa + 8 + i)) return 0; return LD8(sa + 23) <= 1; }
    return 0;
}
static uint16_t sa_port(uint64_t sa) { uint16_t p = (uint16_t)LD16(sa + 2); return (uint16_t)((p >> 8) | (p << 8)); }
static int port_taken(uint16_t p) { for (int i = 0; i < NSOCK; i++) if (socks[i].used && socks[i].port == p) return 1; return 0; }
static uint16_t free_port(void) { while (port_taken(next_port) || next_port < 49152) next_port = (uint16_t)(next_port + 1 ? next_port + 1 : 49152); return next_port++; }

static void put_addr(Sock* k, uint64_t sa, uint64_t lenp, uint16_t port)
{
    if (!sa) return;
    uint32_t cap = lenp ? (uint32_t)LD32(lenp) : 16, n = k->family == 23 ? 28 : 16;
    uint8_t tmp[28] = { 0 };
    tmp[0] = (uint8_t)k->family; tmp[2] = (uint8_t)(port >> 8); tmp[3] = (uint8_t)port;
    if (k->family == 23) tmp[23] = 1; else { tmp[4] = 127; tmp[7] = 1; }
    memcpy(G2H(sa), tmp, cap < n ? cap : n);
    if (lenp) ST32(lenp, n);
}

HLE(bind)
{
    NL("bind %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    uint16_t p = A2 ? sa_port(A2) : 0;
    if (p && port_taken(p)) { err(E_ADDRINUSE); RET((uint64_t)-1); return; }
    k->port = p ? p : free_port();
    RET(0);
}
HLE(getsockname)
{
    NL("getsockname %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (!k->port) k->port = free_port();
    put_addr(k, A2, A3, k->port);
    RET(0);
}
HLE(getpeername)
{
    Sock* k = get(A1);
    if (!k || k->peer < 0) { err(k ? E_NOTCONN : E_NOTSOCK); RET((uint64_t)-1); return; }
    put_addr(k, A2, A3, socks[k->peer].port);
    RET(0);
}
HLE(listen)
{
    NL("listen %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (!k->port) k->port = free_port();
    k->listening = 1;
    RET(0);
}

static int find_listener(uint16_t port, int type)
{
    for (int i = 0; i < NSOCK; i++) if (socks[i].used && socks[i].port == port && socks[i].type == type && (type == 2 || socks[i].listening)) return i;
    return -1;
}

HLE(connect)
{
    NL("connect %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (!A2) { err(E_INVAL); RET((uint64_t)-1); return; }
    if (k->connected) { err(E_ISCONN); RET((uint64_t)-1); return; }
    uint16_t port = sa_port(A2);
    if (k->type == 1 && LD16(A2) == 2 && LD8(A2 + 4) == 127 && LD8(A2 + 5) == 1) {
        int route = (int)(LD8(A2 + 6) << 8 | LD8(A2 + 7));
        k->bridge = net_open(route, port);
        if (!k->bridge) { err(E_REFUSED); RET((uint64_t)-1); return; }
        if (!k->port) k->port = free_port();
        k->connected = 1;
        RET(0);
        return;
    }
    if (!is_loopback(A2)) { err(E_UNREACH); RET((uint64_t)-1); return; }
    if (!k->port) k->port = free_port();
    if (k->type == 2) { k->peer = find_listener(port, 2); k->connected = k->peer >= 0; RET(0); return; }
    int l = find_listener(port, 1);
    if (l < 0 || socks[l].nq >= QMAX) { err(E_REFUSED); RET((uint64_t)-1); return; }
    uint64_t sh = new_sock((uint32_t)socks[l].family, 1);
    if (sh == (uint64_t)-1) { RET((uint64_t)-1); return; }
    Sock* s = get(sh);
    s->port = socks[l].port; s->peer = idx(k); s->connected = 1;
    k->peer = idx(s); k->connected = 1;
    socks[l].queue[socks[l].nq++] = idx(s);
    RET(0);
}

/* bridged sockets: what the page sent so far moves into the socket's own buffer (so peeks and FIONREAD work) */
static void pull(Sock* k)
{
    if (!k->buf) k->buf = malloc(BUFSZ);
    while (k->len < BUFSZ) {
        uint32_t tail = (k->head + k->len) % BUFSZ, room = BUFSZ - k->len, run = tail + room > BUFSZ ? BUFSZ - tail : room;
        int r = net_recv(k->bridge, k->buf + tail, (int)run);
        if (r <= 0) break;
        k->len += (uint32_t)r;
    }
}

static int readable(Sock* k)
{
    if (k->bridge) return k->len > 0 || net_avail(k->bridge) != 0;
    if (k->listening) return k->nq > 0;
    if (k->type == 2) return k->nd > 0;
    return k->len > 0 || k->peer_closed || k->shut_rd;
}

HLE(accept)
{
    NL("accept %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k || !k->listening) { err(k ? E_INVAL : E_NOTSOCK); RET((uint64_t)-1); return; }
    while (!k->nq) {
        if (k->nonblock) { err(E_WOULDBLOCK); RET((uint64_t)-1); return; }
        sched_sleep(c, 1);
        k = get(A1);
        if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    }
    int s = k->queue[0];
    memmove(k->queue, k->queue + 1, sizeof(int) * (size_t)--k->nq);
    if (socks[s].peer >= 0) put_addr(&socks[s], A2, A3, socks[socks[s].peer].port);
    RET(SOCK_BASE + (uint64_t)s);
}

static int64_t do_send(CPU* c, Sock* k, const uint8_t* p, uint32_t n)
{
    if (k->shut_wr) { err(10058); return -1; }
    if (k->bridge) { net_send(k->bridge, p, (int)n); return n; }
    if (k->peer < 0 || !socks[k->peer].used) { err(k->connected ? 10054 : E_NOTCONN); return -1; }
    Sock* d = &socks[k->peer];
    if (!d->buf) d->buf = malloc(BUFSZ);
    uint32_t done = 0;
    while (done < n) {
        uint32_t room = BUFSZ - d->len;
        if (!room) {
            if (done || k->nonblock) break;
            sched_sleep(c, 1);
            continue;
        }
        uint32_t m = n - done < room ? n - done : room;
        for (uint32_t i = 0; i < m; i++) d->buf[(d->head + d->len + i) % BUFSZ] = p[done + i];
        d->len += m;
        done += m;
    }
    if (!done && n) { err(E_WOULDBLOCK); return -1; }
    return done;
}
static int64_t do_recv(CPU* c, uint64_t s, uint8_t* p, uint32_t n, int peek)
{
    Sock* k = get(s);
    if (!k) { err(E_NOTSOCK); return -1; }
    if (k->bridge) pull(k);
    while (!k->len) {
        if (k->bridge) {
            if (net_avail(k->bridge) < 0) return 0;
            if (k->nonblock) { err(E_WOULDBLOCK); return -1; }
            sched_sleep(c, 1);
            k = get(s);
            if (!k) { err(E_NOTSOCK); return -1; }
            pull(k);
            continue;
        }
        if (k->peer_closed || k->shut_rd) return 0;
        if (k->peer < 0) { err(E_NOTCONN); return -1; }
        if (k->nonblock) { err(E_WOULDBLOCK); return -1; }
        sched_sleep(c, 1);
        k = get(s);
        if (!k) { err(E_NOTSOCK); return -1; }
    }
    uint32_t m = n < k->len ? n : k->len;
    for (uint32_t i = 0; i < m; i++) p[i] = k->buf[(k->head + i) % BUFSZ];
    if (!peek) { k->head = (k->head + m) % BUFSZ; k->len -= m; }
    return m;
}

HLE(send)
{
    NL("send %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (k->type == 2) { err(E_NOTCONN); RET((uint64_t)-1); return; }
    RET((uint64_t)do_send(c, k, G2H(A2), (uint32_t)A3));
}
HLE(recv) { NL("recv %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3); RET((uint64_t)do_recv(c, A1, G2H(A2), (uint32_t)A3, (A4 & 2) != 0)); }

HLE(sendto)
{
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (k->type != 2) { RET((uint64_t)do_send(c, k, G2H(A2), (uint32_t)A3)); return; }
    uint64_t to = ARG(5);
    int d = to ? (is_loopback(to) ? find_listener(sa_port(to), 2) : -2) : k->peer;
    if (d == -2) { err(E_UNREACH); RET((uint64_t)-1); return; }
    if (!k->port) k->port = free_port();
    /* to a closed port UDP is just lost */
    if (d >= 0 && socks[d].nd < QMAX) {
        Sock* r = &socks[d];
        r->dgram[r->nd].p = malloc(A3 ? A3 : 1);
        memcpy(r->dgram[r->nd].p, G2H(A2), (size_t)A3);
        r->dgram[r->nd].n = (uint32_t)A3;
        r->dgram[r->nd].from = k->port;
        r->nd++;

    }
    RET(A3);
}
HLE(recvfrom)
{
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    if (k->type != 2) { RET((uint64_t)do_recv(c, A1, G2H(A2), (uint32_t)A3, (A4 & 2) != 0)); return; }
    while (!k->nd) {
        if (k->nonblock) { err(E_WOULDBLOCK); RET((uint64_t)-1); return; }
        sched_sleep(c, 1);
        k = get(A1);
        if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    }
    uint32_t n = k->dgram[0].n < (uint32_t)A3 ? k->dgram[0].n : (uint32_t)A3;
    memcpy(G2H(A2), k->dgram[0].p, n);
    put_addr(k, ARG(5), ARG(6), k->dgram[0].from);
    if (!(A4 & 2)) { free(k->dgram[0].p); memmove(k->dgram, k->dgram + 1, sizeof k->dgram[0] * (size_t)--k->nd); }
    RET(n);
}

HLE(shutdown)
{
    NL("shutdown %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    uint32_t how = (uint32_t)A2;
    if (how == 0 || how == 2) k->shut_rd = 1;
    if (how == 1 || how == 2) { k->shut_wr = 1; if (k->peer >= 0) socks[k->peer].peer_closed = 1; }
    RET(0);
}

HLE(ioctlsocket)
{
    NL("ioctlsocket %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    uint32_t cmd = (uint32_t)A2;
    if (cmd == 0x8004667E) k->nonblock = A3 && LD32(A3) != 0;                                     /* FIONBIO */
    else if (cmd == 0x4004667F && A3) ST32(A3, k->bridge ? (pull(k), k->len) : k->type == 2 ? (k->nd ? k->dgram[0].n : 0) : k->len); /* FIONREAD */
    RET(0);
}

HLE(setsockopt) { NL("setsockopt %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3); if (!get(A1)) { err(E_NOTSOCK); RET((uint64_t)-1); return; } RET(0); }
HLE(getsockopt)
{
    NL("getsockopt %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    uint64_t val = A4, lenp = ARG(5);
    uint32_t v = 0;
    if ((uint32_t)A2 == 0xFFFF && (uint32_t)A3 == 0x1008) v = (uint32_t)k->type;   /* SO_TYPE */
    else if ((uint32_t)A2 == 0xFFFF && ((uint32_t)A3 == 0x1001 || (uint32_t)A3 == 0x1002)) v = BUFSZ;
    if (val) { uint32_t cap = lenp ? (uint32_t)LD32(lenp) : 4; if (cap >= 4) ST32(val, v); else if (cap) ST8(val, (uint8_t)v); }
    if (lenp) ST32(lenp, 4);
    RET(0);
}

/* fd_set: u_int count, then SOCKETs from +8 */
static int set_has(uint64_t set, uint64_t s) { if (!set) return 0; uint32_t n = (uint32_t)LD32(set); for (uint32_t i = 0; i < n && i < 64; i++) if (LD64(set + 8 + 8ull * i) == s) return 1; return 0; }
HLE(__WSAFDIsSet) { RET(set_has(A2, A1)); }

static int filter(uint64_t set, int want)
{
    if (!set) return 0;
    uint32_t n = (uint32_t)LD32(set), o = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        uint64_t s = LD64(set + 8 + 8ull * i);
        Sock* k = get(s);
        int ok = k && (want == 0 ? readable(k) : want == 1 ? (k->connected || k->type == 2) && !k->shut_wr : 0);
        if (ok) ST64(set + 8 + 8ull * o++, s);
    }
    ST32(set, o);
    return (int)o;
}
static int ready(uint64_t set, int want)
{
    if (!set) return 0;
    uint32_t n = (uint32_t)LD32(set);
    for (uint32_t i = 0; i < n && i < 64; i++) {
        Sock* k = get(LD64(set + 8 + 8ull * i));
        if (k && (want == 0 ? readable(k) : (k->connected || k->type == 2) && !k->shut_wr)) return 1;
    }
    return 0;
}
HLE(select)
{
    NL("select %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    uint64_t rd = A2, wr = A3, ex = A4, tv = ARG(5);
    uint64_t limit = tv ? (uint64_t)LD32(tv) * 1000000000ull + (uint64_t)LD32(tv + 4) * 1000ull : ~0ull, t0 = host_ticks_ns();
    while (!ready(rd, 0) && !ready(wr, 1)) {
        if (host_ticks_ns() - t0 >= limit) { if (rd) ST32(rd, 0); if (wr) ST32(wr, 0); if (ex) ST32(ex, 0); RET(0); return; }
        sched_sleep(c, 1);
    }
    int n = filter(rd, 0) + filter(wr, 1);
    if (ex) ST32(ex, 0);
    RET((uint64_t)n);
}

/* WSAPOLLFD: SOCKET fd, short events, short revents (16 bytes) */
HLE(WSAPoll)
{
    NL("WSAPoll %llx %llx %llx %llx", (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3, (unsigned long long)A4);
    uint64_t fds = A1;
    uint32_t n = (uint32_t)A2;
    int32_t timeout = (int32_t)A3;
    uint64_t t0 = host_ticks_ns();
    for (;;) {
        int hits = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t e = fds + 16ull * i;
            Sock* k = get(LD64(e));
            uint16_t want = (uint16_t)LD16(e + 8), got = 0;
            if (!k) got = 0x4;                                                          /* POLLNVAL */
            else {
                if ((want & 0x300) && readable(k)) got |= (uint16_t)(want & 0x300);
                if ((want & 0x10) && (k->connected || k->type == 2) && !k->shut_wr) got |= 0x10;
                if (k->peer_closed && k->type == 1 && !k->listening) got |= 0x2;      /* POLLHUP */
            }
            ST16(e + 10, got);
            hits += got != 0;
        }
        if (hits || timeout == 0 || (timeout > 0 && host_ticks_ns() - t0 >= (uint64_t)timeout * 1000000ull)) { RET((uint64_t)hits); return; }
        sched_sleep(c, 1);
    }
}

/* ---- names and addresses: numbers and localhost resolve, other names don't (offline) ---- */

HLE(htons) { RET((uint16_t)(((A1 & 0xFF) << 8) | ((A1 >> 8) & 0xFF))); }
HLE(ntohs) { hle_htons(c); }
HLE(htonl) { RET(BSWAP32(A1)); }
HLE(ntohl) { RET(BSWAP32(A1)); }

static int parse_v4(const char* s, uint8_t out[4])
{
    unsigned a, b, cc, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &cc, &d, &tail) != 4 || a > 255 || b > 255 || cc > 255 || d > 255) return 0;
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)cc; out[3] = (uint8_t)d;
    return 1;
}
static void read_a(uint64_t p, char* out, size_t cap) { size_t i = 0; for (; p && i + 1 < cap; i++) { out[i] = (char)LD8(p + i); if (!out[i]) return; } out[i] = 0; }
static void read_w(uint64_t p, char* out, size_t cap) { size_t i = 0; for (; p && i + 1 < cap; i++) { uint16_t ch = (uint16_t)LD16(p + 2 * i); out[i] = (char)(ch < 128 ? ch : '?'); if (!ch) return; } out[i] = 0; }

HLE(inet_addr)
{
    char s[64]; uint8_t a[4];
    read_a(A1, s, sizeof s);
    if (!parse_v4(s, a)) { RET(0xFFFFFFFF); return; }
    uint32_t v; memcpy(&v, a, 4);
    RET(v);
}
static int pton(uint32_t af, const char* s, uint64_t dst)
{
    if (af == 2) { uint8_t a[4]; if (!parse_v4(s, a)) return 0; memcpy(G2H(dst), a, 4); return 1; }
    if (af == 23 && (!strcmp(s, "::1") || !strcmp(s, "::"))) { memset(G2H(dst), 0, 16); if (s[2] == '1') ST8(dst + 15, 1); return 1; }
    return 0;
}
HLE(inet_pton) { char s[64]; read_a(A2, s, sizeof s); RET((uint64_t)pton((uint32_t)A1, s, A3)); }
HLE(InetPtonW) { char s[64]; read_w(A2, s, sizeof s); RET((uint64_t)pton((uint32_t)A1, s, A3)); }
static int ntop(uint32_t af, uint64_t src, char* out, size_t cap)
{
    if (af == 2) { snprintf(out, cap, "%u.%u.%u.%u", (unsigned)LD8(src), (unsigned)LD8(src + 1), (unsigned)LD8(src + 2), (unsigned)LD8(src + 3)); return 1; }
    if (af == 23) { snprintf(out, cap, LD8(src + 15) == 1 ? "::1" : "::"); return 1; }
    return 0;
}
HLE(inet_ntop)
{
    char s[64];
    if (!ntop((uint32_t)A1, A2, s, sizeof s) || strlen(s) + 1 > A4) { err(E_INVAL); RET(0); return; }
    memcpy(G2H(A3), s, strlen(s) + 1);
    RET(A3);
}
HLE(InetNtopW)
{
    char s[64];
    if (!ntop((uint32_t)A1, A2, s, sizeof s) || strlen(s) + 1 > A4) { err(E_INVAL); RET(0); return; }
    for (size_t i = 0; i <= strlen(s); i++) ST16(A3 + 2 * i, (uint8_t)s[i]);
    RET(A3);
}
static uint64_t ntoa_buf;
HLE(inet_ntoa)
{
    if (!ntoa_buf) ntoa_buf = g_alloc(32);
    uint32_t v = (uint32_t)A1;
    snprintf((char*)G2H(ntoa_buf), 32, "%u.%u.%u.%u", v & 255, (v >> 8) & 255, (v >> 16) & 255, v >> 24);
    RET(ntoa_buf);
}
HLE(gethostname) { snprintf((char*)G2H(A1), (size_t)A2, "localhost"); RET(0); }

/* ADDRINFO (both widths): flags, family, socktype, protocol, addrlen (8), canonname, addr, next: 48 bytes */
static uint32_t resolve(const char* node, const char* svc, uint64_t hints, uint64_t res)
{
    uint8_t a[4] = { 127, 0, 0, 1 };
    int fam = hints ? (int)LD32(hints + 4) : 0, st = hints ? (int)LD32(hints + 8) : 0, pr = hints ? (int)LD32(hints + 12) : 0;
    int flags = hints ? (int)LD32(hints) : 0;
    if (node && *node) {
        int route = parse_v4(node, a) || !strcmp(node, "localhost") || !strcmp(node, "::1") ? 0 : net_claim(node);
        if (route) { a[0] = 127; a[1] = 1; a[2] = (uint8_t)(route >> 8); a[3] = (uint8_t)route; }
        else if (!parse_v4(node, a) && strcmp(node, "localhost") && strcmp(node, "::1")) return 11001;   /* WSAHOST_NOT_FOUND */
    } else if (flags & 1) memset(a, 0, 4);                                                        /* AI_PASSIVE */
    if (fam == 23) return 11001;
    unsigned port = svc && *svc ? (unsigned)strtoul(svc, NULL, 10) : 0;
    uint64_t ai = g_calloc(48 + 16);
    uint64_t sa = ai + 48;
    ST32(ai + 4, 2); ST32(ai + 8, (uint32_t)st); ST32(ai + 12, (uint32_t)pr); ST64(ai + 16, 16); ST64(ai + 32, sa);
    ST16(sa, 2); ST8(sa + 2, (uint8_t)(port >> 8)); ST8(sa + 3, (uint8_t)port); memcpy(G2H(sa + 4), a, 4);
    ST64(res, ai);
    return 0;
}
HLE(getaddrinfo)
{
    char node[256], svc[64];
    read_a(A1, node, sizeof node); read_a(A2, svc, sizeof svc);
    if (!A4) { RET(E_INVAL); return; }
    ST64(A4, 0);
    uint32_t r = resolve(A1 ? node : NULL, A2 ? svc : NULL, A3, A4);
    err(r);
    RET(r);
}
HLE(GetAddrInfoW)
{
    char node[256], svc[64];
    read_w(A1, node, sizeof node); read_w(A2, svc, sizeof svc);
    if (!A4) { RET(E_INVAL); return; }
    ST64(A4, 0);
    uint32_t r = resolve(A1 ? node : NULL, A2 ? svc : NULL, A3, A4);
    err(r);
    RET(r);
}
static void free_ai(uint64_t ai) { while (ai) { uint64_t n = LD64(ai + 40); g_free(ai); ai = n; } }
HLE(freeaddrinfo) { free_ai(A1); RET(0); }
HLE(FreeAddrInfoW) { free_ai(A1); RET(0); }
HLE(getnameinfo)
{
    if (A3 && A4) snprintf((char*)G2H(A3), (size_t)A4, "%u.%u.%u.%u", (unsigned)LD8(A1 + 4), (unsigned)LD8(A1 + 5), (unsigned)LD8(A1 + 6), (unsigned)LD8(A1 + 7));
    if (ARG(5) && ARG(6)) snprintf((char*)G2H(ARG(5)), (size_t)ARG(6), "%u", (unsigned)sa_port(A1));
    RET(0);
}
STUB(gethostbyname, 0)
STUB(gethostbyaddr, 0)
STUB(getservbyname, 0)
STUB(getprotobyname, 0)

/* WSA events: their own objects, signalled by WSASetEvent or by a socket selected onto them becoming ready */
enum { NEV = 64, EV_BASE = 0x6000 };
static struct { int used, set; } evs[NEV];

static int ev_of(uint64_t h) { uint64_t i = h - EV_BASE; return h >= EV_BASE && i < NEV && evs[i].used ? (int)i : -1; }
HLE(WSACreateEvent)
{
    for (int i = 0; i < NEV; i++) if (!evs[i].used) { evs[i].used = 1; evs[i].set = 0; RET(EV_BASE + (uint64_t)i); return; }
    err(10024);
    RET(0);
}
HLE(WSACloseEvent) { int e = ev_of(A1); if (e < 0) { RET(0); return; } evs[e].used = 0; for (int i = 0; i < NSOCK; i++) if (sel[i].ev == e + 1) sel[i].ev = 0; RET(1); }
HLE(WSASetEvent) { int e = ev_of(A1); if (e >= 0) evs[e].set = 1; RET(e >= 0); }
HLE(WSAResetEvent) { int e = ev_of(A1); if (e >= 0) evs[e].set = 0; RET(e >= 0); }

/* FD_READ 1, FD_WRITE 2, FD_ACCEPT 8, FD_CONNECT 0x10, FD_CLOSE 0x20 */
static uint32_t net_events(Sock* k)
{
    uint32_t m = 0;
    if (k->listening) return k->nq ? 8 : 0;
    if (k->type == 2) return (k->nd ? 1 : 0) | 2;
    if (k->len) m |= 1;
    if (k->connected && !k->shut_wr) m |= 2 | 0x10;
    if (k->peer_closed) m |= 0x20;
    return m;
}
static int ev_ready(int e)
{
    if (evs[e].set) return 1;
    for (int i = 0; i < NSOCK; i++)
        if (sel[i].ev == e + 1 && socks[i].used) {
            /* write and connect are edge events: reported once until something changes */
            uint32_t now = net_events(&socks[i]) & sel[i].mask, fresh = now & ~(sel[i].seen & (2 | 0x10));
            if (fresh) return 1;
        }
    return 0;
}
HLE(WSAEventSelect)
{
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    int i = idx(k), e = ev_of(A2);
    sel[i].ev = e + 1; sel[i].mask = (uint32_t)A3; sel[i].seen = 0;
    k->nonblock = 1;
    RET(0);
}
HLE(WSAEnumNetworkEvents)
{
    Sock* k = get(A1);
    if (!k) { err(E_NOTSOCK); RET((uint64_t)-1); return; }
    int i = idx(k), e = ev_of(A2);
    uint32_t now = net_events(k) & sel[i].mask, out = now & ~(sel[i].seen & (2 | 0x10));
    sel[i].seen = now;
    if (A3) { memset(G2H(A3), 0, 44); ST32(A3, out); }
    if (e >= 0) evs[e].set = 0;
    RET(0);
}
HLE(WSAWaitForMultipleEvents)
{
    uint32_t n = (uint32_t)A1, all = (uint32_t)A3, ms = (uint32_t)A4;
    uint64_t t0 = host_ticks_ns();
    for (;;) {
        uint32_t hit = 0xFFFFFFFF, count = 0;
        for (uint32_t i = 0; i < n; i++) {
            int e = ev_of(LD64(A2 + 8ull * i));
            if (e >= 0 && ev_ready(e)) { count++; if (hit == 0xFFFFFFFF) hit = i; }
        }
        if (all ? count == n : count > 0) { RET(hit); return; }
        if (ms != 0xFFFFFFFF && host_ticks_ns() - t0 >= (uint64_t)ms * 1000000ull) { RET(0x102); return; }
        sched_sleep(c, 1);
    }
}

STUB(WSAAddressToStringW, 0xFFFFFFFF)
STUB(WSAStringToAddressW, 0xFFFFFFFF)
