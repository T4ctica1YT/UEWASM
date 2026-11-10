// Offline stand-in for Epic's online services (accounts, lightswitch, MCP profiles, storefront, cloud storage), for
// games of the era that talk to them over HTTP. The runtime routes their host names here (Module.uewNet), so the game
// signs in to a local account and reaches its menus without Epic's servers. With Module.uewNetPlaceholders, files the
// game fetches that aren't known here (images, JSON) come back as stand-ins with the right type instead of errors.
(function () {
  const accountId = "a0f1c2d3e4f5a6b7c8d9e0f1a2b3c4d5", displayName = localStorage.getItem("uewName") || "Player";
  const now = () => new Date().toISOString();
  const json = (body, status) => ({ status: status || 200, headers: { "Content-Type": "application/json" }, body });
  const empty = status => ({ status: status || 204, headers: {}, body: "" });
  const store = (() => { try { return JSON.parse(localStorage.getItem("uewEpic") || "{}"); } catch (e) { return {}; } })();
  const save = () => { try { localStorage.setItem("uewEpic", JSON.stringify(store)); } catch (e) {} };

  function token() {
    return {
      access_token: "uewasm-offline", expires_in: 28800, expires_at: "9999-12-31T23:59:59.999Z", token_type: "bearer",
      refresh_token: "uewasm-offline-refresh", refresh_expires: 115200, refresh_expires_at: "9999-12-31T23:59:59.999Z",
      account_id: accountId, client_id: "ec684b8c687f479fadea3cb2ad83f5c6", internal_client: true, client_service: "fortnite",
      displayName, app: "fortnite", in_app_id: accountId, device_id: "uewasm",
    };
  }
  function account() {
    return { id: accountId, displayName, name: displayName, email: "player@localhost", failedLoginAttempts: 0, lastLogin: now(),
             numberOfDisplayNameChanges: 0, ageGroup: "UNKNOWN", headless: false, country: "US", lastName: "", preferredLanguage: "en",
             canUpdateDisplayName: false, tfaEnabled: false, emailVerified: true, minorVerified: false, minorExpected: false, minorStatus: "UNKNOWN" };
  }

  // MCP profiles: kept in localStorage so changes made in the menus persist
  function newProfile(profileId) {
    const p = { _id: accountId, created: now(), updated: now(), rvn: 1, wipeNumber: 1, accountId, profileId, version: "uewasm", items: {},
                stats: { attributes: {} }, commandRevision: 0 };
    const a = p.stats.attributes;
    if (profileId === "athena") {
      Object.assign(a, { past_seasons: [], season_match_boost: 0, loadouts: [], mfa_reward_claimed: true, rested_xp_overflow: 0,
                         quest_manager: { dailyLoginInterval: now(), dailyQuestRerolls: 1 }, book_level: 1, season_num: 3, book_xp: 0,
                         permissions: [], book_purchased: false, lifetime_wins: 0, party_assist_quest: "", purchased_battle_pass_tier_offers: [],
                         rested_xp_exchange: 1, level: 1, xp_overflow: 0, rested_xp: 0, rested_xp_mult: 0, accountLevel: 1,
                         competitive_identity: {}, inventory_limit_bonus: 0, last_applied_loadout: "", daily_rewards: {}, xp: 0,
                         season_friend_match_boost: 0, active_loadout_index: 0, favorite_musicpack: "", favorite_glider: "",
                         favorite_pickaxe: "", favorite_skydivecontrail: "", favorite_backpack: "", favorite_dance: ["", "", "", "", "", ""],
                         favorite_itemwraps: [], favorite_character: "", favorite_loadingscreen: "" });
      for (const t of ["AthenaPickaxe:DefaultPickaxe", "AthenaGlider:DefaultGlider", "AthenaDance:EID_DanceMoves"])
        p.items[t] = { templateId: t, attributes: { max_level_bonus: 0, level: 1, item_seen: true, xp: 0, variants: [], favorite: false }, quantity: 1 };
    } else if (profileId === "common_core") {
      Object.assign(a, { mtx_affiliate: "", current_mtx_platform: "EpicPC", mtx_purchase_history: {}, allowed_to_receive_gifts: true,
                         allowed_to_send_gifts: true, gift_history: {}, in_app_purchases: {}, undo_cooldowns: [], personal_offers: {},
                         mfa_enabled: true, ban_status: { bRequiresUserAck: false, banReasons: [], bBanHasStarted: false, banStartTimeUtc: null,
                         banDurationDays: null, additionalInfo: "", exploitProgramName: "", competitiveBanReason: "None" }, ban_history: {} });
      p.items["Currency:MtxPurchased"] = { templateId: "Currency:MtxPurchased", attributes: { platform: "EpicPC" }, quantity: 0 };
    } else if (profileId === "common_public") {
      Object.assign(a, { banner_color: "", homebase_name: "", banner_icon: "" });
    }
    return p;
  }
  function profile(profileId) {
    store.profiles = store.profiles || {};
    if (!store.profiles[profileId]) { store.profiles[profileId] = newProfile(profileId); save(); }
    return store.profiles[profileId];
  }
  // commands that change the profile; the rest only read it
  function mcp(command, profileId, body) {
    const p = profile(profileId);
    const a = p.stats.attributes;
    let changed = false;
    try {
      if (command === "SetCosmeticLockerSlot" || command === "EquipBattleRoyaleCustomization") {
        const slot = (body.category || body.slotName || "").toLowerCase(), item = body.itemToSlot || "";
        const key = { character: "favorite_character", backpack: "favorite_backpack", pickaxe: "favorite_pickaxe", glider: "favorite_glider",
                      skydivecontrail: "favorite_skydivecontrail", musicpack: "favorite_musicpack", loadingscreen: "favorite_loadingscreen" }[slot];
        if (key) { a[key] = item; changed = true; }
        if (slot === "dance" && body.indexWithinSlot !== undefined) { a.favorite_dance[body.indexWithinSlot] = item; changed = true; }
      } else if (command === "SetMtxPlatform") { a.current_mtx_platform = body.newPlatform || "EpicPC"; changed = true; }
      else if (command === "MarkItemSeen") { for (const id of body.itemIds || []) if (p.items[id]) p.items[id].attributes.item_seen = true; changed = true; }
      else if (command === "SetItemFavoriteStatusBatch") {
        (body.itemIds || []).forEach((id, i) => { if (p.items[id]) p.items[id].attributes.favorite = !!(body.itemFavStatus || [])[i]; }); changed = true;
      }
    } catch (e) { console.log("[epic] " + command + ": " + e); }
    if (changed) { p.rvn++; p.commandRevision++; p.updated = now(); save(); }
    return { profileRevision: p.rvn, profileId, profileChangesBaseRevision: p.rvn, profileChanges: [{ changeType: "fullProfileUpdate", profile: p }],
             profileCommandRevision: p.commandRevision, serverTime: now(), responseVersion: 1 };
  }

  const status = { serviceInstanceId: "fortnite", status: "UP", message: "Offline (UEWASM)", maintenanceUri: null, overrideCatalogIds: [],
                   allowedActions: ["PLAY", "DOWNLOAD"], banned: false, launcherInfoDTO: { appName: "Fortnite", catalogItemId: "", namespace: "fn" } };
  const calendar = () => ({ channels: {
    "client-matchmaking": { states: [], cacheExpire: "9999-01-01T00:00:00.000Z" },
    "client-events": { states: [{ validFrom: "2018-01-01T00:00:00.000Z", activeEvents: [], state: { activeStorefronts: [], eventNamedWeights: {},
      seasonNumber: 3, seasonTemplateId: "AthenaSeason:athenaseason3", matchXpBonusPoints: 0, seasonBegin: "2018-01-01T00:00:00Z",
      seasonEnd: "9999-01-01T00:00:00Z", seasonDisplayedEnd: "9999-01-01T00:00:00Z", weeklyStoreEnd: "9999-01-01T00:00:00Z",
      stwEventStoreEnd: "9999-01-01T00:00:00.000Z", stwWeeklyStoreEnd: "9999-01-01T00:00:00.000Z", dailyStoreEnd: "9999-01-01T00:00:00Z" } }],
      cacheExpire: "9999-01-01T00:00:00.000Z" } }, eventsTimeOffsetHrs: 0, cacheIntervalMins: 10, currentTime: now() });

  // a 64x64 grey PNG for image placeholders
  let png;
  function placeholderPng() {
    if (png) return png;
    try {
      const cv = document.createElement("canvas"); cv.width = cv.height = 64;
      const g = cv.getContext("2d"); g.fillStyle = "#5a5a64"; g.fillRect(0, 0, 64, 64);
      g.fillStyle = "#8a8a96"; g.fillRect(8, 8, 48, 48);
      const b = atob(cv.toDataURL("image/png").split(",")[1]);
      png = Uint8Array.from(b, ch => ch.charCodeAt(0));
    } catch (e) { png = new Uint8Array(0); }
    return png;
  }

  function route(req) {
    const u = new URL(req.url), path = u.pathname, m = req.method.toUpperCase();
    let body = {};
    try { const t = new TextDecoder().decode(req.body); body = t.startsWith("{") ? JSON.parse(t) : Object.fromEntries(new URLSearchParams(t)); } catch (e) {}
    let r;
    // accounts
    if (path.endsWith("/account/api/oauth/token")) r = json(token());
    else if (path.endsWith("/account/api/oauth/verify")) r = json(Object.assign(token(), { token: "uewasm-offline" }));
    else if (path.includes("/account/api/oauth/sessions/kill")) r = empty();
    else if (/\/account\/api\/public\/account\/[^/]+\/externalAuths$/.test(path)) r = json([]);
    else if (/\/account\/api\/public\/account\/[^/]+$/.test(path)) r = json(account());
    else if (path.endsWith("/account/api/public/account")) r = json([account()]);
    else if (path.includes("/account/api/epicdomains/ssodomains")) r = json([]);
    // service status and versions
    else if (path.includes("/lightswitch/api/service/bulk/status")) r = json([status]);
    else if (path.includes("/lightswitch/api/service/")) r = json(status);
    else if (path.includes("/versioncheck")) r = json({ type: "NO_UPDATE" });
    else if (path.endsWith("/fortnite/api/version")) r = json({ app: "fortnite", serverDate: now(), overridePropertiesVersion: "unknown", cln: "0",
                                                                build: "0", moduleName: "Fortnite-Core", buildDate: now(), version: "3.6", branch: "Release-3.6", modules: {} });
    else if (path.includes("/waitingroom/")) r = empty();
    // game data
    else if (/\/game\/v2\/profile\/[^/]+\/(client|dedicated_server|public)\/[^/]+$/.test(path))
      r = json(mcp(path.split("/").pop(), u.searchParams.get("profileId") || "athena", body));
    else if (path.includes("/fortnite/api/calendar/v1/timeline")) r = json(calendar());
    else if (path.includes("/storefront/v2/catalog")) r = json({ refreshIntervalHrs: 24, dailyPurchaseHrs: 24, expiration: "9999-12-31T00:00:00.000Z", storefronts: [] });
    else if (path.includes("/storefront/v2/keychain")) r = json([]);
    else if (path.includes("/cloudstorage/system/config")) r = json({});
    else if (/\/cloudstorage\/(system|user\/[^/]+)$/.test(path)) r = json([]);
    else if (path.includes("/cloudstorage/user/") && m === "PUT") { store.cloud = store.cloud || {}; r = empty(); }
    else if (path.includes("/cloudstorage/")) r = empty(m === "GET" ? 404 : 204);
    else if (path.includes("/enabled_features")) r = json([]);
    else if (path.includes("/receipts/")) r = json([]);
    else if (path.includes("/world/info")) r = json({});
    else if (path.includes("/tryPlayOnPlatform/")) r = { status: 200, headers: { "Content-Type": "text/plain" }, body: "true" };
    else if (path.includes("/game/v2/privacy/account/")) r = json({ accountId, optOutOfPublicLeaderboards: false });
    else if (path.includes("/br-inventory/account/")) r = json({ stash: { globalcash: 0 } });
    else if (path.includes("/matchmaking/session/findPlayer/")) r = empty(200);
    else if (path.includes("/statsv2/") || path.includes("/stats/accountId/")) r = json([]);
    else if (path.includes("/content/api/pages/")) r = json({ "jcr:isCheckedOut": true, _title: "Fortnite Game", "jcr:baseVersion": "a7ca237317f1e7", _activeDate: "2018-01-01T00:00:00.000Z",
                                                              lastModified: now(), _locale: "en-US", battleroyalenews: { news: { messages: [] } }, emergencynotice: { news: { messages: [] } } });
    // social
    else if (path.includes("/friends/api/") && path.endsWith("/settings")) r = json({ acceptInvites: "public" });
    else if (path.includes("/friends/api/")) r = json(path.includes("/summary") ? { friends: [], incoming: [], outgoing: [], suggested: [], blocklist: [], settings: {} } : []);
    else if (path.includes("/party/api/v1/") && path.includes("/user/")) r = json({ current: [], pending: [], invites: [], pings: [] });
    else if (path.includes("/party/api/")) r = json({});
    else if (path.includes("/socialban/")) r = json({ bans: [], warnings: [] });
    else if (path.includes("/eulatracking/")) r = empty();
    else if (path.includes("/presence/")) r = json([]);
    else if (path.includes("/datarouter/")) r = empty();
    else if (path.includes("/api/v1/events/")) r = json({ player: { gameId: "Fortnite", accountId, tokens: [], teams: {}, pendingPayouts: [], pendingPenalties: {}, persistentScores: {}, groupIdentity: {} }, events: [], templates: [], scores: [] });
    if (r) return r;
    // anything else: a stand-in of the expected type when placeholders are on, else not found
    if (Module.uewNetPlaceholders) {
      if (/\.(png|jpe?g|webp)$/i.test(path)) return { status: 200, headers: { "Content-Type": "image/png" }, body: placeholderPng() };
      return m === "GET" ? json({}) : empty();
    }
    console.log("[epic] no answer for " + m + " " + path);
    return json({ errorCode: "errors.com.epicgames.common.not_found", errorMessage: "Not available offline", numericErrorCode: 1004 }, 404);
  }

  Module.uewNet = Module.uewNet || {};
  const prev = Module.uewNet.claim;
  Module.uewNet.claim = host => /(^|\.)(epicgames|unrealengine|fortnite)\.(com|net|dev)$/i.test(host) || (prev ? prev(host) : false);
  Module.uewNet.http = req => route(req);
})();
