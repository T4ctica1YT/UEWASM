#include "host.h"

#include <stdio.h>
#include <stdlib.h>

int host_truncate(FILE* f, uint64_t size);
FILE* host_fopen(const char* path, const char* mode);
#ifdef _WIN32
#define strtok_r strtok_s
#endif

enum { H_FREE, H_FILE, H_DIR, H_STD, H_FIND, H_OBJ };

typedef struct {
    int kind;
    FILE* f;
    int std;
    char path[600];
    struct FindList* find;
    void* obj;
} Handle;

typedef struct FindEntry { char name[260]; int is_dir; uint64_t size, mtime; } FindEntry;
typedef struct FindList { FindEntry* e; size_t n, cap, pos; char pat[260]; } FindList;

#define MAX_HANDLES 4096
static Handle handles[MAX_HANDLES];
static char cwd[600] = "C:\\";

#define HVAL(i) (0x100ull + 4ull * (i))
#define STD_IN 0x10ull
#define STD_OUT 0x14ull
#define STD_ERR 0x18ull

static Handle* hget(uint64_t h)
{
    if (h >= 0x100 && h < HVAL(MAX_HANDLES) && (h & 3) == 0) {
        Handle* x = &handles[(h - 0x100) / 4];
        return x->kind ? x : NULL;
    }
    return NULL;
}

uint64_t handle_new(int kind, void* obj)
{
    for (int i = 1; i < MAX_HANDLES; i++)
        if (!handles[i].kind) {
            memset(&handles[i], 0, sizeof handles[i]);
            handles[i].kind = kind;
            handles[i].obj = obj;
            return HVAL(i);
        }
    return (uint64_t)-1;
}

void* handle_obj(uint64_t h)
{
    Handle* x = hget(h);
    return x && x->kind == H_OBJ ? x->obj : NULL;
}

void hle_set_error(uint32_t e) { ST32(g_teb + 0x68, e); }
static void set_error(uint32_t e) { hle_set_error(e); }

HLE(GetLastError) { RET(LD32(g_teb + 0x68)); }
HLE(SetLastError) { set_error((uint32_t)A1); RET(0); }

void guest_abs(const char* in, char* out, size_t cap)
{
    char tmp[1200];
    if (in[0] && in[1] == ':') snprintf(tmp, sizeof tmp, "C:%s", in + 2);
    else if (in[0] == '\\' && in[1] == '\\' && in[2] == '?' && in[3] == '\\') { guest_abs(in + 4, out, cap); return; }
    else if (in[0] == '\\' || in[0] == '/') snprintf(tmp, sizeof tmp, "C:%s", in);
    else snprintf(tmp, sizeof tmp, "%s\\%s", cwd, in);
    for (char* p = tmp; *p; p++) if (*p == '/') *p = '\\';
    char* parts[256];
    int n = 0;
    char* save = NULL;
    for (char* t = strtok_r(tmp + 2, "\\", &save); t; t = strtok_r(NULL, "\\", &save)) {
        if (!strcmp(t, ".") || !*t) continue;
        if (!strcmp(t, "..")) { if (n) n--; continue; }
        if (n < 256) parts[n++] = t;
    }
    size_t o = (size_t)snprintf(out, cap, "C:");
    for (int i = 0; i < n && o < cap; i++) o += (size_t)snprintf(out + o, cap - o, "\\%s", parts[i]);
    if (n == 0) snprintf(out + o, cap - o, "\\");
}

static void join_root(const char* root, const char* guest, char* out, size_t cap)
{
    char abs[1200];
    guest_abs(guest, abs, sizeof abs);
    size_t o = (size_t)snprintf(out, cap, "%s", root);
    for (const char* p = abs + 2; *p && o + 1 < cap; p++) out[o++] = *p == '\\' ? '/' : *p;
    out[o] = 0;
}

static void make_parents(const char* path)
{
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* q = tmp + 1; *q; q++)
        if (*q == '/') { *q = 0; host_mkdir(tmp); *q = '/'; }
}

