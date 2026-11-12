#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>

uint64_t handle_new(int kind, void* obj);
void* handle_obj(uint64_t h);
#define H_OBJ 5

enum { O_EVENT = 1, O_SEM, O_MUTEX, O_THREAD };
typedef struct Obj { int type, manual, signaled; int64_t count, max; GThread* thread; uint32_t owner; } Obj;

static uint64_t new_obj(int type)
{
    Obj* o = calloc(1, sizeof *o);
    o->type = type;
    return handle_new(H_OBJ, o);
}

void hle_set_error(uint32_t e);
HLE(CreateEventW) { hle_set_error(0); uint64_t h = new_obj(O_EVENT); Obj* o = handle_obj(h); o->manual = (int)A2; o->signaled = (int)A3; RET(h); }
HLE(CreateEventA) { hle_CreateEventW(c); }
HLE(CreateEventExW) { hle_set_error(0); uint64_t h = new_obj(O_EVENT); Obj* o = handle_obj(h); o->manual = (A3 & 1) != 0; o->signaled = (A3 & 2) != 0; RET(h); }
HLE(OpenEventW) { RET(0); }
HLE(SetEvent) { Obj* o = handle_obj(A1); if (o) o->signaled = 1; sched_notify(); RET(o != NULL); }
/* completed overlapped I/O signals the OVERLAPPED's event */
void hle_event_signal(uint64_t h) { Obj* o = handle_obj(h); if (o && o->type == O_EVENT) o->signaled = 1; }
void sync_set_event(uint64_t h) { Obj* o = handle_obj(h); if (o && o->type == O_EVENT) o->signaled = 1; }
HLE(ResetEvent) { Obj* o = handle_obj(A1); if (o) o->signaled = 0; RET(o != NULL); }
HLE(PulseEvent) { Obj* o = handle_obj(A1); if (o) o->signaled = 0; RET(o != NULL); }
HLE(CreateSemaphoreW) { hle_set_error(0); uint64_t h = new_obj(O_SEM); Obj* o = handle_obj(h); o->count = (int32_t)A2; o->max = (int32_t)A3; RET(h); }
HLE(CreateSemaphoreA) { hle_CreateSemaphoreW(c); }
HLE(CreateSemaphoreExW) { hle_CreateSemaphoreW(c); }
HLE(ReleaseSemaphore) { Obj* o = handle_obj(A1); if (!o) { RET(0); return; } if (A3) ST32(A3, (uint64_t)o->count); o->count += (int32_t)A2; sched_notify(); RET(1); }
static uint64_t new_mutex(int owned)
{
    hle_set_error(0);
    uint64_t h = new_obj(O_MUTEX);
    Obj* o = handle_obj(h);
    if (owned) { o->owner = sched_current_id(); o->count = 1; }
    return h;
}
HLE(CreateMutexW) { RET(new_mutex(A2 != 0)); }
HLE(CreateMutexA) { RET(new_mutex(A2 != 0)); }
HLE(CreateMutexExW) { RET(new_mutex((A3 & 1) != 0)); }
HLE(OpenMutexW) { RET(0); }
HLE(ReleaseMutex)
{
    Obj* o = handle_obj(A1);
    if (!o || o->type != O_MUTEX || o->owner != sched_current_id()) { hle_set_error(288); RET(0); return; }
    if (--o->count <= 0) { o->count = 0; o->owner = 0; sched_notify(); }
    RET(1);
}

static int ready(Obj* o, int consume, uint32_t tid)
{
    if (!o) return 1;
    switch (o->type) {
    case O_MUTEX:
        if (o->owner && o->owner != tid) {
            /* held by a thread that has exited: abandoned, the next waiter gets it */
            GThread* ow = sched_by_tid(o->owner);
            if (ow && !ow->done) return 0;
            if (consume) o->count = 0;
        }
        if (consume) { o->owner = tid; o->count++; }
        return 1;
    case O_EVENT:
        if (!o->signaled) return 0;
        if (consume && !o->manual) o->signaled = 0;
        return 1;
    case O_SEM:
        if (o->count <= 0) return 0;
        if (consume) o->count--;
        return 1;
    case O_THREAD:
        return o->thread == NULL || o->thread->done;
    default:
        return 1;
    }
}

