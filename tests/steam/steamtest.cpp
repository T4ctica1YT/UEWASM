// Drives the Steam API the way Unreal's Steam subsystem does, for testing WebWorks Steam in a web build: interfaces
// found by version, polled call results (IsAPICallCompleted / GetAPICallResult), registered callbacks, lobbies with
// data and chat, and P2P packets. Run two copies: "host" makes a lobby and answers pings; "join" finds the lobby,
// joins, chats and pings the host. Each prints OK/FAIL lines.
//   cl /EHsc /MD /I <steamworks sdk>\public steamtest.cpp
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "steam/steam_api.h"

typedef bool (*InitFn)();
typedef void (*RunFn)();
typedef void* (*FindFn)(HSteamUser, const char*);
typedef void (*RegFn)(CCallbackBase*, int);

static RunFn run_callbacks;
static RegFn reg_callback;
static ISteamUtils* utils;

template <class T, int ID> struct Callback : CCallbackBase {
    void (*fn)(T*);
    explicit Callback(void (*f)(T*)) : fn(f) { reg_callback(this, ID); }
    void Run(void* p) override { fn((T*)p); }
    void Run(void* p, bool, SteamAPICall_t) override { fn((T*)p); }
    int GetCallbackSizeBytes() override { return sizeof(T); }
};

template <class T> static bool wait_call(SteamAPICall_t call, T* out, int id, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        run_callbacks();
        bool failed = false;
        if (utils->IsAPICallCompleted(call, &failed)) {
            bool got = utils->GetAPICallResult(call, out, sizeof(T), id, &failed);
            if (!got || failed) printf("call %llx: result %d, failed %d, first word %d\n", (unsigned long long)call, got, failed, *(int*)out);
            return got && !failed;
        }
        Sleep(50);
    }
    return false;
}

static int joined, chats;
static uint64 chat_lobby;
static ISteamMatchmaking* mm;

