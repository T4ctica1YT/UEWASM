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
        o += (size_t)snprintf(g_cmdline + o, sizeof g_cmdline - o, strchr(argv[i], ' ') ? " \"%s\"" : " %s", argv[i]);
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
    if (!v) { ST32(g_teb + 0x68, 203); RET(0); return; }
    size_t n = strlen(v);
    if (A3 <= n) { RET(n + 1); return; }
    RET(utf8_to_wstr(v, A2, (size_t)A3));
}
HLE(GetEnvironmentVariableA)
{
    const char* v = env_lookup(GSTR(A1));
    if (!v) { ST32(g_teb + 0x68, 203); RET(0); return; }
    size_t n = strlen(v);
    if (A3 <= n) { RET(n + 1); return; }
    memcpy(G2H(A2), v, n + 1);
    RET(n);
}
HLE(SetEnvironmentVariableW) { RET(1); }
HLE(SetEnvironmentVariableA) { RET(1); }
HLE(ExpandEnvironmentStringsW) { uint64_t n = 0; while (LD16(A1 + 2 * n)) n++; if (A3 > n) memcpy(G2H(A2), G2H(A1), 2 * (n + 1)); RET(n + 1); }

HLE(GetStartupInfoW) { memset(G2H(A1), 0, 104); ST32(A1, 104); RET(0); }
HLE(GetStartupInfoA) { memset(G2H(A1), 0, 104); ST32(A1, 104); RET(0); }
HLE(SetErrorMode) { RET(0); }
HLE(GetErrorMode) { RET(0); }
HLE(SetThreadErrorMode) { if (A2) ST32(A2, 0); RET(1); }
HLE(HeapQueryInformation) { RET(1); }
HLE(HeapSetInformation) { RET(1); }
HLE(HeapCompact) { RET(0); }
HLE(HeapValidate) { RET(1); }
HLE(HeapWalk) { ST32(g_teb + 0x68, 259); RET(0); }
HLE(HeapCreate) { RET(0x7000); }
HLE(HeapDestroy) { RET(1); }
HLE(HeapLock) { RET(1); }
HLE(HeapUnlock) { RET(1); }
HLE(GetProcessHeaps) { if (A1 >= 1) ST64(A2, 0x7000); RET(1); }
HLE(LocalAlloc) { RET((A1 & 0x40) ? g_calloc(A2) : g_alloc(A2)); }
HLE(LocalFree) { g_free(A1); RET(0); }
HLE(GlobalAlloc) { RET((A1 & 0x40) ? g_calloc(A2) : g_alloc(A2)); }
HLE(GlobalFree) { g_free(A1); RET(0); }
HLE(GlobalLock) { RET(A1); }
HLE(GlobalUnlock) { RET(1); }

HLE(VirtualQuery)
{
    uint64_t a = A1 & ~0xFFFull;
    memset(G2H(A2), 0, 48);
    const ModuleInfo* m = rt_module_at(A1);
    uint64_t len, heap = m ? 0 : g_pages_query(A1, &len);
    if (!m && !heap) {
        ST64(A2, a);
        ST64(A2 + 24, 0x1000);
        ST32(A2 + 32, 0x10000);
        ST32(A2 + 36, 1);
        RET(48);
        return;
    }
    uint64_t base = m ? m->base : heap, size = m ? m->size : len;
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
HLE(IsThreadAFiber) { RET(0); }
HLE(Beep) { RET(1); }
HLE(GetExitCodeProcess) { ST32(A2, 259); RET(1); }
HLE(CreateProcessW) { ST32(g_teb + 0x68, 2); RET(0); }
HLE(CreateProcessA) { ST32(g_teb + 0x68, 2); RET(0); }
HLE(RtlPcToFileHeader) { const ModuleInfo* m = rt_module_at(A1); ST64(A2, m ? m->base : 0); RET(m ? m->base : 0); }
HLE(GetProcessTimes) { ST64(A2, 0); ST64(A3, 0); ST64(A4, 0); ST64(ARG(5), host_ticks_ns() / 100); RET(1); }
HLE(GetThreadTimes) { ST64(A2, 0); ST64(A3, 0); ST64(A4, 0); ST64(ARG(5), host_ticks_ns() / 100); RET(1); }
int hle_cores(void);
HLE(GetProcessAffinityMask) { uint64_t m = (1ull << hle_cores()) - 1; ST64(A2, m); ST64(A3, m); RET(1); }
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
    uint64_t head = A1, e = A2, old = LD64(head);
    ST64(e, old);
    ST64(head, e);
    ST64(head + 8, LD64(head + 8) + 1);
    RET(old);
}
HLE(InterlockedPopEntrySList)
{
    uint64_t head = A1, e = LD64(head);
    if (e) { ST64(head, LD64(e)); ST64(head + 8, LD64(head + 8) - 1); }
    RET(e);
}
HLE(InterlockedFlushSList) { uint64_t e = LD64(A1); ST64(A1, 0); ST64(A1 + 8, 0); RET(e); }
HLE(QueryDepthSList) { RET(LD64(A1 + 8) & 0xFFFF); }
HLE(RtlInitializeSListHead) { ST64(A1, 0); ST64(A1 + 8, 0); RET(0); }

HLE(RaiseException)
{
    if ((uint32_t)A1 == 0x406D1388) {
        /* MSVC thread-name exception: { 0x1000, name, thread id, flags } */
        if (A4 && A3 >= 3 && LD32(A4) == 0x1000) {
            uint32_t tid = (uint32_t)LD64(A4 + 16);
            GThread* t = tid == 0xFFFFFFFFu ? sched_current() : sched_by_tid(tid);
            if (t) { const char* n = GSTR(LD64(A4 + 8)); snprintf(t->name, sizeof t->name, "%s", n); }
        }
        RET(0);
        return;
    }
    host_log("[seh] RaiseException code=%08llx flags=%llx from %llx", (unsigned long long)(uint32_t)A1, (unsigned long long)A2, (unsigned long long)LD64(R(4)));
    rt_exit((int)(uint32_t)A1);
}
void rt_backtrace(CPU* c, const char* tag);
void rt_sj_jump(CPU* c, uint64_t jb, uint64_t val);
HLE(RtlUnwind)
{
    /* the only unwind we see from CRT code is longjmp: STATUS_LONGJUMP with the jump buffer as the first parameter */
    if (A3 && (uint32_t)LD32(A3) == 0x80000026u) rt_sj_jump(c, LD64(A3 + 32), A4);
    host_log("[seh] RtlUnwind(frame=%llx, target=%llx, code=%x)", (unsigned long long)A1, (unsigned long long)A2, A3 ? (unsigned)LD32(A3) : 0);
    rt_backtrace(c, "unwind");
    rt_exit(3);
}
HLE(RtlUnwindEx) { host_log("[seh] RtlUnwindEx"); rt_exit(3); }
