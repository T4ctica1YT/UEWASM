#include "host.h"

#include <string.h>
#include <time.h>

static int is_classic(void);

/* steam_api: games that use the SteamInternal_* interfaces (UE4 and later) run as if Steam weren't there; older ones
   (the classic accessors below) get an offline Steam. RestartAppIfNecessary must say no, or the game quits to
   relaunch itself through Steam. */
HLE(SteamAPI_RestartAppIfNecessary) { RET(0); }
HLE(SteamAPI_Init) { RET(is_classic()); }
HLE(SteamAPI_InitSafe) { RET(is_classic()); }
HLE(SteamAPI_InitFlat) { RET(1); }
HLE(SteamAPI_Shutdown) { RET(0); }
HLE(SteamAPI_RunCallbacks) { RET(0); }
HLE(SteamAPI_RegisterCallback) { RET(0); }
HLE(SteamAPI_UnregisterCallback) { RET(0); }
HLE(SteamAPI_RegisterCallResult) { RET(0); }
HLE(SteamAPI_UnregisterCallResult) { RET(0); }
HLE(SteamAPI_GetHSteamUser) { RET(is_classic()); }
HLE(SteamAPI_GetHSteamPipe) { RET(is_classic()); }
HLE(SteamAPI_IsSteamRunning) { RET(is_classic()); }
HLE(SteamInternal_ContextInit) { RET(0); }
HLE(SteamInternal_CreateInterface) { RET(0); }
HLE(SteamInternal_FindOrCreateUserInterface) { RET(0); }
HLE(SteamInternal_FindOrCreateGameServerInterface) { RET(0); }
HLE(SteamInternal_GameServer_Init) { RET(0); }
HLE(SteamGameServer_Shutdown) { RET(0); }
HLE(SteamGameServer_RunCallbacks) { RET(0); }
HLE(SteamGameServer_GetHSteamUser) { RET(0); }
HLE(SteamGameServer_GetHSteamPipe) { RET(0); }
/* SDK 1.58+: ESteamAPIInitResult, 1 = failed (Steam isn't running here), with the reason in a 1024-byte buffer */
static void init_failed(CPU* c, uint64_t msg)
{
    static const char why[] = "Steam is not available in the browser";
    if (msg) memcpy(G2H(msg), why, sizeof why);
    RET(1);
}
HLE(SteamInternal_SteamAPI_Init) { init_failed(c, A2); }
HLE(SteamInternal_GameServer_Init_V2) { init_failed(c, ARG(7)); }

/* ---- offline Steam for the classic accessor API (SteamUser(), SteamFriends()... the UE3 era) ----
   Games built on it expect Steam: they create their interfaces only when SteamAPI_Init succeeds, and some code uses
   them unchecked. So for these games Steam starts, with a user who is logged off: every interface exists, and its
   methods answer as an offline Steam would (no friends, no cloud files, no stats, subscribed to the app and its DLC,
   English). Newer games (SteamInternal_* interfaces, UE4 and later) keep running as if Steam weren't there.
   The interfaces' layouts differ between SDK versions, so the methods aren't modelled one by one: every slot returns
   0, and on x86 pops what its call site pushed (rt_call_args, from the lifter). The few slots that have held the
   same method in every SDK version answer properly. ?steamdebug=1 logs each method used. */
enum { IF_USER, IF_FRIENDS, IF_UTILS, IF_APPS, IF_USERSTATS, IF_REMOTE, IF_MATCHMAKING, IF_MMSERVERS, IF_NETWORKING,
       IF_SCREENSHOTS, IF_HTTP, IF_UGC, IF_COUNT, NSLOTS = 128 };
static const char* const if_names[IF_COUNT] = { "ISteamUser", "ISteamFriends", "ISteamUtils", "ISteamApps", "ISteamUserStats",
    "ISteamRemoteStorage", "ISteamMatchmaking", "ISteamMatchmakingServers", "ISteamNetworking", "ISteamScreenshots",
    "ISteamHTTP", "ISteamUGC" };
static uint64_t objs[IF_COUNT];
static int classic = -1;

