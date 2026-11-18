#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>

int _strnicmp_portable(const char* a, const char* b, size_t n);
static uint64_t cmd_w, cmd_a, env_w, env_a;
char g_cmdline[4096];

static int skip_arg(const char* a) { return !strcmp(a, "--image") || !strcmp(a, "--root") || !strcmp(a, "--exe") || !strcmp(a, "--save"); }

void process_init(int argc, char** argv)
{
    size_t o = (size_t)snprintf(g_cmdline, sizeof g_cmdline, "\"C:\\%s\"", g_exe_rel);
    for (char* p = g_cmdline; *p; p++) if (*p == '/') *p = '\\';
    for (int i = 1; i < argc; i++) {
        if (skip_arg(argv[i])) { i++; continue; }
        const char* a = argv[i];
        const char *sp = strchr(a, ' '), *eq = strchr(a, '=');
        if (!sp || strchr(a, '"'))
            o += (size_t)snprintf(g_cmdline + o, sizeof g_cmdline - o, " %s", a);
        else if (a[0] == '-' && eq && eq < sp)
            /* -Key=value with spaces: quote only the value, as a shell would pass it. Unreal reads the raw command
               line and skips quoted text when it looks for "Key=", so "-ExecCmds=r.Foo 0" would be ignored */
            o += (size_t)snprintf(g_cmdline + o, sizeof g_cmdline - o, " %.*s\"%s\"", (int)(eq + 1 - a), a, eq + 1);
        else
            o += (size_t)snprintf(g_cmdline + o, sizeof g_cmdline - o, " \"%s\"", a);
        if (o >= sizeof g_cmdline) o = sizeof g_cmdline - 1;
    }
    cmd_a = g_strdup(g_cmdline);
    cmd_w = g_alloc(2 * strlen(g_cmdline) + 2);
    utf8_to_wstr(g_cmdline, cmd_w, strlen(g_cmdline) + 1);
    static const char env[] = "SystemRoot=C:\\Windows\0USERPROFILE=C:\\Users\\Player\0LOCALAPPDATA=C:\\Users\\Player\\AppData\\Local\0APPDATA=C:\\Users\\Player\\AppData\\Roaming\0TEMP=C:\\Temp\0TMP=C:\\Temp\0USERNAME=Player\0COMPUTERNAME=WEB\0NUMBER_OF_PROCESSORS=4\0\0";
    env_a = g_alloc(sizeof env);
    memcpy(G2H(env_a), env, sizeof env);
    env_w = g_alloc(2 * sizeof env);
    for (size_t i = 0; i < sizeof env; i++) ST16(env_w + 2 * i, (uint8_t)env[i]);
}

HLE(GetCommandLineW) { RET(cmd_w); }
HLE(GetCommandLineA) { RET(cmd_a); }
HLE(GetEnvironmentStringsW) { RET(env_w); }
HLE(GetEnvironmentStrings) { RET(env_a); }
HLE(GetEnvironmentStringsA) { RET(env_a); }
HLE(FreeEnvironmentStringsW) { RET(1); }
HLE(FreeEnvironmentStringsA) { RET(1); }

static const char* env_lookup(const char* name)
{
    const char* e = GSTR(env_a);
    size_t n = strlen(name);
    while (*e) {
        if (!_strnicmp_portable(e, name, n) && e[n] == '=') return e + n + 1;
        e += strlen(e) + 1;
    }
    return NULL;
}

int _strnicmp_portable(const char* a, const char* b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int x = a[i] >= 'A' && a[i] <= 'Z' ? a[i] + 32 : a[i], y = b[i] >= 'A' && b[i] <= 'Z' ? b[i] + 32 : b[i];
        if (x != y || !x) return x - y;
    }
    return 0;
}

