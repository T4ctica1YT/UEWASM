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
    int nthreads, ndead, nprio, nframe_waiters;
    uint64_t prio_next, run_since, idle_ns, nswitch, slice_start;
} Carrier;

static Carrier carriers[MAX_CARRIERS];
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

void rt_lock(void) { if (lock_depth++ == 0) pthread_mutex_lock(&big_lock); }
void rt_unlock(void) { if (--lock_depth == 0) pthread_mutex_unlock(&big_lock); }

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
    rt_lock();
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
    rt_unlock();
}
unsigned sched_current_id(void) { return (unsigned)K->current->tid; }
int sched_is_main(void) { return K->current == &carriers[0].base; }
GThread* sched_current(void) { return K->current; }
CPU* sched_current_cpu(void) { return K->current->cpu; }
GThread* sched_by_tid(uint32_t tid) { for (GThread* t = all_threads; t; t = t->all_next) if ((uint32_t)t->tid == tid) return t; return NULL; }

static int frame_check(GThread* t);
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
    report_last = host_ticks_ns();
    /* one carrier per core, the browser's main thread included */
    int want = emscripten_num_logical_cores();
    const char* env = getenv("UEW_CARRIERS");
    if (env && atoi(env) > 0) want = atoi(env);
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
    for (;;) {
        GThread* t = pick();
        if (t) switch_to(t);
        else lock_wait(4000000ull);
    }
    return NULL;
}

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
    /* the carrier with the fewest threads; the browser's own thread (carrier 0) keeps only what starts there */
    int best = ncarriers > 1 ? 1 : 0;
    for (int i = 1; i < ncarriers; i++) if (carriers[i].nthreads < carriers[best].nthreads) best = i;
    t->carrier = best;
    t->lockd = 1;
    t->ctx = host_ctx_new(thread_entry, t, 4u << 20);
    if (!t->ctx) {
        host_log("[thread] can't create host context; thread %d won't run", t->tid);
        return t;
    }
    Carrier* k = &carriers[best];
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
    int base_runs = k->id == 0;
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
    if (k->id == 0) {
        /* the browser's thread: let its event loop run (downloads, frames, audio) */
        unlocked_call(host_idle, until > now ? until - now : 0);
    } else lock_wait(until > now ? until - now : 0);
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
        host_log("%s", line);
        reports++;
    }
    memset(idle_by, 0, sizeof idle_by);
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
    if (k->current == &carriers[0].base) {
        rt_exit((int)code);
        return;
    }
    k->current->exit_code = code;
    k->current->done = 1;
    k->nthreads--;
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
