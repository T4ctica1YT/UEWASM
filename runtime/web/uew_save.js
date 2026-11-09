// The save folder lives in IndexedDB so progress and settings survive reloads.
(() => {
  const dir = Module.uewSave;
  if (!dir) return;
  let busy = false;
  const flush = () => {
    if (busy) return;
    busy = true;
    FS.syncfs(false, e => { busy = false; if (e) err(`[save] ${e}`); });
  };
  Module.preRun = [].concat(Module.preRun || []);
  Module.preRun.push(() => {
    FS.mkdirTree(dir);
    FS.mount(IDBFS, {}, dir);
    addRunDependency("uew-save");
    FS.syncfs(true, e => {
      if (e) err(`[save] ${e}`);
      // tests: window.uewSeedFiles = { "path/under/the/save/folder": base64 } puts files in place before the game starts
      const seed = typeof window !== "undefined" && window.uewSeedFiles;
      if (seed) for (const [rel, b64] of Object.entries(seed)) {
        const p = dir + "/" + rel, bin = atob(b64), a = new Uint8Array(bin.length);
        for (let i = 0; i < bin.length; i++) a[i] = bin.charCodeAt(i);
        FS.mkdirTree(p.slice(0, p.lastIndexOf("/")));
        FS.writeFile(p, a);
      }
      removeRunDependency("uew-save");
      setInterval(flush, 5000);
      addEventListener("pagehide", flush);
      addEventListener("visibilitychange", () => { if (document.visibilityState == "hidden") flush(); });
    });
  });
})();
