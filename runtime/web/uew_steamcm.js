// A minimal Steam client in the page, for builds made with --steam account: it signs the player in to their own Steam
// account and connects to Steam's servers (the CM WebSocket endpoints, which pages may open directly), so the game gets
// the real SteamID, persona name, and real tickets (GetAuthSessionTicket, GetAuthTicketForWebApi, encrypted app
// tickets) that its online services check with Valve. Lobbies and P2P still go through WebWorks or the relay.
//
// Sign-in: account name and password are sent only to Steam (RSA-encrypted with Steam's key, inside the browser's own
// TLS); Steam Guard codes or approval in the Steam Mobile App as Steam asks. Only Steam's refresh token is kept, in this
// browser's storage, so later launches sign in at once (?steamlogout=1 forgets it). Nothing reaches UEWASM or the host.
//
// Through WISP (build option, or ?steamwisp=1): Steam's TCP protocol to its CMs, carried as a WISP stream, so the page
// never contacts Steam itself. That channel has Steam's own encryption (AES-256 with a key sent RSA-encrypted to Valve's
// published key), so the WISP server only relays ciphertext.
addToLibrary({
  $uewSCM__deps: ['$UTF8ToString', '$stringToUTF8', '$lengthBytesUTF8'],
  $uewSCM: {
    s: null,
    PROTOCOL: 65581,
    EMSG: { Multi: 1, ClientHeartBeat: 703, ClientLogOnResponse: 751, ClientLoggedOff: 757, ClientAccountInfo: 768, ClientGameConnectTokens: 779,
            ClientCMList: 783, ClientGetAppOwnershipTicket: 857, ClientGetAppOwnershipTicketResponse: 858, ClientAuthList: 5432,
            ClientAuthListAck: 5575, ClientGamesPlayed: 742, ClientLogon: 5514, ClientRequestEncryptedAppTicket: 5526,
            ClientRequestEncryptedAppTicketResponse: 5527, ServiceMethodCallFromClientNonAuthed: 9804, ServiceMethodResponse: 147,
            ClientHello: 9805 },
    CMS: ['cmp1-ord1.steamserver.net:443', 'cmp2-ord1.steamserver.net:443', 'cmp1-iad1.steamserver.net:443', 'cmp1-sea1.steamserver.net:443',
          'cmp1-lax1.steamserver.net:443', 'cmp1-fra1.steamserver.net:443', 'cmp1-ams1.steamserver.net:443', 'cmp1-sto3.steamserver.net:443'],

    // ---- protobuf: just enough to write and read Steam's messages ----
    pb: {
      varint(out, v) {
        v = BigInt.asUintN(64, BigInt(v));
        while (v >= 0x80n) { out.push(Number(v & 0x7fn) | 0x80); v >>= 7n; }
        out.push(Number(v));
      },
      // fields: [number, kind, value]; kind: v (varint), b (bool), s (string), y (bytes), q (fixed64), m (nested bytes)
      enc(fields) {
        const out = [], te = new TextEncoder();
        for (const [n, k, v] of fields) {
          if (v === undefined || v === null) continue;
          if (k === 'v' || k === 'b') { this.varint(out, n << 3); this.varint(out, k === 'b' ? (v ? 1 : 0) : v); }
          else if (k === 'q') {
            this.varint(out, n << 3 | 1);
            let x = BigInt.asUintN(64, BigInt(v));
            for (let i = 0; i < 8; i++) { out.push(Number(x & 0xffn)); x >>= 8n; }
          } else {
            const b = k === 's' ? te.encode(v) : v;
            this.varint(out, n << 3 | 2); this.varint(out, b.length);
            for (let i = 0; i < b.length; i++) out.push(b[i]);
          }
        }
        return Uint8Array.from(out);
      },
      // field number -> list of values (BigInt for varint/fixed64, Uint8Array for length-delimited, number for fixed32)
      dec(b) {
        const m = new Map();
        let p = 0;
        const rv = () => { let r = 0n, s = 0n, c; do { c = b[p++]; r |= BigInt(c & 0x7f) << s; s += 7n; } while (c & 0x80); return r; };
        while (p < b.length) {
          const key = Number(rv()), n = key >>> 3, wt = key & 7;
          let v;
          if (wt === 0) v = rv();
          else if (wt === 1) { v = 0n; for (let i = 7; i >= 0; i--) v = v << 8n | BigInt(b[p + i]); p += 8; }
          else if (wt === 2) { const l = Number(rv()); v = b.subarray(p, p + l); p += l; }
          else if (wt === 5) { v = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | b[p + 3] << 24; p += 4; }
          else break;
          if (!m.has(n)) m.set(n, []);
          m.get(n).push(v);
        }
        return m;
      },
      str(m, n) { const v = m.get(n); return v ? new TextDecoder().decode(v[0]) : ''; },
      num(m, n, d = 0) { const v = m.get(n); return v ? Number(BigInt.asIntN(64, BigInt(v[0]))) : d; },
      big(m, n) { const v = m.get(n); return v ? BigInt(v[0]) : 0n; },
      bytes(m, n) { const v = m.get(n); return v ? v[0] : new Uint8Array(0); },
      all(m, n) { return m.get(n) || []; },
    },

    get() {
      if (this.s) return this.s;
      this.s = { state: 0, ws: null, steamid: 0n, session: 0, jobs: new Map(), nextJob: 1n, tokens: [], tickets: [], acks: new Map(),
                 owner: new Map(), name: '', appid: 0, reqs: new Map(), nextReq: 1, seq: 0, heartbeat: 0 };
      return this.s;
    },
    log(t) { console.log('[steam] ' + t); },

    // ---- the connection ----
    connect() {
      const W = Module.uewNet && Module.uewNet.wisp, q = new URLSearchParams(location.search);
      const via = W && (q.has('steamwisp') ? q.get('steamwisp') !== '0' : !!(Module.uewWisp && Module.uewWisp.steam));
      return via ? this.connectTcp(W) : this.connectWs();
    },
    connectWs() {
      const s = this.get();
      let list = [];
      try { list = JSON.parse(localStorage.getItem('uew_steam_cms') || '[]'); } catch (e) { }
      const q = new URLSearchParams(location.search).get('steamcm');
      const hosts = [...new Set([...(q ? [q] : []), ...list, ...this.CMS])];
      return new Promise((resolve, reject) => {
        const tryNext = i => {
          if (i >= hosts.length) { reject(new Error("can't reach Steam's servers")); return; }
          const ws = new WebSocket('wss://' + hosts[i] + '/cmsocket/');
          ws.binaryType = 'arraybuffer';
          const t = setTimeout(() => { ws.onopen = ws.onerror = null; ws.close(); tryNext(i + 1); }, 6000);
          ws.onopen = () => {
            clearTimeout(t);
            s.ws = ws;
            ws.onmessage = e => this.onPacket(new Uint8Array(e.data));
            ws.onclose = () => { if (s.ws === ws) { s.ws = null; if (s.state === 1) { this.log('disconnected from Steam'); s.state = -1; } } };
            this.send(this.EMSG.ClientHello, this.pb.enc([[1, 'v', this.PROTOCOL]]));
            resolve();
          };
          ws.onerror = () => { clearTimeout(t); ws.onopen = ws.onerror = null; tryNext(i + 1); };
        };
        tryNext(0);
      });
    },
    // Valve's public-universe key (SubjectPublicKeyInfo), as every Steam client carries it
    UNIVERSE_KEY: '30819d300d06092a864886f70d010101050003818b0030818702818100dfec1ad62c10662c17353a14b07c59117f9dd3d82b7ae3e015cd191e46e87b8774a2184631a9031479828ee945a24912a923687389cf69a1b16146bdc1bebfd6011bd881d4dc90fbfe4f527366cb9570d7c58eba1c7a3375a1623446bb60b78068fa13a77a8a374b9ec6f45d5f3a99f99ec43ae963a2bb881928e0e714c04289020111',
    TCP_CMS: ['ext1-ord1.steamserver.net:27017', 'ext1-iad1.steamserver.net:27017', 'ext1-sea1.steamserver.net:27017',
              'ext1-fra1.steamserver.net:27017'],
    connectTcp(W) {
      const s = this.get(), sub = crypto.subtle;
      let list = [];
      try { list = JSON.parse(localStorage.getItem('uew_steam_cms_tcp') || '[]'); } catch (e) { }
      const hosts = [...new Set([...list, ...this.TCP_CMS])];
      const hex = h => Uint8Array.from(h.match(/../g), x => parseInt(x, 16));
      const cat = (...a) => { const o = new Uint8Array(a.reduce((n, x) => n + x.length, 0)); let p = 0; for (const x of a) { o.set(x, p); p += x.length; } return o; };
      const crc32 = b => { let c = ~0; for (const x of b) { c ^= x; for (let k = 0; k < 8; k++) c = c >>> 1 ^ (0xEDB88320 & -(c & 1)); } return ~c >>> 0; };
      const zero = new Uint8Array(16), pad = new Uint8Array(16).fill(16);
      return new Promise((resolve, reject) => {
        const tryNext = i => {
          if (i >= hosts.length) { reject(new Error("can't reach Steam's servers through WISP")); return; }
          const [host, port] = hosts[i].split(':');
          const c = W.open(host, +port || 27017, false);
          let inbuf = new Uint8Array(0), stage = 0, aes = null, hmac = null, inq = Promise.resolve(), outq = Promise.resolve();
          const fail = why => { if (stage < 3) { stage = 9; clearTimeout(t); c.close(); this.log(hosts[i] + ': ' + why); tryNext(i + 1); } };
          const t = setTimeout(() => fail('no answer'), 8000);
          const frame = d => { const f = new Uint8Array(8 + d.length), v = new DataView(f.buffer); v.setUint32(0, d.length, true); v.setUint32(4, 0x31305456, true); f.set(d, 8); c.send(f); };
          // one AES block without chaining, from WebCrypto's CBC (which has no ECB): encrypting with a zero IV, and
          // decrypting with a second block made so the padding check passes
          const ecbEnc = async b => new Uint8Array(await sub.encrypt({ name: 'AES-CBC', iv: zero }, aes, b)).subarray(0, 16);
          const ecbDec = async b => {
            const x = new Uint8Array(16); for (let k = 0; k < 16; k++) x[k] = b[k] ^ pad[k];
            return new Uint8Array(await sub.decrypt({ name: 'AES-CBC', iv: zero }, aes, cat(b, await ecbEnc(x))));
          };
          const seal = async plain => {
            const iv = new Uint8Array(16); crypto.getRandomValues(iv.subarray(13));
            iv.set(new Uint8Array(await sub.sign('HMAC', hmac, cat(iv.subarray(13), plain))).subarray(0, 13));
            return cat(await ecbEnc(iv), new Uint8Array(await sub.encrypt({ name: 'AES-CBC', iv }, aes, plain)));
          };
          const open = async d => {
            const iv = await ecbDec(d.subarray(0, 16));
            const plain = new Uint8Array(await sub.decrypt({ name: 'AES-CBC', iv }, aes, d.subarray(16)));
            const h = new Uint8Array(await sub.sign('HMAC', hmac, cat(iv.subarray(13), plain)));
            for (let k = 0; k < 13; k++) if (h[k] !== iv[k]) throw new Error('bad HMAC');
            return plain;
          };
          const handshake = async d => {
            const v = new DataView(d.buffer, d.byteOffset, d.byteLength), emsg = v.getUint32(0, true);
            if (stage === 0 && emsg === 1303) {   // ChannelEncryptRequest: protocol, universe, challenge
              const key = crypto.getRandomValues(new Uint8Array(32));
              const rsa = await sub.importKey('spki', hex(this.UNIVERSE_KEY), { name: 'RSA-OAEP', hash: 'SHA-1' }, false, ['encrypt']);
              const blob = new Uint8Array(await sub.encrypt({ name: 'RSA-OAEP' }, rsa, cat(key, d.subarray(28))));
              aes = await sub.importKey('raw', key, 'AES-CBC', false, ['encrypt', 'decrypt']);
              hmac = await sub.importKey('raw', key.subarray(0, 16), { name: 'HMAC', hash: 'SHA-1' }, false, ['sign']);
              const r = new Uint8Array(20 + 8 + blob.length + 8), rv = new DataView(r.buffer);
              rv.setUint32(0, 1304, true); rv.setBigUint64(4, 0xFFFFFFFFFFFFFFFFn, true); rv.setBigUint64(12, 0xFFFFFFFFFFFFFFFFn, true);
              rv.setUint32(20, 1, true); rv.setUint32(24, 128, true); r.set(blob, 28); rv.setUint32(28 + blob.length, crc32(blob), true);
              stage = 1; frame(r);
            } else if (stage === 1 && emsg === 1305) {   // ChannelEncryptResult
              const er = v.getUint32(20, true);
              if (er !== 1) { fail('channel encryption refused (' + er + ')'); return; }
              stage = 3; clearTimeout(t);
              s.ws = { send: p => { outq = outq.then(async () => frame(await seal(p))).catch(() => { }); } };
              c.end = () => { if (s.ws && s.ws.c === c) { s.ws = null; if (s.state === 1) { this.log('disconnected from Steam'); s.state = -1; } } };
              s.ws.c = c;
              this.log('connected to Steam through WISP (' + hosts[i] + ')');
              this.send(this.EMSG.ClientHello, this.pb.enc([[1, 'v', this.PROTOCOL]]));
              resolve();
            }
          };
          c.end = () => fail('closed');
          c.push = data => {
            inbuf = cat(inbuf, data);
            while (inbuf.length >= 8) {
              const v = new DataView(inbuf.buffer, inbuf.byteOffset), n = v.getUint32(0, true);
              if (v.getUint32(4, true) !== 0x31305456) { fail('not a Steam server'); return; }
              if (inbuf.length < 8 + n) break;
              const d = inbuf.slice(8, 8 + n);
              inbuf = inbuf.slice(8 + n);
              if (stage < 3) inq = inq.then(() => handshake(d)).catch(e => fail(e.message));
              else inq = inq.then(async () => this.onPacket(await open(d))).catch(e => this.log('Steam packet: ' + e.message));
            }
          };
        };
        tryNext(0);
      });
    },
    send(emsg, body, hdr = []) {
      const s = this.get();
      const h = this.pb.enc([[1, 'q', s.steamid || 0x0110000100000000n], [2, 'v', s.session], ...hdr]);
      const p = new Uint8Array(8 + h.length + body.length);
      const dv = new DataView(p.buffer);
      dv.setUint32(0, (emsg | 0x80000000) >>> 0, true);
      dv.setUint32(4, h.length, true);
      p.set(h, 8); p.set(body, 8 + h.length);
      if (s.ws) s.ws.send(p);
    },
    // a request with a job id; resolves with { hdr, body } of the answer
    job(emsg, body, hdr = []) {
      const s = this.get();
      const id = s.nextJob++;
      return new Promise((resolve, reject) => {
        s.jobs.set(id, { resolve, t: setTimeout(() => { s.jobs.delete(id); reject(new Error('Steam did not answer')); }, 20000) });
        this.send(emsg, body, [[10, 'q', id], ...hdr]);
      });
    },
    async service(method, body) {
      const r = await this.job(this.EMSG.ServiceMethodCallFromClientNonAuthed, body, [[12, 's', method]]);
      const er = this.pb.num(r.hdr, 13, 2);
      const known = { 5: 'Wrong account name or password', 65: 'That Steam Guard code is wrong', 84: 'Too many sign-in attempts: wait a while and try again',
                      88: 'That Steam Guard code is wrong', 18: 'No Steam account by that name' };
      if (er !== 1) throw new Error(known[er] || this.pb.str(r.hdr, 14) || 'Steam error ' + er);
      return r.body;
    },
    async onPacket(p) {
      const dv = new DataView(p.buffer, p.byteOffset, p.byteLength);
      const raw = dv.getUint32(0, true), emsg = raw & 0x7fffffff;
      if (!(raw & 0x80000000)) return;   // every message a client gets on a WebSocket CM is protobuf
      const hl = dv.getUint32(4, true);
      const hdr = this.pb.dec(p.subarray(8, 8 + hl)), body = p.subarray(8 + hl);
      if (emsg === this.EMSG.Multi) { await this.onMulti(this.pb.dec(body)); return; }
      this.onMessage(emsg, hdr, body);
    },
    async onMulti(m) {
      let b = this.pb.bytes(m, 2);
      if (this.pb.num(m, 1)) b = new Uint8Array(await new Response(new Blob([b]).stream().pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
      for (let o = 0; o + 4 <= b.length;) {
        const n = b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24;
        await this.onPacket(b.slice(o + 4, o + 4 + n));
        o += 4 + n;
      }
    },
    onMessage(emsg, hdr, body) {
      const s = this.get(), E = this.EMSG, pb = this.pb;
      const target = pb.big(hdr, 11);
      if (target && s.jobs.has(target)) {
        const j = s.jobs.get(target);
        s.jobs.delete(target);
        clearTimeout(j.t);
        j.resolve({ emsg, hdr, body: pb.dec(body) });
        return;
      }
      const m = pb.dec(body);
      switch (emsg) {
        case E.ClientLogOnResponse: {
          const er = pb.num(m, 1, 2);
          if (s.onLogon) s.onLogon(er, hdr, m);
          break;
        }
        case E.ClientLoggedOff: this.log('signed out by Steam (result ' + pb.num(m, 1) + ')'); s.state = -1; break;
        case E.ClientAccountInfo: s.name = pb.str(m, 1) || s.name; break;
        case E.ClientGameConnectTokens: {
          for (const t of pb.all(m, 2)) s.tokens.push(t.slice());
          const keep = pb.num(m, 1, 10);
          while (s.tokens.length > keep) s.tokens.shift();
          break;
        }
        case E.ClientCMList: {
          const ws = pb.all(m, 6).map(v => new TextDecoder().decode(v)).filter(Boolean);
          if (ws.length) try { localStorage.setItem('uew_steam_cms', JSON.stringify(ws.slice(0, 12))); } catch (e) { }
          // TCP CMs: IPv4 addresses (host order) and ports, each list plain or packed
          const nums = n => pb.all(m, n).flatMap(v => {
            if (!(v instanceof Uint8Array)) return [Number(v)];
            const o = []; let p = 0;
            while (p < v.length) { let r = 0, sh = 0, c; do { c = v[p++]; r += (c & 0x7f) * 2 ** sh; sh += 7; } while (c & 0x80); o.push(r); }
            return o;
          });
          const ips = nums(1), ports = nums(2);
          const tcp = ips.map((a, i) => `${a >>> 24}.${a >>> 16 & 255}.${a >>> 8 & 255}.${a & 255}:${ports[i] || 27017}`);
          if (tcp.length) try { localStorage.setItem('uew_steam_cms_tcp', JSON.stringify(tcp.slice(0, 12))); } catch (e) { }
          break;
        }
        case E.ClientAuthListAck: for (const c of pb.all(m, 1)) { const w = s.acks.get(Number(c)); if (w) w(); } break;
      }
    },

    // ---- sign-in ----
    rsa(modHex, expHex, text) {
      const n = BigInt('0x' + modHex), e = BigInt('0x' + expHex);
      const k = modHex.length / 2, msg = new TextEncoder().encode(text);
      const em = new Uint8Array(k);
      em[1] = 2;
      const ps = k - 3 - msg.length;
      const rnd = crypto.getRandomValues(new Uint8Array(ps));
      for (let i = 0; i < ps; i++) em[2 + i] = rnd[i] || 1;
      em.set(msg, k - msg.length);
      let m = 0n;
      for (const b of em) m = m << 8n | BigInt(b);
      let r = 1n, base = m % n, x = e;
      while (x > 0n) { if (x & 1n) r = r * base % n; base = base * base % n; x >>= 1n; }
      const out = new Uint8Array(k);
      for (let i = k - 1; i >= 0; i--) { out[i] = Number(r & 0xffn); r >>= 8n; }
      return btoa(String.fromCharCode(...out));
    },
    device() {
      return this.pb.enc([[1, 's', 'UEWASM (browser)'], [2, 'v', 1], [3, 'v', 20]]);
    },
    async signInWithPassword(account, password, ui) {
      const pb = this.pb;
      const key = await this.service('Authentication.GetPasswordRSAPublicKey#1', pb.enc([[1, 's', account]]));
      const enc = this.rsa(pb.str(key, 1), pb.str(key, 2), password);
      const r = await this.service('Authentication.BeginAuthSessionViaCredentials#1', pb.enc([
        [1, 's', 'UEWASM (browser)'], [2, 's', account], [3, 's', enc], [4, 'v', pb.big(key, 3)], [5, 'b', true], [6, 'v', 1],
        [7, 'v', 1], [8, 's', 'Client'], [9, 'm', this.device()]]));
      const client = pb.big(r, 1), request = pb.bytes(r, 2), steamid = pb.big(r, 5), interval = 2000;
      const kinds = pb.all(r, 4).map(c => pb.num(pb.dec(c), 1));
      // 2 email code, 3 authenticator code, 4 approve in the mobile app, 5 email link
      const codeKind = kinds.includes(3) ? 3 : kinds.includes(2) ? 2 : 0;
      ui.guard(kinds, codeKind ? async code => {
        await this.service('Authentication.UpdateAuthSessionWithSteamGuardCode#1', pb.enc([[1, 'v', client], [2, 'q', steamid], [3, 's', code], [4, 'v', codeKind]]));
      } : null);
      for (let tries = 0; tries < 300; tries++) {
        await new Promise(res => setTimeout(res, interval));
        const p = await this.service('Authentication.PollAuthSessionStatus#1', pb.enc([[1, 'v', client], [2, 'y', request]]));
        const refresh = pb.str(p, 3);
        if (refresh) return { account: pb.str(p, 6) || account, refresh };
      }
      throw new Error('sign-in timed out');
    },
    // sign-in by scanning a QR code with the Steam Mobile App; runs beside the password form until stop.done
    async signInWithQR(ui, stop) {
      const pb = this.pb, sleep = ms => new Promise(r => setTimeout(r, ms));
      for (let sessions = 0; !stop.done && sessions < 20; sessions++) {
        let r;
        try {
          r = await this.service('Authentication.BeginAuthSessionViaQR#1', pb.enc([[1, 's', 'UEWASM (browser)'], [2, 'v', 1], [3, 'm', this.device()], [4, 's', 'Client']]));
        } catch (e) { ui.qrSay("QR sign-in isn't available: " + e.message); return null; }
        let client = pb.big(r, 1);
        const request = pb.bytes(r, 3), iv = r.get(4) ? new Float32Array(Int32Array.of(r.get(4)[0]).buffer)[0] : 0;
        ui.qr(pb.str(r, 2));
        ui.qrSay('Scan with the Steam Mobile App');
        for (;;) {
          await sleep(iv > 0 ? iv * 1000 : 5000);
          if (stop.done) return null;
          let p;
          try { p = await this.service('Authentication.PollAuthSessionStatus#1', pb.enc([[1, 'v', client], [2, 'y', request]])); }
          catch (e) { break; }   // the code expired: start a new one
          if (pb.big(p, 1)) client = pb.big(p, 1);
          if (pb.str(p, 2)) ui.qr(pb.str(p, 2));
          if (pb.num(p, 5)) ui.qrSay('Approve the sign-in on your phone');
          const refresh = pb.str(p, 3);
          if (refresh) return { account: pb.str(p, 6), refresh };
        }
      }
      return null;
    },
    // QR code modules for text (byte mode, error correction M, versions 1-10): { n, dark(x, y) }
    qrCode(text) {
      const data = Array.from(new TextEncoder().encode(text));
      const ECC = [0, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26], BLOCKS = [0, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5];
      const raw = v => { let r = (16 * v + 128) * v + 64; if (v >= 2) { const a = Math.floor(v / 7) + 2; r -= (25 * a - 10) * a - 55; if (v >= 7) r -= 36; } return r; };
      const cap = v => Math.floor(raw(v) / 8) - ECC[v] * BLOCKS[v];
      let ver = 1;
      while (ver <= 10 && 4 + (ver < 10 ? 8 : 16) + 8 * data.length > cap(ver) * 8) ver++;
      if (ver > 10) throw new Error('QR text too long');
      // bits: byte mode, length, data, terminator, padding
      const bits = [], put = (v, n) => { for (let i = n - 1; i >= 0; i--) bits.push(v >>> i & 1); };
      put(4, 4); put(data.length, ver < 10 ? 8 : 16);
      for (const b of data) put(b, 8);
      const cw = cap(ver);
      put(0, Math.min(4, cw * 8 - bits.length));
      put(0, (8 - bits.length % 8) % 8);
      for (let pad = 0xEC; bits.length < cw * 8; pad ^= 0xEC ^ 0x11) put(pad, 8);
      const bytes = [];
      for (let i = 0; i < bits.length; i += 8) bytes.push(bits.slice(i, i + 8).reduce((a, b) => a << 1 | b, 0));
      // Reed-Solomon over GF(256), split into blocks and interleaved
      const mul = (x, y) => { let z = 0; for (let i = 7; i >= 0; i--) { z = z << 1 ^ (z >>> 7) * 0x11D; z ^= (y >>> i & 1) * x; } return z; };
      const deg = ECC[ver], div = new Array(deg).fill(0);
      div[deg - 1] = 1;
      for (let i = 0, root = 1; i < deg; i++, root = mul(root, 2))
        for (let j = 0; j < deg; j++) { div[j] = mul(div[j], root); if (j + 1 < deg) div[j] ^= div[j + 1]; }
      const rem = d => { const r = new Array(deg).fill(0); for (const b of d) { const f = b ^ r.shift(); r.push(0); for (let i = 0; i < deg; i++) r[i] ^= mul(div[i], f); } return r; };
      const nb = BLOCKS[ver], total = Math.floor(raw(ver) / 8), nshort = nb - total % nb, slen = Math.floor(total / nb);
      const blocks = [];
      for (let i = 0, k = 0; i < nb; i++) {
        const d = bytes.slice(k, k + slen - deg + (i < nshort ? 0 : 1));
        k += d.length;
        const e = rem(d);
        if (i < nshort) d.push(0);
        blocks.push(d.concat(e));
      }
      const words = [];
      for (let i = 0; i < blocks[0].length; i++)
        for (let j = 0; j < nb; j++) if (i !== slen - deg || j >= nshort) words.push(blocks[j][i]);
      // function patterns
      const n = ver * 4 + 17, mod = new Uint8Array(n * n), fn = new Uint8Array(n * n);
      const set = (x, y, d) => { mod[y * n + x] = d ? 1 : 0; fn[y * n + x] = 1; };
      for (let i = 0; i < n; i++) { set(6, i, i % 2 === 0); set(i, 6, i % 2 === 0); }
      for (const [cx, cy] of [[3, 3], [n - 4, 3], [3, n - 4]])
        for (let dy = -4; dy <= 4; dy++) for (let dx = -4; dx <= 4; dx++) {
          const x = cx + dx, y = cy + dy, d = Math.max(Math.abs(dx), Math.abs(dy));
          if (x >= 0 && x < n && y >= 0 && y < n) set(x, y, d !== 2 && d !== 4);
        }
      if (ver > 1) {
        const na = Math.floor(ver / 7) + 2, step = Math.ceil((ver * 4 + 4) / (na * 2 - 2)) * 2, pos = [6];
        for (let p = n - 7; pos.length < na; p -= step) pos.splice(1, 0, p);
        for (let i = 0; i < na; i++) for (let j = 0; j < na; j++) {
          if ((i === 0 && j === 0) || (i === 0 && j === na - 1) || (i === na - 1 && j === 0)) continue;
          for (let dy = -2; dy <= 2; dy++) for (let dx = -2; dx <= 2; dx++) set(pos[i] + dx, pos[j] + dy, Math.max(Math.abs(dx), Math.abs(dy)) !== 1);
        }
      }
      const format = mask => {
        const d = mask;   // level M is 00
        let r = d;
        for (let i = 0; i < 10; i++) r = r << 1 ^ (r >>> 9) * 0x537;
        const b = (d << 10 | r) ^ 0x5412, bit = i => (b >>> i & 1) === 1;
        for (let i = 0; i <= 5; i++) set(8, i, bit(i));
        set(8, 7, bit(6)); set(8, 8, bit(7)); set(7, 8, bit(8));
        for (let i = 9; i < 15; i++) set(14 - i, 8, bit(i));
        for (let i = 0; i < 8; i++) set(n - 1 - i, 8, bit(i));
        for (let i = 8; i < 15; i++) set(8, n - 15 + i, bit(i));
        set(8, n - 8, true);
      };
      format(0);
      if (ver >= 7) {
        let r = ver;
        for (let i = 0; i < 12; i++) r = r << 1 ^ (r >>> 11) * 0x1F25;
        const b = ver << 12 | r;
        for (let i = 0; i < 18; i++) { const d = (b >>> i & 1) === 1, a = n - 11 + i % 3, c = Math.floor(i / 3); set(a, c, d); set(c, a, d); }
      }
      // data, in the zigzag column pairs
      for (let right = n - 1, i = 0; right >= 1; right -= 2) {
        if (right === 6) right = 5;
        for (let v = 0; v < n; v++) for (let j = 0; j < 2; j++) {
          const x = right - j, y = (right + 1 & 2) === 0 ? n - 1 - v : v;
          if (!fn[y * n + x] && i < words.length * 8) { mod[y * n + x] = words[i >>> 3] >>> 7 - (i & 7) & 1; i++; }
        }
      }
      // the mask with the lowest penalty
      const masks = [(x, y) => (x + y) % 2 === 0, (x, y) => y % 2 === 0, x => x % 3 === 0, (x, y) => (x + y) % 3 === 0,
                     (x, y) => (Math.floor(x / 3) + Math.floor(y / 2)) % 2 === 0, (x, y) => x * y % 2 + x * y % 3 === 0,
                     (x, y) => (x * y % 2 + x * y % 3) % 2 === 0, (x, y) => ((x + y) % 2 + x * y % 3) % 2 === 0];
      const apply = k => { for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) if (!fn[y * n + x] && masks[k](x, y)) mod[y * n + x] ^= 1; };
      const penalty = () => {
        let p = 0, dark = 0;
        const at = (x, y) => x >= 0 && x < n && y >= 0 && y < n ? mod[y * n + x] : 0;
        for (let pass = 0; pass < 2; pass++)
          for (let a = 0; a < n; a++) {
            let run = 1;
            for (let b = 1; b <= n; b++) {
              const cur = b < n ? (pass ? at(a, b) : at(b, a)) : -1, prev = pass ? at(a, b - 1) : at(b - 1, a);
              if (cur === prev) run++; else { if (run >= 5) p += run - 2; run = 1; }
            }
            for (let b = -4; b < n; b++) {   // finder-like 1:1:3:1:1 with 4 light modules on one side
              const g = k => pass ? at(a, b + k) : at(b + k, a);
              const core = g(4) && !g(5) && g(6) && g(7) && g(8) && !g(9) && g(10);
              if (core && ((!g(0) && !g(1) && !g(2) && !g(3)) || (!g(11) && !g(12) && !g(13) && !g(14)))) p += 40;
            }
          }
        for (let y = 0; y < n - 1; y++) for (let x = 0; x < n - 1; x++) {
          const c = mod[y * n + x];
          if (c === mod[y * n + x + 1] && c === mod[(y + 1) * n + x] && c === mod[(y + 1) * n + x + 1]) p += 3;
        }
        for (const c of mod) dark += c;
        return p + Math.floor(Math.abs(dark * 20 - n * n * 10) / (n * n)) * 10;
      };
      let best = 0, bestP = Infinity;
      for (let k = 0; k < 8; k++) { apply(k); format(k); const q = penalty(); if (q < bestP) { bestP = q; best = k; } apply(k); }
      apply(best); format(best);
      return { n, dark: (x, y) => mod[y * n + x] === 1 };
    },
    machineId() {
      // binary KeyValues: MessageObject { BB3, FF2, 3B3 } hex strings, stable for this browser
      let key = 'uewasm';
      try { key = localStorage.getItem('uew_webworks_device') || key; } catch (e) { }
      const hex = s => Array.from(new TextEncoder().encode(s)).map(b => b.toString(16).padStart(2, '0')).join('').slice(0, 40).padEnd(40, '0');
      const parts = [0x00, ...new TextEncoder().encode('MessageObject'), 0];
      for (const [k, v] of [['BB3', hex(key + 'b')], ['FF2', hex(key + 'f')], ['3B3', hex(key + '3')]])
        parts.push(0x01, ...new TextEncoder().encode(k), 0, ...new TextEncoder().encode(v), 0);
      parts.push(0x08, 0x08);
      return Uint8Array.from(parts);
    },
    logon(account, refresh) {
      const s = this.get(), pb = this.pb;
      return new Promise((resolve, reject) => {
        s.onLogon = (er, hdr, m) => {
          s.onLogon = null;
          if (er !== 1) { reject(new Error('Steam refused the sign-in (result ' + er + ')')); return; }
          s.steamid = pb.big(hdr, 1);
          s.session = pb.num(hdr, 2);
          const beat = pb.num(m, 3, 9);
          clearInterval(s.heartbeat);
          s.heartbeat = setInterval(() => this.send(this.EMSG.ClientHeartBeat, pb.enc([[1, 'b', false]])), beat * 1000);
          resolve();
        };
        this.send(this.EMSG.ClientLogon, pb.enc([
          [1, 'v', this.PROTOCOL], [3, 'v', 0], [5, 'v', 1771], [6, 's', 'english'], [7, 'v', 20], [8, 'b', true], [30, 'y', this.machineId()],
          [50, 's', account], [96, 's', 'UEWASM (browser)'], [102, 'b', true], [108, 's', refresh]]));
        setTimeout(() => { if (s.onLogon) { s.onLogon = null; reject(new Error('Steam did not answer the sign-in')); } }, 20000);
      });
    },

    // the sign-in dialog over the page
    dialog() {
      const box = document.createElement('div');
      box.style.cssText = 'position:fixed;inset:0;z-index:2147483647;display:flex;align-items:center;justify-content:center;background:#000b;font:14px system-ui,sans-serif;color:#dfe3e6';
      box.innerHTML = `<div style="display:flex;flex-wrap:wrap;gap:24px;align-items:flex-start;background:#1b2838;padding:22px 24px;border-radius:8px;max-width:calc(100vw - 32px);box-sizing:border-box;box-shadow:0 8px 32px #000a">
        <form style="width:300px;max-width:100%">
        <div style="font-size:18px;font-weight:600;margin-bottom:4px">Sign in to Steam</div>
        <div style="font-size:12px;color:#8f98a0;margin-bottom:14px">This game uses your Steam account. Your password goes only to Steam.</div>
        <input name="u" placeholder="Steam account name" autocomplete="username" style="width:100%;box-sizing:border-box;margin:4px 0;padding:8px;background:#32404e;border:0;color:#fff;border-radius:3px">
        <input name="p" type="password" placeholder="Password" autocomplete="current-password" style="width:100%;box-sizing:border-box;margin:4px 0;padding:8px;background:#32404e;border:0;color:#fff;border-radius:3px">
        <label style="display:block;margin:8px 0;font-size:12px"><input name="r" type="checkbox" checked> Remember me in this browser</label>
        <div data-guard style="display:none;margin:8px 0"></div>
        <div data-msg style="min-height:18px;font-size:12px;color:#c6d4df;margin:6px 0"></div>
        <button style="width:100%;padding:9px;border:0;border-radius:3px;background:linear-gradient(90deg,#06bfff,#2d73ff);color:#fff;font-weight:600;cursor:pointer">Sign in</button>
        <div style="text-align:right;margin-top:8px"><a href="#" data-skip style="color:#8f98a0;font-size:12px">Play without Steam</a></div>
        </form>
        <div style="width:200px;text-align:center">
          <div style="font-size:12px;font-weight:600;color:#1999ff;letter-spacing:.5px;margin:6px 0 10px">OR SIGN IN WITH QR</div>
          <canvas data-qr width="200" height="200" style="width:200px;height:200px;background:#fff;border-radius:6px"></canvas>
          <div data-qrmsg style="font-size:12px;color:#8f98a0;margin-top:8px;min-height:32px">Getting a code from Steam…</div>
        </div>
      </div>`;
      document.body.appendChild(box);
      const f = box.querySelector('form'), msg = box.querySelector('[data-msg]'), guard = box.querySelector('[data-guard]');
      const canvas = box.querySelector('[data-qr]'), qrmsg = box.querySelector('[data-qrmsg]'), self = this;
      return {
        box, form: f,
        say(t) { msg.textContent = t; },
        qrSay(t) { qrmsg.textContent = t; },
        qr(url) {
          const q = self.qrCode(url), g = canvas.getContext('2d'), cell = Math.floor(canvas.width / (q.n + 8)), off = Math.floor((canvas.width - cell * q.n) / 2);
          g.fillStyle = '#fff'; g.fillRect(0, 0, canvas.width, canvas.height);
          g.fillStyle = '#000';
          for (let y = 0; y < q.n; y++) for (let x = 0; x < q.n; x++) if (q.dark(x, y)) g.fillRect(off + x * cell, off + y * cell, cell, cell);
          canvas.dataset.url = url;
        },
        guard(kinds, submit) {
          guard.style.display = 'block';
          const app = kinds.includes(4) ? 'Approve the sign-in in the Steam Mobile App' : kinds.includes(5) ? 'Open the link Steam emailed you' : '';
          guard.innerHTML = (app ? `<div style="margin-bottom:6px">${app}${submit ? ', or enter a code:' : '.'}</div>` : '') +
            (submit ? `<input name="g" placeholder="${kinds.includes(3) ? 'Steam Guard code from the app' : 'Code Steam emailed you'}" style="width:100%;box-sizing:border-box;padding:8px;background:#32404e;border:0;color:#fff;border-radius:3px">` : '');
          f.querySelector('button').textContent = submit ? 'Submit code' : 'Waiting…';
          f.onsubmit = async e => {
            e.preventDefault();
            const g = f.querySelector('[name=g]');
            if (submit && g && g.value.trim()) { msg.textContent = 'Checking the code…'; try { await submit(g.value.trim()); } catch (x) { msg.textContent = String(x.message || x); } }
          };
        },
        close() { box.remove(); },
      };
    },
    // signs in (stored session, else the dialog); resolves true when signed in
    async start(appid) {
      const s = this.get();
      if (s.starting) return s.starting;
      s.appid = appid;
      return s.starting = (async () => {
        if (/[?&]steamlogout=1/.test(location.search)) try { localStorage.removeItem('uew_steam_session'); } catch (e) { }
        try { await this.connect(); } catch (e) { this.log(e.message); s.state = -1; return false; }
        let saved = null;
        try { saved = JSON.parse(localStorage.getItem('uew_steam_session') || 'null'); } catch (e) { }
        if (saved && saved.account && saved.refresh) {
          try { await this.logon(saved.account, saved.refresh); return this.ready(); }
          catch (e) { this.log('stored sign-in no longer works: ' + e.message); try { localStorage.removeItem('uew_steam_session'); } catch (x) { } await this.connect().catch(() => { }); }
        }
        for (;;) {
          const ui = this.dialog(), stop = { done: false };
          const r = await new Promise(resolve => {
            this.signInWithQR(ui, stop).then(t => { if (t) resolve({ ...t, keep: ui.form.r.checked }); }, e => ui.qrSay(String(e.message || e)));
            ui.box.querySelector('[data-skip]').onclick = e => { e.preventDefault(); resolve(null); };
            ui.form.onsubmit = async e => {
              e.preventDefault();
              const u = ui.form.u.value.trim(), p = ui.form.p.value, keep = ui.form.r.checked;
              if (!u || !p) { ui.say('Enter your account name and password'); return; }
              ui.say('Signing in…');
              try { const t = await this.signInWithPassword(u, p, ui); resolve({ ...t, keep }); }
              catch (x) { ui.say(String(x.message || x)); }
            };
          });
          stop.done = true;
          ui.close();
          if (!r) { this.log('playing without Steam'); s.state = -1; return false; }
          try {
            await this.logon(r.account, r.refresh);
            if (r.keep) try { localStorage.setItem('uew_steam_session', JSON.stringify({ account: r.account, refresh: r.refresh })); } catch (e) { }
            return this.ready();
          } catch (e) { this.log(e.message); await this.connect().catch(() => { }); }
        }
      })();
    },
    async ready() {
      const s = this.get(), pb = this.pb, E = this.EMSG;
      s.state = 1;
      // playing the game, as the Steam client reports it
      this.send(E.ClientGamesPlayed, pb.enc([[1, 'm', pb.enc([[2, 'q', BigInt(s.appid)]])], [2, 'v', 20]]));
      for (let i = 0; i < 20 && !s.name; i++) await new Promise(r => setTimeout(r, 100));
      this.log(`signed in to Steam as ${s.name || '?'} (${s.steamid})`);
      this.ownership(s.appid).catch(e => this.log('no ownership ticket: ' + e.message));
      return true;
    },

    // ---- tickets ----
    async ownership(appid) {
      const s = this.get(), pb = this.pb;
      if (s.owner.has(appid)) return s.owner.get(appid);
      const r = await this.job(this.EMSG.ClientGetAppOwnershipTicket, pb.enc([[1, 'v', appid]]));
      if (pb.num(r.body, 1, 2) !== 1) throw new Error('Steam says this account does not own app ' + appid);
      const t = pb.bytes(r.body, 3).slice();
      s.owner.set(appid, t);
      return t;
    },
    crc32(b) {
      let c = ~0;
      for (let i = 0; i < b.length; i++) { c ^= b[i]; for (let k = 0; k < 8; k++) c = c >>> 1 ^ (0xEDB88320 & -(c & 1)); }
      return ~c >>> 0;
    },
    // an auth ticket (type 2 session, 5 web API) registered with Steam, combined with the ownership ticket
    ticket(appid, type, identity) {
      const s = this.get(), pb = this.pb;
      const owner = s.owner.get(appid), token = s.tokens.shift();
      if (!owner || !token) return null;
      const a = new Uint8Array(4 + token.length + 28), dv = new DataView(a.buffer);
      dv.setUint32(0, token.length, true); a.set(token, 4);
      let o = 4 + token.length;
      dv.setUint32(o, 24, true); dv.setUint32(o + 4, 1, true); dv.setUint32(o + 8, type, true);
      a.set(crypto.getRandomValues(new Uint8Array(8)), o + 12);
      dv.setUint32(o + 20, performance.now() >>> 0, true); dv.setUint32(o + 24, ++s.seq, true);
      const crc = this.crc32(a);
      s.tickets.push({ appid, crc, ticket: a, secret: identity ? new TextEncoder().encode('str:' + identity + '\0') : null });
      this.sendAuthList();
      const raw = a.length + 4 + owner.length, size = type === 5 ? Math.max(raw, 2560) : raw;
      const out = new Uint8Array(size);
      out.set(a);
      new DataView(out.buffer).setUint32(a.length, owner.length, true);
      out.set(owner, a.length + 4);
      if (size > raw) out.set(crypto.getRandomValues(new Uint8Array(size - raw)), raw);
      return { crc, bytes: out };
    },
    sendAuthList() {
      const s = this.get(), pb = this.pb;
      const apps = [...new Set(s.tickets.map(t => t.appid))];
      const fields = [[1, 'v', s.tokens.length]];
      for (const t of s.tickets) fields.push([4, 'm', pb.enc([[4, 'q', BigInt(t.appid)], [6, 'v', t.crc], [7, 'y', t.ticket], [8, 'y', t.secret]])]);
      for (const a of apps) fields.push([5, 'v', a]);
      this.send(this.EMSG.ClientAuthList, pb.enc(fields), [[10, 'q', s.nextJob++]]);
    },
    cancel(crc) {
      const s = this.get();
      s.tickets = s.tickets.filter(t => t.crc !== crc);
      this.sendAuthList();
    },
    async encryptedTicket(appid, userdata) {
      const pb = this.pb;
      const r = await this.job(this.EMSG.ClientRequestEncryptedAppTicket, pb.enc([[1, 'v', appid], [2, 'y', userdata]]));
      if (pb.num(r.body, 2, 2) !== 1) throw new Error('no encrypted app ticket (result ' + pb.num(r.body, 2, 2) + ')');
      return pb.bytes(r.body, 3).slice();   // the serialized EncryptedAppTicket, as Steam hands it to games
    },
    // async requests the runtime polls: status 0 waiting, 1 done, 2 failed
    request(fn) {
      const s = this.get(), id = s.nextReq++, r = { status: 0, bytes: null, crc: 0 };
      s.reqs.set(id, r);
      fn().then(v => { r.bytes = v.bytes || v; r.crc = v.crc || 0; r.status = 1; }, e => { this.log(e.message); r.status = 2; });
      return id;
    },
  },

  // 1 when this build signs in to a Steam account (--steam account); starts the sign-in
  scm_start__proxy: 'sync', scm_start__deps: ['$uewSCM'], scm_start__sig: 'ii',
  scm_start: function (appid) {
    const cfg = Module.uewSteam || {};
    const mode = new URLSearchParams(location.search).get('steam') || cfg.mode;
    if (mode !== 'account') return 0;
    Module.uewSteamClient = uewSCM;   // for the debug panel and tests
    uewSCM.start(appid);
    return 1;
  },
  // 0 signing in, 1 signed in, -1 not signed in (declined, offline)
  scm_state__proxy: 'sync', scm_state__deps: ['$uewSCM'], scm_state__sig: 'i',
  scm_state: function () { return uewSCM.get().state; },
  scm_self__proxy: 'sync', scm_self__deps: ['$uewSCM'], scm_self__sig: 'ipi',
  scm_self: function (buf, cap) { const s = uewSCM.get(); stringToUTF8(String(s.steamid), buf, cap); return 1; },
  scm_name__proxy: 'sync', scm_name__deps: ['$uewSCM'], scm_name__sig: 'ipi',
  scm_name: function (buf, cap) { stringToUTF8(uewSCM.get().name || 'Player', buf, cap); return 1; },
  // a session (type 2) or web API (type 5) ticket now, when the ownership ticket is at hand: its length, 0 when not
  scm_ticket__proxy: 'sync', scm_ticket__deps: ['$uewSCM'], scm_ticket__sig: 'iiippip',
  scm_ticket: function (appid, type, identity, buf, cap, crcp) {
    const t = uewSCM.ticket(appid, type, identity ? UTF8ToString(identity) : '');
    if (!t || t.bytes.length > cap) return 0;
    HEAPU8.set(t.bytes, buf);
    if (crcp) HEAPU32[crcp >> 2] = t.crc;
    return t.bytes.length;
  },
  // a web API ticket, waiting for the ownership ticket if needed: a request id
  scm_ticket_async__proxy: 'sync', scm_ticket_async__deps: ['$uewSCM'], scm_ticket_async__sig: 'iiip',
  scm_ticket_async: function (appid, type, identity) {
    const id = identity ? UTF8ToString(identity) : '';
    return uewSCM.request(async () => {
      await uewSCM.ownership(appid);
      for (let i = 0; i < 50 && !uewSCM.get().tokens.length; i++) await new Promise(r => setTimeout(r, 100));
      const t = uewSCM.ticket(appid, type, id);
      if (!t) throw new Error('no game connect token from Steam');
      return t;
    });
  },
  scm_eticket_async__proxy: 'sync', scm_eticket_async__deps: ['$uewSCM'], scm_eticket_async__sig: 'iipi',
  scm_eticket_async: function (appid, data, len) {
    const u = HEAPU8.slice(data, data + len);
    return uewSCM.request(() => uewSCM.encryptedTicket(appid, u));
  },
  scm_req_status__proxy: 'sync', scm_req_status__deps: ['$uewSCM'], scm_req_status__sig: 'ii',
  scm_req_status: function (id) { const r = uewSCM.get().reqs.get(id); return r ? r.status : 2; },
  // the finished request's bytes (and ticket CRC): its length; the request is kept until scm_req_free
  scm_req_take__proxy: 'sync', scm_req_take__deps: ['$uewSCM'], scm_req_take__sig: 'iipip',
  scm_req_take: function (id, buf, cap, crcp) {
    const r = uewSCM.get().reqs.get(id);
    if (!r || r.status !== 1 || !r.bytes) return 0;
    const n = Math.min(cap, r.bytes.length);
    if (buf) HEAPU8.set(r.bytes.subarray(0, n), buf);
    if (crcp) HEAPU32[crcp >> 2] = r.crc;
    return r.bytes.length;
  },
  scm_req_free__proxy: 'sync', scm_req_free__deps: ['$uewSCM'], scm_req_free__sig: 'vi',
  scm_req_free: function (id) { uewSCM.get().reqs.delete(id); },
  scm_cancel__proxy: 'sync', scm_cancel__deps: ['$uewSCM'], scm_cancel__sig: 'vi',
  scm_cancel: function (crc) { uewSCM.cancel(crc >>> 0); },
});
