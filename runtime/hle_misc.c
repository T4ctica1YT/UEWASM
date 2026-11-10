#include "host.h"

#include <stdio.h>
#include <stdlib.h>

uint64_t handle_new(int kind, void* obj);
void* handle_obj(uint64_t h);
void guest_to_host(const char* guest, char* out, size_t cap);


STUB(GetAdaptersAddresses, 232)
STUB(GetAdaptersInfo, 232)
HLE(IdnToAscii) { int64_t n = (int32_t)A3; if (n < 0) { n = 0; while (LD16(A2 + 2 * n)) n++; n++; } if (A4 && (int32_t)ARG(5) >= n) memcpy(G2H(A4), G2H(A2), 2 * (size_t)n); RET(n); }

/* registry: empty but writable */
STUB(RegOpenKeyExW, 2)
STUB(RegOpenKeyExA, 2)
HLE(RegCreateKeyExW) { ST64(ARG(8), 0x80000100); if (ARG(9)) ST32(ARG(9), 1); RET(0); }
STUB(RegQueryValueExW, 2)
STUB(RegQueryValueExA, 2)
STUB(RegSetValueExW, 0)
STUB(RegCloseKey, 0)
STUB(RegDeleteKeyW, 0)
STUB(RegDeleteValueW, 0)
STUB(RegEnumKeyExW, 259)
STUB(RegEnumValueW, 259)
STUB(RegGetValueW, 2)
STUB(RegisterEventSourceW, 0x80000200)
STUB(DeregisterEventSource, 1)
STUB(ReportEventW, 1)
STUB(InitializeSecurityDescriptor, 1)
STUB(SetSecurityDescriptorDacl, 1)

/* COM / OLE */
STUB(CoInitialize, 0)
STUB(CoInitializeEx, 0)
STUB(CoUninitialize, 0)
STUB(OleInitialize, 0)
STUB(OleUninitialize, 0)
int com_create_known(CPU* c, uint64_t clsid, uint64_t out);
HLE(CoCreateInstance)
{
    if (com_create_known(c, A1, ARG(5))) return;
    /* classes we don't provide, once each: the usual reason a game feature stays off */
    static uint32_t seen[64];
    static int nseen;
    uint32_t k = LD32(A1);
    int known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i] == k;
    if (!known && nseen < 64) {
        seen[nseen++] = k;
        uint8_t* g = G2H(A1);
        host_log("[com] CoCreateInstance: no class {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", (unsigned)LD32(A1), (unsigned)LD16(A1 + 4),
                 (unsigned)LD16(A1 + 6), g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    }
    if (ARG(5)) ST64(ARG(5), 0);
    RET(0x80040154);
}
HLE(CoTaskMemAlloc) { RET(g_alloc(A1)); }
HLE(CoTaskMemFree) { g_free(A1); RET(0); }
HLE(CoCreateGuid) { for (int i = 0; i < 16; i++) ST8(A1 + i, (uint64_t)(rand() & 0xFF)); RET(0); }
HLE(StringFromGUID2)
{
    char b[64];
    uint8_t* g = G2H(A1);
    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g, 4); memcpy(&d2, g + 4, 2); memcpy(&d3, g + 6, 2);
    snprintf(b, sizeof b, "{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    RET(utf8_to_wstr(b, A2, (size_t)A3) + 1);
}
STUB(IIDFromString, 0x80004005)
STUB(PropVariantClear, 0)
STUB(RegisterDragDrop, 0)
STUB(RevokeDragDrop, 0)
STUB(ReleaseStgMedium, 0)

