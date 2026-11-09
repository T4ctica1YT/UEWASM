#include "host.h"
#include "sched.h"
#include <string.h>

#include <stdio.h>

HLE(GetCurrentProcess) { RET((uint64_t)-1); }
HLE(GetCurrentThread) { RET((uint64_t)-2); }
HLE(GetCurrentProcessId) { RET(0x1000); }
HLE(GetCurrentThreadId) { RET((uint64_t)c->tid); }
HLE(IsDebuggerPresent) { RET(0); }
HLE(IsProcessorFeaturePresent)
{
    uint32_t f = (uint32_t)A1;
    RET(f == 6 || f == 7 || f == 10 || f == 13 || f == 36 || f == 37 || f == 38);
}
HLE(QueryPerformanceCounter) { ST64(A1, host_ticks_ns() / 100); RET(1); }
HLE(QueryPerformanceFrequency) { ST64(A1, 10000000); RET(1); }
HLE(GetSystemTimeAsFileTime) { ST64(A1, host_time_100ns()); RET(0); }
HLE(GetSystemTimePreciseAsFileTime) { ST64(A1, host_time_100ns()); RET(0); }
HLE(GetTickCount) { RET((uint32_t)(host_ticks_ns() / 1000000)); }
HLE(GetTickCount64) { RET(host_ticks_ns() / 1000000); }
HLE(InitializeSListHead) { ST64(A1, 0); ST64(A1 + 8, 0); RET(0); }
HLE(RtlCaptureContext) { RET(0); }
HLE(RtlLookupFunctionEntry) { RET(0); }
HLE(RtlVirtualUnwind) { RET(0); }
HLE(UnhandledExceptionFilter) { RET(0); }
HLE(SetUnhandledExceptionFilter) { RET(0); }
HLE(TerminateProcess) { host_log("[exit] TerminateProcess(%d) from %llx", (int)A2, (unsigned long long)LD64(R(4))); rt_exit((int)A2); }
void rt_backtrace(CPU* c, const char* tag);
HLE(ExitProcess) { int code = (int)A1; host_log("[exit] ExitProcess(%d)", code); rt_backtrace(c, "exit"); rt_detach_all(c); rt_exit(code); }
HLE(GetProcessHeap) { RET(0x7000); }
HLE(HeapAlloc) { RET((A2 & 8) ? g_calloc(A3) : g_alloc(A3)); }
HLE(HeapFree) { g_free(A3); RET(1); }
HLE(HeapReAlloc) { RET(g_realloc(A3, A4)); }
HLE(HeapSize) { RET(g_size(A3)); }
HLE(EncodePointer) { RET(A1); }
HLE(DecodePointer) { RET(A1); }
HLE(OutputDebugStringA) { host_log("[debug] %s", GSTR(A1)); RET(0); }

static int tls_used[1088];
static uint64_t tls_addr(CPU* c, uint64_t i, int create);
HLE(TlsAlloc) { for (int i = 0; i < 1088; i++) if (!tls_used[i]) { tls_used[i] = 1; ST64(tls_addr(c, (uint64_t)i, 1), 0); RET(i); return; } RET(0xFFFFFFFF); }
HLE(TlsFree) { if (A1 < 1088) tls_used[A1] = 0; RET(1); }
/* TLS values are per thread, where Windows keeps them: TEB TlsSlots (+0x1480, 64) then TlsExpansionSlots (+0x1780) */
static uint64_t tls_addr(CPU* c, uint64_t i, int create)
{
    if (i < 64) return c->gsbase + 0x1480 + 8 * i;
    uint64_t ex = LD64(c->gsbase + 0x1780);
    if (!ex) {
        if (!create) return 0;
        ex = g_calloc(8 * 1024);
        ST64(c->gsbase + 0x1780, ex);
    }
    return ex + 8 * (i - 64);
}
HLE(TlsGetValue) { uint64_t a = A1 < 1088 ? tls_addr(c, A1, 0) : 0; RET(a ? LD64(a) : 0); }
HLE(TlsSetValue) { if (A1 < 1088) ST64(tls_addr(c, A1, 1), A2); RET(1); }
HLE(FlsAlloc) { hle_TlsAlloc(c); }
HLE(FlsFree) { hle_TlsFree(c); }
HLE(FlsGetValue) { hle_TlsGetValue(c); }
HLE(FlsSetValue) { hle_TlsSetValue(c); }

/* Locks are real: a green thread can be switched out inside one (time slices, blocking calls), so another thread
   that wants it yields until it is free. CRITICAL_SECTION keeps RecursionCount at +12 and OwningThread at +16;
   an SRW lock word is 1 when held exclusively, else 2 per shared holder. */