/* Reads see the save folder first, then the game install. */
void guest_to_host(const char* guest, char* out, size_t cap)
{
    if (g_save[0]) {
        HostStat st;
        join_root(g_save, guest, out, cap);
        if (host_stat(out, &st)) return;
    }
    join_root(g_root, guest, out, cap);
}

/* Anything that changes the file system only ever touches the save folder. */
void guest_to_host_w(const char* guest, char* out, size_t cap)
{
    join_root(g_save[0] ? g_save : g_root, guest, out, cap);
    if (g_save[0]) make_parents(out);
}

static void wpath(uint64_t p, char* host, size_t cap)
{
    char g[1200];
    wstr_to_utf8(p, g, sizeof g);
    guest_to_host(g, host, cap);
}

static void wpath_w(uint64_t p, char* host, size_t cap)
{
    char g[1200];
    wstr_to_utf8(p, g, sizeof g);
    guest_to_host_w(g, host, cap);
}

static int copy_file(const char* a, const char* b)
{
    FILE* in = host_fopen(a, "rb");
    if (!in) return 0;
    FILE* out = fopen(b, "wb");
    if (!out) { fclose(in); return 0; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return 1;
}

static uint64_t open_file(const char* host, uint32_t access, uint32_t disp, uint32_t flags);
static uint32_t missing_error(const char* host);

static int file_trace = -1;
void mt_arm(const char* path);
static uint64_t open_guest2(const char* guest, uint32_t access, uint32_t disp, uint32_t flags);
static uint64_t open_guest(const char* guest, uint32_t access, uint32_t disp, uint32_t flags)
{
    uint64_t h = open_guest2(guest, access, disp, flags);
    mt_arm(guest);
    if (file_trace < 0) file_trace = getenv("UEW_FILES") != NULL;
    if (file_trace) host_log("[file] open %s -> %llx", guest, (unsigned long long)h);
    return h;
}
static uint64_t open_guest2(const char* guest, uint32_t access, uint32_t disp, uint32_t flags)
{
    char rd[1200], wr[1200];
    guest_to_host(guest, rd, sizeof rd);
    int write = (access & 0x40000000) || (access & 0x10000000) || (access & 2) || (access & 4) || disp == 1 || disp == 2 || disp == 5;
    if (!write || !g_save[0]) return open_file(rd, access, disp, flags);
    guest_to_host_w(guest, wr, sizeof wr);
    HostStat st;
    if (strcmp(rd, wr) && host_stat(rd, &st) && !st.is_dir && (disp == 3 || disp == 4)) copy_file(rd, wr);
    if (host_stat(rd, &st) && st.is_dir) return open_file(rd, access, disp, flags);
    return open_file(wr, access, disp, flags);
}

static uint64_t open_file(const char* host, uint32_t access, uint32_t disp, uint32_t flags)
{
    HostStat st;
    int exists = host_stat(host, &st);
    if (exists && st.is_dir) {
        uint64_t h = handle_new(H_DIR, NULL);
        snprintf(hget(h)->path, sizeof hget(h)->path, "%s", host);
        set_error(0);
        return h;
    }
    int write = (access & 0x40000000) || (access & 0x10000000) || (access & 2) || (access & 4);
    const char* mode = NULL;
    switch (disp) {
    case 1: if (exists) { set_error(80); return (uint64_t)-1; } mode = "w+b"; break;
    case 2: mode = "w+b"; break;
    case 3: if (!exists) { set_error(missing_error(host)); return (uint64_t)-1; } mode = write ? "r+b" : "rb"; break;
    case 4: mode = exists ? (write ? "r+b" : "rb") : "w+b"; break;
    case 5: if (!exists) { set_error(2); return (uint64_t)-1; } mode = "w+b"; break;
    default: set_error(87); return (uint64_t)-1;
    }
    FILE* f = host_fopen(host, mode);
    if (!f) { set_error(exists ? 5 : 3); return (uint64_t)-1; }
    uint64_t h = handle_new(H_FILE, NULL);
    Handle* x = hget(h);
    x->f = f;
    snprintf(x->path, sizeof x->path, "%s", host);
    set_error((disp == 2 || disp == 4) && exists ? 183 : 0);
    (void)flags;
    return h;
}

HLE(CreateFileW) { char g[1200]; wstr_to_utf8(A1, g, sizeof g); RET(open_guest(g, (uint32_t)A2, (uint32_t)ARG(5), (uint32_t)ARG(6))); }
HLE(CreateFileA) { RET(open_guest(GSTR(A1), (uint32_t)A2, (uint32_t)ARG(5), (uint32_t)ARG(6))); }
HLE(CreateFile2) { char g[1200]; wstr_to_utf8(A1, g, sizeof g); RET(open_guest(g, (uint32_t)A2, (uint32_t)A4, 0)); }

static int std_of(uint64_t h) { return h == STD_IN ? 0 : h == STD_OUT ? 1 : h == STD_ERR ? 2 : -1; }

HLE(GetStdHandle)
{
    int32_t n = (int32_t)A1;
    RET(n == -10 ? STD_IN : n == -11 ? STD_OUT : n == -12 ? STD_ERR : (uint64_t)-1);
}
HLE(SetStdHandle) { RET(1); }

static int seek(Handle* x, int64_t d, uint32_t method, uint64_t* out)
{
    int w = method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END;
#ifdef _WIN32
    if (_fseeki64(x->f, d, w)) return 0;
    *out = (uint64_t)_ftelli64(x->f);
#else
    if (fseeko(x->f, (off_t)d, w)) return 0;
    *out = (uint64_t)ftello(x->f);
#endif
    return 1;
}

/* OVERLAPPED: Internal +0, InternalHigh +8, Offset +16, OffsetHigh +20, hEvent +24 */
static void ov_seek(Handle* x, uint64_t ov)
{
    uint64_t pos;
    if (ov) seek(x, (int64_t)((uint64_t)LD32(ov + 16) | (uint64_t)LD32(ov + 20) << 32), 0, &pos);
    else seek(x, 0, 1, &pos);
}
static void ov_done(uint64_t ov, size_t n)
{
    if (!ov) return;
    ST64(ov, 0);
    ST64(ov + 8, n);
}

HLE(WriteFile)
{
    uint64_t h = A1, buf = A2, n = A3 & 0xFFFFFFFF, written = A4, ov = ARG(5);
    int s = std_of(h);
    if (s > 0) { host_write(s, GSTR(buf), (size_t)n); if (written) ST32(written, n); ov_done(ov, n); RET(1); return; }
    Handle* x = hget(h);
    if (!x || x->kind != H_FILE) { set_error(6); RET(0); return; }
    ov_seek(x, ov);
    size_t w = fwrite(G2H(buf), 1, (size_t)n, x->f);
    if (written) ST32(written, w);
    ov_done(ov, w);
    RET(w == n);
}

HLE(ReadFile)
{
    uint64_t h = A1, buf = A2, n = A3 & 0xFFFFFFFF, got = A4, ov = ARG(5);
    if (std_of(h) == 0) { if (got) ST32(got, 0); RET(1); return; }
    Handle* x = hget(h);
    if (!x || x->kind != H_FILE) { set_error(6); RET(0); return; }
    ov_seek(x, ov);
    size_t r = fread(G2H(buf), 1, (size_t)n, x->f);
    if (got) ST32(got, r);
    ov_done(ov, r);
    if (ov && !r && n) { set_error(38); RET(0); return; }
    RET(1);
}

HLE(SetFilePointerEx)
{
    Handle* x = hget(A1);
    uint64_t pos;
    if (!x || x->kind != H_FILE || !seek(x, (int64_t)A2, (uint32_t)A4, &pos)) { set_error(6); RET(0); return; }
    if (A3) ST64(A3, pos);
    RET(1);
}

HLE(SetFilePointer)
{
    Handle* x = hget(A1);
    int64_t d = (int32_t)A2;
    if (A3) d = (int64_t)(((uint64_t)LD32(A3) << 32) | (uint32_t)A2);
    uint64_t pos;
    if (!x || x->kind != H_FILE || !seek(x, d, (uint32_t)A4, &pos)) { set_error(6); RET(0xFFFFFFFF); return; }
    if (A3) ST32(A3, pos >> 32);
    RET((uint32_t)pos);
}

static uint64_t fsize(Handle* x)
{
    HostStat st = { 0 };
    fflush(x->f);
    host_stat(x->path, &st);
    return st.size;
}

HLE(GetFileSizeEx) { Handle* x = hget(A1); if (!x || x->kind != H_FILE) { RET(0); return; } ST64(A2, fsize(x)); RET(1); }
HLE(GetFileSize) { Handle* x = hget(A1); if (!x || x->kind != H_FILE) { RET(0xFFFFFFFF); return; } uint64_t s = fsize(x); if (A2) ST32(A2, s >> 32); RET((uint32_t)s); }
HLE(GetFileType) { int s = std_of(A1); RET(s >= 0 ? 3 : hget(A1) ? 1 : 0); }
HLE(FlushFileBuffers) { Handle* x = hget(A1); if (x && x->f) fflush(x->f); RET(1); }
HLE(SetEndOfFile)
{
    Handle* x = hget(A1);
    uint64_t pos;
    if (!x || x->kind != H_FILE || !seek(x, 0, 1, &pos)) { RET(0); return; }
    RET(host_truncate(x->f, pos));
}

static void fill_times(uint64_t p, uint64_t t) { ST64(p, t); ST64(p + 8, t); ST64(p + 16, t); }

HLE(GetFileInformationByHandle)
{
    Handle* x = hget(A1);
    if (!x) { set_error(6); RET(0); return; }
    HostStat st = { 0 };
    if (x->f) fflush(x->f);
    host_stat(x->path, &st);
    memset(G2H(A2), 0, 52);
    ST32(A2, st.is_dir ? 0x10 : 0x80);
    fill_times(A2 + 4, st.mtime);
    ST32(A2 + 28, 0x1234);
    ST32(A2 + 32, st.size >> 32);
    ST32(A2 + 36, (uint32_t)st.size);
    ST32(A2 + 40, 1);
    RET(1);
}

HLE(GetFileInformationByHandleEx)
{
    Handle* x = hget(A1);
    if (!x) { set_error(6); RET(0); return; }
    HostStat st = { 0 };
    if (x->f) fflush(x->f);
    host_stat(x->path, &st);
    uint32_t cls = (uint32_t)A2;
    if (cls == 0) {
        memset(G2H(A3), 0, 40);
        ST64(A3, st.mtime); ST64(A3 + 8, st.mtime); ST64(A3 + 16, st.mtime); ST64(A3 + 24, st.mtime);
        ST32(A3 + 32, st.is_dir ? 0x10 : 0x80);
        RET(1);
    } else if (cls == 1) {
        memset(G2H(A3), 0, 24);
        ST64(A3, st.size); ST64(A3 + 8, st.size); ST32(A3 + 16, 1); ST8(A3 + 21, st.is_dir);
        RET(1);
    }
    set_error(50);
    RET(0);
}

HLE(CloseHandle)
{
    Handle* x = hget(A1);
    if (!x) { RET(std_of(A1) >= 0); return; }
    if (x->f) fclose(x->f);
    if (x->find) { free(x->find->e); free(x->find); }
    x->kind = H_FREE;
    RET(1);
}

HLE(DuplicateHandle) { if (A4) ST64(A4, A2); RET(1); }

static uint32_t missing_error(const char* host)
{
    char d[1200];
    snprintf(d, sizeof d, "%s", host);
    char* s = strrchr(d, '/');
    HostStat st;
    if (s) { *s = 0; if (!host_stat(d, &st)) return 3; }
    return 2;
}

static int attr_of(const char* host, uint64_t out_data)
{
    HostStat st;
    if (!host_stat(host, &st)) { set_error(missing_error(host)); return -1; }
    if (out_data) {
        ST32(out_data, st.is_dir ? 0x10 : 0x80);
        fill_times(out_data + 4, st.mtime);
        ST32(out_data + 28, st.size >> 32);
        ST32(out_data + 32, (uint32_t)st.size);
    }
    return st.is_dir ? 0x10 : 0x80;
}

HLE(GetFileAttributesW)
{
    char host[1200];
    wpath(A1, host, sizeof host);
    int a = attr_of(host, 0);
    if (file_trace < 0) file_trace = getenv("UEW_FILES") != NULL;
    if (file_trace) host_log("[file] attr %s -> %d", host, a);
    RET(a < 0 ? 0xFFFFFFFF : (uint64_t)a);
}
HLE(GetFileAttributesA) { char host[1200]; guest_to_host(GSTR(A1), host, sizeof host); int a = attr_of(host, 0); RET(a < 0 ? 0xFFFFFFFF : (uint64_t)a); }
HLE(GetFileAttributesExW)
{
    char host[1200];
    wpath(A1, host, sizeof host);
    int a = attr_of(host, A3);
    if (file_trace < 0) file_trace = getenv("UEW_FILES") != NULL;
    if (file_trace) host_log("[file] attrex %s -> %d", host, a);
    RET(a >= 0);
}
HLE(GetFileAttributesExA) { char host[1200]; guest_to_host(GSTR(A1), host, sizeof host); RET(attr_of(host, A3) >= 0); }
HLE(SetFileAttributesW) { RET(1); }
HLE(SetFileTime) { RET(1); }
HLE(LockFileEx) { RET(1); }
HLE(UnlockFileEx) { RET(1); }
HLE(LockFile) { RET(1); }
HLE(UnlockFile) { RET(1); }

HLE(CreateDirectoryW) { char host[1200], w[1200]; wpath(A1, host, sizeof host); HostStat st; if (host_stat(host, &st)) { set_error(183); RET(0); return; } wpath_w(A1, w, sizeof w); RET(host_mkdir(w)); }
HLE(CreateDirectoryA) { char host[1200]; guest_to_host_w(GSTR(A1), host, sizeof host); RET(host_mkdir(host)); }
HLE(RemoveDirectoryW) { char host[1200]; wpath_w(A1, host, sizeof host); host_rmdir(host); RET(1); }
HLE(DeleteFileW) { char host[1200], w[1200]; wpath(A1, host, sizeof host); wpath_w(A1, w, sizeof w); HostStat st; if (!host_stat(host, &st)) { set_error(2); RET(0); return; } host_remove(w); RET(1); }
HLE(DeleteFileA) { char host[1200]; guest_to_host_w(GSTR(A1), host, sizeof host); host_remove(host); RET(1); }
HLE(MoveFileExW) { char a[1200], aw[1200], b[1200]; wpath(A1, a, sizeof a); wpath_w(A1, aw, sizeof aw); wpath_w(A2, b, sizeof b); if (strcmp(a, aw)) { RET(copy_file(a, b)); return; } RET(host_rename(a, b)); }
HLE(MoveFileW) { char a[1200], aw[1200], b[1200]; wpath(A1, a, sizeof a); wpath_w(A1, aw, sizeof aw); wpath_w(A2, b, sizeof b); if (strcmp(a, aw)) { RET(copy_file(a, b)); return; } RET(host_rename(a, b)); }
HLE(CopyFileW)
{
    char a[1200], b[1200];
    wpath(A1, a, sizeof a);
    wpath_w(A2, b, sizeof b);
    FILE* in = host_fopen(a, "rb");
    if (!in) { set_error(2); RET(0); return; }
    FILE* out = fopen(b, "wb");
    if (!out) { fclose(in); RET(0); return; }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    RET(1);
}

static int wild(const char* p, const char* s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (wild(p + 1, s)) return 1;
            return 0;
        }
        if (!*s) return 0;
        char a = *p >= 'A' && *p <= 'Z' ? *p + 32 : *p, b = *s >= 'A' && *s <= 'Z' ? *s + 32 : *s;
        if (*p != '?' && a != b) return 0;
    }
    return !*s || !strcmp(s, ".");
}

