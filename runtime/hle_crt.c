#include "host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static uint64_t g_argc_p, g_argv_p, g_env_p, g_commode_p, g_fmode_p, g_iob, g_exc_p;
static uint64_t atexit_fns[256];
static int atexit_n;

void process_init(int argc, char** argv);
void file_init(void);

void hle_init(int argc, char** argv)
{
    file_init();
    process_init(argc, argv);
    int n = 0;
    uint64_t list[64];
    for (int i = 0; i < argc && n < 63; i++) {
        if (!strcmp(argv[i], "--image")) { i++; continue; }
        list[n++] = g_strdup(i == 0 ? "game.exe" : argv[i]);
    }
    g_argv_p = g_calloc(8ull * (n + 1));
    for (int i = 0; i < n; i++) ST64(g_argv_p + 8ull * i, list[i]);
    g_argc_p = g_calloc(8);
    ST32(g_argc_p, (uint64_t)n);
    g_env_p = g_calloc(16);
    g_commode_p = g_calloc(8);
    g_fmode_p = g_calloc(8);
    g_iob = g_calloc(3 * 0x58);
    g_exc_p = g_calloc(16);
}

GuestFn hle_by_ordinal(const char* dll, const char* name);
void hle_missing(CPU* c, const Thunk* e)
{
    GuestFn f = hle_by_ordinal(e->dll, e->name);
    if (f) { f(c); return; }
    static const char* seen[512];
    static int nseen;
    int known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i] == e->name;
    if (!known && nseen < 512) {
        seen[nseen++] = e->name;
        host_log("[hle] unimplemented %s!%s", e->dll, e->name);
    }
    RET(0);
}

static void run_atexit(CPU* c)
{
    while (atexit_n > 0) {
        uint64_t fn = atexit_fns[--atexit_n];
        rt_invoke(c, fn, 0, NULL);
    }
}

typedef struct { char* p; size_t n, cap; } Buf;

static void bput(Buf* b, const char* s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (b->n + 1 < b->cap) b->p[b->n] = s[i];
        b->n++;
    }
}

static size_t wlen(uint64_t p) { size_t n = 0; while (LD16(p + 2 * n)) n++; return n; }

size_t win_format(Buf* out, uint64_t fmt, uint64_t va, int wide_fmt)
{
    uint64_t f = fmt;
    char spec[64], tmp[512];
#define FCH(i) (wide_fmt ? (char)LD16(f + 2 * (i)) : (char)LD8(f + (i)))
#define STEP (wide_fmt ? 2 : 1)
    for (;;) {
        char ch = FCH(0);
        if (!ch) break;
        if (ch != '%') { bput(out, &ch, 1); f += STEP; continue; }
        f += STEP;
        if (FCH(0) == '%') { bput(out, "%", 1); f += STEP; continue; }
        int sp = 0;
        spec[sp++] = '%';
        while (FCH(0) && strchr("-+ #0", FCH(0)) && sp < 8) { spec[sp++] = FCH(0); f += STEP; }
        if (FCH(0) == '*') { sp += snprintf(spec + sp, 16, "%d", (int)LD64(va)); va += 8; f += STEP; }
        else while (FCH(0) >= '0' && FCH(0) <= '9') { spec[sp++] = FCH(0); f += STEP; }
        if (FCH(0) == '.') {
            spec[sp++] = '.'; f += STEP;
            if (FCH(0) == '*') { sp += snprintf(spec + sp, 16, "%d", (int)LD64(va)); va += 8; f += STEP; }
            else while (FCH(0) >= '0' && FCH(0) <= '9') { spec[sp++] = FCH(0); f += STEP; }
        }
        int len = 0;
        for (;;) {
            char l = FCH(0);
            if (l == 'h') { len = len == -1 ? -2 : -1; f += STEP; }
            else if (l == 'l') { len = len == 1 ? 2 : 1; f += STEP; }
            else if (l == 'L' || l == 'j' || l == 'z' || l == 't') { len = 2; f += STEP; }
            else if (l == 'I') {
                if (FCH(1) == '6' && FCH(2) == '4') { len = 2; f += 3 * STEP; }
                else if (FCH(1) == '3' && FCH(2) == '2') { len = 0; f += 3 * STEP; }
                else { len = 2; f += STEP; }
            } else break;
        }
        char conv = FCH(0);
        f += STEP;
        uint64_t v = LD64(va);
        va += 8;
        int n = 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t x = len == 2 ? (int64_t)v : len == 1 ? (int64_t)(int32_t)v : len == -1 ? (int16_t)v : len == -2 ? (int8_t)v : (int32_t)v;
            strcpy(spec + sp, "lld");
            n = snprintf(tmp, sizeof tmp, spec, (long long)x);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t x = len == 2 ? v : len == -1 ? (uint16_t)v : len == -2 ? (uint8_t)v : (uint32_t)v;
            spec[sp] = 'l'; spec[sp + 1] = 'l'; spec[sp + 2] = conv; spec[sp + 3] = 0;
            n = snprintf(tmp, sizeof tmp, spec, (unsigned long long)x);
            break;
        }
        case 'p':
            n = snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)v);
            break;
        case 'c': case 'C': {
            char ch2 = (char)v;
            spec[sp] = 'c'; spec[sp + 1] = 0;
            n = snprintf(tmp, sizeof tmp, spec, ch2);
            break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
            double d;
            memcpy(&d, &v, 8);
            spec[sp] = conv; spec[sp + 1] = 0;
            n = snprintf(tmp, sizeof tmp, spec, d);
            break;
        }
        case 's': case 'S': case 'Z': {
            int wide = (conv == 'S') ^ wide_fmt;
            if (len == 1) wide = 1;
            if (len == -1) wide = 0;
            char* s;
            char* heap = NULL;
            if (!v) s = "(null)";
            else if (wide) {
                size_t wl = wlen(v);
                heap = malloc(wl + 1);
                for (size_t i = 0; i < wl; i++) { uint16_t w = (uint16_t)LD16(v + 2 * i); heap[i] = w < 128 ? (char)w : '?'; }
                heap[wl] = 0;
                s = heap;
            } else s = (char*)G2H(v);
            spec[sp] = 's'; spec[sp + 1] = 0;
            int need = snprintf(NULL, 0, spec, s);
            char* big = need < (int)sizeof tmp ? tmp : malloc((size_t)need + 1);
            snprintf(big, (size_t)need + 1, spec, s);
            bput(out, big, (size_t)need);
            if (big != tmp) free(big);
            free(heap);
            continue;
        }
        case 'n':
            ST32(v, out->n);
            continue;
        default:
            bput(out, "%", 1);
            bput(out, &conv, 1);
            va -= 8;
            continue;
        }
        if (n > 0) bput(out, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1));
    }
    if (out->cap) out->p[out->n < out->cap ? out->n : out->cap - 1] = 0;
    return out->n;
}

