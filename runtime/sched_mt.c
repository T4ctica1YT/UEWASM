/* Real-thread scheduler (UEW_THREADS builds; sched.c is the single-threaded one).
   Guest threads still run as green threads, but on several host threads ("carriers", one per core): each carrier
   schedules its own green threads exactly as the single-threaded scheduler does, so waits, priorities and frame
   pacing keep their behaviour. Guest code runs in parallel; system calls and every scheduler structure are guarded
   by one global recursive lock (rt_lock), which a carrier gives up while it idles. */
#ifdef UEW_THREADS
#include "sched.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <emscripten/threading.h>

#define MAX_CARRIERS 16

typedef struct Carrier {
    int id;
    GThread base;                 /* the carrier's own context (carrier 0: the guest main thread) */
    GThread* threads;             /* its green threads */
    GThread* current;
    int nthreads, ndead, nprio, nframe_waiters, load;
    uint64_t prio_next, run_since, idle_ns, nswitch, slice_start;
} Carrier;

static Carrier carriers[MAX_CARRIERS];
static int weight(const char* n);
static int ncarriers = 1;
static __thread Carrier* K;
static GThread* all_threads;      /* every thread, through all_next */
static int next_tid = 0x1008;
static int total_threads = 1;
static struct { SchedCheck fn; const char* name; void (*desc)(GThread*, char*, int); } check_names[16];
static int ncheck_names;
static uint64_t idle_by[18];
static uint64_t report_last;

/* the global lock: a plain mutex taken once per host thread, with a depth count (system calls nest through callbacks).
   Green threads on a carrier switch only while it is held, each keeping its own depth */
static pthread_mutex_t big_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t big_cv = PTHREAD_COND_INITIALIZER;
static __thread int lock_depth;
static int idle_n;                /* carriers waiting for work: only then do releases wake anyone */
extern volatile uint32_t uew_wake_word;
static GThread* main_thread;      /* the guest's main thread */
static int base_is_guest = 1;     /* carrier 0's own context is the guest main thread (until sched_run_main moves it) */
static void switch_to(GThread* t);
static GThread* pick(void);
static void sched_idle(uint64_t deadline);
static void wake_carriers(void);

/* the page's thread never blocks: while it waits for the lock it runs the calls other cores sent it (browser
   functions, uew_host.js), one of which may be what the holder is waiting for */
void rt_lock_report(void);
static uint64_t lock_wait_ns[MAX_CARRIERS];   /* time each carrier spent waiting for the lock, for the hitch report */
void rt_lock(void)
{
    if (lock_depth++) return;
    if (!pthread_mutex_trylock(&big_lock)) {
        if (emscripten_is_main_runtime_thread()) emscripten_main_thread_process_queued_calls();
        return;
    }
    uint64_t t0 = host_ticks_ns();
    if (emscripten_is_main_runtime_thread())
        while (pthread_mutex_trylock(&big_lock)) emscripten_main_thread_process_queued_calls();
    else pthread_mutex_lock(&big_lock);
    if (K) lock_wait_ns[K->id] += host_ticks_ns() - t0;
}
void rt_unlock(void) { if (--lock_depth == 0) pthread_mutex_unlock(&big_lock); }
int rt_unlock_all(void)
{
    int d = lock_depth;
    if (d) { lock_depth = 0; pthread_mutex_unlock(&big_lock); }
    return d;
}
void rt_relock(int d) { if (d) { rt_lock(); lock_depth = d; } }
void sched_notify(void)
{
    if (!__atomic_load_n(&idle_n, __ATOMIC_RELAXED)) return;
    __atomic_fetch_add(&uew_wake_word, 1, __ATOMIC_SEQ_CST);
    emscripten_futex_wake((void*)&uew_wake_word, 0x7FFFFFFF);
    pthread_cond_broadcast(&big_cv);
}


/* waits up to ns with the lock fully released (another carrier, or a signal from outside, may make work) */
static void lock_wait(uint64_t ns)
{
    if (ns > 4000000ull) ns = 4000000ull;
    if (ns < 50000ull) ns = 50000ull;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_nsec + ns;
    ts.tv_sec += (time_t)(t / 1000000000ull);
    ts.tv_nsec = (long)(t % 1000000000ull);
    pthread_cond_timedwait(&big_cv, &big_lock, &ts);   /* the mutex is held once whatever the depth */
}
/* runs f with the lock fully released, then takes it back at the same depth */
static void unlocked_call(void (*f)(uint64_t), uint64_t arg)
{
    if (!lock_depth) { f(arg); return; }
    int d = lock_depth;
    lock_depth = 0;
    pthread_mutex_unlock(&big_lock);
    f(arg);
    pthread_mutex_lock(&big_lock);
    lock_depth = d;
}
static void wake_carriers(void) { pthread_cond_broadcast(&big_cv); }