static void find_cb(void* ctx, const char* name, int is_dir, uint64_t size, uint64_t mtime)
{
    FindList* l = ctx;
    size_t pl = strlen(l->pat);
    int ok = wild(l->pat, name);
    if (!ok && pl >= 2 && !strcmp(l->pat + pl - 2, ".*")) {
        char p[260];
        snprintf(p, sizeof p, "%.*s", (int)(pl - 2), l->pat);
        ok = wild(p, name);
    }
    if (!ok) return;
    for (size_t i = 0; i < l->n; i++)
        if (!strcmp(l->e[i].name, name)) return;
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 32; l->e = realloc(l->e, l->cap * sizeof *l->e); }
    FindEntry* e = &l->e[l->n++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->is_dir = is_dir;
    e->size = size;
    e->mtime = mtime;
}

static void fill_find(uint64_t out, FindEntry* e, int wide)
{
    memset(G2H(out), 0, wide ? 592 : 320);
    ST32(out, e->is_dir ? 0x10 : 0x80);
    fill_times(out + 4, e->mtime);
    ST32(out + 28, e->size >> 32);
    ST32(out + 32, (uint32_t)e->size);
    if (wide) utf8_to_wstr(e->name, out + 44, 260);
    else snprintf((char*)G2H(out + 44), 260, "%s", e->name);
}

