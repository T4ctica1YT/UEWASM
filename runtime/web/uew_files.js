// Game files fetched on demand in 1 MB blocks (host_remote.c). data/index.txt lists every file;
// data/<id> is its first part and data/<id>.<n> the rest.
(() => {
  const BLOCK = 1 << 20;
  const F = Module.uewFiles = {
    base: Module.uewDataUrl || "data/", part: 16 << 20, cacheMax: (Module.uewCacheMB || 640) << 20,
    files: [], byPath: new Map(), dirs: new Map(), cache: new Map(), cacheBytes: 0, pending: new Map(), next: new Map(),
    fetched: 0,
  };
  const norm = p => p.replace(/\\/g, "/").replace(/\/+/g, "/").replace(/\/$/, "").toLowerCase();
  const key = (id, b) => id * 1048576 + b;
  // something a green thread may be blocked on has finished: end an idle wait now instead of at its timer
  Module.uewWake = () => { const w = Module.uewIdleWake; if (w) { Module.uewIdleWake = null; w(); } };

  F.load = text => {
    const lines = text.split(/\r?\n/);
    const head = lines[0].split(" ");
    if (head[0] != "uewasm-files") throw new Error("bad data/index.txt");
    F.part = +head[2];
    // keyed by the lowercased path; listings keep the names' own case (games build paths from what they list)
    const addDir = (orig, name, e) => {
      const p = norm(orig);
      let d = F.dirs.get(p);
      if (!d) {
        d = new Map();
        F.dirs.set(p, d);
        const i = orig.lastIndexOf("/");
        if (i >= 0) addDir(orig.slice(0, i), orig.slice(i + 1), { dir: true });
      }
      if (name && !d.has(name)) d.set(name, e);
    };
    for (let i = 1; i < lines.length; i++) {
      if (!lines[i]) continue;
      const [size, mtime, flags, path, blocks] = lines[i].split("\t");
      const f = { id: F.files.length, size: +size, mtime: +mtime, stub: flags == "1", z: flags == "2", blocks };
      F.files.push(f);
      const n = norm(path);
      F.byPath.set(n, f);
      const j = path.lastIndexOf("/");
      addDir(path.slice(0, j), path.slice(j + 1), f);
    }
  };
  F.lookup = p => {
    p = norm(p);
    return F.byPath.get(p) || (F.dirs.has(p) ? { dir: true } : null);
  };
  F.list = p => {
    const d = F.dirs.get(norm(p));
    if (!d) return null;
    let s = "";
    for (const [name, e] of d) s += `${name}\t${e.dir ? 1 : 0}\t${e.size || 0}\t${e.mtime || 0}\n`;
    return s;
  };

  const get = k => {
    const v = F.cache.get(k);
    if (v) { F.cache.delete(k); F.cache.set(k, v); }
    return v;
  };
  const put = (k, buf) => {
    if (F.cache.has(k)) return;
    F.cache.set(k, buf);
    F.cacheBytes += buf.byteLength;
    while (F.cacheBytes > F.cacheMax) {
      const [k0, v0] = F.cache.entries().next().value;
      F.cache.delete(k0);
      F.cacheBytes -= v0.byteLength;
    }
  };
  const url = (id, part) => F.base + (part ? `${id}.${part}` : `${id}`);

  // where each block of a file sits: raw files are plain slices, deflated ones follow the size list in the index
  const layout = f => {
    if (f.L) return f.L;
    if (!f.z) {
      const per = F.part / BLOCK, n = Math.max(1, Math.ceil(f.size / BLOCK));
      f.L = Array.from({ length: n }, (_, b) => ({ part: Math.floor(b / per), off: (b % per) * BLOCK, len: Math.min(BLOCK, f.size - b * BLOCK) }));
    } else {
      let part = 0, used = 0;
      f.L = f.blocks.split(",").map(x => {
        const len = parseInt(x, 16);
        if (used && used + len > F.part) { part++; used = 0; }
        const e = { part, off: used, len };
        used += len;
        return e;
      });
    }
    return f.L;
  };
  const inflate = async buf =>
    new Uint8Array(await new Response(new Blob([buf]).stream().pipeThrough(new DecompressionStream("deflate-raw"))).arrayBuffer());
  const unpack = (f, b, buf) => {
    const raw = Math.min(BLOCK, f.size - b * BLOCK);
    return f.z && buf.length != raw ? inflate(buf) : buf;
  };

  async function download(f, b) {
    if (f.stub) return new Uint8Array(Math.min(BLOCK, f.size - b * BLOCK));
    const L = layout(f), e = L[b];
    const whole = L.length == 1;
    for (let attempt = 0; ; attempt++) {
      try {
        const t0 = performance.now();
        F.inflight = (F.inflight || 0) + 1;
        F.maxInflight = Math.max(F.maxInflight || 0, F.inflight);
        const r = await fetch(url(f.id, e.part), whole ? {} : { headers: { Range: `bytes=${e.off}-${e.off + e.len - 1}` } });
        if (!r.ok) throw new Error(`HTTP ${r.status} for ${url(f.id, e.part)}`);
        let buf = new Uint8Array(await r.arrayBuffer());
        F.fetched += buf.length;
        F.requests = (F.requests || 0) + 1;
        F.inflight--;
        F.fetchMs = (F.fetchMs || 0) + performance.now() - t0;
        if (window.uewStats) {
          const st = (F.per || (F.per = new Map())), e = st.get(f.id) || { n: 0, bytes: 0, uniq: new Set() };
          e.n++; e.bytes += buf.length; e.uniq.add(b); st.set(f.id, e);
        }
        if (!whole && r.status == 200) {
          // no Range support on this server: keep every block of the part
          for (let i = 0; i < L.length; i++)
            if (i != b && L[i].part == e.part && !F.cache.has(key(f.id, i)))
              put(key(f.id, i), await unpack(f, i, buf.slice(L[i].off, L[i].off + L[i].len)));
          buf = buf.slice(e.off, e.off + e.len);
        }
        return await unpack(f, b, buf);
      } catch (err_) {
        if (attempt >= 3) throw err_;
        await new Promise(r => setTimeout(r, 500 << attempt));
      }
    }
  }

  const block = (id, b) => {
    const k = key(id, b);
    const hit = get(k);
    if (hit) return hit;
    let p = F.pending.get(k);
    if (!p) {
      p = download(F.files[id], b).then(buf => { F.pending.delete(k); F.failed.delete(k); put(k, buf); Module.uewWake(); return buf; },
                                         e => { F.pending.delete(k); F.failed.add(k); Module.uewWake(); throw e; });
      F.pending.set(k, p);
    }
    return p;
  };

  // several neighbouring blocks in one request (browsers allow only 6 at a time per server, so fewer, bigger ones win)
  const blocks = (id, b0, b1) => {
    const f = F.files[id];
    if (f.stub || b1 <= b0) { for (let b = b0; b <= b1; b++) { const r = block(id, b); if (r instanceof Promise) r.catch(() => {}); } return; }
    const L = layout(f);
    if (L.length == 1) { const r = block(id, b0); if (r instanceof Promise) r.catch(() => {}); return; }
    let run = [];
    const go = () => {
      if (!run.length) return;
      const bs = run;
      run = [];
      if (bs.length == 1) { block(id, bs[0]).catch(() => {}); return; }
      const first = L[bs[0]], lastE = L[bs[bs.length - 1]], from = first.off, to = lastE.off + lastE.len;
      const all = (async () => {
        for (let attempt = 0; ; attempt++) {
          try {
            const t0 = performance.now();
            const r = await fetch(url(f.id, first.part), { headers: { Range: `bytes=${from}-${to - 1}` } });
            if (!r.ok) throw new Error(`HTTP ${r.status} for ${url(f.id, first.part)}`);
            let buf = new Uint8Array(await r.arrayBuffer());
            // a server without Range support sends the whole part
            if (r.status == 200 && buf.length >= to) buf = buf.slice(from, to);
            if (buf.length != to - from) throw new Error(`HTTP ${r.status}: ${buf.length} bytes for ${url(f.id, first.part)}`);
            F.fetched += buf.length;
            F.requests = (F.requests || 0) + 1;
            F.fetchMs = (F.fetchMs || 0) + performance.now() - t0;
            if (window.uewStats) {
              const st = (F.per || (F.per = new Map())), e = st.get(f.id) || { n: 0, bytes: 0, uniq: new Set() };
              e.n++; e.bytes += buf.length; for (const b of bs) e.uniq.add(b); st.set(f.id, e);
            }
            return buf;
          } catch (err_) {
            if (attempt >= 3) throw err_;
            await new Promise(r => setTimeout(r, 500 << attempt));
          }
        }
      })();
      for (const b of bs) {
        const k = key(id, b), e = L[b];
        const pr = all.then(buf => unpack(f, b, buf.slice(e.off - from, e.off - from + e.len)))
          .then(v => { F.pending.delete(k); F.failed.delete(k); put(k, v); Module.uewWake(); return v; },
                e => { F.pending.delete(k); F.failed.add(k); Module.uewWake(); throw e; });
        pr.catch(() => {});
        F.pending.set(k, pr);
      }
    };
    for (let b = b0; b <= b1; b++) {
      const k = key(id, b);
      if (F.cache.has(k) || F.pending.has(k)) { go(); continue; }
      if (run.length && L[run[0]].part != L[b].part) go();
      run.push(b);
    }
    go();
  };

  const ahead = (id, off, len) => {
    const f = F.files[id], end = off + len;
    const seq = F.next.get(id) === off;
    F.next.set(id, end);
    if (!seq) return;
    const last = Math.floor((f.size - 1) / BLOCK), b = Math.floor(end / BLOCK);
    if (b <= last) blocks(id, b, Math.min(last, b + 7));
  };

  // boot plan: the blocks the game read up to its first few hundred frames, in order, kept from the last session.
  // The next boot fetches them a window ahead of the game (starting while the wasm is still compiling).
  const T = { key: "", seen: new Set(), order: [], rec: true, plan: [], pos: 0, q: [], qset: new Set() };
  const pump = () => {
    while (T.pos < T.plan.length && T.q.length < 160) {
      const [id, b0, n] = T.plan[T.pos++];
      if (!F.files[id]) continue;
      const last = Math.floor((F.files[id].size - 1) / BLOCK), b1 = Math.min(last, b0 + n - 1);
      blocks(id, b0, b1);
      for (let b = b0; b <= b1; b++) { const k = key(id, b); if (!T.qset.has(k)) { T.qset.add(k); T.q.push(k); } }
    }
  };
  const touch = (id, b0, b1) => {
    for (let b = b0; b <= b1; b++) {
      const k = key(id, b);
      // the game reached a planned block: everything planned before it is behind us too
      if (T.qset.has(k)) { let x; do { x = T.q.shift(); T.qset.delete(x); } while (x !== k); pump(); }
      if (T.rec && !T.seen.has(k)) { T.seen.add(k); T.order.push(k); }
    }
    if (T.rec && T.order.length > 20000) save();
  };
  const save = () => {
    if (!T.rec) return;
    T.rec = false;
    const runs = [];
    for (const k of T.order) {
      const id = Math.floor(k / 1048576), b = k % 1048576, r = runs[runs.length - 1];
      if (r && r[0] === id && r[1] + r[2] === b) r[2]++;
      else runs.push([id, b, 1]);
    }
    try { localStorage.setItem(T.key, runs.map(r => r.join(":")).join(",")); } catch (e) {}
  };
  F.boot = () => {
    T.key = "uewBoot:" + location.pathname + ":" + F.files.length + ":" + F.files.reduce((a, f) => (a + f.size) % 1e15, 0);
    let saved = "";
    try { saved = localStorage.getItem(T.key) || ""; } catch (e) {}
    T.plan = saved ? saved.split(",").map(r => r.split(":").map(Number)) : [];
    pump();
    const t = setInterval(() => { if ((Module.uewPresents | 0) >= 300) { clearInterval(t); save(); } }, 1000);
  };

  const copy = (bufs, b0, off, len, dst) => {
    let done = 0;
    for (let i = 0; i < bufs.length; i++) {
      const s = i ? 0 : off - b0 * BLOCK, n = Math.min(bufs[i].length - s, len - done);
      HEAPU8.set(bufs[i].subarray(s, s + n), dst + done);
      done += n;
    }
    return done;
  };

  F.readCached = (id, off, len, dst) => {
    dst >>>= 0;
    if (!len) return 0;
    const b0 = Math.floor(off / BLOCK), b1 = Math.floor((off + len - 1) / BLOCK), bufs = [];
    for (let b = b0; b <= b1; b++) {
      const v = get(key(id, b));
      if (!v) return -1;
      bufs.push(v);
    }
    touch(id, b0, b1);
    ahead(id, off, len);
    return copy(bufs, b0, off, len, dst);
  };

  // non-blocking form for green threads: start the downloads, poll ready() (true once every block is in, or one failed)
  F.failed = new Set();
  F.start = (id, off, len) => {
    const b0 = Math.floor(off / BLOCK), b1 = Math.floor((off + len - 1) / BLOCK);
    // a miss in a big file (pak reads hop around): the blocks after it usually follow soon, fetch them in the same request
    const last = Math.floor((F.files[id].size - 1) / BLOCK);
    blocks(id, b0, Math.min(last, b1 + 7));
    touch(id, b0, b1);
    ahead(id, off, len);
  };
  F.ready = (id, off, len) => {
    const b0 = Math.floor(off / BLOCK), b1 = Math.floor((off + len - 1) / BLOCK);
    for (let b = b0; b <= b1; b++) {
      const k = key(id, b);
      if (F.failed.has(k)) return 1;
      if (!get(k)) return 0;
    }
    return 1;
  };

  // debug: which files were downloaded most, and how often blocks came down again after leaving the cache
  F.report = () => {
    const paths = new Map();
    for (const [p, f] of F.byPath) paths.set(f.id, p);
    return [`boot plan ${T.plan.length} runs, at ${T.pos}, ${T.q.length} ahead, recorded ${T.order.length} blocks (${T.rec ? "still recording" : "saved"})`, `${F.requests} requests, ${(F.fetchMs / F.requests).toFixed(1)} ms each on average, up to ${F.maxInflight} at once`].concat([...(F.per || [])].sort((x, y) => y[1].bytes - x[1].bytes).slice(0, 15)
      .map(([id, e]) => `${(e.bytes / 1048576).toFixed(0)} MB in ${e.n} (${e.uniq.size} distinct blocks of ${Math.ceil(F.files[id].size / BLOCK)}) ${paths.get(id)}`)).join("\n");
  };

  F.read = async (id, off, len, dst) => {
    dst >>>= 0;
    const b0 = Math.floor(off / BLOCK), b1 = Math.floor((off + len - 1) / BLOCK), want = [];
    for (let b = b0; b <= b1; b++) want.push(block(id, b));
    touch(id, b0, b1);
    ahead(id, off, len);
    try {
      return copy(await Promise.all(want), b0, off, len, dst);
    } catch (e) {
      err(`[files] ${e.message}`);
      return -1;
    }
  };

  // the index (and the boot plan's downloads) start now, alongside the wasm download and compile
  const index = fetch(F.base + "index.txt")
    .then(r => r.ok ? r.text() : null)
    .then(t => { if (t) { F.load(t); F.boot(); } else err("[files] no data/index.txt, game files unavailable"); })
    .catch(e => err(`[files] ${e.message}`));
  Module.preRun = [].concat(Module.preRun || []);
  Module.preRun.push(() => {
    addRunDependency("uew-files");
    index.finally(() => removeRunDependency("uew-files"));
  });
})();