static int stream_fd(uint64_t s) { return s == g_iob + 0x58 * 2 ? 2 : 1; }

HLE(__acrt_iob_func) { RET(g_iob + 0x58 * (A1 & 3)); }

HLE(__stdio_common_vfprintf)
{
    uint64_t stream = A2, fmt = A3, va = ARG(5);
    Buf b = { NULL, 0, 0 };
    size_t n = win_format(&b, fmt, va, 0);
    char* s = malloc(n + 1);
    Buf o = { s, 0, n + 1 };
    win_format(&o, fmt, va, 0);
    host_write(stream_fd(stream), s, n);
    free(s);
    RET(n);
}

HLE(__stdio_common_vsprintf)
{
    uint64_t opts = A1, buf = A2, cnt = A3, fmt = A4, va = ARG(6);
    Buf b = { buf ? (char*)G2H(buf) : NULL, 0, buf ? (size_t)cnt : 0 };
    size_t n = win_format(&b, fmt, va, 0);
    if (buf && n >= cnt && !(opts & 2)) { RET(-1); return; }
    RET(n);
}

HLE(__stdio_common_vsnprintf_s)
{
    uint64_t buf = A2, cnt = A3, fmt = ARG(5), va = ARG(7);
    Buf b = { (char*)G2H(buf), 0, (size_t)cnt };
    size_t n = win_format(&b, fmt, va, 0);
    RET(n >= cnt ? (uint64_t)-1 : n);
}

HLE(__stdio_common_vsprintf_s)
{
    uint64_t buf = A2, cnt = A3, fmt = A4, va = ARG(6);
    Buf b = { (char*)G2H(buf), 0, (size_t)cnt };
    RET(win_format(&b, fmt, va, 0));
}

HLE(__stdio_common_vswprintf)
{
    uint64_t buf = A2, cnt = A3, fmt = A4, va = ARG(6);
    Buf b = { NULL, 0, 0 };
    size_t n = win_format(&b, fmt, va, 1);
    char* s = malloc(n + 1);
    Buf o = { s, 0, n + 1 };
    win_format(&o, fmt, va, 1);
    if (buf) {
        size_t m = n < cnt ? n : (cnt ? cnt - 1 : 0);
        for (size_t i = 0; i < m; i++) ST16(buf + 2 * i, (uint8_t)s[i]);
        if (cnt) ST16(buf + 2 * m, 0);
    }
    free(s);
    RET(buf && n >= cnt ? (uint64_t)-1 : n);
}

