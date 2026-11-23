#include "host.h"
#include "sched.h"
#include "steam_ifaces.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* steam_api. Two modes, picked when the game was built (--steam):
   - webworks (the default): Steam runs on a WebWorks server (runtime/web/uew_webworks.js). SteamAPI_Init signs the
     player in as a guest; every interface exists with the method layout of the version the game asks for
     (steam_ifaces.h, from the SDK headers), and lobbies, lobby chat and data, P2P packets and rich presence go
     through the server. Steam callbacks and call results are delivered from SteamAPI_RunCallbacks, as Steam does.
     When no server answers, the game gets the offline Steam below instead.
   - offline: games that use the SteamInternal_* interfaces (UE4 and later) run as if Steam weren't there; older ones
     (the classic accessors) get an offline Steam.
   RestartAppIfNecessary must say no, or the game quits to relaunch itself through Steam. */
static int is_classic(void);
static int ww_active(CPU* c);
static uint64_t ww_iface(const char* version);
static uint64_t ww_context_init(CPU* c, uint64_t data);
static int ww_on = -1;   /* 1 once signed in to a WebWorks server */
static int scm_on;       /* --steam account: the player's own Steam account, signed in by the page (uew_steamcm.js) */

#ifdef __EMSCRIPTEN__
int ww_init(int appid);
int ww_state(void);
int ww_self(char* buf, int cap);
int ww_name(const char* id, char* buf, int cap);
int ww_req_status(int id);
int ww_req_lobby(int id, int i, char* buf, int cap);
int ww_req_count(int id);
void ww_req_free(int id);
int ww_create_lobby(int type, int max);
int ww_join_lobby(const char* id);
int ww_lobby_list(const char* json);
int ww_lobby_refresh(const char* id);
void ww_lobby_leave(const char* id);
void ww_lobby_set(int what, const char* id, const char* key, const char* value);
void ww_lobby_setting(int what, const char* id, int val, const char* sval);
int ww_lobby_int(int what, const char* id);
int ww_lobby_str(int what, const char* id, int i, char* buf, int cap);
int ww_lobby_get(const char* id, const char* member, const char* key, char* buf, int cap);
int ww_chat_send(const char* id, const void* data, int len);
int ww_chat_get(const char* id, int chat, void* buf, int cap, char* who, int whocap);
int ww_p2p_send(const char* to, const void* data, int len, int channel);
int ww_p2p_peek(int channel);
int ww_p2p_read(int channel, void* buf, int cap, char* who, int whocap);
void ww_p2p_session(int what, const char* peer);
void ww_presence(const char* key, const char* value);
int ww_event(char* buf);
int ww_iface_version(const char* family, char* buf, int cap);
int scm_start(int appid);
int scm_state(void);
int scm_ticket(int appid, int type, const char* identity, void* buf, int cap, uint32_t* crc);
int scm_ticket_async(int appid, int type, const char* identity);
int scm_eticket_async(int appid, const void* data, int len);
int scm_req_status(int id);
int scm_req_take(int id, void* buf, int cap, uint32_t* crc);
void scm_req_free(int id);
void scm_cancel(uint32_t crc);
#else
static int scm_start(int appid) { (void)appid; return 0; }
static int scm_state(void) { return -1; }
static int scm_ticket(int a, int t, const char* i, void* b, int n, uint32_t* c) { (void)a; (void)t; (void)i; (void)b; (void)n; (void)c; return 0; }
static int scm_ticket_async(int a, int t, const char* i) { (void)a; (void)t; (void)i; return 0; }
static int scm_eticket_async(int a, const void* d, int n) { (void)a; (void)d; (void)n; return 0; }
static int scm_req_status(int i) { (void)i; return 2; }
static int scm_req_take(int i, void* b, int n, uint32_t* c) { (void)i; (void)b; (void)n; (void)c; return 0; }
static void scm_req_free(int i) { (void)i; }
static void scm_cancel(uint32_t c) { (void)c; }
static int ww_init(int appid) { (void)appid; return 0; }
static int ww_state(void) { return -1; }
static int ww_self(char* b, int n) { if (n) b[0] = 0; return 0; }
static int ww_name(const char* i, char* b, int n) { (void)i; if (n) b[0] = 0; return 0; }
static int ww_req_status(int i) { (void)i; return 2; }
static int ww_req_lobby(int i, int k, char* b, int n) { (void)i; (void)k; if (n) b[0] = 0; return 0; }
static int ww_req_count(int i) { (void)i; return 0; }
static void ww_req_free(int i) { (void)i; }
static int ww_create_lobby(int t, int m) { (void)t; (void)m; return 0; }
static int ww_join_lobby(const char* i) { (void)i; return 0; }
static int ww_lobby_list(const char* j) { (void)j; return 0; }
static int ww_lobby_refresh(const char* i) { (void)i; return 0; }
static void ww_lobby_leave(const char* i) { (void)i; }
static void ww_lobby_set(int w, const char* i, const char* k, const char* v) { (void)w; (void)i; (void)k; (void)v; }
static void ww_lobby_setting(int w, const char* i, int v, const char* s) { (void)w; (void)i; (void)v; (void)s; }
static int ww_lobby_int(int w, const char* i) { (void)w; (void)i; return 0; }
static int ww_lobby_str(int w, const char* i, int k, char* b, int n) { (void)w; (void)i; (void)k; if (n) b[0] = 0; return 0; }
static int ww_lobby_get(const char* i, const char* m, const char* k, char* b, int n) { (void)i; (void)m; (void)k; (void)b; (void)n; return -1; }
static int ww_chat_send(const char* i, const void* d, int l) { (void)i; (void)d; (void)l; return 0; }
static int ww_chat_get(const char* i, int c, void* b, int n, char* w, int wn) { (void)i; (void)c; (void)b; (void)n; (void)w; (void)wn; return -1; }
static int ww_p2p_send(const char* t, const void* d, int l, int c) { (void)t; (void)d; (void)l; (void)c; return 0; }
static int ww_p2p_peek(int c) { (void)c; return -1; }
static int ww_p2p_read(int c, void* b, int n, char* w, int wn) { (void)c; (void)b; (void)n; (void)w; (void)wn; return -1; }
static void ww_p2p_session(int w, const char* p) { (void)w; (void)p; }
static void ww_presence(const char* k, const char* v) { (void)k; (void)v; }
static int ww_event(char* b) { (void)b; return 0; }
static int ww_iface_version(const char* f, char* b, int n) { (void)f; if (n) b[0] = 0; return 0; }
#endif

static int dbg_steam(void)
{
    static int dbg = -1;
    if (dbg < 0) dbg = host_query_int("steamdebug");
    return dbg;
}

HLE(SteamAPI_RestartAppIfNecessary) { RET(0); }
HLE(SteamAPI_Init) { RET(ww_active(c) || is_classic()); }
HLE(SteamAPI_InitSafe) { RET(ww_active(c) || is_classic()); }
HLE(SteamAPI_InitFlat) { RET(ww_active(c) ? 0 : 1); }
HLE(SteamAPI_Shutdown) { RET(0); }
HLE(SteamAPI_GetHSteamUser) { RET(ww_active(c) || is_classic()); }
HLE(SteamAPI_GetHSteamPipe) { RET(ww_active(c) || is_classic()); }
HLE(SteamAPI_IsSteamRunning) { RET(ww_active(c) || is_classic()); }
HLE(SteamInternal_ContextInit) { RET(ww_active(c) ? ww_context_init(c, A1) : 0); }
HLE(SteamInternal_CreateInterface) { RET(ww_active(c) && A1 ? ww_iface(GSTR(A1)) : 0); }
HLE(SteamInternal_FindOrCreateUserInterface) { RET(ww_active(c) && A2 ? ww_iface(GSTR(A2)) : 0); }
HLE(SteamInternal_FindOrCreateGameServerInterface) { RET(0); }
HLE(SteamInternal_GameServer_Init) { RET(0); }
HLE(SteamGameServer_Shutdown) { RET(0); }
HLE(SteamGameServer_RunCallbacks) { RET(0); }
HLE(SteamGameServer_GetHSteamUser) { RET(0); }
HLE(SteamGameServer_GetHSteamPipe) { RET(0); }
/* SDK 1.58+: ESteamAPIInitResult (0 OK, 1 failed), with the reason in a 1024-byte buffer */
static void init_failed(CPU* c, uint64_t msg)
{
    static const char why[] = "Steam is not available in the browser";
    if (msg) memcpy(G2H(msg), why, sizeof why);
    RET(1);
}
HLE(SteamInternal_SteamAPI_Init) { if (ww_active(c)) { RET(0); return; } init_failed(c, A2); }
HLE(SteamInternal_GameServer_Init_V2) { init_failed(c, ARG(7)); }

