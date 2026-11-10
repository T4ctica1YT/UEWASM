/* Smaller Windows APIs newer Unreal Engine builds import: string and number helpers, ANSI message variants, shell
   folders, crypto randomness, power and process queries, and services a browser doesn't have (timer queues, wait
   registration, UI Automation, security tokens), which report failure the way Windows does when they're unavailable. */
#include "host.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

void hle_set_error(uint32_t e);
const char* hle_csidl_path(int csidl);
void hle_GetMessageW(CPU*);
void hle_PostMessageW(CPU*);
void hle_SendMessageW(CPU*);
void hle_DefWindowProcW(CPU*);
void hle_CreateEventExW(CPU*);
void hle_GetFileAttributesW(CPU*);
void hle_CreateDirectoryW(CPU*);

/* ---- strings and numbers ---- */

static int wcmp(uint64_t a, uint64_t b, int fold)
{
    for (;; a += 2, b += 2) {
        uint32_t x = (uint32_t)LD16(a), y = (uint32_t)LD16(b);
        if (fold) { if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; }
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
static int acmp(uint64_t a, uint64_t b)
{
    for (;; a++, b++) {
        uint32_t x = (uint32_t)LD8(a), y = (uint32_t)LD8(b);
        if (x != y) return x < y ? -1 : 1;
        if (!x) return 0;
    }
}
void hle_GetModuleFileNameW(CPU*);
HLE(lstrcmpW) { RET((int64_t)(A1 && A2 ? wcmp(A1, A2, 0) : (A1 ? 1 : A2 ? -1 : 0))); }
HLE(lstrcmpA) { RET((int64_t)(A1 && A2 ? acmp(A1, A2) : (A1 ? 1 : A2 ? -1 : 0))); }
HLE(MulDiv)
{
    int32_t a = (int32_t)A1, b = (int32_t)A2, d = (int32_t)A3;
    if (!d) { RET((uint64_t)(int64_t)-1); return; }
    int64_t p = (int64_t)a * b;
    /* rounded to nearest, like Windows */
    int64_t q = (p >= 0) == (d > 0) ? (p + (int64_t)(d > 0 ? d : -d) / 2 * (d > 0 ? 1 : -1)) / d : (p - (int64_t)(d > 0 ? d : -d) / 2 * (d > 0 ? 1 : -1)) / d;
    RET((uint64_t)(int64_t)(int32_t)q);
}
HLE(CompareFileTime)
{
    uint64_t a = LD64(A1), b = LD64(A2);
    RET((uint64_t)(int64_t)(a < b ? -1 : a > b ? 1 : 0));
}
/* RtlUTF8ToUnicodeN(dst, dst bytes, written bytes, src, src bytes) */
HLE(RtlUTF8ToUnicodeN)
{
    uint64_t dst = A1, cap = A2 / 2, outn = A3, src = A4;
    uint32_t n = (uint32_t)ARG(5), i = 0, w = 0;
    while (i < n) {
        uint32_t ch = LD8(src + i), len = ch < 0x80 ? 1 : ch < 0xE0 ? 2 : ch < 0xF0 ? 3 : 4;
        if (len > 1) { ch &= (0xFF >> (len + 1)); for (uint32_t k = 1; k < len && i + k < n; k++) ch = (ch << 6) | (LD8(src + i + k) & 0x3F); }
        i += len;
        if (ch >= 0x10000) {
            if (dst && w + 1 < cap) { ST16(dst + 2ull * w, 0xD800 + ((ch - 0x10000) >> 10)); ST16(dst + 2ull * w + 2, 0xDC00 + ((ch - 0x10000) & 0x3FF)); }
            w += 2;
        } else { if (dst && w < cap) ST16(dst + 2ull * w, ch); w++; }
    }
    if (outn) ST32(outn, w * 2);
    RET(dst && w > cap ? 0xC0000023 : 0);
}

/* ---- ANSI window-message variants ---- */
HLE(GetMessageA) { hle_GetMessageW(c); }
HLE(PostMessageA) { hle_PostMessageW(c); }
HLE(SendMessageA) { hle_SendMessageW(c); }
HLE(DefWindowProcA) { hle_DefWindowProcW(c); }
HLE(CreateEventExA) { hle_CreateEventExW(c); }
HLE(GetMessageExtraInfo) { RET(0); }
HLE(GetWindowTextLengthW) { RET(0); }
HLE(LoadStringW) { if (A3 && A4) ST16(A3, 0); RET(0); }
HLE(AllowSetForegroundWindow) { RET(1); }
HLE(GetWindowDisplayAffinity) { if (A2) ST32(A2, 0); RET(1); }
HLE(SetWindowDisplayAffinity) { RET(1); }
HLE(GetDialogBaseUnits) { RET((16u << 16) | 8u); }
HLE(GetClassLongPtrW) { RET(0); }
HLE(SetWindowsHookExW) { RET(0x7E000001); }
HLE(UnhookWindowsHookEx) { RET(1); }
HLE(CallNextHookEx) { RET(0); }

/* ---- shell and files ---- */
HLE(SHGetSpecialFolderPathW) { utf8_to_wstr(hle_csidl_path((int)A3), A2, 260); RET(1); }
HLE(PathFileExistsW)
{
    uint64_t save = A1;
    hle_GetFileAttributesW(c);
    RET((uint32_t)R(0) != 0xFFFFFFFFu ? 1 : 0);
    (void)save;
}
/* SHCreateDirectoryExW(hwnd, path, sa): every missing level */
HLE(SHCreateDirectoryExW)
{
    char p[1024];
    wstr_to_utf8(A2, p, sizeof p);
    for (char* q = p; *q; q++) {
        if ((*q == '\\' || *q == '/') && q > p + 2) {
            char keep = *q;
            *q = 0;
            uint64_t w = g_alloc(2048);
            utf8_to_wstr(p, w, 1024);
            uint64_t a1 = R(1);
            R(1) = w;
            hle_CreateDirectoryW(c);
            R(1) = a1;
            g_free(w);
            *q = keep;
        }
    }
    uint64_t a1 = R(1);
    R(1) = A2;
    hle_CreateDirectoryW(c);
    R(1) = a1;
    RET((uint32_t)R(0) ? 0 : 183);
}

/* ---- crypto: randomness is real; keys and certificates aren't available ---- */
static void fill_random(uint64_t p, uint64_t n) { for (uint64_t i = 0; i < n; i++) ST8(p + i, (uint64_t)((rand() ^ (rand() >> 7)) & 0xFF)); }
HLE(CryptAcquireContextW) { if (A1) ST64(A1, 0x7C000001); RET(1); }
HLE(CryptAcquireContextA) { if (A1) ST64(A1, 0x7C000001); RET(1); }
HLE(CryptReleaseContext) { RET(1); }
HLE(CryptGenRandom) { fill_random(A3, (uint32_t)A2); RET(1); }
HLE(CryptCreateHash) { hle_set_error(0x80090008); RET(0); }
HLE(CryptHashData) { RET(0); }
HLE(CryptGetHashParam) { RET(0); }
HLE(CryptSetHashParam) { RET(0); }
HLE(CryptDestroyHash) { RET(1); }
HLE(CryptDestroyKey) { RET(1); }
HLE(CryptDecrypt) { RET(0); }
HLE(CryptExportKey) { RET(0); }
HLE(CryptGetUserKey) { hle_set_error(0x8009000D); RET(0); }
HLE(CryptGetProvParam) { RET(0); }
HLE(CryptSignHashW) { RET(0); }
HLE(CryptEnumProvidersW) { hle_set_error(259); RET(0); }
HLE(CertOpenStore) { RET(0); }
HLE(CertCloseStore) { RET(1); }
HLE(CertEnumCertificatesInStore) { RET(0); }
HLE(CertFindCertificateInStore) { RET(0); }
HLE(CertDuplicateCertificateContext) { RET(A1); }
HLE(CertAddCertificateContextToStore) { RET(0); }
HLE(CertGetCertificateContextProperty) { RET(0); }
HLE(CertGetNameStringW) { if (ARG(5) && ARG(6)) ST16(ARG(5), 0); RET(1); }
HLE(CertGetNameStringA) { if (ARG(5) && ARG(6)) ST8(ARG(5), 0); RET(1); }
HLE(CertCreateCertificateChainEngine) { RET(0); }
HLE(CertFreeCertificateChainEngine) { RET(0); }
HLE(CryptQueryObject) { RET(0); }
HLE(WinVerifyTrust) { RET(0x800B0100); }

/* ---- processes, security, power ---- */
HLE(OpenProcessToken) { hle_set_error(5); RET(0); }
HLE(OpenThreadToken) { hle_set_error(1008); RET(0); }
HLE(AdjustTokenPrivileges) { RET(0); }
HLE(LookupPrivilegeValueA) { RET(0); }
HLE(AllocateLocallyUniqueId) { static uint32_t n = 0x10000; if (A1) { ST32(A1, n++); ST32(A1 + 4, 0); } RET(1); }
HLE(ImpersonateSelf) { RET(1); }
HLE(RevertToSelf) { RET(1); }
HLE(AccessCheck) { if (ARG(7)) ST32(ARG(7), 1); if (ARG(6)) ST32(ARG(6), (uint32_t)A3); RET(1); }
HLE(GetFileSecurityW) { hle_set_error(50); RET(0); }
HLE(OpenProcess) { hle_set_error(5); RET(0); }
HLE(ReadProcessMemory) { RET(0); }
HLE(K32EnumProcessModules) { if (A4) ST32(A4, 8); if (A2 && (uint32_t)A3 >= 8) ST64(A2, g_exe_base); RET(1); }
HLE(K32EnumProcessModulesEx) { if (A4) ST32(A4, 8); if (A2 && (uint32_t)A3 >= 8) ST64(A2, g_exe_base); RET(1); }
HLE(K32GetModuleFileNameExW) { R(1) = A2; R(2) = A3; R(8) = A4; hle_GetModuleFileNameW(c); }
HLE(K32GetModuleFileNameExA) { if (A3 && A4 > 11) memcpy(G2H(A3), "C:\\Game.exe", 12); RET(11); }
HLE(Process32First) { hle_set_error(18); RET(0); }
HLE(Process32Next) { hle_set_error(18); RET(0); }
/* SYSTEM_POWER_STATUS: on mains power, no battery */
HLE(GetSystemPowerStatus) { if (A1) { ST8(A1, 1); ST8(A1 + 1, 128); ST8(A1 + 2, 255); ST8(A1 + 3, 0); ST32(A1 + 4, 0xFFFFFFFF); ST32(A1 + 8, 0xFFFFFFFF); } RET(1); }
HLE(GetLargePageMinimum) { RET(0); }
HLE(VirtualUnlock) { RET(1); }
HLE(DebugBreak) { host_log("[debug] DebugBreak at %llx", (unsigned long long)LD64(R(4))); }
HLE(DeviceIoControl) { hle_set_error(1); RET(0); }
HLE(ReOpenFile) { hle_set_error(6); RET((uint64_t)-1); }
HLE(ReadConsoleA) { RET(0); }
HLE(EventWriteTransfer) { RET(0); }

/* ---- vectored exception handlers: kept so removal matches; exceptions are delivered through SEH ---- */
static uint64_t veh[64];
static int nveh;
HLE(AddVectoredExceptionHandler) { if (nveh < 64) veh[nveh++] = A2; RET(A2 ? 0x7D000000ull + (uint64_t)nveh : 0); }
HLE(RemoveVectoredExceptionHandler) { RET(A1 >= 0x7D000001ull && A1 <= 0x7D000040ull ? 1 : 0); }

/* ---- timers, thread pool and wait registration: not available (callers fall back to their own threads) ---- */
HLE(CreateTimerQueue) { hle_set_error(50); RET(0); }
HLE(CreateTimerQueueTimer) { hle_set_error(50); RET(0); }
HLE(ChangeTimerQueueTimer) { RET(0); }
HLE(DeleteTimerQueueTimer) { RET(1); }
HLE(CreateWaitableTimerExW) { hle_set_error(50); RET(0); }
HLE(CancelWaitableTimer) { RET(1); }
HLE(QueueUserWorkItem) { hle_set_error(50); RET(0); }
HLE(RegisterWaitForSingleObject) { hle_set_error(50); RET(0); }
HLE(UnregisterWait) { RET(1); }
HLE(UnregisterWaitEx) { RET(1); }

/* ---- multimedia thread priority (avrt) ---- */
HLE(AvSetMmThreadCharacteristicsA) { if (A2) ST32(A2, 1); RET(0x7B000001); }
HLE(AvRevertMmThreadCharacteristics) { RET(1); }
HLE(AvSetMmThreadPriority) { RET(1); }

/* ---- UI Automation: no screen readers listening ---- */
HLE(UiaClientsAreListening) { RET(0); }
HLE(UiaHostProviderFromHwnd) { if (A2) ST64(A2, 0); RET(0x80004005); }
HLE(UiaRaiseAutomationEvent) { RET(0); }
HLE(UiaRaiseAutomationPropertyChangedEvent) { RET(0); }
HLE(UiaReturnRawElementProvider) { RET(0); }

/* ---- locale formatting ---- */
static void copy_w(uint64_t src, uint64_t dst, uint32_t cap, uint32_t* n)
{
    uint32_t i = 0;
    while (LD16(src + 2ull * i)) i++;
    *n = i + 1;
    if (dst && cap >= *n) for (uint32_t k = 0; k <= i; k++) ST16(dst + 2ull * k, LD16(src + 2ull * k));
}
/* GetNumberFormatEx / GetCurrencyFormatEx(locale, flags, value, format, out, cch): the value as given */
HLE(GetNumberFormatEx) { uint32_t n; copy_w(A3, ARG(5), (uint32_t)ARG(6), &n); RET(ARG(5) && (uint32_t)ARG(6) < n ? 0 : n); }
HLE(GetCurrencyFormatEx) { uint32_t n; copy_w(A3, ARG(5), (uint32_t)ARG(6), &n); RET(ARG(5) && (uint32_t)ARG(6) < n ? 0 : n); }
HLE(ResolveLocaleName) { if (A2 && (uint32_t)A3 >= 6) utf8_to_wstr("en-US", A2, 6); RET(6); }
HLE(GetGeoInfoW) { RET(0); }

/* ---- optional APIs engines probe for ---- */
HLE(GetProductInfo) { if (ARG(5)) ST32(ARG(5), 0x30); RET(1); }
HLE(SetWindowFeedbackSetting) { RET(1); }
HLE(RtlAreLongPathsEnabled) { RET(0); }

/* ---- display configuration: no paths to describe; callers fall back to EnumDisplaySettings ---- */
HLE(GetDisplayConfigBufferSizes) { if (A2) ST32(A2, 0); if (A3) ST32(A3, 0); RET(50); }
HLE(QueryDisplayConfig) { if (A2) ST32(A2, 0); if (A4) ST32(A4, 0); RET(50); }
HLE(DisplayConfigGetDeviceInfo) { RET(50); }
