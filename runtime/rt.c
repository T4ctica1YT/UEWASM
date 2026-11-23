#include "rt.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#if defined(_WIN32) && defined(UEW_STACKCHECK)
#include <windows.h>
#endif
#include "host.h"
#include "sched.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

FILE* host_fopen(const char* path, const char* mode);

#ifdef _MSC_VER
extern char __ImageBase;
unsigned short __stdcall RtlCaptureStackBackTrace(unsigned long, unsigned long, void**, unsigned long*);
#endif

uint8_t* MEM;
uint64_t MEM_SIZE;
static jmp_buf exit_jmp;
static int exit_code;
static uint64_t bins[32];
CPU main_cpu;

/* guest address space above the images, handed out in 64K units */
#define UNIT 65536ull
#define BIG 0xFF
static uint64_t unit_lo, unit_n, unit_rover, units_used, units_peak;
static uint64_t *unit_free, *unit_dirty;
static uint32_t* unit_run;
static uint64_t arena_cur, arena_end;

#define BIT(m, i) ((m)[(i) >> 6] >> ((i) & 63) & 1)
#define SETB(m, i) ((m)[(i) >> 6] |= 1ull << ((i) & 63))
#define CLRB(m, i) ((m)[(i) >> 6] &= ~(1ull << ((i) & 63)))

static void units_init(uint64_t lo)
{
    unit_lo = (lo + UNIT - 1) & ~(UNIT - 1);
    unit_n = (MEM_SIZE - unit_lo) / UNIT;
    size_t words = (size_t)(unit_n + 63) / 64;
    unit_free = calloc(words, 8);
    unit_dirty = calloc(words, 8);
    unit_run = calloc((size_t)unit_n, 4);
    for (uint64_t i = 0; i < unit_n; i++) SETB(unit_free, i);
    /* fake module handles for HLE dlls sit here */
    g_pages_at(0x7FF00000ull, 0x400000);
}

static uint64_t units_scan(uint64_t from, uint64_t to, uint64_t n)
{
    uint64_t run = 0;
    for (uint64_t i = from; i < to;) {
        uint64_t w = unit_free[i >> 6];
        if (!(i & 63) && i + 64 <= to && (w == 0 || w == ~0ull)) {
            run = w ? run + 64 : 0;
            i += 64;
            if (run >= n) return i - run;
            continue;
        }
        run = BIT(unit_free, i) ? run + 1 : 0;
        i++;
        if (run == n) return i - n;
    }
    return ~0ull;
}

/* a line each time guest memory in use climbs to the next 256 MB step, or falls two steps: the shape of loading peaks
   (without repeating while it hovers around one step) */
static void mem_timeline(void)
{
    static uint64_t last_step = 0;
    uint64_t step = units_used * UNIT >> 28;
    if (step <= last_step && step + 2 > last_step) return;
    last_step = step;
    host_log("[memline] %llu MB in use (most %llu MB)", (unsigned long long)(units_used * UNIT >> 20), (unsigned long long)(units_peak * UNIT >> 20));
}

static void units_take(uint64_t i, uint64_t n)
{
    for (uint64_t j = i; j < i + n; j++) {
        CLRB(unit_free, j);
        if (BIT(unit_dirty, j)) { memset(MEM + unit_lo + j * UNIT, 0, UNIT); CLRB(unit_dirty, j); }
    }
    unit_run[i] = (uint32_t)n;
    units_used += n;
    if (units_used > units_peak) units_peak = units_used;
    mem_timeline();
}

/* the highest run of n free units */
static uint64_t units_scan_down(uint64_t n)
{
    uint64_t run = 0;
    for (uint64_t i = unit_n; i-- > 0;) {
        uint64_t w = unit_free[i >> 6];
        if ((i & 63) == 63 && i >= 63 && (w == 0 || w == ~0ull)) {
            run = w ? run + 64 : 0;
            if (run >= n) return i - 63 + (run - n);
            i -= 63;
            continue;
        }
        run = BIT(unit_free, i) ? run + 1 : 0;
        if (run == n) return i;
    }
    return ~0ull;
}

/* small blocks first-fit from the bottom, big ones (4 MB and up) from the top: the small long-lived ones stay packed
   low instead of being strewn across the space, so a big contiguous block can still be found when most is in use
   (Dead as Disco ran out of room for a 48 MB block with 400 MB free) */
uint64_t g_pages(uint64_t size)
{
    uint64_t n = (size + UNIT - 1) / UNIT;
    if (!n) n = 1;
    uint64_t i;
#ifdef UEW_MEM64
    /* 64-bit memory: small blocks start above 4 GB as well (big ones come from the top already), so code that still
       keeps a guest address in 32 bits breaks on the first allocation, not deep into a game */
    uint64_t at4 = unit_lo < (4ull << 30) ? ((4ull << 30) - unit_lo) / UNIT : 0;
    if (n < 64 && at4 < unit_n) {
        i = units_scan(at4, unit_n, n);
        if (i == ~0ull) i = units_scan(0, at4, n);
    } else
#endif
    i = n >= 64 ? units_scan_down(n) : units_scan(0, unit_n, n);
    if (i == ~0ull) i = n >= 64 ? units_scan(0, unit_n, n) : units_scan_down(n);
    if (i == ~0ull) {
        host_log("[mem] out of guest memory (%llu bytes requested, %llu MB in use)", (unsigned long long)size,
                 (unsigned long long)(units_used * UNIT >> 20));
        /* once: who asked (an absurd size is usually a bad count further up) */
        static int said;
        if (!said++) {
            void rt_backtrace(CPU* c, const char* tag);
            void rt_ring_dump(void);
            CPU* cur = sched_current_cpu();
            void rt_host_chain(const char* tag);
            if (cur) rt_backtrace(cur, "mem");
            rt_host_chain("mem");
            rt_ring_dump();
        }
        return 0;
    }
    units_take(i, n);
    unit_rover = i + n;
    return unit_lo + i * UNIT;
}

uint64_t g_pages_at(uint64_t a, uint64_t size)
{
    if (a < unit_lo || (a - unit_lo) % UNIT) return 0;
    uint64_t i = (a - unit_lo) / UNIT, n = (size + UNIT - 1) / UNIT;
    if (!n || i + n > unit_n) return 0;
    for (uint64_t j = i; j < i + n; j++)
        if (!BIT(unit_free, j)) return 0;
    units_take(i, n);
    return a;
}

/* region holding a: returns its base and length, or 0 when a is not allocated */
uint64_t g_pages_query(uint64_t a, uint64_t* len)
{
    if (a < unit_lo) return 0;
    uint64_t i = (a - unit_lo) / UNIT;
    if (i >= unit_n || BIT(unit_free, i)) return 0;
    for (uint64_t j = i + 1; j-- > 0;)
        if (unit_run[j]) {
            if (j + unit_run[j] <= i) return 0;
            if (len) *len = unit_run[j] * UNIT;
            return unit_lo + j * UNIT;
        }
    return 0;
}

/* a block is zeroed when it is freed, not when its memory is next handed out: the same work, done earlier, except for
   memory that is never handed out again. WebAssembly can't give memory back, so that memory stays in the page; holding
   zeros, Windows compresses it to almost nothing when it trims the page under memory pressure, where stale data only
   halves. Rocket League frees 2.6 GB of shader-cache buffers after loading: zeroing them took ~0.2 s, and a trimmed
   page then held 1.25 GB less in Windows' compression store. ?nozerofree=1 zeroes at reuse instead */
static int zero_free;
void g_pages_free(uint64_t a)
{
    uint64_t len, base = g_pages_query(a, &len);
    if (!base || base != a) return;
    uint64_t i = (a - unit_lo) / UNIT, n = len / UNIT;
    if (zero_free) {
        memset(MEM + unit_lo + i * UNIT, 0, (size_t)(n * UNIT));
        for (uint64_t j = i; j < i + n; j++) { SETB(unit_free, j); CLRB(unit_dirty, j); }
    } else
        for (uint64_t j = i; j < i + n; j++) { SETB(unit_free, j); SETB(unit_dirty, j); }
    unit_run[i] = 0;
    units_used -= n;
    mem_timeline();
    if (i < unit_rover) unit_rover = i;
}

uint64_t g_mem_used(void) { return unit_lo + units_used * UNIT; }
/* the most guest memory ever in use: wasm memory can't give pages back, so this is what the game keeps resident */
uint64_t g_mem_peak(void) { return unit_lo + units_peak * UNIT; }

static int size_class(uint64_t n)
{
    int k = 4;
    while ((1ull << k) < n) k++;
    return k;
}

/* heap bookkeeping for the [mem] log line: bytes asked for and held by live blocks, bytes sitting in the free lists */
static uint64_t heap_req, heap_held, heap_binned, heap_big;
void g_heap_stats(uint64_t* req, uint64_t* held, uint64_t* binned, uint64_t* big) { *req = heap_req; *held = heap_held; *binned = heap_binned; *big = heap_big; }

uint64_t g_alloc(uint64_t n)
{
    if (n == 0) n = 1;
    int k = size_class(n + 16);
    uint64_t blk;
    heap_req += n;
    if (k > 15) {
        blk = g_pages(n + 16);
        if (!blk) { heap_req -= n; return 0; }
        k = BIG;
        uint64_t len; g_pages_query(blk, &len);
        heap_held += len; heap_big += len;
    } else if (bins[k]) {
        blk = bins[k];
        bins[k] = LD64(blk + 16);
        heap_binned -= 1ull << k;
        heap_held += 1ull << k;
    } else {
        heap_held += 1ull << k;
        uint64_t sz = 1ull << k;
        if (arena_cur + sz > arena_end) {
            arena_cur = g_pages(1 << 20);
            if (!arena_cur) { heap_req -= n; heap_held -= sz; return 0; }
            arena_end = arena_cur + (1 << 20);
        }
        blk = arena_cur;
        arena_cur += sz;
    }
    ST64(blk, n);
    ST64(blk + 8, (uint64_t)k);
    return blk + 16;
}

uint64_t g_size(uint64_t p) { return p ? LD64(p - 16) : 0; }

static uint64_t block_cap(uint64_t p)
{
    int k = (int)LD64(p - 8);
    if (k != BIG) return (1ull << k) - 16;
    uint64_t len;
    g_pages_query(p - 16, &len);
    return len - 16;
}

void g_free(uint64_t p)
{
    if (!p) return;
    int k = (int)LD64(p - 8);
    heap_req -= LD64(p - 16);
    if (k == BIG) { uint64_t len; if (g_pages_query(p - 16, &len)) { heap_held -= len; heap_big -= len; } g_pages_free(p - 16); return; }
    if (k < 4 || k > 15) return;
    heap_held -= 1ull << k;
    heap_binned += 1ull << k;
    ST64(p, bins[k]);
    bins[k] = p - 16;
}

