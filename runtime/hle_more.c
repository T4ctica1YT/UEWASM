/* More Windows APIs games import: fibers, ANSI variants, profile (.ini) files, system queries, GDI and dialog
   drawing (headless here), device enumeration (no devices), and services a browser doesn't have, which fail the way
   Windows does when the feature is absent so callers take their fallback path. */
#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

void hle_set_error(uint32_t e);
uint64_t handle_new(int kind, void* obj);
void hle_GetMonitorInfoW(CPU*);
void hle_EnumDisplaySettingsW(CPU*);
void hle_CreateDirectoryW(CPU*);
void hle_MoveFileExW(CPU*);
void hle_GetFileInformationByHandle(CPU*);
void hle_VirtualQuery(CPU*);
void hle_GetModuleFileNameW(CPU*);
void hle_RegEnumKeyExW(CPU*);
void hle_RegGetValueW(CPU*);
void hle_GetFileVersionInfoW(CPU*);
void hle_GetFileVersionInfoSizeW(CPU*);
void hle_VerQueryValueW(CPU*);
void hle_ShellExecuteW(CPU*);

static void read_a(uint64_t p, char* out, size_t cap) { size_t i = 0; for (; p && i + 1 < cap; i++) { out[i] = (char)LD8(p + i); if (!out[i]) return; } out[i] = 0; }
static void read_w(uint64_t p, char* out, size_t cap) { if (!p) { out[0] = 0; return; } wstr_to_utf8(p, out, cap); }
static size_t wlen(uint64_t p) { size_t n = 0; while (p && LD16(p + 2 * n)) n++; return n; }
/* an ANSI string argument as a temporary wide guest string (freed by the caller with g_free) */
static uint64_t a2w(uint64_t a)
{
    if (!a) return 0;
    char s[2048];
    read_a(a, s, sizeof s);
    uint64_t w = g_alloc(2 * strlen(s) + 2);
    utf8_to_wstr(s, w, strlen(s) + 1);
    return w;
}
static void w2a(uint64_t w, uint64_t a, size_t cap) { char s[2048]; read_w(w, s, sizeof s); size_t n = strlen(s); if (n + 1 > cap) n = cap ? cap - 1 : 0; if (cap) { memcpy(G2H(a), s, n); ST8(a + n, 0); } }

/* ---- fibers: each is a coroutine of its own; the thread's registers go with whichever fiber runs ---- */

typedef struct Fiber {
    uint64_t guest;             /* FIBER object: fiber data at +0 (GetFiberData reads it) */
    void* hctx;
    uint64_t start, stack;
    uint64_t r[16];
    XMM x[16];
    uint32_t mxcsr;
    int converted;              /* made from a thread by ConvertThreadToFiber */
} Fiber;
enum { NFIB = 1024 };
static Fiber* fibers[NFIB];

static Fiber* fib_of(uint64_t g) { for (int i = 0; i < NFIB; i++) if (fibers[i] && fibers[i]->guest == g) return fibers[i]; return NULL; }
static Fiber* fib_new(uint64_t data)
{
    for (int i = 0; i < NFIB; i++)
        if (!fibers[i]) {
            Fiber* f = calloc(1, sizeof *f);
            f->guest = g_calloc(64);
            ST64(f->guest, data);
            fibers[i] = f;
            return f;
        }
    return NULL;
}
static void fib_free(Fiber* f)
{
    for (int i = 0; i < NFIB; i++) if (fibers[i] == f) fibers[i] = NULL;
    if (!f->converted && f->hctx) host_ctx_free(f->hctx);
    if (f->stack) g_free(f->stack);
    g_free(f->guest);
    free(f);
}
/* NT_TIB.FiberData is TEB+0x20: GetCurrentFiber reads it inline */
static uint64_t cur_fiber_guest(CPU* c) { return LD64(c->gsbase + 0x20); }

static void fiber_entry(void* arg)
{
    Fiber* f = arg;
    CPU* c = sched_current_cpu();
    R(4) = (f->stack + LD64(f->guest + 8) - 64) & ~15ull;
    uint64_t data = LD64(f->guest);
    rt_invoke(c, f->start, 1, &data);
    /* a fiber routine that returns ends its thread */
    sched_exit_current(c, 0);
}

HLE(ConvertThreadToFiber)
{
    if (cur_fiber_guest(c) && fib_of(cur_fiber_guest(c))) { hle_set_error(1280); RET(0); return; }   /* ERROR_ALREADY_FIBER */
    Fiber* f = fib_new(A1);
    if (!f) { hle_set_error(8); RET(0); return; }
    f->converted = 1;
    f->hctx = sched_current()->ctx;
    ST64(c->gsbase + 0x20, f->guest);
    RET(f->guest);
}
HLE(ConvertThreadToFiberEx) { hle_ConvertThreadToFiber(c); }
HLE(ConvertFiberToThread)
{
    Fiber* f = fib_of(cur_fiber_guest(c));
    if (!f || !f->converted) { hle_set_error(1281); RET(0); return; }   /* ERROR_ALREADY_THREAD */
    ST64(c->gsbase + 0x20, 0);
    fib_free(f);
    RET(1);
}
static void create_fiber(CPU* c, uint64_t size, uint64_t start, uint64_t data)
{
    Fiber* f = fib_new(data);
    if (!f) { hle_set_error(8); RET(0); return; }
    if (size < (64u << 10)) size = 1u << 20;
    f->stack = g_alloc(size);
    ST64(f->guest + 8, size);
    f->start = start;
    f->hctx = host_ctx_new(fiber_entry, f, 2u << 20);
    if (!f->stack || !f->hctx) { fib_free(f); hle_set_error(8); RET(0); return; }
    RET(f->guest);
}
HLE(CreateFiber) { create_fiber(c, A1, A2, A3); }
HLE(CreateFiberEx) { create_fiber(c, A2 ? A2 : A1, A4, ARG(5)); }
HLE(SwitchToFiber)
{
    Fiber* to = fib_of(A1);
    Fiber* me = fib_of(cur_fiber_guest(c));
    if (!to || to == me) { RET(0); return; }
    if (me) { memcpy(me->r, c->r, sizeof me->r); memcpy(me->x, c->x, sizeof me->x); me->mxcsr = c->mxcsr; }
    ST64(c->gsbase + 0x20, to->guest);
    GThread* t = sched_current();
    t->ctx = to->hctx;
    host_ctx_switch(to->hctx);
    /* back on this fiber */
    if (me) { memcpy(c->r, me->r, sizeof me->r); memcpy(c->x, me->x, sizeof me->x); c->mxcsr = me->mxcsr; t->ctx = me->hctx; }
    RET(0);
}
HLE(DeleteFiber)
{
    Fiber* f = fib_of(A1);
    if (!f) { RET(0); return; }
    if (f->guest == cur_fiber_guest(c)) sched_exit_current(c, 0);
    fib_free(f);
    RET(0);
}
HLE(IsThreadAFiber) { RET(fib_of(cur_fiber_guest(c)) != NULL); }

