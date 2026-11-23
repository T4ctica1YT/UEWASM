"""Serving a web build: the work is done by uewserve (Rust, rustlift/src/bin/uewserve.rs), which also runs the built-in
relay for online games. This starts it."""
import subprocess
import sys
import threading

from .recomp.build import rust_tool


def _exe():
    exe = rust_tool("uewserve")
    if exe is None:
        raise RuntimeError("uewserve isn't built and there's no Rust toolchain to build it (https://rustup.rs)")
    return str(exe)


def _cmd(root, port, logfile=None, host="127.0.0.1", relay=None, ranges=True, wisp=None, wisp_allow=None):
    cmd = [_exe(), str(root), "--port", str(port), "--host", host] + ([] if ranges else ["--no-ranges"])
    if wisp is not None:
        cmd.append("--wisp" if wisp else "--no-wisp")
    if wisp_allow:
        cmd += ["--wisp-allow", wisp_allow]
    if logfile:
        cmd += ["--log", str(logfile)]
    if relay is not None:
        cmd.append("--relay" if relay else "--no-relay")
    return cmd


def _run(cmd):
    p = subprocess.Popen(cmd)
    try:
        p.wait()
    except KeyboardInterrupt:
        p.terminate()


def serve(root, port=8080, logfile=None, host="127.0.0.1", relay=None, wisp=None, wisp_allow=None):
    _run(_cmd(root, port, logfile, host, relay, wisp=wisp, wisp_allow=wisp_allow))


def relay(host="0.0.0.0", port=3000, wisp=False, wisp_allow=None):
    """the relay alone (and the WISP server with wisp), for builds made with --relay-url / --wisp-url"""
    _run([_exe(), "--relay-only", "--host", host, "--port", str(port)] + (["--wisp"] if wisp else [])
         + (["--wisp-allow", wisp_allow] if wisp_allow else []))


class Background:
    """a server running beside a test: .server_port, .shutdown()"""

    def __init__(self, root, port=0, relay=None, ranges=True, wisp=None):
        self.proc = subprocess.Popen(_cmd(root, port, relay=relay, ranges=ranges, wisp=wisp), stdout=subprocess.PIPE, text=True)
        self.server_port = None
        for line in self.proc.stdout:
            if line.startswith("@@port "):
                self.server_port = int(line.split()[1])
                break
        if self.server_port is None:
            raise RuntimeError("uewserve didn't start")
        threading.Thread(target=lambda: [None for _ in self.proc.stdout], daemon=True).start()

    def shutdown(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def background(root, port=0, relay=None, ranges=True, wisp=None):
    """ranges=False: answer byte-range requests with the whole file (as some hosts do)"""
    return Background(root, port, relay, ranges, wisp)


if __name__ == "__main__":
    serve(sys.argv[1])