HLE(fwrite)
{
    uint64_t n = A2 * A3;
    host_write(stream_fd(A4), GSTR(A1), (size_t)n);
    RET(A3);
}

HLE(fputs) { size_t n = strlen(GSTR(A1)); host_write(stream_fd(A2), GSTR(A1), n); RET(0); }
HLE(puts) { size_t n = strlen(GSTR(A1)); host_write(1, GSTR(A1), n); host_write(1, "\n", 1); RET(0); }
HLE(putchar) { char ch = (char)A1; host_write(1, &ch, 1); RET(A1 & 0xFF); }
HLE(fputc) { char ch = (char)A1; host_write(stream_fd(A2), &ch, 1); RET(A1 & 0xFF); }
HLE(fflush) { host_flush(); RET(0); }

HLE(malloc) { RET(g_alloc(A1)); }
HLE(calloc) { RET(g_calloc(A1 * A2)); }
HLE(realloc) { RET(g_realloc(A1, A2)); }
HLE(free) { g_free(A1); RET(0); }
HLE(_msize) { RET(g_size(A1)); }
HLE(_aligned_malloc)
{
    uint64_t al = A2 < 16 ? 16 : A2;
    uint64_t raw = g_alloc(A1 + al + 8);
    uint64_t p = (raw + 8 + al - 1) & ~(al - 1);
    ST64(p - 8, raw);
    RET(p);
}
HLE(_aligned_free) { if (A1) g_free(LD64(A1 - 8)); RET(0); }
HLE(_set_new_mode) { RET(0); }
HLE(_callnewh) { RET(0); }

HLE(memset) { if (A3) memset(G2H(A1), (int)(uint8_t)A2, (size_t)A3); RET(A1); }
static FILE* mt_file;
void mt_arm(const char* path)
{
    const char* t = getenv("UEW_MEMTRACE");
    if (t && !mt_file && strstr(path, t)) mt_file = fopen("memtrace.txt", "w");
}
static void mt_log(CPU* c)
{
    fprintf(mt_file, "%d %llx %llu ", c->tid, (unsigned long long)LD64(R(4)), (unsigned long long)A3);
    for (uint64_t i = 0; i < A3 && i < 16; i++) fprintf(mt_file, "%02x", G2H(A2)[i]);
    fputc(10, mt_file);
}
static void mt_watch_fn(void* d, void* s) { (void)d; (void)s; }
void (*volatile mt_watch)(void*, void*) = mt_watch_fn;
static int64_t mt_size = -2;
HLE(memcpy)
{
    if (mt_file && A3) mt_log(c);
    if (A3) memmove(G2H(A1), G2H(A2), (size_t)A3);
    if (mt_size == -2) mt_size = getenv("UEW_WATCHSIZE") ? atoll(getenv("UEW_WATCHSIZE")) : -1;
    if ((int64_t)A3 == mt_size) mt_watch(G2H(A1), G2H(A2));
    RET(A1);
}
HLE(memmove) { if (mt_file && A3) mt_log(c); if (A3) memmove(G2H(A1), G2H(A2), (size_t)A3); RET(A1); }
HLE(memcmp) { RET((int64_t)memcmp(G2H(A1), G2H(A2), (size_t)A3)); }
HLE(memchr) { uint8_t* p = memchr(G2H(A1), (int)(uint8_t)A2, (size_t)A3); RET(p ? (uint64_t)(p - MEM) : 0); }
HLE(strlen) { RET(strlen(GSTR(A1))); }
HLE(strcmp) { RET((int64_t)strcmp(GSTR(A1), GSTR(A2))); }
HLE(strncmp) { RET((int64_t)strncmp(GSTR(A1), GSTR(A2), (size_t)A3)); }
HLE(strcpy) { strcpy((char*)G2H(A1), GSTR(A2)); RET(A1); }
HLE(strncpy) { strncpy((char*)G2H(A1), GSTR(A2), (size_t)A3); RET(A1); }
HLE(strcat) { strcat((char*)G2H(A1), GSTR(A2)); RET(A1); }
HLE(strchr) { const char* p = strchr(GSTR(A1), (int)(char)A2); RET(p ? (uint64_t)((uint8_t*)p - MEM) : 0); }
HLE(strrchr) { const char* p = strrchr(GSTR(A1), (int)(char)A2); RET(p ? (uint64_t)((uint8_t*)p - MEM) : 0); }
HLE(strstr) { const char* p = strstr(GSTR(A1), GSTR(A2)); RET(p ? (uint64_t)((uint8_t*)p - MEM) : 0); }
HLE(wcslen) { uint64_t n = 0; while (LD16(A1 + 2 * n)) n++; RET(n); }
HLE(atoi) { RET((int64_t)atoi(GSTR(A1))); }
HLE(strtol) { char* e; long long v = strtoll(GSTR(A1), &e, (int)A3); if (A2) ST64(A2, A1 + (uint64_t)(e - GSTR(A1))); RET(v); }
HLE(strtoul) { char* e; unsigned long long v = strtoull(GSTR(A1), &e, (int)A3); if (A2) ST64(A2, A1 + (uint64_t)(e - GSTR(A1))); RET(v); }
HLE(atof) { RETD(atof(GSTR(A1))); }
HLE(strtod) { char* e; double v = strtod(GSTR(A1), &e); if (A2) ST64(A2, A1 + (uint64_t)(e - GSTR(A1))); RETD(v); }
HLE(tolower) { RET(tolower((int)A1)); }
HLE(toupper) { RET(toupper((int)A1)); }

