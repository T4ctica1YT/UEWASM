#include "host.h"

/* steam_api64: the game runs as if Steam isn't there (offline). RestartAppIfNecessary must say no, or the game
   quits to relaunch itself through Steam. */
HLE(SteamAPI_RestartAppIfNecessary) { RET(0); }
HLE(SteamAPI_Init) { RET(0); }
HLE(SteamAPI_InitSafe) { RET(0); }
HLE(SteamAPI_InitFlat) { RET(1); }
HLE(SteamAPI_Shutdown) { RET(0); }
HLE(SteamAPI_RunCallbacks) { RET(0); }
HLE(SteamAPI_RegisterCallback) { RET(0); }
HLE(SteamAPI_UnregisterCallback) { RET(0); }
HLE(SteamAPI_RegisterCallResult) { RET(0); }
HLE(SteamAPI_UnregisterCallResult) { RET(0); }
HLE(SteamAPI_GetHSteamUser) { RET(0); }
HLE(SteamAPI_GetHSteamPipe) { RET(0); }
HLE(SteamAPI_IsSteamRunning) { RET(0); }
HLE(SteamInternal_ContextInit) { RET(0); }
HLE(SteamInternal_CreateInterface) { RET(0); }
HLE(SteamInternal_FindOrCreateUserInterface) { RET(0); }
HLE(SteamInternal_FindOrCreateGameServerInterface) { RET(0); }
HLE(SteamInternal_GameServer_Init) { RET(0); }
HLE(SteamGameServer_Shutdown) { RET(0); }
HLE(SteamGameServer_RunCallbacks) { RET(0); }
HLE(SteamGameServer_GetHSteamUser) { RET(0); }
HLE(SteamGameServer_GetHSteamPipe) { RET(0); }
