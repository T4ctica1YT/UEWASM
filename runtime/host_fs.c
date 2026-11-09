#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define stat64_t struct _stat64
#define do_stat _stat64
#define do_mkdir(p) _mkdir(p)
#define do_rmdir(p) _rmdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define stat64_t struct stat
#define do_stat stat
#define do_mkdir(p) mkdir(p, 0777)
#define do_rmdir(p) rmdir(p)
#endif

char g_root[1024] = ".";
char g_exe_rel[512] = "game.exe";
char g_save[1024];

int host_remote_stat(const char* path, HostStat* st);
int host_remote_list(const char* path, void (*cb)(void*, const char*, int, uint64_t, uint64_t), void* ctx);

int host_stat(const char* path, HostStat* st)
{
    int r = host_remote_stat(path, st);
    if (r >= 0) return r;
    stat64_t s;
    if (do_stat(path, &s)) return 0;
    st->is_dir = (s.st_mode & S_IFDIR) != 0;
    st->size = (uint64_t)s.st_size;
    st->mtime = ((uint64_t)s.st_mtime + 11644473600ull) * 10000000ull;
    return 1;
}

int host_mkdir(const char* path) { return do_mkdir(path) == 0; }
int host_rmdir(const char* path) { return do_rmdir(path) == 0; }
int host_remove(const char* path) { return remove(path) == 0; }
int host_rename(const char* a, const char* b) { remove(b); return rename(a, b) == 0; }

int host_truncate(FILE* f, uint64_t size)
{
    fflush(f);
#ifdef _WIN32
    return _chsize_s(_fileno(f), (long long)size) == 0;
#else
    return ftruncate(fileno(f), (off_t)size) == 0;
#endif
}

int host_list_dir(const char* path, void (*cb)(void* ctx, const char* name, int is_dir, uint64_t size, uint64_t mtime), void* ctx)
{
    int r = host_remote_list(path, cb, ctx);
    if (r >= 0) return r;
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s\\*", path);
    struct _finddata64i32_t fd;
    intptr_t h = _findfirst64i32(pat, &fd);
    if (h == -1) return 0;
    do {
        cb(ctx, fd.name, (fd.attrib & _A_SUBDIR) != 0, (uint64_t)fd.size, ((uint64_t)fd.time_write + 11644473600ull) * 10000000ull);
    } while (_findnext64i32(h, &fd) == 0);
    _findclose(h);
    return 1;
#else
    DIR* d = opendir(path);
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        char full[1400];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        HostStat st = { 0 };
        host_stat(full, &st);
        cb(ctx, e->d_name, st.is_dir, st.size, st.mtime);
    }
    closedir(d);
    return 1;
#endif
}