/* ---- offline Steam for the classic accessor API (SteamUser(), SteamFriends()... the UE3 era) ----
   Games built on it expect Steam: they create their interfaces only when SteamAPI_Init succeeds, and some code uses
   them unchecked. So for these games Steam starts, with a user who is logged off: every interface exists, and its
   methods answer as an offline Steam would (no friends, no cloud files, no stats, subscribed to the app and its DLC,
   English). The interfaces' layouts differ between SDK versions, so the methods aren't modelled one by one: every slot
   returns 0, and on x86 pops what its call site pushed (rt_call_args, from the lifter). The few slots that have held
   the same method in every SDK version answer properly. ?steamdebug=1 logs each method used. */
enum { IF_USER, IF_FRIENDS, IF_UTILS, IF_APPS, IF_USERSTATS, IF_REMOTE, IF_MATCHMAKING, IF_MMSERVERS, IF_NETWORKING,
       IF_SCREENSHOTS, IF_HTTP, IF_UGC, IF_COUNT, NSLOTS = 128 };
static const char* const if_names[IF_COUNT] = { "ISteamUser", "ISteamFriends", "ISteamUtils", "ISteamApps", "ISteamUserStats",
    "ISteamRemoteStorage", "ISteamMatchmaking", "ISteamMatchmakingServers", "ISteamNetworking", "ISteamScreenshots",
    "ISteamHTTP", "ISteamUGC" };
static const char* const if_families[IF_COUNT] = { "steamuser", "steamfriends", "steamutils", "steamapps", "steamuserstats",
    "steamremotestorage", "steammatchmaking", "steammatchmakingservers", "steamnetworking", "steamscreenshots", "steamhttp",
    "steamugc" };
static uint64_t objs[IF_COUNT];
static int classic = -1;

/* the game imports the accessors from steam_api (rather than only the runtime offering them) */
static int is_classic(void)
{
    if (classic < 0) {
        classic = 0;
        for (size_t i = 0; i < rt_thunk_count; i++)
            if (strcmp(rt_thunks[i].dll, "hle") && !strcmp(rt_thunks[i].name, "SteamUser")) classic = 1;
        if (classic && ww_state() < 1) host_log("[steam] offline Steam (app %u, logged off)", rt_steam_appid);
    }
    return classic;
}

static uint64_t gstr(const char* s)
{
    uint64_t p = g_calloc(strlen(s) + 1);
    memcpy(G2H(p), s, strlen(s) + 1);
    return p;
}

/* a method's own arguments (after this): x86 thiscall passes them all on the stack, Win64 after rcx */
static uint64_t sarg(CPU* c, int i)
{
    if (RT_G32) return i == 1 ? A1 : i == 2 ? A2 : i == 3 ? A3 : i == 4 ? A4 : ARG(i);
    return i == 1 ? A2 : i == 2 ? A3 : i == 3 ? A4 : ARG(i + 1);
}

static void ww_method(CPU* c, int k, int n);

static void slot_call(CPU* c, int n)
{
    uint64_t self = RT_G32 ? HLE32_ECX(c) : A1, r = 0;
    if (ww_on == 1) { ww_method(c, -1, n); return; }
    int k = 0;
    while (k < IF_COUNT && objs[k] != self) k++;
    static uint64_t player, country, english;
    if (!player) { player = gstr("Player"); country = gstr("US"); english = gstr("english"); }
    switch (k) {
    case IF_USER:
        if (n == 0) r = 1;                                  /* GetHSteamUser; BLoggedOn (1) stays false */
        if (n == 2) {                                       /* GetSteamID: a CSteamID through a hidden pointer */
            r = sarg(c, 1);
            if (r) ST64(r, 0x0110000100000001ull);
        }
        break;
    case IF_FRIENDS:
        if (n == 0) r = player;                             /* GetPersonaName */
        break;
    case IF_UTILS:
        if (n == 2) r = 1;                                  /* GetConnectedUniverse: public */
        if (n == 3) r = (uint64_t)time(NULL);               /* GetServerRealTime */
        if (n == 4) r = country;                            /* GetIPCountry */
        if (n == 8) r = 255;                                /* GetCurrentBatteryPower: on AC */
        if (n == 9) r = rt_steam_appid;                     /* GetAppID */
        break;
    case IF_APPS:
        if (n == 0 || n == 6 || n == 7) r = 1;              /* BIsSubscribed, BIsSubscribedApp, BIsDlcInstalled */
        if (n == 4 || n == 5) r = english;                  /* GetCurrentGameLanguage, GetAvailableGameLanguages */
        break;
    }
    static uint8_t said[IF_COUNT + 1][NSLOTS];
    if (dbg_steam() && !said[k][n]++)
        host_log("[steam] %s slot %d -> %llx (from %llx)", k < IF_COUNT ? if_names[k] : "unknown interface", n, (unsigned long long)r,
                 (unsigned long long)(RT_G32 ? LD32(HLE32_ESP(c)) : LD64(R(4))));
    RET(r);
}

#define S8(b) static void s##b##0(CPU* c) { slot_call(c, b##0); } static void s##b##1(CPU* c) { slot_call(c, b##1); } \
    static void s##b##2(CPU* c) { slot_call(c, b##2); } static void s##b##3(CPU* c) { slot_call(c, b##3); } \
    static void s##b##4(CPU* c) { slot_call(c, b##4); } static void s##b##5(CPU* c) { slot_call(c, b##5); } \
    static void s##b##6(CPU* c) { slot_call(c, b##6); } static void s##b##7(CPU* c) { slot_call(c, b##7); } \
    static void s##b##8(CPU* c) { slot_call(c, b##8); } static void s##b##9(CPU* c) { slot_call(c, b##9); }
static void s0(CPU* c) { slot_call(c, 0); } static void s1(CPU* c) { slot_call(c, 1); } static void s2(CPU* c) { slot_call(c, 2); }
static void s3(CPU* c) { slot_call(c, 3); } static void s4(CPU* c) { slot_call(c, 4); } static void s5(CPU* c) { slot_call(c, 5); }
static void s6(CPU* c) { slot_call(c, 6); } static void s7(CPU* c) { slot_call(c, 7); } static void s8(CPU* c) { slot_call(c, 8); }
static void s9(CPU* c) { slot_call(c, 9); }
S8(1) S8(2) S8(3) S8(4) S8(5) S8(6) S8(7) S8(8) S8(9) S8(10) S8(11)
static void s120(CPU* c) { slot_call(c, 120); } static void s121(CPU* c) { slot_call(c, 121); } static void s122(CPU* c) { slot_call(c, 122); }
static void s123(CPU* c) { slot_call(c, 123); } static void s124(CPU* c) { slot_call(c, 124); } static void s125(CPU* c) { slot_call(c, 125); }
static void s126(CPU* c) { slot_call(c, 126); } static void s127(CPU* c) { slot_call(c, 127); }
#define L10(b) s##b##0, s##b##1, s##b##2, s##b##3, s##b##4, s##b##5, s##b##6, s##b##7, s##b##8, s##b##9
static const GuestFn slot_fns[NSLOTS] = { s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, L10(1), L10(2), L10(3), L10(4), L10(5), L10(6),
                                          L10(7), L10(8), L10(9), L10(10), L10(11), s120, s121, s122, s123, s124, s125, s126, s127 };

/* x86: each slot pops its method's own argument bytes when the layout is known (pops), else what the call site pushed */
static uint64_t new_object(const char* name, const unsigned char* pops, int count)
{
    uint64_t vt = g_calloc((uint64_t)GP * NSLOTS), o = g_calloc(16);
    for (int i = 0; i < NSLOTS; i++)
        STP(vt + (uint64_t)GP * i, rt_dyn_thunk_n(slot_fns[i], name, pops && i < count ? pops[i] / 4 : RT_SITE_ARGS));
    STP(o, vt);
    return o;
}

static uint64_t iface(int k)
{
    if (ww_on == 1) {
        char v[96];
        ww_iface_version(if_families[k], v, sizeof v);
        return v[0] ? ww_iface(v) : 0;
    }
    if (!is_classic()) return 0;
    if (!objs[k]) objs[k] = new_object(if_names[k], NULL, 0);
    return objs[k];
}