uint64_t g_calloc(uint64_t n)
{
    uint64_t p = g_alloc(n);
    if (p) memset(G2H(p), 0, (size_t)n);
    return p;
}

uint64_t g_realloc(uint64_t p, uint64_t n)
{
    if (!p) return g_alloc(n);
    if (!n) { g_free(p); return 0; }
    uint64_t old = g_size(p);
    if (n <= block_cap(p)) { heap_req += n - old; ST64(p - 16, n); return p; }
    uint64_t q = g_alloc(n);
    if (q) memcpy(G2H(q), G2H(p), (size_t)(old < n ? old : n));
    g_free(p);
    return q;
}

uint64_t g_strdup(const char* s)
{
    size_t n = strlen(s) + 1;
    uint64_t p = g_alloc(n);
    memcpy(G2H(p), s, n);
    return p;
}

static FnEntry* fn_hash;
static uint64_t fn_mask;

static void build_fn_hash(void)
{
    uint64_t n = 1;
    while (n < 2 * (uint64_t)rt_fn_count) n <<= 1;
    fn_hash = calloc((size_t)n, sizeof(FnEntry));
    fn_mask = n - 1;
    for (size_t i = 0; i < rt_fn_count; i++) {
        uint64_t h = (rt_fn_table[i].addr * 0x9E3779B97F4A7C15ull) >> 40 & fn_mask;
        while (fn_hash[h].fn) h = (h + 1) & fn_mask;
        fn_hash[h] = rt_fn_table[i];
    }
}

static const FnEntry* find_fn(uint64_t t)
{
    if (fn_hash) {
        uint64_t h = (t * 0x9E3779B97F4A7C15ull) >> 40 & fn_mask;
        while (fn_hash[h].fn) {
            if (fn_hash[h].addr == t) return &fn_hash[h];
            h = (h + 1) & fn_mask;
        }
        return NULL;
    }
    size_t lo = 0, hi = rt_fn_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (rt_fn_table[mid].addr < t) lo = mid + 1;
        else hi = mid;
    }
    return (lo < rt_fn_count && rt_fn_table[lo].addr == t) ? &rt_fn_table[lo] : NULL;
}

#define DYN_BASE 0x40000ull
#define DYN_MAX 16384
static Thunk dyn[DYN_MAX];
static size_t ndyn;
#ifdef UEW_THREADS
_Thread_local int rt_hle_budget = 1024;
#else
int rt_hle_budget = 1024;
#endif
void sched_tick(CPU* c);
void rt_hle_tick(CPU* c) { rt_hle_budget = 1024; sched_tick(c); }
#include "dyn_tramp.inc"

static uint16_t dyn_pops[DYN_MAX];
#define POPS_SITE 0xFFFFu

/* games lifted before the lifter wrote these still link: no call-site counts, no Steam app id */
#if defined(__clang__) || defined(__GNUC__)
__attribute__((weak)) const uint32_t rt_call_args[2] = { 0, 0 };
__attribute__((weak)) const size_t rt_call_arg_count = 0;
__attribute__((weak)) const uint32_t rt_steam_appid = 0;
#endif

/* what the call site at this return address pushed (rt_call_args is sorted by return address); 0 if it isn't known */
static unsigned site_pops(uint32_t ret)
{
    size_t lo = 0, hi = rt_call_arg_count;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (rt_call_args[2 * m] < ret) lo = m + 1;
        else hi = m;
    }
    if (lo < rt_call_arg_count && rt_call_args[2 * lo] == ret) return rt_call_args[2 * lo + 1];
    static int said;
    if (said++ < 20) host_log("[call] no argument size known for the call returning to %x; taken as 0", ret);
    return 0;
}

uint64_t rt_dyn_thunk_n(GuestFn fn, const char* name, int nargs)
{
    for (size_t i = 0; i < ndyn; i++)
        if (dyn[i].fn == fn && dyn_pops[i] == (nargs < 0 ? POPS_SITE : 4 * nargs)) return DYN_BASE + 8ull * i;
    if (ndyn == DYN_MAX) return 0;
    dyn[ndyn].fn = fn;
    dyn[ndyn].name = name;
    dyn[ndyn].dll = "dyn";
    dyn_pops[ndyn] = nargs < 0 ? POPS_SITE : (uint16_t)(4 * nargs);
    return DYN_BASE + 8ull * ndyn++;
}

uint64_t rt_dyn_thunk(GuestFn fn, const char* name) { return rt_dyn_thunk_n(fn, name, 0); }

/* a 32-bit guest calling the runtime: the stack arguments are laid out as a Win64 frame (rcx, rdx, r8, r9, then the
   stack slots) so every HLE function serves both; small negative values (handles like -1) widen with their sign */
/* ?watch=<hex address>: a guest dword checked after every system call of a 32-bit game; a change is logged with the
   call that made it (or, for a change by the game's own code, the next call after it) */
static uint64_t watch_addr = ~0ull;
static uint32_t watch_val;
static void watch_check(const char* name)
{
    if (watch_addr == ~0ull) {
        watch_addr = (uint64_t)host_query_hex("watch");
        if (watch_addr) { watch_val = (uint32_t)LD32(watch_addr); host_log("[watch] %llx = %x", (unsigned long long)watch_addr, watch_val); }
    }
    if (!watch_addr) return;
    static int64_t want = -2;
    if (want == -2) want = host_query_hex("watchval") ? (int64_t)host_query_hex("watchval") : -1;
    uint32_t v = (uint32_t)LD32(watch_addr);
    if (v != watch_val && (want < 0 || v == (uint32_t)want)) {
        host_log("[watch] %llx: %x -> %x after %s (thread %u)", (unsigned long long)watch_addr, watch_val, v, name, sched_current_id());
        watch_val = v;
    } else watch_val = v;
}

void rt_ring_dump(void);
#define ring_dump rt_ring_dump
void rt_hle32(CPU* c, const Thunk* e, unsigned pops)
{
    uint64_t esp = R(4), f = (esp - 0x200) & ~15ull;
    /* x86 code keeps the stack 4-byte aligned; the first call that sees otherwise points at what broke it */
    static int misaligned;
    if ((esp & 3) && !misaligned++) host_log("[stack] esp %llx not aligned at %s, called from %llx (thread %u)", (unsigned long long)esp,
                                              e->name, (unsigned long long)LD32(esp), sched_current_id());
    if ((esp & 3) && misaligned == 1) ring_dump();
    /* and stays inside the thread's own stack (the TEB's StackLimit..StackBase): the first call made from anywhere else
       (esp loaded from something that was never a stack address) is logged with the calls before it */
    static int strayed;
    if (!strayed && c->gsbase) {
        uint64_t hi = LD32(c->gsbase + 4), lo = LD32(c->gsbase + 8);
        if (hi > lo && (esp < lo || esp > hi) && !strayed++) {
            host_log("[stack] esp %llx outside the stack of thread %u (%llx-%llx) at %s, called from %llx, ebp %llx",
                     (unsigned long long)esp, sched_current_id(), (unsigned long long)lo, (unsigned long long)hi, e->name,
                     (unsigned long long)LD32(esp), (unsigned long long)R(5));
            ring_dump();
        }
    }
    uint64_t keep[4] = { R(3), R(5), R(6), R(7) };
    ST64(f, LD32(esp));
    for (unsigned i = 1; i <= 20; i++) {
        uint64_t v = LD32(esp + 4ull * i);
        if (v >= 0xFFFF0000ull) v |= 0xFFFFFFFF00000000ull;
        ST64(f + 8ull * i, v);
    }
    ST64(f + 8ull * 21, esp);   /* the guest esp at the call, for handlers that need it (RaiseException, RtlUnwind) */
    ST64(f + 8ull * 22, (uint32_t)R(1));   /* ecx: a thiscall method's this (HLE32_ECX) */
    R(1) = LD64(f + 8); R(2) = LD64(f + 16); R(8) = LD64(f + 24); R(9) = LD64(f + 32);
    R(4) = f;
    if (e->fn) e->fn(c);
    else hle_missing(c, e);
    watch_check(e->name);
    R(3) = keep[0]; R(5) = keep[1]; R(6) = keep[2]; R(7) = keep[3];
    R(2) = (uint32_t)(R(0) >> 32);
    R(0) = (uint32_t)R(0);
    R(4) = esp + 4 + pops;
}

uint64_t g_alloc_zero_aligned(uint64_t n, uint64_t align)
{
    uint64_t p = g_calloc(n + align);
    return (p + align - 1) & ~(align - 1);
}

void rt_backtrace(CPU* c, const char* tag);
#if defined(_WIN32) && defined(UEW_STACKCHECK)
static void stack_check(CPU* c, uint64_t t)
{
    ULONG_PTR lo, hi;
    GetCurrentThreadStackLimits(&lo, &hi);
    static int seen;
    static uint64_t last[32];
    last[seen++ & 31] = t;
    if ((ULONG_PTR)&lo - lo < (256u << 10)) {
        host_log("[stack] host stack nearly exhausted (%llu KB used); recent targets:", (unsigned long long)((hi - (ULONG_PTR)&lo) >> 10));
        for (int i = 0; i < 32; i++) host_log("  %llx", (unsigned long long)last[(seen + i) & 31]);
        rt_backtrace(c, "stack");
        host_flush();
        rt_exit(99);
    }
}
#define STACK_CHECK(c, t) stack_check(c, t)
#else
#define STACK_CHECK(c, t)
#endif
/* HLE call log: UEW_TRACE at start, or switched on and off from the page (Module._uew_set_trace) */
int rt_trace_on = -1;
#define trace_on rt_trace_on
#if defined(__EMSCRIPTEN__) && defined(UEW_DEBUG_TOOLS)
__attribute__((used, visibility("default")))
#endif
void uew_set_trace(int on) { trace_on = on; }
/* one traced call: name, the first four arguments, the guest return address and the green thread */
const char* host_trace_filter(void);
/* ?tracering=1: the last calls are kept quietly and printed when the game traps */
static struct { const char* name; uint64_t a[4], ret, bp; unsigned tid; } ring[64];
static unsigned ring_n;
static int ring_on = -1;
/* also the debug panel's thread dump, to show what a busy thread keeps calling */
#ifndef UEW_EBPCHECK
void rt_ebp_dump_all(void) {}
#endif
void rt_ring_dump(void)
{
    void rt_ebp_dump_all(void);
    rt_ebp_dump_all();
    if (ring_on <= 0) return;
    /* one snapshot: with real threads the other threads keep adding calls faster than they can be printed, and a bound
       read on every pass never ends (Rocket League's audio thread printed for minutes after a trap) */
    unsigned end = *(volatile unsigned*)&ring_n;
    host_log("[trap] last %u system calls:", end < 64 ? end : 64);
    for (unsigned i = end > 64 ? end - 64 : 0; i < end; i++) {
        unsigned k = i & 63;
        host_log("  %s(%llx, %llx, %llx, %llx) ret=%llx rbp=%llx t=%u", ring[k].name, (unsigned long long)ring[k].a[0], (unsigned long long)ring[k].a[1],
                 (unsigned long long)ring[k].a[2], (unsigned long long)ring[k].a[3], (unsigned long long)ring[k].ret, (unsigned long long)ring[k].bp, ring[k].tid);
    }
}

