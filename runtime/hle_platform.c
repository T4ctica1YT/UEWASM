/* Windows calls that Unreal Engine 4/5's Windows layer and the middleware games ship with it (Epic Online Services,
   ONNX Runtime, crash reporting) make directly or look up by name: DPI awareness, job objects, cycle counters, timer
   resolution, process queries, the credential store, SIDs, multimedia timers, network interface tables. Each answers
   as an idle PC would, or fails the way Windows does when the feature is absent. */
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hle_set_error(uint32_t e);
uint32_t wait_many(CPU* c, uint32_t n, const uint64_t* hs, int all, uint32_t ms);
void rt_backtrace(CPU* c, const char* why);
void hle_CreateEventW(CPU*);
void hle_SetEvent(CPU*);
void hle_PulseEvent(CPU*);
void hle_GetSystemMetrics(CPU*);
void hle_AdjustWindowRectEx(CPU*);
void hle_VirtualQuery(CPU*);
void hle_GetTimeZoneInformation(CPU*);
void hle_GetOverlappedResult(CPU*);

#define WAIT_TIMEOUT 0x102
#define E_NOTIMPL 0x80004001u

static uint64_t alloc_w(const char* s)
{
    uint64_t w = g_alloc(2 * strlen(s) + 2);
    utf8_to_wstr(s, w, strlen(s) + 1);
    return w;
}

static uint64_t alloc_a(const char* s)
{
    uint64_t a = g_alloc(strlen(s) + 1);
    memcpy(G2H(a), s, strlen(s) + 1);
    return a;
}

/* ---- waits that also wake for window messages ---- */

/* messages don't signal these waits: they come back at least every 50 ms saying input may be waiting (WAIT_OBJECT_0
   + n), which callers answer by peeking at messages and waiting again, so a thread never spins or sleeps past input */
static uint32_t msg_wait(CPU* c, uint32_t n, uint64_t hp, int all, uint32_t ms)
{
    uint64_t hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = LDP(hp + (uint64_t)GP * i);
    uint32_t cap = ms < 50 ? ms : 50;
    uint32_t r = WAIT_TIMEOUT;
    if (n) r = wait_many(c, n, hs, all, cap);
    else sched_sleep(c, cap);
    return r == WAIT_TIMEOUT && ms > cap ? n : r;
}

HLE(MsgWaitForMultipleObjects) { RET(msg_wait(c, (uint32_t)A1, A2, (int)A3, (uint32_t)A4)); }
HLE(MsgWaitForMultipleObjectsEx) { RET(msg_wait(c, (uint32_t)A1, A2, (ARG(5) & 1) != 0, (uint32_t)A3)); }

HLE(CoWaitForMultipleHandles)
{
    uint32_t n = (uint32_t)A3;
    uint64_t hs[64];
    if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) hs[i] = LDP(A4 + (uint64_t)GP * i);
    uint32_t r = n ? wait_many(c, n, hs, (A1 & 1) != 0, (uint32_t)A2) : WAIT_TIMEOUT;
    if (r == WAIT_TIMEOUT) { RET(0x80010115); return; }   /* RPC_S_CALLPENDING */
    if (ARG(5)) ST32(ARG(5), r);
    RET(0);
}

/* ---- ntdll ---- */

HLE(NtDelayExecution)
{
    int64_t t = A2 ? (int64_t)LD64(A2) : 0;
    sched_sleep(c, t < 0 ? (uint32_t)((-t + 9999) / 10000) : 0);
    RET(0);
}

/* 100 ns units: Windows' coarsest 15.6 ms, finest 0.5 ms, and the 1 ms games ask for */
HLE(NtQueryTimerResolution) { if (A1) ST32(A1, 156250); if (A2) ST32(A2, 5000); if (A3) ST32(A3, 10000); RET(0); }
HLE(NtSetTimerResolution) { if (A3) ST32(A3, A1 < 5000 ? 5000 : (uint32_t)A1); RET(0); }
HLE(RtlGetNtVersionNumbers) { if (A1) ST32(A1, 10); if (A2) ST32(A2, 0); if (A3) ST32(A3, 0xF0000000u | 19045); RET(0); }

