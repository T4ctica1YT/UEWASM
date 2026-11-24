#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>

int _strnicmp_portable(const char* a, const char* b, size_t n);
static uint64_t cmd_w, cmd_a, env_w, env_a;
static char env_buf[16384];
static size_t env_len;
static void env_publish(void);
static int env_dbg = -1;
#define EL(...) do { if (env_dbg < 0) env_dbg = host_query_flag("envdebug"); if (env_dbg) host_log("[env] " __VA_ARGS__); } while (0)
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
    memcpy(env_buf, env, sizeof env);
    env_len = sizeof env - 2;
    env_publish();
}

/* the guest's copies (GetEnvironmentStrings) are rebuilt on every change; old ones stay valid for callers holding them */
static void env_publish(void)
{
    env_a = g_alloc(env_len + 1);
    memcpy(G2H(env_a), env_buf, env_len + 1);
    env_w = g_alloc(2 * (env_len + 1));
    uint64_t w = env_w;
    for (const char* e = env_buf; *e; e += strlen(e) + 1) {
        size_t l = strlen(e) + 1;
        w += 2 * utf8_to_wstr(e, w, l);
        ST16(w, 0);
        w += 2;
    }
    ST16(w, 0);
}

static void env_set(const char* name, const char* value)
{
    size_t n = strlen(name);
    EL("set %s=%s", name, value ? value : "(deleted)");
    if (!n || strchr(name, '=')) return;
    char* e = env_buf;
    while (*e) {
        size_t l = strlen(e) + 1;
        if (!_strnicmp_portable(e, name, n) && e[n] == '=') {
            memmove(e, e + l, (size_t)(env_buf + env_len + 1 - (e + l)));
            env_len -= l;
            break;
        }
        e += l;
    }
    if (value) {
        size_t add = n + 1 + strlen(value) + 1;
        if (env_len + add + 1 > sizeof env_buf) return;
        snprintf(env_buf + env_len, add, "%s=%s", name, value);
        env_len += add;
        env_buf[env_len] = 0;
    }
    env_publish();
}

/* arguments added before the game first reads its command line (the Epic sign-in's, as the launcher passes them) */
void process_cmdline_append(const char* s)
{
    size_t o = strlen(g_cmdline);
    snprintf(g_cmdline + o, sizeof g_cmdline - o, " %s", s);
    cmd_a = g_strdup(g_cmdline);
    cmd_w = g_alloc(2 * strlen(g_cmdline) + 2);
    utf8_to_wstr(g_cmdline, cmd_w, strlen(g_cmdline) + 1);
}
void epic_signin(CPU* c);
static void cmdline_ready(CPU* c)
{
    static int done;
    if (!done) { done = 1; epic_signin(c); }
}
HLE(GetCommandLineW) { cmdline_ready(c); RET(cmd_w); }
HLE(GetCommandLineA) { cmdline_ready(c); RET(cmd_a); }
HLE(GetEnvironmentStringsW) { RET(env_w); }
HLE(GetEnvironmentStrings) { RET(env_a); }
HLE(GetEnvironmentStringsA) { RET(env_a); }
HLE(FreeEnvironmentStringsW) { RET(1); }
HLE(FreeEnvironmentStringsA) { RET(1); }

