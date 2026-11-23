// WebWorks (github.com/Stratus-Games/Webworks): a self-hosted backend for Steam-style features. With --steam webworks
// (the default) the runtime's steam_api answers a game's Steam calls through it: a guest session for the player's
// identity, lobbies with their data, members and chat, and P2P packets over the realtime socket. The page keeps the
// state; the runtime asks for it and collects events (ww_event) when the game runs its Steam callbacks.
// Steam IDs and lobby IDs pass as decimal strings (they don't fit a double).
addToLibrary({
  $uewWW__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  $uewWW: {
    w: null,
    get() {
      if (this.w) return this.w;
      const cfg = Module.uewSteam || {};
      const q = new URLSearchParams(location.search);
      let url = (q.get('webworks') || (cfg.relay ? location.origin : cfg.url) || 'http://localhost:3000').trim().replace(/\/+$/, '').replace(/\/api(\/.*)?$/i, '');
      const mode = q.get('steam') || cfg.mode || 'webworks';
      this.w = { mode, url, api: url + '/api', state: mode === 'webworks' || mode === 'account' ? 0 : -1, me: '0', name: 'Player', token: '',
                 lobbies: new Map(), names: new Map(), events: [], reqs: new Map(), nextReq: 1, packets: new Map(),
                 peers: new Set(), chats: new Map(), ws: null, wsQueue: [] };
      return this.w;
    },
    put(s, buf, cap) {
      if (!buf || cap <= 0) return lengthBytesUTF8(s);
      stringToUTF8(s, buf, cap);
      return Math.min(lengthBytesUTF8(s), cap - 1);
    },
    lobby(w, id) {
      let l = w.lobbies.get(id);
      if (!l) { l = { id, ownerId: '0', maxMembers: 0, members: [], data: {}, memberData: {}, joinable: true }; w.lobbies.set(id, l); }
      return l;
    },
    absorb(w, s) {
      if (!s || !s.id) return null;
      const l = this.lobby(w, String(s.id));
      l.ownerId = String(s.ownerId || l.ownerId);
      l.maxMembers = s.maxMembers || l.maxMembers;
      if (s.data) l.data = Object.assign({}, s.data);
      if (Array.isArray(s.members) && (s.members.length || !s.remote)) {
        l.members = s.members.map(m => String(m.steamId));
        for (const m of s.members) if (m.name) w.names.set(String(m.steamId), m.name);
      }
      if (s.memberData) l.memberData = JSON.parse(JSON.stringify(s.memberData));
      if (typeof s.joinable === 'boolean') l.joinable = s.joinable;
      l.memberCount = s.memberCount != null ? s.memberCount : l.members.length;
      return l;
    },
    async call(w, method, path, body) {
      const opt = { method, headers: { 'X-Webworks-Session': w.token } };
      if (body !== undefined) { opt.headers['Content-Type'] = 'application/json'; opt.body = JSON.stringify(body); }
      const r = await fetch(w.api + path, opt);
      let j = null;
      try { j = await r.json(); } catch (e) { }
      if (!r.ok) throw new Error((j && j.error) || ('HTTP ' + r.status));
      return j || {};
    },
    // a request the game waits on through a Steam call handle: ww_req_status reports 1 done, 2 failed
    request(w, fn) {
      const id = w.nextReq++;
      const r = { status: 0, result: null };
      w.reqs.set(id, r);
      if (w.state !== 1) { r.status = 2; r.error = 'no WebWorks server'; return id; }
      fn().then(v => { r.result = v; r.status = 1; }, e => { r.error = String(e && e.message || e); r.status = 2; console.log('[webworks] ' + r.error); });
      return id;
    },
    fire(w, method, path, body) { if (w.state !== 1) return; this.call(w, method, path, body).catch(e => console.log('[webworks] ' + path + ': ' + (e && e.message || e))); },
    send(w, msg) {
      const s = JSON.stringify(msg);
      if (w.ws && w.ws.readyState === 1) w.ws.send(s);
      else w.wsQueue.push(s);
    },
    connect(w) {
      const ws = new WebSocket(w.url.replace(/^http/i, 'ws') + '/realtime?session=' + encodeURIComponent(w.token));
      w.ws = ws;
      ws.onopen = () => { for (const s of w.wsQueue.splice(0)) ws.send(s); };
      ws.onmessage = e => { try { this.onMessage(w, JSON.parse(e.data)); } catch (x) { console.log('[webworks] bad message: ' + x); } };
      ws.onclose = () => { if (w.state === 1) setTimeout(() => this.connect(w), 2000); };
    },
    onMessage(w, m) {
      const id = m.lobbyId != null ? String(m.lobbyId) : '';
      const who = m.steamId != null ? String(m.steamId) : (m.from != null ? String(m.from) : '0');
      if (m.name && who !== '0') w.names.set(who, m.name);
      switch (m.type) {
        case 'LobbyMemberJoined': {
          const l = this.lobby(w, id);
          if (!l.members.includes(who)) l.members.push(who);
          w.events.push([1, id, who]);
          break;
        }
        case 'LobbyMemberLeave': case 'LobbyMemberDisconnected': case 'LobbyMemberKicked': {
          const l = this.lobby(w, id);
          l.members = l.members.filter(x => x !== who);
          delete l.memberData[who];
          if (m.ownerId) l.ownerId = String(m.ownerId);
          w.events.push([2, id, who, m.type === 'LobbyMemberLeave' ? '2' : m.type === 'LobbyMemberKicked' ? '8' : '4']);
          break;
        }
        case 'LobbyDataChanged': {
          const l = this.lobby(w, id);
          if (m.value === null || m.value === undefined) delete l.data[m.key]; else l.data[m.key] = String(m.value);
          w.events.push([3, id, id]);
          break;
        }
        case 'LobbyMemberDataChanged': {
          const l = this.lobby(w, id);
          (l.memberData[who] = l.memberData[who] || {})[m.key] = m.value == null ? '' : String(m.value);
          w.events.push([3, id, who]);
          break;
        }
        case 'LobbyOwnerChanged': this.lobby(w, id).ownerId = String(m.ownerId || who); w.events.push([3, id, id]); break;
        case 'LobbyChat': {
          const bytes = Uint8Array.from(atob(m.data || ''), ch => ch.charCodeAt(0));
          w.chats.set(id + ':' + m.chatId, { who, bytes, type: m.entryType || 1 });
          if (w.chats.size > 256) w.chats.delete(w.chats.keys().next().value);
          w.events.push([5, id, who, String(m.chatId)]);
          break;
        }
        case 'GameInvite': case 'LobbyInvite':
          if (id) w.events.push([8, id, who]);
          break;
        case 'P2PPacket': {
          const ch = m.channel | 0;
          const bytes = Uint8Array.from(atob(m.data || ''), x => x.charCodeAt(0));
          if (!w.peers.has(who)) { w.peers.add(who); w.events.push([6, who]); }
          let q = w.packets.get(ch);
          if (!q) w.packets.set(ch, q = []);
          q.push([who, bytes]);
          if (q.length > 4096) q.shift();
          break;
        }
        case 'P2PSessionRequest': if (!w.peers.has(who)) { w.peers.add(who); w.events.push([6, who]); } break;
        case 'P2PConnectionFailed': w.events.push([7, who, '4']); break;
        case 'PeerDisconnected': w.peers.delete(who); w.events.push([7, who, '2']); break;
        case 'PersonaStateChange': w.events.push([9, who]); break;
      }
    },
    deviceKey() {
      const k = 'uew_webworks_device';
      let v = null;
      try { v = localStorage.getItem(k); } catch (e) { }
      if (!v || !/^[0-9a-f]{32}$/.test(v)) {
        v = Array.from(crypto.getRandomValues(new Uint8Array(16)), b => b.toString(16).padStart(2, '0')).join('');
        try { localStorage.setItem(k, v); } catch (e) { }
      }
      return v;
    },
  },

  // this browser's lasting device key (32 hex digits): the EOS stand-in's player identity, and WebWorks' too
  ww_device__proxy: 'sync', ww_device__deps: ['$uewWW'], ww_device__sig: 'ipi',
  ww_device: function (buf, cap) { return uewWW.put(uewWW.deviceKey(), buf, cap); },
  // mode: 1 WebWorks, 0 the offline Steam; starts the guest session the first time
  // with --steam account the player first signs in to their Steam account (uew_steamcm.js): that identity is the one
  // the game sees, and the one the WebWorks server or relay is told for lobbies and P2P
  ww_init__proxy: 'sync', ww_init__deps: ['$uewWW', '$uewSCM'], ww_init__sig: 'ii',
  ww_init: function (appid) {
    const w = uewWW.get();
    if (w.mode !== 'webworks' && w.mode !== 'account') return 0;
    if (w.started) return 1;
    w.started = true;
    w.appId = String(appid);
    const guest = () => {
      const name = w.account ? w.account.name : new URLSearchParams(location.search).get('name') || undefined;
      fetch(w.api + '/User/GuestSession', { method: 'POST', headers: { 'Content-Type': 'application/json' },
                                            body: JSON.stringify({ appId: w.appId, deviceKey: uewWW.deviceKey(), name,
                                                                   steamId: w.account ? w.account.steamid : undefined }) })
        .then(r => r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)))
        .then(j => {
          w.token = j.sessionToken;
          w.me = w.account ? w.account.steamid : String(j.steamId);
          w.name = w.account ? w.account.name : j.name || 'Player';
          w.names.set(w.me, w.name);
          w.state = 1;
          console.log('[webworks] signed in to ' + w.url + ' as ' + w.name + ' (' + w.me + ')');
          uewWW.connect(w);
        })
        .catch(e => {
          // no server: a local Steam session (the Steam account's identity, or a lasting one from the device key), so
          // games that need a signed-in user carry on; lobbies and P2P fail as with no connection
          if (w.account) { w.me = w.account.steamid; w.name = w.account.name; }
          else {
            const acct = parseInt(uewWW.deviceKey().slice(0, 8), 16) & 0x7fffffff;
            w.me = String(76561197960265728n + BigInt(acct));
            w.name = new URLSearchParams(location.search).get('name') || 'Player';
          }
          w.names.set(w.me, w.name);
          w.state = 2;
          console.log('[webworks] no server at ' + w.url + ' (' + (e && e.message || e) + '): local Steam session as ' + w.name);
        });
    };
    if (w.mode === 'account')
      uewSCM.start(appid).then(ok => {
        const s = uewSCM.get();
        if (ok) w.account = { steamid: String(s.steamid), name: s.name || 'Player' };
        guest();
      });
    else guest();
    return 1;
  },
  // 0 signing in, 1 signed in, 2 local session (no server), -1 offline mode
  ww_state__proxy: 'sync', ww_state__deps: ['$uewWW'], ww_state__sig: 'i',
  ww_state: function () { return uewWW.get().state; },
  ww_self__proxy: 'sync', ww_self__deps: ['$uewWW'], ww_self__sig: 'ipi',
  ww_self: function (buf, cap) { return uewWW.put(uewWW.get().me, buf, cap); },
  ww_name__proxy: 'sync', ww_name__deps: ['$uewWW'], ww_name__sig: 'ippi',
  ww_name: function (id, buf, cap) {
    const w = uewWW.get(), s = UTF8ToString(id);
    return uewWW.put(s === w.me || s === '0' ? w.name : (w.names.get(s) || ''), buf, cap);
  },

  ww_req_status__proxy: 'sync', ww_req_status__deps: ['$uewWW'], ww_req_status__sig: 'ii',
  ww_req_status: function (id) { const r = uewWW.get().reqs.get(id); return r ? r.status : 2; },
  // a finished request's lobby id (create, join) or i-th listed lobby; done frees it
  ww_req_lobby__proxy: 'sync', ww_req_lobby__deps: ['$uewWW'], ww_req_lobby__sig: 'iiipi',
  ww_req_lobby: function (id, i, buf, cap) {
    const r = uewWW.get().reqs.get(id);
    const v = r && r.result;
    const s = !v ? '0' : Array.isArray(v) ? (v[i] || '0') : String(v);
    return uewWW.put(s, buf, cap);
  },
  ww_req_count__proxy: 'sync', ww_req_count__deps: ['$uewWW'], ww_req_count__sig: 'ii',
  ww_req_count: function (id) { const r = uewWW.get().reqs.get(id); return r && Array.isArray(r.result) ? r.result.length : 0; },
  ww_req_free__proxy: 'sync', ww_req_free__deps: ['$uewWW'], ww_req_free__sig: 'vi',
  ww_req_free: function (id) { uewWW.get().reqs.delete(id); },

  ww_create_lobby__proxy: 'sync', ww_create_lobby__deps: ['$uewWW'], ww_create_lobby__sig: 'iii',
  ww_create_lobby: function (type, max) {
    const w = uewWW.get();
    const vis = ['private', 'friends', 'public', 'invisible'][type] || 'public';
    return uewWW.request(w, async () => {
      const s = await uewWW.call(w, 'POST', '/Matchmaking/CreateLobby', { maxMembers: max, visibility: vis });
      const l = uewWW.absorb(w, s);
      if (vis !== 'public') { await uewWW.call(w, 'POST', '/Matchmaking/LobbyVisibility', { lobbyId: l.id, mode: vis }); }
      return l.id;
    });
  },
  ww_join_lobby__proxy: 'sync', ww_join_lobby__deps: ['$uewWW'], ww_join_lobby__sig: 'ip',
  ww_join_lobby: function (id) {
    const w = uewWW.get(), s = UTF8ToString(id);
    return uewWW.request(w, async () => uewWW.absorb(w, await uewWW.call(w, 'POST', '/Matchmaking/JoinLobby', { lobbyId: s })).id);
  },
  // filters: JSON {slots, max, filters, numeric, near} as the server takes it
  ww_lobby_list__proxy: 'sync', ww_lobby_list__deps: ['$uewWW'], ww_lobby_list__sig: 'ip',
  ww_lobby_list: function (json) {
    const w = uewWW.get(), body = JSON.parse(UTF8ToString(json));
    return uewWW.request(w, async () => (await uewWW.call(w, 'POST', '/Matchmaking/LobbyList', body)).lobbies.map(s => uewWW.absorb(w, s).id));
  },
  ww_lobby_refresh__proxy: 'sync', ww_lobby_refresh__deps: ['$uewWW'], ww_lobby_refresh__sig: 'ip',
  ww_lobby_refresh: function (id) {
    const w = uewWW.get(), s = UTF8ToString(id);
    uewWW.call(w, 'GET', '/Matchmaking/Lobby?lobbyId=' + encodeURIComponent(s))
      .then(j => { uewWW.absorb(w, j); w.events.push([3, s, s]); }, () => w.events.push([4, s]));
    return 1;
  },
  ww_lobby_leave__proxy: 'sync', ww_lobby_leave__deps: ['$uewWW'], ww_lobby_leave__sig: 'vp',
  ww_lobby_leave: function (id) {
    const w = uewWW.get(), s = UTF8ToString(id);
    uewWW.fire(w, 'POST', '/Matchmaking/LeaveLobby', { lobbyId: s });
    w.lobbies.delete(s);
  },
  // what: 0 lobby data, 1 own member data, 2 delete lobby data
  ww_lobby_set__proxy: 'sync', ww_lobby_set__deps: ['$uewWW'], ww_lobby_set__sig: 'vippp',
  ww_lobby_set: function (what, id, key, value) {
    const w = uewWW.get(), s = UTF8ToString(id), k = UTF8ToString(key), v = value ? UTF8ToString(value) : '';
    const l = uewWW.lobby(w, s);
    if (what === 0) { l.data[k] = v; uewWW.fire(w, 'POST', '/Matchmaking/LobbyData', { lobbyId: s, key: k, value: v }); }
    else if (what === 1) { (l.memberData[w.me] = l.memberData[w.me] || {})[k] = v; uewWW.fire(w, 'POST', '/Matchmaking/MemberData', { lobbyId: s, key: k, value: v }); }
    else { delete l.data[k]; uewWW.fire(w, 'DELETE', '/Matchmaking/LobbyData', { lobbyId: s, key: k }); }
  },
  // what: 0 type (Steam's ELobbyType), 1 joinable, 2 member limit, 3 owner (steam id string in sval)
  ww_lobby_setting__proxy: 'sync', ww_lobby_setting__deps: ['$uewWW'], ww_lobby_setting__sig: 'vipip',
  ww_lobby_setting: function (what, id, val, sval) {
    const w = uewWW.get(), s = UTF8ToString(id), l = uewWW.lobby(w, s);
    if (what === 0) uewWW.fire(w, 'POST', '/Matchmaking/LobbyVisibility', { lobbyId: s, mode: ['private', 'friends', 'public', 'invisible'][val] || 'public' });
    else if (what === 1) { l.joinable = !!val; uewWW.fire(w, 'POST', '/Matchmaking/LobbyJoinable', { lobbyId: s, joinable: !!val }); }
    else if (what === 2) { l.maxMembers = val; uewWW.fire(w, 'POST', '/Matchmaking/LobbyMaxMembers', { lobbyId: s, maxMembers: val }); }
    else if (what === 3) { const o = UTF8ToString(sval); l.ownerId = o; uewWW.fire(w, 'POST', '/Matchmaking/LobbyOwner', { lobbyId: s, ownerId: o }); }
  },
  // what: 0 member count, 1 member limit, 2 data count, 3 joinable
  ww_lobby_int__proxy: 'sync', ww_lobby_int__deps: ['$uewWW'], ww_lobby_int__sig: 'iip',
  ww_lobby_int: function (what, id) {
    const l = uewWW.get().lobbies.get(UTF8ToString(id));
    if (!l) return what === 3 ? 0 : 0;
    return what === 0 ? (l.members.length || l.memberCount || 0) : what === 1 ? l.maxMembers : what === 2 ? Object.keys(l.data).length : (l.joinable ? 1 : 0);
  },
  // what: 0 owner, 1 i-th member, 2 i-th data key, 3 i-th data value
  ww_lobby_str__proxy: 'sync', ww_lobby_str__deps: ['$uewWW'], ww_lobby_str__sig: 'iipipi',
  ww_lobby_str: function (what, id, i, buf, cap) {
    const l = uewWW.get().lobbies.get(UTF8ToString(id));
    let s = what < 2 ? '0' : '';
    if (l) {
      if (what === 0) s = l.ownerId;
      else if (what === 1) s = l.members[i] || '0';
      else { const k = Object.keys(l.data)[i]; if (k !== undefined) s = what === 2 ? k : String(l.data[k]); }
    }
    return uewWW.put(s, buf, cap);
  },
  // a lobby value (member '' for lobby data); -1 when the key isn't set
  ww_lobby_get__proxy: 'sync', ww_lobby_get__deps: ['$uewWW'], ww_lobby_get__sig: 'ippppi',
  ww_lobby_get: function (id, member, key, buf, cap) {
    const w = uewWW.get(), l = w.lobbies.get(UTF8ToString(id));
    if (!l) return -1;
    const m = member ? UTF8ToString(member) : '', k = UTF8ToString(key);
    const src = m ? (l.memberData[m] || {}) : l.data;
    if (!(k in src)) return -1;
    return uewWW.put(String(src[k]), buf, cap);
  },
  ww_chat_send__proxy: 'sync', ww_chat_send__deps: ['$uewWW'], ww_chat_send__sig: 'ippi',
  ww_chat_send: function (id, data, len) {
    const w = uewWW.get();
    const b = HEAPU8.subarray(data, data + len);
    let s = '';
    for (let i = 0; i < b.length; i += 8192) s += String.fromCharCode.apply(null, b.subarray(i, i + 8192));
    w.ws && w.ws.readyState === 1 ? uewWW.send(w, { type: 'LobbyChat', lobbyId: UTF8ToString(id), data: btoa(s) })
                                  : uewWW.fire(w, 'POST', '/Matchmaking/ChatSend', { lobbyId: UTF8ToString(id), data: btoa(s) });
    return 1;
  },
  // a chat entry's bytes into buf (its sender into who); its length, or -1
  ww_chat_get__proxy: 'sync', ww_chat_get__deps: ['$uewWW'], ww_chat_get__sig: 'ipipipi',
  ww_chat_get: function (id, chat, buf, cap, who, whocap) {
    const e = uewWW.get().chats.get(UTF8ToString(id) + ':' + chat);
    if (!e) return -1;
    if (who) uewWW.put(e.who, who, whocap);
    const n = Math.min(cap, e.bytes.length);
    HEAPU8.set(e.bytes.subarray(0, n), buf);
    return n;
  },

  ww_p2p_send__proxy: 'sync', ww_p2p_send__deps: ['$uewWW'], ww_p2p_send__sig: 'ippii',
  ww_p2p_send: function (to, data, len, channel) {
    const w = uewWW.get();
    if (w.state !== 1) return 0;
    const b = HEAPU8.subarray(data, data + len);
    let s = '';
    for (let i = 0; i < b.length; i += 8192) s += String.fromCharCode.apply(null, b.subarray(i, i + 8192));
    uewWW.send(w, { type: 'P2PPacket', to: UTF8ToString(to), channel, data: btoa(s) });
    return 1;
  },
  // the next packet's size on a channel, or -1
  ww_p2p_peek__proxy: 'sync', ww_p2p_peek__deps: ['$uewWW'], ww_p2p_peek__sig: 'ii',
  ww_p2p_peek: function (channel) {
    const q = uewWW.get().packets.get(channel);
    return q && q.length ? q[0][1].length : -1;
  },
  ww_p2p_read__proxy: 'sync', ww_p2p_read__deps: ['$uewWW'], ww_p2p_read__sig: 'iipipi',
  ww_p2p_read: function (channel, buf, cap, who, whocap) {
    const q = uewWW.get().packets.get(channel);
    if (!q || !q.length) return -1;
    const [from, bytes] = q.shift();
    const n = Math.min(cap, bytes.length);
    HEAPU8.set(bytes.subarray(0, n), buf);
    uewWW.put(from, who, whocap);
    return n;
  },
  // what: 0 accept, 1 close
  ww_p2p_session__proxy: 'sync', ww_p2p_session__deps: ['$uewWW'], ww_p2p_session__sig: 'vip',
  ww_p2p_session: function (what, peer) {
    const w = uewWW.get(), s = UTF8ToString(peer);
    if (what === 0) w.peers.add(s); else w.peers.delete(s);
    uewWW.fire(w, 'POST', what === 0 ? '/Networking/AcceptSession' : '/Networking/CloseSession', { peerSteamId: s });
  },
  ww_presence__proxy: 'sync', ww_presence__deps: ['$uewWW'], ww_presence__sig: 'vpp',
  ww_presence: function (key, value) {
    const w = uewWW.get();
    if (!key) { uewWW.fire(w, 'DELETE', '/Friends/RichPresence'); return; }
    uewWW.fire(w, 'POST', '/Friends/RichPresence', { key: UTF8ToString(key), value: value ? UTF8ToString(value) : '' });
  },
  // the interface version the game's own steam_api was built with (found in it at build time), for games that use
  // the classic accessors (SteamUser()...) and so never name a version themselves
  ww_iface_version__proxy: 'sync', ww_iface_version__deps: ['$uewWW'], ww_iface_version__sig: 'ippi',
  ww_iface_version: function (family, buf, cap) {
    const f = UTF8ToString(family);
    const norm = v => v.toLowerCase().replace(/_?interface_?(version|v)/, '').replace(/_+$/, '').replace(/\d+$/, '');
    const v = ((Module.uewSteam || {}).versions || []).find(x => norm(x) === f) || '';
    return uewWW.put(v, buf, cap);
  },
  // the next event: its type (0 none) and up to three string fields, 64 bytes apart in buf
  ww_event__proxy: 'sync', ww_event__deps: ['$uewWW'], ww_event__sig: 'ip',
  ww_event: function (buf) {
    const e = uewWW.get().events.shift();
    if (!e) return 0;
    for (let i = 1; i < 4; i++) uewWW.put(e[i] !== undefined ? String(e[i]) : '0', buf + 64 * (i - 1), 64);
    return e[0];
  },
});
