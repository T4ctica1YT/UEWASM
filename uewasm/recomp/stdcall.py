"""Bytes a 32-bit system function pops on return (stdcall), read from the `ret N` in Windows' own SysWOW64 copy.
Results are cached in stdcall32.json next to this file, so builds on machines without SysWOW64 still get them."""
import json
import os
from pathlib import Path

import iced_x86 as I

from . import pe as pemod

WOW = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "SysWOW64"
CACHE = Path(__file__).with_name("stdcall32.json")
# where API-set forwarders and runtime-only names usually land
FALLBACK = ["kernelbase.dll", "kernel32.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "sechost.dll", "combase.dll",
            "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "ws2_32.dll", "winmm.dll", "wininet.dll", "version.dll",
            "dbghelp.dll", "comdlg32.dll", "comctl32.dll", "gdiplus.dll", "imm32.dll", "setupapi.dll", "dsound.dll", "dinput8.dll",
            "xinput1_4.dll", "d3d9.dll", "dwmapi.dll", "winhttp.dll", "iphlpapi.dll", "crypt32.dll", "bcrypt.dll", "uxtheme.dll",
            "shcore.dll", "powrprof.dll", "psapi.dll", "dxgi.dll", "d3d11.dll", "hid.dll", "cfgmgr32.dll", "msacm32.dll",
            "avrt.dll", "mmdevapi.dll", "propsys.dll", "userenv.dll", "secur32.dll", "wtsapi32.dll", "ucrtbase.dll"]
# where API sets (api-ms-win-*, ext-ms-win-*) are implemented: tried first for those, since the DLL that names one
# (gdi32 for SetDeviceGammaRamp, winmm for waveOutOpen) only jumps on to it
API_SET_HOSTS = ["kernelbase.dll", "windows.storage.dll", "gdi32full.dll", "winmmbase.dll", "win32u.dll", "rpcrt4.dll",
                 "shcore.dll", "sechost.dll", "combase.dll", "ucrtbase.dll"]
CDECL_DLLS = ("ucrtbase", "msvcr", "msvcp", "vcruntime", "api-ms-win-crt-", "msvcrt")