#define CS_REC(cs) ((cs) + 12)
#define CS_OWNER(cs) ((cs) + 16)
static int cs_free(GThread* t) { uint64_t o = LD64(CS_OWNER(t->block_a)); return !o || o == (uint64_t)t->tid; }
void cs_enter(CPU* c, uint64_t cs)
{
    uint64_t me = sched_current_id();
    uint64_t owner = LD64(CS_OWNER(cs));
    if (owner && owner != me) {
        sched_current()->block_a = cs;
        while (!cs_free(sched_current())) sched_block(c, cs_free, ~0ull);
    }
    ST64(CS_OWNER(cs), me);
    ST32(CS_REC(cs), LD32(CS_REC(cs)) + 1);
}
/* like Windows, any thread may leave (some code hands a section from one thread to another) */
void cs_leave(uint64_t cs)
{
    if (!LD64(CS_OWNER(cs))) return;
    uint32_t r = LD32(CS_REC(cs));
    if (r > 1) { ST32(CS_REC(cs), r - 1); return; }
    ST32(CS_REC(cs), 0);
    ST64(CS_OWNER(cs), 0);
}
static void cs_init(uint64_t cs) { memset(G2H(cs), 0, 40); ST32(cs + 8, 0xFFFFFFFF); }
HLE(InitializeCriticalSection) { cs_init(A1); RET(0); }
HLE(InitializeCriticalSectionAndSpinCount) { cs_init(A1); RET(1); }
HLE(InitializeCriticalSectionEx) { cs_init(A1); RET(1); }
HLE(EnterCriticalSection) { cs_enter(c, A1); RET(0); }
HLE(LeaveCriticalSection) { cs_leave(A1); RET(0); }
HLE(TryEnterCriticalSection)
{
    uint64_t owner = LD64(CS_OWNER(A1)), me = sched_current_id();
    if (owner && owner != me) { RET(0); return; }
    ST64(CS_OWNER(A1), me);
    ST32(CS_REC(A1), LD32(CS_REC(A1)) + 1);
    RET(1);
}
HLE(DeleteCriticalSection) { RET(0); }

static void cs_desc(GThread* t, char* out, int cap)
{
    snprintf(out, cap, " %llx owned by %llu (depth %u)", (unsigned long long)t->block_a, (unsigned long long)LD64(CS_OWNER(t->block_a)),
             (unsigned)LD32(CS_REC(t->block_a)));
}
static int srw_free(GThread* t);
static void srw_desc(GThread* t, char* out, int cap)
{
    uint64_t v = LD64(t->block_a);
    snprintf(out, cap, " %llx wanted %s, held %s", (unsigned long long)t->block_a, t->block_b ? "shared" : "exclusive",
             v == 1 ? "exclusive" : v ? "shared" : "by nobody");
}
__attribute__((constructor)) static void lock_names(void)
{
    sched_name_check(cs_free, "critical section", cs_desc);
    sched_name_check(srw_free, "SRW lock", srw_desc);
}
/* a writer waiting on a lock makes new readers queue behind it, as on Windows, so readers can't starve it */
static int srw_free(GThread* t)
{
    uint64_t v = LD64(t->block_a);
    if (!t->block_b) return v == 0;
    return !(v & 1) && !sched_find_blocked(srw_free, t->block_a, 0);
}
void srw_acquire(CPU* c, uint64_t l, int shared)
{
    GThread* t = sched_current();
    for (;;) {
        uint64_t v = LD64(l);
        int ok = shared ? !(v & 1) && !sched_find_blocked(srw_free, l, 0) : v == 0;
        if (ok) { ST64(l, shared ? v + 2 : 1); return; }
        t->block_a = l;
        t->block_b = (uint64_t)shared;
        sched_block(c, srw_free, ~0ull);
    }
}
void srw_release(uint64_t l, int shared)
{
    uint64_t v = LD64(l);
    if (shared) ST64(l, v >= 2 ? v - 2 : 0);
    else ST64(l, 0);
}
HLE(InitializeSRWLock) { ST64(A1, 0); RET(0); }
HLE(AcquireSRWLockExclusive) { srw_acquire(c, A1, 0); RET(0); }
HLE(ReleaseSRWLockExclusive) { srw_release(A1, 0); RET(0); }
HLE(AcquireSRWLockShared) { srw_acquire(c, A1, 1); RET(0); }
HLE(ReleaseSRWLockShared) { srw_release(A1, 1); RET(0); }
HLE(TryAcquireSRWLockExclusive) { if (LD64(A1) == 0) { ST64(A1, 1); RET(1); } else RET(0); }
HLE(TryAcquireSRWLockShared) { uint64_t v = LD64(A1); if (!(v & 1)) { ST64(A1, v + 2); RET(1); } else RET(0); }