HLE(GetEnvironmentVariableW)
{
    char name[256];
    wstr_to_utf8(A1, name, sizeof name);
    const char* v = env_lookup(name);
    if (!v) { ST32(g_teb + TEB_ERR, 203); RET(0); return; }
    size_t n = strlen(v);
    if (A3 <= n) { RET(n + 1); return; }
    RET(utf8_to_wstr(v, A2, (size_t)A3));
}
HLE(GetEnvironmentVariableA)
{
    const char* v = env_lookup(GSTR(A1));
    if (!v) { ST32(g_teb + TEB_ERR, 203); RET(0); return; }
    size_t n = strlen(v);
    if (A3 <= n) { RET(n + 1); return; }
    memcpy(G2H(A2), v, n + 1);
    RET(n);
}
HLE(SetEnvironmentVariableW) { RET(1); }
HLE(SetEnvironmentVariableA) { RET(1); }
HLE(ExpandEnvironmentStringsW) { uint64_t n = 0; while (LD16(A1 + 2 * n)) n++; if (A3 > n) memcpy(G2H(A2), G2H(A1), 2 * (n + 1)); RET(n + 1); }

HLE(GetStartupInfoW) { uint32_t n = RT_G32 ? 68 : 104; memset(G2H(A1), 0, n); ST32(A1, n); RET(0); }
HLE(GetStartupInfoA) { uint32_t n = RT_G32 ? 68 : 104; memset(G2H(A1), 0, n); ST32(A1, n); RET(0); }
HLE(SetErrorMode) { RET(0); }
HLE(GetErrorMode) { RET(0); }
HLE(SetThreadErrorMode) { if (A2) ST32(A2, 0); RET(1); }
HLE(HeapQueryInformation) { RET(1); }
HLE(HeapSetInformation) { RET(1); }
HLE(HeapCompact) { RET(0); }
HLE(HeapValidate) { RET(1); }
HLE(HeapWalk) { ST32(g_teb + TEB_ERR, 259); RET(0); }
HLE(HeapCreate) { RET(0x7000); }
HLE(HeapDestroy) { RET(1); }
HLE(HeapLock) { RET(1); }
HLE(HeapUnlock) { RET(1); }
HLE(GetProcessHeaps) { if (A1 >= 1) STP(A2, 0x7000); RET(1); }
HLE(LocalAlloc) { RET((A1 & 0x40) ? g_calloc(A2) : g_alloc(A2)); }
HLE(LocalFree) { g_free(A1); RET(0); }
HLE(GlobalAlloc) { RET((A1 & 0x40) ? g_calloc(A2) : g_alloc(A2)); }
HLE(GlobalFree) { g_free(A1); RET(0); }
HLE(GlobalLock) { RET(A1); }
HLE(GlobalUnlock) { RET(1); }

