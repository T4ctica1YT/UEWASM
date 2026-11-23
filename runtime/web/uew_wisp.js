// Real network connections through a WISP server (github.com/MercuryWorkshop/wisp-protocol): TCP and UDP streams
// multiplexed over one WebSocket. With a WISP server configured (--wisp / --wisp-url at build time, ?wisp=URL in the page
// address, ?wisp=0 to turn it off) every host name and address the game uses that no offline backend takes goes out
// for real: the game's own HTTPS, the EOS SDK, game servers. TLS is the game's own (its OpenSSL), end to end, so the
// WISP server only relays encrypted bytes. The runtime's sockets reach this through Module.uewNet.wisp (uew_host.js).
(() => {
  if (typeof location === "undefined" || location.protocol === "file:") return;
  const q = new URLSearchParams(location.search);
  const cfg = Module.uewWisp || {};
  let url = q.has("wisp") ? q.get("wisp") : cfg.url || "";
  if (!url || url === "0") return;
  if (url === "origin") url = location.origin.replace(/^http/i, "ws") + "/wisp/";
  if (!/\/$/.test(url)) url += "/";
  const debug = /[?&]netdebug=1/.test(location.search);
  const TCP = 1, UDP = 2, CONNECT = 1, DATA = 2, CONTINUE = 3, CLOSE = 4;
  const enc = new TextEncoder();
  let ws = null, opened = false, initial = null, nextId = 1, waiting = [];
  const streams = new Map();

  function frame(type, id, payload) {
    const f = new Uint8Array(5 + payload.length);
    f[0] = type;
    new DataView(f.buffer).setUint32(1, id, true);
    f.set(payload, 5);
    return f;
  }
  function put(f) {
    if (opened) ws.send(f);
    else waiting.push(f);
  }
  function flush(s) {
    while (s.pending.length && s.buf > 0) { put(frame(DATA, s.id, s.pending.shift())); s.buf--; }
  }
  function end(s, why) {
    if (!streams.has(s.id)) return;
    streams.delete(s.id);
    if (debug) console.log(`[wisp] ${s.host}:${s.port} closed (${why})`);
    s.c.end();
  }
  function connect() {
    opened = false; initial = null;
    ws = new WebSocket(url);
    ws.binaryType = "arraybuffer";
    ws.onopen = () => {
      opened = true;
      console.log("[wisp] connected to " + url);
      for (const f of waiting.splice(0)) ws.send(f);
    };
    ws.onmessage = e => {
      const d = new Uint8Array(e.data);
      if (d.length < 5) return;
      const dv = new DataView(d.buffer, d.byteOffset, d.byteLength);
      const type = d[0], id = dv.getUint32(1, true);
      if (type === CONTINUE) {
        const n = dv.getUint32(5, true);
        if (id === 0) {
          initial = n;
          for (const s of streams.values()) if (s.buf === null) { s.buf = n; flush(s); }
        } else {
          const s = streams.get(id);
          if (s) { s.buf = n; flush(s); }
        }
        return;
      }
      const s = streams.get(id);
      if (!s) return;
      if (type === DATA) s.c.push(d.slice(5));
      else if (type === CLOSE) end(s, "reason 0x" + (d[5] || 0).toString(16));
    };
    ws.onclose = ws.onerror = () => {
      if (ws === null) return;
      const was = opened;
      ws = null; opened = false;
      if (!was) console.log("[wisp] can't reach " + url);
      for (const s of [...streams.values()]) end(s, "WISP connection lost");
      waiting = [];
    };
  }

  Module.uewNet = Module.uewNet || {};
  Module.uewNet.wisp = {
    url,
    // everything but this machine: the server decides what it will reach
    ok: host => !/^(localhost|127\.|0\.0\.0\.0$|::1$)/i.test(host),
    open(host, port, udp) {
      if (!ws) connect();
      const id = nextId++;
      const c = {};
      const s = { id, host, port, udp, c, buf: udp ? Infinity : initial, pending: [] };
      c.send = data => {
        if (!streams.has(id)) return;
        if (s.udp) put(frame(DATA, id, data));
        else { s.pending.push(data); if (s.buf !== null) flush(s); }
      };
      c.close = () => {
        if (!streams.has(id)) return;
        streams.delete(id);
        put(frame(CLOSE, id, Uint8Array.of(0x02)));
      };
      streams.set(id, s);
      const name = enc.encode(host);
      const p = new Uint8Array(3 + name.length);
      p[0] = udp ? UDP : TCP;
      p[1] = port & 255; p[2] = port >> 8;
      p.set(name, 3);
      put(frame(CONNECT, id, p));
      if (debug) console.log(`[wisp] open ${udp ? "udp" : "tcp"} ${host}:${port}`);
      return c;
    },
  };
})();