/* BSTR: 4-byte byte length prefix, UTF-16 data, two-byte terminator */
static uint64_t bstr_alloc(uint64_t src, uint32_t bytes)
{
    uint64_t p = g_alloc(bytes + 6);
    if (!p) return 0;
    ST32(p, bytes);
    if (src) memcpy(G2H(p + 4), G2H(src), bytes);
    else memset(G2H(p + 4), 0, bytes);
    ST16(p + 4 + bytes, 0);
    ST8(p + 5 + bytes, 0);
    return p + 4;
}
static uint32_t wlen(uint64_t s) { uint32_t n = 0; if (s) while (LD16(s + 2ull * n)) n++; return n; }
HLE(SysAllocString) { RET(A1 ? bstr_alloc(A1, wlen(A1) * 2) : 0); }
HLE(SysAllocStringLen) { RET(bstr_alloc(A1, (uint32_t)A2 * 2)); }
HLE(SysAllocStringByteLen) { RET(bstr_alloc(A1, (uint32_t)A2)); }
HLE(SysFreeString) { if (A1) g_free(A1 - 4); RET(0); }
HLE(SysStringLen) { RET(A1 ? LD32(A1 - 4) / 2 : 0); }
HLE(SysStringByteLen) { RET(A1 ? LD32(A1 - 4) : 0); }
HLE(SysReAllocString) { uint64_t n = A2 ? bstr_alloc(A2, wlen(A2) * 2) : 0; if (A1 && LD64(A1)) g_free(LD64(A1) - 4); if (A1) ST64(A1, n); RET(n != 0 || !A2); }
HLE(VariantInit) { if (A1) memset(G2H(A1), 0, 24); RET(0); }
HLE(VariantClear) { if (A1) { if (LD16(A1) == 8 && LD64(A1 + 8)) g_free(LD64(A1 + 8) - 4); memset(G2H(A1), 0, 24); } RET(0); }

void hle_XInputGetState(CPU* c);
void hle_XInputGetStateEx(CPU* c);
void hle_XInputSetState(CPU* c);
void hle_XInputGetCapabilities(CPU* c);
void hle_XInputGetBatteryInformation(CPU* c);
void hle_XInputEnable(CPU* c);

/* functions that only need a harmless answer; found by name when the import table was built without them */
static void late_power_info(CPU* c)
{
    /* ProcessorInformation: one PROCESSOR_POWER_INFORMATION per CPU (number, max, current, limit MHz, idle states) */
    uint64_t out = A4;
    uint32_t len = (uint32_t)ARG(5);
    if (out && len) memset(G2H(out), 0, len);
    if ((uint32_t)A1 == 11 && out)
        for (uint32_t i = 0; (i + 1) * 24 <= len; i++) { ST32(out + 24 * i, i); ST32(out + 24 * i + 4, 3000); ST32(out + 24 * i + 8, 3000); ST32(out + 24 * i + 12, 3000); }
    RET(0);
}
static void late_zero(CPU* c) { RET(0); }
static void late_one(CPU* c) { RET(1); }
static const struct { const char* name; GuestFn fn; } late[] = {
    { "CallNtPowerInformation", late_power_info }, { "GetMessageExtraInfo", late_zero }, { "UnhookWindowsHookEx", late_one },
    { "GetGeoInfoW", late_zero }, { "CertOpenSystemStoreW", late_zero }, { "DirectSoundEnumerateW", late_zero },
    { "DirectSoundEnumerateA", late_zero }, { "DirectSoundCaptureEnumerateW", late_zero }, { "DirectSoundCaptureEnumerateA", late_zero },
};

/* oleaut32 and xinput are commonly imported by ordinal */
void hle_XAudio2Create(CPU*); void hle_CreateAudioReverb(CPU*); void hle_CreateAudioVolumeMeter(CPU*); void hle_CreateFX(CPU*);
void hle_X3DAudioCalculate(CPU*); void hle_X3DAudioInitialize(CPU*);
GuestFn hle_by_ordinal(const char* dll, const char* name)
{
    if (!dll || !name) return NULL;
    for (size_t i = 0; i < sizeof late / sizeof *late; i++) if (!strcmp(name, late[i].name)) return late[i].fn;
    if (name[0] != '#') return NULL;
    int n = atoi(name + 1);
    char d[16] = { 0 };
    for (int i = 0; i < 15 && dll[i]; i++) d[i] = (char)(dll[i] >= 'A' && dll[i] <= 'Z' ? dll[i] + 32 : dll[i]);
    if (!strcmp(d, "oleaut32.dll")) switch (n) {
        case 2: return hle_SysAllocString; case 3: return hle_SysReAllocString; case 4: return hle_SysAllocStringLen;
        case 6: return hle_SysFreeString; case 7: return hle_SysStringLen; case 8: return hle_VariantInit; case 9: return hle_VariantClear;
        case 149: return hle_SysStringByteLen; case 150: return hle_SysAllocStringByteLen;
        }
    if (!strcmp(d, "dsound.dll") && (n == 7 || n == 8)) return late_zero;
    /* xaudio2_9redist.dll's export ordinals */
    if (!strncmp(d, "xaudio2_9", 9)) switch (n) {
        case 1: return hle_XAudio2Create; case 2: return hle_CreateAudioReverb; case 3: return hle_CreateAudioVolumeMeter;
        case 4: return hle_CreateFX; case 5: return hle_X3DAudioCalculate; case 6: return hle_X3DAudioInitialize;
        }
    if (!strncmp(d, "xinput", 6)) switch (n) {
        case 2: return hle_XInputGetState; case 3: return hle_XInputSetState; case 4: return hle_XInputGetCapabilities;
        case 5: return hle_XInputEnable; case 7: return hle_XInputGetBatteryInformation; case 100: return hle_XInputGetStateEx;
        }
    return NULL;
}