static uint64_t find_first(const char* guest, uint64_t out, int wide)
{
    char abs[1200], host[1200];
    guest_abs(guest, abs, sizeof abs);
    char* slash = strrchr(abs, '\\');
    FindList* l = calloc(1, sizeof *l);
    snprintf(l->pat, sizeof l->pat, "%s", slash ? slash + 1 : abs);
    if (slash) *slash = 0;
    int listed = 0;
    if (g_save[0]) { join_root(g_save, abs, host, sizeof host); listed |= host_list_dir(host, find_cb, l); }
    join_root(g_root, abs, host, sizeof host);
    listed |= host_list_dir(host, find_cb, l);
    if (file_trace < 0) file_trace = getenv("UEW_FILES") != NULL;
    if (!listed || !l->n) {
        if (file_trace > 0) host_log("[file] find %s -> none", guest);
        free(l->e);
        free(l);
        set_error(2);
        return (uint64_t)-1;
    }
    if (file_trace > 0) host_log("[file] find %s -> %d entries", guest, l->n);
    uint64_t h = handle_new(H_FIND, NULL);
    hget(h)->find = l;
    fill_find(out, &l->e[l->pos++], wide);
    return h;
}

HLE(FindFirstFileExW) { char g[1200]; wstr_to_utf8(A1, g, sizeof g); RET(find_first(g, A3, 1)); }
HLE(FindFirstFileW) { char g[1200]; wstr_to_utf8(A1, g, sizeof g); RET(find_first(g, A2, 1)); }
HLE(FindFirstFileA) { RET(find_first(GSTR(A1), A2, 0)); }
static void find_next(CPU* c, int wide)
{
    Handle* x = hget(A1);
    if (!x || !x->find || x->find->pos >= x->find->n) { set_error(18); RET(0); return; }
    fill_find(A2, &x->find->e[x->find->pos++], wide);
    RET(1);
}
HLE(FindNextFileW) { find_next(c, 1); }
HLE(FindNextFileA) { find_next(c, 0); }
HLE(FindClose) { hle_CloseHandle(c); }