# documented argument bytes for functions whose Windows copy can't be read for it: bodies reached through pointers
# filled in at run time (user32's DefWindowProc), noreturn calls, DLLs that aren't in SysWOW64 (CUDA, XAudio2 2.8+,
# Media Foundation on N editions), and cdecl libraries that don't look like one by name (LDAP, X3DAudio, Steam)
KNOWN = {
    "DefWindowProcA": 16, "DefWindowProcW": 16, "DebugBreak": 0, "ExitThread": 4, "FreeLibraryAndExitThread": 8,
    # Get/SetWindowLongPtr and Get/SetClassLongPtr are macros for the 32-bit Long versions; GetProcAddress may still ask
    "GetWindowLongPtrA": 8, "GetWindowLongPtrW": 8, "SetWindowLongPtrA": 12, "SetWindowLongPtrW": 12,
    "GetClassLongPtrA": 8, "GetClassLongPtrW": 8, "SetClassLongPtrA": 12, "SetClassLongPtrW": 12,
    "AlphaBlend": 44, "TransparentBlt": 44,
    "RpcStringFreeA": 4, "RpcStringFreeW": 4, "UuidCreate": 4, "UuidCreateNil": 4, "UuidCreateSequential": 4,
    "UuidToStringA": 8, "UuidToStringW": 8,
    "GetNamedSecurityInfoW": 32, "SetNamedSecurityInfoW": 28, "SetEntriesInAclW": 16,
    "NCryptFreeObject": 4, "NCryptGetProperty": 24, "WinVerifyTrust": 12, "Tbsi_GetDeviceInfo": 8,
    "wglCreateContext": 4, "wglDeleteContext": 4, "wglGetCurrentContext": 0, "wglGetCurrentDC": 0, "wglGetProcAddress": 4,
    "wglMakeCurrent": 8,
    "XInputGetStateEx": 8,
    # XAudio2 2.8+ (2.7 creates itself through COM); X3DAudio is cdecl (STDAPIVCALLTYPE)
    "XAudio2Create": 12, "XAudio2CreateWithVersionInfo": 16, "CreateAudioReverb": 4, "CreateAudioVolumeMeter": 4,
    "CreateFX": 16, "X3DAudioInitialize": 0, "X3DAudioCalculate": 0,
    "MFStartup": 8, "MFShutdown": 0, "MFCreateAttributes": 8, "MFCreateMediaType": 4, "MFCreateMemoryBuffer": 8,
    "MFCreateSample": 4, "MFCreateSourceReaderFromByteStream": 12, "MFCreateSourceReaderFromURL": 12,
    "MFCreateSourceResolver": 4, "MFCreateTopology": 4, "MFCreateTopologyNode": 8, "MFGetService": 16,
    "MFCreateMediaSession": 8, "MFCreateSampleGrabberSinkActivate": 12, "DXVA2CreateDirect3DDeviceManager9": 8,
    "D3DDisassemble": 20,
    "UiaClientsAreListening": 0, "UiaHostProviderFromHwnd": 8, "UiaRaiseAutomationEvent": 8,
    "UiaRaiseAutomationPropertyChangedEvent": 40, "UiaReturnRawElementProvider": 16,
    # the CUDA runtime is __stdcall on Windows (CUDARTAPI); cudaCreateChannelDesc also pops its hidden result pointer
    "__cudaRegisterFatBinary": 4, "__cudaUnregisterFatBinary": 4, "__cudaRegisterFunction": 40, "__cudaRegisterVar": 32,
    "__cudaRegisterTexture": 28, "__cudaRegisterShared": 8, "cudaCreateChannelDesc": 24, "cudaEventCreate": 4,
    "cudaGetDeviceProperties": 8, "cudaGetErrorString": 4, "cudaGetLastError": 0, "cudaMalloc": 8, "cudaMallocHost": 8,
    "cudaStreamCreate": 4,
    # 64-bit only: never called by a 32-bit game
    "RtlAddFunctionTable": 0, "RtlDeleteFunctionTable": 0, "RtlInstallFunctionTableCallback": 0, "RtlLookupFunctionEntry": 0,
    "RtlVirtualUnwind": 0, "RtlUnwindEx": 0, "__C_specific_handler": 0, "D3D12SerializeRootSignature": 0,
    "D3D12SerializeVersionedRootSignature": 0,
}
# cdecl families: the C runtime's maths, LDAP (LDAPAPI/BERAPI), the Steam API
CDECL_NAMES = ("ldap_", "ber_", "SteamAPI_", "SteamInternal_", "SteamGameServer")
CDECL_FUNCS = {"LdapGetLastError", "acosf", "asinf", "atan2f", "atanf", "ceilf", "cosf", "expf", "floorf", "fmodf", "hypotf",
               "log10f", "logf", "modff", "sinf", "sqrtf", "tanf", "SteamApps", "SteamFriends", "SteamMatchmaking",
               "SteamMatchmakingServers", "SteamNetworking", "SteamRemoteStorage", "SteamUser", "SteamUserStats", "SteamUtils",
               "SteamScreenshots", "SteamHTTP", "SteamUGC"}


def documented(name):
    if name in KNOWN:
        return KNOWN[name]
    if name in CDECL_FUNCS or name.startswith(CDECL_NAMES):
        return 0
    return None


_pe = {}
_cache = None
# the game's own folders (set by the linker): DLLs a game ships but that aren't recompiled (CUDA, Steam) are read from
# there, after Windows' copies
SEARCH = []


def _load(dll):
    dll = dll.lower()
    if dll not in _pe:
        _pe[dll] = None
        for d in [WOW, *SEARCH]:
            p = Path(d) / dll
            if p.is_file():
                try:
                    _pe[dll] = pemod.load(p)
                except Exception:
                    continue
                break
    return _pe[dll]