HLE(VirtualQuery)
{
    uint64_t a = A1 & ~0xFFFull;
    const ModuleInfo* m = rt_module_at(A1);
    uint64_t len, heap = m ? 0 : g_pages_query(A1, &len);
    uint64_t base = m ? m->base : heap, size = m ? m->size : len;
    if (!m && !heap) { base = 0; size = 0; }
    if (RT_G32) {
        memset(G2H(A2), 0, 28);
        ST32(A2, a);
        ST32(A2 + 4, base);
        ST32(A2 + 8, (m || heap) ? 4 : 0);
        ST32(A2 + 12, (m || heap) ? base + size - a : 0x1000);
        ST32(A2 + 16, (m || heap) ? 0x1000 : 0x10000);
        ST32(A2 + 20, (m || heap) ? (m ? 0x20 : 4) : 1);
        ST32(A2 + 24, (m || heap) ? (m ? 0x1000000 : 0x20000) : 0);
        RET(28);
        return;
    }
    memset(G2H(A2), 0, 48);
    if (!m && !heap) {
        ST64(A2, a);
        ST64(A2 + 24, 0x1000);
        ST32(A2 + 32, 0x10000);
        ST32(A2 + 36, 1);
        RET(48);
        return;
    }
    ST64(A2, a);
    ST64(A2 + 8, base);
    ST32(A2 + 16, 4);
    ST64(A2 + 24, base + size - a);
    ST32(A2 + 32, 0x1000);
    ST32(A2 + 36, m ? 0x20 : 4);
    ST32(A2 + 40, m ? 0x1000000 : 0x20000);
    RET(48);
}
HLE(VerSetConditionMask) { RET(A1); }
HLE(VerifyVersionInfoW) { RET(1); }
HLE(VerifyVersionInfoA) { RET(1); }
HLE(GetVersion) { RET(0x47BB000A); }
HLE(GetVersionExW)
{
    uint32_t sz = (uint32_t)LD32(A1);
    memset(G2H(A1 + 4), 0, sz - 4);
    ST32(A1 + 4, 10);
    ST32(A1 + 8, 0);
    ST32(A1 + 12, 19045);
    ST32(A1 + 16, 2);
    RET(1);
}
HLE(GetVersionExA) { hle_GetVersionExW(c); }
HLE(RtlGetVersion) { hle_GetVersionExW(c); RET(0); }
HLE(GetNativeSystemInfo) { extern void hle_GetSystemInfo(CPU*); hle_GetSystemInfo(c); }
HLE(GlobalMemoryStatusEx)
{
    ST32(A1 + 4, 30);
    ST64(A1 + 8, MEM_SIZE);
    ST64(A1 + 16, MEM_SIZE - g_mem_used());
    ST64(A1 + 24, MEM_SIZE);
    ST64(A1 + 32, MEM_SIZE - g_mem_used());
    ST64(A1 + 40, MEM_SIZE);
    ST64(A1 + 48, MEM_SIZE - g_mem_used());
    ST64(A1 + 56, 0);
    RET(1);
}
HLE(GetComputerNameW) { utf8_to_wstr("WEB", A1, 16); ST32(A2, 3); RET(1); }
HLE(GetComputerNameA) { snprintf((char*)G2H(A1), 16, "WEB"); ST32(A2, 3); RET(1); }
HLE(GetUserNameW) { utf8_to_wstr("Player", A1, 32); ST32(A2, 7); RET(1); }
HLE(GetUserNameA) { snprintf((char*)G2H(A1), 32, "Player"); ST32(A2, 7); RET(1); }
HLE(OutputDebugStringW) { char s[2048]; wstr_to_utf8(A1, s, sizeof s); host_log("[debug] %s", s); RET(0); }
HLE(Beep) { RET(1); }
HLE(GetExitCodeProcess) { ST32(A2, 259); RET(1); }
HLE(CreateProcessW) { ST32(g_teb + TEB_ERR, 2); RET(0); }
HLE(CreateProcessA) { ST32(g_teb + TEB_ERR, 2); RET(0); }
HLE(RtlPcToFileHeader) { const ModuleInfo* m = rt_module_at(A1); STP(A2, m ? m->base : 0); RET(m ? m->base : 0); }
HLE(GetProcessTimes) { ST64(A2, 0); ST64(A3, 0); ST64(A4, 0); ST64(ARG(5), host_ticks_ns() / 100); RET(1); }
HLE(GetThreadTimes) { ST64(A2, 0); ST64(A3, 0); ST64(A4, 0); ST64(ARG(5), host_ticks_ns() / 100); RET(1); }
int hle_cores(void);
HLE(GetProcessAffinityMask) { uint64_t m = (1ull << hle_cores()) - 1; STP(A2, m); STP(A3, m); RET(1); }
HLE(SetProcessAffinityMask) { RET(1); }
HLE(GetActiveProcessorCount) { RET(hle_cores()); }
HLE(GetMaximumProcessorCount) { RET(hle_cores()); }
HLE(GetCurrentProcessorNumber) { RET(0); }
HLE(GetPriorityClass) { RET(0x20); }
HLE(SetPriorityClass) { RET(1); }
HLE(GetProcessId) { RET(0x1000); }
HLE(K32GetProcessMemoryInfo) { memset(G2H(A2), 0, (size_t)A3); ST32(A2, (uint32_t)A3); ST64(A2 + 16, g_mem_used()); ST64(A2 + 8, g_mem_used()); RET(1); }
HLE(GetProcessMemoryInfo) { hle_K32GetProcessMemoryInfo(c); }
HLE(FlushInstructionCache) { RET(1); }

