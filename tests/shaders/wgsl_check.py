"""Compiles translated WGSL with the browser's WebGPU compiler (headless) and groups the errors."""
import json
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from playwright.sync_api import sync_playwright  # noqa: E402

from uewasm.serve import background  # noqa: E402

root = Path(sys.argv[1])
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 0
files = sorted(root.glob("*.wgsl"))[: limit or None]
(root / "check.html").write_text("""<!doctype html><script>
window.dev = (async () => { const a = await navigator.gpu.requestAdapter(); return a.requestDevice({ requiredLimits: { maxSampledTexturesPerShaderStage: a.limits.maxSampledTexturesPerShaderStage, maxStorageBuffersPerShaderStage: a.limits.maxStorageBuffersPerShaderStage } }); })();
window.run = async (items) => {
  const dev = await window.dev; const bad = [];
  for (const [n, code] of items) {
    const m = dev.createShaderModule({ code });
    const info = await m.getCompilationInfo();
    const errs = info.messages.filter(x => x.type === 'error');
    if (errs.length) bad.push([n, errs.map(e => `${e.lineNum}:${e.linePos} ${e.message}`).join('\\n')]);
  }
  return bad;
};
</script>""")
srv = background(root)
bad = []
with sync_playwright() as p:
    b = p.chromium.launch(headless=True, channel="msedge", args=["--mute-audio", "--enable-unsafe-webgpu", "--ignore-gpu-blocklist"])
    for i in range(0, len(files), 400):
        page = b.new_page()
        page.goto(f"http://localhost:{srv.server_port}/check.html")
        items = [[f.name, f.read_text(encoding="utf-8", errors="replace")] for f in files[i:i + 400]]
        bad += page.evaluate("x => window.run(x)", items)
        page.close()
    b.close()
srv.shutdown()
print(f"compiled {len(files) - len(bad)} / {len(files)}")
kinds, example = Counter(), {}
for n, log in bad:
    first = log.splitlines()[0] if log else "?"
    key = re.sub(r"'[^']*'", "'X'", re.sub(r"\d+", "N", first.split(" ", 1)[-1]))[:120]
    kinds[key] += 1
    example.setdefault(key, (n, first))
for k, v in kinds.most_common(20):
    print(f"{v:6}  {k}\n          e.g. {example[k][0]}: {example[k][1][:200]}")
(root / "wgsl_errors.json").write_text(json.dumps(bad[:3000], indent=1))
