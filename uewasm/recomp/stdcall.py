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
CDECL_DLLS = ("ucrtbase", "msvcr", "msvcp", "vcruntime", "api-ms-win-crt-", "msvcrt")

_pe = {}
_cache = None


def _load(dll):
    dll = dll.lower()
    if dll not in _pe:
        p = WOW / dll
        try:
            _pe[dll] = pemod.load(p) if p.is_file() else None
        except Exception:
            _pe[dll] = None
    return _pe[dll]


def _imports(p):
    if not hasattr(p, "_iat"):
        p._iat = {p.image_base + i.iat_rva: (i.dll, i.name) for i in p.imports}
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
                    imp = _imports(p).get(ins.memory_displacement & 0xFFFFFFFF)
                    if imp and depth < 6:
                        r = lookup(imp[0], imp[1], depth + 1)
                        if r is not None:
                            return r
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
    cands += [d for d in FALLBACK if d != dll]
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
            if depth < 6:
                return lookup(fdll, fname, depth + 1)
            return None
        if d.startswith(CDECL_DLLS):
            return 0
        return _scan(p, t, depth)
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
    if not WOW.is_dir():
        return None
    v = lookup(dll or "kernel32.dll", name)
    if v is not None:
        t[key] = v
        if save:
            CACHE.write_text(json.dumps(t, indent=0, sort_keys=True), encoding="utf-8")
    return v


def save():
    if _cache is not None and WOW.is_dir():
        CACHE.write_text(json.dumps(_cache, indent=0, sort_keys=True), encoding="utf-8")