HLE(VirtualAlloc)
{
    uint64_t addr = A1, size = A2;
    if (!addr) { RET(g_pages(size)); return; }
    if (g_pages_query(addr, NULL)) { RET(addr & ~0xFFFull); return; }
    RET(g_pages_at(addr & ~0xFFFFull, size + (addr & 0xFFFF)));
}
HLE(VirtualFree)
{
    uint64_t len, base = g_pages_query(A1, &len);
    if (!base) { RET(0); return; }
    if (A3 & 0x8000) g_pages_free(base);
    else if (A3 & 0x4000) {
        uint64_t a = A1 & ~0xFFFull, n = A2 ? ((A1 + A2 + 0xFFF) & ~0xFFFull) - a : base + len - a;
        if (a + n > base + len) n = base + len - a;
        memset(G2H(a), 0, (size_t)n);
    }
    RET(1);
}
HLE(VirtualProtect) { if (A4) ST32(A4, 4); RET(1); }
HLE(GetSystemInfo)
{
    memset(G2H(A1), 0, 48);
    ST32(A1 + 4, 4096);
    ST64(A1 + 8, 0x10000);
    ST64(A1 + 16, MEM_SIZE - 1);
    ST64(A1 + 24, 1);
    ST32(A1 + 32, 1);
    ST32(A1 + 36, 8664);
    ST32(A1 + 40, 65536);
    RET(0);
}

void wstr_to_utf8(uint64_t p, char* out, size_t cap)
{
    size_t n = 0;
    for (; p && n + 4 < cap; p += 2) {
        uint32_t w = (uint32_t)LD16(p);
        if (!w) break;
        if (w >= 0xD800 && w < 0xDC00) { uint32_t lo = (uint32_t)LD16(p + 2); w = 0x10000 + ((w - 0xD800) << 10) + (lo - 0xDC00); p += 2; }
        if (w < 0x80) out[n++] = (char)w;
        else if (w < 0x800) { out[n++] = (char)(0xC0 | (w >> 6)); out[n++] = (char)(0x80 | (w & 63)); }
        else if (w < 0x10000) { out[n++] = (char)(0xE0 | (w >> 12)); out[n++] = (char)(0x80 | ((w >> 6) & 63)); out[n++] = (char)(0x80 | (w & 63)); }
        else { out[n++] = (char)(0xF0 | (w >> 18)); out[n++] = (char)(0x80 | ((w >> 12) & 63)); out[n++] = (char)(0x80 | ((w >> 6) & 63)); out[n++] = (char)(0x80 | (w & 63)); }
    }
    out[n] = 0;
}