/* shell */
STUB(ShellExecuteW, 2)
STUB(ShellExecuteExW, 0)
/* user folders live in the save area; make sure the host side exists */
static void make_user_dir(const char* p)
{
    char host[1200];
    guest_to_host_w(p, host, sizeof host);
    char tmp[1200] = { 0 };
    size_t n = strlen(host);
    for (size_t i = 0; i <= n; i++) {
        tmp[i] = host[i];
        if (host[i] == '/' || host[i] == 0) { tmp[i] = 0; host_mkdir(tmp); tmp[i] = host[i]; }
    }
}

/* the folder behind a CSIDL, created on first use */
const char* hle_csidl_path(int csidl)
{
    csidl &= 0xFF;
    const char* p = csidl == 0x1C ? "C:\\Users\\Player\\AppData\\Local" : csidl == 0x1A ? "C:\\Users\\Player\\AppData\\Roaming"
                  : csidl == 0x05 ? "C:\\Users\\Player\\Documents" : csidl == 0x23 ? "C:\\ProgramData" : csidl == 0x28 ? "C:\\Users\\Player"
                  : csidl == 0x10 ? "C:\\Users\\Player\\Desktop" : "C:\\Users\\Player";
    make_user_dir(p);
    return p;
}
HLE(SHGetFolderPathW)
{
    utf8_to_wstr(hle_csidl_path((int)A2), ARG(5), 260);
    RET(0);
}

/* SHGetKnownFolderPath: the folders games ask for, by FOLDERID; the path is CoTaskMemAlloc'd */
HLE(SHGetKnownFolderPath)
{
    static const struct { uint32_t d1; const char* path; } known[] = {
        { 0xF1B32785, "C:\\Users\\Player\\AppData\\Local" }, { 0x3EB685DB, "C:\\Users\\Player\\AppData\\Roaming" },
        { 0xFDD39AD0, "C:\\Users\\Player\\Documents" }, { 0x4C5C32FF, "C:\\Users\\Player\\Saved Games" },
        { 0x5E6C858F, "C:\\Users\\Player" }, { 0x62AB5D82, "C:\\ProgramData" }, { 0xB4BFCC3A, "C:\\Users\\Player\\Desktop" },
        { 0xA520A1A4, "C:\\Users\\Player\\AppData\\LocalLow" }, { 0x374DE290, "C:\\Users\\Player\\Downloads" },
    };
    uint32_t d1 = LD32(A1);
    const char* p = NULL;
    for (size_t i = 0; i < sizeof known / sizeof *known; i++) if (known[i].d1 == d1) p = known[i].path;
    if (!p) { if (A4) ST64(A4, 0); RET(0x80070002); return; }
    uint64_t out = g_alloc(2 * strlen(p) + 2);
    utf8_to_wstr(p, out, strlen(p) + 1);
    make_user_dir(p);
    if (A4) ST64(A4, out);
    RET(0);
}