#define MATH1(fn) HLE(fn) { RETD(fn(c->x[0].g[0])); }
#define MATH1F(fn) HLE(fn) { RETF(fn(c->x[0].f[0])); }
#define MATH2(fn) HLE(fn) { RETD(fn(c->x[0].g[0], c->x[1].g[0])); }
#define MATH2F(fn) HLE(fn) { RETF(fn(c->x[0].f[0], c->x[1].f[0])); }
MATH1(sin) MATH1(cos) MATH1(tan) MATH1(asin) MATH1(acos) MATH1(atan) MATH1(sqrt) MATH1(exp) MATH1(log) MATH1(log10)
MATH1(floor) MATH1(ceil) MATH1(sinh) MATH1(cosh) MATH1(tanh) MATH1(round) MATH1(trunc) MATH1(log2) MATH1(exp2) MATH1(cbrt)
MATH2(pow) MATH2(fmod) MATH2(atan2) MATH2(hypot)
MATH1F(sinf) MATH1F(cosf) MATH1F(tanf) MATH1F(asinf) MATH1F(acosf) MATH1F(atanf) MATH1F(sqrtf) MATH1F(expf) MATH1F(logf)
MATH1F(log10f) MATH1F(floorf) MATH1F(ceilf) MATH1F(roundf) MATH1F(truncf) MATH1F(log2f) MATH1F(exp2f)
MATH2F(powf) MATH2F(fmodf) MATH2F(atan2f) MATH2F(hypotf)
HLE(ldexp) { RETD(ldexp(c->x[0].g[0], (int)R(2))); }
HLE(frexp) { int e; double v = frexp(c->x[0].g[0], &e); ST32(A2, (uint64_t)e); RETD(v); }
HLE(modf) { double i; double v = modf(c->x[0].g[0], &i); memcpy(G2H(A2), &i, 8); RETD(v); }
HLE(modff) { float i; float v = modff(c->x[0].f[0], &i); memcpy(G2H(A2), &i, 4); RETF(v); }
HLE(_fdclass) { RET(fpclassify(c->x[0].f[0]) == FP_NAN ? 2 : fpclassify(c->x[0].f[0]) == FP_INFINITE ? 1 : 0); }
HLE(_dclass) { RET(isnan(c->x[0].g[0]) ? 2 : isinf(c->x[0].g[0]) ? 1 : 0); }
HLE(__setusermatherr) { RET(0); }