HLE(SteamUser) { RET(iface(IF_USER)); }
HLE(SteamFriends) { RET(iface(IF_FRIENDS)); }
HLE(SteamUtils) { RET(iface(IF_UTILS)); }
HLE(SteamMatchmaking) { RET(iface(IF_MATCHMAKING)); }
HLE(SteamMatchmakingServers) { RET(iface(IF_MMSERVERS)); }
HLE(SteamUserStats) { RET(iface(IF_USERSTATS)); }
HLE(SteamApps) { RET(iface(IF_APPS)); }
HLE(SteamNetworking) { RET(iface(IF_NETWORKING)); }
HLE(SteamRemoteStorage) { RET(iface(IF_REMOTE)); }
HLE(SteamScreenshots) { RET(iface(IF_SCREENSHOTS)); }
HLE(SteamHTTP) { RET(iface(IF_HTTP)); }
HLE(SteamUGC) { RET(iface(IF_UGC)); }
HLE(SteamClient) { RET(ww_active(c) ? ww_iface("SteamClient017") : 0); }
/* no game server runs */
HLE(SteamGameServer) { RET(0); }

/* ============================== WebWorks ============================== */


static int ww_ready_check(GThread* t) { (void)t; return ww_state() != 0; }

/* the first Steam call signs in (at most a few seconds); without a server Steam stays offline */
static int ww_active(CPU* c)
{
    if (ww_on >= 0) return ww_on;
    scm_on = scm_start((int)rt_steam_appid);
    if (!ww_init((int)rt_steam_appid)) { ww_on = 0; return 0; }
    /* an account sign-in waits for the player (the dialog); a WebWorks guest session a few seconds at most */
    if (ww_state() == 0) sched_block_ext(c, ww_ready_check, host_ticks_ns() + (scm_on ? 3600000000000ull : 8000000000ull));
    if (scm_on && scm_state() != 1) scm_on = 0;
    ww_on = ww_state() >= 1;
    if (ww_on) {
        char me[32];
        ww_self(me, sizeof me);
        host_log(ww_state() == 1 ? "[steam] WebWorks Steam: app %u, signed in as %s" : "[steam] local Steam session (no WebWorks server): app %u, %s",
                 rt_steam_appid, me);
    }
    return ww_on;
}

static uint64_t my_id(void)
{
    static uint64_t id;
    if (!id) { char b[32]; ww_self(b, sizeof b); id = strtoull(b, NULL, 10); }
    return id;
}

static void idstr(uint64_t id, char* b) { snprintf(b, 24, "%llu", (unsigned long long)id); }

/* ---- interfaces by version ---- */

typedef struct {
    uint64_t obj;
    const SteamIfaceTable* t;
} WIface;
static WIface wif[64];
static int nwif;

static const SteamIfaceTable* table_for(const char* version, char* fam_out)
{
    char f[96];
    size_t n = 0;
    for (const char* p = version; *p && n + 1 < sizeof f; p++) f[n++] = (char)(*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    f[n] = 0;
    int ver = 0;
    while (n && f[n - 1] >= '0' && f[n - 1] <= '9') n--;
    ver = atoi(f + n);
    f[n] = 0;
    char* s;
    if ((s = strstr(f, "_interface_version"))) *s = 0;
    if ((s = strstr(f, "_interface_v"))) *s = 0;
    while (n && (f[n - 1] == '_')) f[--n] = 0;
    if (fam_out) strcpy(fam_out, f);
    for (size_t i = 0; i < sizeof steam_iface_tables / sizeof *steam_iface_tables; i++)
        if (steam_iface_tables[i].version == ver && !strcmp(steam_iface_tables[i].family, f)) return &steam_iface_tables[i];
    return NULL;
}

static uint64_t ww_iface(const char* version)
{
    char fam[96];
    const SteamIfaceTable* t = table_for(version, fam);
    if (!t) {
        host_log("[steam] no layout for interface %s", version);
        return 0;
    }
    for (int i = 0; i < nwif; i++) if (wif[i].t == t) return wif[i].obj;
    if (nwif == (int)(sizeof wif / sizeof *wif)) return 0;
    wif[nwif].t = t;
    wif[nwif].obj = new_object(t->family, t->pops, t->count);
    if (dbg_steam()) host_log("[steam] interface %s (%d methods)", version, t->count);
    return wif[nwif++].obj;
}

/* SDK 1.37+: the inline accessors keep their interfaces in a context the game owns; steam_api fills it by calling the
   game's init function whenever its counter is stale */
static uint64_t ww_context_init(CPU* c, uint64_t data)
{
    if (!data) return 0;
    if (LDP(data + GP) != 1) {
        STP(data + GP, 1);
        uint64_t a[1] = { data + 2ull * GP };
        rt_invoke(c, LDP(data), 1, a);
    }
    return data + 2ull * GP;
}

/* ---- arguments and returns ---- */

typedef struct { CPU* c; int i; } Args;
static uint64_t a_u32(Args* a) { return (uint32_t)sarg(a->c, a->i++); }
static uint64_t a_ptr(Args* a) { uint64_t v = sarg(a->c, a->i++); return RT_G32 ? (uint32_t)v : v; }
static uint64_t a_u64(Args* a)
{
    if (!RT_G32) return sarg(a->c, a->i++);
    uint64_t lo = (uint32_t)sarg(a->c, a->i), hi = (uint32_t)sarg(a->c, a->i + 1);
    a->i += 2;
    return lo | hi << 32;
}
/* 64-bit results: rt_hle32 splits R(0) into edx:eax for 32-bit callers */
static void ret64(CPU* c, uint64_t v) { RET(v); }
static void put_str(uint64_t dst, uint32_t cap, const char* s)
{
    if (!dst || !cap) return;
    size_t n = strlen(s);
    if (n + 1 > cap) n = cap - 1;
    memcpy(G2H(dst), s, n);
    ST8(dst + n, 0);
}

/* strings handed back to the game stay put until the ring comes round again (lobby data is at most 8 KB) */
static uint64_t ring_str(const char* s)
{
    static uint64_t ring[64];
    static uint32_t cap[64];
    static int k;
    size_t n = strlen(s) + 1;
    int i = k++ & 63;
    if (cap[i] < n) {
        if (ring[i]) g_free(ring[i]);
        cap[i] = (uint32_t)(n < 64 ? 64 : n);
        ring[i] = g_alloc(cap[i]);
    }
    memcpy(G2H(ring[i]), s, n);
    return ring[i];
}

/* the method's name in the table up to its overload signature */
static int is(const char* m, const char* name)
{
    size_t n = strlen(name);
    return !strncmp(m, name, n) && (m[n] == 0 || m[n] == '|');
}

/* ---- calls (SteamAPICall_t) and callbacks ---- */

enum { K_CREATE = 1, K_JOIN, K_LIST, K_IMMEDIATE, K_ETICKET };
typedef struct {
    uint64_t call;
    int cb, kind, req, done, failed, delivered;
    uint32_t size;
    uint8_t data[64];
} Call;
static Call calls[64];
static uint64_t next_call = 0x5EED00000001ull;
static int last_list = -1;

typedef struct { uint64_t obj; int id; uint64_t call; } Listener;
static Listener cbs[256];
static int ncbs;

static Call* new_call(int cb, int kind, int req)
{
    int best = 0;
    for (int i = 0; i < 64; i++) {
        if (!calls[i].call) { best = i; break; }
        if (calls[i].call < calls[best].call) best = i;
    }
    Call* k = &calls[best];
    if (k->kind == K_LIST && k->req > 0 && k->req != last_list) ww_req_free(k->req);
    memset(k, 0, sizeof *k);
    k->call = next_call++;
    k->cb = cb;
    k->kind = kind;
    k->req = req;
    return k;
}

static Call* find_call(uint64_t call)
{
    for (int i = 0; i < 64; i++) if (calls[i].call == call && call) return &calls[i];
    return NULL;
}

/* a result available at once (written into the call by the caller) */
static Call* done_call(int cb, uint32_t size)
{
    Call* k = new_call(cb, K_IMMEDIATE, 0);
    k->done = 1;
    k->size = size;
    return k;
}

static void invoke_method(CPU* c, uint64_t obj, int slot, int n, const uint64_t* args)
{
    uint64_t fn = LDP(LDP(obj) + (uint64_t)GP * slot);
    if (RT_G32) {
        uint64_t save = R(1);
        R(1) = obj;   /* thiscall */
        rt_invoke(c, fn, n, args);
        R(1) = save;
    } else {
        uint64_t a[8] = { obj };
        for (int i = 0; i < n && i < 7; i++) a[i + 1] = args[i];
        rt_invoke(c, fn, n + 1, a);
    }
}

/* CCallbackBase (MSVC): vtable Run(param, ioFailure, call), Run(param), GetCallbackSizeBytes; then flags, id */
static void post(CPU* c, int id, const void* data, uint32_t size)
{
    uint64_t buf = g_calloc(size < 8 ? 8 : size);
    memcpy(G2H(buf), data, size);
    Listener snap[256];
    int n = ncbs;
    memcpy(snap, cbs, sizeof(Listener) * n);
    for (int i = 0; i < n; i++)
        if (snap[i].id == id && !snap[i].call) {
            uint64_t a[1] = { buf };
            invoke_method(c, snap[i].obj, 1, 1, a);
        }
    g_free(buf);
    if (dbg_steam()) host_log("[steam] callback %d (%u bytes)", id, size);
}

static void finish(CPU* c, Call* k)
{
    uint64_t buf = g_calloc(k->size < 8 ? 8 : k->size);
    memcpy(G2H(buf), k->data, k->size);
    Listener snap[256];
    int n = ncbs;
    memcpy(snap, cbs, sizeof(Listener) * n);
    for (int i = 0; i < n; i++)
        if (snap[i].call == k->call) {
            for (int j = 0; j < ncbs; j++) if (cbs[j].obj == snap[i].obj && cbs[j].call == k->call) { cbs[j] = cbs[--ncbs]; break; }
            uint64_t a[3] = { buf, (uint64_t)k->failed, k->call };
            if (RT_G32) {
                uint64_t a32[4] = { buf, (uint64_t)k->failed, (uint32_t)k->call, k->call >> 32 };
                uint64_t fn = LDP(LDP(snap[i].obj));
                uint64_t save = R(1);
                R(1) = snap[i].obj;
                rt_invoke(c, fn, 4, a32);
                R(1) = save;
            } else invoke_method(c, snap[i].obj, 0, 3, a);
        }
    g_free(buf);
    k->delivered = 1;
}

/* packed as the SDK's Windows structs (8-byte packing) */
static void w64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }
static void w32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }

static void lobby_enter(CPU* c, uint64_t lobby, int ok)
{
    uint8_t d[24] = { 0 };
    w64(d, lobby);
    w32(d + 16, ok ? 1 : 2);   /* EChatRoomEnterResponse: success, doesn't exist */
    post(c, 504, d, 24);
}

static void lobby_data_update(CPU* c, uint64_t lobby, uint64_t member, int ok)
{
    uint8_t d[24] = { 0 };
    w64(d, lobby);
    w64(d + 8, member);
    d[16] = (uint8_t)ok;
    post(c, 505, d, 24);
}

/* --steam account: the last encrypted app ticket Steam gave, and web API tickets being made */
static uint8_t eticket[2048];
static uint32_t eticket_len;
static struct { int req; uint32_t handle; } wtix[16];
static int nwtix;
static struct { uint32_t handle, crc; } stix[32];
static int nstix;
static uint32_t next_ticket = 0x100;

static void poll_tickets(CPU* c)
{
    for (int i = 0; i < nwtix; i++) {
        int st = scm_req_status(wtix[i].req);
        if (!st) continue;
        static uint8_t d[12 + 2560];
        memset(d, 0, sizeof d);
        uint32_t crc = 0;
        int n = st == 1 ? scm_req_take(wtix[i].req, d + 12, 2560, &crc) : 0;
        w32(d, wtix[i].handle); w32(d + 4, n > 0 ? 1 : 2); w32(d + 8, (uint32_t)(n > 0 ? n : 0));   /* GetTicketForWebApiResponse_t */
        post(c, 168, d, sizeof d);
        if (n > 0 && nstix < 32) { stix[nstix].handle = wtix[i].handle; stix[nstix++].crc = crc; }
        if (dbg_steam()) host_log("[steam] web API ticket %u: %d bytes", wtix[i].handle, n);
        scm_req_free(wtix[i].req);
        wtix[i--] = wtix[--nwtix];
    }
}

static void poll_calls(CPU* c)
{
    for (int i = 0; i < 64; i++) {
        Call* k = &calls[i];
        if (!k->call || k->done || k->kind == K_IMMEDIATE) continue;
        if (k->kind == K_ETICKET) {
            int st = scm_req_status(k->req);
            if (!st) continue;
            k->done = 1;
            eticket_len = st == 1 ? (uint32_t)scm_req_take(k->req, eticket, sizeof eticket, NULL) : 0;
            if (eticket_len > sizeof eticket) eticket_len = 0;
            w32(k->data, eticket_len ? 1 : 2);   /* EncryptedAppTicketResponse_t */
            k->size = 4;
            scm_req_free(k->req);
            continue;
        }
        int st = ww_req_status(k->req);
        if (!st) continue;
        k->done = 1;
        k->failed = 0;
        char b[32];
        uint64_t lobby = 0;
        if (k->kind != K_LIST) { ww_req_lobby(k->req, 0, b, sizeof b); lobby = st == 1 ? strtoull(b, NULL, 10) : 0; }
        switch (k->kind) {
        case K_CREATE:   /* LobbyCreated_t, then the lobby is entered */
            w32(k->data, st == 1 ? 1 : 2);
            w64(k->data + 8, lobby);
            k->size = 16;
            if (lobby) { lobby_enter(c, lobby, 1); lobby_data_update(c, lobby, lobby, 1); }
            break;
        case K_JOIN:     /* LobbyEnter_t */
            w64(k->data, lobby);
            w32(k->data + 16, st == 1 ? 1 : 2);
            k->size = 24;
            if (lobby) { post(c, 504, k->data, 24); lobby_data_update(c, lobby, lobby, 1); }
            break;
        case K_LIST:     /* LobbyMatchList_t */
            w32(k->data, st == 1 ? (uint32_t)ww_req_count(k->req) : 0);
            k->size = 4;
            if (last_list > 0 && last_list != k->req) ww_req_free(last_list);
            last_list = k->req;
            break;
        }
        if (k->kind != K_LIST) ww_req_free(k->req);
        if (dbg_steam()) host_log("[steam] call %llx (callback %d) %s", (unsigned long long)k->call, k->cb, st == 1 ? "done" : "failed");
    }
}

static void drain_events(CPU* c)
{
    char f[192];
    int ty;
    while ((ty = ww_event(f))) {
        uint64_t a = strtoull(f, NULL, 10), b = strtoull(f + 64, NULL, 10), x = strtoull(f + 128, NULL, 10);
        uint8_t d[32] = { 0 };
        switch (ty) {
        case 1: case 2:   /* LobbyChatUpdate_t: entered, or left (2) / disconnected (4) / kicked (8) */
            w64(d, a); w64(d + 8, b); w64(d + 16, b); w32(d + 24, ty == 1 ? 1 : (uint32_t)x);
            post(c, 506, d, 32);
            break;
        case 3: lobby_data_update(c, a, b, 1); break;
        case 4: lobby_data_update(c, a, a, 0); break;
        case 5:           /* LobbyChatMsg_t */
            w64(d, a); w64(d + 8, b); d[16] = 1; w32(d + 20, (uint32_t)x);
            post(c, 507, d, 24);
            break;
        case 6: w64(d, a); post(c, 1202, d, 8); break;                           /* P2PSessionRequest_t */
        case 7: w64(d, a); d[8] = (uint8_t)b; post(c, 1203, d, 16); break;       /* P2PSessionConnectFail_t */
        case 8: w64(d, a); w64(d + 8, b); post(c, 333, d, 16); break;            /* GameLobbyJoinRequested_t */
        case 9: w64(d, a); w32(d + 8, 1); post(c, 304, d, 16); break;            /* PersonaStateChange_t: name */
        }
    }
}

/* immediate results queued by methods (stats received, tickets) wait for the next RunCallbacks */
static struct { int id; uint32_t size; uint8_t d[2576]; } pending[32];
static int npending;
static void queue_post(int id, const void* d, uint32_t size)
{
    if (npending == 32) return;
    pending[npending].id = id;
    pending[npending].size = size;
    memcpy(pending[npending].d, d, size);
    npending++;
}