/* CommandLineToArgvW: Windows quoting rules; one LocalAlloc block of pointers then strings */
HLE(CommandLineToArgvW)
{
    uint64_t s = A1;
    uint16_t buf[4096];
    int n = 0, argc = 0, i = 0;
    uint16_t starts[256];
    int in_q = 0;
    uint64_t q = s;
    while (LD16(q) == ' ' || LD16(q) == '\t') q += 2;
    while (LD16(q) && argc < 256 && n < 4000) {
        starts[argc++] = (uint16_t)n;
        int first = argc == 1;
        while (LD16(q)) {
            uint16_t ch = (uint16_t)LD16(q);
            if (!in_q && (ch == ' ' || ch == '\t')) break;
            if (first) { if (ch == '"') in_q = !in_q; else buf[n++] = ch; q += 2; continue; }
            int bs = 0;
            while (LD16(q) == '\\') { bs++; q += 2; }
            if (LD16(q) == '"') {
                for (int k = 0; k < bs / 2; k++) buf[n++] = '\\';
                if (bs & 1) { buf[n++] = '"'; q += 2; continue; }
                if (in_q && LD16(q + 2) == '"') { buf[n++] = '"'; q += 4; continue; }
                in_q = !in_q;
                q += 2;
                continue;
            }
            for (int k = 0; k < bs; k++) buf[n++] = '\\';
            if (!LD16(q) || (!in_q && (LD16(q) == ' ' || LD16(q) == '\t'))) break;
            buf[n++] = (uint16_t)LD16(q);
            q += 2;
        }
        buf[n++] = 0;
        in_q = 0;
        while (LD16(q) == ' ' || LD16(q) == '\t') q += 2;
    }
    if (!argc) { starts[argc++] = (uint16_t)n; buf[n++] = 0; }
    uint64_t block = g_alloc(8ull * (argc + 1) + 2ull * n);
    uint64_t strs = block + 8ull * (argc + 1);
    for (i = 0; i < n; i++) ST16(strs + 2ull * i, buf[i]);
    for (i = 0; i < argc; i++) ST64(block + 8ull * i, strs + 2ull * starts[i]);
    ST64(block + 8ull * argc, 0);
    if (A2) ST32(A2, (uint32_t)argc);
    RET(block);
}
STUB(GetFileVersionInfoW, 0)
STUB(GetFileVersionInfoSizeW, 0)
STUB(VerQueryValueW, 0)

/* debug help, IME */
STUB(MiniDumpWriteDump, 0)
STUB(StackWalk64, 0)
STUB(SymSetOptions, 0)
STUB(SymGetOptions, 0)
STUB(SymFunctionTableAccess64, 0)
STUB(SymGetModuleInfo64, 0)
STUB(SymGetModuleInfoW64, 0)
STUB(SymGetModuleBase64, 0)
STUB(SymGetSymFromAddr64, 0)
STUB(SymGetLineFromAddr64, 0)
STUB(SymInitializeW, 0)
STUB(SymInitialize, 0)
STUB(SymRefreshModuleList, 0)
STUB(SymCleanup, 1)
STUB(ImmGetContext, 0)
STUB(ImmReleaseContext, 1)
STUB(ImmAssociateContext, 0)
STUB(ImmCreateContext, 0)
STUB(ImmDestroyContext, 1)
STUB(ImmGetCompositionStringW, 0)
STUB(ImmNotifyIME, 0)
STUB(ImmSetCompositionWindow, 0)
STUB(ImmSetCandidateWindow, 0)
STUB(ImmGetDescriptionW, 0)
STUB(ImmGetIMEFileNameW, 0)
STUB(ImmGetProperty, 0)