static uint32_t wait_loop(CPU* c, uint32_t n, const uint64_t* hs, int all, uint32_t ms);
uint32_t wait_many(CPU* c, uint32_t n, const uint64_t* hs, int all, uint32_t ms)
{
    sched_note_wait(c, n, hs[0], ms);
    uint32_t r = wait_loop(c, n, hs, all, ms);
    sched_note_wait(c, 0, 0, 0);
    return r;
}

static int wait_check(GThread* t)
{
    for (uint32_t i = 0; i < t->block_n; i++) {
        int r = ready(handle_obj(t->block_hs[i]), 0, (uint32_t)t->tid);
        if (t->block_all ? !r : r) return !t->block_all;
    }
    return t->block_all;
}
/* thread dump: what each handle of a blocked wait is */
static void wait_desc(GThread* t, char* out, int cap)
{
    int n = snprintf(out, cap, " (%s of %u, %s)", t->block_all ? "all" : "any", t->block_n,
                     t->wait_ms == 0xFFFFFFFF ? "no timeout" : "with timeout");
    for (uint32_t i = 0; i < t->block_n && i < 4 && n < cap; i++) {
        uint64_t h = t->block_hs[i];
        Obj* o = handle_obj(h);
        if (!o) n += snprintf(out + n, cap - n, " %llx:unknown", (unsigned long long)h);
        else if (o->type == O_EVENT) n += snprintf(out + n, cap - n, " %llx:event(%s,%s)", (unsigned long long)h, o->manual ? "manual" : "auto", o->signaled ? "set" : "unset");
        else if (o->type == O_SEM) n += snprintf(out + n, cap - n, " %llx:semaphore(%lld)", (unsigned long long)h, (long long)o->count);
        else if (o->type == O_MUTEX) n += snprintf(out + n, cap - n, " %llx:mutex(owner %u x%lld)", (unsigned long long)h, o->owner, (long long)o->count);
        else if (o->type == O_THREAD) n += snprintf(out + n, cap - n, " %llx:thread(%d%s)", (unsigned long long)h, o->thread ? o->thread->tid : 0,
                                                   o->thread && o->thread->done ? " exited" : "");
    }
}
static int cv_check(GThread* t);
static int addr_differs(GThread* t);
static void cv_desc(GThread* t, char* out, int cap) { snprintf(out, cap, " %llx", (unsigned long long)t->block_a); }
INIT_FN(sync_names)
{
    sched_name_check(wait_check, "wait", wait_desc);
    sched_name_check(cv_check, "condition variable", cv_desc);
    sched_name_check(addr_differs, "WaitOnAddress", cv_desc);
}
static uint32_t wait_loop(CPU* c, uint32_t n, const uint64_t* hs, int all, uint32_t ms)
{
    uint64_t deadline = ms == 0xFFFFFFFF ? ~0ull : host_ticks_ns() + (uint64_t)ms * 1000000ull;
    uint32_t me = sched_current_id();
    GThread* t = sched_current();
    t->block_hs = hs;
    t->block_n = n;
    t->block_all = all;
    for (;;) {
        if (all) {
            int ok = 1;
            for (uint32_t i = 0; i < n; i++) ok &= ready(handle_obj(hs[i]), 0, me);
            if (ok) {
                for (uint32_t i = 0; i < n; i++) ready(handle_obj(hs[i]), 1, me);
                return 0;
            }
        } else {
            for (uint32_t i = 0; i < n; i++)
                if (ready(handle_obj(hs[i]), 1, me)) return i;
        }
        if (host_ticks_ns() >= deadline) return 0x102;
        if (sched_thread_count() <= 1 && ms == 0xFFFFFFFF) {
            static int warned;
            if (!warned++) host_log("[sync] wait would block forever with no other thread; continuing");
            return 0;
        }
        sched_block(c, wait_check, deadline);
    }
}