/* 32-bit games: the arguments are on the stack; with ?tracering=1 they go to the ring, otherwise every call is logged */
static void trace_call32(CPU* c, const char* name)
{
    if (ring_on < 0) ring_on = host_query_flag("tracering");
    if (ring_on) {
        unsigned k = ring_n++ & 63;
        ring[k].name = name;
        for (int i = 0; i < 4; i++) ring[k].a[i] = LD32(R(4) + 4ull * (i + 1));
        ring[k].ret = LD32(R(4)); ring[k].bp = R(5); ring[k].tid = sched_current_id();
        return;
    }
    /* the same ",Name,Name," filter as 64-bit games (UEW_TRACE_ONLY / window.uewTraceOnly) */
    static const char* only;
    if (!only) only = host_trace_filter();
    if (only[0]) {
        char k[96];
        snprintf(k, sizeof k, ",%s,", name);
        if (!strstr(only, k)) return;
    }
    host_log("[hle] %s(%x, %x, %x, %x) ret=%x", name, (unsigned)LD32(R(4) + 4), (unsigned)LD32(R(4) + 8),
             (unsigned)LD32(R(4) + 12), (unsigned)LD32(R(4) + 16), (unsigned)LD32(R(4)));
}

void rt_trace_call(CPU* c, const char* name)
{
    if (ring_on < 0) ring_on = host_query_flag("tracering");
    if (ring_on) {
        unsigned k = ring_n++ & 63;
        ring[k].name = name; ring[k].a[0] = R(1); ring[k].a[1] = R(2); ring[k].a[2] = R(8); ring[k].a[3] = R(9);
        ring[k].ret = LD64(R(4)); ring[k].bp = R(5); ring[k].tid = sched_current_id();
        return;
    }
    /* optional ",Name,Name," list (UEW_TRACE_ONLY / window.uewTraceOnly) keeps the log to the calls of interest */
    static const char* only;
    if (!only) only = host_trace_filter();
    if (only[0]) {
        char k[96];
        snprintf(k, sizeof k, ",%s,", name);
        if (!strstr(only, k)) return;
    }
    host_log("[hle] %s(%llx, %llx, %llx, %llx) ret=%llx t=%u", name, (unsigned long long)R(1), (unsigned long long)R(2),
             (unsigned long long)R(8), (unsigned long long)R(9), (unsigned long long)LD64(R(4)), sched_current_id());
}

static void sj_jump32(CPU* c, uint64_t t);
#ifdef UEW_THREADS
/* system calls that only touch the calling thread's own state or the clock skip the global lock (they are the most
   frequent ones: UE reads its thread-local slots and the clock all the time). 2: only for the first 64 TLS slots */
static uint8_t* nolock;
static void nolock_init(void)
{
    static const char* const names[] = { "TlsGetValue", "FlsGetValue", "GetCurrentThreadId", "GetCurrentThread", "GetCurrentProcess",
        "GetCurrentProcessId", "QueryPerformanceCounter", "QueryPerformanceFrequency", "GetSystemTimeAsFileTime",
        "GetSystemTimePreciseAsFileTime", "GetTickCount", "GetTickCount64", "GetLastError", "SetLastError",
        "GetCurrentProcessorNumber", "GetCurrentProcessorNumberEx", "EnterCriticalSection", "LeaveCriticalSection",
        "TryEnterCriticalSection", "SwitchToThread", "AcquireSRWLockExclusive", "ReleaseSRWLockExclusive", "AcquireSRWLockShared",
        "ReleaseSRWLockShared", "TryAcquireSRWLockExclusive", "TryAcquireSRWLockShared", "timeGetTime", "QueryUnbiasedInterruptTime",
        "GetSystemMetrics", "IsIconic", "GetForegroundWindow", "GetActiveWindow", "GetFocus", "GetDesktopWindow" };
    uint8_t* m = calloc(rt_thunk_count + 1, 1);
    for (size_t k = 0; k < rt_thunk_count; k++) {
        if (!rt_thunks[k].fn) continue;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) if (!strcmp(rt_thunks[k].name, names[i])) m[k] = 1;
        if (!strcmp(rt_thunks[k].name, "TlsSetValue") || !strcmp(rt_thunks[k].name, "FlsSetValue")) m[k] = 2;
        /* the C runtime's pure functions: maths, and memory/string routines (bar the ones with hidden state) */
        const char* n = rt_thunks[k].name;
        static const char* const maths[] = { "pow", "log", "log10", "log2", "exp", "exp2", "trunc", "floor", "ceil", "sqrt", "sin", "cos",
            "tan", "asin", "acos", "atan", "atan2", "fmod", "round", "lround", "fabs", "hypot", "cbrt", "modf", "frexp", "ldexp", "sinh",
            "cosh", "tanh", "nearbyint", "rint", "lrint", "llrint", "llround", "remainder", "copysign", "fmin", "fmax", "log1p", "expm1" };
        size_t nl = strlen(n);
        for (size_t i = 0; i < sizeof maths / sizeof *maths; i++) {
            size_t ml = strlen(maths[i]);
            if (!strncmp(n, maths[i], ml) && (nl == ml || (nl == ml + 1 && n[ml] == 'f'))) m[k] = 1;
        }
        if (strstr(rt_thunks[k].dll, "crt-math") || ((!strncmp(n, "mem", 3) || !strncmp(n, "str", 3) || !strncmp(n, "wcs", 3)) &&
            !strstr(n, "tok") && strcmp(n, "strerror") && strcmp(n, "strdup") && strcmp(n, "wcsdup") && strncmp(n, "str_", 4)))
            m[k] = 1;
    }
    nolock = m;
}
#define THUNK_LOCKS(k) (!(nolock[k] == 1 || (nolock[k] == 2 && R(1) < 64)))
/* ?lockstats=1: which system calls take the lock most, printed with each hitch report */
static uint32_t* lock_calls;
static int lock_stats = -1;
void rt_lock_report(void)
{
    if (lock_stats <= 0 || !lock_calls) return;
    char line[600];
    int n = snprintf(line, sizeof line, "[lock] most locked calls:");
    for (int r = 0; r < 12; r++) {
        size_t best = 0;
        for (size_t k = 1; k < rt_thunk_count; k++) if (lock_calls[k] > lock_calls[best]) best = k;
        if (!lock_calls[best]) break;
        n += snprintf(line + n, sizeof line - n, " %s %u,", rt_thunks[best].name, lock_calls[best]);
        lock_calls[best] = 0;
        if (n >= (int)sizeof line - 40) break;
    }
    memset(lock_calls, 0, rt_thunk_count * sizeof *lock_calls);
    host_log("%s", line);
}
#else
#define THUNK_LOCKS(k) 1
#endif
void rt_call(CPU* c, uint64_t t)
{
    STACK_CHECK(c, t);
    if (t >= DYN_BASE && t < DYN_BASE + 8ull * ndyn) {
        size_t k = (t - DYN_BASE) / 8;
        rt_lock();
        if (trace_on < 0) trace_on = getenv("UEW_TRACE") != NULL || host_query_flag("tracering");
        if (RT_G32 && trace_on) trace_call32(c, dyn[k].name);
        if (RT_G32) rt_hle32(c, &dyn[k], dyn_pops[k] == POPS_SITE ? site_pops((uint32_t)LD32(R(4))) : dyn_pops[k]);
        else {
            dyn[k].fn(c);
            R(4) += 8;
        }
        if (--rt_hle_budget < 0) rt_hle_tick(c);
        rt_unlock();
        return;
    }
    if (t >= THUNK_BASE && t < THUNK_BASE + 8ull * rt_thunk_count) {
        size_t k = (t - THUNK_BASE) / 8;
        const Thunk* e = &rt_thunks[k];
#ifdef UEW_THREADS
        if (!nolock) nolock_init();
#endif
        int locks = THUNK_LOCKS(k);
        if (locks) {
            rt_lock();
#ifdef UEW_THREADS
            if (lock_stats < 0) { lock_stats = host_query_flag("lockstats"); lock_calls = calloc(rt_thunk_count + 1, 4); }
            if (lock_stats) lock_calls[k]++;
#endif
        }
        if (trace_on < 0) trace_on = getenv("UEW_TRACE") != NULL || host_query_flag("tracering");
        if (RT_G32) {
            if (trace_on) trace_call32(c, e->name);
            rt_hle32(c, e, rt_thunk_pops[k]);
        } else {
            if (trace_on) rt_trace_call(c, e->name);
            if (e->fn) e->fn(c);
            else hle_missing(c, e);
            R(4) += 8;
        }
        if (locks) {
            if (--rt_hle_budget < 0) rt_hle_tick(c);
            rt_unlock();
        } else if (--rt_hle_budget < 0) { rt_lock(); rt_hle_tick(c); rt_unlock(); }
        return;
    }
    const FnEntry* f = find_fn(t);
    if (!f && RT_G32) sj_jump32(c, t);
    if (!f) {
        host_log("[trap] call to %llx from %llx (thread %u)", (unsigned long long)t, (unsigned long long)LDP(R(4)), sched_current_id());
        /* a background thread that jumps into the void (a corrupt callback in telemetry, say) ends alone, as an
           unhandled fault there would, instead of taking the game down */
        if (!sched_is_main()) { host_log("[trap] thread %u ended", sched_current_id()); sched_exit_current(c, 0xC0000005); return; }
        rt_trap(c, t, "call to unknown address");
        return;
    }
    f->fn(c);
}

void rt_jump(CPU* c, uint64_t t) { rt_call(c, t); }

/* targets outside the lifted image (thunks, dynamic stubs) go through rt_call */
#ifdef _MSC_VER
static __declspec(thread) uint64_t jump_pending;
#else
static _Thread_local uint64_t jump_pending;
#endif
static void jump_slow(CPU* c) { rt_call(c, jump_pending); }

/* lifted function at a guest address, or NULL. Indirect calls that miss their call site's cache (virtual calls with
   many targets) come here constantly; a small direct-mapped cache in front of the big table keeps them in the CPU's cache.
   One host thread only: with real threads an entry could be read half-written */