/* GDI and OpenGL: no native drawing surface */
STUB(GetStockObject, 0x90001)
STUB(SelectObject, 0x90001)
STUB(DeleteObject, 1)
STUB(CreateCompatibleDC, 0x70002)
STUB(CreateDCW, 0x70003)
STUB(DeleteDC, 1)
STUB(CreateFontIndirectW, 0x90002)
STUB(CreateBitmap, 0x90003)
/* regions are rectangles (round corners ignored): Slate hit-tests the cursor against its window's region */
#define RGN_BASE 0x00A00000u
static int32_t rgns[1024][4];
static int nrgn;
static uint64_t new_rgn(int32_t l, int32_t t, int32_t r, int32_t b)
{
    int i = nrgn < 1024 ? nrgn++ : 1023;
    rgns[i][0] = l; rgns[i][1] = t; rgns[i][2] = r; rgns[i][3] = b;
    return RGN_BASE + (uint64_t)i;
}
static int32_t* rgn(uint64_t h) { return h >= RGN_BASE && h < RGN_BASE + 1024u ? rgns[h - RGN_BASE] : NULL; }
HLE(CreateRectRgn) { RET(new_rgn((int32_t)A1, (int32_t)A2, (int32_t)A3, (int32_t)A4)); }
HLE(CreateRectRgnIndirect) { RET(new_rgn((int32_t)LD32(A1), (int32_t)LD32(A1 + 4), (int32_t)LD32(A1 + 8), (int32_t)LD32(A1 + 12))); }
HLE(CreateRoundRectRgn) { RET(new_rgn((int32_t)A1, (int32_t)A2, (int32_t)A3, (int32_t)A4)); }
HLE(PtInRegion)
{
    int32_t* r = rgn(A1);
    int32_t x = (int32_t)A2, y = (int32_t)A3;
    RET(r && x >= r[0] && x < r[2] && y >= r[1] && y < r[3]);
}
HLE(GetRgnBox)
{
    int32_t* r = rgn(A1);
    if (!r) { RET(0); return; }
    for (int i = 0; i < 4; i++) ST32(A2 + 4ull * i, (uint32_t)r[i]);
    RET(2);
}
HLE(CombineRgn)
{
    int32_t *d = rgn(A1), *a = rgn(A2), *b = rgn(A3);
    if (!d || !a) { RET(0); return; }
    memcpy(d, a, 16);
    if (b && (uint32_t)A4 == 2) { if (b[0] < d[0]) d[0] = b[0]; if (b[1] < d[1]) d[1] = b[1]; if (b[2] > d[2]) d[2] = b[2]; if (b[3] > d[3]) d[3] = b[3]; }
    RET(2);
}
STUB(SetTextColor, 0)
STUB(SetBkColor, 0)
STUB(SetBkMode, 1)
STUB(SetTextAlign, 0)
STUB(TextOutW, 1)
STUB(ExtEscape, 0)
STUB(GetObjectW, 0)
STUB(ChoosePixelFormat, 0)
STUB(SetPixelFormat, 0)
STUB(SwapBuffers, 0)
HLE(GetDeviceCaps)
{
    int i = (int)A2;
    RET(i == 88 || i == 90 ? 96 : i == 12 ? 32 : i == 8 ? 1920 : i == 10 ? 1080 : i == 116 ? 60 : i == 14 ? 1 : 0);
}
HLE(GetTextExtentPoint32W) { ST32(A4, (uint32_t)(8 * A3)); ST32(A4 + 4, 16); RET(1); }
HLE(CreateDIBSection) { uint32_t w = (uint32_t)LD32(A2 + 4), h = (uint32_t)(int32_t)LD32(A2 + 8); uint64_t bits = g_calloc((uint64_t)w * (h & 0x7FFFFFFF) * 4 + 16); if (A4) ST64(A4, bits); RET(0x90006); }
STUB(wglCreateContext, 0)
STUB(wglDeleteContext, 0)
STUB(wglMakeCurrent, 0)
STUB(wglGetCurrentContext, 0)
STUB(wglGetCurrentDC, 0)
STUB(wglGetProcAddress, 0)

/* crypto, devices, compositor, timers */
STUB(BCryptOpenAlgorithmProvider, 0xC00000BB)
STUB(BCryptCloseAlgorithmProvider, 0)
STUB(BCryptGetProperty, 0xC00000BB)
STUB(BCryptCreateHash, 0xC00000BB)
STUB(BCryptHashData, 0xC00000BB)
STUB(BCryptFinishHash, 0xC00000BB)
STUB(BCryptDestroyHash, 0)
HLE(BCryptGenRandom) { for (uint64_t i = 0; i < A3; i++) ST8(A2 + i, (uint64_t)(rand() & 0xFF)); RET(0); }
STUB(CryptProtectMemory, 1)
STUB(CryptUnprotectMemory, 1)
STUB(CertCreateCertificateContext, 0)
STUB(CertFreeCertificateContext, 1)
STUB(CertGetCertificateChain, 0)
STUB(CertFreeCertificateChain, 0)
void hle_set_error(uint32_t e);
STUB(SetupDiGetClassDevsExW, 0xFFFFFFFFFFFFFFFFull)
STUB(SetupDiGetClassDevsW, 0xFFFFFFFFFFFFFFFFull)
HLE(SetupDiEnumDeviceInfo) { hle_set_error(259); RET(0); }
STUB(SetupDiDestroyDeviceInfoList, 1)
STUB(SetupDiOpenDevRegKey, 0xFFFFFFFFFFFFFFFFull)
STUB(CM_Get_Device_IDW, 0x25)
HLE(DwmIsCompositionEnabled) { ST32(A1, 1); RET(0); }
STUB(DwmFlush, 0)
STUB(DwmSetWindowAttribute, 0)
STUB(DwmGetCompositionTimingInfo, 0x80004005)
STUB(timeBeginPeriod, 0)
STUB(timeEndPeriod, 0)
HLE(timeGetTime) { RET((uint32_t)(host_ticks_ns() / 1000000)); }
STUB(D3DPERF_BeginEvent, 0)
STUB(D3DPERF_EndEvent, 0)
STUB(D3DPERF_SetMarker, 0)
STUB(D3DPERF_GetStatus, 0)
STUB(DirectSoundCaptureCreate8, 0x88780078)

