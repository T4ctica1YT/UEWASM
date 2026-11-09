// Debug panel: \ or Insert shows it. Live numbers, switches that apply on reload, and tools that save a log, dump the
// green threads or capture a frame to a file.
(() => {
  if (typeof window === "undefined" || typeof document === "undefined") return;
  const q = new URLSearchParams(location.search);
  if (q.get("capture") === "1") window.uewCaptureFrame = 1 << 30;

  // every console line, so the whole session can be saved even when the console drops or truncates it
  const lines = [];
  const keep = (args) => {
    lines.push(`${(performance.now() / 1000).toFixed(1)} ${Array.from(args).map(a => typeof a === "string" ? a : String(a)).join(" ")}`);
    if (lines.length > 200000) lines.splice(0, 50000);
  };
  for (const k of ["log", "warn", "error"]) {
    const orig = console[k].bind(console);
    console[k] = (...a) => { keep(a); orig(...a); };
  }

  const save = (name, text, type) => {
    const a = document.createElement("a");
    a.href = URL.createObjectURL(new Blob([text], { type }));
    a.download = name;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  };
  const stamp = () => new Date().toISOString().replace(/[:.]/g, "-").slice(0, 19);

  const SWITCHES = [
    ["swvideo", "Software video decoding"],
    ["syncshaders", "Compile shaders when first drawn"],
    ["skipshaders", "Drop draws while shaders compile (can lose effects)"],
    ["syncreadback", "Wait for every GPU readback"],
    ["noresolve", "Skip ResolveSubresource"],
    ["capture", "Enable frame capture"],
  ];

  let panel = null, timer = 0;

  // UE developer console: the runtime attaches UE's own console to the game viewport once the engine is up, so it retries
  // until then. The result line comes from the runtime log.
  const conAuto = () => { try { return localStorage.getItem("uewConsole") === "1"; } catch (e) { return false; } };
  let conTimer = 0;
  const console_on = (say) => {
    if (conTimer) return;
    let tries = 0;
    const go = () => {
      if (!window.Module || !Module._uew_ue_console) return;
      const ok = Module._uew_ue_console();
      const last = lines.filter(l => l.includes("[ue console]")).pop() || "";
      say && say(last.replace(/.*\[ue console\] /, "UE console: "));
      if (ok || ++tries > 60) { clearInterval(conTimer); conTimer = 0; }
    };
    conTimer = setInterval(go, 3000);
    go();
  };
  // after a reload the panel may never be opened: start it from here when asked to
  if (conAuto()) setTimeout(() => console_on(null), 5000);
  const css = `#uewdbg{position:fixed;right:8px;top:8px;z-index:10;width:360px;max-height:calc(100% - 16px);overflow:auto;
    background:#101418ee;color:#dde;border:1px solid #3a4652;border-radius:6px;font:12px ui-monospace,Consolas,monospace;padding:10px}
    #uewdbg h3{margin:0 0 6px;font:600 13px system-ui,sans-serif;color:#9fd}#uewdbg button{font:12px system-ui,sans-serif;margin:2px 4px 2px 0;
    padding:4px 8px;background:#243040;color:#dde;border:1px solid #3a4652;border-radius:4px;cursor:pointer}#uewdbg button:hover{background:#2e3e52}
    #uewdbg label{display:block;margin:2px 0;cursor:pointer}#uewdbg pre{white-space:pre-wrap;margin:4px 0;color:#bbc;font-size:11px}
    #uewdbg .sec{margin-top:8px;border-top:1px solid #2a343e;padding-top:6px}`;

  const build = () => {
    const st = document.createElement("style");
    st.textContent = css;
    document.head.appendChild(st);
    panel = document.createElement("div");
    panel.id = "uewdbg";
    panel.innerHTML = `<h3>UEWASM debug <span style="color:#778;font-weight:400">(\\ or Insert closes)</span></h3>
      <pre id="uewdbg-live">…</pre>
      <div class="sec"><b>Game speed</b> (sound is muted above 1x)<br>
        ${[1, 2, 4, 8].map(v => `<button data-speed="${v}">${v}x</button>`).join("")}</div>
      <div class="sec"><b>Unreal Engine</b><br>
        <button id="uewdbg-con">Enable UE dev console</button>
        <label><input type="checkbox" id="uewdbg-conauto"> Enable it on every launch</label></div>
      <div class="sec"><b>Switches</b> (apply on reload)
        ${SWITCHES.map(([k, t]) => `<label><input type="checkbox" data-k="${k}" ${q.get(k) === "1" ? "checked" : ""}> ${t}</label>`).join("")}
        <button id="uewdbg-reload">Reload with these</button></div>
      <div class="sec"><b>Tools</b><br>
        <button id="uewdbg-log">Save log</button><button id="uewdbg-thr">Dump threads</button><button id="uewdbg-cap">Capture frame</button>
        <br><button id="uewdbg-sexp">Export saves</button><button id="uewdbg-simp">Import saves</button>
        <input id="uewdbg-sfile" type="file" accept=".json" style="display:none">
        <pre id="uewdbg-msg"></pre></div>
      <div class="sec"><b>Recent hitches</b><pre id="uewdbg-hitch"></pre></div>`;
    document.body.appendChild(panel);
    for (const ev of ["mousedown", "mouseup", "click", "keydown", "keyup", "wheel"]) panel.addEventListener(ev, e => e.stopPropagation());
    const msg = t => { panel.querySelector("#uewdbg-msg").textContent = t; };
    for (const b of panel.querySelectorAll("button[data-speed]")) b.onclick = () => {
      if (!window.Module || !Module._uew_set_speed) return msg("game not running yet");
      Module._uew_set_speed(+b.dataset.speed);
      window.uewSpeed = +b.dataset.speed;
      msg(`game speed ${b.dataset.speed}x`);
    };
    const auto = panel.querySelector("#uewdbg-conauto");
    auto.checked = conAuto();
    auto.onchange = () => { try { localStorage.setItem("uewConsole", auto.checked ? "1" : ""); } catch (e) {} if (auto.checked) console_on(msg); };
    panel.querySelector("#uewdbg-con").onclick = () => console_on(msg);
    panel.querySelector("#uewdbg-reload").onclick = () => {
      const p = new URLSearchParams(location.search);
      for (const box of panel.querySelectorAll("input[data-k]")) box.checked ? p.set(box.dataset.k, "1") : p.delete(box.dataset.k);
      location.search = p.toString();
    };
    panel.querySelector("#uewdbg-log").onclick = () => { save(`uewasm-log-${stamp()}.txt`, lines.join("\n"), "text/plain"); msg(`saved ${lines.length} lines`); };
    panel.querySelector("#uewdbg-thr").onclick = () => {
      if (!window.Module || !Module._uew_threads) return msg("game not running yet");
      Module._uew_threads();
      msg("thread list written to the log (Save log to keep it)");
    };
    // the whole save folder (game saves and settings) as one JSON file, and back
    panel.querySelector("#uewdbg-sexp").onclick = () => {
      const root = window.Module && Module.uewSave;
      if (!root || typeof FS === "undefined") return msg("no save folder");
      const out = {};
      const walk = d => {
        for (const n of FS.readdir(d)) {
          if (n === "." || n === "..") continue;
          const p = d + "/" + n;
          if (FS.isDir(FS.stat(p).mode)) walk(p);
          else { const b = FS.readFile(p); let s = ""; for (let i = 0; i < b.length; i += 32768) s += String.fromCharCode.apply(null, b.subarray(i, i + 32768)); out[p.slice(root.length + 1)] = btoa(s); }
        }
      };
      walk(root);
      save(`uewasm-saves-${stamp()}.json`, JSON.stringify(out), "application/json");
      msg(`exported ${Object.keys(out).length} files`);
    };
    const pick = panel.querySelector("#uewdbg-sfile");
    panel.querySelector("#uewdbg-simp").onclick = () => pick.click();
    pick.onchange = async () => {
      const f = pick.files[0];
      if (!f) return;
      const root = Module.uewSave, files = JSON.parse(await f.text());
      for (const [rel, b64] of Object.entries(files)) {
        const p = root + "/" + rel, bin = atob(b64), a = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
        FS.mkdirTree(p.slice(0, p.lastIndexOf("/")));
        FS.writeFile(p, a);
      }
      msg(`imported ${Object.keys(files).length} files; reloading`);
      FS.syncfs(false, () => location.reload());
    };
    panel.querySelector("#uewdbg-cap").onclick = () => {
      const g = window.Module && Module.uewg;
      if (!g) return msg("game not running yet");
      if (q.get("capture") !== "1") return msg("tick 'Enable frame capture' and reload first");
      Module.uewgCaptured = null;
      window.uewCaptureFrame = g.frames + 3;
      msg("capturing…");
      const wait = setInterval(() => {
        if (!Module.uewgCaptured) return;
        clearInterval(wait);
        save(`uewasm-frame-${stamp()}.json`, JSON.stringify(Module.uewgCaptured), "application/json");
        msg("frame saved");
      }, 250);
    };
  };

  let lastN = 0, lastT = performance.now();
  const refresh = () => {
    if (!panel || panel.style.display === "none") return;
    const now = performance.now(), n = (window.Module && Module.uewPresents) || 0;
    const fps = (n - lastN) * 1000 / Math.max(1, now - lastT);
    lastN = n; lastT = now;
    const g = window.Module && Module.uewg, V = window.Module && Module.uewVpx;
    const gpu = (lines.find(l => l.includes("[webgpu] GPU:")) || "").replace(/.*GPU: /, "").replace(/;.*/, "") || "?";
    const heap = typeof HEAPU8 !== "undefined" ? (HEAPU8.length / 1048576).toFixed(0) + " MB" : "?";
    panel.querySelector("#uewdbg-live").textContent =
      `FPS ${fps.toFixed(0)}   frames ${n}\nGPU ${gpu}\nshaders compiling ${g ? g.pendingPipes || 0 : 0}   draws skipped ${g && g.skip ? g.skip.nopipe : 0}\n` +
      `speed ${window.uewSpeed || 1}x
` + `video ${V ? `hardware, ${V.s.size} open stream(s)` : q.get("swvideo") === "1" ? "software (switch)" : "software"}\nwasm memory ${heap}`;
    panel.querySelector("#uewdbg-hitch").textContent = lines.filter(l => l.includes("[hitch]") || l.includes("[watchdog]")).slice(-6).join("\n") || "none";
  };

  const toggle = () => {
    if (!panel) build();
    else panel.style.display = panel.style.display === "none" ? "" : "none";
    clearInterval(timer);
    if (panel.style.display !== "none") { refresh(); timer = setInterval(refresh, 500); }
  };
  addEventListener("keydown", e => {
    if (window.Module && Module.uewDebugPanel === false) return;
    if (e.code === "Backslash" || e.code === "Insert") { toggle(); e.preventDefault(); e.stopImmediatePropagation(); }
  }, true);
  addEventListener("keyup", e => { if (window.Module && Module.uewDebugPanel === false) return; if (e.code === "Backslash" || e.code === "Insert") { e.preventDefault(); e.stopImmediatePropagation(); } }, true);
})();