static GuestFn lookup_fn(uint64_t t)
{
    if (t < IMAGE_LO || t >= IMAGE_HI) return NULL;
#ifndef UEW_THREADS
    static struct { uint64_t a; GuestFn f; } jc[65536];
    size_t k = (size_t)((t * 0x9E3779B97F4A7C15ull) >> 48);
    if (jc[k].a == t) return jc[k].f;
    const FnEntry* e = find_fn(t);
    if (e) { jc[k].a = t; jc[k].f = e->fn; }
    return e ? e->fn : NULL;
#else
    const FnEntry* e = find_fn(t);
    return e ? e->fn : NULL;
#endif
}

static void eh_check(CPU* c, uint64_t target);
GuestFn rt_jump_fn(CPU* c, uint64_t t)
{
    STACK_CHECK(c, t);
    if (RT_G32) eh_check(c, t);
    GuestFn f = lookup_fn(t);
    if (f) return f;
    jump_pending = t;
    return jump_slow;
}

/* a call site's cache: with real threads it is filled once (claimed, then the target published after the function),
   so a reader that sees the address always sees its own function */
static void ic_fill(IC* ic, uint64_t t, GuestFn f)
{
#ifdef UEW_THREADS
    /* filled once: claim the empty cache (IC_INIT) first, so no reader sees a target with another site's function */
    uint64_t z = ~0ull;
    if (!__atomic_compare_exchange_n((uint64_t*)&ic->a, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    ic->f = f;
    __atomic_store_n((uint64_t*)&ic->a, t, __ATOMIC_RELEASE);
#else
    ic->f = f;
    ic->a = t;
#endif
}

GuestFn rt_ic_fn(CPU* c, uint64_t t, IC* ic)
{
    GuestFn f = rt_jump_fn(c, t);
    if (f != jump_slow) ic_fill(ic, t, f);
    return f;
}

void rt_call_ic(CPU* c, uint64_t t, IC* ic)
{
    STACK_CHECK(c, t);
    GuestFn f = lookup_fn(t);
    if (f) {
        ic_fill(ic, t, f);
        f(c);
        return;
    }
#ifndef UEW_THREADS
    /* system calls straight from the call site (with real threads they go through rt_call, which takes the lock) */
    if (!RT_G32 && t >= DYN_BASE && t < DYN_BASE + 8ull * ndyn && (t - DYN_BASE) / 8 < DYN_TRAMPS)
        ic_fill(ic, t, dyn_tramp[(t - DYN_BASE) / 8]);
    /* imported functions: cache the wrapper unless calls are being logged */
    if (!RT_G32 && t >= THUNK_BASE && t < THUNK_BASE + 8ull * rt_thunk_count && rt_thunks[(t - THUNK_BASE) / 8].call)
        ic_fill(ic, t, rt_thunks[(t - THUNK_BASE) / 8].call);
#endif
    rt_call(c, t);
}

/* 32-bit: arguments pushed right to left; the stack is put back afterwards, which suits stdcall and cdecl alike */
static uint64_t invoke32(CPU* c, uint64_t fn, int n, const uint64_t* a)
{
    uint64_t save[16];
    memcpy(save, c->r, sizeof save);
    uint64_t sp = (R(4) - 64) & ~15ull;
    for (int i = n; i-- > 0;) { sp -= 4; ST32(sp, a[i]); }
    sp -= 4;
    ST32(sp, RET_SENTINEL);
    R(4) = sp;
    int lockd = rt_unlock_all();
    rt_call(c, fn);
    rt_relock(lockd);
    uint64_t ret = (uint32_t)R(0);
    static const int keep[] = { 3, 5, 6, 7 };
    for (int i = 0; i < 4; i++) c->r[keep[i]] = save[keep[i]];
    R(4) = save[4];
    return ret;
}

uint64_t rt_invoke(CPU* c, uint64_t fn, int n, const uint64_t* a)
{
    if (RT_G32) return invoke32(c, fn, n, a);
    static const int regs[4] = { 1, 2, 8, 9 };
    uint64_t save_sp = R(4);
    uint64_t save[16];
    memcpy(save, c->r, sizeof save);
    int slots = n > 4 ? n : 4;
    uint64_t sp = (R(4) - 8ull * slots - 64) & ~15ull;
    for (int i = 0; i < n; i++) {
        if (i < 4) R(regs[i]) = a[i];
        else ST64(sp + 8ull * i, a[i]);
    }
    for (int i = 0; i < n && i < 4; i++) c->x[i].q[0] = a[i];
    sp -= 8;
    ST64(sp, RET_SENTINEL);
    R(4) = sp;
    int lockd = rt_unlock_all();
    rt_call(c, fn);
    rt_relock(lockd);
    uint64_t ret = R(0);
    static const int keep[] = { 3, 5, 6, 7, 12, 13, 14, 15 };
    for (int i = 0; i < 8; i++) c->r[keep[i]] = save[keep[i]];
    R(4) = save_sp;
    return ret;
}

void rt_detach_all(CPU* c)
{
    static int done;
    if (done) return;
    done = 1;
    for (size_t i = rt_module_count; i-- > 0;) {
        const ModuleInfo* m = &rt_modules[i];
        if (m->is_dll && m->entry_rva) {
            uint64_t a[3] = { m->base, 0, 1 };
            rt_invoke(c, m->base + m->entry_rva, 3, a);
        }
    }
}

void rt_exit(int code)
{
#ifdef UEW_THREADS
    /* any thread may end the process; the other cores' stacks can't be unwound to rt_main. The page's thread ends it
       (sched_mt.c: Emscripten's exit() from another core can leave the process running) */
    void sched_exit_process(int code);
    host_flush();
    sched_exit_process(code);
#else
    exit_code = code;
    longjmp(exit_jmp, 1);
#endif
}

/* the WebAssembly call stack: with function names in the build (names=True), every recompiled frame is F_<guest
   address>, so this is the guest call chain that led here (tail calls leave no frame) */
void rt_host_chain(const char* tag)
{
#ifdef __EMSCRIPTEN__
    static char st[16384];
    emscripten_get_callstack(EM_LOG_C_STACK | EM_LOG_NO_PATHS, st, sizeof st);
    char line[2048];
    int n = 0;
    for (char* s = st; *s && n < (int)sizeof line - 64;) {
        char* e = strchr(s, 10);   /* end of line */
        if (e) *e = 0;
        char* at = strstr(s, "at ");
        if (at) {
            at += 3;
            char* sp = strpbrk(at, " (");
            int len = sp ? (int)(sp - at) : (int)strlen(at);
            if (len > 40) len = 40;
            if (strncmp(at, "rt_trap", 7) && strncmp(at, "emscripten_get_callstack", 24))
                n += snprintf(line + n, sizeof line - n, " %.*s", len, at);
        }
        if (!e) break;
        s = e + 1;
    }
    host_log("[%s] call chain:%s", tag, line);

#else
    (void)tag;
#endif
}

void rt_trap(CPU* c, uint64_t pc, const char* what)
{
    ring_dump();
    rt_backtrace(c, "trap");
    host_log("[trap] %s at %llx (rsp=%llx rax=%llx rcx=%llx)", what, (unsigned long long)pc,
             (unsigned long long)R(4), (unsigned long long)R(0), (unsigned long long)R(1));
    host_log("[trap] rdx=%llx rbx=%llx rbp=%llx rsi=%llx rdi=%llx", (unsigned long long)R(2), (unsigned long long)R(3),
             (unsigned long long)R(5), (unsigned long long)R(6), (unsigned long long)R(7));
#ifdef _MSC_VER
    void* fr[24];
    char buf[24 * 12];
    int k = RtlCaptureStackBackTrace(1, 24, fr, NULL), n = 0;
    for (int i = 0; i < k; i++)
        n += snprintf(buf + n, sizeof(buf) - n, " %llx", (unsigned long long)((char*)fr[i] - &__ImageBase));
    host_log("[trap] host stack:%s", buf);
#elif defined(__EMSCRIPTEN__)
    rt_host_chain("trap");
#endif
    rt_exit(0xC0000005);
}

/* FXSAVE area: x87 control/status at +0/+2, MXCSR at +24, ST0-7 at +32, XMM0-15 at +160 */
void rt_fxsave(CPU* c, uint64_t a)
{
    memset(G2H(a), 0, 512);
    ST16(a, c->fcw);
    ST16(a + 2, fstsw(c));
    ST32(a + 24, c->mxcsr);
    ST32(a + 28, 0xFFFF);
    for (int i = 0; i < 8; i++) {
        uint64_t m; uint16_t e;
        F80S(c->st[(c->ftop + i) & 7], &m, &e);
        ST64(a + 32 + 16ull * i, m);
        ST16(a + 40 + 16ull * i, e);
    }
    for (int i = 0; i < 16; i++) memcpy(G2H(a + 160 + 16ull * i), &c->x[i], 16);
}
void rt_fxrstor(CPU* c, uint64_t a)
{
    c->fcw = (uint32_t)LD16(a);
    uint32_t sw = (uint32_t)LD16(a + 2);
    c->ftop = (sw >> 11) & 7;
    c->fsw = sw & ~0x3800u;
    c->mxcsr = (uint32_t)LD32(a + 24);
    for (int i = 0; i < 8; i++) c->st[(c->ftop + i) & 7] = F80B(LD64(a + 32 + 16ull * i), LD16(a + 40 + 16ull * i));
    for (int i = 0; i < 16; i++) memcpy(&c->x[i], G2H(a + 160 + 16ull * i), 16);
}

/* 32-bit protected-mode environment (28 bytes) and FSAVE image (environment then ST0-7 as 80-bit) */
void rt_fnstenv(CPU* c, uint64_t a)
{
    memset(G2H(a), 0, 28);
    ST32(a, c->fcw | 0xFFFF0000u);
    ST32(a + 4, fstsw(c) | 0xFFFF0000u);
    ST32(a + 8, 0xFFFFFFFFu);
}
void rt_fldenv(CPU* c, uint64_t a)
{
    c->fcw = (uint32_t)LD16(a);
    uint32_t sw = (uint32_t)LD16(a + 4);
    c->ftop = (sw >> 11) & 7;
    c->fsw = sw & ~0x3800u;
}
void rt_fnsave(CPU* c, uint64_t a)
{
    rt_fnstenv(c, a);
    ST32(a + 8, 0);
    for (int i = 0; i < 8; i++) {
        uint64_t m; uint16_t e;
        F80S(c->st[(c->ftop + i) & 7], &m, &e);
        ST64(a + 28 + 10ull * i, m);
        ST16(a + 36 + 10ull * i, e);
    }
    c->ftop = 0; c->fsw = 0; c->fcw = 0x27F;
}
void rt_frstor(CPU* c, uint64_t a)
{
    rt_fldenv(c, a);
    for (int i = 0; i < 8; i++) c->st[(c->ftop + i) & 7] = F80B(LD64(a + 28 + 10ull * i), LD16(a + 36 + 10ull * i));
}

void rt_cpuid(CPU* c)
{
    uint32_t leaf = (uint32_t)R(0), sub = (uint32_t)R(1);
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    (void)sub;
    switch (leaf) {
    case 0: a = 7; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;
    case 1: a = 0x000906EA; b = 0x00100800; cc = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23); d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26);
        /* 32-bit games probe MMX by running an MMX instruction under SEH; the lifter has no MMX registers, so the CPU
           reports none and those games keep to their SSE and integer paths */
        if (RT_G32) d &= ~(1u << 23);
        break;
    case 0x80000000: a = 0x80000008; break;
    case 0x80000001: cc = 1u << 5; d = (1u << 29); break;
    case 0x80000002: a = 0x4D534157; b = 0x50432034; cc = 0x00000055; break;
    case 0x80000006: cc = (256u << 16) | (6u << 12) | 64; break;
    case 0x80000008: a = 0x3027; break;
    default: break;
    }
    R(0) = a; R(3) = b; R(1) = cc; R(2) = d;
}