/* ---- kernel32: processes, NUMA, memory, job objects ---- */

HLE(GetNumaProcessorNode) { if (A2) ST8(A2, 0); RET(1); }
HLE(GetNumaProcessorNodeEx) { if (A2) ST16(A2, 0); RET(1); }
int hle_cores(void);
static uint64_t all_cores_mask(void) { int n = hle_cores(); return n >= 64 ? ~0ull : (1ull << n) - 1; }
HLE(GetNumaNodeProcessorMask) { if ((uint8_t)A1) { RET(0); return; } if (A2) ST64(A2, all_cores_mask()); RET(1); }
/* GROUP_AFFINITY: mask, group, reserved */
HLE(GetNumaNodeProcessorMaskEx) { if ((uint16_t)A1) { RET(0); return; } if (A2) { memset(G2H(A2), 0, 16); ST64(A2, all_cores_mask()); } RET(1); }
HLE(GetThreadIdealProcessorEx) { if (A2) { ST16(A2, 0); ST8(A2 + 2, 0); } RET(1); }
HLE(IsProcessInJob) { if (A3) ST32(A3, 0); RET(1); }
HLE(QueryInformationJobObject) { hle_set_error(6); RET(0); }
static uint64_t mem_note;
HLE(CreateMemoryResourceNotification) { if (!mem_note) mem_note = handle_new(5, NULL); RET(mem_note); }
/* low memory: answer from the guest heap use */
HLE(QueryMemoryResourceNotification) { if (A2) ST32(A2, 0); RET(1); }
HLE(GetHandleInformation) { if (A2) ST32(A2, 0); RET(1); }
HLE(QueryUnbiasedInterruptTime) { if (A1) ST64(A1, host_ticks_ns() / 100); RET(1); }
HLE(GetSystemTimeAdjustment) { if (A1) ST32(A1, 156250); if (A2) ST32(A2, 156250); if (A3) ST32(A3, 1); RET(1); }
HLE(WaitForThreadpoolWaitCallbacks) { RET(0); }
HLE(WerRegisterRuntimeExceptionModule) { RET(0); }
HLE(SetConsoleOutputCP) { RET(1); }
HLE(SetConsoleTitleA) { RET(1); }
HLE(VirtualQueryEx) { R(1) = A2; R(2) = A3; R(8) = A4; hle_VirtualQuery(c); }
HLE(WriteProcessMemory)
{
    /* only into this process */
    if (A3 && A4) memmove(G2H(A2), G2H(A3), (size_t)A4);
    if (ARG(5)) ST64(ARG(5), A4);
    RET(1);
}
HLE(QueryFullProcessImageNameW)
{
    char s[600];
    snprintf(s, sizeof s, "C:\\%s", g_exe_rel);
    for (char* p = s; *p; p++) if (*p == '/') *p = '\\';
    uint32_t cap = A4 ? (uint32_t)LD32(A4) : 0, n = (uint32_t)strlen(s);
    if (n + 1 > cap) { hle_set_error(122); RET(0); return; }
    utf8_to_wstr(s, A3, cap);
    ST32(A4, n);
    RET(1);
}
HLE(QueryFullProcessImageNameA)
{
    char s[600];
    snprintf(s, sizeof s, "C:\\%s", g_exe_rel);
    for (char* p = s; *p; p++) if (*p == '/') *p = '\\';
    uint32_t cap = A4 ? (uint32_t)LD32(A4) : 0, n = (uint32_t)strlen(s);
    if (n + 1 > cap) { hle_set_error(122); RET(0); return; }
    memcpy(G2H(A3), s, n + 1);
    ST32(A4, n);
    RET(1);
}
/* process creation attributes: accepted and ignored (CreateProcess fails here anyway) */
HLE(InitializeProcThreadAttributeList) { if (A4) { if (!A1) { ST64(A4, 64 + 32 * (uint64_t)A2); hle_set_error(122); RET(0); return; } } RET(1); }
HLE(UpdateProcThreadAttribute) { RET(1); }
HLE(DeleteProcThreadAttributeList) { RET(0); }

/* toolhelp module walk: no snapshot of other processes, and modules of this one aren't listed */
HLE(Module32FirstW) { hle_set_error(18); RET(0); }
HLE(Module32NextW) { hle_set_error(18); RET(0); }
HLE(Module32First) { hle_set_error(18); RET(0); }
HLE(Module32Next) { hle_set_error(18); RET(0); }

/* named pipes: no other processes to talk to */
HLE(CreateNamedPipeW) { hle_set_error(50); RET((uint64_t)-1); }
HLE(CreateNamedPipeA) { hle_set_error(50); RET((uint64_t)-1); }
HLE(WaitNamedPipeW) { hle_set_error(2); RET(0); }
HLE(TransactNamedPipe) { hle_set_error(6); RET(0); }
HLE(SetNamedPipeHandleState) { hle_set_error(6); RET(0); }
HLE(ConnectNamedPipe) { hle_set_error(6); RET(0); }
HLE(DisconnectNamedPipe) { RET(1); }

/* ---- files ---- */

