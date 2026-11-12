#ifndef UEW_THREADS
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GThread main_thread;
static GThread* threads = &main_thread;
static GThread* current = &main_thread;
static int next_tid = 0x1008;
static int nthreads = 1;
static uint64_t run_since, idle_ns, nswitch;
static int ndead, nprio;
static uint64_t prio_next = ~0ull;   /* earliest end of a sleep by a time-critical thread */
static struct { SchedCheck fn; const char* name; void (*desc)(GThread*, char*, int); } check_names[16];
static int ncheck_names;
/* idle time by what the threads were waiting on: index into check_names, 16 = sleeping, 17 = other */
static uint64_t idle_by[18];
static uint64_t report_last;

int sched_thread_count(void) { return nthreads; }

/* debug: what each green thread is doing (Module._uew_threads() from the page) */
void sched_note_wait(CPU* c, uint32_t n, uint64_t h, uint32_t ms)
{
    current->wait_n = n;
    current->wait_h = h;
    current->wait_ms = ms;
    current->wait_ret = n ? LD64(c->r[4]) : 0;
    current->wait_since = n ? host_ticks_ns() : 0;
}
#ifdef __EMSCRIPTEN__
__attribute__((used, visibility("default")))
#endif
void uew_threads(void)
{
    uint64_t now = host_ticks_ns();
    for (GThread* t = threads; t; t = t->next) {
        if (t->done) continue;
        const char* st = t->suspended ? "suspended" : t == current ? "running" : t->wait_n ? "waiting" : t->check ? "blocked" : t->wake_at > now ? "sleeping" : "ready";
        char what[200] = "";
        if (t->check) {
            int k = 0;
            while (k < ncheck_names && check_names[k].fn != t->check) k++;
            if (k < ncheck_names) {
                int n = snprintf(what, sizeof what, " on %s", check_names[k].name);
                if (check_names[k].desc && n < (int)sizeof what) check_names[k].desc(t, what + n, (int)sizeof what - n);
            } else snprintf(what, sizeof what, " on check %p a=%llx", (void*)t->check, (unsigned long long)t->block_a);
        } else if (t->wake_at > now) snprintf(what, sizeof what, " for %.0f ms", (t->wake_at - now) / 1e6);
        host_log("[thr] %u %-24s %s%s, %.1f s, start=%llx ret=%llx", (unsigned)t->tid, t->name, st, what,
                 t->wait_n ? (now - t->wait_since) / 1e9 : 0.0, (unsigned long long)t->start, (unsigned long long)t->wait_ret);
    }
}
unsigned sched_current_id(void) { return (unsigned)current->tid; }
int sched_is_main(void) { return current == &main_thread; }
GThread* sched_current(void) { return current; }
GThread* sched_by_tid(uint32_t tid) { for (GThread* t = threads; t; t = t->next) if ((uint32_t)t->tid == tid) return t; return NULL; }

static int frame_check(GThread* t);
void sched_init(CPU* cpu)
{
    sched_name_check(frame_check, "next browser frame", NULL);
    main_thread.tid = cpu->tid;
    main_thread.cpu = cpu;
    main_thread.started = 1;
    main_thread.ctx = host_ctx_main();
    report_last = host_ticks_ns();
}

static void switch_to(GThread* t);
static void thread_entry(void* arg)
{
    GThread* t = arg;
    uint64_t a = t->param;
    t->cpu->r[0] = rt_invoke(t->cpu, t->start, 1, &a);
    t->exit_code = (uint32_t)t->cpu->r[0];
    t->done = 1;
    nthreads--;
    ndead++;
    sched_yield_blocked(t->cpu);
    for (;;) switch_to(&main_thread);
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
    t->ctx = host_ctx_new(thread_entry, t, 4u << 20);
    if (!t->ctx) {
        host_log("[thread] can't create host context; thread %d won't run", t->tid);
        return t;
    }
    t->next = threads;
    threads = t;
    nthreads++;
    return t;
}