void rt_xgetbv(CPU* c) { R(0) = 3; R(2) = 0; }

/* SSE4.2 CRC32: CRC-32C (Castagnoli), reflected, no inversion */
uint64_t rt_crc32(uint64_t crc, uint64_t v, int bytes)
{
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t r = i;
            for (int k = 0; k < 8; k++) r = r & 1 ? (r >> 1) ^ 0x82F63B78u : r >> 1;
            t[i] = r;
        }
    uint32_t r = (uint32_t)crc;
    for (int i = 0; i < bytes; i++) r = t[(r ^ (uint8_t)(v >> (8 * i))) & 0xFF] ^ (r >> 8);
    return r;
}

/* PCMPESTRI/PCMPISTRI/PCMPESTRM/PCMPISTRM: returns the index (I forms); M forms write xmm0 */
uint64_t rt_pcmpstr(Flags* f, XMM* x0, XMM a, XMM b, int imm, int64_t la, int64_t lb, int expl, int mask)
{
    int words = imm & 1, sgn = imm & 2, agg = (imm >> 2) & 3, pol = (imm >> 4) & 3, n = words ? 8 : 16;
    int32_t ea[16], eb[16];
    for (int i = 0; i < n; i++) {
        ea[i] = words ? (sgn ? (int32_t)(int16_t)a.w[i] : a.w[i]) : (sgn ? (int32_t)(int8_t)a.b[i] : a.b[i]);
        eb[i] = words ? (sgn ? (int32_t)(int16_t)b.w[i] : b.w[i]) : (sgn ? (int32_t)(int8_t)b.b[i] : b.b[i]);
    }
    int na, nb;
    if (expl) {
        int64_t x = la < 0 ? -la : la, y = lb < 0 ? -lb : lb;
        na = x > n ? n : (int)x;
        nb = y > n ? n : (int)y;
    } else {
        na = n; nb = n;
        for (int i = 0; i < n; i++) if (!ea[i]) { na = i; break; }
        for (int i = 0; i < n; i++) if (!eb[i]) { nb = i; break; }
    }
    uint32_t res = 0;
    for (int j = 0; j < n; j++) {
        int bit = 0;
        if (agg == 0) {                 /* equal any */
            if (j < nb) for (int i = 0; i < na && !bit; i++) bit = ea[i] == eb[j];
        } else if (agg == 1) {          /* ranges */
            if (j < nb) for (int i = 0; i + 1 < na && !bit; i += 2) bit = eb[j] >= ea[i] && eb[j] <= ea[i + 1];
        } else if (agg == 2) {          /* equal each */
            int va = j < na, vb = j < nb;
            bit = va && vb ? ea[j] == eb[j] : !va && !vb;
        } else {                        /* equal ordered */
            bit = 1;
            for (int k = 0; k < n - j && bit; k++) {
                if (k >= na) break;
                if (j + k >= nb) { bit = 0; break; }
                bit = ea[k] == eb[j + k];
            }
        }
        res |= (uint32_t)bit << j;
    }
    uint32_t all = n == 16 ? 0xFFFFu : 0xFFu, valid = nb >= 32 ? all : ((1u << nb) - 1) & all;
    if (pol == 1) res = ~res & all;
    else if (pol == 3) res = (res ^ valid) & all;
    rt_setflags(f, 2 | (res ? 1 : 0) | (nb < n ? 0x40 : 0) | (na < n ? 0x80 : 0) | ((res & 1) ? 0x800 : 0));
    if (mask) {
        XMM m;
        memset(&m, 0, sizeof m);
        if (imm & 0x40) { for (int j = 0; j < n; j++) if (res >> j & 1) { if (words) m.w[j] = 0xFFFF; else m.b[j] = 0xFF; } }
        else m.d[0] = res;
        *x0 = m;
        return 0;
    }
    if (!res) return (uint64_t)n;
    if (imm & 0x40) { int k = 31; while (!(res >> k & 1)) k--; return (uint64_t)k; }
    int k = 0;
    while (!(res >> k & 1)) k++;
    return (uint64_t)k;
}

void rt_rdtsc(CPU* c)
{
    static uint64_t last;
    uint64_t t = host_ticks_ns();
    if (t <= last) t = last + 1;   /* strictly increasing, like the hardware counter */
    last = t;
    R(0) = (uint32_t)t;
    R(2) = t >> 32;
}

uint64_t rt_div_ip;
#define DIV_ERR() rt_trap(c, rt_div_ip, "integer divide error")
void rt_div8(CPU* c, uint64_t s) { uint16_t n = (uint16_t)R(0); if (!(uint8_t)s) { DIV_ERR(); return; } uint16_t q = n / (uint8_t)s, r = n % (uint8_t)s; R(0) = (R(0) & ~0xFFFFull) | (q & 0xFF) | ((uint64_t)(r & 0xFF) << 8); }
void rt_div16(CPU* c, uint64_t s) { uint32_t n = ((uint32_t)(uint16_t)R(2) << 16) | (uint16_t)R(0); if (!(uint16_t)s) { DIV_ERR(); return; } R(0) = (R(0) & ~0xFFFFull) | (uint16_t)(n / (uint16_t)s); R(2) = (R(2) & ~0xFFFFull) | (uint16_t)(n % (uint16_t)s); }
void rt_div32(CPU* c, uint64_t s) { uint64_t n = ((uint64_t)(uint32_t)R(2) << 32) | (uint32_t)R(0); if (!(uint32_t)s) { DIV_ERR(); return; } R(0) = (uint32_t)(n / (uint32_t)s); R(2) = (uint32_t)(n % (uint32_t)s); }
void rt_idiv8(CPU* c, uint64_t s) { int16_t n = (int16_t)R(0); if (!(int8_t)s) { DIV_ERR(); return; } int q = n / (int8_t)s, r = n % (int8_t)s; R(0) = (R(0) & ~0xFFFFull) | (uint8_t)q | ((uint64_t)(uint8_t)r << 8); }
void rt_idiv16(CPU* c, uint64_t s) { int32_t n = (int32_t)(((uint32_t)(uint16_t)R(2) << 16) | (uint16_t)R(0)); if (!(int16_t)s) { DIV_ERR(); return; } R(0) = (R(0) & ~0xFFFFull) | (uint16_t)(n / (int16_t)s); R(2) = (R(2) & ~0xFFFFull) | (uint16_t)(n % (int16_t)s); }
void rt_idiv32(CPU* c, uint64_t s) { int64_t n = (int64_t)(((uint64_t)(uint32_t)R(2) << 32) | (uint32_t)R(0)); if (!(int32_t)s) { DIV_ERR(); return; } R(0) = (uint32_t)(int32_t)(n / (int32_t)s); R(2) = (uint32_t)(int32_t)(n % (int32_t)s); }

#ifdef _MSC_VER
void rt_div64(CPU* c, uint64_t s) { uint64_t r; if (!s) { DIV_ERR(); return; } R(0) = _udiv128(R(2), R(0), s, &r); R(2) = r; }
void rt_idiv64(CPU* c, uint64_t s) { int64_t r; if (!s) { DIV_ERR(); return; } R(0) = (uint64_t)_div128((int64_t)R(2), (int64_t)R(0), (int64_t)s, &r); R(2) = (uint64_t)r; }
#else
void rt_div64(CPU* c, uint64_t s) { if (!s) { DIV_ERR(); return; } unsigned __int128 n = ((unsigned __int128)R(2) << 64) | R(0); R(0) = (uint64_t)(n / s); R(2) = (uint64_t)(n % s); }
void rt_idiv64(CPU* c, uint64_t s) { if (!s) { DIV_ERR(); return; } __int128 n = (__int128)(((unsigned __int128)R(2) << 64) | R(0)); R(0) = (uint64_t)(n / (int64_t)s); R(2) = (uint64_t)(n % (int64_t)s); }
#endif

#define F (*f)
/* a locked one already holds its address's lock (lifted code takes it around the call) */
void rt_cmpxchg16b(CPU* c, Flags* f, uint64_t a)
{
#ifdef UEW_THREADS
    /* other cores update either half with 8-byte locked instructions, which use the hardware's atomics and not the
       stripe lock this one holds: the low half changes only by a real compare-exchange, so their updates aren't lost */
    if (!((uint32_t)a & 7)) {
        uint64_t* p = (uint64_t*)(MEMB + (uint32_t)a);
        uint64_t hi = __atomic_load_n(p + 1, __ATOMIC_SEQ_CST), lo = R(0);
        if (hi == R(2) && __atomic_compare_exchange_n(p, &lo, R(3), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(p + 1, R(1), __ATOMIC_SEQ_CST);
            rt_setzf(f, 1);
        } else {
            if (hi != R(2)) lo = __atomic_load_n(p, __ATOMIC_SEQ_CST);
            R(0) = lo;
            R(2) = hi;
            rt_setzf(f, 0);
        }
        return;
    }
#endif
    uint64_t lo = LD64(a), hi = LD64(a + 8);
    if (lo == R(0) && hi == R(2)) {
        ST64(a, R(3));
        ST64(a + 8, R(1));
        rt_setzf(f, 1);
    } else {
        R(0) = lo;
        R(2) = hi;
        rt_setzf(f, 0);
    }
}

static uint64_t ldn(uint64_t a, int n) { return n == 1 ? LD8(a) : n == 2 ? LD16(a) : n == 4 ? LD32(a) : LD64(a); }
static void stn(uint64_t a, uint64_t v, int n) { if (n == 1) ST8(a, v); else if (n == 2) ST16(a, v); else if (n == 4) ST32(a, v); else ST64(a, v); }
static uint64_t accn(CPU* c, int n) { return n == 8 ? R(0) : R(0) & ((1ull << (8 * n)) - 1); }

void rt_movs(CPU* c, int n, int rep)
{
    if (c->df) {
        uint64_t cnt = rep ? R(1) : 1;
        for (uint64_t i = 0; i < cnt; i++) { stn(R(7), ldn(R(6), n), n); R(6) -= n; R(7) -= n; }
        if (rep) R(1) = 0;
        return;
    }
    if (!rep) { stn(R(7), ldn(R(6), n), n); R(6) += n; R(7) += n; return; }
    uint64_t cnt = R(1) * (uint64_t)n;
    if (cnt) memmove(G2H(R(7)), G2H(R(6)), (size_t)cnt);
    R(6) += cnt; R(7) += cnt; R(1) = 0;
}

void rt_stos(CPU* c, int n, int rep)
{
    uint64_t cnt = rep ? R(1) : 1;
    if (n == 1 && !c->df) { if (cnt) memset(G2H(R(7)), (int)(uint8_t)R(0), (size_t)cnt); R(7) += cnt; }
    else for (uint64_t i = 0; i < cnt; i++) { stn(R(7), accn(c, n), n); R(7) += c->df ? -n : n; }
    if (rep) R(1) = 0;
}

void rt_lods(CPU* c, int n, int rep)
{
    uint64_t cnt = rep ? R(1) : 1;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t v = ldn(R(6), n);
        if (n == 4) R(0) = v; else if (n == 8) R(0) = v; else R(0) = (R(0) & ~((1ull << (8 * n)) - 1)) | v;
        R(6) += c->df ? -n : n;
    }
    if (rep) R(1) = 0;
}

