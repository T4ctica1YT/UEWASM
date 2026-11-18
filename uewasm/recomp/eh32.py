"""32-bit MSVC C++ exception handling, found statically so the lifted code can resume at a catch.

A function with try/catch registers a handler thunk (`mov eax, FuncInfo; jmp __CxxFrameHandler3`) in its prologue.
When a catch runs, its funclet returns the address to continue at (`mov eax, continuation; ret`) and the CRT jumps
there with the function's own esp and ebp. The lifter gives every such function a host setjmp on entry, keyed by its
frame pointer, and lifts each continuation as an entry point, so the runtime can resume there from that frame (see
rt_eh_slot32 in runtime/rt.c).

analyze() returns the functions to instrument ({start: frame pointer offset from the entry esp}) and the
continuation addresses, both absolute."""
import bisect
import re
import struct

MAGIC = (0x19930520, 0x19930521, 0x19930522)


def _u32(img, o):
    return struct.unpack_from("<I", img, o)[0] if 0 <= o <= len(img) - 4 else None


def analyze(img, base, sections):
    """img: the module image relocated to base; sections: (rva, size, executable) triples"""
    text = [(rva, rva + size) for rva, size, x in sections if x]

    def in_text(a):
        r = a - base
        return any(s <= r < e for s, e in text)

    def rd(a):
        return _u32(img, a - base)

    thunks, conts = set(), set()
    for s, e in text:
        blob = img[s:e]
        # mov eax, imm32; jmp rel32
        for m in re.finditer(rb"\xB8(?=[\s\S]{4}\xE9)", blob):
            fi = struct.unpack_from("<I", blob, m.start() + 1)[0]
            if rd(fi) not in MAGIC:
                continue
            ntry, tmap = rd(fi + 12), rd(fi + 16)
            if not ntry or ntry > 4096 or tmap is None or rd(tmap) is None:
                continue
            thunks.add(base + s + m.start())
            # TryBlockMapEntry { tryLow, tryHigh, catchHigh, nCatches, HandlerType* } (20 bytes);
            # HandlerType { adjectives, type, catch object, handler } (16 bytes)
            for i in range(ntry):
                ncatch, handlers = rd(tmap + 20 * i + 12), rd(tmap + 20 * i + 16)
                if not ncatch or ncatch > 256 or handlers is None:
                    continue
                for k in range(ncatch):
                    h = rd(handlers + 16 * k + 12)
                    if h is None or not in_text(h):
                        continue
                    # the funclet's returns: mov eax, continuation; ret
                    o = h - base
                    for r in re.finditer(rb"\xB8([\s\S]{4})\xC3", img[o:o + 1024]):
                        t = struct.unpack_from("<I", r.group(1))[0]
                        if in_text(t) and abs(t - h) < 0x100000:
                            conts.add(t)
    starts = {}
    if not thunks:
        return starts, conts
    order = sorted(thunks)

    def is_thunk(t):
        """the thunk itself, or a /GS prelude before it (mov edx, [esp+8]; lea eax, [edx+0Ch]; cookie checks)"""
        if t in thunks:
            return True
        o = t - base
        if img[o:o + 4] != b"\x8B\x54\x24\x08":
            return False
        k = bisect.bisect_left(order, t)
        return k < len(order) and order[k] - t <= 64

    for s, e in text:
        blob = img[s:e]
        for m in re.finditer(rb"[\x68\xB8]", blob):
            i = m.start()
            if i + 5 > len(blob) or not is_thunk(struct.unpack_from("<I", blob, i + 1)[0]):
                continue
            a = base + s + i
            if blob[i] == 0x68:
                # push ebp; mov ebp, esp; push -1 (or -2); push handler
                if i >= 5 and blob[i - 5:i - 1] == b"\x55\x8B\xEC\x6A" and blob[i - 1] in (0xFF, 0xFE):
                    starts[a - 5] = 4
                # frame-pointer-less: push -1; push handler (the CRT then sets ebp to the entry esp)
                elif i >= 2 and blob[i - 2:i] == b"\x6A\xFF":
                    starts[a - 2] = 0
            elif i + 6 <= len(blob) and blob[i + 5] == 0xE8:
                # mov eax, handler; call __EH_prolog (ebp = entry esp - 4), maybe after push N for __EH_prolog3
                starts[a] = 4
                if i >= 2 and blob[i - 2] == 0x6A:
                    starts[a - 2] = 4
                if i >= 5 and blob[i - 5] == 0x68:
                    starts[a - 5] = 4
    _seh(img, base, text, in_text, rd, starts, conts)
    return starts, conts


def _scope_records(rd, in_text, table):
    """SEH scope records { enclosing level, filter (0 for __finally), handler } from table on, while they look valid"""
    recs = []
    for i in range(64):
        lvl, flt, hnd = rd(table + 12 * i), rd(table + 12 * i + 4), rd(table + 12 * i + 8)
        if lvl is None or hnd is None or not in_text(hnd) or (flt and not in_text(flt)):
            break
        if lvl not in (0xFFFFFFFF, 0xFFFFFFFE) and lvl >= i:
            break
        recs.append((flt, hnd))
    return recs


def _seh(img, base, text, in_text, rd, starts, conts):
    """__try/__except and __finally (_except_handler3/4): the CRT calls their filters and finally blocks through the
    scope table and jumps into the __except blocks with the function's frame, the same way as a catch continuation.
    Frames are set up inline (push ebp; mov ebp, esp; push -1/-2; push scope table; push handler) or through
    __SEH_prolog3/4 (push N; push scope table; call)"""
    def prolog(t):
        """3 or 4 for __SEH_prolog3/4 at t: push _except_handler; then mov eax, fs:[0] (3) or push fs:[0] (4)"""
        o = t - base
        if img[o:o + 1] != b"\x68":
            return 0
        tail = img[o + 5:o + 12]
        return 4 if tail == b"\x64\xFF\x35\x00\x00\x00\x00" else 3 if tail[:6] == b"\x64\xA1\x00\x00\x00\x00" else 0

    def add(start, table, seh4):
        recs = _scope_records(rd, in_text, table + (16 if seh4 else 0))
        if not recs:
            return
        starts[start] = 4
        for flt, hnd in recs:
            conts.add(hnd)
            if flt:
                conts.add(flt)

    for s, e in text:
        blob = img[s:e]
        for m in re.finditer(rb"\x68", blob):
            i = m.start()
            if i + 10 > len(blob):
                continue
            a = base + s + i
            table = struct.unpack_from("<I", blob, i + 1)[0]
            if i >= 5 and blob[i - 5:i - 1] == b"\x55\x8B\xEC\x6A" and blob[i - 1] in (0xFF, 0xFE) and blob[i + 5] == 0x68:
                add(a - 5, table, blob[i - 1] == 0xFE)
            elif blob[i + 5] == 0xE8:
                kind = prolog(a + 10 + struct.unpack_from("<i", blob, i + 6)[0])
                if not kind:
                    continue
                if i >= 2 and blob[i - 2] == 0x6A:
                    add(a - 2, table, kind == 4)
                if i >= 5 and blob[i - 5] == 0x68:
                    add(a - 5, table, kind == 4)