int sched_thread_count(void) { return total_threads; }

void sched_note_wait(CPU* c, uint32_t n, uint64_t h, uint32_t ms)
{
    GThread* cur = K->current;
    cur->wait_n = n;
    cur->wait_h = h;
    cur->wait_ms = ms;
    cur->wait_ret = n ? LD64(c->r[4]) : 0;
    cur->wait_since = n ? host_ticks_ns() : 0;
}
__attribute__((used, visibility("default")))
void uew_threads(void)
{
    /* called from the page's watchdog timer: waiting for the lock there would stop the page's event loop, which the
       holder may need, so a busy lock means a best-effort read without it */
    int locked = !lock_depth && pthread_mutex_trylock(&big_lock) == 0;
    if (locked) lock_depth = 1;
    uint64_t now = host_ticks_ns();
    for (GThread* t = all_threads; t; t = t->all_next) {
        if (t->done) continue;
        int running = carriers[t->carrier].current == t;
        const char* st = t->suspended ? "suspended" : running ? "running" : t->wait_n ? "waiting" : t->check ? "blocked" : t->wake_at > now ? "sleeping" : "ready";
        char what[200] = "";
        if (t->check) {
            int k = 0;
            while (k < ncheck_names && check_names[k].fn != t->check) k++;
            if (k < ncheck_names) {
                int n = snprintf(what, sizeof what, " on %s", check_names[k].name);
                if (check_names[k].desc && n < (int)sizeof what) check_names[k].desc(t, what + n, (int)sizeof what - n);
            } else snprintf(what, sizeof what, " on check %p a=%llx", (void*)t->check, (unsigned long long)t->block_a);
        } else if (t->wake_at > now) snprintf(what, sizeof what, " for %.0f ms", (t->wake_at - now) / 1e6);
        host_log("[thr] %u c%d %-24s %s%s, %.1f s, start=%llx ret=%llx", (unsigned)t->tid, t->carrier, t->name, st, what,
                 t->wait_n ? (now - t->wait_since) / 1e9 : 0.0, (unsigned long long)t->start, (unsigned long long)t->wait_ret);
    }
    if (locked) { lock_depth = 0; pthread_mutex_unlock(&big_lock); }
}
unsigned sched_current_id(void) { return (unsigned)K->current->tid; }
int sched_is_main(void) { return K->current == main_thread; }
GThread* sched_current(void) { return K->current; }
CPU* sched_current_cpu(void) { return K->current->cpu; }
GThread* sched_by_tid(uint32_t tid) { for (GThread* t = all_threads; t; t = t->all_next) if ((uint32_t)t->tid == tid) return t; return NULL; }

static int frame_check(GThread* t);
/* the guest main thread on a core of its own; the page's thread then only serves the browser and the threads placed
   on it */
static int (*main_fn)(void*);
static void* main_arg;
static volatile int main_done, main_ret;
static void main_entry(void* arg)
{
    GThread* t = arg;
    rt_unlock();
    int r = main_fn(main_arg);
    rt_lock();
    main_ret = r;
    main_done = 1;
    t->done = 1;
    K->nthreads--;
    K->load -= weight(t->name);
    total_threads--;
    K->ndead++;
    sched_notify();
    wake_carriers();
    for (;;) switch_to(&K->base);
}
int sched_run_main(int (*fn)(void*), void* arg)
{
    if (ncarriers < 2) { rt_unlock_all(); return fn(arg); }
    Carrier* k0 = &carriers[0];
    GThread* t = calloc(1, sizeof *t);
    t->tid = k0->base.tid;
    t->cpu = k0->base.cpu;
    t->started = 1;
    snprintf(t->name, sizeof t->name, "main");
    /* carrier 0's own context becomes a scheduler-only one with a CPU of its own */
    k0->base.cpu = calloc(1, sizeof(CPU));
    k0->base.tid = 0x1000;
    main_fn = fn;
    main_arg = arg;
    t->carrier = 1;
    t->lockd = 1;
    t->ctx = host_ctx_new(main_entry, t, 8u << 20);
    Carrier* k = &carriers[1];
    k->load += weight("GameThread");
    t->next = k->threads;
    k->threads = t;
    k->nthreads++;
    t->all_next = all_threads;
    all_threads = t;
    main_thread = t;
    base_is_guest = 0;
    wake_carriers();
    while (!main_done) {
        GThread* n = pick();
        if (n) switch_to(n);
        else sched_idle(~0ull);
    }
    rt_unlock_all();
    return main_ret;
}