/* FILETIME from BY_HANDLE_FILE_INFORMATION: creation +4, access +12, write +20 */
HLE(GetFileTime)
{
    uint64_t ct = A2, at = A3, wt = A4;
    uint64_t info = g_alloc(64);
    R(2) = info;
    hle_GetFileInformationByHandle(c);
    int ok = (int)R(0);
    if (ok) { if (ct) ST64(ct, LD64(info + 4)); if (at) ST64(at, LD64(info + 12)); if (wt) ST64(wt, LD64(info + 20)); }
    g_free(info);
    RET(ok);
}
/* path, prefix, number .tmp; with no unique number given, the file is created so the name stays taken */
HLE(GetTempFileNameW)
{
    static uint32_t counter;
    char dir[600], pre[16], name[700], host[1200];
    read_w(A1, dir, sizeof dir); read_w(A2, pre, sizeof pre);
    pre[3] = 0;
    uint32_t u = (uint32_t)A3 & 0xFFFF;
    extern void guest_to_host(const char* guest, char* out, size_t cap);
    for (int tries = 0; tries < 65535; tries++) {
        uint32_t n = u ? u : (uint32_t)((host_ticks_ns() / 1000 + ++counter) & 0xFFFF);
        if (!n) continue;
        size_t l = strlen(dir);
        snprintf(name, sizeof name, "%s%s%s%04X.tmp", dir, l && dir[l - 1] != 92 ? "\\" : "", pre, n);
        if (u) { u = n; break; }
        guest_to_host(name, host, sizeof host);
        HostStat st;
        if (host_stat(host, &st)) continue;
        FILE* f = fopen(host, "wb");
        if (f) fclose(f);
        u = n;
        break;
    }
    utf8_to_wstr(name, A4, 260);
    RET(u);
}
HLE(GetTempFileNameA)
{
    uint64_t p = a2w(A1), pre = a2w(A2), out = g_alloc(520);
    R(1) = p; R(2) = pre; R(9) = out;
    hle_GetTempFileNameW(c);
    uint64_t r = R(0);
    if (r) w2a(out, A4, 260);
    g_free(p); g_free(pre); g_free(out);
    RET(r);
}
HLE(MoveFileExA) { uint64_t a = a2w(A1), b = a2w(A2); R(1) = a; R(2) = b; hle_MoveFileExW(c); g_free(a); g_free(b); }
HLE(ReplaceFileW)
{
    /* replaced <- replacement (no backup kept) */
    R(1) = A2; R(2) = A1; R(8) = 1;
    hle_MoveFileExW(c);
}
HLE(CreateDirectoryExW) { R(1) = A2; R(2) = A3; hle_CreateDirectoryW(c); }

/* ---- profile (.ini) files: read from the game's files, writes accepted and dropped ---- */

static int ini_get(const char* file, const char* sec, const char* key, char* out, size_t cap)
{
    char host[1200];
    extern void guest_to_host(const char* guest, char* out, size_t cap);
    guest_to_host(file, host, sizeof host);
    FILE* f = fopen(host, "rb");
    if (!f) return 0;
    char line[1024];
    int in = !sec, found = 0;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        char* e = p + strlen(p);
        while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (*p == '[') { char* q = strchr(p, ']'); if (q) *q = 0; in = sec && !strcasecmp(p + 1, sec); continue; }
        if (!in || *p == ';' || *p == '#') continue;
        char* eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = p; char* ke = eq; while (ke > k && ke[-1] == ' ') *--ke = 0;
        char* v = eq + 1; while (*v == ' ') v++;
        if (key && !strcasecmp(k, key)) { snprintf(out, cap, "%s", v); found = 1; break; }
    }
    fclose(f);
    return found;
}
HLE(GetPrivateProfileStringW)
{
    char sec[256], key[256], def[1024], file[600], val[1024];
    read_w(A1, sec, sizeof sec); read_w(A2, key, sizeof key); read_w(A3, def, sizeof def); read_w(ARG(6), file, sizeof file);
    uint32_t cap = (uint32_t)ARG(5);
    if (!cap) { RET(0); return; }
    const char* v = (A1 && A2 && ini_get(file, sec, key, val, sizeof val)) ? val : (A3 ? def : "");
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", v);
    size_t n = strlen(tmp);
    if (n + 1 > cap) n = cap - 1;
    tmp[n] = 0;
    utf8_to_wstr(tmp, A4, cap);
    RET(n);
}
HLE(GetPrivateProfileStringA)
{
    char sec[256], key[256], def[1024], file[600], val[1024];
    read_a(A1, sec, sizeof sec); read_a(A2, key, sizeof key); read_a(A3, def, sizeof def); read_a(ARG(6), file, sizeof file);
    uint32_t cap = (uint32_t)ARG(5);
    if (!cap) { RET(0); return; }
    const char* v = (A1 && A2 && ini_get(file, sec, key, val, sizeof val)) ? val : (A3 ? def : "");
    size_t n = strlen(v);
    if (n + 1 > cap) n = cap - 1;
    memcpy(G2H(A4), v, n);
    ST8(A4 + n, 0);
    RET(n);
}
HLE(GetPrivateProfileIntW)
{
    char sec[256], key[256], file[600], val[64];
    read_w(A1, sec, sizeof sec); read_w(A2, key, sizeof key); read_w(A4, file, sizeof file);
    RET(ini_get(file, sec, key, val, sizeof val) ? (uint64_t)(uint32_t)strtol(val, NULL, 0) : (uint32_t)A3);
}
HLE(GetPrivateProfileIntA)
{
    char sec[256], key[256], file[600], val[64];
    read_a(A1, sec, sizeof sec); read_a(A2, key, sizeof key); read_a(A4, file, sizeof file);
    RET(ini_get(file, sec, key, val, sizeof val) ? (uint64_t)(uint32_t)strtol(val, NULL, 0) : (uint32_t)A3);
}
HLE(WritePrivateProfileStringW) { RET(1); }
HLE(WritePrivateProfileStringA) { RET(1); }

/* ---- ntdll ---- */