HLE(GetFullPathNameW)
{
    char g[1200], abs[1200];
    wstr_to_utf8(A1, g, sizeof g);
    guest_abs(g, abs, sizeof abs);
    size_t need = strlen(abs) + 1;
    if (A2 < need) { RET(need); return; }
    size_t n = utf8_to_wstr(abs, A3, (size_t)A2);
    if (A4) { const char* s = strrchr(abs, '\\'); ST64(A4, s && s[1] ? A3 + 2 * (uint64_t)(s + 1 - abs) : 0); }
    RET(n);
}
HLE(GetFullPathNameA)
{
    char abs[1200];
    guest_abs(GSTR(A1), abs, sizeof abs);
    size_t n = strlen(abs);
    if (A2 <= n) { RET(n + 1); return; }
    memcpy(G2H(A3), abs, n + 1);
    if (A4) { const char* s = strrchr(abs, '\\'); ST64(A4, s ? A3 + (uint64_t)(s + 1 - abs) : 0); }
    RET(n);
}
HLE(GetCurrentDirectoryW) { size_t need = strlen(cwd) + 1; if (A1 < need) { RET(need); return; } RET(utf8_to_wstr(cwd, A2, (size_t)A1)); }
HLE(GetCurrentDirectoryA) { size_t n = strlen(cwd); if (A1 <= n) { RET(n + 1); return; } memcpy(G2H(A2), cwd, n + 1); RET(n); }
HLE(SetCurrentDirectoryW) { char g[1200]; wstr_to_utf8(A1, g, sizeof g); guest_abs(g, cwd, sizeof cwd); RET(1); }
HLE(SetCurrentDirectoryA) { guest_abs(GSTR(A1), cwd, sizeof cwd); RET(1); }
HLE(GetTempPathW) { RET(utf8_to_wstr("C:\\Temp\\", A2, (size_t)A1)); }
HLE(GetTempPathA) { snprintf((char*)G2H(A2), (size_t)A1, "C:\\Temp\\"); RET(8); }
HLE(GetDriveTypeW) { RET(3); }
HLE(GetDriveTypeA) { RET(3); }
HLE(GetLogicalDrives) { RET(4); }
HLE(GetDiskFreeSpaceW) { if (A2) ST32(A2, 8); if (A3) ST32(A3, 512); if (A4) ST32(A4, 0x1000000); if (ARG(5)) ST32(ARG(5), 0x2000000); RET(1); }
HLE(GetDiskFreeSpaceExW) { if (A2) ST64(A2, 64ull << 30); if (A3) ST64(A3, 128ull << 30); if (A4) ST64(A4, 64ull << 30); RET(1); }
HLE(GetVolumeInformationW) { if (A2) ST16(A2, 0); if (ARG(5)) ST32(ARG(5), 0x1234); RET(1); }

