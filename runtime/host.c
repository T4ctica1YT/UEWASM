#include "host.h"
#include "sched.h"
#include <stdlib.h>

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

void host_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

void host_write(int fd, const char* s, size_t n)
{
    FILE* f = fd == 2 ? stderr : stdout;
#ifdef __EMSCRIPTEN__
    for (size_t i = 0; i < n; i++)
        if (s[i] != '\r') fputc(s[i], f);
#else
    fwrite(s, 1, n, f);
#endif
}

void host_flush(void)
{
    fflush(stdout);
    fflush(stderr);
}

static uint64_t real_ns(void)
{
#ifdef __EMSCRIPTEN__
    return (uint64_t)(emscripten_get_now() * 1e6);
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}
/* the game's clock: real time, or faster while fast-forwarding (debug panel) */
static double speed = 1.0;
static uint64_t real0, virt0;
uint64_t host_ticks_ns(void)
{
    uint64_t r = real_ns() - real0;
    return virt0 + (speed == 1.0 ? r : (uint64_t)((double)r * speed));
}
double host_speed(void) { return speed; }
#if defined(__EMSCRIPTEN__) && defined(UEW_DEBUG_TOOLS)
EMSCRIPTEN_KEEPALIVE
#endif
void uew_set_speed(double s)
{
    if (s < 0.25) s = 0.25;
    if (s > 16) s = 16;
    uint64_t v = host_ticks_ns();
    real0 = real_ns();
    virt0 = v;
    speed = s;
}

uint64_t host_time_100ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
void* host_ctx_main(void) { return ConvertThreadToFiber(NULL); }
typedef struct { void (*entry)(void*); void* arg; } FiberStart;
static void WINAPI fiber_proc(void* p) { FiberStart s = *(FiberStart*)p; free(p); s.entry(s.arg); }
void* host_ctx_new(void (*entry)(void*), void* arg, size_t stack)
{
    FiberStart* s = malloc(sizeof *s);
    s->entry = entry;
    s->arg = arg;
    return CreateFiberEx(64 << 10, stack < (64u << 20) ? 64u << 20 : stack, 0, fiber_proc, s);
}
void host_ctx_switch(void* to) { SwitchToFiber(to); }
void host_ctx_free(void* ctx) { DeleteFiber(ctx); }
void host_sleep_ms(uint32_t ms) { Sleep(ms); }
void host_idle(uint64_t ns) { Sleep((DWORD)(ns / 1000000)); }
#elif defined(__EMSCRIPTEN__)
#include <emscripten/stack.h>
typedef struct HCtx { int id; uintptr_t sp, top; void (*fn)(void*); void* arg; void* mem; } HCtx;
void _emscripten_stack_restore(uintptr_t);
/* per host thread: with real threads every core switches its own coroutines */
static _Thread_local HCtx main_hctx;
static _Thread_local HCtx* cur_hctx;
static int next_hctx = 1;

EM_ASYNC_JS(void, uew_js_switch, (int from, int to, HCtx* target), {
    Module.uewRes = Module.uewRes || {};
    await new Promise(resolve => {
        Module.uewRes[from] = resolve;
        const r = Module.uewRes[to];
        if (r) { delete Module.uewRes[to]; r(); }
        else Module._uew_ctx_start(target);
    });
});

EMSCRIPTEN_KEEPALIVE void uew_ctx_start(HCtx* c)
{
    _emscripten_stack_restore(c->top);
    cur_hctx = c;
    c->fn(c->arg);
}