HLE(WaitForSingleObject) { uint64_t h = A1; RET(wait_many(c, 1, &h, 0, (uint32_t)A2)); }
HLE(WaitForSingleObjectEx) { uint64_t h = A1; RET(wait_many(c, 1, &h, 0, (uint32_t)A2)); }
HLE(WaitForMultipleObjects)
{
    uint32_t n = (uint32_t)A1;
    uint64_t hs[64];
    for (uint32_t i = 0; i < n && i < 64; i++) hs[i] = LDP(A2 + (uint64_t)GP * i);
    RET(wait_many(c, n, hs, (int)A3, (uint32_t)A4));
}
HLE(WaitForMultipleObjectsEx) { hle_WaitForMultipleObjects(c); }
HLE(SignalObjectAndWait) { Obj* o = handle_obj(A1); if (o && o->type == O_EVENT) o->signaled = 1; sched_notify(); uint64_t h = A2; RET(wait_many(c, 1, &h, 0, (uint32_t)A3)); }

/* condition variables: the word counts wakes; a sleeper drops its lock and blocks until the count moves (waking
   every sleeper on WakeConditionVariable is allowed: callers must recheck, as wakeups may be spurious on Windows) */
HLE(InitializeConditionVariable) { STP(A1, 0); RET(0); }
HLE(WakeConditionVariable) { STP(A1, LDP(A1) + 4); sched_notify(); RET(0); }
HLE(WakeAllConditionVariable) { STP(A1, LDP(A1) + 4); sched_notify(); RET(0); }
static int cv_check(GThread* t) { return LDP(t->block_a) != t->block_b; }
static int cv_sleep(CPU* c, uint64_t cv, uint32_t ms)
{
    GThread* t = sched_current();
    t->block_a = cv;
    t->block_b = LDP(cv);
    uint64_t deadline = ms == 0xFFFFFFFF ? ~0ull : host_ticks_ns() + (uint64_t)ms * 1000000ull;
    if (sched_block(c, cv_check, deadline)) return 1;
    hle_set_error(1460);
    return 0;
}
void cs_enter(CPU* c, uint64_t cs);
void cs_leave(uint64_t cs);
void srw_acquire(CPU* c, uint64_t l, int shared);
void srw_release(uint64_t l, int shared);
HLE(SleepConditionVariableCS)
{
    uint64_t cv = A1, cs = A2;
    uint32_t ms = (uint32_t)A3, rec = LD32(cs + 12);
    ST32(cs + 12, 1);
    cs_leave(cs);
    int ok = cv_sleep(c, cv, ms);
    cs_enter(c, cs);
    ST32(cs + 12, rec ? rec : 1);
    RET(ok);
}
HLE(SleepConditionVariableSRW)
{
    uint64_t cv = A1, l = A2;
    uint32_t ms = (uint32_t)A3;
    int shared = (A4 & 1) != 0;
    srw_release(l, shared);
    int ok = cv_sleep(c, cv, ms);
    srw_acquire(c, l, shared);
    RET(ok);
}
HLE(InitOnceInitialize) { STP(A1, 0); RET(0); }
HLE(InitOnceExecuteOnce)
{
    uint64_t once = A1, fn = A2, param = A3, ctx = A4;
    if (LDP(once) & 2) { if (ctx) STP(ctx, LDP(once) & ~3ull); RET(1); return; }
    uint64_t args[3] = { once, param, ctx };
    uint64_t ok = rt_invoke(c, fn, 3, args);
    if ((uint32_t)ok) STP(once, (ctx ? LDP(ctx) & ~3ull : 0) | 2);
    RET((uint32_t)ok != 0);
}
/* the ntdll form of InitOnceExecuteOnce: returns an NTSTATUS */
HLE(RtlRunOnceExecuteOnce)
{
    uint64_t once = A1, fn = A2, param = A3, ctx = A4;
    if (LDP(once) & 2) { if (ctx) STP(ctx, LDP(once) & ~3ull); RET(0); return; }
    uint64_t args[3] = { once, param, ctx };
    uint64_t ok = rt_invoke(c, fn, 3, args);
    if ((uint32_t)ok) STP(once, (ctx ? LDP(ctx) & ~3ull : 0) | 2);
    RET((uint32_t)ok ? 0 : 0xC0000001);
}
HLE(InitOnceBeginInitialize) { ST32(A3, (LDP(A1) & 2) ? 0 : 1); if (A4) STP(A4, LDP(A1) & ~3ull); RET(1); }
HLE(InitOnceComplete) { STP(A1, (A3 & ~3ull) | 2); sched_notify(); RET(1); }
static int addr_differs(GThread* t)
{
    switch (t->block_n) {
    case 1: return LD8(t->block_a) != LD8(t->block_b);
    case 2: return LD16(t->block_a) != LD16(t->block_b);
    case 4: return LD32(t->block_a) != LD32(t->block_b);
    default: return LD64(t->block_a) != LD64(t->block_b);
    }
}
HLE(WaitOnAddress)
{
    GThread* t = sched_current();
    t->block_a = A1;
    t->block_b = A2;
    t->block_n = (uint32_t)A3;
    uint32_t ms = (uint32_t)A4;
    uint64_t deadline = ms == 0xFFFFFFFF ? ~0ull : host_ticks_ns() + (uint64_t)ms * 1000000ull;
    if (sched_block(c, addr_differs, deadline)) { RET(1); return; }
    hle_set_error(1460);
    RET(0);
}
HLE(WakeByAddressSingle) { sched_notify(); RET(0); }
HLE(WakeByAddressAll) { sched_notify(); RET(0); }