/* the game imports the accessors from steam_api (rather than only the runtime offering them) */
static int is_classic(void)
{
    if (classic < 0) {
        classic = 0;
        for (size_t i = 0; i < rt_thunk_count; i++)
            if (strcmp(rt_thunks[i].dll, "hle") && !strcmp(rt_thunks[i].name, "SteamUser")) classic = 1;
        if (classic) host_log("[steam] offline Steam (app %u, logged off)", rt_steam_appid);
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

static void slot_call(CPU* c, int n)
{
    uint64_t self = RT_G32 ? HLE32_ECX(c) : A1, r = 0;
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
    static int dbg = -1;
    static uint8_t said[IF_COUNT + 1][NSLOTS];
    if (dbg < 0) dbg = host_query_flag("steamdebug");
    if (dbg && !said[k][n]++)
        host_log("[steam] %s slot %d -> %llx (from %llx)", k < IF_COUNT ? if_names[k] : "unknown interface", n, (unsigned long long)r,
                 (unsigned long long)(RT_G32 ? LD32(HLE32_ESP(c)) : LD64(R(4))));
    RET(r);
}

static void s0(CPU* c) { slot_call(c, 0); }
static void s1(CPU* c) { slot_call(c, 1); }
static void s2(CPU* c) { slot_call(c, 2); }
static void s3(CPU* c) { slot_call(c, 3); }
static void s4(CPU* c) { slot_call(c, 4); }
static void s5(CPU* c) { slot_call(c, 5); }
static void s6(CPU* c) { slot_call(c, 6); }
static void s7(CPU* c) { slot_call(c, 7); }
static void s8(CPU* c) { slot_call(c, 8); }
static void s9(CPU* c) { slot_call(c, 9); }
static void s10(CPU* c) { slot_call(c, 10); }
static void s11(CPU* c) { slot_call(c, 11); }
static void s12(CPU* c) { slot_call(c, 12); }
static void s13(CPU* c) { slot_call(c, 13); }
static void s14(CPU* c) { slot_call(c, 14); }
static void s15(CPU* c) { slot_call(c, 15); }
static void s16(CPU* c) { slot_call(c, 16); }
static void s17(CPU* c) { slot_call(c, 17); }
static void s18(CPU* c) { slot_call(c, 18); }
static void s19(CPU* c) { slot_call(c, 19); }
static void s20(CPU* c) { slot_call(c, 20); }
static void s21(CPU* c) { slot_call(c, 21); }
static void s22(CPU* c) { slot_call(c, 22); }
static void s23(CPU* c) { slot_call(c, 23); }
static void s24(CPU* c) { slot_call(c, 24); }
static void s25(CPU* c) { slot_call(c, 25); }
static void s26(CPU* c) { slot_call(c, 26); }
static void s27(CPU* c) { slot_call(c, 27); }
static void s28(CPU* c) { slot_call(c, 28); }
static void s29(CPU* c) { slot_call(c, 29); }
static void s30(CPU* c) { slot_call(c, 30); }
static void s31(CPU* c) { slot_call(c, 31); }
static void s32(CPU* c) { slot_call(c, 32); }
static void s33(CPU* c) { slot_call(c, 33); }
static void s34(CPU* c) { slot_call(c, 34); }
static void s35(CPU* c) { slot_call(c, 35); }
static void s36(CPU* c) { slot_call(c, 36); }
static void s37(CPU* c) { slot_call(c, 37); }
static void s38(CPU* c) { slot_call(c, 38); }
static void s39(CPU* c) { slot_call(c, 39); }
static void s40(CPU* c) { slot_call(c, 40); }
static void s41(CPU* c) { slot_call(c, 41); }
static void s42(CPU* c) { slot_call(c, 42); }
static void s43(CPU* c) { slot_call(c, 43); }
static void s44(CPU* c) { slot_call(c, 44); }
static void s45(CPU* c) { slot_call(c, 45); }
static void s46(CPU* c) { slot_call(c, 46); }
static void s47(CPU* c) { slot_call(c, 47); }
static void s48(CPU* c) { slot_call(c, 48); }
static void s49(CPU* c) { slot_call(c, 49); }
static void s50(CPU* c) { slot_call(c, 50); }
static void s51(CPU* c) { slot_call(c, 51); }
static void s52(CPU* c) { slot_call(c, 52); }
static void s53(CPU* c) { slot_call(c, 53); }
static void s54(CPU* c) { slot_call(c, 54); }
static void s55(CPU* c) { slot_call(c, 55); }
static void s56(CPU* c) { slot_call(c, 56); }
static void s57(CPU* c) { slot_call(c, 57); }
static void s58(CPU* c) { slot_call(c, 58); }
static void s59(CPU* c) { slot_call(c, 59); }
static void s60(CPU* c) { slot_call(c, 60); }
static void s61(CPU* c) { slot_call(c, 61); }
static void s62(CPU* c) { slot_call(c, 62); }
static void s63(CPU* c) { slot_call(c, 63); }
static void s64(CPU* c) { slot_call(c, 64); }
static void s65(CPU* c) { slot_call(c, 65); }
static void s66(CPU* c) { slot_call(c, 66); }
static void s67(CPU* c) { slot_call(c, 67); }
static void s68(CPU* c) { slot_call(c, 68); }
static void s69(CPU* c) { slot_call(c, 69); }
static void s70(CPU* c) { slot_call(c, 70); }
static void s71(CPU* c) { slot_call(c, 71); }
static void s72(CPU* c) { slot_call(c, 72); }
static void s73(CPU* c) { slot_call(c, 73); }
static void s74(CPU* c) { slot_call(c, 74); }
static void s75(CPU* c) { slot_call(c, 75); }
static void s76(CPU* c) { slot_call(c, 76); }
static void s77(CPU* c) { slot_call(c, 77); }
static void s78(CPU* c) { slot_call(c, 78); }
static void s79(CPU* c) { slot_call(c, 79); }
static void s80(CPU* c) { slot_call(c, 80); }
static void s81(CPU* c) { slot_call(c, 81); }
static void s82(CPU* c) { slot_call(c, 82); }
static void s83(CPU* c) { slot_call(c, 83); }
static void s84(CPU* c) { slot_call(c, 84); }
static void s85(CPU* c) { slot_call(c, 85); }
static void s86(CPU* c) { slot_call(c, 86); }
static void s87(CPU* c) { slot_call(c, 87); }
static void s88(CPU* c) { slot_call(c, 88); }
static void s89(CPU* c) { slot_call(c, 89); }
static void s90(CPU* c) { slot_call(c, 90); }
static void s91(CPU* c) { slot_call(c, 91); }
static void s92(CPU* c) { slot_call(c, 92); }
static void s93(CPU* c) { slot_call(c, 93); }
static void s94(CPU* c) { slot_call(c, 94); }
static void s95(CPU* c) { slot_call(c, 95); }
static void s96(CPU* c) { slot_call(c, 96); }
static void s97(CPU* c) { slot_call(c, 97); }
static void s98(CPU* c) { slot_call(c, 98); }
static void s99(CPU* c) { slot_call(c, 99); }
static void s100(CPU* c) { slot_call(c, 100); }
static void s101(CPU* c) { slot_call(c, 101); }
static void s102(CPU* c) { slot_call(c, 102); }
static void s103(CPU* c) { slot_call(c, 103); }
static void s104(CPU* c) { slot_call(c, 104); }
static void s105(CPU* c) { slot_call(c, 105); }
static void s106(CPU* c) { slot_call(c, 106); }
static void s107(CPU* c) { slot_call(c, 107); }
static void s108(CPU* c) { slot_call(c, 108); }
static void s109(CPU* c) { slot_call(c, 109); }
static void s110(CPU* c) { slot_call(c, 110); }
static void s111(CPU* c) { slot_call(c, 111); }
static void s112(CPU* c) { slot_call(c, 112); }
static void s113(CPU* c) { slot_call(c, 113); }
static void s114(CPU* c) { slot_call(c, 114); }
static void s115(CPU* c) { slot_call(c, 115); }
static void s116(CPU* c) { slot_call(c, 116); }
static void s117(CPU* c) { slot_call(c, 117); }
static void s118(CPU* c) { slot_call(c, 118); }
static void s119(CPU* c) { slot_call(c, 119); }
static void s120(CPU* c) { slot_call(c, 120); }
static void s121(CPU* c) { slot_call(c, 121); }
static void s122(CPU* c) { slot_call(c, 122); }
static void s123(CPU* c) { slot_call(c, 123); }
static void s124(CPU* c) { slot_call(c, 124); }
static void s125(CPU* c) { slot_call(c, 125); }
static void s126(CPU* c) { slot_call(c, 126); }
static void s127(CPU* c) { slot_call(c, 127); }
static const GuestFn slot_fns[NSLOTS] = { s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22, s23, s24, s25, s26, s27, s28, s29, s30, s31, s32, s33, s34, s35, s36, s37, s38, s39, s40, s41, s42, s43, s44, s45, s46, s47, s48, s49, s50, s51, s52, s53, s54, s55, s56, s57, s58, s59, s60, s61, s62, s63, s64, s65, s66, s67, s68, s69, s70, s71, s72, s73, s74, s75, s76, s77, s78, s79, s80, s81, s82, s83, s84, s85, s86, s87, s88, s89, s90, s91, s92, s93, s94, s95, s96, s97, s98, s99, s100, s101, s102, s103, s104, s105, s106, s107, s108, s109, s110, s111, s112, s113, s114, s115, s116, s117, s118, s119, s120, s121, s122, s123, s124, s125, s126, s127 };

static uint64_t iface(int k)
{
    if (!is_classic()) return 0;
    if (!objs[k]) {
        uint64_t vt = g_calloc((uint64_t)GP * NSLOTS), o = g_calloc(16);
        for (int i = 0; i < NSLOTS; i++) STP(vt + (uint64_t)GP * i, rt_dyn_thunk_n(slot_fns[i], if_names[k], RT_SITE_ARGS));
        STP(o, vt);
        objs[k] = o;
    }
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
/* no game server runs */
HLE(SteamGameServer) { RET(0); }