static void on_chat_update(LobbyChatUpdate_t* u)
{
    printf("callback LobbyChatUpdate: %llu state %u\n", (unsigned long long)u->m_ulSteamIDUserChanged, u->m_rgfChatMemberStateChange);
    if (u->m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) joined++;
}
static void on_chat_msg(LobbyChatMsg_t* m)
{
    char text[256] = { 0 };
    CSteamID who;
    EChatEntryType type;
    int n = mm->GetLobbyChatEntry(CSteamID(m->m_ulSteamIDLobby), m->m_iChatID, &who, text, sizeof text - 1, &type);
    printf("callback LobbyChatMsg: \"%s\" (%d bytes) from %llu\n", text, n, (unsigned long long)who.ConvertToUint64());
    chats++;
}
static void on_p2p_request(P2PSessionRequest_t* r);
static ISteamNetworking* net;
static void on_p2p_request(P2PSessionRequest_t* r)
{
    printf("callback P2PSessionRequest from %llu\n", (unsigned long long)r->m_steamIDRemote.ConvertToUint64());
    net->AcceptP2PSessionWithUser(r->m_steamIDRemote);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    bool host = argc > 1 && !strcmp(argv[argc - 1], "host");
    HMODULE api = LoadLibraryA("steam_api64.dll");
    InitFn init = api ? (InitFn)GetProcAddress(api, "SteamAPI_Init") : NULL;
    run_callbacks = api ? (RunFn)GetProcAddress(api, "SteamAPI_RunCallbacks") : NULL;
    reg_callback = api ? (RegFn)GetProcAddress(api, "SteamAPI_RegisterCallback") : NULL;
    FindFn find = api ? (FindFn)GetProcAddress(api, "SteamInternal_FindOrCreateUserInterface") : NULL;
    if (!init || !run_callbacks || !reg_callback || !find || !init()) { printf("FAIL SteamAPI_Init\n"); return 1; }
    ISteamUser* user = (ISteamUser*)find(1, STEAMUSER_INTERFACE_VERSION);
    ISteamFriends* friends = (ISteamFriends*)find(1, STEAMFRIENDS_INTERFACE_VERSION);
    utils = (ISteamUtils*)find(1, STEAMUTILS_INTERFACE_VERSION);
    mm = (ISteamMatchmaking*)find(1, STEAMMATCHMAKING_INTERFACE_VERSION);
    net = (ISteamNetworking*)find(1, STEAMNETWORKING_INTERFACE_VERSION);
    if (!user || !friends || !utils || !mm || !net) { printf("FAIL interfaces\n"); return 1; }
    CSteamID me = user->GetSteamID();
    printf("steam id %llu\n", (unsigned long long)me.ConvertToUint64());
    int logged_on = user->BLoggedOn();
    printf("logged on %d\n", logged_on);
    unsigned app = utils->GetAppID();
    printf("app %u\n", app);
    const char* name = friends->GetPersonaName();
    printf("OK signed in as %s (%llu), logged on %d, app %u, role %s\n", name, (unsigned long long)me.ConvertToUint64(), logged_on, app,
           host ? "host" : "join");

    Callback<LobbyChatUpdate_t, LobbyChatUpdate_t::k_iCallback> cb1(on_chat_update);
    Callback<LobbyChatMsg_t, LobbyChatMsg_t::k_iCallback> cb2(on_chat_msg);
    Callback<P2PSessionRequest_t, P2PSessionRequest_t::k_iCallback> cb3(on_p2p_request);

    char msg[256];
    uint32 n;
    CSteamID from;
    if (host) {
        LobbyCreated_t created;
        if (!wait_call(mm->CreateLobby(k_ELobbyTypePublic, 4), &created, LobbyCreated_t::k_iCallback, 15000) || created.m_eResult != k_EResultOK) {
            printf("FAIL CreateLobby\n");
            return 1;
        }
        CSteamID lobby(created.m_ulSteamIDLobby);
        mm->SetLobbyData(lobby, "uewtest", "1");
        mm->SetLobbyData(lobby, "map", "Arena");
        printf("OK lobby %llu created, owner is me: %d, members %d\n", (unsigned long long)lobby.ConvertToUint64(),
               mm->GetLobbyOwner(lobby) == me, mm->GetNumLobbyMembers(lobby));
        for (int t = 0; t < 120000; t += 50) {
            run_callbacks();
            while (net->IsP2PPacketAvailable(&n)) {
                memset(msg, 0, sizeof msg);
                if (net->ReadP2PPacket(msg, sizeof msg - 1, &n, &from)) {
                    printf("OK host got \"%s\" (%u bytes) from %llu\n", msg, n, (unsigned long long)from.ConvertToUint64());
                    net->SendP2PPacket(from, "pong", 5, k_EP2PSendReliable);
                }
            }
            if (joined && chats == 1) { printf("OK host saw the join and the chat message\n"); chats++; }
            Sleep(50);
        }
    } else {
        CSteamID lobby;
        for (int tries = 0; tries < 20 && !lobby.IsValid(); tries++) {
            mm->AddRequestLobbyListStringFilter("uewtest", "1", k_ELobbyComparisonEqual);
            LobbyMatchList_t list;
            if (wait_call(mm->RequestLobbyList(), &list, LobbyMatchList_t::k_iCallback, 10000) && list.m_nLobbiesMatching)
                lobby = mm->GetLobbyByIndex(0);
            else Sleep(1000);
        }
        if (!lobby.IsValid()) { printf("FAIL no lobby found\n"); return 1; }
        printf("OK found lobby %llu, map %s\n", (unsigned long long)lobby.ConvertToUint64(), mm->GetLobbyData(lobby, "map"));
        LobbyEnter_t entered;
        if (!wait_call(mm->JoinLobby(lobby), &entered, LobbyEnter_t::k_iCallback, 15000) || entered.m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
            printf("FAIL JoinLobby\n");
            return 1;
        }
        CSteamID owner = mm->GetLobbyOwner(lobby);
        printf("OK joined, %d members, owner %llu, uewtest=%s\n", mm->GetNumLobbyMembers(lobby), (unsigned long long)owner.ConvertToUint64(),
               mm->GetLobbyData(lobby, "uewtest"));
        mm->SendLobbyChatMsg(lobby, "hello from join", 16);
        net->SendP2PPacket(owner, "ping", 5, k_EP2PSendReliable);
        for (int t = 0; t < 30000; t += 50) {
            run_callbacks();
            if (net->IsP2PPacketAvailable(&n) && net->ReadP2PPacket(msg, sizeof msg, &n, &from)) {
                printf("OK join got \"%s\" back from %llu\nDONE\n", msg, (unsigned long long)from.ConvertToUint64());
                return 0;
            }
            Sleep(50);
        }
        printf("FAIL no reply\n");
        return 1;
    }
    return 0;
}