HLE(NtQueryInformationProcess)
{
    uint32_t cls = (uint32_t)A2;
    uint64_t buf = A3, len = A4, retlen = ARG(5);
    uint32_t need;
    switch (cls) {
    case 0:   /* ProcessBasicInformation */
        need = 6 * GP;
        if (len < need) break;
        memset(G2H(buf), 0, need);
        STP(buf + GP, g_peb);
        STP(buf + 2 * GP, 1);
        STP(buf + 3 * GP, 8);
        STP(buf + 4 * GP, 0x1000);
        if (retlen) ST32(retlen, need);
        RET(0);
        return;
    case 7:   /* ProcessDebugPort: no debugger */
        need = GP;
        if (len < need) break;
        STP(buf, 0);
        if (retlen) ST32(retlen, need);
        RET(0);
        return;
    case 0x1e:   /* ProcessDebugObjectHandle */
        RET(0xC0000353);   /* STATUS_PORT_NOT_SET */
        return;
    case 0x1f:   /* ProcessDebugFlags: 1 = not being debugged */
        need = 4;
        if (len < need) break;
        ST32(buf, 1);
        if (retlen) ST32(retlen, need);
        RET(0);
        return;
    default:
        RET(0xC0000003);   /* STATUS_INVALID_INFO_CLASS */
        return;
    }
    if (retlen) ST32(retlen, need);
    RET(0xC0000004);   /* STATUS_INFO_LENGTH_MISMATCH */
}

HLE(NtQueryVirtualMemory)
{
    uint64_t addr = A2, cls = A3, buf = A4, len = ARG(5), retlen = ARG(6);
    if (cls != 0) { RET(0xC0000003); return; }
    R(1) = addr;
    R(2) = buf;
    R(8) = len;
    hle_VirtualQuery(c);
    uint64_t n = R(0);
    if (retlen) STP(retlen, n);
    RET(n ? 0 : 0xC000000D);   /* STATUS_INVALID_PARAMETER */
}

/* ---- DPI: the page is one per-monitor-aware window at 96 DPI (the browser scales it) ---- */

static uint64_t dpi_ctx = (uint64_t)-4;   /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */

HLE(SetThreadDpiAwarenessContext) { uint64_t old = dpi_ctx; if (A1) dpi_ctx = A1; RET(RT_G32 ? (uint32_t)old : old); }
HLE(GetThreadDpiAwarenessContext) { RET(RT_G32 ? (uint32_t)dpi_ctx : dpi_ctx); }
HLE(GetAwarenessFromDpiAwarenessContext)
{
    int32_t v = (int32_t)A1;
    RET(v == -1 || v == -5 ? 0 : v == -2 ? 1 : v == -3 || v == -4 ? 2 : (uint64_t)-1);
}
HLE(AreDpiAwarenessContextsEqual) { RET((uint32_t)A1 == (uint32_t)A2); }
HLE(GetDpiForSystem) { RET(96); }
HLE(EnableNonClientDpiScaling) { RET(1); }
HLE(GetSystemMetricsForDpi) { hle_GetSystemMetrics(c); }
HLE(AdjustWindowRectExForDpi) { hle_AdjustWindowRectEx(c); }
HLE(GetScaleFactorForMonitor) { if (A2) ST32(A2, 100); RET(0); }

/* ---- windowing extras ---- */

HLE(ChangeWindowMessageFilterEx) { RET(1); }
HLE(SetWindowCompositionAttribute) { RET(1); }
HLE(UpdateLayeredWindow) { RET(1); }
HLE(IsImmersiveProcess) { hle_set_error(0); RET(0); }
HLE(GetRegisteredRawInputDevices) { if (A2) ST32(A2, 0); RET(0); }
HLE(RegisterPowerSettingNotification) { RET(0x5EED0001); }
HLE(UnregisterPowerSettingNotification) { RET(1); }
HLE(GetPointerInfo) { hle_set_error(87); RET(0); }
HLE(DwmEnableMMCSS) { RET(0); }
HLE(DXGIDeclareAdapterRemovalSupport) { RET(0); }
HLE(ImmGetOpenStatus) { RET(0); }
HLE(ImmSetOpenStatus) { RET(1); }
HLE(ImmGetDefaultIMEWnd) { RET(0); }
HLE(ImmDisableIME) { RET(1); }

/* no GameInput runtime: the engine's GameInput plugin sees it missing and leaves input to XInput and raw input */
HLE(GameInputCreate) { if (A1) STP(A1, 0); RET(E_NOTIMPL); }

/* ---- processes, jobs, scheduling ---- */

/* a job object is an event nobody signals: children can't be started here, so there is nothing to group */
HLE(CreateJobObjectW) { R(1) = 0; R(2) = 1; R(8) = 0; R(9) = 0; hle_CreateEventW(c); }
HLE(CreateJobObjectA) { hle_CreateJobObjectW(c); }
HLE(SetInformationJobObject) { RET(1); }
HLE(AssignProcessToJobObject) { RET(1); }