static void* carrier_main(void* arg);

void sched_init(CPU* cpu)
{
    sched_name_check(frame_check, "next browser frame", NULL);
    Carrier* k0 = &carriers[0];
    K = k0;
    k0->id = 0;
    k0->base.tid = cpu->tid;
    k0->base.cpu = cpu;
    k0->base.started = 1;
    k0->base.ctx = host_ctx_main();
    k0->threads = k0->current = &k0->base;
    k0->nthreads = 1;
    k0->prio_next = ~0ull;
    all_threads = &k0->base;
    main_thread = &k0->base;
    report_last = host_ticks_ns();
    /* four carriers (the browser's thread included) measured fastest: more cores mostly add contention for the global
       lock. ?carriers=N in the page address overrides */
    int want = emscripten_num_logical_cores() < 4 ? emscripten_num_logical_cores() : 4;
    const char* env = getenv("UEW_CARRIERS");
    if (env && atoi(env) > 0) want = atoi(env);
    if (host_query_int("carriers") > 0) want = host_query_int("carriers");
    if (want > MAX_CARRIERS) want = MAX_CARRIERS;
    if (want < 1) want = 1;
    rt_lock();
    for (int i = 1; i < want; i++) {
        Carrier* k = &carriers[i];
        k->id = i;
        k->prio_next = ~0ull;
        pthread_t th;
        if (pthread_create(&th, NULL, carrier_main, k) != 0) break;
        pthread_detach(th);
        ncarriers = i + 1;
    }
    host_log("[thread] %d carriers (real threads)", ncarriers);
}

static void switch_to(GThread* t);
static GThread* pick(void);

static void thread_entry(void* arg)
{
    GThread* t = arg;
    uint64_t a = t->param;
    rt_unlock();                  /* switched in with the lock held; guest code runs without it */
    t->cpu->r[0] = rt_invoke(t->cpu, t->start, 1, &a);
    rt_lock();
    t->exit_code = (uint32_t)t->cpu->r[0];
    t->done = 1;
    K->nthreads--;
    K->load -= weight(t->name);
    total_threads--;
    K->ndead++;
    wake_carriers();
    sched_yield_blocked(t->cpu);
    for (;;) switch_to(&K->base);
}

static void* carrier_main(void* arg)
{
    Carrier* k = arg;
    K = k;
    rt_lock();
    k->base.ctx = host_ctx_main();
    k->base.carrier = k->id;
    k->current = &k->base;
    k->run_since = k->slice_start = host_ticks_ns();
    for (;;) {
        GThread* t = pick();
        if (t) switch_to(t);
        else lock_wait(4000000ull);
    }
    return NULL;
}

/* UE creates its threads suspended, with the FRunnableThread as the parameter and its name (an FString: pointer,
   length with the terminator) already stored in it */
static void ue_name(GThread* t, uint64_t obj)
{
    if (obj < 0x10000 || obj + 0x200 > MEM_SIZE) return;
    for (uint64_t off = 8; off < 0x200; off += 8) {
        uint64_t p = LD64(obj + off);
        uint32_t n = (uint32_t)LD32(obj + off + 8);
        if (p < 0x10000 || p + 128 > MEM_SIZE || n < 4 || n > 40) continue;
        uint32_t i = 0;
        for (; i < n - 1; i++) { uint16_t ch = (uint16_t)LD16(p + 2ull * i); if (ch < 0x20 || ch > 0x7E) break; }
        if (i != n - 1 || LD16(p + 2ull * i)) continue;
        for (i = 0; i < n - 1 && i < sizeof t->name - 1; i++) t->name[i] = (char)LD16(p + 2ull * i);
        t->name[i] = 0;
        return;
    }
}
/* threads that call the browser all the time stay on the page's thread. The rendering thread too: moved to another
   core (?rtoffpage=1, when there is an RHI thread) its own calls got slower than the core it freed (Dead as Disco) */