void rt_scas(CPU* c, Flags* f, int n, int rep)
{
    uint64_t mask = n == 8 ? ~0ull : ((1ull << (8 * n)) - 1);
    do {
        if (rep && !R(1)) break;
        uint64_t a = accn(c, n), b = ldn(R(7), n), r = (a - b) & mask;
        FL(F_SUB, n * 8, a, b, r);
        R(7) += c->df ? -n : n;
        if (rep) R(1)--;
        if (rep == 1 && !ZF(f)) break;
        if (rep == 2 && ZF(f)) break;
    } while (rep);
}

void rt_cmps(CPU* c, Flags* f, int n, int rep)
{
    uint64_t mask = n == 8 ? ~0ull : ((1ull << (8 * n)) - 1);
    do {
        if (rep && !R(1)) break;
        uint64_t a = ldn(R(6), n), b = ldn(R(7), n), r = (a - b) & mask;
        FL(F_SUB, n * 8, a, b, r);
        R(6) += c->df ? -n : n; R(7) += c->df ? -n : n;
        if (rep) R(1)--;
        if (rep == 1 && !ZF(f)) break;
        if (rep == 2 && ZF(f)) break;
    } while (rep);
}


static int ieq(const char* a, const char* b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, y = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (x != y) return 0;
    }
    return *a == *b;
}

