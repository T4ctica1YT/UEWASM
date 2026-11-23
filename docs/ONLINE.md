# Online services: how they fit together

This page covers the internals behind the README's [Steam](../README.md#steam-multiplayer-webworks),
[EOS](../README.md#epic-online-services) and [official servers](../README.md#official-servers) sections, for people
changing them.

## Layers

```
game ─ steam_api64 ───▶ runtime/hle_steam.c ─┬─ offline Steam (in the runtime)
                                             ├─ WebWorks client (uew_webworks.js) ─▶ WebWorks server or uewserve's relay
                                             └─ Steam client (uew_steamcm.js) ─▶ Steam CMs (WebSocket, or TCP over WISP)
game ─ EOSSDK ────────▶ runtime/hle_eos.c (stand-in)   or   the real EOS SDK, recompiled (--eos real)
game ─ Winsock ───────▶ runtime/hle_net.c ─▶ uew_host.js ─▶ uew_wisp.js ─▶ WISP server (uewserve /wisp/)
game ─ Schannel ──────▶ runtime/hle_schannel.c (BearSSL) + hle_certstore.c (Mozilla roots, runtime/ca_roots.h)
```

## Steam

`hle_steam.c` answers every Steam interface version: the method tables are generated from the Steamworks SDK headers
(`runtime/steam_ifaces.h`, by `gen_steam_ifaces`). Methods dispatch by family and name. `?steamdebug=1` logs each
method once, and also callbacks and tickets.

The first Steam call picks a backend (`ww_active`):

- **WebWorks / relay** (`--steam webworks`, `--relay`). Guest sign-in, lobbies, P2P packets and stats go through
  `uew_webworks.js`. The relay in `uewserve` (`rustlift/src/bin/uewserve/main.rs`) speaks the same protocol, so the page
  doesn't know which one it talks to. `tests/steam/run_pair.py` runs two players against it.
- **Steam account** (`--steam account`). `uew_steamcm.js` is a small Steam client. It signs in with a password plus
  Steam Guard, or by QR code (Steam's `BeginAuthSessionViaQR`, the same flow as the desktop client). It logs on with
  the refresh token, then reports the game as running (`ClientGamesPlayed`), fetches the app ownership ticket and
  collects game connect tokens.
  - **Tickets** follow SteamKit2: a game connect token, a session header and the ownership ticket. Each ticket is
    registered with `ClientAuthList` and handed to the game once Steam acknowledges it (`ClientAuthListAck`), or after
    5 seconds.
  - **Web API tickets** (`GetAuthTicketForWebApi`, what EOS Connect uses) carry the identity as the server secret
    (`str:<identity>\0`) and keep their real length; the EOS SDK refuses one padded to 2560 bytes.
  - **Transport:** the CM WebSocket endpoints accept any page origin. With `--steam-wisp`, the client speaks Steam's TCP
    protocol (`VT01` framing, `ChannelEncrypt` handshake to Valve's public key, then AES-256-CBC with an HMAC-SHA1 IV)
    over a WISP stream instead. ECB, which WebCrypto doesn't offer, is built from single-block CBC.
  - **Lobbies and P2P** still go through WebWorks or the relay.
- **Offline** (`--steam offline`): a local session with fixed answers.

`ISteamNetworkingSockets` has minimal support: signed in, listen sockets and poll groups exist, but there are no remote
connections yet, so a game that waits for it starts up.

## Epic Online Services

- **Stand-in** (`--eos standin`, `hle_eos.c`): all ~640 SDK functions, generated from the SDK headers
  (`runtime/eos_api.h`, by `gen_eos_api`). Sign-in, user info, local lobbies, and player data storage kept in the save
  folder.
- **Real** (`--eos real`): the game's own `EOSSDK-Win64-Shipping.dll` is recompiled
  (`link.rs`: `skipped(name, real_eos)`) and talks to Epic through WISP and Schannel. With Steam sign-in it needs a
  real ticket, so pair it with `--steam account`.
  - The SDK carries OpenSSL 1.1.1 for its own crypto. Player data storage is encrypted client-side with AES-256-GCM,
    which is why the lifter's handling of hand-written assembly matters here (see [LIFTER.md](LIFTER.md)).
  - The SDK caches downloaded files under the game's `Saved/PersistentDownloadDir/EOSCache`. A cache written by a
    broken build stays broken; `?clearsave=<path>` removes it.

## WISP and sockets

`hle_net.c` implements Winsock. Names the page serves itself (the offline stand-ins) resolve to 127.1.x.y and connect
inside the page. Every other name or address becomes a WISP stream (`uew_wisp.js`: WISP v1 CONNECT/DATA/CONTINUE/CLOSE
over one WebSocket). The WISP server in `uewserve` (`rustlift/src/bin/uewserve/wisp.rs`) refuses private and loopback
addresses unless `--wisp-private` is given, and `--wisp-allow` narrows it to a list of hosts.

Blocking socket calls (`select`, `WSAPoll`, `recv`, `WSAWaitForMultipleEvents`) check again every `NET_POLL_MS`
(4 ms). Data only arrives when the browser gets a turn, and a shorter interval kept the page from painting while an
SDK's network threads waited (seconds per frame during sign-in).

## TLS and crypto

- **Schannel** (`hle_schannel.c`) is a BearSSL client behind SSPI. It's shaped to libcurl's Schannel backend:
  - empty output buffers are allowed;
  - the returned attributes must equal the requested ones;
  - whole records only, with `SECBUFFER_EXTRA` and `SEC_E_INCOMPLETE_MESSAGE`;
  - in manual-validation mode the certificate store holds the chain the server sent.
- **Certificates** (`hle_certstore.c`) are checked against Mozilla's roots in `runtime/ca_roots.h`. Regenerate that
  with `gen_ca_roots` from a current `cacert.pem`.
- **Crypto:** CryptoAPI and CNG hashes and HMAC, and secure random numbers (`hle_crypto.c`), are real.
- **Debugging:** `?tlsdebug=1` logs handshakes. `?httpdebug=1` logs request lines and failed answers' bodies, leaves
  out tokens and successful bodies, and offers servers HTTP/1.1 only so the traffic stays readable.

## Testing online changes

| Change | Run |
|---|---|
| sockets, WISP, TLS, crypto | `python tests/net/run_net.py` (TCP, UDP, HTTPS in libcurl's style, a full download, hashes; against native) |
| Steam lobbies and P2P | build `tests/steam/steamtest.cpp`, then `python tests/steam/run_pair.py <web>` |
| AES or other hand-written crypto | `python tests/aes/run_aes.py` |
| Steam account sign-in | nothing automated (it needs an account). A page that loads `uew_wisp.js` and `uew_steamcm.js` and calls `connect()`, `service("Authentication.GetPasswordRSAPublicKey#1", …)` and an anonymous `ClientLogon` (SteamID type 10) checks the transport and encryption with no account, over both WebSocket and WISP (`?steamwisp=0/1`) |

Never commit anything tied to a real account: tokens, SteamIDs, names, or logs that contain them.