size_t utf8_to_wstr(const char* s, uint64_t out, size_t cap)
{
    size_t n = 0;
    const uint8_t* p = (const uint8_t*)s;
    while (*p && n + 1 < cap) {
        uint32_t w = *p++;
        if (w >= 0xF0) { w = ((w & 7) << 18) | ((p[0] & 63) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if (w >= 0xE0) { w = ((w & 15) << 12) | ((p[0] & 63) << 6) | (p[1] & 63); p += 2; }
        else if (w >= 0xC0) { w = ((w & 31) << 6) | (p[0] & 63); p += 1; }
        if (w >= 0x10000) { if (n + 2 >= cap) break; w -= 0x10000; ST16(out + 2 * n++, 0xD800 + (w >> 10)); ST16(out + 2 * n++, 0xDC00 + (w & 1023)); }
        else ST16(out + 2 * n++, w);
    }
    if (cap) ST16(out + 2 * n, 0);
    return n;
}

static const char* hle_modules[] = { "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
    "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "ws2_32.dll", "winmm.dll", "xinput1_3.dll", "xinput1_4.dll",
    "xinput9_1_0.dll", "d3d11.dll", "dxgi.dll", "d3d9.dll", "dsound.dll", "xaudio2_7.dll", "xaudio2_9.dll", "dbghelp.dll",
    "version.dll", "imm32.dll", "dwmapi.dll", "setupapi.dll", "iphlpapi.dll", "winhttp.dll", "wininet.dll", "crypt32.dll",
    "bcrypt.dll", "d3dcompiler_47.dll", "dinput8.dll", "hid.dll", "psapi.dll", "powrprof.dll", "secur32.dll", "normaliz.dll",
    "steam_api64.dll", NULL };
#define HLE_MOD_BASE 0x7FF00000ull

static uint64_t module_handle(const char* name)
{
    if (!name) return g_exe_base;
    const ModuleInfo* m = rt_find_module(name);
    if (m) return m->base;
    const char* base = name;
    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s", base);
    for (char* p = tmp; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
    if (!strchr(tmp, '.')) strcat(tmp, ".dll");
    if (!strncmp(tmp, "api-ms-win-", 11) || !strncmp(tmp, "ext-ms-", 7)) return HLE_MOD_BASE;
    for (int i = 0; hle_modules[i]; i++)
        if (!strcmp(hle_modules[i], tmp)) return HLE_MOD_BASE + 0x10000ull * (i + 1);
    return 0;
}

HLE(GetModuleHandleA) { RET(module_handle(A1 ? GSTR(A1) : NULL)); }
HLE(GetModuleHandleW) { char n[520]; if (A1) wstr_to_utf8(A1, n, sizeof n); RET(module_handle(A1 ? n : NULL)); }
HLE(GetModuleHandleExW) { char n[520]; uint64_t h; if (A1 & 4) { const ModuleInfo* m = rt_module_at(A2); h = m ? m->base : 0; } else { if (A2) wstr_to_utf8(A2, n, sizeof n); h = module_handle(A2 ? n : NULL); } if (A3) ST64(A3, h); RET(h != 0); }
HLE(GetModuleHandleExA) { uint64_t h; if (A1 & 4) { const ModuleInfo* m = rt_module_at(A2); h = m ? m->base : 0; } else h = module_handle(A2 ? GSTR(A2) : NULL); if (A3) ST64(A3, h); RET(h != 0); }
/* every module asked for, once: what a game tries to load says which features it turns on */
static void note_load(const char* n, uint64_t h)
{
    static uint32_t seen[256];
    static int nseen;
    uint32_t k = 2166136261u;
    for (const char* p = n; *p; p++) k = (k ^ (uint8_t)(*p | 0x20)) * 16777619u;
    for (int i = 0; i < nseen; i++) if (seen[i] == k) return;
    if (nseen < 256) seen[nseen++] = k;
    host_log(h ? "[load] LoadLibrary(%s)" : "[load] LoadLibrary(%s) failed", n);
}
HLE(LoadLibraryA) { uint64_t h = module_handle(GSTR(A1)); note_load(GSTR(A1), h); RET(h); }
HLE(LoadLibraryW) { char n[520]; wstr_to_utf8(A1, n, sizeof n); uint64_t h = module_handle(n); note_load(n, h); RET(h); }
HLE(LoadLibraryExA) { hle_LoadLibraryA(c); }
HLE(LoadLibraryExW) { hle_LoadLibraryW(c); }
HLE(FreeLibrary) { RET(1); }
HLE(GetProcAddress)
{
    uint64_t h = A1;
    const char* name = A2 < 0x10000 ? (const char*)(uintptr_t)A2 : GSTR(A2);
    if (h >= HLE_MOD_BASE && A2 < 0x10000) {
        GuestFn hle_by_ordinal(const char* dll, const char* name);
        uint64_t i = (h - HLE_MOD_BASE) / 0x10000;
        char ord[16];
        snprintf(ord, sizeof ord, "#%u", (unsigned)A2);
        GuestFn f = i >= 1 && i <= sizeof hle_modules / sizeof *hle_modules - 1 ? hle_by_ordinal(hle_modules[i - 1], ord) : NULL;
        RET(f ? rt_dyn_thunk(f, ord) : 0);
        return;
    }
    if (h >= HLE_MOD_BASE) { RET(rt_thunk_by_name(name)); return; }
    uint64_t hle_export_override(const char* module, const char* name);
    const ModuleInfo* m = A2 >= 0x10000 ? rt_module_at(h) : NULL;
    uint64_t o = m ? hle_export_override(m->name, name) : 0;
    RET(o ? o : rt_get_export(h, name));
}

static void module_path(uint64_t h, char* out, size_t cap)
{
    const ModuleInfo* m = rt_module_at(h ? h : g_exe_base);
    char exe[600];
    snprintf(exe, sizeof exe, "C:\\%s", g_exe_rel);
    for (char* p = exe; *p; p++) if (*p == '/') *p = '\\';
    if (!m || !m->is_dll) { snprintf(out, cap, "%s", exe); return; }
    char* slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(out, cap, "%s%s", exe, m->name);
}
HLE(GetModuleFileNameA) { char p[300]; module_path(A1, p, sizeof p); size_t n = strlen(p); if (n >= A3) n = A3 ? A3 - 1 : 0; memcpy(G2H(A2), p, n); if (A3) ST8(A2 + n, 0); RET(n); }
HLE(GetModuleFileNameW) { char p[300]; module_path(A1, p, sizeof p); RET(utf8_to_wstr(p, A2, (size_t)A3)); }