static int has_rhi_thread;
/* how busy a new thread is likely to be, by name: threads stay on the carrier they start on, so the few that run all
   the time (Unreal's workers and render thread, audio mixers) are spread out first; the many that mostly wait count little */
static int weight(const char* n)
{
    static const char* busy[] = { "Worker", "RenderThread", "FMOD mixer", "Mixer", "TaskGraph", "GameThread" };
    for (size_t i = 0; i < sizeof busy / sizeof *busy; i++) if (strstr(n, busy[i])) return 16;
    return 1;
}
static int on_page(const char* n)
{
    if (strstr(n, "RHIThread") || strstr(n, "RHISubmission")) { has_rhi_thread = 1; return 1; }
    if (strstr(n, "RenderThread")) return !has_rhi_thread || !host_query_flag("rtoffpage");
    return strstr(n, "Audio") != NULL;
}
void sched_named(GThread* t) { (void)t; }

GThread* sched_create(uint64_t start, uint64_t param, int suspended)
{
    GThread* t = calloc(1, sizeof *t);
    t->tid = next_tid;
    next_tid += 4;
    t->start = start;
    t->param = param;
    t->suspended = suspended;
    t->cpu = calloc(1, sizeof(CPU));
    t->cpu->mxcsr = 0x1F80;
    t->cpu->fcw = 0x27F;
    t->cpu->tid = t->tid;
    uint64_t stack_size = 4ull << 20;
    uint64_t stack = g_alloc(stack_size);
    t->stack = stack;
    t->cpu->gsbase = rt_make_teb(stack, stack_size, t->tid);
    t->cpu->r[4] = (stack + stack_size - 64) & ~15ull;
    /* the carrier with the fewest threads. Threads that talk to the browser all the time (rendering, audio) go on
       carrier 0, the page's own thread, where those calls are direct; the rest share the other cores */
    ue_name(t, param);
    int best = ncarriers > 1 ? 1 : 0;
    for (int i = 1; i < ncarriers; i++) if (carriers[i].load < carriers[best].load) best = i;
    if (on_page(t->name)) best = 0;
    t->carrier = best;
    t->lockd = 1;
    t->ctx = host_ctx_new(thread_entry, t, 4u << 20);
    if (!t->ctx) {
        host_log("[thread] can't create host context; thread %d won't run", t->tid);
        return t;
    }
    Carrier* k = &carriers[best];
    k->load += weight(t->name);
    t->next = k->threads;
    k->threads = t;
    k->nthreads++;
    t->all_next = all_threads;
    all_threads = t;
    total_threads++;
    wake_carriers();
    return t;
}