/* kernel32 leftovers */
HLE(FormatMessageW)
{
    char b[64];
    snprintf(b, sizeof b, "Error %u", (unsigned)A3);
    uint64_t flags = A1;
    if (flags & 0x100) {
        uint64_t buf = g_alloc(2 * sizeof b);
        utf8_to_wstr(b, buf, sizeof b);
        ST64(ARG(5), buf);
    } else utf8_to_wstr(b, ARG(5), (size_t)ARG(6));
    RET(strlen(b));
}
HLE(FormatMessageA)
{
    char b[64];
    snprintf(b, sizeof b, "Error %u", (unsigned)A3);
    if (A1 & 0x100) { ST64(ARG(5), g_strdup(b)); }
    else snprintf((char*)G2H(ARG(5)), (size_t)ARG(6), "%s", b);
    RET(strlen(b));
}
/* the processor count the game sees; ?cores=N overrides it (everything runs on one browser thread either way, so fewer
   cores means fewer worker threads taking turns) */
int host_query_int(const char* name);
int hle_cores(void)
{
    static int n;
    if (!n) { n = host_query_int("cores"); if (n < 1 || n > 16) n = 4; }
    return n;
}
#define CORES hle_cores()
HLE(GetLogicalProcessorInformation)
{
    uint32_t need = 32 * (CORES + 1);
    if ((uint32_t)LD32(A2) < need || !A1) { ST32(A2, need); ST32(g_teb + 0x68, 122); RET(0); return; }
    memset(G2H(A1), 0, need);
    for (int i = 0; i < CORES; i++) { ST64(A1 + 32ull * i, 1ull << i); ST32(A1 + 32ull * i + 8, 0); }
    ST64(A1 + 32ull * CORES, (1ull << CORES) - 1);
    ST32(A1 + 32ull * CORES + 8, 3);
    ST32(A2, need);
    RET(1);
}
HLE(GetLogicalProcessorInformationEx)
{
    uint32_t rel = (uint32_t)A1, each = 48, n = rel == 0 || rel == 0xFFFF ? CORES : 1, need = each * n;
    if ((uint32_t)LD32(A3) < need || !A2) { ST32(A3, need); ST32(g_teb + 0x68, 122); RET(0); return; }
    memset(G2H(A2), 0, need);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t p = A2 + (uint64_t)each * i;
        ST32(p, rel == 0xFFFF ? 0 : rel);
        ST32(p + 4, each);
        ST16(p + 30, 1);
        ST64(p + 32, rel == 3 ? (1ull << CORES) - 1 : 1ull << i);
    }
    ST32(A3, need);
    RET(1);
}
HLE(GetThreadGroupAffinity) { memset(G2H(A2), 0, 16); ST64(A2, (1ull << CORES) - 1); RET(1); }
HLE(GetNumaHighestNodeNumber) { ST32(A1, 0); RET(1); }
HLE(GetCurrentProcessorNumberEx) { memset(G2H(A1), 0, 4); RET(0); }
STUB(FlushProcessWriteBuffers, 0)
STUB(SetCriticalSectionSpinCount, 0)
HLE(OpenThread) { RET(0x8001); }
STUB(SetThreadContext, 0)
STUB(SetHandleInformation, 1)
STUB(CreateWaitableTimerA, 0x8002)
STUB(CreateWaitableTimerW, 0x8002)
STUB(SetWaitableTimer, 1)
STUB(CreateToolhelp32Snapshot, 0xFFFFFFFFFFFFFFFFull)
STUB(Process32FirstW, 0)
STUB(Process32NextW, 0)
STUB(Thread32First, 0)
STUB(Thread32Next, 0)
HLE(GetProcessHandleCount) { ST32(A2, 64); RET(1); }
STUB(RtlCaptureStackBackTrace, 0)
HLE(lstrlenA) { RET(A1 ? strlen(GSTR(A1)) : 0); }
HLE(lstrlenW) { uint64_t n = 0; while (A1 && LD16(A1 + 2 * n)) n++; RET(n); }
HLE(GetSystemDirectoryW) { RET(utf8_to_wstr("C:\\Windows\\System32", A1, (size_t)A2)); }
HLE(GetSystemDirectoryA) { snprintf((char*)G2H(A1), (size_t)A2, "C:\\Windows\\System32"); RET(19); }
HLE(GetWindowsDirectoryW) { RET(utf8_to_wstr("C:\\Windows", A1, (size_t)A2)); }
HLE(GetLongPathNameW) { uint64_t n = 0; while (LD16(A1 + 2 * n)) n++; if (A3 > n) memcpy(G2H(A2), G2H(A1), 2 * (n + 1)); RET(n); }
HLE(ExpandEnvironmentStringsA) { size_t n = strlen(GSTR(A1)); if (A3 > n) memcpy(G2H(A2), G2H(A1), n + 1); RET(n + 1); }
HLE(GetFinalPathNameByHandleW) { RET(utf8_to_wstr("C:\\file", A2, (size_t)A3)); }
HLE(GetCurrencyFormatW) { uint64_t n = 0; while (LD16(A3 + 2 * n)) n++; if ((int32_t)ARG(6) > (int64_t)n) memcpy(G2H(ARG(5)), G2H(A3), 2 * (n + 1)); RET(n + 1); }
HLE(GetNumberFormatW) { uint64_t n = 0; while (LD16(A3 + 2 * n)) n++; if ((int32_t)ARG(6) > (int64_t)n) memcpy(G2H(ARG(5)), G2H(A3), 2 * (n + 1)); RET(n + 1); }
STUB(GetGeoInfoA, 0)
STUB(FlushViewOfFile, 1)
HLE(GlobalMemoryStatus) { ST32(A1, 56); ST32(A1 + 4, 30); ST64(A1 + 8, MEM_SIZE); ST64(A1 + 16, MEM_SIZE / 2); ST64(A1 + 24, MEM_SIZE); ST64(A1 + 32, MEM_SIZE); ST64(A1 + 40, 1ull << 40); ST64(A1 + 48, 1ull << 39); RET(0); }
STUB(CreateSymbolicLinkW, 0)
STUB(CreateHardLinkW, 0)
STUB(AreFileApisANSI, 1)
STUB(SetFileInformationByHandle, 1)
STUB(DisableThreadLibraryCalls, 1)
STUB(SetDllDirectoryW, 1)
HLE(GetDllDirectoryW) { if (A1) ST16(A2, 0); RET(0); }
STUB(AttachConsole, 0)
STUB(SetConsoleScreenBufferSize, 1)
STUB(SetConsoleWindowInfo, 1)
STUB(FlushConsoleInputBuffer, 1)
STUB(FindResourceW, 0)
STUB(FindResourceA, 0)
STUB(LoadResource, 0)
STUB(LockResource, 0)
STUB(SizeofResource, 0)
HLE(CopyFile2) { extern void hle_CopyFileW(CPU*); R(8) = 0; hle_CopyFileW(c); RET((uint32_t)R(0) ? 0 : 0x80070002); }