HLE(SetThreadSelectedCpuSets) { RET(1); }
HLE(SetProcessInformation) { RET(1); }
HLE(SetThreadInformation) { RET(1); }
HLE(SetProcessPriorityBoost) { RET(1); }
HLE(SetProcessMitigationPolicy) { RET(1); }
HLE(GetProcessMitigationPolicy) { if (A3 && A4) memset(G2H(A3), 0, (size_t)A4); RET(1); }
HLE(GetActiveProcessorGroupCount) { RET(1); }
HLE(GetProcessGroupAffinity)
{
    if (!A2) { hle_set_error(87); RET(0); return; }
    uint16_t cap = (uint16_t)LD32(A2);
    ST16(A2, 1);
    if (cap < 1 || !A3) { hle_set_error(122); RET(0); return; }
    ST16(A3, 0);
    RET(1);
}

/* cycle counts at a nominal 3 GHz */
HLE(QueryThreadCycleTime) { if (A2) ST64(A2, host_ticks_ns() * 3); RET(1); }
HLE(QueryProcessCycleTime) { if (A2) ST64(A2, host_ticks_ns() * 3); RET(1); }
HLE(GetPhysicallyInstalledSystemMemory) { if (!A1) { hle_set_error(87); RET(0); return; } ST64(A1, (uint64_t)MEM_SIZE / 1024); RET(1); }

HLE(PrefetchVirtualMemory) { RET(1); }
HLE(DiscardVirtualMemory) { RET(0); }
HLE(OfferVirtualMemory) { RET(0); }
HLE(ReclaimVirtualMemory) { RET(0); }

static void module_information(CPU* c)
{
    const ModuleInfo* m = rt_module_at(A2 ? A2 : g_exe_base);
    if (!m || !A3 || A4 < 3ull * GP) { hle_set_error(87); RET(0); return; }
    STP(A3, m->base);
    ST32(A3 + GP, (uint32_t)m->size);
    STP(A3 + 2 * GP, m->entry_rva ? m->base + m->entry_rva : 0);
    RET(1);
}
HLE(GetModuleInformation) { module_information(c); }
HLE(K32GetModuleInformation) { module_information(c); }
HLE(K32EnumProcesses) { if (A1 && A2 >= 4) ST32(A1, 0x1000); if (A3) ST32(A3, 4); RET(1); }

HLE(GetOverlappedResultEx) { R(9) = A4 != 0; hle_GetOverlappedResult(c); }
HLE(GetTimeZoneInformationForYear) { if (!A3) { hle_set_error(87); RET(0); return; } R(1) = A3; hle_GetTimeZoneInformation(c); RET(1); }

HLE(RaiseFailFastException)
{
    host_log("[exit] RaiseFailFastException (code %x)", A1 ? (unsigned)LD32(A1) : 0u);
    rt_backtrace(c, "fail fast");
    rt_exit(0xC0000409);
}

HLE(AddVectoredContinueHandler) { RET(A2 ? A2 : 1); }
HLE(RemoveVectoredContinueHandler) { RET(1); }
/* x87 and SSE state only: no AVX register state to save in crash contexts */
HLE(GetEnabledXStateFeatures) { RET(3); }
HLE(GetXStateFeaturesMask) { if (A2) ST64(A2, 3); RET(1); }

/* ---- AVRT: audio threads get their "Pro Audio" registration (nothing changes, but callers get a handle) ---- */

HLE(AvSetMmThreadCharacteristicsW) { if (A2) ST32(A2, 1); RET(0xA5A50001); }

/* ---- COM ---- */

HLE(CoGetApartmentType) { if (A1) ST32(A1, 1); if (A2) ST32(A2, 0); RET(0); }   /* APTTYPE_MTA */
HLE(CoInitializeSecurity) { RET(0); }

/* ---- credential store (Epic Online Services keeps sign-in tokens here): kept for the page's lifetime ---- */

typedef struct {
    char target[256], user[256];
    uint32_t type, n;
    uint8_t* blob;
} Cred;
static Cred creds[32];
static int ncreds;

/* CREDENTIALW fields, by pointer size */
static uint32_t al(uint32_t v) { return (v + GP - 1) & ~(GP - 1); }
#define C_TARGET 8u
#define C_BLOBSIZE (16u + 2 * GP)
#define C_BLOB al(C_BLOBSIZE + 4)
#define C_PERSIST (C_BLOB + GP)
#define C_USER (al(C_PERSIST + 8) + 2 * GP)
#define C_SIZE al(C_USER + GP)