def _imports(p):
    if not hasattr(p, "_iat"):
        p._iat = {p.image_base + i.iat_rva: (i.dll, i.name) for i in [*p.delay_imports, *p.imports]}
    return p._iat


def _scan(p, rva, depth):
    img = p.__dict__.setdefault("_img", p.image(p.image_base))
    base = p.image_base
    work, seen = [base + rva], set()
    steps = 0
    while work and steps < 4000:
        a = work.pop()
        while a not in seen and steps < 4000:
            seen.add(a)
            steps += 1
            o = a - base
            if not (0 <= o < len(img)):
                break
            ins = I.Decoder(32, img[o:o + 16], ip=a).decode()
            if ins.code == I.Code.INVALID:
                break
            fc = ins.flow_control
            if fc == I.FlowControl.RETURN:
                return ins.immediate(0) if ins.op_count else 0
            if fc == I.FlowControl.CONDITIONAL_BRANCH:
                work.append(ins.near_branch_target)
            elif fc == I.FlowControl.UNCONDITIONAL_BRANCH:
                a = ins.near_branch_target
                continue
            elif fc == I.FlowControl.INDIRECT_BRANCH:
                if ins.op_kind(0) == I.OpKind.MEMORY and ins.memory_base == I.Register.NONE and ins.memory_index == I.Register.NONE:
                    slot = ins.memory_displacement & 0xFFFFFFFF
                    imp = _imports(p).get(slot)
                    if imp and depth < 6:
                        r = lookup(imp[0], imp[1], depth + 1)
                        if r is not None:
                            return r
                    # newer Windows builds route exports through function pointers in the DLL's own data
                    # (user32's SetCursor is `mov eax, 1000h; jmp [slot]`, the slot holding the real body)
                    so = slot - base
                    if not imp and 0 <= so <= len(img) - 4:
                        t = int.from_bytes(img[so:so + 4], "little")
                        if 0 <= t - base < len(img):
                            a = t
                            continue
                break
            elif fc in (I.FlowControl.INTERRUPT, I.FlowControl.EXCEPTION):
                break
            a = ins.next_ip
    return None


def lookup(dll, name, depth=0):
    """bytes popped, or None when unknown"""
    dll = dll.lower()
    if not dll.endswith(".dll"):
        dll += ".dll"
    if dll.startswith(CDECL_DLLS):
        return 0
    cands = [dll] if _load(dll) else []
    if dll.startswith(("api-ms-", "ext-ms-")):
        cands += API_SET_HOSTS
    cands += [d for d in FALLBACK if d not in cands]
    for d in cands:
        p = _load(d)
        if not p:
            continue
        t = p.exports.get(name)
        if t is None and name.startswith("#"):
            t = p.exports_ord.get(int(name[1:]))
        if t is None:
            continue
        if isinstance(t, str):
            fdll, fname = t.split(".", 1)
            r = lookup(fdll, fname, depth + 1) if depth < 6 else None
        elif d.startswith(CDECL_DLLS):
            return 0
        else:
            r = _scan(p, t, depth)
        # a copy that only forwards on (or can't be read) doesn't settle it: another DLL may hold the body
        if r is not None:
            return r
    return None


def table():
    global _cache
    if _cache is None:
        try:
            _cache = json.loads(CACHE.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            _cache = {}
    return _cache


def pops(dll, name, save=False):
    t = table()
    key = name
    if key in t:
        return t[key]
    v = lookup(dll or "kernel32.dll", name) if WOW.is_dir() else None
    if v is None:
        return documented(name)
    t[key] = v
    if save:
        CACHE.write_text(json.dumps(t, indent=0, sort_keys=True), encoding="utf-8")
    return v


def save():
    if _cache is not None and WOW.is_dir():
        CACHE.write_text(json.dumps(_cache, indent=0, sort_keys=True), encoding="utf-8")