HLE(InterlockedPushEntrySList)
{
    uint64_t head = A1, e = A2, old = LDP(head), d = head + GP;
    STP(e, old);
    STP(head, e);
    ST16(d, LD16(d) + 1);
    RET(old);
}
HLE(InterlockedPopEntrySList)
{
    uint64_t head = A1, e = LDP(head), d = head + GP;
    if (e) { STP(head, LDP(e)); ST16(d, LD16(d) - 1); }
    RET(e);
}
HLE(InterlockedFlushSList) { uint64_t e = LDP(A1); memset(G2H(A1), 0, 2 * GP); RET(e); }
HLE(QueryDepthSList) { RET(LD16(A1 + GP)); }
HLE(RtlInitializeSListHead) { memset(G2H(A1), 0, 2 * GP); RET(0); }

void rt_backtrace(CPU* c, const char* tag);
int rt_seh_raise32(CPU* c, uint32_t code, uint32_t flags, uint32_t n, uint64_t args, uint32_t ret, uint32_t esp);
static int guest_ptr(uint64_t p) { return p >= 0x10000 && p < MEM_SIZE - 256; }
/* an MSVC C++ exception (0xE06D7363, parameters { magic, object, ThrowInfo[, image base] }): the thrown type's name
   from its first catchable type, and the message of a std::exception (vtable, then the what() string) */