HLE(RtlInitUnicodeString)
{
    /* UNICODE_STRING: Length, MaximumLength (bytes), Buffer at +8 */
    size_t n = wlen(A2);
    ST16(A1, (uint16_t)(2 * n)); ST16(A1 + 2, (uint16_t)(A2 ? 2 * n + 2 : 0)); ST64(A1 + 8, A2);
    RET(0);
}
HLE(RtlInitAnsiString) { size_t n = A2 ? strlen((const char*)G2H(A2)) : 0; ST16(A1, (uint16_t)n); ST16(A1 + 2, (uint16_t)(A2 ? n + 1 : 0)); ST64(A1 + 8, A2); RET(0); }
HLE(RtlNtStatusToDosError)
{
    uint32_t s = (uint32_t)A1;
    uint32_t e = s == 0 ? 0 : s == 0xC0000034 ? 2 : s == 0xC000003A ? 3 : s == 0xC0000022 ? 5 : s == 0xC0000008 ? 6 : s == 0xC0000017 ? 8
               : s == 0xC000000D ? 87 : s == 0xC0000023 ? 122 : s == 0xC0000011 ? 38 : s == 0x00000103 ? 997 : s == 0xC0000002 ? 1 : 317;
    RET(e);
}
HLE(NtSetInformationThread) { RET(0); }

/* ---- shell paths ---- */

static uint64_t strip_spec(uint64_t p, int keep_root_slash)
{
    size_t n = wlen(p), last = (size_t)-1;
    for (size_t i = 0; i < n; i++) { uint16_t ch = (uint16_t)LD16(p + 2 * i); if (ch == '\\' || ch == '/') last = i; }
    if (last == (size_t)-1) { if (n) { ST16(p, 0); return 1; } return 0; }
    size_t cut = last;
    if (keep_root_slash && (last == 2 && LD16(p + 2) == ':')) cut = last + 1;
    if (last == 0) cut = 1;
    if (cut >= n) return 0;
    ST16(p + 2 * cut, 0);
    return 1;
}
HLE(PathRemoveFileSpecW) { RET(A1 ? strip_spec(A1, 1) : 0); }
HLE(PathCchRemoveFileSpec) { RET(A1 && strip_spec(A1, 1) ? 0 : 1); }   /* S_OK, or S_FALSE when nothing was removed */
HLE(PathCchRemoveBackslash)
{
    size_t n = wlen(A1);
    if (n > 3 && (LD16(A1 + 2 * (n - 1)) == '\\')) { ST16(A1 + 2 * (n - 1), 0); RET(0); return; }
    RET(1);
}
HLE(ShellExecuteA) { RET(2); }    /* SE_ERR_FNF: nothing to launch */
HLE(SHCreateItemFromParsingName) { if (A4) ST64(A4, 0); RET(0x80004001); }

/* ---- version info, ANSI ---- */
HLE(GetFileVersionInfoSizeA) { if (A2) ST32(A2, 0); hle_set_error(1812); RET(0); }
HLE(GetFileVersionInfoA) { hle_set_error(1812); RET(0); }
HLE(GetFileVersionInfoSizeExW) { R(1) = A2; R(2) = A3; hle_GetFileVersionInfoSizeW(c); }
HLE(GetFileVersionInfoExW) { R(1) = A2; R(2) = A3; R(8) = A4; R(9) = ARG(5); hle_GetFileVersionInfoW(c); }
HLE(VerQueryValueA) { if (A3) ST64(A3, 0); if (A4) ST32(A4, 0); RET(0); }

/* ---- registry, ANSI and the rest: an empty registry ---- */
HLE(RegGetValueA) { RET(2); }
HLE(RegEnumKeyExA) { RET(259); }
HLE(RegQueryInfoKeyW)
{
    /* subkeys, max subkey len, max class len, values, max value name, max value data, security, last write */
    uint64_t outs[] = { ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10), ARG(11) };
    for (int i = 0; i < 7; i++) if (outs[i]) ST32(outs[i], 0);
    if (A3) ST32(A3, 0);
    if (ARG(12)) ST64(ARG(12), 0);
    RET(0);
}
HLE(RegQueryInfoKeyA) { hle_RegQueryInfoKeyW(c); }
HLE(RegSetKeyValueW) { RET(0); }
HLE(RegDeleteTreeW) { RET(0); }
HLE(RegNotifyChangeKeyValue) { RET(0); }

/* ---- advapi32: tracing, security descriptors, privileges ---- */
HLE(EventRegister) { if (A4) ST64(A4, 0x5E7E0001); RET(0); }
HLE(EventUnregister) { RET(0); }
HLE(EventSetInformation) { RET(0); }
HLE(EventActivityIdControl) { if (A2) memset(G2H(A2), 0, 16); RET(0); }
HLE(EventWrite) { RET(0); }
HLE(EventEnabled) { RET(0); }
HLE(LookupPrivilegeValueW) { if (A3) { ST32(A3, 20); ST32(A3 + 4, 0); } RET(1); }
HLE(SystemFunction036)   /* RtlGenRandom */
{
    static uint64_t s = 0x9E3779B97F4A7C15ull;
    s ^= host_ticks_ns();
    for (uint32_t i = 0; i < (uint32_t)A2; i++) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; ST8(A1 + i, (uint8_t)s); }
    RET(1);
}
HLE(BuildExplicitAccessWithNameW) { if (A1) memset(G2H(A1), 0, 48); RET(0); }
HLE(BuildSecurityDescriptorW) { if (ARG(8)) ST32(ARG(8), 0); if (ARG(9)) ST64(ARG(9), 0); RET(50); }
HLE(ConvertStringSecurityDescriptorToSecurityDescriptorW) { hle_set_error(50); RET(0); }