typedef struct { uint64_t file; uint64_t size; } Mapping;
void hle_set_error(uint32_t e);
HLE(CreateFileMappingW)
{
    hle_set_error(0);
    Mapping* m = calloc(1, sizeof *m);
    m->file = A1;
    m->size = ((uint64_t)(uint32_t)A4 << 32) | (uint32_t)ARG(5);
    RET(handle_new(5, m));
}
HLE(CreateFileMappingA) { hle_CreateFileMappingW(c); }
HLE(MapViewOfFile)
{
    Mapping* m = handle_obj(A1);
    if (!m) { RET(0); return; }
    uint64_t off = ((uint64_t)(uint32_t)A3 << 32) | (uint32_t)A4, len = ARG(5);
    extern void hle_GetFileSizeEx(CPU*);
    if (!len && (m->file == (uint64_t)-1 || !m->file)) len = m->size > off ? m->size - off : 0;
    if (!len) {
        uint64_t tmp = g_calloc(8);
        uint64_t s1 = R(1), s2 = R(2);
        R(1) = m->file; R(2) = tmp;
        hle_GetFileSizeEx(c);
        len = LD64(tmp) - off;
        R(1) = s1; R(2) = s2;
    }
    /* a view is a copy of that part of the file, in pages of its own so unmapping gives them back */
    uint64_t mem = g_pages(len + 16);
    if (!mem) { hle_set_error(8); RET(0); return; }
    extern uint64_t hle_file_pread(uint64_t h, uint64_t off, uint64_t buf, uint64_t len);
    uint64_t got = m->file ? hle_file_pread(m->file, off, mem, len) : 0;
    memset(G2H(mem + got), 0, (size_t)(len + 16 - got));
    RET(mem);
}
HLE(UnmapViewOfFile) { if (A1 && g_pages_query(A1, NULL) == A1) g_pages_free(A1); RET(1); }