static const char* env_lookup(const char* name)
{
    const char* e = env_buf;
    size_t n = strlen(name);
    while (*e) {
        if (!_strnicmp_portable(e, name, n) && e[n] == '=') { EL("get %s: %s", name, e + n + 1); return e + n + 1; }
        e += strlen(e) + 1;
    }
    EL("get %s: not set", name);
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
HLE(SetEnvironmentVariableW)
{
    char name[256], value[4096];
    wstr_to_utf8(A1, name, sizeof name);
    if (A2) wstr_to_utf8(A2, value, sizeof value);
    env_set(name, A2 ? value : NULL);
    RET(1);
}
HLE(SetEnvironmentVariableA) { env_set(GSTR(A1), A2 ? GSTR(A2) : NULL); RET(1); }
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
void hle_set_error(uint32_t e);
/* version checks against what GetVersionEx reports (Windows 10 build 19045, workstation, no service pack): each
   VER_* field's condition sits in 3 bits at 3 * its bit index; major, minor and the service pack compare as one
   version under the condition of the most significant one given, as Windows does */
HLE(VerSetConditionMask)
{
    uint64_t m = RT_G32 ? (uint32_t)A1 | (uint64_t)(uint32_t)A2 << 32 : A1;
    uint32_t type = (uint32_t)(RT_G32 ? A3 : A2), cond = (uint32_t)(RT_G32 ? A4 : A3) & 7;
    for (int i = 0; i < 8; i++)
        if (type & (1u << i)) m = (m & ~(7ull << (3 * i))) | (uint64_t)cond << (3 * i);
    RET(m);   /* a 32-bit caller gets it in edx:eax (rt_hle32 splits it) */
}

static int ver_cmp(int64_t have, int64_t want, int cond)
{
    switch (cond) {
    case 1: return have == want;
    case 2: return have > want;
    case 3: return have >= want;
    case 4: return have < want;
    case 5: return have <= want;
    default: return 0;
    }
}

/* 1 when the system matches; wide: OSVERSIONINFOEXW (else the A form, with a 128-byte service pack string) */
static int verify_version(uint64_t vi, uint32_t type, uint64_t m, int wide)
{
    uint64_t sp = vi + 20 + (wide ? 256 : 128);
    int64_t have[4] = { 10, 0, 0, 0 }, want[4] = { LD32(vi + 4), LD32(vi + 8), LD16(sp), LD16(sp + 2) };
    static const uint32_t bits[4] = { 2, 1, 0x20, 0x10 };
    static const int idx[4] = { 1, 0, 5, 4 };
    int lead = -1;
    for (int i = 0; i < 4 && lead < 0; i++) if (type & bits[i]) lead = i;
    if (lead >= 0) {
        int cond = (int)(m >> (3 * idx[lead]) & 7), r = 0;
        for (int i = lead; i < 4; i++) {
            if (!(type & bits[i])) continue;
            if (have[i] != want[i]) { r = have[i] > want[i] ? 1 : -1; break; }
        }
        if (!ver_cmp(r, 0, cond)) return 0;
    }
    if ((type & 4) && !ver_cmp(19045, LD32(vi + 12), (int)(m >> 6 & 7))) return 0;
    if ((type & 8) && !ver_cmp(2, LD32(vi + 16), (int)(m >> 9 & 7))) return 0;
    if (type & 0x40) {
        uint32_t want_s = LD16(sp + 4), have_s = 0x100, c = (uint32_t)(m >> 18 & 7);   /* VER_SUITE_SINGLEUSERTS */
        if (c == 6 ? (have_s & want_s) != want_s : !(have_s & want_s)) return 0;
    }
    if ((type & 0x80) && !ver_cmp(1, LD8(sp + 6), (int)(m >> 21 & 7))) return 0;   /* VER_NT_WORKSTATION */
    return 1;
}

static void verify_version_info(CPU* c, int wide)
{
    uint32_t type = (uint32_t)A2;
    uint64_t m = RT_G32 ? (uint32_t)A3 | (uint64_t)(uint32_t)A4 << 32 : A3;
    if (!A1 || !type || !m) { hle_set_error(87); RET(0); return; }
    if (!verify_version(A1, type, m, wide)) { hle_set_error(1150); RET(0); return; }   /* ERROR_OLD_WIN_VERSION */
    RET(1);
}
HLE(VerifyVersionInfoW) { verify_version_info(c, 1); }
HLE(VerifyVersionInfoA) { verify_version_info(c, 0); }
HLE(RtlVerifyVersionInfo)
{
    uint32_t type = (uint32_t)A2;
    uint64_t m = RT_G32 ? (uint32_t)A3 | (uint64_t)(uint32_t)A4 << 32 : A3;
    if (!A1 || !type || !m) { RET(0xC000000D); return; }
    RET(verify_version(A1, type, m, 1) ? 0 : 0xC0000059);   /* STATUS_REVISION_MISMATCH */
}
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
            if (t) { const char* n = GSTR(LDP(A4 + GP)); snprintf(t->name, sizeof t->name, "%s", n); sched_named(t); }
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