HLE(SteamAPI_RunCallbacks)
{
    if (ww_on != 1) { RET(0); return; }
    static int busy;
    if (busy) { RET(0); return; }
    busy = 1;
    int n = npending;
    for (int i = 0; i < n; i++) post(c, pending[i].id, pending[i].d, pending[i].size);
    memmove(pending, pending + n, sizeof pending[0] * (npending - n));
    npending -= n;
    poll_calls(c);
    if (scm_on) poll_tickets(c);
    for (int i = 0; i < 64; i++) if (calls[i].call && calls[i].done && !calls[i].delivered) finish(c, &calls[i]);
    drain_events(c);
    busy = 0;
    RET(0);
}

/* CCallback/CCallResult register themselves: flags at GP (1 = registered), callback id after it */
HLE(SteamAPI_RegisterCallback)
{
    uint64_t o = A1;
    int id = (int)A2;
    if (!o) return;
    ST8(o + GP, LD8(o + GP) | 1);
    ST32(o + GP + 4, (uint32_t)id);
    for (int i = 0; i < ncbs; i++) if (cbs[i].obj == o && !cbs[i].call) { cbs[i].id = id; return; }
    if (ncbs < 256) cbs[ncbs++] = (Listener){ o, id, 0 };
}
HLE(SteamAPI_UnregisterCallback)
{
    uint64_t o = A1;
    if (o) ST8(o + GP, LD8(o + GP) & ~1);
    for (int i = 0; i < ncbs; i++) if (cbs[i].obj == o && !cbs[i].call) { cbs[i] = cbs[--ncbs]; break; }
}
HLE(SteamAPI_RegisterCallResult)
{
    uint64_t o = A1, call = RT_G32 ? ((uint32_t)A2 | (uint64_t)(uint32_t)A3 << 32) : A2;
    if (o && call && ncbs < 256) cbs[ncbs++] = (Listener){ o, (int)LD32(o + GP + 4), call };
}
HLE(SteamAPI_UnregisterCallResult)
{
    uint64_t o = A1, call = RT_G32 ? ((uint32_t)A2 | (uint64_t)(uint32_t)A3 << 32) : A2;
    for (int i = 0; i < ncbs; i++) if (cbs[i].obj == o && cbs[i].call == call) { cbs[i] = cbs[--ncbs]; break; }
}

/* ---- the methods ---- */

static struct { char name[64]; uint32_t bits; int ach; } stats[256];
static int nstats;
static uint32_t* stat_slot(const char* name, int ach, int make)
{
    for (int i = 0; i < nstats; i++) if (stats[i].ach == ach && !strcmp(stats[i].name, name)) return &stats[i].bits;
    if (!make || nstats == 256) return NULL;
    snprintf(stats[nstats].name, sizeof stats[nstats].name, "%s", name);
    stats[nstats].ach = ach;
    stats[nstats].bits = 0;
    return &stats[nstats++].bits;
}

/* lobby-list filters gathered for the next RequestLobbyList, as the server's JSON */
static char lf_filters[2048], lf_numeric[2048], lf_near[1024], lf_compat[32];
static int lf_slots, lf_max;

static void json_str(char* out, size_t cap, const char* s)
{
    size_t n = strlen(out);
    if (n + 2 >= cap) return;
    out[n++] = '"';
    for (; *s && n + 8 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[n++] = '\\'; out[n++] = (char)ch; }
        else if (ch < 0x20) n += snprintf(out + n, cap - n, "\\u%04x", ch);
        else out[n++] = (char)ch;
    }
    out[n++] = '"';
    out[n] = 0;
}

static void append(char* out, size_t cap, const char* s) { size_t n = strlen(out); snprintf(out + n, cap - n, "%s", s); }

static const char* cmp_op(int c) { return c == -2 ? "lte" : c == -1 ? "lt" : c == 1 ? "gt" : c == 2 ? "gte" : c == 3 ? "ne" : "eq"; }

static int has_avail_voice_api(const char* m) { return is(m, "GetAvailableVoice") || is(m, "GetVoice") || is(m, "DecompressVoice"); }

/* methods that return a CSteamID through a hidden pointer (the first argument after this) */
static int returns_id(const char* fam, const char* m)
{
    static const char* const names[] = { "GetSteamID", "GetFriendByIndex", "GetClanByIndex", "GetChatMemberByIndex", "GetClanOwner",
        "GetClanOfficerByIndex", "GetCoplayFriend", "GetFriendFromSourceByIndex", "GetLobbyByIndex", "GetLobbyMemberByIndex",
        "GetLobbyOwner", "GetAppOwner", "GetLeaderboardEntryUser", NULL };
    (void)fam;
    for (int i = 0; names[i]; i++) if (is(m, names[i])) return 1;
    return 0;
}