/* ---- appmodel / WinRT: not a packaged app, no WinRT ---- */
HLE(GetCurrentPackageFullName) { if (A1) ST32(A1, 0); RET(15700); }   /* APPMODEL_ERROR_NO_PACKAGE */
HLE(GetCurrentPackageId) { if (A1) ST32(A1, 0); RET(15700); }
HLE(GetCurrentPackageFamilyName) { if (A1) ST32(A1, 0); RET(15700); }
HLE(RoGetActivationFactory) { if (A3) ST64(A3, 0); RET(0x80040154); }
HLE(RoInitialize) { RET(0); }
HLE(RoUninitialize) { RET(0); }
/* HSTRINGs: +0 flags (1 = a reference to the caller's buffer), +4 length, +8 characters; NULL is the empty string */
HLE(WindowsCreateStringReference)
{
    uint64_t hdr = A3, out = A4;
    uint32_t n = (uint32_t)A2;
    if (!out || (n && !A1) || !hdr) { RET(0x80070057); return; }
    if (!n) { ST64(out, 0); RET(0); return; }
    ST32(hdr, 1); ST32(hdr + 4, n); ST64(hdr + 8, A1);
    ST64(out, hdr);
    RET(0);
}
HLE(WindowsCreateString)
{
    uint32_t n = (uint32_t)A2;
    if (!A3 || (n && !A1)) { RET(0x80070057); return; }
    if (!n) { ST64(A3, 0); RET(0); return; }
    uint64_t h = g_alloc(16 + 2ull * n + 2);
    ST32(h, 0); ST32(h + 4, n); ST64(h + 8, h + 16);
    memcpy(G2H(h + 16), G2H(A1), 2ull * n);
    ST16(h + 16 + 2ull * n, 0);
    ST64(A3, h);
    RET(0);
}
HLE(WindowsDeleteString) { if (A1 && !(LD32(A1) & 1)) g_free(A1); RET(0); }
HLE(WindowsGetStringLen) { RET(A1 ? LD32(A1 + 4) : 0); }
HLE(WindowsGetStringRawBuffer)
{
    static uint64_t empty;
    if (!empty) empty = g_calloc(2);
    if (A2) ST32(A2, A1 ? (uint32_t)LD32(A1 + 4) : 0);
    RET(A1 ? LD64(A1 + 8) : empty);
}
HLE(WindowsDuplicateString)
{
    if (!A2) { RET(0x80070057); return; }
    if (!A1) { ST64(A2, 0); RET(0); return; }
    uint32_t n = (uint32_t)LD32(A1 + 4);
    uint64_t h = g_alloc(16 + 2ull * n + 2);
    ST32(h, 0); ST32(h + 4, n); ST64(h + 8, h + 16);
    memcpy(G2H(h + 16), G2H(LD64(A1 + 8)), 2ull * n);
    ST16(h + 16 + 2ull * n, 0);
    ST64(A2, h);
    RET(0);
}
HLE(WindowsIsStringEmpty) { RET(!A1 || !LD32(A1 + 4)); }

/* ---- memory mapping ---- */
void hle_MapViewOfFile(CPU*);
HLE(MapViewOfFileEx) { hle_MapViewOfFile(c); }
HLE(OpenSemaphoreW) { hle_set_error(2); RET(0); }

/* ---- DPI ---- */
HLE(GetDpiForMonitor) { if (A3) ST32(A3, 96); if (A4) ST32(A4, 96); RET(0); }
HLE(GetProcessDpiAwareness) { if (A2) ST32(A2, 2); RET(0); }

/* ---- power, TPM, hardware enumeration: nothing to report ---- */
HLE(CallNtPowerInformation) { if (A4 && ARG(5)) memset(G2H(A4), 0, (size_t)(uint32_t)ARG(5)); RET(0xC00000BB); }
HLE(Tbsi_GetDeviceInfo) { RET(0x8028400F); }   /* TBS_E_TPM_NOT_FOUND */
HLE(CM_Get_Device_ID_List_SizeW) { if (A1) ST32(A1, 1); RET(0); }
HLE(CM_Get_Device_ID_ListW) { if (A2 && A3) ST16(A2, 0); RET(0); }
HLE(CM_Locate_DevNodeW) { if (A1) ST32(A1, 0); RET(13); }    /* CR_NO_SUCH_DEVNODE */
HLE(CM_Get_DevNode_PropertyW) { RET(13); }
HLE(CM_MapCrToWin32Err) { RET((uint32_t)A1 ? (uint32_t)A2 : 0); }
HLE(SetupDiGetClassDevsA) { RET((uint64_t)-1); }
HLE(SetupDiEnumDeviceInterfaces) { hle_set_error(259); RET(0); }
HLE(SetupDiGetDeviceInterfaceDetailW) { hle_set_error(259); RET(0); }
HLE(SetupDiGetDevicePropertyW) { hle_set_error(1168); RET(0); }
HLE(SetupDiGetDeviceRegistryPropertyW) { hle_set_error(13); RET(0); }
HLE(SetupDiOpenDeviceInterfaceW) { hle_set_error(13); RET(0); }
HLE(SetupGetInfDriverStoreLocationW) { hle_set_error(2); RET(0); }
HLE(HidD_GetHidGuid) { if (A1) { static const uint8_t g[16] = { 0xB2, 0x55, 0x1E, 0x4D, 0x6F, 0xF1, 0xCF, 0x11, 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 }; memcpy(G2H(A1), g, 16); } RET(0); }
HLE(HidD_GetAttributes) { RET(0); }
HLE(HidD_GetFeature) { RET(0); }
HLE(HidD_FlushQueue) { RET(0); }

/* ---- networking helpers ---- */
HLE(IcmpCreateFile) { RET((uint64_t)-1); }
HLE(IcmpCloseHandle) { RET(1); }
HLE(IcmpSendEcho) { hle_set_error(11050); RET(0); }
HLE(GetCurrentThreadCompartmentId) { RET(1); }
HLE(SetCurrentThreadCompartmentId) { RET(0); }
HLE(if_nametoindex) { RET(0); }
HLE(WSAIoctl) { hle_set_error(10045); RET((uint64_t)-1); }
HLE(WSARecv) { hle_set_error(10045); RET((uint64_t)-1); }
HLE(WSASend) { hle_set_error(10045); RET((uint64_t)-1); }
HLE(WSASendDisconnect) { hle_set_error(10045); RET((uint64_t)-1); }

