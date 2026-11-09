/* libvorbisfile served natively. UE asks for these through GetProcAddress (delay-load); decoding Vorbis in lifted
   x86 code took ~10% of a UE4 game's boot and kept streamed sounds behind in game. The game's own read/seek/tell/
   close callbacks still feed the data. Web builds only: native builds keep the game's DLL. */
#include "host.h"
#include <string.h>
#include <stdlib.h>

#ifdef __EMSCRIPTEN__
#include <vorbis/vorbisfile.h>

typedef struct {
    uint64_t vf;                    /* the game's OggVorbis_File, used as the key */
    uint64_t gds, rd, sk, cl, tl;   /* guest datasource and callbacks */
    uint64_t scratch, info;         /* guest buffers: read target, vorbis_info copy */
    CPU* c;
    OggVorbis_File f;
} Stream;

enum { SCRATCH = 64 << 10, MAXS = 512 };
static Stream* streams[MAXS];
static int nstreams;

static Stream* find(uint64_t vf)
{
    for (int i = 0; i < nstreams; i++) if (streams[i]->vf == vf) return streams[i];
    return NULL;
}
static void drop(Stream* s)
{
    for (int i = 0; i < nstreams; i++) if (streams[i] == s) { streams[i] = streams[--nstreams]; break; }
    g_free(s->scratch);
    g_free(s->info);
    free(s);
}

static size_t s_read(void* ptr, size_t size, size_t n, void* ds)
{
    Stream* s = ds;
    size_t want = size * n, got = 0;
    while (got < want) {
        size_t chunk = want - got < SCRATCH ? want - got : SCRATCH;
        uint64_t a[4] = { s->scratch, 1, chunk, s->gds };
        uint64_t r = rt_invoke(s->c, s->rd, 4, a);
        if (!r) break;
        if (r > chunk) r = chunk;
        memcpy((char*)ptr + got, G2H(s->scratch), (size_t)r);
        got += (size_t)r;
        if (r < chunk) break;
    }
    return size ? got / size : 0;
}
static int s_seek(void* ds, ogg_int64_t off, int whence)
{
    Stream* s = ds;
    uint64_t a[3] = { s->gds, (uint64_t)off, (uint64_t)(int64_t)whence };
    return (int32_t)rt_invoke(s->c, s->sk, 3, a);
}
static int s_close(void* ds)
{
    Stream* s = ds;
    uint64_t a[1] = { s->gds };
    return s->cl ? (int32_t)rt_invoke(s->c, s->cl, 1, a) : 0;
}
static long s_tell(void* ds)
{
    Stream* s = ds;
    uint64_t a[1] = { s->gds };
    return (int32_t)rt_invoke(s->c, s->tl, 1, a);
}

#define S_OR(fail) Stream* s = find(A1); if (!s) { fail; return; } s->c = c

/* int ov_open_callbacks(void* ds, OggVorbis_File* vf, const char* initial, long ibytes, ov_callbacks cb) */
static void v_open(CPU* c)
{
    static int said;
    if (!said++) host_log("[vorbis] decoding Ogg Vorbis natively");
    Stream* old = find(A2);
    if (old) drop(old);
    if (nstreams >= MAXS) { RET(-129); return; }
    uint64_t cb = ARG(5);
    Stream* s = calloc(1, sizeof *s);
    s->vf = A2;
    s->gds = A1;
    s->rd = LD64(cb);
    s->sk = LD64(cb + 8);
    s->cl = LD64(cb + 16);
    s->tl = LD64(cb + 24);
    s->scratch = g_alloc(SCRATCH);
    s->info = g_calloc(64);
    s->c = c;
    ov_callbacks nc = { s_read, s->sk ? s_seek : NULL, s_close, s->tl ? s_tell : NULL };
    streams[nstreams++] = s;
    int r = ov_open_callbacks(s, &s->f, A3 ? (const char*)G2H(A3) : NULL, (long)(int32_t)A4, nc);
    if (r < 0) drop(s);
    RET((int64_t)r);
}
static void v_clear(CPU* c)
{
    S_OR(RET(0));
    ov_clear(&s->f);
    drop(s);
    RET(0);
}
/* vorbis_info laid out as the game (Win64, 32-bit long) reads it */
static void v_info(CPU* c)
{
    S_OR(RET(0));
    vorbis_info* vi = ov_info(&s->f, (int)A2);
    if (!vi) { RET(0); return; }
    ST32(s->info + 0, (uint32_t)vi->version);
    ST32(s->info + 4, (uint32_t)vi->channels);
    ST32(s->info + 8, (uint32_t)vi->rate);
    ST32(s->info + 12, (uint32_t)vi->bitrate_upper);
    ST32(s->info + 16, (uint32_t)vi->bitrate_nominal);
    ST32(s->info + 20, (uint32_t)vi->bitrate_lower);
    ST32(s->info + 24, (uint32_t)vi->bitrate_window);
    RET(s->info);
}
/* long ov_read(vf, char* buffer, int length, int bigendianp, int word, int sgned, int* bitstream) */
static void v_read(CPU* c)
{
    S_OR(RET(-131));
    uint64_t bs = ARG(7);
    int cur = 0;
    long r = ov_read(&s->f, (char*)G2H(A2), (int)A3, (int)A4, (int)ARG(5), (int)ARG(6), &cur);
    if (bs) ST32(bs, (uint32_t)cur);
    RET((int64_t)r);
}
static void v_pcm_seek_page(CPU* c) { S_OR(RET(-131)); RET((int64_t)ov_pcm_seek_page(&s->f, (ogg_int64_t)A2)); }
static void v_time_seek(CPU* c) { S_OR(RET(-131)); RET((int64_t)ov_time_seek(&s->f, c->x[1].g[0])); }
static void v_time_total(CPU* c) { S_OR(RETD(-131.0)); RETD(ov_time_total(&s->f, (int)A2)); }
static void v_pcm_total(CPU* c) { S_OR(RET(-131)); RET((int64_t)ov_pcm_total(&s->f, (int)A2)); }
static void v_halfrate(CPU* c) { S_OR(RET(-131)); RET((int64_t)ov_halfrate(&s->f, (int)A2)); }

static const struct { const char* name; GuestFn fn; } fns[] = {
    { "ov_open_callbacks", v_open }, { "ov_clear", v_clear }, { "ov_info", v_info }, { "ov_read", v_read },
    { "ov_pcm_seek_page", v_pcm_seek_page }, { "ov_time_seek", v_time_seek }, { "ov_time_total", v_time_total },
    { "ov_pcm_total", v_pcm_total }, { "ov_halfrate", v_halfrate },
};
#endif

/* GetProcAddress asks here first: a native stand-in for an export of a recompiled DLL, or 0 */
uint64_t hle_export_override(const char* module, const char* name)
{
#ifdef __EMSCRIPTEN__
    if (!module || !name || host_query_flag("guestvorbis")) return 0;
    const char* b = module;
    for (const char* p = module; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    if (strncasecmp(b, "libvorbisfile", 13)) return 0;
    static uint64_t thunk[sizeof fns / sizeof *fns];
    for (size_t i = 0; i < sizeof fns / sizeof *fns; i++)
        if (!strcmp(fns[i].name, name)) return thunk[i] ? thunk[i] : (thunk[i] = rt_dyn_thunk(fns[i].fn, fns[i].name));
#else
    (void)module; (void)name;
#endif
    return 0;
}