void* host_ctx_main(void) { cur_hctx = &main_hctx; return &main_hctx; }
void* host_ctx_new(void (*entry)(void*), void* arg, size_t stack)
{
    HCtx* c = calloc(1, sizeof *c);
    uint8_t* mem = malloc(stack);
    if (!c || !mem) return NULL;
    c->id = __atomic_fetch_add(&next_hctx, 1, __ATOMIC_RELAXED);
    c->mem = mem;
    memset(mem, 0xA5, 256);     /* canary: a coroutine that runs off the bottom of its native stack overwrites it */
    c->top = ((uintptr_t)mem + stack - 64) & ~(uintptr_t)15;
    c->fn = entry;
    c->arg = arg;
    return c;
}
void host_ctx_switch(void* to)
{
    HCtx* me = cur_hctx;
    HCtx* t = to;
    if (t == me) return;
    me->sp = emscripten_stack_get_current();
    if (me->mem) {
        static int said;
        const uint8_t* m = me->mem;
        for (int i = 0; i < 256; i++) if (m[i] != 0xA5) { if (said++ < 10) host_log("[stack] coroutine %d overflowed its native stack (lowest sp %p)", me->id, (void*)me->sp); break; }
    }
    cur_hctx = t;
    uew_js_switch(me->id, t->id, t);
    _emscripten_stack_restore(me->sp);
    cur_hctx = me;
}
/* only for contexts that will never run again (their thread exited) */
EM_JS(void, uew_js_ctx_drop, (int id), { if (Module.uewRes) delete Module.uewRes[id]; });
void host_ctx_free(void* ctx)
{
    HCtx* c = ctx;
    if (!c || c == &main_hctx || c == cur_hctx) return;
    uew_js_ctx_drop(c->id);
    free(c->mem);
    free(c);
}
EM_ASYNC_JS(void, uew_js_sleep, (int ms), { await new Promise(r => setTimeout(r, ms)); });
/* one macrotask turn without setTimeout's 4 ms clamp */
EM_ASYNC_JS(void, uew_js_yield, (void), {
    await new Promise(r => {
        if (typeof setImmediate === "function") { setImmediate(r); return; }
        const ch = Module.uewYieldCh || (Module.uewYieldCh = new MessageChannel());
        ch.port1.onmessage = () => r();
        ch.port2.postMessage(0);
    });
});
static uint64_t browser_turn;
static int frame_pending;
int host_ext_waits;
void host_sleep_ms(uint32_t ms) { uew_js_sleep((int)((double)ms / speed)); browser_turn = host_ticks_ns(); }
void host_browser_turned(void) { browser_turn = host_ticks_ns(); }
/* green threads switch through promise microtasks, which never let the page's event loop run (fetches, timers,
   input); when the game hasn't presented or slept for a quarter second (loading), give the browser a turn. While a
   frame is wanted the turns come every 2 ms, so the animation frame callback isn't held up by worker threads. */
void host_browser_turn(void)
{
    uint64_t now = host_ticks_ns();
    if (now - browser_turn < (frame_pending || host_ext_waits ? 2000000ull : 250000000ull)) return;
    uew_js_yield();
    browser_turn = host_ticks_ns();
}
/* frames are counted, not handed out: every thread waiting for the next one wakes when the count moves. The page's
   frame callback (uew_js_frame_request, uew_host.js) writes the count here, where every core can read it */
void uew_js_frame_request(volatile uint32_t* seq);
volatile uint32_t uew_frame_seq;
volatile uint32_t uew_wake_word;
/* waits up to ms, or until the animation frame fires; short waits use a message turn (setTimeout clamps nested
   calls to 4 ms) */
EM_ASYNC_JS(void, uew_js_idle, (double ms, volatile uint32_t* wake), {
    await new Promise(r => {
        let done = false;
        const fin = () => { if (!done) { done = true; Module.uewIdleWake = null; r(); } };
        // real threads: another core's notify (sched_notify) ends the wait too
        if (wake && typeof SharedArrayBuffer != "undefined" && HEAP32.buffer instanceof SharedArrayBuffer) {
            const w = Atomics.waitAsync(HEAP32, wake >>> 2, Atomics.load(HEAP32, wake >>> 2), ms);
            if (w.async) w.value.then(fin);
        }
        // a finished download, pipeline or readback ends the wait early (Module.uewWake)
        Module.uewIdleWake = fin;
        if (ms < 3) {
            if (typeof setImmediate === "function") { setImmediate(fin); return; }
            const ch = Module.uewIdleCh || (Module.uewIdleCh = new MessageChannel());
            ch.port1.onmessage = () => fin();
            ch.port2.postMessage(0);
        } else setTimeout(fin, ms - 1);
    });
});
void host_frame_request(void) { frame_pending = 1; uew_js_frame_request(&uew_frame_seq); }
uint32_t host_frame_seq(void) { return uew_frame_seq; }
void host_frame_done(void) { frame_pending = 0; browser_turn = host_ticks_ns(); }
#ifdef UEW_THREADS
void host_idle(uint64_t ns) { uew_js_idle((double)ns / 1e6 / speed, &uew_wake_word); browser_turn = host_ticks_ns(); }
#else
void host_idle(uint64_t ns) { uew_js_idle((double)ns / 1e6 / speed, NULL); browser_turn = host_ticks_ns(); }
#endif
#else
void* host_ctx_main(void) { return (void*)1; }
void* host_ctx_new(void (*entry)(void*), void* arg, size_t stack) { (void)entry; (void)arg; (void)stack; return NULL; }
void host_ctx_switch(void* to) { (void)to; }
void host_ctx_free(void* ctx) { (void)ctx; }
void host_sleep_ms(uint32_t ms) { (void)ms; }
void host_idle(uint64_t ns) { (void)ns; }
#endif