GThread* sched_find_blocked(SchedCheck check, uint64_t a, uint64_t b)
{
    for (GThread* t = all_threads; t; t = t->all_next)
        if (t->check == check && t->block_a == a && t->block_b == b && !t->done) return t;
    return NULL;
}
void sched_name_check(SchedCheck check, const char* name, void (*desc)(GThread* t, char* out, int n))
{
    for (int i = 0; i < ncheck_names; i++) if (check_names[i].fn == check) return;
    if (ncheck_names < 16) { check_names[ncheck_names].fn = check; check_names[ncheck_names].name = name; check_names[ncheck_names].desc = desc; ncheck_names++; }
}
static int runnable(GThread* t, uint64_t now)
{
    if (t->done || t->suspended || !t->ctx || t->wake_at > now) return 0;
    return !t->check || now >= t->block_until || t->check(t);
}
static void reap(void)
{
    Carrier* k = K;
    for (GThread** pp = &k->threads; *pp;) {
        GThread* t = *pp;
        if (t->done && t != k->current && t != &k->base) {
            *pp = t->next;
            t->next = NULL;
            if (t->ctx) { host_ctx_free(t->ctx); t->ctx = NULL; }
            if (t->stack) { g_free(t->stack); t->stack = 0; }
            k->ndead--;
            continue;
        }
        pp = &t->next;
    }
}
static GThread* pick(void)
{
    Carrier* k = K;
    if (k->ndead > 0) reap();
    uint64_t now = host_ticks_ns();
    GThread* cur = k->current;
    if (k->nframe_waiters)
        for (GThread* t = k->threads; t; t = t->next) if (t->frame_wait && t != cur && t != &k->base && runnable(t, now)) return t;
    if (k->nprio)
        for (GThread* t = k->threads; t; t = t->next) if (t->prio && t != cur && runnable(t, now)) return t;
    /* the carrier's base context is only a place to wait (carrier 0's base is the guest main thread, a real one) */
    int base_runs = k->id == 0 && base_is_guest;
    if (cur->done || (cur == &k->base && !base_runs)) {
        for (GThread* t = k->threads; t; t = t->next) if (t != cur && (t != &k->base || base_runs) && runnable(t, now)) return t;
        return NULL;
    }
    GThread* t = cur;
    for (int i = 0; i < 1000000; i++) {
        t = t->next ? t->next : k->threads;
        if (t == cur) break;
        if (t == &k->base && !base_runs) continue;
        if (runnable(t, now)) return t;
    }
    return NULL;
}
static void sched_idle(uint64_t deadline)
{
    Carrier* k = K;
    uint64_t now = host_ticks_ns(), until = deadline;
    for (GThread* t = k->threads; t; t = t->next) {
        if (t == k->current || t->done || t->suspended || !t->ctx) continue;
        if (t->wake_at > now && t->wake_at < until) until = t->wake_at;
        if (t->check && t->block_until < until) until = t->block_until;
    }
    if (until > now + 4000000ull) until = now + 4000000ull;
    k->current->cpu_ns += now - k->run_since;
    __atomic_fetch_add(&idle_n, 1, __ATOMIC_SEQ_CST);
    if (k->id == 0) {
        /* the browser's thread: let its event loop run (downloads, frames, audio, calls from other cores) */
        unlocked_call(host_idle, until > now ? until - now : 0);
    } else lock_wait(until > now ? until - now : 0);
    __atomic_fetch_sub(&idle_n, 1, __ATOMIC_SEQ_CST);
    k->run_since = host_ticks_ns();
    k->idle_ns += k->run_since - now;
    idle_by[17] += k->run_since - now;
}
int sched_block(CPU* c, SchedCheck check, uint64_t deadline)
{
    GThread* me = K->current;
    for (;;) {
        if (check(me)) return 1;
        if (host_ticks_ns() >= deadline) return 0;
        me->check = check;
        me->block_until = deadline;
        int switched = sched_yield(c);
        me->check = NULL;
        if (!switched) sched_idle(deadline);
    }
}
int sched_block_ext(CPU* c, SchedCheck check, uint64_t deadline)
{
    host_ext_waits++;
    int r = sched_block(c, check, deadline);
    host_ext_waits--;
    return r;
}
static int frame_check(GThread* t) { return host_frame_seq() != (uint32_t)t->block_a; }
void sched_frame_report(void)
{
    static int reports;
    uint64_t now = host_ticks_ns(), last = report_last;
    if (last && now - last > 100000000ull && reports < 300) {
        char line[512];
        int n = snprintf(line, sizeof line, "[hitch] %.0f ms frame:", (now - last) / 1e6);
        for (int k = 0; k < 6; k++) {
            GThread* best = NULL;
            for (GThread* t = all_threads; t; t = t->all_next)
                if (t->cpu_ns > 2000000ull && (!best || t->cpu_ns > best->cpu_ns)) best = t;
            if (!best) break;
            n += snprintf(line + n, sizeof line - n, " %s(c%d) %.0f ms,", best->name[0] ? best->name : "thread", best->carrier, best->cpu_ns / 1e6);
            if (n >= (int)sizeof line) break;
            best->cpu_ns = 0;
        }
        if (n < (int)sizeof line) n += snprintf(line + n, sizeof line - n, " lock waits:");
        for (int i = 0; i < ncarriers && n < (int)sizeof line; i++)
            if (lock_wait_ns[i] > 1000000ull) n += snprintf(line + n, sizeof line - n, " c%d %.0f ms", i, lock_wait_ns[i] / 1e6);
        host_log("%s", line);
        rt_lock_report();
        reports++;
    }
    /* ?perf=1: every 5 s, the average frame: each thread's run time and core, each core's idle time and lock waits */
    static int perf = -1;
    static uint64_t perf_t0, perf_frames, perf_lock[MAX_CARRIERS], perf_idle0[MAX_CARRIERS];
    if (perf < 0) { perf = host_query_flag("perf"); perf_t0 = now; for (int i = 0; i < ncarriers; i++) perf_idle0[i] = carriers[i].idle_ns; }
    if (perf) {
        perf_frames++;
        for (int i = 0; i < ncarriers; i++) perf_lock[i] += lock_wait_ns[i];
        for (GThread* t = all_threads; t; t = t->all_next) t->acc_ns += t->cpu_ns;
        if (now - perf_t0 >= 5000000000ull) {
            char line[900];
            double fr = (double)perf_frames;
            int n = snprintf(line, sizeof line, "[perf] %.1f fps, %.1f ms/frame;", fr * 1e9 / (now - perf_t0), (now - perf_t0) / 1e6 / fr);
            for (int i = 0; i < ncarriers && n < (int)sizeof line - 80; i++) {
                n += snprintf(line + n, sizeof line - n, " c%d idle %.1f lock %.1f,", i, (carriers[i].idle_ns - perf_idle0[i]) / 1e6 / fr, perf_lock[i] / 1e6 / fr);
                perf_idle0[i] = carriers[i].idle_ns;
                perf_lock[i] = 0;
            }
            for (int k = 0; k < 8; k++) {
                GThread* best = NULL;
                for (GThread* t = all_threads; t; t = t->all_next)
                    if (t->acc_ns && (!best || t->acc_ns > best->acc_ns)) best = t;
                if (!best || n >= (int)sizeof line - 60) break;
                n += snprintf(line + n, sizeof line - n, " %s(c%d) %.1f,", best->name[0] ? best->name : "thread", best->carrier, best->acc_ns / 1e6 / fr);
                best->acc_ns = 0;
            }
            host_log("%s", line);
            rt_lock_report();
            for (GThread* t = all_threads; t; t = t->all_next) t->acc_ns = 0;
            perf_t0 = now;
            perf_frames = 0;
        }
    }
    memset(idle_by, 0, sizeof idle_by);
    memset(lock_wait_ns, 0, sizeof lock_wait_ns);
    for (GThread* t = all_threads; t; t = t->all_next) t->cpu_ns = 0;
    report_last = now;
}
void sched_wait_frame(CPU* c)
{
    Carrier* k = K;
    GThread* me = k->current;
    me->block_a = host_frame_seq();
    host_frame_request();
    me->frame_wait = 1;
    k->nframe_waiters++;
    sched_block(c, frame_check, ~0ull);
    me->frame_wait = 0;
    if (!--k->nframe_waiters) host_frame_done();
}
static void switch_to(GThread* t)
{
    Carrier* k = K;
    if (t == k->current) return;
    uint64_t now = host_ticks_ns();
    k->current->cpu_ns += now - k->run_since;
    k->nswitch++;
    k->current->lockd = lock_depth;
    k->current = t;
    host_ctx_switch(t->ctx);
    lock_depth = K->current->lockd;
    K->slice_start = K->run_since = host_ticks_ns();
}

