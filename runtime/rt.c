#include "rt.h"
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
static uint64_t unit_lo, unit_n, unit_rover, units_used;
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

static void units_take(uint64_t i, uint64_t n)
{
    for (uint64_t j = i; j < i + n; j++) {
        CLRB(unit_free, j);
        if (BIT(unit_dirty, j)) { memset(MEM + unit_lo + j * UNIT, 0, UNIT); CLRB(unit_dirty, j); }
    }
    unit_run[i] = (uint32_t)n;
    units_used += n;
}

uint64_t g_pages(uint64_t size)
{
    uint64_t n = (size + UNIT - 1) / UNIT;
    if (!n) n = 1;
    uint64_t i = units_scan(unit_rover, unit_n, n);
    if (i == ~0ull) i = units_scan(0, unit_n, n);
    if (i == ~0ull) {
        host_log("[mem] out of guest memory (%llu bytes requested, %llu MB in use)", (unsigned long long)size,
                 (unsigned long long)(units_used * UNIT >> 20));
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

void g_pages_free(uint64_t a)
{
    uint64_t len, base = g_pages_query(a, &len);
    if (!base || base != a) return;
    uint64_t i = (a - unit_lo) / UNIT, n = len / UNIT;
    for (uint64_t j = i; j < i + n; j++) { SETB(unit_free, j); SETB(unit_dirty, j); }
    unit_run[i] = 0;
    units_used -= n;
    if (i < unit_rover) unit_rover = i;
}

uint64_t g_mem_used(void) { return unit_lo + units_used * UNIT; }

static int size_class(uint64_t n)
{
    int k = 4;
    while ((1ull << k) < n) k++;
    return k;
}

uint64_t g_alloc(uint64_t n)
{
    if (n == 0) n = 1;
    int k = size_class(n + 16);
    uint64_t blk;
    if (k > 15) {
        blk = g_pages(n + 16);
        if (!blk) return 0;
        k = BIG;
    } else if (bins[k]) {
        blk = bins[k];
        bins[k] = LD64(blk + 16);
    } else {
        uint64_t sz = 1ull << k;
        if (arena_cur + sz > arena_end) {
            arena_cur = g_pages(1 << 20);
            if (!arena_cur) return 0;
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
    if (k == BIG) { g_pages_free(p - 16); return; }
    if (k < 4 || k > 15) return;
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
    if (n <= block_cap(p)) { ST64(p - 16, n); return p; }
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
int rt_hle_budget = 1024;
void sched_tick(CPU* c);
void rt_hle_tick(CPU* c) { rt_hle_budget = 1024; sched_tick(c); }
#include "dyn_tramp.inc"

uint64_t rt_dyn_thunk(GuestFn fn, const char* name)
{
    for (size_t i = 0; i < ndyn; i++)
        if (dyn[i].fn == fn) return DYN_BASE + 8ull * i;
    if (ndyn == DYN_MAX) return 0;
    dyn[ndyn].fn = fn;
    dyn[ndyn].name = name;
    dyn[ndyn].dll = "dyn";
    return DYN_BASE + 8ull * ndyn++;
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
static struct { const char* name; uint64_t a[4], ret; unsigned tid; } ring[64];
static unsigned ring_n;
static int ring_on = -1;
static void ring_dump(void)
{
    if (ring_on <= 0) return;
    host_log("[trap] last %u system calls:", ring_n < 64 ? ring_n : 64);
    for (unsigned i = ring_n > 64 ? ring_n - 64 : 0; i < ring_n; i++) {
        unsigned k = i & 63;
        host_log("  %s(%llx, %llx, %llx, %llx) ret=%llx t=%u", ring[k].name, (unsigned long long)ring[k].a[0], (unsigned long long)ring[k].a[1],
                 (unsigned long long)ring[k].a[2], (unsigned long long)ring[k].a[3], (unsigned long long)ring[k].ret, ring[k].tid);
    }
}

void rt_trace_call(CPU* c, const char* name)
{
    if (ring_on < 0) ring_on = host_query_flag("tracering");
    if (ring_on) {
        unsigned k = ring_n++ & 63;
        ring[k].name = name; ring[k].a[0] = R(1); ring[k].a[1] = R(2); ring[k].a[2] = R(8); ring[k].a[3] = R(9);
        ring[k].ret = LD64(R(4)); ring[k].tid = sched_current_id();
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

void rt_call(CPU* c, uint64_t t)
{
    STACK_CHECK(c, t);
    if (t >= DYN_BASE && t < DYN_BASE + 8ull * ndyn) {
        dyn[(t - DYN_BASE) / 8].fn(c);
        R(4) += 8;
        if (--rt_hle_budget < 0) rt_hle_tick(c);
        return;
    }
    if (t >= THUNK_BASE && t < THUNK_BASE + 8ull * rt_thunk_count) {
        const Thunk* e = &rt_thunks[(t - THUNK_BASE) / 8];
        if (trace_on < 0) trace_on = getenv("UEW_TRACE") != NULL || host_query_flag("tracering");
        if (trace_on) rt_trace_call(c, e->name);
        if (e->fn) e->fn(c);
        else hle_missing(c, e);
        R(4) += 8;
        if (--rt_hle_budget < 0) rt_hle_tick(c);
        return;
    }
    const FnEntry* f = find_fn(t);
    if (!f) {
        host_log("[trap] call to %llx from %llx (thread %u)", (unsigned long long)t, (unsigned long long)LD64(R(4)), sched_current_id());
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

GuestFn rt_jump_fn(CPU* c, uint64_t t)
{
    (void)c;
    STACK_CHECK(c, t);
    const FnEntry* f = (t >= IMAGE_LO && t < IMAGE_HI) ? find_fn(t) : NULL;
    if (f) return f->fn;
    jump_pending = t;
    return jump_slow;
}

GuestFn rt_ic_fn(CPU* c, uint64_t t, IC* ic)
{
    GuestFn f = rt_jump_fn(c, t);
    if (f != jump_slow) { ic->f = f; ic->a = t; }
    return f;
}

void rt_call_ic(CPU* c, uint64_t t, IC* ic)
{
    STACK_CHECK(c, t);
    const FnEntry* f = (t >= IMAGE_LO && t < IMAGE_HI) ? find_fn(t) : NULL;
    if (f) {
        ic->f = f->fn;
        ic->a = t;
        f->fn(c);
        return;
    }
    if (t >= DYN_BASE && t < DYN_BASE + 8ull * ndyn && (t - DYN_BASE) / 8 < DYN_TRAMPS) {
        ic->f = dyn_tramp[(t - DYN_BASE) / 8];
        ic->a = t;
    }
    /* imported functions: cache the wrapper unless calls are being logged */
    if (t >= THUNK_BASE && t < THUNK_BASE + 8ull * rt_thunk_count && rt_thunks[(t - THUNK_BASE) / 8].call) {
        ic->f = rt_thunks[(t - THUNK_BASE) / 8].call;
        ic->a = t;
    }
    rt_call(c, t);
}

uint64_t rt_invoke(CPU* c, uint64_t fn, int n, const uint64_t* a)
{
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
    rt_call(c, fn);
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
    exit_code = code;
    longjmp(exit_jmp, 1);
}

void rt_trap(CPU* c, uint64_t pc, const char* what)
{
    ring_dump();
    rt_backtrace(c, "trap");
    host_log("[trap] %s at %llx (rsp=%llx rax=%llx rcx=%llx)", what, (unsigned long long)pc,
             (unsigned long long)R(4), (unsigned long long)R(0), (unsigned long long)R(1));
#ifdef _MSC_VER
    void* fr[24];
    char buf[24 * 12];
    int k = RtlCaptureStackBackTrace(1, 24, fr, NULL), n = 0;
    for (int i = 0; i < k; i++)
        n += snprintf(buf + n, sizeof(buf) - n, " %llx", (unsigned long long)((char*)fr[i] - &__ImageBase));
    host_log("[trap] host stack:%s", buf);
#endif
    rt_exit(0xC0000005);
}

/* FXSAVE area: x87 control/status/tag in their reset state (no x87 here), MXCSR at +24, XMM0-15 at +160 */
void rt_fxsave(CPU* c, uint64_t a)
{
    memset(G2H(a), 0, 512);
    ST16(a, 0x037F);
    ST32(a + 24, c->mxcsr);
    ST32(a + 28, 0xFFFF);
    for (int i = 0; i < 16; i++) memcpy(G2H(a + 160 + 16ull * i), &c->x[i], 16);
}
void rt_fxrstor(CPU* c, uint64_t a)
{
    c->mxcsr = (uint32_t)LD32(a + 24);
    for (int i = 0; i < 16; i++) memcpy(&c->x[i], G2H(a + 160 + 16ull * i), 16);
}

void rt_cpuid(CPU* c)
{
    uint32_t leaf = (uint32_t)R(0), sub = (uint32_t)R(1);
    uint32_t a = 0, b = 0, cc = 0, d = 0;
    (void)sub;
    switch (leaf) {
    case 0: a = 7; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;
    case 1: a = 0x000906EA; b = 0x00100800; cc = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23); d = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15) | (1u << 23) | (1u << 25) | (1u << 26); break;
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
void rt_cmpxchg16b(CPU* c, Flags* f, uint64_t a)
{
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
    if (!rep) { stn(R(7), ldn(R(6), n), n); R(6) += n; R(7) += n; return; }
    uint64_t cnt = R(1) * (uint64_t)n;
    if (cnt) memmove(G2H(R(7)), G2H(R(6)), (size_t)cnt);
    R(6) += cnt; R(7) += cnt; R(1) = 0;
}

void rt_stos(CPU* c, int n, int rep)
{
    uint64_t cnt = rep ? R(1) : 1;
    if (n == 1) { if (cnt) memset(G2H(R(7)), (int)(uint8_t)R(0), (size_t)cnt); R(7) += cnt; }
    else for (uint64_t i = 0; i < cnt; i++) { stn(R(7), accn(c, n), n); R(7) += n; }
    if (rep) R(1) = 0;
}

void rt_lods(CPU* c, int n, int rep)
{
    uint64_t cnt = rep ? R(1) : 1;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t v = ldn(R(6), n);
        if (n == 4) R(0) = v; else if (n == 8) R(0) = v; else R(0) = (R(0) & ~((1ull << (8 * n)) - 1)) | v;
        R(6) += n;
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
        R(7) += n;
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
        R(6) += n; R(7) += n;
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
    uint64_t dir = LD32(pe + 24 + 112), size = LD32(pe + 24 + 116);
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
    ST64(teb + 0x58, arr);
    int k = 0;
    for (size_t i = 0; i < rt_module_count; i++) {
        const ModuleInfo* m = &rt_modules[i];
        if (!m->tls_index) continue;
        uint64_t n = m->tls_end - m->tls_start;
        uint64_t blk = g_calloc(n + m->tls_zero + 16);
        if (n) memcpy(G2H(blk), G2H(m->tls_start), (size_t)n);
        ST64(arr + 8ull * k, blk);
        ST32(m->tls_index, (uint64_t)k);
        k++;
    }
}

uint64_t rt_make_teb(uint64_t stack, uint64_t size, int tid)
{
    uint64_t teb = g_calloc(0x2000);
    ST64(teb + 0x08, stack + size);
    ST64(teb + 0x10, stack);
    ST64(teb + 0x30, teb);
    ST64(teb + 0x40, 0x1000);
    ST64(teb + 0x48, (uint64_t)tid);
    ST64(teb + 0x60, g_peb);
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
int rt_main(int argc, char** argv, const char* image_path)
{
    MEM_SIZE = (uint64_t)GUEST_MEM_MB << 20;
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
    build_fn_hash();
    units_init(IMAGE_HI);
    if (!load_image(image_path)) return 1;
    const ModuleInfo* exe = NULL;
    for (size_t i = 0; i < rt_module_count; i++)
        if (!rt_modules[i].is_dll) exe = &rt_modules[i];
    g_exe_base = exe->base;

    CPU* c = &main_cpu;
    memset(c, 0, sizeof *c);
    c->mxcsr = 0x1F80;
    g_peb = g_calloc(0x1000);
    ST64(g_peb + 0x10, g_exe_base);
    uint64_t stack_size = 16ull << 20;
    uint64_t stack = g_alloc(stack_size);
    c->tid = 0x1004;
    g_teb = rt_make_teb(stack, stack_size, c->tid);
    c->gsbase = g_teb;
    sched_init(c);
    hle_init(argc, argv);
    hle_vpx_init();
    R(4) = (stack + stack_size - 64) & ~15ull;

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
        R(4) -= 8;
        ST64(R(4), RET_SENTINEL);
        rt_call(c, exe->base + exe->entry_rva);
        exit_code = (int)R(0);
    }
    host_flush();
    return exit_code;
}

void rt_backtrace(CPU* c, const char* tag)
{
    uint64_t sp = c->r[4];
    int n = 0;
    char line[512];
    int k = snprintf(line, sizeof line, "[%s] stack:", tag);
    for (int i = 0; i < 4096 && n < 16; i++) {
        uint64_t v = LD64(sp + 8ull * i);
        if (v >= IMAGE_LO && v < IMAGE_HI && rt_module_at(v)) { k += snprintf(line + k, sizeof line - (size_t)k, " %llx", (unsigned long long)v); n++; }
    }
    host_log("%s", line);
}

/* flags handed across a jump into another lifted fragment */
#ifdef _MSC_VER
__declspec(thread) uint64_t rt_xfl = 2;
#else
_Thread_local uint64_t rt_xfl = 2;
#endif

/* setjmp/longjmp: lifted setjmp sites call rt_sj_slot and run a host setjmp in their own frame.
   Guest buffer layout is MSVC x64 _JUMP_BUFFER. */
typedef struct { uint64_t jb; jmp_buf host; } SjSlot;
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