const ModuleInfo* rt_find_module(const char* name)
{
    const char* base = name;
    for (const char* p = name; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    char tmp[260];
    snprintf(tmp, sizeof tmp, "%s", base);
    if (!strchr(tmp, '.')) strncat(tmp, ".dll", sizeof tmp - strlen(tmp) - 1);
    for (size_t i = 0; i < rt_module_count; i++)
        if (ieq(rt_modules[i].name, tmp)) return &rt_modules[i];
    return NULL;
}

const ModuleInfo* rt_module_at(uint64_t addr)
{
    for (size_t i = 0; i < rt_module_count; i++)
        if (addr >= rt_modules[i].base && addr < rt_modules[i].base + rt_modules[i].size) return &rt_modules[i];
    return NULL;
}

uint64_t rt_thunk_by_name(const char* name)
{
    for (size_t i = 0; i < rt_thunk_count; i++)
        if (rt_thunks[i].fn && !strcmp(rt_thunks[i].name, name)) return THUNK_BASE + 8ull * i;
    return 0;
}

uint64_t rt_get_export(uint64_t base, const char* name)
{
    uint64_t pe = base + LD32(base + 0x3C);
    uint64_t dd = pe + 24 + (LD16(pe + 24) == 0x10B ? 96 : 112);
    uint64_t dir = LD32(dd), size = LD32(dd + 4);
    if (!dir) return 0;
    uint64_t e = base + dir;
    uint64_t ordbase = LD32(e + 16), nfun = LD32(e + 20), nnames = LD32(e + 24);
    uint64_t afun = base + LD32(e + 28), anames = base + LD32(e + 32), aords = base + LD32(e + 36);
    uint64_t rva = 0;
    if ((uintptr_t)name < 0x10000) {
        uint64_t i = (uintptr_t)name - ordbase;
        if (i < nfun) rva = LD32(afun + 4 * i);
    } else {
        for (uint64_t i = 0; i < nnames; i++)
            if (!strcmp(GSTR(base + LD32(anames + 4 * i)), name)) {
                rva = LD32(afun + 4 * LD16(aords + 2 * i));
                break;
            }
    }
    if (!rva) return 0;
    if (rva >= dir && rva < dir + size) {
        char fwd[256];
        snprintf(fwd, sizeof fwd, "%s", GSTR(base + rva));
        char* dot = strchr(fwd, '.');
        if (!dot) return 0;
        *dot = 0;
        const ModuleInfo* m = rt_find_module(fwd);
        return m ? rt_get_export(m->base, dot + 1) : rt_thunk_by_name(dot + 1);
    }
    return base + rva;
}

static void setup_tls(uint64_t teb)
{
    uint64_t arr = g_calloc(64 * 8);
    STP(teb + (RT_G32 ? 0x2C : 0x58), arr);
    int k = 0;
    for (size_t i = 0; i < rt_module_count; i++) {
        const ModuleInfo* m = &rt_modules[i];
        if (!m->tls_index) continue;
        uint64_t n = m->tls_end - m->tls_start;
        uint64_t blk = g_calloc(n + m->tls_zero + 16);
        if (n) memcpy(G2H(blk), G2H(m->tls_start), (size_t)n);
        STP(arr + (uint64_t)GP * k, blk);
        ST32(m->tls_index, (uint64_t)k);
        k++;
    }
}

uint64_t rt_make_teb(uint64_t stack, uint64_t size, int tid)
{
    uint64_t teb = g_calloc(0x2000);
    if (RT_G32) {
        ST32(teb, 0xFFFFFFFFu);
        ST32(teb + 0x04, stack + size);
        ST32(teb + 0x08, stack);
        ST32(teb + 0x18, teb);
        ST32(teb + 0x20, 0x1000);
        ST32(teb + 0x24, (uint64_t)tid);
        ST32(teb + 0x30, g_peb);
    } else {
        ST64(teb + 0x08, stack + size);
        ST64(teb + 0x10, stack);
        ST64(teb + 0x30, teb);
        ST64(teb + 0x40, 0x1000);
        ST64(teb + 0x48, (uint64_t)tid);
        ST64(teb + 0x60, g_peb);
    }
    setup_tls(teb);
    return teb;
}
static int load_image(const char* path)
{
    FILE* f = host_fopen(path, "rb");
    if (!f) {
        host_log("can't open %s", path);
        return 0;
    }
    uint64_t span = IMAGE_HI - IMAGE_LO;
    size_t got = fread(G2H(IMAGE_LO), 1, (size_t)span, f);
    fclose(f);
    if (got != span) {
        host_log("image is %zu bytes, expected %llu", got, (unsigned long long)span);
        return 0;
    }
    return 1;
}

uint64_t g_teb, g_peb, g_exe_base;

void hle_vpx_init(void);
static const ModuleInfo* main_exe;
#if defined(UEW_THREADS) && defined(UEW_WEBGPU)
void* gpu_wgpu_backend(void);
#endif
static int guest_main(void* arg);
int rt_main(int argc, char** argv, const char* image_path)
{
    MEM_SIZE = (uint64_t)GUEST_MEM_MB << 20;
    /* a 32-bit game can't point past 4 GB: more guest memory (a 64-bit memory build) would only hand it addresses
       it truncates */
    if (RT_G32 && MEM_SIZE > (4ull << 30)) MEM_SIZE = 4ull << 30;
#ifdef __EMSCRIPTEN__
    /* fresh wasm memory is already zero; calloc would memset (and commit) all of it */
    MEM = (uint8_t*)malloc((size_t)MEM_SIZE);
#else
    MEM = (uint8_t*)calloc(1, (size_t)MEM_SIZE);
#endif
    if (!MEM) {
        host_log("couldn't reserve %llu MB of guest memory", (unsigned long long)GUEST_MEM_MB);
        return 1;
    }
    zero_free = !host_query_flag("nozerofree");
    build_fn_hash();
    units_init(IMAGE_HI);
    const ModuleInfo* exe = NULL;
    for (size_t i = 0; i < rt_module_count; i++)
        if (!rt_modules[i].is_dll) exe = &rt_modules[i];
    g_exe_base = exe->base;

    CPU* c = &main_cpu;
    memset(c, 0, sizeof *c);
    c->mxcsr = 0x1F80;
    c->fcw = 0x27F;
    g_peb = g_calloc(0x1000);
    if (RT_G32) {
        ST32(g_peb + 0x08, g_exe_base);
        ST32(g_peb + 0x64, 8);
        ST32(g_peb + 0xA4, 10);
        ST32(g_peb + 0xAC, 19045);
    } else {
        ST64(g_peb + 0x10, g_exe_base);
    }
    uint64_t stack_size = 16ull << 20;
    uint64_t stack = g_alloc(stack_size);
    c->tid = 0x1004;
    /* the scheduler before the image: reading it may have to wait for its download, which blocks through the scheduler.
       The TEB after it (its TLS blocks are copied from the image) */
    sched_init(c);
    if (!load_image(image_path)) return 1;
    g_teb = rt_make_teb(stack, stack_size, c->tid);
    c->gsbase = g_teb;
    hle_init(argc, argv);
    hle_vpx_init();
    R(4) = (stack + stack_size - 64) & ~15ull;
    main_exe = exe;
#if defined(UEW_THREADS) && defined(UEW_WEBGPU)
    /* the GPU device belongs to the page's thread: make it here, before the game's main thread moves to another core */
    gpu_wgpu_backend();
#endif
    exit_code = sched_run_main(guest_main, c);
    host_flush();
    return exit_code;
}

static int guest_main(void* arg)
{
    CPU* c = arg;
    const ModuleInfo* exe = main_exe;
    if (setjmp(exit_jmp) == 0) {
        for (size_t i = 0; i < rt_module_count; i++) {
            const ModuleInfo* m = &rt_modules[i];
            for (size_t k = 0; k < m->cb_count; k++) {
                uint64_t a[3] = { m->base, 1, 0 };
                rt_invoke(c, rt_tls_callbacks[m->cb_first + k], 3, a);
            }
            if (m->is_dll && m->entry_rva) {
                uint64_t a[3] = { m->base, 1, 0 };
                if (!(uint32_t)rt_invoke(c, m->base + m->entry_rva, 3, a)) host_log("[load] %s DllMain failed", m->name);
            }
        }
        R(4) -= GP;
        STP(R(4), RET_SENTINEL);
        rt_call(c, exe->base + exe->entry_rva);
        exit_code = (int)R(0);
    }
    return exit_code;
}

/* code addresses found on the guest stack (return addresses, and some stale ones), nearest first; up to max of them */
void rt_backtrace_n(CPU* c, const char* tag, int max)
{
    uint64_t sp = c->r[4];
    int n = 0;
    char line[2048];
    int k = snprintf(line, sizeof line, "[%s] stack:", tag);
    for (int i = 0; i < 8192 && n < max && k < (int)sizeof line - 24; i++) {
        uint64_t v = LD64(sp + 8ull * i);
        if (v >= IMAGE_LO && v < IMAGE_HI && rt_module_at(v)) { k += snprintf(line + k, sizeof line - (size_t)k, " %llx", (unsigned long long)v); n++; }
    }
    host_log("%s", line);
}

void rt_backtrace(CPU* c, const char* tag) { rt_backtrace_n(c, tag, 16); }

/* flags handed across a jump into another lifted fragment */
#ifdef _MSC_VER
__declspec(thread) uint64_t rt_xfl = 2;
#else
_Thread_local uint64_t rt_xfl = 2;
#endif

/* setjmp/longjmp: lifted setjmp sites call rt_sj_slot and run a host setjmp in their own frame.
   Guest buffer layout is MSVC x64 _JUMP_BUFFER. */
typedef struct { uint64_t jb, rip; jmp_buf host; } SjSlot;
static SjSlot sj[256];
static unsigned sj_next;

jmp_buf* rt_sj_slot(CPU* c, uint64_t jb, uint64_t frame, uint64_t rip)
{
    static const int regs[] = { 3, 4, 5, 6, 7, 12, 13, 14, 15 };
    ST64(jb, frame);
    for (int i = 0; i < 9; i++) ST64(jb + 8 + 8ull * i, c->r[regs[i]]);
    ST64(jb + 80, rip);
    ST32(jb + 88, c->mxcsr);
    ST16(jb + 92, 0x27F);
    for (int i = 0; i < 10; i++) memcpy(G2H(jb + 96 + 16ull * i), &c->x[6 + i], 16);
    for (unsigned i = 0; i < 256; i++)
        if (sj[i].jb == jb) return &sj[i].host;
    SjSlot* s = &sj[sj_next++ & 255];
    s->jb = jb;
    return &s->host;
}

/* 32-bit: the guest's own setjmp/longjmp keep the buffer; a jump to a return address a setjmp site recorded is a
   longjmp, which lands in the host setjmp of the lifted frame that made the call (registers already restored) */
jmp_buf* rt_sj_slot32(CPU* c, uint64_t jb, uint64_t rip)
{
    (void)c;
    for (unsigned i = 0; i < 256; i++)
        if (sj[i].jb == jb) { sj[i].rip = rip; return &sj[i].host; }
    SjSlot* s = &sj[sj_next++ & 255];
    s->jb = jb;
    s->rip = rip;
    return &s->host;
}

static void sj_jump32(CPU* c, uint64_t t)
{
    for (unsigned i = 0; i < 256; i++)
        if (sj[i].jb && sj[i].rip == t && LD32(sj[i].jb + 0x14) == t) longjmp(sj[i].host, 1);
    (void)c;
}

/* ---- 32-bit SEH and C++ exceptions ----
   RaiseException walks the thread's handler chain (fs:[0]) and calls each handler as Windows does. A C++ catch ends
   in the CRT's _JumpToContinuation: esp and ebp set to the catching function's frame and a jump into its body. That
   function took a host setjmp on entry (rt_eh_slot32, from the lifter for every function with a try block, keyed by
   its frame pointer), so the jump longjmps back to that host frame, which then runs the continuation (lifted as an
   entry point of its own) and returns for it: the host call stack matches the guest's again. */
enum { EH_SLOTS = 256, EH_DEPTH = 16 };
typedef struct { uint32_t frame, fn; jmp_buf host; } EhSlot;
typedef struct {
    int n, nd;
    uint32_t target;
    uint32_t disp_sp[EH_DEPTH];      /* guest esp of each dispatch in progress (RaiseException calls), innermost last */
    EhSlot s[EH_SLOTS];               /* frames that can catch, outermost first */
} EhThread;

static EhThread* eh_me(void)
{
    GThread* t = sched_current();
    if (!t) return NULL;
    if (!t->eh) t->eh = calloc(1, sizeof(EhThread));
    return (EhThread*)t->eh;
}

/* function entry: frame is what ebp will be inside it, fn its address */
jmp_buf* rt_eh_slot32(CPU* c, uint64_t frame, uint64_t fn)
{
    static jmp_buf spare;
    EhThread* t = eh_me();
    if (!t) return &spare;
    uint32_t sp = (uint32_t)c->r[4];
    /* frames at or below this one have returned (the stack grows down) */
    while (t->n && t->s[t->n - 1].frame < sp) t->n--;
    if (t->n == EH_SLOTS) {
        memmove(t->s, t->s + 1, sizeof t->s[0] * (EH_SLOTS - 1));
        t->n--;
    }
    EhSlot* e = &t->s[t->n++];
    e->frame = (uint32_t)frame;
    e->fn = (uint32_t)fn;
    return &e->host;
}

/* a jump while an exception is being dispatched: into a registered frame's own function, with that frame's ebp, is
   a catch continuation */
static void eh_check(CPU* c, uint64_t target)
{
    GThread* g = sched_current();
    EhThread* t = g ? (EhThread*)g->eh : NULL;
    if (!t || !t->nd) return;
    uint32_t bp = (uint32_t)c->r[5];
    for (int i = t->n; i-- > 0;) {
        EhSlot* e = &t->s[i];
        if (e->frame != bp || target <= e->fn || target - e->fn >= 0x100000) continue;
        /* the dispatches below the frame are over */
        while (t->nd && t->disp_sp[t->nd - 1] < e->frame) t->nd--;
        t->n = i + 1;
        t->target = (uint32_t)target;
        longjmp(e->host, 1);
    }
}

/* in the catching function's host frame, after the longjmp: run the continuation; the caller returns next */
void rt_eh_land(CPU* c)
{
    EhThread* t = eh_me();
    uint32_t target = t ? t->target : 0;
    GuestFn f = target ? lookup_fn(target) : NULL;
    if (!f) rt_trap(c, target, "catch continuation is not a lifted entry point");
    f(c);
}

static int seh_frame_ok(CPU* c, uint32_t reg)
{
    uint32_t hi = (uint32_t)LD32(c->gsbase + 4), lo = (uint32_t)LD32(c->gsbase + 8);
    return reg != 0xFFFFFFFFu && reg >= lo && reg + 8 <= hi && !(reg & 3);
}

/* EXCEPTION_RECORD (x86): code, flags, record, address, parameter count, parameters[15] */
static void seh_record(uint64_t rec, uint32_t code, uint32_t flags, uint32_t addr, uint32_t n, uint64_t args)
{
    memset(G2H(rec), 0, 80);
    ST32(rec, code);
    ST32(rec + 4, flags);
    ST32(rec + 12, addr);
    if (n > 15) n = 15;
    ST32(rec + 16, n);
    for (uint32_t i = 0; i < n && args; i++) ST32(rec + 20 + 4ull * i, LD32(args + 4ull * i));
}

/* CONTEXT (x86): the integer registers as the caller of RaiseException/RtlUnwind has them */
static void seh_context(CPU* c, uint64_t ctx, uint32_t eip, uint32_t esp)
{
    memset(G2H(ctx), 0, 0x2CC);
    ST32(ctx, 0x10007);
    ST32(ctx + 0x9C, (uint32_t)c->r[7]); ST32(ctx + 0xA0, (uint32_t)c->r[6]); ST32(ctx + 0xA4, (uint32_t)c->r[3]);
    ST32(ctx + 0xA8, (uint32_t)c->r[2]); ST32(ctx + 0xAC, (uint32_t)c->r[1]); ST32(ctx + 0xB0, (uint32_t)c->r[0]);
    ST32(ctx + 0xB4, (uint32_t)c->r[5]); ST32(ctx + 0xB8, eip); ST32(ctx + 0xBC, 0x23); ST32(ctx + 0xC0, 0x202);
    ST32(ctx + 0xC4, esp); ST32(ctx + 0xC8, 0x2B);
}

/* runtime scratch for one dispatch, below the runtime's own frame; handlers run below it */
static uint64_t seh_area(CPU* c) { return (c->r[4] - 0x1000) & ~15ull; }

/* the dispatcher's own registration, which Windows puts at the head of the chain while a handler runs: the CRT's
   catch code relinks it on top of the catching frame after unwinding (_UnwindNestedFrames), so without it the
   catching frame would be linked to itself. Its handler lets every search go on past it. */
static void seh_dispatcher(CPU* c) { RET(1); }

/* calls handler(record, frame, context, dispatcher context); returns its EXCEPTION_DISPOSITION */
static uint32_t seh_call(CPU* c, uint64_t area, uint32_t reg, uint64_t rec, uint64_t ctx)
{
    static uint64_t thunk;
    if (!thunk) thunk = rt_dyn_thunk_n(seh_dispatcher, "seh_dispatcher", 0);
    uint64_t save = c->r[4], node = area + 0x700;
    ST32(node, LD32(c->gsbase));
    ST32(node + 4, thunk);
    ST32(c->gsbase, node);
    uint64_t a[4] = { rec, reg, ctx, area + 0x600 };
    ST32(area + 0x600, 0);
    c->r[4] = area;
    uint32_t r = (uint32_t)rt_invoke(c, LD32(reg + 4), 4, a);
    c->r[4] = save;
    /* whatever the handler left under the dispatcher's record */
    if ((uint32_t)LD32(c->gsbase) == (uint32_t)node) ST32(c->gsbase, LD32(node));
    return r;
}

/* RaiseException for 32-bit games: returns 1 if a handler said to continue execution, 0 if nothing handled it (a
   catch never returns here) */
int rt_seh_raise32(CPU* c, uint32_t code, uint32_t flags, uint32_t n, uint64_t args, uint32_t ret, uint32_t esp)
{
    EhThread* t = eh_me();
    uint64_t area = seh_area(c), rec = area + 0x100, ctx = area + 0x200;
    seh_record(rec, code, flags, ret, n, args);
    seh_context(c, ctx, ret, esp);
    if (t && t->nd < EH_DEPTH) t->disp_sp[t->nd++] = esp;
    int handled = 0;
    static int dbg = -1;
    if (dbg < 0) dbg = host_query_flag("sehdebug");
    /* the chain runs up the stack: anything else is a broken chain, not a handler to call again */
    uint32_t reg = (uint32_t)LD32(c->gsbase), prev = 0;
    for (; seh_frame_ok(c, reg) && reg > prev; prev = reg, reg = (uint32_t)LD32(reg)) {
        uint32_t d = seh_call(c, area, reg, rec, ctx);
        if (dbg) host_log("[seh]   frame %x handler %x -> %u", reg, (unsigned)LD32(reg + 4), d);
        if (d == 0 && !(flags & 1)) { handled = 1; break; }   /* ExceptionContinueExecution */
    }
    if (dbg && !handled) host_log("[seh]   chain ends at %x (after %x)", reg, prev);
    if (t && t->nd) t->nd--;
    return handled;
}

/* RtlUnwind(target frame, target ip, record, return value) for 32-bit games: every handler above the target frame is
   called with the unwinding flag (they run their destructors) and taken off the chain; then back to the caller */
void rt_seh_unwind32(CPU* c, uint32_t target, uint64_t record, uint32_t ret, uint32_t esp)
{
    uint64_t area = seh_area(c), rec = area + 0x100, ctx = area + 0x200;
    if (record) {
        memcpy(G2H(rec), G2H(record), 80);
        ST32(rec + 4, (uint32_t)LD32(rec + 4) | 2 | (target ? 0 : 4));   /* EXCEPTION_UNWINDING, EXIT_UNWIND */
    } else seh_record(rec, 0xC0000027u, 2 | (target ? 0 : 4), ret, 0, 0);   /* STATUS_UNWIND */
    seh_context(c, ctx, ret, esp);
    uint32_t reg = (uint32_t)LD32(c->gsbase);
    while (seh_frame_ok(c, reg) && reg != target) {
        uint32_t next = (uint32_t)LD32(reg);
        seh_call(c, area, reg, rec, ctx);
        ST32(c->gsbase, next);
        if (next <= reg) break;
        reg = next;
    }
}

void rt_sj_jump(CPU* c, uint64_t jb, uint64_t val)
{
    static const int regs[] = { 3, 4, 5, 6, 7, 12, 13, 14, 15 };
    for (unsigned i = 0; i < 256; i++) {
        if (sj[i].jb != jb) continue;
        for (int k = 0; k < 9; k++) c->r[regs[k]] = LD64(jb + 8 + 8ull * k);
        c->mxcsr = LD32(jb + 88);
        for (int k = 0; k < 10; k++) memcpy(&c->x[6 + k], G2H(jb + 96 + 16ull * k), 16);
        c->r[0] = val ? val : 1;
        longjmp(sj[i].host, 1);
    }
    rt_trap(c, LD64(jb + 80), "longjmp to an unknown setjmp buffer");
}

/* UEW_PTRCHECK builds: the first stores of pointer-shaped values with a stray bit 32, with the storing function and
   the guest call stack */
void rt_ptr_store(uint64_t a, uint64_t v, const char* fn)
{
    static int n;
    /* sizes look the same; by default only a heap block's own address with bit 32 set counts. UEW_PTRVAL=<low 32 bits>
       (or ?ptrval= on the page) narrows it to one value */
    static int64_t want = -2;
    if (want == -2) {
        const char* e = getenv("UEW_PTRVAL");
        want = e ? (int64_t)strtoull(e, NULL, 16) : host_query_hex("ptrval") ? (int64_t)host_query_hex("ptrval") : -1;
    }
    if (want >= 0 ? (uint32_t)v != (uint32_t)want : !(uint32_t)v || g_pages_query((uint32_t)v, NULL) != (uint32_t)v) return;
    if (n++ >= 200) return;
    host_log("[ptr] %s stores %llx at %llx (thread %u)", fn, (unsigned long long)v, (unsigned long long)a, sched_current_id());
    if (n <= 40) rt_backtrace(sched_current_cpu(), "ptr");
}

/* UEW_STORE_CHECK builds: the first bad stores, with the lifted function that made them */
void rt_bad_store(uint64_t a, const char* fn)
{
    static int n;
    if (n++ < 30) host_log("[store] %s writes %llx (thread %u)", fn, (unsigned long long)a, sched_current_id());
}

#ifdef UEW_THREADS
/* locked instructions with guest threads on several cores: a spinlock per 16-byte stripe of the address space */
static volatile int atomic_stripes[8192];
void rt_atomic_begin(uint64_t a)
{
    volatile int* l = &atomic_stripes[(a >> 4) & 8191];
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) while (__atomic_load_n(l, __ATOMIC_RELAXED)) {}
}
void rt_atomic_end(uint64_t a) { __atomic_store_n(&atomic_stripes[(a >> 4) & 8191], 0, __ATOMIC_RELEASE); }
#endif