/* ---- crypto: no certificate stores or keys ---- */
HLE(CertOpenSystemStoreW) { RET(0); }
HLE(CertOpenSystemStoreA) { RET(0); }
HLE(CertCreateContext) { RET(0); }
HLE(CertGetEnhancedKeyUsage) { hle_set_error(0x80092004); RET(0); }
HLE(CertGetIntendedKeyUsage) { RET(0); }
HLE(CertSetCertificateContextProperty) { RET(1); }
HLE(CertVerifyCertificateChainPolicy) { if (A4) ST32(A4 + 4, 0x800B0109); RET(1); }
HLE(CryptAcquireCertificatePrivateKey) { hle_set_error(0x8009200B); RET(0); }
HLE(PFXExportCertStoreEx) { RET(0); }
HLE(NCryptFreeObject) { RET(0); }
HLE(NCryptGetProperty) { RET(0x80090029); }

/* ---- user32: dialogs, menus, accelerators (none here), drawing helpers ---- */
HLE(LoadAcceleratorsW) { RET(0x7A000001); }
HLE(LoadAcceleratorsA) { RET(0x7A000001); }
HLE(DestroyAcceleratorTable) { RET(1); }
HLE(TranslateAcceleratorW) { RET(0); }
HLE(TranslateAcceleratorA) { RET(0); }
HLE(IsDialogMessageW) { RET(0); }
HLE(CreateDialogParamW) { hle_set_error(1813); RET(0); }   /* no dialog resources: callers report and carry on */
HLE(CreateDialogIndirectParamW) { hle_set_error(1813); RET(0); }
HLE(DialogBoxIndirectParamW) { hle_set_error(1813); RET((uint64_t)-1); }
HLE(GetDlgItemTextW) { if (A3 && A4) ST16(A3, 0); RET(0); }
HLE(CreatePopupMenu) { RET(0x7A000101); }
HLE(CreateMenu) { RET(0x7A000102); }
HLE(DestroyMenu) { RET(1); }
HLE(DeleteMenu) { RET(1); }
HLE(InsertMenuItemW) { RET(1); }
HLE(AppendMenuW) { RET(1); }
HLE(TrackPopupMenu) { RET(0); }
HLE(TrackPopupMenuEx) { RET(0); }
HLE(GetSysColor)
{
    static const uint32_t col[31] = { 0xC8C8C8, 0x000000, 0xD1B499, 0xDBCDBF, 0xF0F0F0, 0xFFFFFF, 0x646464, 0x000000, 0x000000, 0x000000,
                                      0xB4B4B4, 0xFCF7F4, 0xABABAB, 0xD77800, 0xFFFFFF, 0xF0F0F0, 0xA0A0A0, 0x6D6D6D, 0x000000, 0x000000,
                                      0xFFFFFF, 0x696969, 0xE3E3E3, 0x000000, 0xE1FFFF, 0x000000, 0xCC6600, 0xEAD1B9, 0xF2E4D7, 0xD77800, 0xF0F0F0 };
    RET((uint32_t)A1 < 31 ? col[(uint32_t)A1] : 0);
}
HLE(GetSysColorBrush) { RET(0x7A000200 + ((uint32_t)A1 & 0xFF)); }
HLE(PtInRect)
{
    int32_t x = (int32_t)(uint32_t)A2, y = (int32_t)(A2 >> 32);
    RET(A1 && x >= (int32_t)LD32(A1) && x < (int32_t)LD32(A1 + 8) && y >= (int32_t)LD32(A1 + 4) && y < (int32_t)LD32(A1 + 12));
}
HLE(IntersectRect)
{
    int32_t l = (int32_t)LD32(A2) > (int32_t)LD32(A3) ? (int32_t)LD32(A2) : (int32_t)LD32(A3);
    int32_t t = (int32_t)LD32(A2 + 4) > (int32_t)LD32(A3 + 4) ? (int32_t)LD32(A2 + 4) : (int32_t)LD32(A3 + 4);
    int32_t r = (int32_t)LD32(A2 + 8) < (int32_t)LD32(A3 + 8) ? (int32_t)LD32(A2 + 8) : (int32_t)LD32(A3 + 8);
    int32_t b = (int32_t)LD32(A2 + 12) < (int32_t)LD32(A3 + 12) ? (int32_t)LD32(A2 + 12) : (int32_t)LD32(A3 + 12);
    if (l >= r || t >= b) { memset(G2H(A1), 0, 16); RET(0); return; }
    ST32(A1, (uint32_t)l); ST32(A1 + 4, (uint32_t)t); ST32(A1 + 8, (uint32_t)r); ST32(A1 + 12, (uint32_t)b);
    RET(1);
}
/* one window, no child windows; client and screen coordinates coincide */
HLE(MapWindowPoints) { RET(0); }
HLE(EnumChildWindows) { RET(0); }
HLE(FindWindowExA) { RET(0); }
HLE(FindWindowExW) { RET(0); }
HLE(FindWindowA) { RET(0); }
HLE(FindWindowW) { RET(0); }
HLE(DrawTextW) { RET(16); }
HLE(DrawTextExW) { RET(16); }
HLE(DrawIconEx) { RET(1); }
HLE(FillRect) { RET(1); }
HLE(PrintWindow) { RET(0); }
HLE(GetDCEx) { void hle_GetDC(CPU*); hle_GetDC(c); }
HLE(GetKeyboardLayoutList) { if (A1 && A2) ST64(A2, 0x04090409); RET(1); }
HLE(ActivateKeyboardLayout) { RET(0x04090409); }
HLE(AddClipboardFormatListener) { RET(1); }
HLE(RemoveClipboardFormatListener) { RET(1); }
static uint32_t next_msg = 0xC000;
HLE(RegisterWindowMessageW) { RET(next_msg++); }
HLE(RegisterWindowMessageA) { RET(next_msg++); }
HLE(GetPointerType) { if (A2) ST32(A2, 4); RET(1); }   /* PT_MOUSE */
HLE(GetPointerTouchInfo) { hle_set_error(87); RET(0); }
HLE(GetPointerPenInfo) { hle_set_error(87); RET(0); }
HLE(SetWindowTheme) { RET(0); }
/* MONITORINFOEXA: cbSize 72, the device name in ANSI */
HLE(GetMonitorInfoA)
{
    uint64_t mi = A2;
    uint32_t sz = mi ? (uint32_t)LD32(mi) : 0;
    if (sz >= 72) ST32(mi, 40);
    hle_GetMonitorInfoW(c);
    if (sz >= 72) { ST32(mi, sz); memcpy(G2H(mi + 40), "\\\\.\\DISPLAY1", 13); }
}
/* DEVMODEA: names of 32 chars where DEVMODEW has 32 wide chars */
HLE(EnumDisplaySettingsA)
{
    uint64_t dma = A3, dmw = g_calloc(220);
    ST16(dmw + 68, 220);
    R(1) = 0; R(8) = dmw;
    hle_EnumDisplaySettingsW(c);
    if (R(0) && dma) {
        memcpy(G2H(dma), "WebGPU", 7);
        memcpy(G2H(dma + 32), G2H(dmw + 64), 70);
        ST16(dma + 36, 156);
        memcpy(G2H(dma + 134), G2H(dmw + 198), 22);
    }
    g_free(dmw);
}

