"""Compiles every translated shader with the browser's real GLSL ES compiler (headless) and groups the errors."""
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
files = sorted(root.glob("*.glsl"))
if limit:
    files = files[:limit]
bundles = []
for i in range(0, len(files), 500):
    name = f"bundle_{i // 500:03d}.json"
    (root / name).write_text(json.dumps([[f.name, f.read_text(encoding="utf-8", errors="replace")] for f in files[i:i + 500]]))
    bundles.append(name)
(root / "check.html").write_text("""<!doctype html><script>
window.run = async (bundles) => {
  const gl = new OffscreenCanvas(4, 4).getContext('webgl2');
  const bad = []; let total = 0;
  for (const b of bundles) {
    for (const [n, src] of await (await fetch(b)).json()) {
      total++;
      const s = gl.createShader(n.startsWith('vs') ? gl.VERTEX_SHADER : gl.FRAGMENT_SHADER);
      gl.shaderSource(s, src); gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) bad.push([n, gl.getShaderInfoLog(s)]);
      gl.deleteShader(s);
    }
  }
  return { total, bad };
};
</script>""")
srv = background(root)
with sync_playwright() as p:
    b = p.chromium.launch(headless=True, channel="msedge", args=["--mute-audio", "--ignore-gpu-blocklist"])
    page = b.new_page()
    page.goto(f"http://localhost:{srv.server_port}/check.html")
    res = page.evaluate("b => window.run(b)", bundles)
    b.close()
srv.shutdown()
for name in bundles:
    (root / name).unlink()
bad = res["bad"]
print(f"compiled {res['total'] - len(bad)} / {res['total']}")
kinds = Counter()
example = {}
for n, log in bad:
    first = next((l for l in log.splitlines() if "ERROR" in l), log.splitlines()[0] if log else "?")
    key = re.sub(r"'[^']*'", "'X'", re.sub(r"\d+", "N", first.split("ERROR:", 1)[-1].strip()))
    kinds[key] += 1
    example.setdefault(key, (n, first))
for k, v in kinds.most_common(25):
    print(f"{v:6}  {k}   e.g. {example[k][0]}: {example[k][1][:160]}")
(root / "webgl_errors.json").write_text(json.dumps(bad[:2000], indent=1))