static Cred* cred_find(uint64_t target, uint32_t type)
{
    char t[256];
    if (!target) return NULL;
    wstr_to_utf8(target, t, sizeof t);
    for (int i = 0; i < ncreds; i++)
        if (creds[i].type == type && !strcmp(creds[i].target, t)) return &creds[i];
    return NULL;
}

HLE(CredWriteW)
{
    uint64_t p = A1;
    if (!p || !LDP(p + C_TARGET)) { hle_set_error(87); RET(0); return; }
    uint32_t type = LD32(p + 4);
    Cred* k = cred_find(LDP(p + C_TARGET), type);
    if (!k) {
        if (ncreds == (int)(sizeof creds / sizeof *creds)) { hle_set_error(8); RET(0); return; }
        k = &creds[ncreds++];
        memset(k, 0, sizeof *k);
        wstr_to_utf8(LDP(p + C_TARGET), k->target, sizeof k->target);
        k->type = type;
    }
    free(k->blob);
    k->n = LD32(p + C_BLOBSIZE);
    k->blob = k->n ? malloc(k->n) : NULL;
    if (k->n) memcpy(k->blob, G2H(LDP(p + C_BLOB)), k->n);
    k->user[0] = 0;
    if (LDP(p + C_USER)) wstr_to_utf8(LDP(p + C_USER), k->user, sizeof k->user);
    RET(1);
}

HLE(CredReadW)
{
    Cred* k = cred_find(A1, (uint32_t)A2);
    if (!k || !A4) { hle_set_error(1168); RET(0); return; }   /* ERROR_NOT_FOUND: nothing saved */
    size_t tn = strlen(k->target) + 1, un = strlen(k->user) + 1;
    uint64_t p = g_calloc(C_SIZE + 2 * (tn + un) + k->n + 8);
    uint64_t t = p + C_SIZE, u = t + 2 * tn, b = (u + 2 * un + 7) & ~7ull;
    utf8_to_wstr(k->target, t, tn);
    utf8_to_wstr(k->user, u, un);
    if (k->n) memcpy(G2H(b), k->blob, k->n);
    ST32(p + 4, k->type);
    STP(p + C_TARGET, t);
    ST32(p + C_BLOBSIZE, k->n);
    STP(p + C_BLOB, k->n ? b : 0);
    ST32(p + C_PERSIST, 2);   /* CRED_PERSIST_LOCAL_MACHINE */
    STP(p + C_USER, u);
    STP(A4, p);
    RET(1);
}

HLE(CredDeleteW)
{
    Cred* k = cred_find(A1, (uint32_t)A2);
    if (!k) { hle_set_error(1168); RET(0); return; }
    free(k->blob);
    *k = creds[--ncreds];
    RET(1);
}

HLE(CredFree) { if (A1) g_free(A1); RET(0); }

/* ---- SIDs and accounts ---- */

static int sid_string(uint64_t sid, char* out, size_t cap)
{
    if (!sid || LD8(sid) != 1) return 0;
    uint32_t n = LD8(sid + 1);
    uint64_t auth = 0;
    for (int i = 0; i < 6; i++) auth = auth << 8 | LD8(sid + 2 + i);
    int k = snprintf(out, cap, "S-1-%llu", (unsigned long long)auth);
    for (uint32_t i = 0; i < n && i < 15 && k > 0 && (size_t)k < cap; i++) k += snprintf(out + k, cap - k, "-%u", (unsigned)LD32(sid + 8 + 4ull * i));
    return 1;
}

HLE(ConvertSidToStringSidW)
{
    char s[256];
    if (!A2 || !sid_string(A1, s, sizeof s)) { hle_set_error(1337); RET(0); return; }   /* ERROR_INVALID_SID */
    STP(A2, alloc_w(s));
    RET(1);
}

HLE(ConvertSidToStringSidA)
{
    char s[256];
    if (!A2 || !sid_string(A1, s, sizeof s)) { hle_set_error(1337); RET(0); return; }
    STP(A2, alloc_a(s));
    RET(1);
}