#ifdef UEW_G32CHECK
void rt_g32_wide(const char* fn)
{
    static const char* seen[512];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == fn) return;
    if (n < 512) seen[n++] = fn;
    host_log("[g32] 64-bit guest access in %s", fn);
}
#endif

#ifdef UEW_EBPCHECK
/* ?ebpwatch=<hex>: the first recompiled function entered with that guest ebp, with this thread's own recent history of
   function entries (esp, ebp), printed where the stack pointer moved between memory regions. Also logs the first
   function entered with a misaligned esp. Debug builds only (UEW_EXTRA_CFLAGS=-DUEW_EBPCHECK). */
enum { EH = 16384 };
typedef struct { CPU* c; unsigned n; struct { const char* fn; uint32_t sp, bp; } e[EH]; } EbpHist;
static void ebp_dump(EbpHist* h, CPU* c, const char* tag)
{
    uint32_t next_sp = (uint32_t)c->r[4];
    int shown = 0;
    for (unsigned i = h->n; i-- > (h->n > EH ? h->n - EH : 0) && shown < 60;) {
        unsigned k = i % EH;
        if ((h->e[k].sp >> 20) != (next_sp >> 20) || i + 40 > h->n) {
            host_log("[%s]   -%u %s esp=%x ebp=%x", tag, h->n - 1 - i, h->e[k].fn, h->e[k].sp, h->e[k].bp);
            shown++;
        }
        next_sp = h->e[k].sp;
    }
    /* the outermost functions entered in the whole window: the callers of whatever the thread keeps doing */
    struct { const char* fn; uint32_t sp; unsigned n; } top[24];
    int nt = 0;
    for (unsigned i = h->n > EH ? h->n - EH : 0; i < h->n; i++) {
        unsigned k = i % EH;
        int j = 0;
        while (j < nt && top[j].fn != h->e[k].fn) j++;
        if (j < nt) { top[j].n++; if (h->e[k].sp > top[j].sp) top[j].sp = h->e[k].sp; continue; }
        if (nt < 24) { top[nt].fn = h->e[k].fn; top[nt].sp = h->e[k].sp; top[nt].n = 1; nt++; continue; }
        int lo = 0;
        for (int m = 1; m < nt; m++) if (top[m].sp < top[lo].sp) lo = m;
        if (h->e[k].sp > top[lo].sp) { top[lo].fn = h->e[k].fn; top[lo].sp = h->e[k].sp; top[lo].n = 1; }
    }
    for (int a = 0; a < nt; a++)
        for (int b = a + 1; b < nt; b++)
            if (top[b].sp > top[a].sp) { __typeof__(top[0]) t = top[a]; top[a] = top[b]; top[b] = t; }
    for (int a = 0; a < nt && a < 16; a++)
        host_log("[%s]   outer %s esp=%x (%u entries)", tag, top[a].fn, top[a].sp, top[a].n);
}
static EbpHist* hs[64];
/* the thread dump (debug panel): every thread's latest function entries, to see where a busy thread loops */
void rt_ebp_dump_all(void)
{
    for (int i = 0; i < 64 && hs[i]; i++) {
        host_log("[hist] state %p:", (void*)hs[i]->c);
        ebp_dump(hs[i], hs[i]->c, "hist");
    }
}
void rt_ebp_check(CPU* c, const char* fn)
{
    static uint64_t want = ~0ull;
    static int done, sp_done;
    if (want == ~0ull) want = (uint64_t)host_query_hex("ebpwatch");
    EbpHist* h = NULL;
    for (int i = 0; i < 64; i++) {
        if (!hs[i]) { hs[i] = calloc(1, sizeof(EbpHist)); hs[i]->c = c; }
        if (hs[i]->c == c) { h = hs[i]; break; }
    }
    if (!h) return;
    if (want && !done && (uint32_t)c->r[5] == (uint32_t)want) {
        done = 1;
        host_log("[ebp] %llx first seen entering %s from %llx (thread %u, state %p)", (unsigned long long)want, fn,
                 (unsigned long long)(RT_G32 ? LD32(c->r[4]) : LD64(c->r[4])), sched_current_id(), (void*)c);
        if (RT_G32) {
            uint64_t sp = c->r[4];
            host_log("[ebp]   esp=%llx eax=%llx ecx=%llx edx=%llx ebx=%llx esi=%llx edi=%llx", (unsigned long long)sp,
                     (unsigned long long)c->r[0], (unsigned long long)c->r[1], (unsigned long long)c->r[2],
                     (unsigned long long)c->r[3], (unsigned long long)c->r[6], (unsigned long long)c->r[7]);
            for (int i = 0; i < 32; i += 8)
                host_log("[ebp]   [esp+%02x] %08x %08x %08x %08x %08x %08x %08x %08x", 4 * i, (unsigned)LD32(sp + 4ull * i),
                         (unsigned)LD32(sp + 4ull * i + 4), (unsigned)LD32(sp + 4ull * i + 8), (unsigned)LD32(sp + 4ull * i + 12),
                         (unsigned)LD32(sp + 4ull * i + 16), (unsigned)LD32(sp + 4ull * i + 20), (unsigned)LD32(sp + 4ull * i + 24),
                         (unsigned)LD32(sp + 4ull * i + 28));
        }
        ebp_dump(h, c, "ebp");
    }
    if (RT_G32 && !sp_done && (c->r[4] & 3)) {
        sp_done = 1;
        host_log("[esp] %llx not aligned entering %s (thread %u)", (unsigned long long)c->r[4], fn, sched_current_id());
        ebp_dump(h, c, "esp");
    }
    unsigned k = h->n++ % EH;
    h->e[k].fn = fn; h->e[k].sp = (uint32_t)c->r[4]; h->e[k].bp = (uint32_t)c->r[5];
}
#endif