static int nframe_waiters;
GThread* sched_find_blocked(SchedCheck check, uint64_t a, uint64_t b)
{
    for (GThread* t = threads; t; t = t->next)
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
/* exited threads leave the run list and give back their stacks (8 MB each); the GThread itself stays, since its
   handle still answers "exited" */
static void reap(void)
{
    for (GThread** pp = &threads; *pp;) {
        GThread* t = *pp;
        if (t->done && t != current && t != &main_thread) {
            *pp = t->next;
            t->next = NULL;
            if (t->ctx) { host_ctx_free(t->ctx); t->ctx = NULL; }
            if (t->stack) { g_free(t->stack); t->stack = 0; }
            ndead--;
            continue;
        }
        pp = &t->next;
    }
}
static GThread* pick(void)
{
    if (ndead > 0) reap();
    uint64_t now = host_ticks_ns();
    /* a thread whose frame came gets the CPU first (it is the one presenting) */
    if (nframe_waiters)
        for (GThread* t = threads; t; t = t->next) if (t->frame_wait && t != current && runnable(t, now)) return t;
    if (nprio)
        for (GThread* t = threads; t; t = t->next) if (t->prio && t != current && runnable(t, now)) return t;
    GThread* t = current;
    /* current may have been unlinked (it exited): walk the whole list once from the head then */
    if (current->done && current != &main_thread) {
        for (t = threads; t; t = t->next) if (t != current && runnable(t, now)) return t;
        return NULL;
    }
    for (int i = 0; i < 1000000; i++) {
        t = t->next ? t->next : threads;
        if (t == current) break;
        if (runnable(t, now)) return t;
    }
    return NULL;
}
/* nothing else can run: wait for the browser until the next timed wakeup (or the frame callback) */
static void sched_idle(uint64_t deadline)
{
    uint64_t now = host_ticks_ns(), until = deadline;
    for (GThread* t = threads; t; t = t->next) {
        if (t == current || t->done || t->suspended || !t->ctx) continue;
        if (t->wake_at > now && t->wake_at < until) until = t->wake_at;
        if (t->check && t->block_until < until) until = t->block_until;
    }
    /* objects can also be signalled from outside the threads (audio, input), so look again within 4 ms */
    if (until > now + 4000000ull) until = now + 4000000ull;
    current->cpu_ns += now - run_since;
    host_idle(until > now ? until - now : 0);
    run_since = host_ticks_ns();
    idle_ns += run_since - now;
    /* blame the most telling wait: downloads and GPU work before locks and sleeps */
    static const char* order[] = { "file download", "shader compile", "GPU readback", "next browser frame" };
    int why = 17;
    for (GThread* t = threads; t; t = t->next) {
        if (t->done || !t->ctx) continue;
        int k = 17;
        if (t->check) { for (k = 0; k < ncheck_names && check_names[k].fn != t->check; k++) {} if (k == ncheck_names) k = 17; }
        else if (t->wake_at) k = 16;
        int rank = 99, best = 99;
        for (int o = 0; o < 4; o++) {
            if (k < ncheck_names && check_names[k].name == order[o]) rank = o;
            if (why < ncheck_names && check_names[why].name == order[o]) best = o;
        }
        if (rank < best || (rank == 99 && best == 99 && k < why)) why = k;
    }
    idle_by[why] += run_since - now;
}
int sched_block(CPU* c, SchedCheck check, uint64_t deadline)
{
    GThread* me = current;
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
/* a long frame: which threads had the CPU during it */
void sched_frame_report(void)
{
    static int reports;
    uint64_t now = host_ticks_ns(), last = report_last;
    current->cpu_ns += now - run_since;
    run_since = now;
    if (last && now - last > 100000000ull && reports < 300) {
        char line[512];
        int n = snprintf(line, sizeof line, "[hitch] %.0f ms frame, %llu switches, idle %.0f ms:", (now - last) / 1e6,
                         (unsigned long long)nswitch, idle_ns / 1e6);
        for (int k = 0; k < 5; k++) {
            GThread* best = NULL;
            for (GThread* t = threads; t; t = t->next)
                if (t->cpu_ns > 2000000ull && (!best || t->cpu_ns > best->cpu_ns)) best = t;
            if (!best) break;
            n += snprintf(line + n, sizeof line - n, " %s %.0f ms,", best->name[0] ? best->name : best == &main_thread ? "main" : "thread", best->cpu_ns / 1e6);
            if (n >= (int)sizeof line) break;
            best->cpu_ns = 0;
        }
        n += snprintf(line + n, sizeof line - n, " idle waiting on");
        for (int k = 0; k < 18 && n < (int)sizeof line; k++)
            if (idle_by[k] > 5000000ull)
                n += snprintf(line + n, sizeof line - n, " %s %.0f ms,", k < ncheck_names ? check_names[k].name : k == 16 ? "sleep" : "other", idle_by[k] / 1e6);
        host_log("%s", line);
        reports++;
    }
    memset(idle_by, 0, sizeof idle_by);
    for (GThread* t = threads; t; t = t->next) t->cpu_ns = 0;
    idle_ns = nswitch = 0;
    report_last = now;
}
void sched_wait_frame(CPU* c)
{
    GThread* me = current;
    me->block_a = host_frame_seq();
    host_frame_request();
    me->frame_wait = 1;
    nframe_waiters++;
    sched_block(c, frame_check, ~0ull);
    me->frame_wait = 0;
    if (!--nframe_waiters) host_frame_done();
}
static uint64_t slice_start;
static void switch_to(GThread* t)
{
    if (t == current) return;
    uint64_t now = host_ticks_ns();
    current->cpu_ns += now - run_since;
    nswitch++;
    current = t;
    host_ctx_switch(t->ctx);
    slice_start = run_since = host_ticks_ns();
}

/* a thread that polls through system calls without ever blocking (lock/check/unlock loops) would starve the others
   on one core: after a few ms of running, hand over */
void sched_tick(CPU* c)
{
    host_browser_turn();
    if (nthreads <= 1) return;
    uint64_t now = host_ticks_ns();
    if (now >= prio_next) {
        prio_next = ~0ull;
        if (!current->prio) { sched_yield(c); return; }
    }
    if (now - slice_start < 4000000ull) return;
    slice_start = now;
    sched_yield(c);
}

int sched_yield(CPU* c)
{
    (void)c;
    if (nthreads <= 1) return 0;
    GThread* t = pick();
    if (!t) return 0;
    switch_to(t);
    return 1;
}

int sched_yield_blocked(CPU* c) { return sched_yield(c); }

void sched_sleep(CPU* c, uint32_t ms)
{
    if (nthreads <= 1) {
        if (ms) host_sleep_ms(ms > 50 ? 50 : ms);
        return;
    }
    uint64_t wake = host_ticks_ns() + (uint64_t)ms * 1000000ull;
    current->wake_at = wake;
    if (current->prio && wake < prio_next) prio_next = wake;
    while (!sched_yield(c) && ms && host_ticks_ns() < wake) sched_idle(wake);
    current->wake_at = 0;
}

void sched_exit_current(CPU* c, uint32_t code)
{
    if (current == &main_thread) {
        rt_exit((int)code);
        return;
    }
    current->exit_code = code;
    current->done = 1;
    nthreads--;
    ndead++;
    sched_yield(c);
    for (;;) switch_to(&main_thread);
}

void rt_pause(CPU* c)
{
    static unsigned n;
    if ((++n & 63) == 0) sched_yield(c);
}
CPU* sched_current_cpu(void) { return current->cpu; }
void sched_set_prio(void) { if (!current->prio) { current->prio = 1; nprio++; } }
/* one host thread: nothing to lock */
void rt_lock(void) {}
void rt_unlock(void) {}
#endif
