#pragma once
#include "host.h"

typedef struct GThread {
    int tid;
    uint64_t start, param, handle;
    int done, suspended, started;
    uint32_t exit_code;
    uint64_t wake_at;
    CPU* cpu;
    void* ctx;
    struct GThread* next;
    uint64_t wait_h, wait_ret, wait_since;   /* what it is blocked on, for uew_threads() */
    uint32_t wait_n, wait_ms;
    char name[40];
    /* blocked: the scheduler skips the thread until check(t) holds or block_until passes */
    int (*check)(struct GThread* t);
    uint64_t block_until, block_a, block_b;
    const uint64_t* block_hs;
    uint32_t block_n;
    int block_all;
    uint64_t cpu_ns;                         /* run time since the last frame, for the hitch report */
    int frame_wait;                          /* blocked until the next browser frame */
    uint64_t stack;                          /* guest stack, freed when the thread is reaped */
    int prio;                                /* audio: runs as soon as its sleep ends, ahead of the others */
} GThread;
typedef int (*SchedCheck)(GThread* t);
/* runs other threads until check(current) holds (1) or the deadline passes (0) */
int sched_block(CPU* c, SchedCheck check, uint64_t deadline_ns);
/* the same, for something the browser finishes (download, GPU work): the browser gets frequent turns meanwhile */
int sched_block_ext(CPU* c, SchedCheck check, uint64_t deadline_ns);
extern int host_ext_waits;
/* blocks until the browser is ready for the next frame; the thread that presents gets the CPU first afterwards */
void sched_wait_frame(CPU* c);
/* a thread blocked in check on (a, b), if any */
GThread* sched_find_blocked(SchedCheck check, uint64_t a, uint64_t b);
/* names a check function for the thread dump; desc may add what it waits for */
void sched_name_check(SchedCheck check, const char* name, void (*desc)(GThread* t, char* out, int n));
void host_frame_request(void);
uint32_t host_frame_seq(void);
void host_frame_done(void);
/* the presenting thread calls this once per frame: logs long frames and who had the CPU */
void sched_frame_report(void);
void host_idle(uint64_t ns);
GThread* sched_current(void);
int sched_is_main(void);
CPU* sched_current_cpu(void);
GThread* sched_by_tid(uint32_t tid);
void sched_note_wait(CPU* c, uint32_t n, uint64_t h, uint32_t ms);

GThread* sched_create(uint64_t start, uint64_t param, int suspended);
int sched_yield(CPU* c);
int sched_yield_blocked(CPU* c);
void sched_sleep(CPU* c, uint32_t ms);
/* marks the calling thread as time-critical (audio mixing) */
void sched_set_prio(void);
void sched_exit_current(CPU* c, uint32_t code);
void sched_init(CPU* main_cpu);
int sched_thread_count(void);
unsigned sched_current_id(void);
void host_sleep_ms(uint32_t ms);
void host_browser_turn(void);
void host_browser_turned(void);
uint64_t rt_make_teb(uint64_t stack, uint64_t size, int tid);

void* host_ctx_main(void);
void* host_ctx_new(void (*entry)(void*), void* arg, size_t stack);
void host_ctx_switch(void* to);
void host_ctx_free(void* ctx);