/* ---- gdi32: drawing goes nowhere (games draw with the GPU) ---- */
HLE(CreateCompatibleBitmap) { RET(0x7A000301); }
HLE(CreateFontW) { RET(0x7A000302); }
HLE(CreatePen) { RET(0x7A000303); }
HLE(CreateSolidBrush) { RET(0x7A000304); }
HLE(ExtTextOutW) { RET(1); }
HLE(GetDIBits) { RET(0); }
HLE(GetPixel) { RET(0); }
HLE(GetTextMetricsW) { if (A2) { memset(G2H(A2), 0, 60); ST32(A2, 16); ST32(A2 + 4, 13); ST32(A2 + 8, 3); ST32(A2 + 20, 7); ST32(A2 + 24, 14); ST32(A2 + 28, 400); } RET(1); }
HLE(LineTo) { RET(1); }
HLE(MoveToEx) { if (A4) { ST32(A4, 0); ST32(A4 + 4, 0); } RET(1); }
HLE(Polygon) { RET(1); }
HLE(Rectangle) { RET(1); }
HLE(RoundRect) { RET(1); }
HLE(AddFontMemResourceEx) { if (A4) ST32(A4, 1); RET(0x7A000305); }
HLE(RemoveFontMemResourceEx) { RET(1); }

/* ---- gdiplus: image decoding isn't offered ---- */
HLE(GdiplusStartup) { if (A1) ST64(A1, 1); RET(0); }
HLE(GdiplusShutdown) { RET(0); }
HLE(GdipAlloc) { RET(g_alloc(A1 ? A1 : 1)); }
HLE(GdipFree) { if (A1) g_free(A1); RET(0); }
HLE(GdipLoadImageFromStream) { if (A2) ST64(A2, 0); RET(6); }    /* NotImplemented */
HLE(GdipLoadImageFromStreamICM) { if (A2) ST64(A2, 0); RET(6); }
HLE(GdipCloneImage) { if (A2) ST64(A2, 0); RET(6); }
HLE(GdipDisposeImage) { RET(0); }
HLE(GdipCreateFromHDC) { if (A2) ST64(A2, 0); RET(6); }
HLE(GdipDeleteGraphics) { RET(0); }
HLE(GdipDrawImageRectI) { RET(6); }

/* ---- audio compression manager: no codecs (games decode their own audio) ---- */
HLE(acmFormatSuggest) { RET(512); }   /* ACMERR_NOTPOSSIBLE */
HLE(acmStreamOpen) { if (A1) ST64(A1, 0); RET(512); }
HLE(acmStreamSize) { RET(512); }
HLE(acmStreamPrepareHeader) { RET(512); }
HLE(acmStreamUnprepareHeader) { RET(512); }
HLE(acmStreamConvert) { RET(512); }
HLE(acmStreamClose) { RET(0); }
HLE(timeGetDevCaps) { if (A1 && (uint32_t)A2 >= 8) { ST32(A1, 1); ST32(A1 + 4, 1000000); } RET(0); }

/* ---- DirectSound capture and enumeration: no microphones; one output device for enumeration ---- */
HLE(DirectSoundCaptureCreate) { if (A2) ST64(A2, 0); RET(0x88780078); }   /* DSERR_NODRIVER */
HLE(DirectSoundCaptureEnumerateW) { RET(0); }
HLE(DirectSoundEnumerateW) { RET(0); }
HLE(DirectSoundEnumerateA) { RET(0); }

/* ---- other graphics APIs games probe for ---- */
HLE(Direct3DCreate9) { RET(0); }
HLE(Direct3DCreate9Ex) { if (A2) ST64(A2, 0); RET(0x8876086A); }   /* D3DERR_NOTAVAILABLE */
HLE(DXVA2CreateDirect3DDeviceManager9) { if (A1) ST32(A1, 0); if (A2) ST64(A2, 0); RET(0x80004001); }
HLE(D3D12SerializeRootSignature) { if (A3) ST64(A3, 0); if (A4) ST64(A4, 0); RET(0x80004001); }
HLE(D3D12SerializeVersionedRootSignature) { if (A2) ST64(A2, 0); if (A3) ST64(A3, 0); RET(0x80004001); }
HLE(D3DDisassemble) { if (ARG(5)) ST64(ARG(5), 0); RET(0x80004001); }

