/* Exports only 32-bit programs import (kernel32's Interlocked functions are compiler intrinsics on x64) and ANSI
   forms older engines still call. */
#include "host.h"

#include <stdio.h>
#include <string.h>

void hle_set_error(uint32_t e);
const char* hle_csidl_path(int csidl);

/* the same atomic operations as the guest's own locked instructions (rt.h AT*) */
HLE(InterlockedIncrement) { RET((int64_t)(int32_t)(ATADD32(A1, 1) + 1)); }
HLE(InterlockedDecrement) { RET((int64_t)(int32_t)(ATADD32(A1, 0xFFFFFFFFu) - 1)); }
HLE(InterlockedExchange) { RET(ATXCHG32(A1, A2)); }
HLE(InterlockedExchangeAdd) { RET(ATADD32(A1, A2)); }
HLE(InterlockedCompareExchange) { RET(ATCAS32(A1, A3, A2)); }
/* (ptr, exchange lo/hi, comperand lo/hi) on the 32-bit stack; the old value comes back in edx:eax */
HLE(InterlockedCompareExchange64)
{
    uint64_t x = (uint32_t)A2 | ((uint64_t)(uint32_t)A3 << 32), cmp = (uint32_t)A4 | ((uint64_t)(uint32_t)ARG(5) << 32);
    RET(ATCAS64(A1, cmp, x));
}

HLE(IsBadWritePtr) { RET(0); }
HLE(IsBadReadPtr) { RET(0); }
HLE(IsBadCodePtr) { RET(0); }
HLE(IsBadStringPtrA) { RET(0); }
HLE(IsBadStringPtrW) { RET(0); }

HLE(FatalAppExitA) { host_log("[exit] FatalAppExit: %s", A2 ? GSTR(A2) : ""); rt_exit(255); }
HLE(FatalAppExitW) { char b[512] = ""; if (A2) wstr_to_utf8(A2, b, sizeof b); host_log("[exit] FatalAppExit: %s", b); rt_exit(255); }

HLE(CreateFontA) { RET(0x7A000302); }
HLE(CreateFontIndirectA) { RET(0x7A000302); }

HLE(SHGetSpecialFolderPathA)
{
    const char* p = hle_csidl_path((int)A3);
    size_t n = strlen(p);
    if (n > 259) n = 259;
    memcpy(G2H(A2), p, n);
    ST8(A2 + n, 0);
    RET(1);
}

/* MCI (CD audio, legacy MIDI and video playback) has no device here */
HLE(mciSendStringA) { if (A2 && A3) ST8(A2, 0); RET(274); }
HLE(mciSendStringW) { if (A2 && A3) ST16(A2, 0); RET(274); }
HLE(mciGetErrorStringA) { if (A2 && A3) { const char* m = "The MCI device is not available."; size_t n = strlen(m) + 1; if (n > A3) n = (size_t)A3; memcpy(G2H(A2), m, n); ST8(A2 + n - 1, 0); } RET(1); }
HLE(mciGetErrorStringW) { if (A2 && A3) utf8_to_wstr("The MCI device is not available.", A2, (size_t)A3); RET(1); }
HLE(mciSendCommandA) { RET(274); }
HLE(mciSendCommandW) { RET(274); }

HLE(WSAAddressToStringA)
{
    if (A1 && LD16(A1) == 2 && A4) {
        char b[32];
        uint8_t* ip = G2H(A1 + 4);
        uint16_t port = (uint16_t)((LD8(A1 + 2) << 8) | LD8(A1 + 3));
        int n = port ? snprintf(b, sizeof b, "%u.%u.%u.%u:%u", ip[0], ip[1], ip[2], ip[3], port) : snprintf(b, sizeof b, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        if ((uint32_t)LD32(ARG(5)) > (uint32_t)n) { memcpy(G2H(A4), b, (size_t)n + 1); ST32(ARG(5), (uint64_t)n + 1); RET(0); return; }
        ST32(ARG(5), (uint64_t)n + 1);
    }
    hle_set_error(10014);
    RET(0xFFFFFFFF);
}

HLE(HttpEndRequestW) { hle_set_error(12017); RET(0); }
HLE(HttpEndRequestA) { hle_set_error(12017); RET(0); }
HLE(InternetWriteFile) { if (A4) ST32(A4, (uint32_t)A3); RET(1); }
HLE(InternetCanonicalizeUrlA)
{
    size_t n = strlen(GSTR(A1));
    uint32_t cap = (uint32_t)LD32(A3);
    if (n + 1 > cap) { ST32(A3, n + 1); hle_set_error(122); RET(0); return; }
    memcpy(G2H(A2), GSTR(A1), n + 1);
    ST32(A3, n);
    RET(1);
}
HLE(InternetCanonicalizeUrlW)
{
    size_t n = 0;
    while (LD16(A1 + 2 * n)) n++;
    uint32_t cap = (uint32_t)LD32(A3);
    if (n + 1 > cap) { ST32(A3, n + 1); hle_set_error(122); RET(0); return; }
    memcpy(G2H(A2), G2H(A1), 2 * (n + 1));
    ST32(A3, n);
    RET(1);
}