static void cxx_describe(uint64_t args, uint32_t n)
{
    if (n < 3 || !guest_ptr(args) || (LD32(args) & ~3u) != 0x19930520u) return;
    uint64_t obj = LDP(args + GP), ti = LDP(args + 2 * GP), base = !RT_G32 && n >= 4 ? LDP(args + 3 * GP) : 0;
    /* ThrowInfo { attributes, unwind, forward compat, catchable type array }: pointers on x86, image offsets on x64 */
    if (!guest_ptr(ti)) return;
    uint64_t cta = (uint32_t)LD32(ti + 12) + base;
    if (!guest_ptr(cta) || !LD32(cta)) return;
    uint64_t ct = (uint32_t)LD32(cta + 4) + base, td = guest_ptr(ct) ? (uint32_t)LD32(ct + 4) + base : 0;
    if (!guest_ptr(td)) return;
    const char* type = GSTR(td + 2 * GP);
    host_log("[seh] C++ exception %.120s (object %llx)", type, (unsigned long long)obj);
    if (!guest_ptr(obj)) return;
    /* a thrown string (Unreal's appThrowf throws its TCHAR buffer): .PA_W / .PB_W wide, .PAD / .PBD narrow */
    int wide = !strcmp(type, ".PA_W") || !strcmp(type, ".PB_W"), narrow = !strcmp(type, ".PAD") || !strcmp(type, ".PBD");
    uint64_t w = LDP(obj + (wide || narrow ? 0 : GP));
    if (!guest_ptr(w)) return;
    char msg[256];
    if (wide) wstr_to_utf8(w, msg, sizeof msg);
    else snprintf(msg, sizeof msg, "%s", GSTR(w));
    if ((unsigned char)msg[0] >= 0x20) host_log("[seh]   %s: %s", wide || narrow ? "message" : "what()", msg);
}
HLE(RaiseException)
{
    if ((uint32_t)A1 == 0x406D1388) {
        /* MSVC thread-name exception: { 0x1000, name, thread id, flags }, pointer-sized each */
        if (A4 && A3 >= 3 && LD32(A4) == 0x1000) {
            uint32_t tid = (uint32_t)LD32(A4 + 2 * GP);
            GThread* t = tid == 0xFFFFFFFFu ? sched_current() : sched_by_tid(tid);
            if (t) { const char* n = GSTR(LDP(A4 + GP)); snprintf(t->name, sizeof t->name, "%s", n); }
        }
        RET(0);
        return;
    }
    if (RT_G32) {
        /* the game's own handlers first: a C++ catch takes it from here and never returns */
        static int quiet = -1;
        if (quiet < 0) quiet = !host_query_flag("sehdebug");
        if (!quiet) { host_log("[seh] RaiseException code=%08x from %llx", (unsigned)A1, (unsigned long long)LD64(R(4))); if ((uint32_t)A1 == 0xE06D7363u) cxx_describe(A4, (uint32_t)A3); }
        uint32_t esp = (uint32_t)LD32(R(4) + 8 * 21);   /* the guest esp at the call (rt_hle32 keeps it above the arguments) */
        uint64_t a1 = A1, a2 = A2, a3 = A3, a4 = A4;   /* the handlers' calls reuse the argument registers */
        if (rt_seh_raise32(c, (uint32_t)a1, (uint32_t)a2, (uint32_t)a3, a4, (uint32_t)LD64(R(4)), esp)) { RET(0); return; }
        host_log("[seh] unhandled exception");
        A1 = a1; A2 = a2; A3 = a3; A4 = a4;
    }
    host_log("[seh] RaiseException code=%08llx flags=%llx from %llx", (unsigned long long)(uint32_t)A1, (unsigned long long)A2, (unsigned long long)LD64(R(4)));
    if ((uint32_t)A1 == 0xE06D7363u) cxx_describe(A4, (uint32_t)A3);
    rt_backtrace(c, "seh");
    rt_exit((int)(uint32_t)A1);
}
void rt_sj_jump(CPU* c, uint64_t jb, uint64_t val);
void rt_seh_unwind32(CPU* c, uint32_t target, uint64_t record, uint32_t ret, uint32_t esp);
HLE(RtlUnwind)
{
    /* 32-bit: a real unwind (C++ catches, longjmp past SEH frames); the caller carries on afterwards */
    if (RT_G32) {
        rt_seh_unwind32(c, (uint32_t)A1, A3, (uint32_t)LD64(R(4)), (uint32_t)LD32(R(4) + 8 * 21));
        RET(A4);
        return;
    }
    /* the only unwind we see from CRT code is longjmp: STATUS_LONGJUMP with the jump buffer as the first parameter */
    if (A3 && (uint32_t)LD32(A3) == 0x80000026u) rt_sj_jump(c, LD64(A3 + 32), A4);
    host_log("[seh] RtlUnwind(frame=%llx, target=%llx, code=%x)", (unsigned long long)A1, (unsigned long long)A2, A3 ? (unsigned)LD32(A3) : 0);
    rt_backtrace(c, "unwind");
    rt_exit(3);
}
HLE(RtlUnwindEx) { host_log("[seh] RtlUnwindEx"); rt_exit(3); }

/* ThreadBasicInformation (class 0) of the calling thread: its TEB and ids, laid out for the guest's bitness. Games
   fetch this through GetProcAddress and call it without checking, so it has to exist */
HLE(NtQueryInformationThread)
{
    uint32_t cls = (uint32_t)A2, len = (uint32_t)A4, need = RT_G32 ? 28 : 48;
    uint64_t out = A3;
    if (cls != 0) { if (ARG(5)) ST32(ARG(5), 0); RET(0xC0000003u); return; }        /* STATUS_INVALID_INFO_CLASS */
    if (!out || len < need) { if (ARG(5)) ST32(ARG(5), need); RET(0xC0000004u); return; }   /* STATUS_INFO_LENGTH_MISMATCH */
    memset(G2H(out), 0, need);
    STP(out + GP, c->gsbase);
    STP(out + 2 * GP, 1);
    STP(out + 3 * GP, (uint64_t)c->tid);
    STP(out + 4 * GP, 1);
    ST32(out + 5 * GP, 8);
    ST32(out + 5 * GP + 4, 8);
    if (ARG(5)) ST32(ARG(5), need);
    RET(0);
}