#ifndef __EMSCRIPTEN__
void host_browser_turn(void) {}
void host_browser_turned(void) {}
int host_ext_waits;
int host_query_flag(const char* name) { (void)name; return 0; }
double host_query_hex(const char* name) { const char* e = getenv(name); return e ? (double)strtoull(e, NULL, 16) : 0; }
int host_query_int(const char* name) { (void)name; return 0; }
int host_page_int(const char* name) { (void)name; return 0; }
void host_frame_request(void) {}
uint32_t host_frame_seq(void) { static uint32_t n; return ++n; }
void host_frame_done(void) {}
void host_poll_events(void) {}
const char* host_trace_filter(void) { static char b[1024]; const char* e = getenv("UEW_TRACE_ONLY"); if (e && !b[0]) snprintf(b, sizeof b, ",%s,", e); return b; }
void host_frame_yield(void) {}
void host_canvas_size(int* w, int* h) { *w = 1280; *h = 720; }
void host_show_cursor(int show) { (void)show; }
void host_lock_pointer(int lock) { (void)lock; }
int host_gamepad(int index, uint16_t* buttons, uint8_t* triggers, int16_t* sticks) { (void)index; (void)buttons; (void)triggers; (void)sticks; return 0; }
void host_rumble(int index, int lo, int hi) { (void)index; (void)lo; (void)hi; }

/* native builds only verify the game: audio is paced by the clock and dropped */
static uint64_t audio_t0, audio_written;
static int audio_rate;
int host_audio_open(int rate) { audio_rate = rate; audio_t0 = host_ticks_ns(); audio_written = 0; return 1; }
int host_audio_queued(void)
{
    if (!audio_rate) return 0;
    uint64_t played = (host_ticks_ns() - audio_t0) * (uint64_t)audio_rate / 1000000000ull;
    if (played > audio_written) { audio_written = played; return 0; }
    return (int)(audio_written - played);
}
void host_audio_write(const float* frames, int count) { (void)frames; audio_written += (uint64_t)count; }
#endif

#ifdef _WIN32
CPU* sched_current_cpu(void);
void rt_backtrace(CPU* c, const char* tag);
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* e)
{
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != 0xC0000374 && code != STATUS_STACK_BUFFER_OVERRUN) return EXCEPTION_CONTINUE_SEARCH;
    static int once;
    if (once++) return EXCEPTION_CONTINUE_SEARCH;
    char* img = (char*)GetModuleHandleW(NULL);
    host_log("[crash] host exception %08lx at rva %llx", code, (unsigned long long)((char*)e->ExceptionRecord->ExceptionAddress - img));
    void* fr[32];
    int n = RtlCaptureStackBackTrace(0, 32, fr, NULL);
    for (int i = 0; i < n; i++) {
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, fr[i], &m);
        if (m) GetModuleFileNameA(m, name, MAX_PATH);
        const char* s = strrchr(name, '\\');
        host_log("  %s+%llx", s ? s + 1 : name, (unsigned long long)((char*)fr[i] - (char*)m));
    }
    rt_backtrace(sched_current_cpu(), "crash");
    host_flush();
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main(int argc, char** argv)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, crash_handler);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    const char* image = "image.bin";
    for (int i = 1; i < argc - 1; i++) {
        if (!strcmp(argv[i], "--image")) image = argv[i + 1];
        if (!strcmp(argv[i], "--root")) snprintf(g_root, sizeof g_root, "%s", argv[i + 1]);
        if (!strcmp(argv[i], "--save")) snprintf(g_save, sizeof g_save, "%s", argv[i + 1]);
        if (!strcmp(argv[i], "--exe")) snprintf(g_exe_rel, sizeof g_exe_rel, "%s", argv[i + 1]);
    }
    return rt_main(argc, argv, image);
}