void file_init(void)
{
    char d[600];
    snprintf(d, sizeof d, "C:\\%s", g_exe_rel);
    for (char* p = d; *p; p++) if (*p == '/') *p = '\\';
    char* s = strrchr(d, '\\');
    if (s) *s = 0;
    guest_abs(d, cwd, sizeof cwd);
}

HLE(GetConsoleMode) { set_error(6); RET(0); }
HLE(SetConsoleMode) { RET(0); }
HLE(GetConsoleCP) { RET(65001); }
HLE(GetConsoleOutputCP) { RET(65001); }
HLE(SetConsoleCtrlHandler) { RET(1); }
HLE(WriteConsoleW) { char s[4096]; uint64_t n = A3 < 2000 ? A3 : 2000; uint64_t tmp = g_alloc(2 * n + 2); memcpy(G2H(tmp), G2H(A2), 2 * n); ST16(tmp + 2 * n, 0); wstr_to_utf8(tmp, s, sizeof s); g_free(tmp); host_write(1, s, strlen(s)); if (A4) ST32(A4, n); RET(1); }
HLE(WriteConsoleA) { host_write(1, GSTR(A2), (size_t)A3); if (A4) ST32(A4, A3); RET(1); }
HLE(ReadConsoleW) { if (A4) ST32(A4, 0); RET(1); }
HLE(ReadConsoleInputW) { RET(0); }
HLE(PeekConsoleInputA) { if (A4) ST32(A4, 0); RET(1); }
HLE(GetNumberOfConsoleInputEvents) { ST32(A2, 0); RET(1); }
HLE(AllocConsole) { RET(1); }
HLE(FreeConsole) { RET(1); }
HLE(GetConsoleWindow) { RET(0); }
HLE(GetConsoleScreenBufferInfo) { RET(0); }
HLE(SetConsoleTextAttribute) { RET(1); }
HLE(SetConsoleTitleW) { RET(1); }
HLE(CreatePipe) { set_error(50); RET(0); }
HLE(PeekNamedPipe) { RET(0); }

HLE(GetOverlappedResult)
{
    if (A2) { ST64(A2, 0); ST32(A3, (uint32_t)LD64(A2 + 8)); }
    RET(1);
}
