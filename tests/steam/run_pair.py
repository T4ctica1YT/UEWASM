"""Runs a web build of steamtest.exe twice in headless Edge against a WebWorks server: one page hosts, one joins.
    python tests/steam/run_pair.py <web dir> [seconds]
Each page has its own browser context, so each signs in as its own player. Passes when the joining page prints DONE."""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from playwright.sync_api import sync_playwright  # noqa: E402

from uewasm import serve  # noqa: E402


def main():
    web = sys.argv[1]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 120
    srv = serve.background(web)
    base = f"http://localhost:{srv.server_port}/index.html"
    logs = {"host": [], "join": []}
    ok = False
    with sync_playwright() as p:
        b = p.chromium.launch(headless=True, channel="msedge", args=["--mute-audio", "--enable-unsafe-webgpu", "--ignore-gpu-blocklist"])
        pages = {}
        for role in ("host", "join"):
            ctx = b.new_context()
            pg = ctx.new_page()
            pg.on("console", lambda m, r=role: (logs[r].append(m.text), print(f"[{r}] {m.text}", flush=True))
                  if any(k in m.text for k in ("OK", "FAIL", "DONE", "callback", "[steam]", "[webworks]", "rror")) else None)
            pg.goto(f"{base}?arg={role}&steamdebug=1", timeout=0, wait_until="commit")
            pages[role] = pg
            if role == "host":
                t0 = time.time()
                while time.time() - t0 < 30 and not any("lobby" in l and "created" in l for l in logs["host"]):
                    pg.wait_for_timeout(500)
        t0 = time.time()
        while time.time() - t0 < limit:
            if any("DONE" in l for l in logs["join"]):
                ok = True
                break
            if any(l.startswith("FAIL") for l in logs["join"] + logs["host"]):
                break
            pages["join"].wait_for_timeout(500)
        pages["host"].wait_for_timeout(1500)
        b.close()
    srv.shutdown()
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