static void qsort_guest(CPU* c, uint64_t base, uint64_t n, uint64_t w, uint64_t cmp, uint8_t* tmp)
{
    while (n > 1) {
        if (n < 12) {
            for (uint64_t i = 1; i < n; i++)
                for (uint64_t j = i; j > 0; j--) {
                    uint64_t a[2] = { base + (j - 1) * w, base + j * w };
                    if ((int32_t)rt_invoke(c, cmp, 2, a) <= 0) break;
                    memcpy(tmp, G2H(a[0]), (size_t)w); memmove(G2H(a[0]), G2H(a[1]), (size_t)w); memcpy(G2H(a[1]), tmp, (size_t)w);
                }
            return;
        }
        uint64_t mid = base + (n / 2) * w, last = base + (n - 1) * w;
        memcpy(tmp, G2H(mid), (size_t)w); memmove(G2H(mid), G2H(last), (size_t)w); memcpy(G2H(last), tmp, (size_t)w);
        uint64_t store = base;
        for (uint64_t i = 0; i < n - 1; i++) {
            uint64_t a[2] = { base + i * w, last };
            if ((int32_t)rt_invoke(c, cmp, 2, a) < 0) {
                memcpy(tmp, G2H(a[0]), (size_t)w); memmove(G2H(a[0]), G2H(store), (size_t)w); memcpy(G2H(store), tmp, (size_t)w);
                store += w;
            }
        }
        memcpy(tmp, G2H(store), (size_t)w); memmove(G2H(store), G2H(last), (size_t)w); memcpy(G2H(last), tmp, (size_t)w);
        uint64_t left = (store - base) / w;
        qsort_guest(c, base, left, w, cmp, tmp);
        base = store + w;
        n = n - left - 1;
    }
}

HLE(qsort)
{
    uint64_t base = A1, n = A2, w = A3, cmp = A4;
    uint8_t* tmp = malloc((size_t)w);
    qsort_guest(c, base, n, w, cmp, tmp);
    free(tmp);
    RET(0);
}

HLE(_initterm)
{
    for (uint64_t p = A1, e = A2; p < e; p += 8) {
        uint64_t fn = LD64(p);
        if (fn) rt_invoke(c, fn, 0, NULL);
    }
    RET(0);
}

HLE(_initterm_e)
{
    for (uint64_t p = A1, e = A2; p < e; p += 8) {
        uint64_t fn = LD64(p);
        if (fn) {
            uint64_t r = rt_invoke(c, fn, 0, NULL);
            if ((int32_t)r) { RET(r); return; }
        }
    }
    RET(0);
}

HLE(_crt_atexit) { if (atexit_n < 256) atexit_fns[atexit_n++] = A1; RET(0); }
HLE(atexit) { if (atexit_n < 256) atexit_fns[atexit_n++] = A1; RET(0); }
HLE(_register_onexit_function) { if (atexit_n < 256) atexit_fns[atexit_n++] = A2; RET(0); }
HLE(_initialize_onexit_table) { RET(0); }
HLE(_execute_onexit_table) { run_atexit(c); RET(0); }
HLE(_register_thread_local_exe_atexit_callback) { RET(0); }
HLE(exit) { int code = (int)A1; host_log("[exit] exit(%d) from %llx", code, (unsigned long long)LD64(R(4))); run_atexit(c); rt_exit(code); }
HLE(_exit) { host_log("[exit] _exit(%d) from %llx", (int)A1, (unsigned long long)LD64(R(4))); rt_exit((int)A1); }
HLE(quick_exit) { rt_exit((int)A1); }
HLE(abort) { host_log("[crt] abort()"); rt_exit(3); }
HLE(terminate) { host_log("[crt] terminate()"); rt_exit(3); }
HLE(_cexit) { run_atexit(c); RET(0); }
HLE(_c_exit) { RET(0); }

HLE(__p___argc) { RET(g_argc_p); }
HLE(__p___argv) { RET(g_argv_p); }
HLE(__p__commode) { RET(g_commode_p); }
HLE(__p__fmode) { RET(g_fmode_p); }
HLE(_get_initial_narrow_environment) { RET(g_env_p); }
HLE(_get_initial_wide_environment) { RET(g_env_p); }
HLE(_initialize_narrow_environment) { RET(0); }
HLE(_initialize_wide_environment) { RET(0); }
HLE(_configure_narrow_argv) { RET(0); }
HLE(_configure_wide_argv) { RET(0); }
HLE(_set_app_type) { RET(0); }
HLE(_set_fmode) { RET(0); }
HLE(_configthreadlocale) { RET(0); }
HLE(_seh_filter_exe) { RET(0); }
HLE(_seh_filter_dll) { RET(0); }
HLE(__C_specific_handler) { RET(1); }
HLE(__current_exception) { RET(g_exc_p); }
HLE(__current_exception_context) { RET(g_exc_p + 8); }
HLE(_controlfp_s) { if (A1) ST32(A1, 0x9001F); RET(0); }
HLE(_errno) { static uint64_t e; if (!e) e = g_calloc(8); RET(e); }
HLE(_set_invalid_parameter_handler) { RET(0); }
HLE(_invalid_parameter_noinfo) { RET(0); }
HLE(_invalid_parameter_noinfo_noreturn) { host_log("[crt] invalid parameter"); rt_exit(3); }
HLE(__std_terminate) { host_log("[crt] std::terminate"); rt_exit(3); }