static void ww_method(CPU* c, int unused, int n)
{
    (void)unused;
    uint64_t self = RT_G32 ? HLE32_ECX(c) : A1;
    const SteamIfaceTable* t = NULL;
    for (int i = 0; i < nwif; i++) if (wif[i].obj == self) { t = wif[i].t; break; }
    const char* m = t && n < t->count ? t->slots[n] : "";
    const char* fam = t ? t->family : "";
    Args a = { c, 1 };
    uint64_t r = 0;
    char s1[64], s2[64], buf[8192];
    int handled = 1;

    if (returns_id(fam, m)) {
        uint64_t out = a_ptr(&a), id = 0;
        if (is(m, "GetSteamID") || is(m, "GetAppOwner")) id = my_id();
        else if (is(m, "GetLobbyByIndex")) {
            uint32_t i = (uint32_t)a_u32(&a);
            if (last_list > 0) { ww_req_lobby(last_list, (int)i, s1, sizeof s1); id = strtoull(s1, NULL, 10); }
        } else if (is(m, "GetLobbyMemberByIndex")) {
            idstr(a_u64(&a), s1);
            ww_lobby_str(1, s1, (int)a_u32(&a), s2, sizeof s2);
            id = strtoull(s2, NULL, 10);
        } else if (is(m, "GetLobbyOwner")) {
            idstr(a_u64(&a), s1);
            ww_lobby_str(0, s1, 0, s2, sizeof s2);
            id = strtoull(s2, NULL, 10);
        }
        if (out) ST64(out, id);
        RET(out);
        goto log;
    }

    if (!strcmp(fam, "steamclient")) {
        if (is(m, "CreateSteamPipe") || is(m, "ConnectToGlobalUser") || is(m, "CreateLocalUser") || is(m, "BReleaseSteamPipe")
            || is(m, "BShutdownIfAllPipesClosed")) r = 1;
        else if (!strncmp(m, "GetISteam", 9)) {
            if (!is(m, "GetISteamUtils") && !is(m, "GetISteamNetworkingUtils")) a_u32(&a);
            a_u32(&a);
            uint64_t v = a_ptr(&a);
            r = v ? ww_iface(GSTR(v)) : 0;
        } else handled = 0;
    } else if (!strcmp(fam, "steamuser")) {
        if (is(m, "GetHSteamUser") || is(m, "BLoggedOn") || is(m, "BIsPhoneVerified") || is(m, "BIsTwoFactorEnabled")) r = 1;
        else if (is(m, "GetPlayerSteamLevel")) r = 1;
        else if (is(m, "UserHasLicenseForApp") || is(m, "BeginAuthSession") || is(m, "BIsBehindNAT")) r = 0;
        else if (is(m, "GetAuthTicketForWebApi") && scm_on) {
            /* a real ticket from the Steam account, delivered by RunCallbacks once Steam has registered it */
            uint64_t ident = a_ptr(&a);
            if (nwtix < 16) {
                wtix[nwtix].req = scm_ticket_async((int)rt_steam_appid, 5, ident ? GSTR(ident) : NULL);
                wtix[nwtix++].handle = next_ticket;
            }
            r = next_ticket++;
        } else if (is(m, "GetAuthTicketForWebApi")) {
            /* GetTicketForWebApiResponse_t: ticket 2, OK, its bytes (a backend here takes any ticket) */
            static uint8_t d[12 + 2560];
            uint64_t id = my_id();
            memset(d, 0, sizeof d);
            w32(d, 2); w32(d + 4, 1); w32(d + 8, 16);
            memcpy(d + 12, &id, 8);
            memcpy(d + 20, &rt_steam_appid, 4);
            queue_post(168, d, sizeof d);
            r = 2;
        } else if (is(m, "RequestEncryptedAppTicket") && scm_on) {
            uint64_t data = a_ptr(&a);
            uint32_t len = (uint32_t)a_u32(&a);
            Call* k = new_call(154, K_ETICKET, scm_eticket_async((int)rt_steam_appid, data ? G2H(data) : NULL, (int)len));
            ret64(c, k->call);
            goto log;
        } else if (is(m, "GetEncryptedAppTicket") && scm_on) {
            uint64_t tk = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            uint64_t pcb = a_ptr(&a);
            if (pcb) ST32(pcb, eticket_len);
            if (tk && eticket_len && cap >= eticket_len) memcpy(G2H(tk), eticket, eticket_len);
            r = eticket_len && tk && cap >= eticket_len;
        } else if (is(m, "GetAuthSessionTicket") && scm_on) {
            uint64_t tk = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            uint64_t pcb = a_ptr(&a);
            uint32_t crc = 0;
            int n = tk ? scm_ticket((int)rt_steam_appid, 2, NULL, G2H(tk), (int)cap, &crc) : 0;
            if (pcb) ST32(pcb, (uint32_t)(n > 0 ? n : 0));
            uint8_t d[8] = { 0 };
            uint32_t h = n > 0 ? next_ticket++ : 0;
            w32(d, h); w32(d + 4, n > 0 ? 1 : 2);   /* GetAuthSessionTicketResponse_t */
            queue_post(163, d, 8);
            if (n > 0 && nstix < 32) { stix[nstix].handle = h; stix[nstix++].crc = crc; }
            r = h;
        } else if (is(m, "CancelAuthTicket") && scm_on) {
            uint32_t h = (uint32_t)a_u32(&a);
            for (int i = 0; i < nstix; i++) if (stix[i].handle == h) { scm_cancel(stix[i].crc); stix[i] = stix[--nstix]; break; }
            r = 0;
        } else if (is(m, "RequestEncryptedAppTicket")) {
            Call* k = done_call(154, 4);   /* EncryptedAppTicketResponse_t: OK */
            w32(k->data, 1);
            ret64(c, k->call);
            goto log;
        } else if (is(m, "GetEncryptedAppTicket")) {
            uint64_t tk = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            uint64_t pcb = a_ptr(&a), id = my_id();
            uint8_t t8[16] = { 'U', 'E', 'W', 'S' };
            memcpy(t8 + 4, &id, 8);
            memcpy(t8 + 12, &rt_steam_appid, 4);
            if (tk && cap >= sizeof t8) memcpy(G2H(tk), t8, sizeof t8);
            if (pcb) ST32(pcb, sizeof t8);
            r = tk && cap >= sizeof t8;
        } else if (is(m, "GetAuthSessionTicket")) {
            uint64_t tk = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            uint64_t pcb = a_ptr(&a);
            uint64_t id = my_id();
            if (tk && cap >= 8) memcpy(G2H(tk), &id, 8);
            if (pcb) ST32(pcb, 8);
            uint8_t d[8] = { 0 };
            w32(d, 1); w32(d + 4, 1);   /* GetAuthSessionTicketResponse_t: ticket 1, OK */
            queue_post(163, d, 8);
            r = 1;
        } else if (has_avail_voice_api(m)) {
            uint64_t p = a_ptr(&a);
            if (p) ST32(p, 0);
            r = 3;   /* k_EVoiceResultNoData */
        } else if (is(m, "GetUserDataFolder")) r = 0;
        else handled = 0;
    } else if (!strcmp(fam, "steamfriends")) {
        if (is(m, "GetPersonaName")) { ww_name("0", buf, sizeof buf); r = ring_str(buf); }
        else if (is(m, "GetPersonaState")) r = 1;
        else if (is(m, "GetFriendPersonaName")) {
            idstr(a_u64(&a), s1);
            ww_name(s1, buf, sizeof buf);
            r = ring_str(buf[0] ? buf : "Player");
        } else if (is(m, "GetFriendPersonaState")) r = 1;
        else if (is(m, "GetFriendRichPresence") || is(m, "GetFriendRichPresenceKeyByIndex")) r = ring_str("");
        else if (is(m, "GetPlayerNickname")) r = 0;
        else if (is(m, "SetRichPresence")) {
            uint64_t k = a_ptr(&a), v = a_ptr(&a);
            ww_presence(k ? GSTR(k) : "", v ? GSTR(v) : "");
            r = 1;
        } else if (is(m, "ClearRichPresence")) ww_presence(NULL, NULL);
        else if (is(m, "InviteUserToGame") || is(m, "SetListenForFriendsMessages")) r = 1;
        else if (is(m, "RequestUserInformation")) r = 0;
        else handled = 0;
    } else if (!strcmp(fam, "steamutils")) {
        if (is(m, "GetConnectedUniverse")) r = 1;
        else if (is(m, "GetServerRealTime")) r = (uint64_t)time(NULL);
        else if (is(m, "GetIPCountry")) r = ring_str("US");
        else if (is(m, "GetCurrentBatteryPower")) r = 255;
        else if (is(m, "GetAppID")) r = rt_steam_appid;
        else if (is(m, "GetSteamUILanguage")) r = ring_str("english");
        else if (is(m, "GetSecondsSinceAppActive") || is(m, "GetSecondsSinceComputerActive")) r = 60;
        else if (is(m, "IsAPICallCompleted")) {
            Call* k = find_call(a_u64(&a));
            uint64_t pf = a_ptr(&a);
            if (k && k->done && pf) ST8(pf, k->failed);
            r = k && k->done;
        } else if (is(m, "GetAPICallFailureReason")) r = (uint32_t)-1;
        else if (is(m, "GetAPICallResult")) {
            uint64_t call = a_u64(&a), dst = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            a_u32(&a);
            uint64_t pf = a_ptr(&a);
            Call* k = find_call(call);
            if (dbg_steam() > 1)
                host_log("[steam] GetAPICallResult %llx: %s, %u of %u bytes", (unsigned long long)call, k && k->done ? "done" : "pending", cap,
                         k ? k->size : 0);
            if (k && k->done) {
                if (dst) memcpy(G2H(dst), k->data, cap < k->size ? cap : k->size);
                if (pf) ST8(pf, k->failed);
                k->delivered = 1;
                r = 1;
            }
        } else handled = 0;
    } else if (!strcmp(fam, "steamapps")) {
        if (is(m, "BIsSubscribed") || is(m, "BIsSubscribedApp") || is(m, "BIsDlcInstalled") || is(m, "BIsAppInstalled")) r = 1;
        else if (is(m, "GetCurrentGameLanguage") || is(m, "GetAvailableGameLanguages")) r = ring_str("english");
        else if (is(m, "GetLaunchQueryParam")) r = ring_str("");
        else if (is(m, "GetEarliestPurchaseUnixTime")) r = 1500000000;
        else handled = 0;
    } else if (!strcmp(fam, "steammatchmaking")) {
        if (is(m, "CreateLobby")) {
            int type = (int)a_u32(&a), max = (int)a_u32(&a);
            Call* k = new_call(513, K_CREATE, ww_create_lobby(type, max));
            ret64(c, k->call);
            goto log;
        } else if (is(m, "JoinLobby")) {
            idstr(a_u64(&a), s1);
            Call* k = new_call(504, K_JOIN, ww_join_lobby(s1));
            ret64(c, k->call);
            goto log;
        } else if (is(m, "RequestLobbyList")) {
            snprintf(buf, sizeof buf, "{\"slots\":%d,\"max\":%d,\"filters\":{%s},\"numeric\":[%s],\"near\":[%s]%s%s%s}", lf_slots,
                     lf_max ? lf_max : 50, lf_filters, lf_numeric, lf_near, lf_compat[0] ? ",\"compatible\":\"" : "", lf_compat,
                     lf_compat[0] ? "\"" : "");
            lf_filters[0] = lf_numeric[0] = lf_near[0] = lf_compat[0] = 0;
            lf_slots = lf_max = 0;
            Call* k = new_call(510, K_LIST, ww_lobby_list(buf));
            ret64(c, k->call);
            goto log;
        } else if (is(m, "AddRequestLobbyListStringFilter")) {
            uint64_t k = a_ptr(&a), v = a_ptr(&a);
            if (k && v) {
                if (lf_filters[0]) append(lf_filters, sizeof lf_filters, ",");
                json_str(lf_filters, sizeof lf_filters, GSTR(k));
                append(lf_filters, sizeof lf_filters, ":");
                json_str(lf_filters, sizeof lf_filters, GSTR(v));
            }
        } else if (is(m, "AddRequestLobbyListNumericalFilter") || is(m, "AddRequestLobbyListNearValueFilter")) {
            uint64_t k = a_ptr(&a);
            int v = (int)a_u32(&a), cmp = is(m, "AddRequestLobbyListNumericalFilter") ? (int)a_u32(&a) : 0;
            char* dst = cmp || is(m, "AddRequestLobbyListNumericalFilter") ? lf_numeric : lf_near;
            size_t cap = dst == lf_numeric ? sizeof lf_numeric : sizeof lf_near;
            if (k) {
                if (dst[0]) append(dst, cap, ",");
                append(dst, cap, "{\"key\":");
                json_str(dst, cap, GSTR(k));
                snprintf(s1, sizeof s1, ",\"value\":%d", v);
                append(dst, cap, s1);
                if (dst == lf_numeric) { snprintf(s1, sizeof s1, ",\"op\":\"%s\"", cmp_op(cmp)); append(dst, cap, s1); }
                append(dst, cap, "}");
            }
        } else if (is(m, "AddRequestLobbyListFilterSlotsAvailable")) lf_slots = (int)a_u32(&a);
        else if (is(m, "AddRequestLobbyListResultCountFilter")) lf_max = (int)a_u32(&a);
        else if (is(m, "AddRequestLobbyListCompatibleMembersFilter")) idstr(a_u64(&a), lf_compat);
        else if (is(m, "AddRequestLobbyListDistanceFilter")) {}
        else if (is(m, "LeaveLobby")) { idstr(a_u64(&a), s1); ww_lobby_leave(s1); }
        else if (is(m, "InviteUserToLobby")) r = 1;
        else if (is(m, "GetNumLobbyMembers")) { idstr(a_u64(&a), s1); r = (uint32_t)ww_lobby_int(0, s1); }
        else if (is(m, "GetLobbyMemberLimit")) { idstr(a_u64(&a), s1); r = (uint32_t)ww_lobby_int(1, s1); }
        else if (is(m, "GetLobbyDataCount")) { idstr(a_u64(&a), s1); r = (uint32_t)ww_lobby_int(2, s1); }
        else if (is(m, "GetLobbyData")) {
            idstr(a_u64(&a), s1);
            uint64_t k = a_ptr(&a);
            if (!k || ww_lobby_get(s1, NULL, GSTR(k), buf, sizeof buf) < 0) buf[0] = 0;
            r = ring_str(buf);
        } else if (is(m, "GetLobbyMemberData")) {
            idstr(a_u64(&a), s1);
            idstr(a_u64(&a), s2);
            uint64_t k = a_ptr(&a);
            if (!k || ww_lobby_get(s1, s2, GSTR(k), buf, sizeof buf) < 0) buf[0] = 0;
            r = ring_str(buf);
        } else if (is(m, "SetLobbyData") || is(m, "DeleteLobbyData") || is(m, "SetLobbyMemberData")) {
            idstr(a_u64(&a), s1);
            uint64_t k = a_ptr(&a), v = is(m, "DeleteLobbyData") ? 0 : a_ptr(&a);
            if (k) ww_lobby_set(is(m, "SetLobbyData") ? 0 : is(m, "SetLobbyMemberData") ? 1 : 2, s1, GSTR(k), v ? GSTR(v) : "");
            r = 1;
        } else if (is(m, "GetLobbyDataByIndex")) {
            idstr(a_u64(&a), s1);
            int i = (int)a_u32(&a);
            uint64_t kb = a_ptr(&a);
            uint32_t kc = (uint32_t)a_u32(&a);
            uint64_t vb = a_ptr(&a);
            uint32_t vc = (uint32_t)a_u32(&a);
            if (i < ww_lobby_int(2, s1)) {
                ww_lobby_str(2, s1, i, buf, sizeof buf);
                put_str(kb, kc, buf);
                ww_lobby_str(3, s1, i, buf, sizeof buf);
                put_str(vb, vc, buf);
                r = 1;
            }
        } else if (is(m, "SendLobbyChatMsg")) {
            idstr(a_u64(&a), s1);
            uint64_t d = a_ptr(&a);
            int len = (int)a_u32(&a);
            r = d && len > 0 ? (uint64_t)ww_chat_send(s1, G2H(d), len) : 0;
        } else if (is(m, "GetLobbyChatEntry")) {
            idstr(a_u64(&a), s1);
            int chat = (int)a_u32(&a);
            uint64_t who = a_ptr(&a), d = a_ptr(&a);
            int cap = (int)a_u32(&a);
            uint64_t ty = a_ptr(&a);
            int len = d ? ww_chat_get(s1, chat, G2H(d), cap, s2, sizeof s2) : -1;
            if (len >= 0) {
                if (who) ST64(who, strtoull(s2, NULL, 10));
                if (ty) ST32(ty, 1);
                r = (uint32_t)len;
            }
        } else if (is(m, "RequestLobbyData")) { idstr(a_u64(&a), s1); r = (uint64_t)ww_lobby_refresh(s1); }
        else if (is(m, "SetLobbyMemberLimit") || is(m, "SetLobbyType") || is(m, "SetLobbyJoinable")) {
            idstr(a_u64(&a), s1);
            int v = (int)a_u32(&a);
            ww_lobby_setting(is(m, "SetLobbyType") ? 0 : is(m, "SetLobbyJoinable") ? 1 : 2, s1, v, "");
            r = 1;
        } else if (is(m, "SetLobbyOwner")) {
            idstr(a_u64(&a), s1);
            idstr(a_u64(&a), s2);
            ww_lobby_setting(3, s1, 0, s2);
            r = 1;
        } else if (is(m, "SetLinkedLobby") || is(m, "SetLobbyGameServer")) r = 1;
        else if (is(m, "GetLobbyGameServer") || is(m, "GetFavoriteGameCount")) r = 0;
        else handled = 0;
    } else if (!strcmp(fam, "steamnetworking")) {
        if (is(m, "SendP2PPacket")) {
            idstr(a_u64(&a), s1);
            uint64_t d = a_ptr(&a);
            uint32_t len = (uint32_t)a_u32(&a);
            a_u32(&a);
            int ch = t->version >= 2 ? (int)a_u32(&a) : 0;
            r = d || !len ? (uint64_t)ww_p2p_send(s1, d ? G2H(d) : (void*)"", (int)len, ch) : 0;
        } else if (is(m, "IsP2PPacketAvailable")) {
            uint64_t ps = a_ptr(&a);
            int ch = t->version >= 2 ? (int)a_u32(&a) : 0, sz = ww_p2p_peek(ch);
            if (sz >= 0 && ps) ST32(ps, (uint32_t)sz);
            r = sz >= 0;
        } else if (is(m, "ReadP2PPacket")) {
            uint64_t d = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            uint64_t ps = a_ptr(&a), who = a_ptr(&a);
            int ch = t->version >= 2 ? (int)a_u32(&a) : 0;
            int len = d ? ww_p2p_read(ch, G2H(d), (int)cap, s1, sizeof s1) : -1;
            if (len >= 0) {
                if (ps) ST32(ps, (uint32_t)len);
                if (who) ST64(who, strtoull(s1, NULL, 10));
                r = 1;
            }
        } else if (is(m, "AcceptP2PSessionWithUser") || is(m, "CloseP2PSessionWithUser")) {
            idstr(a_u64(&a), s1);
            ww_p2p_session(is(m, "AcceptP2PSessionWithUser") ? 0 : 1, s1);
            r = 1;
        } else if (is(m, "GetP2PSessionState")) {
            a_u64(&a);
            uint64_t st = a_ptr(&a);
            if (st) { memset(G2H(st), 0, 20); ST8(st, 1); ST8(st + 3, 1); }   /* active, through the relay */
            r = 1;
        } else if (is(m, "CloseP2PChannelWithUser") || is(m, "AllowP2PPacketRelay")) r = 1;
        else handled = 0;
    } else if (!strcmp(fam, "steamnetworkingsockets")) {
        /* ISteamNetworkingSockets (Unreal's SteamSockets net driver): signed in and ready (authentication and the relay
           network report Current, with their callbacks), listen sockets and poll groups exist, the local identity is
           the player's SteamID. Remote connections aren't carried yet: connecting reports no connection, receiving
           finds nothing, so a game hosting for itself (a listen server) runs as with no players joining. */
        static uint32_t next_h = 0x100;
        static int status_posted;
        if (!status_posted) {
            status_posted = 1;
            uint8_t d[4 + 256] = { 0 };
            w32(d, 100);   /* k_ESteamNetworkingAvailability_Current */
            queue_post(1222, d, sizeof d);   /* SteamNetAuthenticationStatus_t */
            uint8_t rd[16 + 256] = { 0 };
            w32(rd, 100); w32(rd + 4, 0); w32(rd + 8, 100); w32(rd + 12, 100);
            queue_post(1281, rd, sizeof rd);   /* SteamRelayNetworkStatus_t */
        }
        if (is(m, "InitAuthentication")) r = 100;
        else if (is(m, "GetAuthenticationStatus")) { uint64_t p = a_ptr(&a); if (p) { memset(G2H(p), 0, 260); ST32(p, 100); } r = 100; }
        else if (is(m, "GetIdentity")) {
            /* SteamNetworkingIdentity: type (k_ESteamNetworkingIdentityType_SteamID 16), size 8, the SteamID64 */
            uint64_t p = a_ptr(&a);
            if (p) { memset(G2H(p), 0, 136); ST32(p, 16); ST32(p + 4, 8); ST64(p + 8, my_id()); }
            r = p != 0;
        } else if (is(m, "CreateListenSocketIP") || is(m, "CreateListenSocketP2P") || is(m, "CreatePollGroup")
                   || is(m, "CreateHostedDedicatedServerListenSocket") || is(m, "CreateListenSocketP2PFakeIP")) r = next_h++;
        else if (is(m, "CloseListenSocket") || is(m, "DestroyPollGroup") || is(m, "SetConnectionPollGroup") || is(m, "CloseConnection")
                 || is(m, "SetConnectionUserData") || is(m, "RunCallbacks")) r = 1;
        else if (is(m, "AcceptConnection") || is(m, "FlushMessagesOnConnection")) r = 1;   /* k_EResultOK */
        else if (is(m, "SendMessageToConnection")) r = 3;   /* k_EResultNoConnection */
        else if (is(m, "GetConnectionUserData")) r = (uint64_t)-1;
        else if (is(m, "ReceiveMessagesOnConnection") || is(m, "ReceiveMessagesOnPollGroup") || is(m, "ConnectByIPAddress") || is(m, "ConnectP2P")
                 || is(m, "ConnectToHostedDedicatedServer") || is(m, "GetConnectionInfo") || is(m, "GetListenSocketAddress")
                 || is(m, "CreateSocketPair") || is(m, "GetConnectionName")) r = 0;
        else if (is(m, "GetConnectionRealTimeStatus")) r = 3;
        else handled = 0;
    } else if (!strcmp(fam, "steamnetworkingutils")) {
        if (is(m, "GetRelayNetworkStatus")) {
            uint64_t p = a_ptr(&a);
            if (p) { memset(G2H(p), 0, 272); ST32(p, 100); ST32(p + 8, 100); ST32(p + 12, 100); }
            r = 100;
        } else if (is(m, "CheckPingDataUpToDate")) r = 1;
        else if (is(m, "GetLocalTimestamp")) r = host_ticks_ns() / 1000;
        else if (is(m, "InitRelayNetworkAccess") || is(m, "SetDebugOutputFunction")) r = 0;
        else if (!strncmp(m, "SetGlobalConfigValue", 20) || !strncmp(m, "SetConnectionConfigValue", 24) || is(m, "SetConfigValue")
                 || is(m, "SetConfigValueStruct") || !strncmp(m, "SetGlobalCallback", 17)) r = 1;
        else if (is(m, "GetConfigValue")) r = (uint64_t)-1;   /* k_ESteamNetworkingGetConfigValue_BadValue */
        else if (is(m, "EstimatePingTimeBetweenTwoLocations") || is(m, "EstimatePingTimeFromLocalHost") || is(m, "GetPingToDataCenter")
                 || is(m, "GetDirectPingToPOP")) r = (uint64_t)-1;
        else if (is(m, "GetPOPCount") || is(m, "GetPOPList") || is(m, "AllocateMessage") || is(m, "ParsePingLocationString")
                 || is(m, "IsFakeIPv4") || is(m, "GetIPv4FakeIPType") || is(m, "GetRealIdentityForFakeIP")) r = 0;
        else if (is(m, "SteamNetworkingIPAddr_ToString") || is(m, "SteamNetworkingIdentity_ToString") || is(m, "ConvertPingLocationToString")) {
            a_ptr(&a);
            uint64_t buf = a_ptr(&a);
            uint32_t cap = (uint32_t)a_u32(&a);
            if (buf && cap) ST8(buf, 0);
            r = 0;
        } else handled = 0;
    } else if (!strcmp(fam, "steamuserstats")) {
        if (is(m, "RequestCurrentStats")) {
            uint8_t d[24] = { 0 };
            w64(d, rt_steam_appid); w32(d + 8, 1); w64(d + 16, my_id());   /* UserStatsReceived_t */
            queue_post(1101, d, 24);
            r = 1;
        } else if (is(m, "GetStat") || is(m, "GetUserStat")) {
            if (is(m, "GetUserStat")) a_u64(&a);
            uint64_t k = a_ptr(&a), p = a_ptr(&a);
            uint32_t* v = k ? stat_slot(GSTR(k), 0, 1) : NULL;
            if (v && p) ST32(p, *v);
            r = v != NULL;
        } else if (is(m, "SetStat")) {
            uint64_t k = a_ptr(&a);
            uint32_t bits;
            if (strstr(m, "float")) { float f = RT_G32 ? 0 : (float)c->x[2].f[0]; if (RT_G32) bits = (uint32_t)a_u32(&a); else memcpy(&bits, &f, 4); }
            else bits = (uint32_t)a_u32(&a);
            uint32_t* v = k ? stat_slot(GSTR(k), 0, 1) : NULL;
            if (v) *v = bits;
            r = v != NULL;
        } else if (is(m, "GetAchievement") || is(m, "GetUserAchievement")) {
            if (is(m, "GetUserAchievement")) a_u64(&a);
            uint64_t k = a_ptr(&a), p = a_ptr(&a);
            uint32_t* v = k ? stat_slot(GSTR(k), 1, 0) : NULL;
            if (p) ST8(p, v ? (uint8_t)*v : 0);
            r = 1;
        } else if (is(m, "SetAchievement") || is(m, "ClearAchievement")) {
            uint64_t k = a_ptr(&a);
            uint32_t* v = k ? stat_slot(GSTR(k), 1, 1) : NULL;
            if (v) *v = is(m, "SetAchievement");
            r = 1;
        } else if (is(m, "StoreStats")) {
            uint8_t d[16] = { 0 };
            w64(d, rt_steam_appid); w32(d + 8, 1);   /* UserStatsStored_t */
            queue_post(1102, d, 16);
            r = 1;
        } else if (is(m, "GetNumAchievements")) r = 0;
        else if (is(m, "GetAchievementDisplayAttribute") || is(m, "GetAchievementName")) r = ring_str("");
        else if (is(m, "FindLeaderboard") || is(m, "FindOrCreateLeaderboard")) {
            Call* k = done_call(1104, 16);   /* LeaderboardFindResult_t: not found */
            ret64(c, k->call);
            goto log;
        } else handled = 0;
    } else if (!strcmp(fam, "steamremotestorage")) {
        r = 0;   /* no cloud: every file query answers none, every write fails (saves stay in the page's storage) */
    } else handled = 0;

    if (!handled) {
        r = 0;
        if (dbg_steam()) {
            static char said[64][128];
            int k = 0;
            for (int i = 0; i < nwif; i++) if (wif[i].obj == self) k = i;
            if (n < 128 && !said[k][n]++) host_log("[steam] %s %d: %s not implemented (returns 0)", fam, t ? t->version : 0, m);
        }
    }
    RET(r);
log:
    if (dbg_steam()) {
        static char seen[64][128];
        int k = 0;
        for (int i = 0; i < nwif; i++) if (wif[i].obj == self) k = i;
        if (n < 128 && !seen[k][n]++) host_log("[steam] %s %d: %s", fam, t ? t->version : 0, m);
    }
}
