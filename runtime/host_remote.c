/* Read-only game files served over HTTP (web builds). The index and block cache live in uew_files.js. */
#define _GNU_SOURCE
#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <sys/types.h>

EM_JS_DEPS(uew_remote, "$stringToNewUTF8,$UTF8ToString");
EM_JS(int, uew_rf_find, (const char* path, double* size, double* mtime), {
    const f = Module.uewFiles && Module.uewFiles.lookup(UTF8ToString(path >>> 0));
    if (!f) return -2;
    HEAPF64[size >>> 3] = f.size || 0;
    HEAPF64[mtime >>> 3] = f.mtime || 0;
    return f.dir ? -1 : f.id;
});
EM_JS(char*, uew_rf_list, (const char* path), {
    const s = Module.uewFiles && Module.uewFiles.list(UTF8ToString(path >>> 0));
    return s == null ? 0 : stringToNewUTF8(s);
});
EM_JS(int, uew_rf_read_cached, (int id, double off, int len, uint8_t* dst), {
    return Module.uewFiles.readCached(id, off, len, dst);
});
EM_ASYNC_JS(int, uew_rf_read, (int id, double off, int len, uint8_t* dst), {
    return await Module.uewFiles.read(id, off, len, dst);
});
EM_JS(void, uew_rf_start, (int id, double off, int len), { Module.uewFiles.start(id, off, len); });
EM_JS(int, uew_rf_ready, (int id, double off, int len), { return Module.uewFiles.ready(id, off, len); });
static int rf_ready(GThread* t) { return uew_rf_ready((int)t->block_a, (double)t->block_b, (int)t->block_n); }
INIT_FN(rf_names) { sched_name_check(rf_ready, "file download", NULL); }

typedef struct { int id; uint64_t size, pos; } RFile;

static ssize_t rf_read(void* p, char* buf, size_t n)
{
    RFile* f = p;
    if (f->pos >= f->size) return 0;
    if (n > f->size - f->pos) n = (size_t)(f->size - f->pos);
    int r = uew_rf_read_cached(f->id, (double)f->pos, (int)n, (uint8_t*)buf);
    if (r < 0 && n) {
        /* only this thread waits for the download; the others keep running */
        GThread* t = sched_current();
        uew_rf_start(f->id, (double)f->pos, (int)n);
        t->block_a = (uint64_t)f->id;
        t->block_b = f->pos;
        t->block_n = (uint32_t)n;
        sched_block_ext(sched_current_cpu(), rf_ready, ~0ull);
        r = uew_rf_read_cached(f->id, (double)f->pos, (int)n, (uint8_t*)buf);
    }
    if (r < 0) r = uew_rf_read(f->id, (double)f->pos, (int)n, (uint8_t*)buf);
    if (r < 0) return -1;
    f->pos += (uint64_t)r;
    return r;
}

static int rf_seek(void* p, off_t* off, int whence)
{
    RFile* f = p;
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (int64_t)f->pos : (int64_t)f->size;
    int64_t np = base + (int64_t)*off;
    if (np < 0) return -1;
    f->pos = (uint64_t)np;
    *off = (off_t)np;
    return 0;
}

static int rf_close(void* p) { free(p); return 0; }

int host_remote_stat(const char* path, HostStat* st)
{
    double size, mtime;
    int id = uew_rf_find(path, &size, &mtime);
    if (id == -2) return -1;
    st->is_dir = id == -1;
    st->size = (uint64_t)size;
    st->mtime = (uint64_t)mtime;
    return 1;
}

int host_remote_list(const char* path, void (*cb)(void* ctx, const char* name, int is_dir, uint64_t size, uint64_t mtime), void* ctx)
{
    char* s = uew_rf_list(path);
    if (!s) return -1;
    /* name \t dir \t size \t mtime \n */
    for (char* line = s; *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        char* t1 = strchr(line, '\t');
        if (t1) {
            *t1 = 0;
            int dir = t1[1] == '1';
            char* t2 = strchr(t1 + 1, '\t');
            uint64_t size = t2 ? strtoull(t2 + 1, &t2, 10) : 0;
            uint64_t mtime = t2 && *t2 ? strtoull(t2 + 1, NULL, 10) : 0;
            cb(ctx, line, dir, size, mtime);
        }
        if (!end) break;
        line = end + 1;
    }
    free(s);
    return 1;
}

FILE* host_fopen(const char* path, const char* mode)
{
    double size, mtime;
    int id = uew_rf_find(path, &size, &mtime);
    if (id < 0) return fopen(path, mode);
    if (strchr(mode, 'w') || strchr(mode, '+') || strchr(mode, 'a')) return NULL;
    RFile* f = calloc(1, sizeof *f);
    f->id = id;
    f->size = (uint64_t)size;
    cookie_io_functions_t io = { rf_read, NULL, rf_seek, rf_close };
    FILE* fp = fopencookie(f, "rb", io);
    if (!fp) { free(f); return NULL; }
    setvbuf(fp, NULL, _IONBF, 0);
    return fp;
}
#else
int host_remote_stat(const char* path, HostStat* st) { (void)path; (void)st; return -1; }
int host_remote_list(const char* path, void (*cb)(void*, const char*, int, uint64_t, uint64_t), void* ctx) { (void)path; (void)cb; (void)ctx; return -1; }
FILE* host_fopen(const char* path, const char* mode) { return fopen(path, mode); }
#endif