typedef struct { uint64_t fn, ctx; } Work;
HLE(CreateThreadpoolWork) { Work* w = calloc(1, sizeof *w); w->fn = A1; w->ctx = A2; RET(handle_new(5, w)); }
HLE(SubmitThreadpoolWork) { Work* w = handle_obj(A1); if (w) { uint64_t a[3] = { 0, w->ctx, A1 }; rt_invoke(c, w->fn, 3, a); } RET(0); }
STUB(CloseThreadpoolWork, 0)
STUB(WaitForThreadpoolWorkCallbacks, 0)
STUB(CreateThreadpoolWait, 0x8003)
STUB(SetThreadpoolWait, 0)
STUB(CloseThreadpoolWait, 0)
STUB(CreateThreadpoolTimer, 0x8004)
STUB(SetThreadpoolTimer, 0)
STUB(CloseThreadpoolTimer, 0)
STUB(WaitForThreadpoolTimerCallbacks, 0)
STUB(FreeLibraryWhenCallbackReturns, 0)

STUB(DirectSoundCreate8, 0x88780078)
STUB(DirectSoundCreate, 0x88780078)
/* XINPUT_GAMEPAD: buttons u16, triggers 2 x u8, sticks 4 x s16 */
static int pad_read(uint32_t i, uint64_t out)
{
    static uint8_t last[4][12];
    static uint32_t packet[4];
    uint8_t g[12];
    if (i >= 4 || !host_gamepad((int)i, (uint16_t*)g, g + 2, (int16_t*)(g + 4))) return 0;
    if (memcmp(last[i], g, 12)) { memcpy(last[i], g, 12); packet[i]++; }
    ST32(out, packet[i]);
    memcpy(G2H(out + 4), g, 12);
    return 1;
}
HLE(XInputGetState) { RET(pad_read((uint32_t)A1, A2) ? 0 : 1167); }
HLE(XInputGetStateEx) { hle_XInputGetState(c); }
HLE(XInputSetState) { if ((uint32_t)A1 < 4 && A2) host_rumble((int)A1, LD16(A2), LD16(A2 + 2)); RET(0); }
HLE(XInputGetCapabilities)
{
    uint64_t tmp = g_calloc(16);
    int ok = pad_read((uint32_t)A1, tmp);
    g_free(tmp);
    if (!ok) { RET(1167); return; }
    memset(G2H(A3), 0, 20);
    ST8(A3, 1);
    ST8(A3 + 1, 1);
    ST16(A3 + 4, 0xF3FF);
    ST8(A3 + 6, 255); ST8(A3 + 7, 255);
    ST16(A3 + 8, 0xFFC0); ST16(A3 + 10, 0xFFC0); ST16(A3 + 12, 0xFFC0); ST16(A3 + 14, 0xFFC0);
    ST16(A3 + 16, 0xFFFF); ST16(A3 + 18, 0xFFFF);
    RET(0);
}
HLE(XInputGetBatteryInformation) { if (A3) { ST8(A3, 1); ST8(A3 + 1, 3); } RET(0); }
STUB(XInputEnable, 0)
