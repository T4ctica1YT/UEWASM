import os
import re
import struct
from dataclasses import dataclass, field
from pathlib import Path

from . import pe as pemod

SYSTEM = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32"
SYSTEM32 = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "SysWOW64"
RUNTIME_DLLS = {"ucrtbase.dll", "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll",
                "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "concrt140.dll", "vccorlib140.dll", "msvcrt.dll"}
# Stateless functions that are faster as host code. Anything that touches CRT state stays recompiled.
PURE = {"memcpy", "memmove", "memset", "memcmp", "memchr", "strlen", "strcmp", "strncmp", "strchr", "strrchr", "strstr",
        "wcslen", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sqrt", "exp", "log", "log10", "pow", "fmod",
        "floor", "ceil", "sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "atan2f", "sqrtf", "expf", "logf", "log10f",
        "powf", "fmodf", "floorf", "ceilf", "hypot", "hypotf", "sinh", "cosh", "tanh", "round", "roundf", "trunc", "truncf",
        "log2", "log2f", "exp2", "exp2f", "cbrt", "ldexp", "frexp", "modf", "modff"}
THUNK_BASE = 0x10000
ORDINALS = {"xaudio2_9redist.dll": {1: "XAudio2Create", 2: "CreateAudioReverb", 3: "CreateAudioVolumeMeter", 4: "CreateFX",
                                   5: "X3DAudioCalculate", 6: "X3DAudioInitialize"},
            "ws2_32.dll": {1: "accept", 2: "bind", 3: "closesocket", 4: "connect", 5: "getpeername", 6: "getsockname", 7: "getsockopt",
                           8: "htonl", 9: "htons", 10: "ioctlsocket", 11: "inet_addr", 12: "inet_ntoa", 13: "listen", 14: "ntohl",
                           15: "ntohs", 16: "recv", 17: "recvfrom", 18: "select", 19: "send", 20: "sendto", 21: "setsockopt",
                           22: "shutdown", 23: "socket", 51: "gethostbyaddr", 52: "gethostbyname", 53: "getprotobyname",
                           55: "getservbyname", 57: "gethostname", 111: "WSAGetLastError", 112: "WSASetLastError",
                           115: "WSAStartup", 116: "WSACleanup", 151: "__WSAFDIsSet"},
            "dsound.dll": {1: "DirectSoundCreate", 2: "DirectSoundEnumerateA", 3: "DirectSoundEnumerateW", 6: "DirectSoundCaptureCreate",
                           11: "DirectSoundCreate8", 12: "DirectSoundCaptureCreate8"}}
FIRST_BASE = 0x00400000


# shipped DLLs that are served by the runtime instead (XAudio2 2.9, dbghelp) or never used in a browser build (denoisers,
# streaming-service SDKs, browsers, VR)
SKIP_DLLS = re.compile(r"(libcef|chrome_elf|libegl|libglesv2|d3dcompiler|gfsdk_aftermath|winpixeventruntime|dxgi|d3d1\d|"
                       r"steam_api|nvtt|embree|tbb|libsndfile|ue4prereq|crashreport|unrealcefsubprocess|steamvr|openvr|oculus|libovr|"
                       r"xaudio2_\d|openimagedenoise|gfnruntimesdk|dbghelp|nvngx|^sl\.|libxess|libxell|amd_fidelityfx|ffx_|directml|"
                       r"crashpad|msquic|nvlowlatency|reflex|^api-ms-win-|^ext-ms-win-|^xinput[0-9]|^x3daudio|beclient|easyanticheat|gfesdk)", re.I)


_sys_ords = {}


def system_ord_name(dll, ordinal, sysdir=SYSTEM):
    """The name Windows' own copy of a DLL exports under an ordinal, for imports by number."""
    key = (dll, str(sysdir))
    if key not in _sys_ords:
        try:
            _sys_ords[key] = pemod.load(sysdir / dll).ord_names if (sysdir / dll).is_file() else {}
        except Exception:
            _sys_ords[key] = {}
    return _sys_ords[key].get(ordinal)


def game_dlls(exe):
    """DLLs shipped with the game that it may load at runtime (PhysX, codecs, middleware)."""
    exe = Path(exe)
    root = exe.parent
    for _ in range(4):
        if (root / "Engine").is_dir():
            break
        root = root.parent
    else:
        root = exe.parent
    found = []
    for p in root.rglob("*.dll"):
        # plugin binaries are runtime middleware (FMOD, Wwise...) in shipped games; editor modules are the exception
        if SKIP_DLLS.search(p.name) or "Mods" in p.parts or re.match(r"(UE4|Unreal)Editor", p.name, re.I):
            continue
        if "Win32" in p.parts or "x86" in p.parts:
            continue
        try:
            if pemod.load(p):
                found.append(p)
        except ValueError:
            pass
    return root, found


def apiset(name):
    n = name.lower()
    if n.startswith("api-ms-win-crt-"):
        return "ucrtbase.dll"
    return n


@dataclass
class Module:
    name: str
    path: Path
    pe: object
    base: int = 0
    deps: list = field(default_factory=list)

    @property
    def is_dll(self):
        return self.path.suffix.lower() == ".dll"


class Linker:
    def __init__(self, exe, search=(), hle_names=(), recompile_system=True, extra=()):
        self.exe = Path(exe)
        self.search = [self.exe.parent, *map(Path, search)]
        self.hle = set(hle_names)
        self.recompile_system = recompile_system
        self.modules = {}
        self.order = []
        self.thunks = []
        self.thunk_index = {}
        self.missing = []
        self.bits = pemod.load(self.exe).bits
        self.sysdir = SYSTEM if self.bits == 64 else SYSTEM32
        root = self.add(self.exe)
        for e in extra:
            self.add(Path(e))
        self.assign_bases()
        self.init_order = self.dependency_order(root)

    def find(self, dll):
        n = apiset(dll)
        # left to the runtime: API-set forwarders a game ships, and DLLs the browser can't use
        if SKIP_DLLS.search(n):
            return None
        if n not in RUNTIME_DLLS and (self.sysdir / n).is_file():
            return None
        for d in self.search:
            p = d / n
            if p.is_file() and self.bits_of(p) == self.bits:
                return p
        if self.recompile_system and n in RUNTIME_DLLS and (self.sysdir / n).is_file():
            return self.sysdir / n
        return None

    def bits_of(self, p):
        try:
            return pemod.load(p).bits
        except Exception:
            return 0

    def add(self, path):
        key = path.name.lower()
        if key in self.modules:
            return self.modules[key]
        m = Module(key, path, pemod.load(path))
        self.modules[key] = m
        self.order.append(m)
        for dll in dict.fromkeys(apiset(i.dll) for i in m.pe.imports):
            p = self.find(dll)
            if p:
                m.deps.append(self.add(p).name)
        return m

    def assign_bases(self):
        base = FIRST_BASE
        exe = self.order[0].pe
        # an image without relocations has to sit at its own base
        if not exe.relocs and exe.image_base >= FIRST_BASE and exe.image_base < 0x40000000:
            base = exe.image_base
        for m in self.order:
            m.base = base
            base = (base + m.pe.image_size + 0xFFFF) & ~0xFFFF
        self.end = base

    def dependency_order(self, root):
        seen, out = set(), []

        def visit(m):
            if m.name in seen:
                return
            seen.add(m.name)
            for d in m.deps:
                visit(self.modules[d])
            out.append(m)

        for m in self.order:
            visit(m)
        return out

    def thunk(self, name, dll):
        if name not in self.thunk_index:
            self.thunk_index[name] = len(self.thunks)
            self.thunks.append((name, dll))
            if name not in self.hle:
                self.missing.append(f"{dll}!{name}")
        return THUNK_BASE + 8 * self.thunk_index[name]

    def resolve(self, dll, name, depth=0):
        n = apiset(dll)
        if name.startswith("#") and n not in self.modules:
            name = ORDINALS[n].get(int(name[1:]), name) if n in ORDINALS else name
            if name.startswith("#"):
                name = system_ord_name(n, int(name[1:]), self.sysdir) or name
        if name in PURE and name in self.hle:
            return self.thunk(name, n)
        m = self.modules.get(n)
        if m is None or depth > 8:
            return self.thunk(name, n)
        if name.startswith("#"):
            t = m.pe.exports_ord.get(int(name[1:]))
        else:
            t = m.pe.exports.get(name)
        if t is None:
            return self.thunk(name, n)
        if isinstance(t, str):
            fdll, fname = t.split(".", 1)
            return self.resolve(fdll + ".dll", fname, depth + 1)
        return m.base + t

    def images(self):
        """Relocated module images with every import slot already pointing at its target."""
        for name in sorted(self.hle):
            self.thunk(name, "")
        out = []
        for m in self.order:
            img = bytearray(m.pe.image(m.base))
            for imp in m.pe.imports:
                struct.pack_into("<Q" if m.pe.bits == 64 else "<I", img, imp.iat_rva, self.resolve(imp.dll, imp.name))
            out.append((m, bytes(img)))
        return out