void sched_tick(CPU* c)
{
    Carrier* k = K;
    if (k->id == 0) host_browser_turn();
    if (k->nthreads <= 1) return;
    uint64_t now = host_ticks_ns();
    if (now >= k->prio_next) {
        k->prio_next = ~0ull;
        if (!k->current->prio) { sched_yield(c); return; }
    }
    if (now - k->slice_start < 4000000ull) return;
    k->slice_start = now;
    sched_yield(c);
}

int sched_yield(CPU* c)
{
    (void)c;
    GThread* t = pick();
    if (!t) return 0;
    switch_to(t);
    return 1;
}

int sched_yield_blocked(CPU* c) { return sched_yield(c); }

void sched_sleep(CPU* c, uint32_t ms)
{
    Carrier* k = K;
    uint64_t wake = host_ticks_ns() + (uint64_t)ms * 1000000ull;
    k->current->wake_at = wake;
    if (k->current->prio && wake < k->prio_next) k->prio_next = wake;
    while (!sched_yield(c) && ms && host_ticks_ns() < wake) sched_idle(wake);
    K->current->wake_at = 0;
}

void sched_exit_current(CPU* c, uint32_t code)
{
    Carrier* k = K;
    if (k->current == main_thread) {
        rt_exit((int)code);
        return;
    }
    k->current->exit_code = code;
    k->current->done = 1;
    k->nthreads--;
    k->load -= weight(k->current->name);
    total_threads--;
    k->ndead++;
    sched_yield(c);
    for (;;) switch_to(&K->base);
}

void rt_pause(CPU* c)
{
    (void)c;
    /* a spin-wait: let the other cores' threads get on; on this carrier, let its other green threads run now and then */
    static __thread unsigned n;
    if ((++n & 63) == 0) { rt_lock(); sched_yield(c); rt_unlock(); }
}
void sched_set_prio(void) { if (!K->current->prio) { K->current->prio = 1; K->nprio++; } }
#endif