/* the one local account: "Player" on machine "UEWASM" */
HLE(LookupAccountSidW)
{
    static const char *name = "Player", *dom = "UEWASM";
    uint64_t pn = A4, dbuf = ARG(5), pd = ARG(6), use = ARG(7);
    uint32_t nn = pn ? LD32(pn) : 0, nd = pd ? LD32(pd) : 0;
    int small = nn < strlen(name) + 1 || nd < strlen(dom) + 1;
    if (pn) ST32(pn, (uint32_t)(small ? strlen(name) + 1 : strlen(name)));
    if (pd) ST32(pd, (uint32_t)(small ? strlen(dom) + 1 : strlen(dom)));
    if (small) { hle_set_error(122); RET(0); return; }
    if (A3) utf8_to_wstr(name, A3, nn);
    if (dbuf) utf8_to_wstr(dom, dbuf, nd);
    if (use) ST32(use, 1);   /* SidTypeUser */
    RET(1);
}

HLE(RegOpenCurrentUser) { if (A2) STP(A2, 0x80000001u); RET(0); }   /* HKEY_CURRENT_USER */
HLE(EventWriteEx) { RET(0); }

/* ---- network interface tables: no interfaces to list (the browser hides them) ---- */

HLE(GetIfTable2) { if (!A1) { RET(87); return; } STP(A1, g_calloc(16)); RET(0); }
HLE(GetIfTable2Ex) { if (!A2) { RET(87); return; } STP(A2, g_calloc(16)); RET(0); }
HLE(GetIpForwardTable2) { if (!A2) { RET(87); return; } STP(A2, g_calloc(16)); RET(0); }
HLE(FreeMibTable) { if (A1) g_free(A1); RET(0); }
HLE(GetIfEntry2) { RET(2); }   /* ERROR_FILE_NOT_FOUND */
HLE(NotifyIpInterfaceChange) { if (ARG(5)) STP(ARG(5), 0x5EED0002); RET(0); }
HLE(CancelMibChangeNotify2) { RET(0); }

/* ---- multimedia timers: a guest thread per timer calls back (or sets an event) every period ---- */

typedef struct {
    uint64_t cb, user;
    uint32_t period, flags;
    volatile int alive;
} MmTimer;
static MmTimer mm_timers[16];

static void mm_timer_loop(CPU* c)
{
    uint32_t id = (uint32_t)A1;
    MmTimer* t = &mm_timers[id - 1];
    while (t->alive) {
        sched_sleep(c, t->period ? t->period : 1);
        if (!t->alive) break;
        if (t->flags & 0x30) {   /* TIME_CALLBACK_EVENT_SET / _PULSE: the callback is an event */
            R(1) = t->cb;
            if (t->flags & 0x20) hle_PulseEvent(c);
            else hle_SetEvent(c);
        } else {
            uint64_t args[5] = { id, 0, t->user, 0, 0 };
            rt_invoke(c, t->cb, 5, args);
        }
        if (!(t->flags & 1)) t->alive = 0;   /* TIME_ONESHOT */
    }
    RET(0);
}

HLE(timeSetEvent)
{
    static uint64_t entry;
    uint32_t id = 0;
    for (uint32_t i = 0; i < sizeof mm_timers / sizeof *mm_timers; i++)
        if (!mm_timers[i].alive) { id = i + 1; break; }
    if (!id || !A3) { RET(0); return; }
    MmTimer* t = &mm_timers[id - 1];
    t->cb = A3;
    t->user = A4;
    t->period = (uint32_t)A1;
    t->flags = (uint32_t)ARG(5);
    t->alive = 1;
    if (!entry) entry = rt_dyn_thunk_n(mm_timer_loop, "uew_mm_timer", 1);
    if (!sched_create(entry, id, 0)) { t->alive = 0; RET(0); return; }
    RET(id);
}

HLE(timeKillEvent)
{
    uint32_t id = (uint32_t)A1;
    if (!id || id > sizeof mm_timers / sizeof *mm_timers || !mm_timers[id - 1].alive) { RET(97); return; }   /* TIMERR_NOCANDO */
    mm_timers[id - 1].alive = 0;
    RET(0);
}

/* ---- cfgmgr32: no device interfaces to list ---- */

HLE(CM_Get_Device_Interface_List_SizeW) { if (A1) ST32(A1, 1); RET(0); }
HLE(CM_Get_Device_Interface_ListW) { if (A3 && A4) ST16(A3, 0); RET(0); }
HLE(CM_Get_Parent) { RET(0x0D); }   /* CR_NO_SUCH_DEVNODE */

/* ---- visual styles off: classic window drawing, which the engine never uses anyway ---- */

HLE(IsThemeActive) { RET(0); }
HLE(OpenThemeData) { RET(0); }
HLE(CloseThemeData) { RET(0); }

HLE(FreeContextBuffer) { if (A1) g_free(A1); RET(0); }
HLE(CryptMsgClose) { RET(1); }

