#pragma once
#include <stddef.h>
#include "rt.h"

#define THUNK_BASE 0x10000ull
#define RET_SENTINEL 0xDEAD0000ull

typedef struct Thunk {
    GuestFn fn;
    const char* name;
    const char* dll;
    GuestFn call;   /* fn plus the return-address pop, so call sites can cache it */
} Thunk;

/* 32-bit guests: pointers are 4 bytes, system calls take their arguments on the stack (stdcall pops them) */
extern const int RT_G32;
extern const unsigned short rt_thunk_pops[];
#define GP (RT_G32 ? 4u : 8u)
#define LDP(a) (RT_G32 ? LD32(a) : LD64(a))
#define STP(a, v) (RT_G32 ? ST32((a), (v)) : ST64((a), (v)))
#define TEB_ERR (RT_G32 ? 0x34 : 0x68)
void rt_hle32(CPU* c, const Thunk* e, unsigned pops);

typedef struct ModuleInfo {
    const char* name;
    uint64_t base, size, entry_rva;
    int is_dll;
    uint64_t tls_start, tls_end, tls_index, tls_zero;
    size_t cb_first, cb_count;
} ModuleInfo;

extern const FnEntry rt_fn_table[];
extern const size_t rt_fn_count;
extern const Thunk rt_thunks[];
/* counts imported-function calls; at zero the scheduler checks the running thread's time slice */
#ifdef UEW_THREADS
extern _Thread_local int rt_hle_budget;
#else
extern int rt_hle_budget;
#endif
extern int rt_trace_on;
void rt_trace_call(CPU* c, const char* name);
void rt_hle_tick(CPU* c);
extern const size_t rt_thunk_count;
extern const ModuleInfo rt_modules[];
extern const size_t rt_module_count;
extern const uint64_t rt_tls_callbacks[];
extern const uint64_t IMAGE_LO, IMAGE_HI, GUEST_MEM_MB;
extern uint64_t g_exe_base;

const ModuleInfo* rt_find_module(const char* name);
const ModuleInfo* rt_module_at(uint64_t addr);
uint64_t rt_get_export(uint64_t base, const char* name);
uint64_t rt_thunk_by_name(const char* name);
extern uint64_t g_teb, g_peb;
extern CPU main_cpu;

uint64_t g_alloc(uint64_t n);
uint64_t g_calloc(uint64_t n);
uint64_t g_realloc(uint64_t p, uint64_t n);
uint64_t g_size(uint64_t p);
void g_free(uint64_t p);
uint64_t g_strdup(const char* s);

uint64_t rt_invoke(CPU* c, uint64_t fn, int n, const uint64_t* args);
void rt_exit(int code);
void rt_detach_all(CPU* c);
int rt_main(int argc, char** argv, const char* image_path);

void hle_init(int argc, char** argv);
void hle_missing(CPU* c, const Thunk* e);

void wstr_to_utf8(uint64_t p, char* out, size_t cap);
size_t utf8_to_wstr(const char* s, uint64_t out, size_t cap);

typedef struct { int is_dir; uint64_t size, mtime; } HostStat;
extern char g_root[1024];
extern char g_exe_rel[512];
extern char g_save[1024];
void guest_to_host_w(const char* guest, char* out, size_t cap);
int host_stat(const char* path, HostStat* st);
int host_mkdir(const char* path);
int host_rmdir(const char* path);
int host_remove(const char* path);
int host_rename(const char* a, const char* b);
struct _iobuf;
int host_list_dir(const char* path, void (*cb)(void* ctx, const char* name, int is_dir, uint64_t size, uint64_t mtime), void* ctx);

enum { HOST_EV_KEY_DOWN = 1, HOST_EV_KEY_UP, HOST_EV_CHAR, HOST_EV_MOUSE_MOVE, HOST_EV_MOUSE_BUTTON, HOST_EV_WHEEL, HOST_EV_RESIZE, HOST_EV_FOCUS, HOST_EV_CLOSE };
void user32_event(int type, int a, int b, int d1, int d2);
void host_poll_events(void);
void host_frame_yield(void);
void host_browser_turned(void);
void host_canvas_size(int* w, int* h);
int host_gamepad(int index, uint16_t* buttons, uint8_t* triggers, int16_t* sticks);
void host_rumble(int index, int lo, int hi);
/* audio out: interleaved stereo float frames at the rate given to open */
int host_audio_open(int rate);
int host_audio_queued(void);
void host_audio_write(const float* frames, int count);
void host_show_cursor(int show);
void host_lock_pointer(int lock);

void host_log(const char* fmt, ...);
void host_write(int fd, const char* s, size_t n);
void host_flush(void);
uint64_t host_ticks_ns(void);
uint64_t host_time_100ns(void);

#define HLE(name) void hle_##name(CPU* c)
#define STUB(name, v) HLE(name) { RET(v); }

uint64_t rt_dyn_thunk(GuestFn fn, const char* name);
/* nargs: stack arguments a 32-bit guest pushes (stdcall: the call pops them) */
uint64_t rt_dyn_thunk_n(GuestFn fn, const char* name, int nargs);
int host_query_flag(const char* name);
int host_query_int(const char* name);
uint64_t g_alloc_zero_aligned(uint64_t n, uint64_t align);
uint64_t g_pages(uint64_t size);
uint64_t g_pages_at(uint64_t a, uint64_t size);
uint64_t g_pages_query(uint64_t a, uint64_t* len);
void g_pages_free(uint64_t a);
uint64_t g_mem_used(void);
#define A1 R(1)
#define A2 R(2)
#define A3 R(8)
#define A4 R(9)
#define ARG(n) LD64(R(4) + 8ull * (n))
#define RET(v) (R(0) = (uint64_t)(v))
#define RETD(v) (c->x[0].g[0] = (v))
#define RETF(v) (c->x[0].f[0] = (v))
#define GSTR(p) ((const char*)G2H(p))
