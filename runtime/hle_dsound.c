/* DirectSound 8 (UE3-era games and their audio middleware, e.g. Wwise on Dishonored): secondary buffers in guest memory,
   mixed at their own sample rate and volume by one guest thread into the page's audio output (host_audio_*), with
   play/write cursors, looping, Lock/Unlock wrap-around and position notifications. The primary buffer only carries the
   output format. 3D, effects and capture are not implemented. */
#include "com.h"
#include "sched.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void sync_set_event(uint64_t h);
enum { RATE = 48000, PASS = 480, MAXBUF = 256 };

typedef struct {
    uint64_t obj, mem;
    uint32_t size, flags, ch, bits, flt, rate, align, freq;
    double pos;                 /* play position in frames */
    int playing, looping, primary;
    float vol;
    int32_t vol_db, pan;
    uint8_t fmt[40];
    uint32_t nnotify;
    struct { uint32_t off; uint64_t ev; } notify[64];
} Buf;

static Buf* bufs[MAXBUF];
static int nbufs, mixer;
static ComKind K_DS = { "IDirectSound8", 12, { 0 } }, K_Buf = { "IDirectSoundBuffer8", 24, { 0 } }, K_Notify = { "IDirectSoundNotify", 4, { 0 } };
static const uint8_t IID_Notify[16] = { 0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 };
/* CLSID_DirectSound {47D4D946-62E8-11CF-93BC-444553540000}, CLSID_DirectSound8 {3901CC3F-84B5-4FA4-BA35-AA8172B8A09B} */
static const uint8_t CLSID_DS[16] = { 0x46, 0xD9, 0xD4, 0x47, 0xE8, 0x62, 0xCF, 0x11, 0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t CLSID_DS8[16] = { 0x3F, 0xCC, 0x01, 0x39, 0xB5, 0x84, 0xA4, 0x4F, 0xBA, 0x35, 0xAA, 0x81, 0x72, 0xB8, 0xA0, 0x9B };

static Buf* B(uint64_t o) { return o ? (Buf*)com_host(o) : NULL; }
static void ok(CPU* c) { RET(0); }

static float sample(const Buf* b, uint32_t frame, uint32_t chn)
{
    const uint8_t* p = G2H(b->mem);
    uint32_t k = frame * b->ch + (chn < b->ch ? chn : 0);
    if (b->flt) { float f; memcpy(&f, p + 4 * k, 4); return f; }
    if (b->bits == 8) return (p[k] - 128) / 128.0f;
    if (b->bits == 16) { int16_t s; memcpy(&s, p + 2 * k, 2); return s / 32768.0f; }
    if (b->bits == 24) { int32_t s = (int32_t)((uint32_t)p[3 * k] << 8 | (uint32_t)p[3 * k + 1] << 16 | (uint32_t)p[3 * k + 2] << 24); return s / 2147483648.0f; }
    int32_t s; memcpy(&s, p + 4 * k, 4); return s / 2147483648.0f;
}

static void fire(Buf* b, uint32_t from, uint32_t to)
{
    /* events for notification offsets the play cursor passed (bytes, [from, to) with wrap-around) */
    for (uint32_t i = 0; i < b->nnotify; i++) {
        uint32_t o = b->notify[i].off;
        if (o == 0xFFFFFFFFu) continue;
        int hit = from <= to ? (o >= from && o < to) : (o >= from || o < to);
        if (hit && b->notify[i].ev) sync_set_event(b->notify[i].ev);
    }
}
static void stopped(Buf* b)
{
    for (uint32_t i = 0; i < b->nnotify; i++) if (b->notify[i].off == 0xFFFFFFFFu && b->notify[i].ev) sync_set_event(b->notify[i].ev);
}

static void mix_loop(CPU* c)
{
    static float out[2 * PASS];
    sched_set_prio();
    for (;;) {
        if (host_audio_queued() > RATE / 20) { sched_sleep(c, 5); continue; }
        memset(out, 0, sizeof out);
        int any = 0;
        for (int i = 0; i < nbufs; i++) {
            Buf* b = bufs[i];
            if (!b || b->primary || !b->playing || !b->align || !b->mem) continue;
            uint32_t frames = b->size / b->align;
            if (!frames) continue;
            double step = (double)(b->freq ? b->freq : b->rate) / RATE;
            uint32_t before = (uint32_t)b->pos * b->align;
            float lv = b->vol * (b->pan > 0 ? powf(10.0f, -b->pan / 2000.0f) : 1.0f);
            float rv = b->vol * (b->pan < 0 ? powf(10.0f, b->pan / 2000.0f) : 1.0f);
            for (int n = 0; n < PASS; n++) {
                uint32_t f = (uint32_t)b->pos;
                if (f >= frames) {
                    if (!b->looping) { b->playing = 0; b->pos = 0; stopped(b); break; }
                    b->pos -= frames;
                    f = (uint32_t)b->pos;
                }
                out[2 * n] += sample(b, f, 0) * lv;
                out[2 * n + 1] += sample(b, f, b->ch > 1 ? 1 : 0) * rv;
                b->pos += step;
            }
            if (b->playing && b->looping && b->pos >= frames) b->pos -= frames * (uint32_t)(b->pos / frames);
            fire(b, before, ((uint32_t)b->pos % frames) * b->align);
            any = 1;
        }
        if (!any) { sched_sleep(c, 5); continue; }
        for (int n = 0; n < 2 * PASS; n++) out[n] = out[n] > 1 ? 1 : out[n] < -1 ? -1 : out[n];
        host_audio_write(out, PASS);
    }
}

static void set_format(Buf* b, uint64_t w)
{
    if (!w) return;
    uint16_t tag = (uint16_t)LD16(w);
    b->ch = (uint32_t)LD16(w + 2);
    b->rate = (uint32_t)LD32(w + 4);
    b->align = (uint32_t)LD16(w + 12);
    b->bits = (uint32_t)LD16(w + 14);
    b->flt = tag == 3 || (tag == 0xFFFE && LD32(w + 24) == 3);
    uint32_t n = tag == 1 || tag == 3 ? 16 : 18 + (uint32_t)LD16(w + 16);
    memcpy(b->fmt, G2H(w), n > sizeof b->fmt ? sizeof b->fmt : n);
    if (!b->align) b->align = b->ch * b->bits / 8;
}

/* ---- IDirectSound8 ---- */
static void ds_create_buffer(CPU* c)
{
    uint64_t d = A2;
    if (!d || !A3 || nbufs >= MAXBUF) { RET(0x80070057); return; }
    Buf* b = calloc(1, sizeof *b);
    b->flags = (uint32_t)LD32(d + 4);
    b->size = (uint32_t)LD32(d + 8);
    b->primary = b->flags & 1;
    b->vol = 1;
    b->ch = 2; b->rate = 44100; b->bits = 16; b->align = 4;
    set_format(b, LDP(d + 16));
    if (b->primary) b->size = b->size ? b->size : 4096;
    b->mem = g_calloc((uint64_t)b->size + 16);
    b->obj = com_new(&K_Buf, b, 0);
    bufs[nbufs++] = b;
    if (!mixer) {
        mixer = 1;
        host_audio_open(RATE);
        sched_create(rt_dyn_thunk(mix_loop, "dsound_mixer"), 0, 0);
    }
    STP(A3, b->obj);
    RET(0);
}
static void ds_caps(CPU* c)
{
    uint64_t p = A2;
    uint32_t n = p ? (uint32_t)LD32(p) : 0;
    if (n > 4) memset(G2H(p + 4), 0, n - 4);
    if (n >= 8) ST32(p + 4, 0x0F | 0x400 | 0xC00);    /* continuous rate, mono/stereo, 8/16 bit, secondary buffers */
    if (n >= 16) { ST32(p + 8, 100); ST32(p + 12, 200000); }
    RET(0);
}
static void ds_duplicate(CPU* c)
{
    Buf* s = B(A2);
    if (!s || !A3 || nbufs >= MAXBUF) { RET(0x80070057); return; }
    Buf* b = calloc(1, sizeof *b);
    *b = *s;
    b->playing = 0; b->pos = 0; b->nnotify = 0;
    b->obj = com_new(&K_Buf, b, 0);
    bufs[nbufs++] = b;
    STP(A3, b->obj);
    RET(0);
}
static void ds_speaker(CPU* c) { if (A2) ST32(A2, 4); RET(0); }   /* DSSPEAKER_STEREO */

/* ---- IDirectSoundBuffer8 ---- */
static void buf_qi(CPU* c)
{
    Buf* b = B(THIS);
    uint64_t o = THIS;
    if (b && iid_eq(A2, IID_Notify)) o = com_new(&K_Notify, b, 0);
    else ST32(THIS + 8, LD32(THIS + 8) + 1);
    STP(A3, o);
    RET(0);
}
static void buf_release(CPU* c)
{
    uint32_t n = (uint32_t)LD32(THIS + 8);
    if (n) n--;
    ST32(THIS + 8, n);
    Buf* b = B(THIS);
    if (!n && b) b->playing = 0;   /* the memory stays: the mixer may be reading it */
    RET(n);
}
static void buf_caps(CPU* c)
{
    Buf* b = B(THIS);
    uint64_t p = A2;
    if (!b || !p) { RET(0x80070057); return; }
    ST32(p + 4, b->flags); ST32(p + 8, b->size); ST32(p + 12, 0); ST32(p + 16, 0);
    RET(0);
}
static void buf_position(CPU* c)
{
    Buf* b = B(THIS);
    if (!b) { RET(0x80070057); return; }
    uint32_t play = b->align ? ((uint32_t)b->pos * b->align) % (b->size ? b->size : 1) : 0;
    /* the write cursor leads by ~15 ms, as on hardware */
    uint32_t lead = b->align * ((b->freq ? b->freq : b->rate) * 15 / 1000);
    if (A2) ST32(A2, play);
    if (A3) ST32(A3, b->size ? (play + lead) % b->size : 0);
    RET(0);
}
static void buf_format(CPU* c)
{
    Buf* b = B(THIS);
    uint32_t n = b ? (b->fmt[0] == 1 || b->fmt[0] == 3 ? 16 : 18 + (uint32_t)(b->fmt[16] | b->fmt[17] << 8)) : 0;
    if (n > sizeof b->fmt) n = sizeof b->fmt;
    if (A2 && b) memcpy(G2H(A2), b->fmt, n < (uint32_t)A3 ? n : (uint32_t)A3);
    if (A4) ST32(A4, n);
    RET(0);
}
static void buf_get_volume(CPU* c) { Buf* b = B(THIS); if (A2) ST32(A2, b ? (uint32_t)b->vol_db : 0); RET(0); }
static void buf_get_pan(CPU* c) { Buf* b = B(THIS); if (A2) ST32(A2, b ? (uint32_t)b->pan : 0); RET(0); }
static void buf_get_freq(CPU* c) { Buf* b = B(THIS); if (A2) ST32(A2, b ? (b->freq ? b->freq : b->rate) : 0); RET(0); }
static void buf_status(CPU* c) { Buf* b = B(THIS); if (A2) ST32(A2, b && b->playing ? 1 | (b->looping ? 4 : 0) : 0); RET(0); }
static void buf_lock(CPU* c)
{
    Buf* b = B(THIS);
    uint32_t off = (uint32_t)A2, n = (uint32_t)A3, fl = (uint32_t)ARG(8);
    if (!b || !b->size) { RET(0x80070057); return; }
    if (fl & 1) { uint32_t w; uint32_t play = b->align ? ((uint32_t)b->pos * b->align) % b->size : 0; w = play; off = w; }
    if (fl & 2) { off = 0; n = b->size; }
    off %= b->size;
    if (n > b->size) n = b->size;
    uint32_t first = off + n <= b->size ? n : b->size - off;
    if (A4) STP(A4, b->mem + off);
    if (ARG(5)) ST32(ARG(5), first);
    if (ARG(6)) STP(ARG(6), first < n ? b->mem : 0);
    if (ARG(7)) ST32(ARG(7), n - first);
    RET(0);
}
static void buf_play(CPU* c)
{
    Buf* b = B(THIS);
    if (b) { b->playing = 1; b->looping = ((uint32_t)ARG(4) & 1) != 0; }
    RET(0);
}
static void buf_set_pos(CPU* c) { Buf* b = B(THIS); if (b && b->align) b->pos = (double)((uint32_t)A2 / b->align); RET(0); }
static void buf_set_format(CPU* c) { Buf* b = B(THIS); if (b) set_format(b, A2); RET(0); }
static void buf_set_volume(CPU* c)
{
    Buf* b = B(THIS);
    if (b) { b->vol_db = (int32_t)A2; b->vol = b->vol_db <= -10000 ? 0.0f : powf(10.0f, b->vol_db / 2000.0f); }
    RET(0);
}
static void buf_set_pan(CPU* c) { Buf* b = B(THIS); if (b) b->pan = (int32_t)A2; RET(0); }
static void buf_set_freq(CPU* c) { Buf* b = B(THIS); if (b) b->freq = (uint32_t)A2; RET(0); }
static void buf_stop(CPU* c) { Buf* b = B(THIS); if (b && b->playing) { b->playing = 0; stopped(b); } RET(0); }
static void buf_unsupported(CPU* c) { RET(0x80004001); }

/* ---- IDirectSoundNotify: DSBPOSITIONNOTIFY { offset, pointer-sized event handle } ---- */
static void notify_set(CPU* c)
{
    Buf* b = B(THIS);
    uint32_t n = (uint32_t)A2, each = RT_G32 ? 8 : 16;
    if (!b) { RET(0x80070057); return; }
    b->nnotify = n < 64 ? n : 64;
    for (uint32_t i = 0; i < b->nnotify; i++) { b->notify[i].off = (uint32_t)LD32(A3 + (uint64_t)each * i); b->notify[i].ev = LDP(A3 + (uint64_t)each * i + GP); }
    RET(0);
}

static void setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    com_iunknown(&K_DS); com_iunknown(&K_Buf); com_iunknown(&K_Notify);
    static const char ds_args[] = "311423312222", buf_args[] = "311234222238422222151445";
    for (int i = 0; ds_args[i]; i++) K_DS.nargs[i] = (uint8_t)(ds_args[i] - '0');
    for (int i = 0; buf_args[i]; i++) K_Buf.nargs[i] = (uint8_t)(buf_args[i] - '0');
    K_Notify.nargs[3] = 3;
    K_DS.slots[3] = ds_create_buffer; K_DS.slots[4] = ds_caps; K_DS.slots[5] = ds_duplicate; K_DS.slots[6] = ok; K_DS.slots[7] = ok;
    K_DS.slots[8] = ds_speaker; K_DS.slots[9] = ok; K_DS.slots[10] = ok; K_DS.slots[11] = ok;
    K_Buf.slots[0] = buf_qi; K_Buf.slots[2] = buf_release;
    GuestFn s[24] = { 0, 0, 0, buf_caps, buf_position, buf_format, buf_get_volume, buf_get_pan, buf_get_freq, buf_status, ok, buf_lock, buf_play,
                      buf_set_pos, buf_set_format, buf_set_volume, buf_set_pan, buf_set_freq, buf_stop, ok, ok, buf_unsupported, buf_unsupported,
                      buf_unsupported };
    for (int i = 3; i < 24; i++) K_Buf.slots[i] = s[i];
    K_Notify.slots[3] = notify_set;
}

static uint64_t new_device(void) { setup(); return com_new(&K_DS, NULL, 0); }

/* CoCreateInstance(CLSID_DirectSound(8)): the object, initialised by the game's IDirectSound8::Initialize */
int dsound_create(CPU* c, uint64_t clsid, uint64_t out)
{
    if (!iid_eq(clsid, CLSID_DS) && !iid_eq(clsid, CLSID_DS8)) return 0;
    if (out) STP(out, new_device());
    RET(0);
    return 1;
}
HLE(DirectSoundCreate8) { if (!A2) { RET(0x80070057); return; } STP(A2, new_device()); RET(0); }
HLE(DirectSoundCreate) { if (!A2) { RET(0x80070057); return; } STP(A2, new_device()); RET(0); }