HLE(Sleep) { sched_sleep(c, (uint32_t)A1); RET(0); }
HLE(SleepEx) { sched_sleep(c, (uint32_t)A1); RET(0); }
HLE(SwitchToThread) { RET(sched_yield(c)); }

HLE(CreateThread)
{
    uint64_t start = A3, param = A4, flags = ARG(5), tid_out = ARG(6);
    GThread* t = sched_create(start, param, (flags & 4) != 0);
    if (!t) { RET(0); return; }
    uint64_t h = new_obj(O_THREAD);
    ((Obj*)handle_obj(h))->thread = t;
    t->handle = h;
    if (tid_out) ST32(tid_out, (uint64_t)t->tid);
    RET(h);
}
HLE(ResumeThread) { Obj* o = handle_obj(A1); if (o && o->thread) { o->thread->suspended = 0; sched_notify(); } RET(o ? 1 : 0xFFFFFFFF); }
HLE(SuspendThread) { RET(0); }
HLE(ExitThread) { sched_exit_current(c, (uint32_t)A1); }
HLE(FreeLibraryAndExitThread) { sched_exit_current(c, (uint32_t)A2); }
HLE(TerminateThread) { Obj* o = handle_obj(A1); if (o && o->thread) o->thread->done = 1; RET(1); }
HLE(GetExitCodeThread) { Obj* o = handle_obj(A1); ST32(A2, o && o->thread && o->thread->done ? o->thread->exit_code : 259); RET(1); }
HLE(GetThreadId) { Obj* o = handle_obj(A1); RET(o && o->thread ? (uint64_t)o->thread->tid : (uint64_t)c->tid); }
HLE(GetThreadPriority) { RET(0); }
HLE(SetThreadPriority) { RET(1); }
HLE(SetThreadPriorityBoost) { RET(1); }
HLE(SetThreadAffinityMask) { RET(1); }
HLE(SetThreadIdealProcessor) { RET(0); }
HLE(SetThreadIdealProcessorEx) { RET(1); }
/* thread names only feed the debug thread dump */
HLE(SetThreadDescription)
{
    GThread* t = NULL;
    if (A1 == (uint64_t)-2) t = sched_current();
    else { Obj* o = handle_obj(A1); if (o && o->type == O_THREAD) t = o->thread; }
    if (t) wstr_to_utf8(A2, t->name, sizeof t->name);
    RET(0);
}
HLE(GetThreadDescription) { RET(0x80004001); }
HLE(SetThreadStackGuarantee) { RET(1); }
HLE(QueueUserAPC) { RET(1); }
HLE(GetThreadContext) { RET(0); }
HLE(SetThreadGroupAffinity) { RET(1); }