/* ---- COM helpers ---- */
static int hexv(int ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
HLE(CLSIDFromString)
{
    char s[64];
    read_w(A1, s, sizeof s);
    /* {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx} */
    if (strlen(s) != 38 || s[0] != '{') { RET(0x800401F3); return; }
    uint8_t g[16]; int k = 0; uint8_t raw[16]; int nib = -1;
    for (int i = 1; i < 37 && k < 16; i++) {
        if (s[i] == '-') continue;
        int v = hexv(s[i]);
        if (v < 0) { RET(0x800401F3); return; }
        if (nib < 0) nib = v; else { raw[k++] = (uint8_t)(nib << 4 | v); nib = -1; }
    }
    if (k != 16) { RET(0x800401F3); return; }
    g[0] = raw[3]; g[1] = raw[2]; g[2] = raw[1]; g[3] = raw[0]; g[4] = raw[5]; g[5] = raw[4]; g[6] = raw[7]; g[7] = raw[6];
    memcpy(g + 8, raw + 8, 8);
    memcpy(G2H(A2), g, 16);
    RET(0);
}
HLE(CoCreateFreeThreadedMarshaler) { if (A2) ST64(A2, 0); RET(0x80004001); }
HLE(PropVariantCopy) { memcpy(G2H(A1), G2H(A2), 24); RET(0); }
HLE(PSStringFromPropertyKey) { if (A2 && A3) ST16(A2, 0); RET(0x80004001); }

/* ---- dbghelp: PE header walking works; symbols and stack walks aren't available ---- */
HLE(ImageNtHeader) { RET(A1 && LD16(A1) == 0x5A4D ? A1 + LD32(A1 + 0x3C) : 0); }
/* the data directory entry's address and size */
static uint64_t dir_entry(uint64_t base, uint32_t idx, uint64_t sizep)
{
    if (!base || LD16(base) != 0x5A4D) return 0;
    uint64_t nt = base + LD32(base + 0x3C), opt = nt + 24;
    uint32_t n = (uint32_t)LD32(opt + 108);
    if (idx >= n) return 0;
    uint32_t rva = (uint32_t)LD32(opt + 112 + 8 * idx), sz = (uint32_t)LD32(opt + 116 + 8 * idx);
    if (sizep) ST32(sizep, sz);
    return rva ? base + rva : 0;
}
HLE(ImageDirectoryEntryToData) { RET(dir_entry(A1, (uint32_t)A3, A4)); }
HLE(ImageDirectoryEntryToDataEx) { if (ARG(5)) ST64(ARG(5), 0); RET(dir_entry(A1, (uint32_t)A3, A4)); }
HLE(ImageRvaToVa) { RET(A2 + (uint32_t)A3); }
HLE(SymFromAddr) { hle_set_error(487); RET(0); }
HLE(SymFromAddrW) { hle_set_error(487); RET(0); }
HLE(SymGetLineFromAddrW64) { hle_set_error(487); RET(0); }
HLE(SymLoadModuleExW) { RET(0); }
HLE(SymLoadModuleEx) { RET(0); }
HLE(SymLoadModule64) { RET(0); }
HLE(SymUnloadModule64) { RET(1); }
HLE(SymSetSearchPathW) { RET(1); }
HLE(SymGetSearchPathW) { if (A2 && A3) ST16(A2, 0); RET(1); }
HLE(SymEnumSymbolsW) { RET(1); }
HLE(SymFromNameW) { RET(0); }
HLE(EnumerateLoadedModulesW64) { RET(1); }
HLE(EnumerateLoadedModules64) { RET(1); }
HLE(StackWalkEx) { RET(0); }
HLE(SymGetTypeInfo) { RET(0); }
HLE(SymSetContext) { RET(1); }
HLE(UnDecorateSymbolName) { char s[512]; read_a(A1, s, sizeof s); uint32_t n = (uint32_t)strlen(s); if (n + 1 > (uint32_t)A3) n = (uint32_t)A3 ? (uint32_t)A3 - 1 : 0; if (A3) { memcpy(G2H(A2), s, n); ST8(A2 + n, 0); } RET(n); }
HLE(UnDecorateSymbolNameW) { size_t n = wlen(A1); if (n + 1 > (uint32_t)A3) n = (uint32_t)A3 ? (uint32_t)A3 - 1 : 0; if (A3) { memcpy(G2H(A2), G2H(A1), 2 * n); ST16(A2 + 2 * n, 0); } RET(n); }

/* ---- newer entry points games look up with GetProcAddress ---- */
void hle_FlsGetValue(CPU*);
void hle_GetTempPathW(CPU*);
HLE(FlsGetValue2) { hle_FlsGetValue(c); }
HLE(GetTempPath2W) { hle_GetTempPathW(c); }
HLE(GetTempPath2A) { uint64_t w = g_alloc(1040); uint64_t cap = A1, out = A2; R(1) = 520; R(2) = w; hle_GetTempPathW(c); uint64_t n = R(0); if (n && n < cap) w2a(w, out, cap); g_free(w); RET(n); }
/* AppPolicy*: a desktop process (0 = the classic behaviour) */
HLE(AppPolicyGetThreadInitializationType) { if (A2) ST32(A2, 0); RET(0); }
HLE(AppPolicyGetProcessTerminationMethod) { if (A2) ST32(A2, 0); RET(0); }
HLE(AppPolicyGetWindowingModel) { if (A2) ST32(A2, 1); RET(0); }   /* ClassicDesktop */
HLE(AppPolicyGetShowDeveloperDiagnostic) { if (A2) ST32(A2, 1); RET(0); }
/* version checks pass: the reported Windows (10) is new enough for whatever is asked */
HLE(RtlVerifyVersionInfo) { RET(0); }
HLE(RtlGetDeviceFamilyInfoEnum) { if (A1) ST64(A1, 0x000A000047BA0000ull); if (A2) ST32(A2, 3); if (A3) ST32(A3, 1); RET(0); }   /* Windows.Desktop */
HLE(AddDllDirectory) { RET(0x7A000401); }
HLE(RemoveDllDirectory) { RET(1); }
HLE(SetDefaultDllDirectories) { RET(1); }

/* ---- input helpers ---- */
void hle_MapVirtualKeyW(CPU*);
void hle_ToUnicode(CPU*);
HLE(MapVirtualKeyExW) { hle_MapVirtualKeyW(c); }
HLE(MapVirtualKeyExA) { hle_MapVirtualKeyW(c); }
HLE(ToUnicodeEx) { hle_ToUnicode(c); }
HLE(XInputGetKeystroke) { RET(4306); }   /* ERROR_EMPTY: no queued pad keystrokes */
/* winmm joysticks: none plugged in (pads come through XInput) */
HLE(joyGetNumDevs) { RET(16); }
HLE(joyGetPosEx) { RET(167); }   /* JOYERR_UNPLUGGED */
HLE(joyGetPos) { RET(167); }
HLE(joyGetDevCapsW) { RET(167); }
HLE(joyGetDevCapsA) { RET(167); }
HLE(joySetCapture) { RET(167); }
HLE(joyReleaseCapture) { RET(0); }
HLE(SetHandleCount) { RET(A1); }   /* obsolete: the count asked for */
