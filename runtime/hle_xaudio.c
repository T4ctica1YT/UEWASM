/* XAudio2 2.7 (the DirectX SDK version games create through COM) and X3DAudio. Its headers pack every struct to 1 byte,
   so pointers after a 32-bit field sit at offset 4 or 36, not 8 or 40. mixed here and played through the
   host's audio output. Source voices take PCM (8/16/24/32-bit, float) and MS-ADPCM; effects are accepted and skipped. */
#include "com.h"
#include "sched.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define I(a, b, c_, d0, d1, d2, d3, d4, d5, d6, d7) { (a) & 0xFF, ((a) >> 8) & 0xFF, ((a) >> 16) & 0xFF, ((a) >> 24) & 0xFF, (b) & 0xFF, ((b) >> 8) & 0xFF, (c_) & 0xFF, ((c_) >> 8) & 0xFF, d0, d1, d2, d3, d4, d5, d6, d7 }
static const uint8_t CLSID_XAudio2[16] = I(0x5a508685, 0xa254, 0x4fba, 0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf);
static const uint8_t CLSID_XAudio2_Debug[16] = I(0xdb05ea35, 0x0329, 0x4d4b, 0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52);
static const uint8_t CLSID_Reverb[16] = I(0x6a93130e, 0x1d53, 0x41d1, 0xa9, 0xcf, 0xe7, 0x58, 0x80, 0x0b, 0xb1, 0x79);
static const uint8_t CLSID_ReverbDbg[16] = I(0xc4f82dd4, 0xcb4e, 0x4ce1, 0x8b, 0xdb, 0xee, 0x32, 0xd4, 0x19, 0x82, 0x69);
static const uint8_t CLSID_Meter[16] = I(0xcac1105f, 0x619b, 0x4d04, 0x83, 0x1a, 0x44, 0xe1, 0xcb, 0xf1, 0x2d, 0x57);
static const uint8_t CLSID_MeterDbg[16] = I(0x2d9a0f9c, 0xe67b, 0x4b24, 0xab, 0x44, 0x92, 0xb3, 0xe7, 0x70, 0xc0, 0x20);
static const uint8_t IID_XAudio2[][16] = { I(0x8bcf1f58, 0x9fe7, 0x4583, 0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb) };
static const uint8_t IID_Any[][16] = { I(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0) };
static const uint8_t IID_XAudio2_9[][16] = { I(0x2b02e3cf, 0x2e0b, 0x4ec3, 0xbe, 0x45, 0x1b, 0x2a, 0x3f, 0xe7, 0x21, 0x0d) };
static const uint8_t CLSID_MMDeviceEnumerator[16] = I(0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
static const uint8_t IID_MMEnum[][16] = { I(0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6) };
static const uint8_t IID_MMDevice[][16] = { I(0xd666063f, 0x1587, 0x4e43, 0x81, 0xf1, 0xb9, 0x48, 0xe8, 0x07, 0x36, 0x3f) };
static const uint8_t IID_MMCollection[][16] = { I(0x0bd7a1be, 0x7a1a, 0x44db, 0x83, 0x97, 0xcc, 0x53, 0x92, 0x38, 0x7b, 0x5e) };
static const uint8_t IID_PropStore[][16] = { I(0x886d8eeb, 0x8cf2, 0x4446, 0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99) };
static const uint8_t PKEY_FriendlyName[16] = I(0xa45c254e, 0xdf1c, 0x4efd, 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0);
static const uint8_t PKEY_DeviceFormat[16] = I(0xf19f064d, 0x082c, 0x4e27, 0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c);

enum { V_SOURCE, V_SUBMIX, V_MASTER };
enum { RATE = 48000, PASS = 480, MAXCH = 8, MAXQ = 64, MAXOUT = 8 };

typedef struct {
    uint32_t flags, bytes, play_begin, play_len, loop_begin, loop_len, loop_count;
    uint64_t data, ctx;
    uint32_t frames, cursor, end, loops;
    int started;
} Buf;

typedef struct Voice {
    int kind, destroyed, stage;
    uint64_t obj, callback;
    uint32_t ch, rate, flags;
    /* source format */
    uint32_t tag, bits, align, spb;
    int16_t coef[32][2];
    int ncoef;
    float volume, chvol[MAXCH];
    int nout;
    struct Voice* out[MAXOUT];
    float matrix[MAXOUT][MAXCH * MAXCH];
    int has_matrix[MAXOUT];
    /* source playback */
    int started;
    float ratio;
    double frac;
    float f0[MAXCH], f1[MAXCH];
    int primed;
    Buf q[MAXQ];
    int qn, flushed_n, eos, starving, nsubmit, id;
    uint64_t starve_t;
    uint64_t flushed[MAXQ];
    uint64_t played;
    int32_t blk_index;
    int16_t blk[8192 * 2];
    /* submix / master input for the current pass */
    float acc[PASS * MAXCH];
    struct Reverb* rv;
} Voice;

static float fbits(uint64_t v);
static int reverb_tag;
typedef struct Reverb {
    int on;
    float wet, fb, damp, level;
    float comb[2][8][1700], cl[2][8];
    int cn[8], ci[2][8];
    float ap[2][4][600];
    int an[4], ai[2][4];
} Reverb;
static const int comb_len[8] = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 }, ap_len[4] = { 556, 441, 341, 225 };
static Reverb* reverb_new(void)
{
    Reverb* r = calloc(1, sizeof *r);
    r->on = 1; r->wet = 1; r->fb = 0.84f; r->damp = 0.25f; r->level = 0.1f;
    for (int i = 0; i < 8; i++) r->cn[i] = comb_len[i] * RATE / 44100;
    for (int i = 0; i < 4; i++) r->an[i] = ap_len[i] * RATE / 44100;
    return r;
}
/* XAUDIO2FX_REVERB_PARAMETERS (packed): WetDryMix % at 0, RoomFilterMain dB at 24, ReverbGain dB at 36, DecayTime s at 40.
   The reverb tail's level is room filter + reverb gain, typically around -20 dB. */
static void reverb_params(Reverb* r, uint64_t p, uint32_t size)
{
    if (!r || !p || size < 44) return;
    float wd = fbits(LD32(p)), room = fbits(LD32(p + 24)), gain = fbits(LD32(p + 36)), decay = fbits(LD32(p + 40));
    r->wet = wd < 0 ? 0 : wd > 100 ? 1 : wd / 100;
    float db = room + gain;
    db = db < -100 ? -100 : db > 20 ? 20 : db;
    r->level = powf(10.0f, db / 20.0f);
    static int said;
    if (said++ < 8 && host_query_flag("audiodebug"))
        host_log("[adbg] reverb: wet %.0f%%, room %.1f dB, gain %.1f dB, decay %.2f s", wd, room, gain, decay);
    if (decay < 0.1f) decay = 0.1f;
    /* comb feedback for a 60 dB fall over the decay time, at the combs' average length */
    float g = powf(10.0f, -3.0f * 0.029f / decay);
    r->fb = g < 0.5f ? 0.5f : g > 0.97f ? 0.97f : g;
}
static void reverb_run(Reverb* r, float* buf, uint32_t ch)
{
    if (!r->on || !ch) return;
    uint32_t right = ch > 1 ? 1 : 0;
    for (int i = 0; i < PASS; i++) {
        float in = (buf[i * ch] + buf[i * ch + right]) * 0.015f;
        for (int c = 0; c <= (int)right; c++) {
            float out = 0;
            int spread = c ? 23 : 0;
            for (int k = 0; k < 8; k++) {
                int n = r->cn[k] + spread, j = r->ci[c][k];
                float y = r->comb[c][k][j];
                r->cl[c][k] = y * (1 - r->damp) + r->cl[c][k] * r->damp;
                r->comb[c][k][j] = in + r->cl[c][k] * r->fb;
                r->ci[c][k] = j + 1 >= n ? 0 : j + 1;
                out += y;
            }
            for (int k = 0; k < 4; k++) {
                int n = r->an[k] + spread, j = r->ai[c][k];
                float b = r->ap[c][k][j];
                r->ap[c][k][j] = out + b * 0.5f;
                out = b - out;
                r->ai[c][k] = j + 1 >= n ? 0 : j + 1;
            }
            buf[i * ch + c] = out * 3.0f * r->wet * r->level;
        }
        for (uint32_t c = right + 1; c < ch; c++) buf[i * ch + c] = 0;
    }
}
/* XAUDIO2_EFFECT_CHAIN { u32 count; descriptor* @4 }, descriptor { IUnknown* effect; BOOL initial @8; u32 channels @12 } */
static void set_chain(Voice* v, uint64_t chain)
{
    if (!chain) return;
    uint32_t n = LD32(chain);
    uint64_t d = LD64(chain + 4);
    for (uint32_t i = 0; i < n && d; i++) {
        uint64_t fx = LD64(d + 16ull * i);
        if (fx && com_host(fx) == &reverb_tag) {
            if (!v->rv) { v->rv = reverb_new(); if (host_query_flag("audiodebug")) host_log("[adbg] voice %d has the reverb effect", v->id); }
            v->rv->on = LD32(d + 16ull * i + 8) != 0;
        }
    }
}
static void v_set_chain(CPU* c);
static void v_enable_fx(CPU* c);
static void v_disable_fx(CPU* c);
static void v_fx_params(CPU* c);

static ComKind K_Engine = { "IXAudio2", 16, {0}, IID_XAudio2, 1 };
static ComKind K_Source = { "IXAudio2SourceVoice", 29, {0} };
static ComKind K_Submix = { "IXAudio2SubmixVoice", 19, {0} };
static ComKind K_Master = { "IXAudio2MasteringVoice", 19, {0} };
/* XAudio2 2.8/2.9 (the flat XAudio2Create API newer engines use): no device enumeration or Initialize on the engine,
   GetChannelMask on the mastering voice; voices, buffers and callbacks are as in 2.7 */
static ComKind K_Engine29 = { "IXAudio2 2.9", 13, {0}, IID_XAudio2_9, 1 };
static ComKind K_Master29 = { "IXAudio2MasteringVoice 2.9", 20, {0} };
static ComKind K_Effect = { "IXAPO", 16, {0}, IID_Any, 0 };

static Voice* voices[512];
static int nvoices;
static Voice* master;
static uint64_t engine_cb[8];
static int nengine_cb, engine_running, thread_started, mixing, nstarved, nstall, adbg = -1;
#define ADBG(...) do { if (adbg < 0) adbg = host_query_flag("audiodebug"); if (adbg) host_log(__VA_ARGS__); } while (0)
static uint64_t stall_max;
static int nlimited;

static float fbits(uint64_t v) { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); return f; }
static Voice* VO(uint64_t obj) { return obj ? (Voice*)com_host(obj) : NULL; }

/* ---- calling back into the game ---- */

static void cb(CPU* c, uint64_t iface, int slot, int n, uint64_t a1, uint64_t a2)
{
    if (!iface) return;
    uint64_t fn = LD64(LD64(iface) + 8ull * slot);
    uint64_t args[3] = { iface, a1, a2 };
    rt_invoke(c, fn, n + 1, args);
}

/* ---- decoding ---- */

static const int adpcm_adapt[16] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

static void adpcm_block(Voice* v, const uint8_t* p)
{
    int ch = (int)v->ch, n = (int)v->spb;
    int pred[2], delta[2], s1[2], s2[2];
    for (int k = 0; k < ch; k++) pred[k] = p[k] < v->ncoef ? p[k] : 0;
    p += ch;
    for (int k = 0; k < ch; k++, p += 2) delta[k] = (int16_t)(p[0] | p[1] << 8);
    for (int k = 0; k < ch; k++, p += 2) s1[k] = (int16_t)(p[0] | p[1] << 8);
    for (int k = 0; k < ch; k++, p += 2) s2[k] = (int16_t)(p[0] | p[1] << 8);
    for (int k = 0; k < ch; k++) { v->blk[k] = (int16_t)s2[k]; v->blk[ch + k] = (int16_t)s1[k]; }
    int at = 2 * ch, nib = 0;
    for (int i = 2; i < n; i++)
        for (int k = 0; k < ch; k++) {
            int code = nib ? (*p++ & 15) : (*p >> 4);
            nib ^= 1;
            int pr = ((s1[k] * v->coef[pred[k]][0]) + (s2[k] * v->coef[pred[k]][1])) >> 8;
            int sc = code & 8 ? code - 16 : code;
            int s = pr + sc * delta[k];
            s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
            s2[k] = s1[k];
            s1[k] = s;
            delta[k] = delta[k] * adpcm_adapt[code] >> 8;
            if (delta[k] < 16) delta[k] = 16;
            v->blk[at++] = (int16_t)s;
        }
}

static void frame_at(Voice* v, Buf* b, uint32_t f, float* out)
{
    const uint8_t* p;
    switch (v->tag) {
    case 2: {
        int32_t bi = (int32_t)(f / v->spb);
        if (bi != v->blk_index) { adpcm_block(v, G2H(b->data + (uint64_t)bi * v->align)); v->blk_index = bi; }
        uint32_t k = f % v->spb;
        for (uint32_t c = 0; c < v->ch; c++) out[c] = v->blk[k * v->ch + c] / 32768.0f;
        return;
    }
    case 3:
        p = G2H(b->data + (uint64_t)f * v->align);
        for (uint32_t c = 0; c < v->ch; c++) memcpy(&out[c], p + 4 * c, 4);
        return;
    default:
        p = G2H(b->data + (uint64_t)f * v->align);
        for (uint32_t c = 0; c < v->ch; c++) {
            switch (v->bits) {
            case 8: out[c] = (p[c] - 128) / 128.0f; break;
            case 24: { int32_t s = (int32_t)((uint32_t)p[3 * c] << 8 | (uint32_t)p[3 * c + 1] << 16 | (uint32_t)p[3 * c + 2] << 24); out[c] = s / 2147483648.0f; break; }
            case 32: { int32_t s; memcpy(&s, p + 4 * c, 4); out[c] = s / 2147483648.0f; break; }
            default: out[c] = (int16_t)(p[2 * c] | p[2 * c + 1] << 8) / 32768.0f; break;
            }
        }
    }
}

/* next source frame, moving through the buffer queue and telling the game as buffers start, loop and end */
static int next_frame(CPU* c, Voice* v, float* out)
{
    for (;;) {
        if (!v->qn || !v->started) {
            if (v->started && v->played && !v->eos && !v->starving) {
                v->starving = 1; v->starve_t = host_ticks_ns(); nstarved++;
                ADBG("[adbg] voice %d ran dry: %u ch %u Hz tag %u, %d buffers submitted, %llu frames played", v->id, v->ch, v->rate, v->tag, v->nsubmit, (unsigned long long)v->played);
            }
            for (uint32_t k = 0; k < v->ch; k++) out[k] = 0;
            return 0;
        }
        v->starving = 0;
        Buf* b = &v->q[0];
        if (!b->started) {
            b->started = 1;
            v->blk_index = -1;
            cb(c, v->callback, 3, 1, b->ctx, 0);
            if (v->destroyed || !v->qn) continue;
        }
        if (b->cursor < b->end) {
            frame_at(v, b, b->cursor++, out);
            v->played++;
            if (b->loop_count && b->loops < b->loop_count && b->cursor >= b->loop_begin + (b->loop_len ? b->loop_len : b->end - b->loop_begin)) {
                if (b->loop_count != 255) b->loops++;
                b->cursor = b->loop_begin;
                cb(c, v->callback, 5, 1, b->ctx, 0);
            }
            return 1;
        }
        Buf done = *b;
        memmove(&v->q[0], &v->q[1], sizeof(Buf) * (size_t)(v->qn - 1));
        v->qn--;
        cb(c, v->callback, 4, 1, done.ctx, 0);
        if (done.flags & 0x40) { v->eos = 1; cb(c, v->callback, 2, 0, 0, 0); }
    }
}

/* ---- mixing ---- */

static void default_matrix(Voice* v, Voice* dst, float* m)
{
    uint32_t sc = v->ch, dc = dst->ch;
    memset(m, 0, sizeof(float) * sc * dc);
    if (sc == 1) { for (uint32_t d = 0; d < dc && d < 2; d++) m[d * sc] = dc >= 2 ? 0.7071f : 1.0f; return; }
    for (uint32_t s = 0; s < sc; s++) {
        if (dc == 2 && sc > 2) {
            if (s == 0) m[0] = 1.0f;
            else if (s == 1) m[sc + 1] = 1.0f;
            else if (s == 2) { m[s] = 0.7071f; m[sc + s] = 0.7071f; }
            else if (s >= 4) m[(s & 1) * sc + s] = 0.7071f;
        } else if (dc == 1) m[s] = s == 3 ? 0.0f : 1.0f / (sc > 1 ? sc : 1);
        else if (s < dc) m[s * sc + s] = 1.0f;
    }
}

static void send(Voice* v, const float* buf)
{
    int nout = v->nout;
    Voice* outs[MAXOUT];
    memcpy(outs, v->out, sizeof outs);
    if (!nout && master && v != master) { outs[0] = master; nout = 1; }
    for (int o = 0; o < nout; o++) {
        Voice* d = outs[o];
        if (!d || d->destroyed) continue;
        float m[MAXCH * MAXCH];
        if (v->has_matrix[o]) memcpy(m, v->matrix[o], sizeof m);
        else default_matrix(v, d, m);
        for (uint32_t dc = 0; dc < d->ch && dc < MAXCH; dc++)
            for (uint32_t sc = 0; sc < v->ch; sc++) {
                float g = m[dc * v->ch + sc] * v->volume * v->chvol[sc];
                if (g == 0) continue;
                for (int i = 0; i < PASS; i++) d->acc[i * d->ch + dc] += buf[i * v->ch + sc] * g;
            }
    }
}

static float tmp[PASS * MAXCH];

static void render_source(CPU* c, Voice* v)
{
    double step = (double)v->rate * v->ratio / RATE;
    if (!v->primed) { next_frame(c, v, v->f0); next_frame(c, v, v->f1); v->primed = 1; v->frac = 0; }
    for (int i = 0; i < PASS; i++) {
        while (v->frac >= 1.0) {
            memcpy(v->f0, v->f1, sizeof v->f0);
            next_frame(c, v, v->f1);
            v->frac -= 1.0;
            if (v->destroyed) return;
        }
        float t = (float)v->frac;
        for (uint32_t k = 0; k < v->ch; k++) tmp[i * v->ch + k] = v->f0[k] + (v->f1[k] - v->f0[k]) * t;
        v->frac += step;
    }
    send(v, tmp);
}

static float out_buf[PASS * 2];

static void mix_pass(CPU* c)
{
    mixing = 1;
    for (int i = 0; i < nengine_cb; i++) cb(c, engine_cb[i], 0, 0, 0, 0);
    for (int i = 0; i < nvoices; i++) if (voices[i]->kind != V_SOURCE) memset(voices[i]->acc, 0, sizeof voices[i]->acc);
    for (int i = 0; i < nvoices; i++) {
        Voice* v = voices[i];
        if (v->kind != V_SOURCE || v->destroyed) continue;
        while (v->flushed_n) cb(c, v->callback, 4, 1, v->flushed[--v->flushed_n], 0);
        if (!v->started) continue;
        uint32_t bytes = (uint32_t)(PASS * v->rate * v->ratio / RATE + 1) * (v->align ? v->align : 2);
        cb(c, v->callback, 0, 1, bytes, 0);
        if (!v->destroyed) render_source(c, v);
        if (!v->destroyed) cb(c, v->callback, 1, 0, 0, 0);
    }
    /* submix voices in processing-stage order, then the master */
    for (int stage = 0; stage < 64; stage++)
        for (int i = 0; i < nvoices; i++) {
            Voice* v = voices[i];
            if (v->kind == V_SUBMIX && !v->destroyed && v->stage == stage) {
                if (v->rv) reverb_run(v->rv, v->acc, v->ch);
                send(v, v->acc);
            }
        }
    if (master) {
        for (int i = 0; i < PASS; i++) {
            float l, r;
            if (master->ch == 1) l = r = master->acc[i];
            else { l = master->acc[i * master->ch]; r = master->acc[i * master->ch + 1];
                   if (master->ch >= 3) { float cc = master->acc[i * master->ch + 2] * 0.7071f; l += cc; r += cc; }
                   if (master->ch >= 6) { l += master->acc[i * master->ch + 4] * 0.7071f; r += master->acc[i * master->ch + 5] * 0.7071f; } }
            l *= master->volume; r *= master->volume;
            /* peak limiter: instant attack, ~150 ms release, so loud moments duck instead of clipping into fuzz */
            static float lim = 1;
            float pk = fabsf(l) > fabsf(r) ? fabsf(l) : fabsf(r);
            if (pk * lim > 0.95f) { lim = 0.95f / pk; nlimited++; }
            else lim += (1 - lim) * 0.00014f;
            l *= lim; r *= lim;
            out_buf[2 * i] = l < -1 ? -1 : l > 1 ? 1 : l;
            out_buf[2 * i + 1] = r < -1 ? -1 : r > 1 ? 1 : r;
        }
        host_audio_write(out_buf, PASS);
        /* a few early reports, so a muted test can still tell sound is being made */
        static int passes, reports;
        static float peak;
        for (int i = 0; i < PASS * 2; i++) peak = fabsf(out_buf[i]) > peak ? fabsf(out_buf[i]) : peak;
        if (++passes % 1000 == 0 && reports < 6) {
            int active = 0;
            for (int i = 0; i < nvoices; i++) active += voices[i]->kind == V_SOURCE && voices[i]->started && voices[i]->qn;
            host_log("[xaudio] %d voices playing, peak %.3f over the last 10 s", active, peak);
            peak = 0;
            reports++;
        }
    }
    for (int i = 0; i < nengine_cb; i++) cb(c, engine_cb[i], 1, 0, 0, 0);
    mixing = 0;
    /* voices destroyed during the pass go now */
    for (int i = 0; i < nvoices;) {
        if (voices[i]->destroyed) { free(voices[i]->rv); free(voices[i]); voices[i] = voices[--nvoices]; }
        else i++;
    }
}

/* the engine's own thread: keeps about 100 ms queued at the output, enough to ride out a long game frame */
static void audio_thread(CPU* c)
{
    uint64_t last = 0, window = 0, longest = 0, mix_ns = 0;
    int underruns = 0, reports = 0;
    sched_set_prio();
    for (;;) {
        int produced = 0;
        uint64_t now = host_ticks_ns();
        if (engine_running && master && last) {
            if (now - last > longest) longest = now - last;
            if (!host_audio_queued()) underruns++;
        }
        while (engine_running && master && host_audio_queued() < RATE / 10 && produced < 12) { mix_pass(c); produced++; }
        last = host_ticks_ns();
        mix_ns += last - now;
        if (!window) window = last;
        /* output that ran dry (heard as a cut-out) and sounds whose game-side stream fell behind */
        if (last - window > 10000000000ull) {
            if ((underruns || nstarved || nlimited) && reports < 60) {
                host_log("[xaudio] last 10 s: %d underruns, longest mixer gap %.0f ms, mixing %.0f ms, %d sounds ran out of data, "
                         "%d resumed after a gap (longest %.0f ms), %d samples limited", underruns, longest / 1e6, mix_ns / 1e6, nstarved, nstall,
                         stall_max / 1e6, nlimited);
                reports++;
            }
            underruns = nstarved = nstall = nlimited = 0;
            stall_max = 0;
            longest = mix_ns = 0;
            window = last;
        }
        sched_sleep(c, 5);
    }
}

/* ---- IXAudio2 ---- */

static Voice* new_voice(int kind, ComKind* k, uint32_t ch, uint32_t rate)
{
    if (nvoices >= 512) return NULL;
    Voice* v = calloc(1, sizeof *v);
    v->kind = kind;
    v->ch = ch ? (ch > MAXCH ? MAXCH : ch) : 2;
    v->rate = rate ? rate : RATE;
    v->volume = 1;
    v->ratio = 1;
    v->blk_index = -1;
    for (int i = 0; i < MAXCH; i++) v->chvol[i] = 1;
    v->obj = com_new(k, v, 0);
    static int ids;
    v->id = ++ids;
    voices[nvoices++] = v;
    return v;
}

static void set_sends(Voice* v, uint64_t sends)
{
    v->nout = 0;
    memset(v->has_matrix, 0, sizeof v->has_matrix);
    if (!sends) return;
    uint32_t n = LD32(sends);
    uint64_t list = LD64(sends + 4);
    for (uint32_t i = 0; i < n && i < MAXOUT && list; i++) {
        Voice* d = VO(LD64(list + 12ull * i + 4));
        if (d) v->out[v->nout++] = d;
    }
}

static void x_device_count(CPU* c) { if (A2) ST32(A2, 1); RET(0); }
static void x_device_details(CPU* c)
{
    uint64_t d = A3;
    if (A2 != 0) { RET(0x80070057); return; }
    memset(G2H(d), 0, 1068);
    utf8_to_wstr("uew-audio-out", d, 256);
    utf8_to_wstr("Speakers", d + 512, 256);
    ST32(d + 1024, 0xF);
    uint64_t f = d + 1028;
    ST16(f, 0xFFFE); ST16(f + 2, 2); ST32(f + 4, RATE); ST32(f + 8, RATE * 8); ST16(f + 12, 8); ST16(f + 14, 32); ST16(f + 16, 22);
    ST16(f + 18, 32); ST32(f + 20, 3);
    static const uint8_t ieee[16] = I(0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
    memcpy(G2H(f + 24), ieee, 16);
    RET(0);
}
static void x_ok(CPU* c) { RET(0); }
static void x_register(CPU* c) { if (A2 && nengine_cb < 8) engine_cb[nengine_cb++] = A2; RET(0); }
static void x_unregister(CPU* c)
{
    for (int i = 0; i < nengine_cb; i++) if (engine_cb[i] == A2) { engine_cb[i] = engine_cb[--nengine_cb]; break; }
    RET(0);
}

static void read_format(Voice* v, uint64_t fmt)
{
    v->tag = LD16(fmt);
    v->ch = LD16(fmt + 2);
    if (v->ch > MAXCH) v->ch = MAXCH;
    if (!v->ch) v->ch = 1;
    v->rate = LD32(fmt + 4);
    v->align = LD16(fmt + 12);
    v->bits = LD16(fmt + 14);
    if (v->tag == 0xFFFE) {
        /* WAVEFORMATEXTENSIBLE: the sub-format GUID's first word is the real tag */
        v->tag = LD16(fmt + 24);
        if (LD16(fmt + 18)) v->bits = LD16(fmt + 14);
    }
    if (v->tag == 2) {
        v->spb = LD16(fmt + 18);
        v->ncoef = LD16(fmt + 20);
        if (v->ncoef > 32) v->ncoef = 32;
        for (int i = 0; i < v->ncoef; i++) { v->coef[i][0] = (int16_t)LD16(fmt + 22 + 4ull * i); v->coef[i][1] = (int16_t)LD16(fmt + 24 + 4ull * i); }
        if (!v->spb) v->spb = 1;
    }
    if (!v->align) v->align = v->ch * (v->bits ? v->bits / 8 : 2);
}

static void x_create_source(CPU* c)
{
    Voice* v = new_voice(V_SOURCE, &K_Source, 1, 0);
    if (!v) { RET(0x8007000E); return; }
    read_format(v, A3);
    v->flags = (uint32_t)A4;
    v->callback = ARG(6);
    set_sends(v, ARG(7));
    ADBG("[adbg] source %d: %u ch %u Hz tag %u bits %u, flags %x, callback %llx, %d sends", v->id, v->ch, v->rate, v->tag, v->bits, v->flags, (unsigned long long)v->callback, v->nout);
    ST64(A2, v->obj);
    RET(0);
}
static void x_create_submix(CPU* c)
{
    Voice* v = new_voice(V_SUBMIX, &K_Submix, (uint32_t)A3, (uint32_t)A4);
    if (!v) { RET(0x8007000E); return; }
    v->stage = (int)(ARG(6) & 63);
    set_sends(v, ARG(7));
    set_chain(v, ARG(8));
    ADBG("[adbg] submix %d: %u ch %u Hz stage %d, %d sends", v->id, v->ch, v->rate, v->stage, v->nout);
    ST64(A2, v->obj);
    RET(0);
}
static void create_master(CPU* c, ComKind* k)
{
    Voice* v = new_voice(V_MASTER, k, (uint32_t)A3, RATE);
    if (!v) { RET(0x8007000E); return; }
    master = v;
    ADBG("[adbg] master %d: %u ch (asked %u)", v->id, v->ch, (unsigned)A3);
    ST64(A2, v->obj);
    RET(0);
}
static void x_create_master(CPU* c) { create_master(c, &K_Master); }
static void x_create_master29(CPU* c) { create_master(c, &K_Master29); }
/* IXAudio2MasteringVoice::GetChannelMask (2.8+) */
static void m_channel_mask(CPU* c)
{
    Voice* v = VO(A1);
    uint32_t ch = v ? v->ch : 2;
    if (A2) ST32(A2, ch == 1 ? 0x4 : ch == 6 ? 0x3F : ch == 8 ? 0x63F : 0x3);
    RET(0);
}
/* the engine runs from creation; StartEngine only matters after StopEngine */
static void start_engine(void)
{
    engine_running = 1;
    if (!thread_started) {
        thread_started = 1;
        host_log("[xaudio] engine started (%s)", master ? "mastering voice ready" : "no mastering voice yet");
        host_audio_open(RATE);
        sched_create(rt_dyn_thunk(audio_thread, "xaudio_thread"), 0, 0);
    }
}
static void x_start(CPU* c) { start_engine(); RET(0); }
static void x_stop(CPU* c) { engine_running = 0; RET(0); }
static void x_perf(CPU* c)
{
    memset(G2H(A2), 0, 64);
    int src = 0, sub = 0;
    for (int i = 0; i < nvoices; i++) { if (voices[i]->kind == V_SOURCE) src++; else if (voices[i]->kind == V_SUBMIX) sub++; }
    ST32(A2 + 20, PASS * 2);
    ST32(A2 + 28, (uint32_t)src); ST32(A2 + 32, (uint32_t)src); ST32(A2 + 36, (uint32_t)sub); ST32(A2 + 40, (uint32_t)sub);
    RET(0);
}

/* ---- IXAudio2Voice / SourceVoice ---- */

static void v_details(CPU* c) { Voice* v = VO(A1); if (v) { ST32(A2, v->flags); ST32(A2 + 4, v->ch); ST32(A2 + 8, v->rate); } RET(0); }
static void v_set_outputs(CPU* c) { Voice* v = VO(A1); if (v) set_sends(v, A2); RET(0); }
static void v_get_filter(CPU* c) { ST32(A2, 0); ST32(A2 + 4, 0x3F800000); ST32(A2 + 8, 0x3F800000); RET(0); }
static void v_get_out_filter(CPU* c) { ST32(A3, 0); ST32(A3 + 4, 0x3F800000); ST32(A3 + 8, 0x3F800000); RET(0); }
static void v_set_volume(CPU* c) { Voice* v = VO(A1); if (v) v->volume = c->x[1].f[0]; RET(0); }
static void v_get_volume(CPU* c) { Voice* v = VO(A1); float f = v ? v->volume : 1; uint32_t u; memcpy(&u, &f, 4); ST32(A2, u); RET(0); }
static void v_set_chvol(CPU* c)
{
    Voice* v = VO(A1);
    for (uint32_t i = 0; v && i < (uint32_t)A2 && i < MAXCH; i++) v->chvol[i] = fbits(LD32(A3 + 4ull * i));
    RET(0);
}
static void v_get_chvol(CPU* c)
{
    Voice* v = VO(A1);
    for (uint32_t i = 0; v && i < (uint32_t)A2 && i < MAXCH; i++) { uint32_t u; memcpy(&u, &v->chvol[i], 4); ST32(A3 + 4ull * i, u); }
    RET(0);
}
static int out_index(Voice* v, uint64_t dst)
{
    Voice* d = VO(dst);
    if (!d) return 0;
    for (int i = 0; i < v->nout; i++) if (v->out[i] == d) return i;
    if (!v->nout && d == master) return 0;
    return -1;
}
static void v_set_matrix(CPU* c)
{
    Voice* v = VO(A1);
    uint32_t sc = (uint32_t)A3, dc = (uint32_t)A4;
    uint64_t m = ARG(5);
    int o = v ? out_index(v, A2) : -1;
    if (v && o >= 0 && m && sc <= MAXCH && dc <= MAXCH) {
        memset(v->matrix[o], 0, sizeof v->matrix[o]);
        for (uint32_t d = 0; d < dc; d++)
            for (uint32_t s = 0; s < sc && s < v->ch; s++) v->matrix[o][d * v->ch + s] = fbits(LD32(m + 4ull * (d * sc + s)));
        v->has_matrix[o] = 1;
    }
    static int nm;
    if (v && nm++ < 40) {
        char t[256]; int k = 0;
        for (uint32_t i = 0; m && i < sc * dc && i < 16 && k < 230; i++) k += snprintf(t + k, sizeof t - k, " %.2f", fbits(LD32(m + 4ull * i)));
        Voice* d = VO(A2);
        ADBG("[adbg] matrix voice %d -> %d (%u ch), %ux%u, out %d:%s", v->id, d ? d->id : 0, d ? d->ch : 0, sc, dc, o, t);
    }
    RET(0);
}
static void v_get_matrix(CPU* c)
{
    Voice* v = VO(A1);
    uint32_t sc = (uint32_t)A3, dc = (uint32_t)A4;
    uint64_t m = ARG(5);
    int o = v ? out_index(v, A2) : -1;
    for (uint32_t d = 0; m && d < dc; d++)
        for (uint32_t s = 0; s < sc; s++) {
            float f = v && o >= 0 && v->has_matrix[o] && s < v->ch ? v->matrix[o][d * v->ch + s] : (d == s ? 1.0f : 0.0f);
            uint32_t u;
            memcpy(&u, &f, 4);
            ST32(m + 4ull * (d * sc + s), u);
        }
    RET(0);
}
static void v_destroy(CPU* c)
{
    Voice* v = VO(A1);
    if (!v) return;
    v->destroyed = 1;
    v->qn = 0;
    if (v == master) master = NULL;
    for (int i = 0; i < nvoices; i++)
        for (int o = 0; o < voices[i]->nout; o++) if (voices[i]->out[o] == v) voices[i]->out[o] = NULL;
    if (!mixing) {
        for (int i = 0; i < nvoices; i++) if (voices[i] == v) { voices[i] = voices[--nvoices]; break; }
        free(v->rv);
        free(v);
    }
}
static void v_set_chain(CPU* c) { Voice* v = VO(A1); if (v) set_chain(v, A2); RET(0); }
static void v_enable_fx(CPU* c) { Voice* v = VO(A1); if (v && v->rv) v->rv->on = 1; RET(0); }
static void v_disable_fx(CPU* c) { Voice* v = VO(A1); if (v && v->rv) v->rv->on = 0; RET(0); }
static void v_fx_params(CPU* c) { Voice* v = VO(A1); if (v) reverb_params(v->rv, A3, (uint32_t)A4); RET(0); }
static void s_start(CPU* c) { Voice* v = VO(A1); if (v) v->started = 1; RET(0); }
static void s_stop(CPU* c) { Voice* v = VO(A1); if (v) { if (v->starving) ADBG("[adbg] voice %d stopped while dry", v->id); v->started = 0; } RET(0); }
static void s_submit(CPU* c)
{
    Voice* v = VO(A1);
    uint64_t x = A2;
    if (!v || !x) { RET(0x80070057); return; }
    if (v->qn >= MAXQ) { RET(0x88960001); return; }
    Buf* b = &v->q[v->qn];
    memset(b, 0, sizeof *b);
    b->flags = LD32(x);
    b->bytes = LD32(x + 4);
    b->data = LD64(x + 8);
    b->play_begin = LD32(x + 16);
    b->play_len = LD32(x + 20);
    b->loop_begin = LD32(x + 24);
    b->loop_len = LD32(x + 28);
    b->loop_count = LD32(x + 32);
    b->ctx = LD64(x + 36);
    b->frames = v->tag == 2 ? b->bytes / (v->align ? v->align : 1) * v->spb : b->bytes / (v->align ? v->align : 1);
    b->cursor = b->play_begin;
    b->end = b->play_len ? b->play_begin + b->play_len : b->frames;
    if (b->end > b->frames) b->end = b->frames;
    if (!b->loop_count) b->loop_begin = 0;
    v->qn++;
    v->nsubmit++;
    if (v->nsubmit <= 2 || v->starving) ADBG("[adbg] voice %d submit #%d: %u bytes flags %x loop %u+%u x%u, queue %d%s", v->id, v->nsubmit, b->bytes, b->flags, b->loop_begin, b->loop_len, b->loop_count, v->qn, v->starving ? " (was dry)" : "");
    v->eos = 0;
    if (v->starving) {
        uint64_t gap = host_ticks_ns() - v->starve_t;
        nstall++;
        if (gap > stall_max) stall_max = gap;
        v->starving = 0;
    }
    RET(0);
}
static void s_flush(CPU* c)
{
    Voice* v = VO(A1);
    if (v) {
        /* everything but the buffer playing now; their OnBufferEnd comes from the audio thread */
        int keep = v->qn && v->q[0].started && v->started ? 1 : 0;
        for (int i = keep; i < v->qn && v->flushed_n < MAXQ; i++) v->flushed[v->flushed_n++] = v->q[i].ctx;
        v->qn = keep;
    }
    RET(0);
}
static void s_exit_loop(CPU* c) { Voice* v = VO(A1); if (v && v->qn) v->q[0].loop_count = v->q[0].loops; RET(0); }
static void s_state(CPU* c)
{
    Voice* v = VO(A1);
    ST64(A2, v && v->qn ? v->q[0].ctx : 0);
    ST32(A2 + 8, v ? (uint32_t)v->qn : 0);
    ST64(A2 + 12, v ? v->played : 0);
    RET(0);
}
static void s_set_ratio(CPU* c) { Voice* v = VO(A1); if (v) { float r = c->x[1].f[0]; v->ratio = r < 0.0005f ? 0.0005f : r > 1024 ? 1024 : r; } RET(0); }
static void s_get_ratio(CPU* c) { Voice* v = VO(A1); float f = v ? v->ratio : 1; uint32_t u; memcpy(&u, &f, 4); ST32(A2, u); RET(0); }
static void s_set_rate(CPU* c) { Voice* v = VO(A1); if (v && A2) v->rate = (uint32_t)A2; RET(0); }

static void setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    ComKind* k = &K_Engine;
    com_iunknown(k);
    k->slots[3] = x_device_count; k->slots[4] = x_device_details; k->slots[5] = x_ok; k->slots[6] = x_register; k->slots[7] = x_unregister;
    k->slots[8] = x_create_source; k->slots[9] = x_create_submix; k->slots[10] = x_create_master; k->slots[11] = x_start;
    k->slots[12] = x_stop; k->slots[13] = x_ok; k->slots[14] = x_perf; k->slots[15] = x_ok;
    ComKind* vk[3] = { &K_Source, &K_Submix, &K_Master };
    for (int i = 0; i < 3; i++) {
        GuestFn base[19] = { v_details, v_set_outputs, v_set_chain, v_enable_fx, v_disable_fx, x_ok, v_fx_params, x_ok, x_ok, v_get_filter, x_ok, v_get_out_filter,
                             v_set_volume, v_get_volume, v_set_chvol, v_get_chvol, v_set_matrix, v_get_matrix, v_destroy };
        for (int s = 0; s < 19; s++) vk[i]->slots[s] = base[s];
    }
    K_Source.slots[19] = s_start; K_Source.slots[20] = s_stop; K_Source.slots[21] = s_submit; K_Source.slots[22] = s_flush;
    K_Source.slots[23] = x_ok; K_Source.slots[24] = s_exit_loop; K_Source.slots[25] = s_state; K_Source.slots[26] = s_set_ratio;
    K_Source.slots[27] = s_get_ratio; K_Source.slots[28] = s_set_rate;
    com_iunknown(&K_Effect);
    for (int s = 3; s < 16; s++) K_Effect.slots[s] = x_ok;
    k = &K_Engine29;
    com_iunknown(k);
    k->slots[3] = x_register; k->slots[4] = x_unregister; k->slots[5] = x_create_source; k->slots[6] = x_create_submix;
    k->slots[7] = x_create_master29; k->slots[8] = x_start; k->slots[9] = x_stop; k->slots[10] = x_ok; k->slots[11] = x_perf;
    k->slots[12] = x_ok;
    for (int s = 0; s < 19; s++) K_Master29.slots[s] = K_Master.slots[s];
    K_Master29.slots[19] = m_channel_mask;
}

/* XAudio2 2.9 entry points (xaudio2_9.dll / xaudio2_9redist.dll) */
static void create29(CPU* c)
{
    setup();
    if (A1) ST64(A1, com_new(&K_Engine29, NULL, 0));
    start_engine();
    RET(0);
}
HLE(XAudio2Create) { create29(c); }
HLE(XAudio2CreateWithVersionInfo) { create29(c); }
HLE(CreateAudioReverb) { setup(); if (A1) ST64(A1, com_new(&K_Effect, &reverb_tag, 0)); RET(0); }
HLE(CreateAudioVolumeMeter) { setup(); if (A1) ST64(A1, com_new(&K_Effect, NULL, 0)); RET(0); }

/* ---- the one audio endpoint (MMDevice API), so engines that enumerate devices find speakers ---- */

static ComKind K_MMEnum = { "IMMDeviceEnumerator", 8, {0}, IID_MMEnum, 1 };
static ComKind K_MMCollection = { "IMMDeviceCollection", 5, {0}, IID_MMCollection, 1 };
static ComKind K_MMDevice = { "IMMDevice", 7, {0}, IID_MMDevice, 1 };
static ComKind K_PropStore = { "IPropertyStore", 8, {0}, IID_PropStore, 1 };
static uint64_t mm_device;

static uint64_t task_wstr(const char* s)
{
    uint64_t p = g_alloc(strlen(s) * 2 + 2);
    utf8_to_wstr(s, p, strlen(s) + 1);
    return p;
}
static uint64_t device(void) { if (!mm_device) mm_device = com_new(&K_MMDevice, NULL, 0); return mm_device; }
static void mm_enum(CPU* c) { if (A4) ST64(A4, com_new(&K_MMCollection, NULL, 0)); RET(0); }
static void mm_default(CPU* c) { if (A4) ST64(A4, device()); RET(0); }
static void mm_get(CPU* c) { if (A3) ST64(A3, device()); RET(0); }
static void mm_count(CPU* c) { if (A2) ST32(A2, 1); RET(0); }
static void mm_item(CPU* c) { if (A3) ST64(A3, A2 == 0 ? device() : 0); RET(A2 == 0 ? 0 : 0x80070490); }
/* ---- WASAPI shared mode (FMOD, Wwise and others): float stereo at RATE straight to the host output; the period
   event is signalled by a green thread every 10 ms ---- */
static const uint8_t IID_AudioClient[][16] = { I(0x1CB9AD4C, 0xDBFA, 0x4c32, 0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2),
    I(0x726778CD, 0xF60A, 0x4eda, 0x82, 0xDE, 0xE4, 0x76, 0x10, 0xCD, 0x78, 0xAA), I(0x7ED4EE07, 0x8E67, 0x4CD4, 0x8C, 0x1A, 0x2B, 0x7A, 0x59, 0x87, 0xAD, 0x42) };
static const uint8_t IID_RenderClient[][16] = { I(0xF294ACFC, 0x3146, 0x4483, 0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2) };
static const uint8_t IID_AudioClock[][16] = { I(0xCD63314F, 0x3FBA, 0x4a1b, 0x81, 0x2C, 0xEF, 0x96, 0x35, 0x87, 0x28, 0xE7) };
static const uint8_t IID_SimpleVolume[][16] = { I(0x87CE5498, 0x68D6, 0x44E5, 0x92, 0x15, 0x6D, 0xA4, 0x7E, 0xF8, 0x83, 0xD8),
    I(0x93014887, 0x242D, 0x4068, 0x8A, 0x15, 0xCF, 0x5E, 0x93, 0xB9, 0x0F, 0xE3) };
static ComKind K_AudioClient = { "IAudioClient", 20, {0}, IID_AudioClient, 3 };
static ComKind K_RenderClient = { "IAudioRenderClient", 5, {0}, IID_RenderClient, 1 };
static ComKind K_AudioClock = { "IAudioClock", 6, {0}, IID_AudioClock, 1 };
static ComKind K_Volume = { "ISimpleAudioVolume", 12, {0}, IID_SimpleVolume, 2 };
void sync_set_event(uint64_t h);
static struct { uint32_t frames, bits, channels, started; uint64_t staging, event, written; int ticker; } wa;

static void wa_format(uint64_t f)
{
    ST16(f, 0xFFFE); ST16(f + 2, 2); ST32(f + 4, RATE); ST32(f + 8, RATE * 8); ST16(f + 12, 8); ST16(f + 14, 32); ST16(f + 16, 22);
    ST16(f + 18, 32); ST32(f + 20, 3);
    static const uint8_t ieee[16] = I(0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
    memcpy(G2H(f + 24), ieee, 16);
}
static uint32_t wa_padding(void) { uint32_t q = (uint32_t)host_audio_queued(); return q > wa.frames ? wa.frames : q; }
static void wa_thread(CPU* c)
{
    sched_set_prio();
    for (;;) {
        if (wa.started && wa.event) sync_set_event(wa.event);
        sched_sleep(c, 10);
    }
}
static void ac_init(CPU* c)
{
    uint64_t dur = A4, fmt = ARG(6);
    uint32_t want = (uint32_t)(dur * RATE / 10000000ull);
    wa.frames = want < RATE / 20 ? RATE / 20 : want > RATE ? RATE : want;
    wa.channels = fmt ? (uint32_t)LD16(fmt + 2) : 2;
    wa.bits = fmt ? (uint32_t)LD16(fmt + 14) : 32;
    if (!wa.staging) wa.staging = g_calloc((uint64_t)RATE * 8 * 2);
    host_audio_open(RATE);
    host_log("[wasapi] stream: %u ch, %u bit, %u frame buffer", wa.channels, wa.bits, wa.frames);
    RET(0);
}
static void ac_buffer_size(CPU* c) { if (A2) ST32(A2, wa.frames); RET(0); }
static void ac_latency(CPU* c) { if (A2) ST64(A2, 100000); RET(0); }
static void ac_padding(CPU* c) { if (A2) ST32(A2, wa_padding()); RET(0); }
static void ac_supported(CPU* c) { if (A4) ST64(A4, 0); RET(0); }
static void ac_mix_format(CPU* c) { uint64_t f = g_alloc(40); wa_format(f); if (A2) ST64(A2, f); RET(0); }
static void ac_period(CPU* c) { if (A2) ST64(A2, 100000); if (A3) ST64(A3, 30000); RET(0); }
static void ac_start(CPU* c)
{
    wa.started = 1;
    if (!wa.ticker) { wa.ticker = 1; sched_create(rt_dyn_thunk(wa_thread, "wasapi_period"), 0, 0); }
    RET(0);
}
static void ac_stop(CPU* c) { wa.started = 0; RET(0); }
static void ac_event(CPU* c) { wa.event = A2; RET(0); }
static void ac_service(CPU* c)
{
    ComKind* k = iid_eq(A2, IID_RenderClient[0]) ? &K_RenderClient : iid_eq(A2, IID_AudioClock[0]) ? &K_AudioClock
               : iid_eq(A2, IID_SimpleVolume[0]) || iid_eq(A2, IID_SimpleVolume[1]) ? &K_Volume : NULL;
    if (A3) ST64(A3, k ? com_new(k, NULL, 0) : 0);
    RET(k ? 0 : 0x80004002);
}
static void rc_get(CPU* c)
{
    uint32_t n = (uint32_t)A2;
    if (n > wa.frames - wa_padding()) { if (A3) ST64(A3, 0); RET(0x88890006); return; }
    if (A3) ST64(A3, wa.staging);
    RET(0);
}
static void rc_release(CPU* c)
{
    uint32_t n = (uint32_t)A2;
    float* p = (float*)G2H(wa.staging);
    if (A3 & 2) memset(p, 0, (size_t)n * 8);
    else if (wa.bits == 16) {
        /* 16-bit PCM: widen in place, back to front */
        const int16_t* s = (const int16_t*)p;
        for (int64_t i = (int64_t)n * 2 - 1; i >= 0; i--) p[i] = s[i] / 32768.0f;
    }
    if (wa.channels == 1) for (int64_t i = (int64_t)n - 1; i >= 0; i--) { p[2 * i + 1] = p[i]; p[2 * i] = p[i]; }
    host_audio_write(p, (int)n);
    wa.written += n;
    RET(0);
}
static void clk_freq(CPU* c) { if (A2) ST64(A2, RATE); RET(0); }
static void clk_pos(CPU* c)
{
    uint64_t played = wa.written > (uint64_t)host_audio_queued() ? wa.written - (uint64_t)host_audio_queued() : 0;
    if (A2) ST64(A2, played);
    if (A3) ST64(A3, host_ticks_ns() / 100);
    RET(0);
}
static void vol_get(CPU* c) { if (A2) ST32(A2, 0x3F800000); RET(0); }
static void mute_get(CPU* c) { if (A2) ST32(A2, 0); RET(0); }
static void wa_setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    ComKind* ks[4] = { &K_AudioClient, &K_RenderClient, &K_AudioClock, &K_Volume };
    for (int i = 0; i < 4; i++) { com_iunknown(ks[i]); for (int s = 3; s < ks[i]->nslots; s++) ks[i]->slots[s] = x_ok; }
    GuestFn ac[] = { ac_init, ac_buffer_size, ac_latency, ac_padding, ac_supported, ac_mix_format, ac_period, ac_start, ac_stop, x_ok, ac_event, ac_service };
    for (int i = 0; i < 12; i++) K_AudioClient.slots[3 + i] = ac[i];
    K_RenderClient.slots[3] = rc_get; K_RenderClient.slots[4] = rc_release;
    K_AudioClock.slots[3] = clk_freq; K_AudioClock.slots[4] = clk_pos;
    K_Volume.slots[4] = vol_get; K_Volume.slots[6] = mute_get;
}
static void mm_activate(CPU* c)
{
    if (iid_eq(A2, IID_AudioClient[0]) || iid_eq(A2, IID_AudioClient[1]) || iid_eq(A2, IID_AudioClient[2])) {
        wa_setup();
        if (ARG(5)) ST64(ARG(5), com_new(&K_AudioClient, NULL, 0));
        RET(0);
        return;
    }
    if (ARG(5)) ST64(ARG(5), 0);
    RET(0x80004002);
}
static void mm_props(CPU* c) { if (A3) ST64(A3, com_new(&K_PropStore, NULL, 0)); RET(0); }
static void mm_id(CPU* c) { if (A2) ST64(A2, task_wstr("{0.0.0.00000000}.{uew-audio-out}")); RET(0); }
static void mm_state(CPU* c) { if (A2) ST32(A2, 1); RET(0); }
static void ps_count(CPU* c) { if (A2) ST32(A2, 0); RET(0); }
static void ps_value(CPU* c)
{
    uint64_t key = A2, pv = A3;
    memset(G2H(pv), 0, 24);
    if (iid_eq(key, PKEY_FriendlyName) && LD32(key + 16) == 14) {
        ST16(pv, 31);
        ST64(pv + 8, task_wstr("Speakers (Browser)"));
    } else if (iid_eq(key, PKEY_DeviceFormat)) {
        uint64_t f = g_alloc(40);
        memset(G2H(f), 0, 40);
        ST16(f, 0xFFFE); ST16(f + 2, 2); ST32(f + 4, RATE); ST32(f + 8, RATE * 8); ST16(f + 12, 8); ST16(f + 14, 32); ST16(f + 16, 22);
        ST16(f + 18, 32); ST32(f + 20, 3);
        static const uint8_t ieee[16] = I(0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
        memcpy(G2H(f + 24), ieee, 16);
        ST16(pv, 65);
        ST32(pv + 8, 40);
        ST64(pv + 16, f);
    }
    RET(0);
}

static void mm_setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    ComKind* ks[4] = { &K_MMEnum, &K_MMCollection, &K_MMDevice, &K_PropStore };
    for (int i = 0; i < 4; i++) { com_iunknown(ks[i]); for (int s = 3; s < ks[i]->nslots; s++) ks[i]->slots[s] = x_ok; }
    K_MMEnum.slots[3] = mm_enum; K_MMEnum.slots[4] = mm_default; K_MMEnum.slots[5] = mm_get;
    K_MMCollection.slots[3] = mm_count; K_MMCollection.slots[4] = mm_item;
    K_MMDevice.slots[3] = mm_activate; K_MMDevice.slots[4] = mm_props; K_MMDevice.slots[5] = mm_id; K_MMDevice.slots[6] = mm_state;
    K_PropStore.slots[3] = ps_count; K_PropStore.slots[5] = ps_value;
}

/* called by CoCreateInstance for classes we provide */
int xaudio_create(CPU* c, uint64_t clsid, uint64_t out)
{
    ComKind* k = NULL;
    if (iid_eq(clsid, CLSID_MMDeviceEnumerator)) {
        mm_setup();
        if (out) ST64(out, com_new(&K_MMEnum, NULL, 0));
        RET(0);
        return 1;
    }
    if (iid_eq(clsid, CLSID_XAudio2) || iid_eq(clsid, CLSID_XAudio2_Debug)) k = &K_Engine;
    else if (iid_eq(clsid, CLSID_Reverb) || iid_eq(clsid, CLSID_ReverbDbg) || iid_eq(clsid, CLSID_Meter) || iid_eq(clsid, CLSID_MeterDbg)) k = &K_Effect;
    if (!k) return 0;
    setup();
    int rev = iid_eq(clsid, CLSID_Reverb) || iid_eq(clsid, CLSID_ReverbDbg);
    if (out) ST64(out, com_new(k, rev ? &reverb_tag : NULL, 0));
    if (k == &K_Engine) start_engine();
    RET(0);
    return 1;
}

/* ---- X3DAudio: equal-power panning and distance curves ---- */

typedef struct { float x, y, z; } V3;
static V3 v3(uint64_t p) { V3 v = { fbits(LD32(p)), fbits(LD32(p + 4)), fbits(LD32(p + 8)) }; return v; }
static float dot3(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 cross3(V3 a, V3 b) { V3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; return r; }

static float curve(uint64_t cv, float d, float def)
{
    if (!cv) return def;
    uint64_t pts = LD64(cv);
    uint32_t n = LD32(cv + 8);
    if (!pts || !n) return def;
    float pd = fbits(LD32(pts)), pv = fbits(LD32(pts + 4));
    if (d <= pd) return pv;
    for (uint32_t i = 1; i < n; i++) {
        float nd = fbits(LD32(pts + 8ull * i)), nv = fbits(LD32(pts + 8ull * i + 4));
        if (d <= nd) return nd > pd ? pv + (nv - pv) * (d - pd) / (nd - pd) : nv;
        pd = nd; pv = nv;
    }
    return pv;
}

static void stf(uint64_t a, float f) { uint32_t u; memcpy(&u, &f, 4); ST32(a, u); }

HLE(X3DAudioInitialize)
{
    /* handle: speaker mask, speed of sound */
    ST32(A3, (uint32_t)A1);
    stf(A3 + 4, c->x[1].f[0]);
    RET(0);
}

HLE(X3DAudioCalculate)
{
    uint64_t L = A2, E = A3, D = ARG(5);
    uint32_t flags = (uint32_t)A4;
    V3 lf = v3(L), lt = v3(L + 12), lp = v3(L + 24), ep = v3(E + 32);
    V3 rel = { ep.x - lp.x, ep.y - lp.y, ep.z - lp.z };
    float dist = sqrtf(dot3(rel, rel));
    V3 right = cross3(lt, lf);
    float az = atan2f(dot3(rel, right), dot3(rel, lf));
    float scaler = fbits(LD32(E + 120));
    if (scaler <= 0) scaler = 1;
    float nd = dist / scaler;
    float gain = curve(LD64(E + 80), nd, nd <= 1 ? 1.0f : 1.0f / nd);
    uint32_t sc = LD32(D + 16), dc = LD32(D + 20), ech = LD32(E + 64);
    uint64_t mat = LD64(D), azs = LD64(E + 72);
    if ((flags & 1) && mat) {
        for (uint32_t s = 0; s < sc; s++) {
            float a = az + (s < ech && azs ? fbits(LD32(azs + 4ull * s)) : 0);
            float pan = sinf(a);
            float th = (pan + 1) * 0.785398f;
            float l = cosf(th) * gain, r = sinf(th) * gain;
            for (uint32_t d = 0; d < dc; d++) stf(mat + 4ull * (d * sc + s), d == 0 ? l : d == 1 ? r : 0.0f);
            if (dc == 1) stf(mat + 4ull * s, gain);
        }
    }
    stf(D + 24, 1); stf(D + 28, 1);
    stf(D + 32, (flags & 0x10) ? curve(LD64(E + 112), nd, 0) : 0);
    stf(D + 36, 1);
    stf(D + 40, az);
    stf(D + 44, dist);
    stf(D + 48, 0); stf(D + 52, 0);
    RET(0);
}

/* XAPOFX (EQ, echo, limiter): accepted and passed through; engines build their mix graph around these effects */
HLE(CreateFX)
{
    setup();
    if (A2) ST64(A2, com_new(&K_Effect, NULL, 0));
    RET(0);
}

/* ---- winmm waveOut: queued PCM buffers played at real-time pace into the same output, resampled to RATE ---- */
typedef struct WaveOut {
    int open;
    uint32_t rate, ch, bits, flt;
    uint64_t cb, inst;
    uint32_t cb_type;           /* CALLBACK_WINDOW 1, THREAD 2, FUNCTION 3, EVENT 5 (bits 16-19 of the flags) */
    uint64_t q[64]; int qn;     /* WAVEHDRs waiting */
    double pos;                 /* resampler position in the head buffer, in source frames */
    float vol;
    int paused;
    uint64_t played;            /* output frames written */
} WaveOut;
static WaveOut wo;
static int wo_thread;
void user32_post(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp);

/* WAVEHDR: data +0, length +8, recorded +12, user +16, flags +24 (DONE 1, PREPARED 2, INQUEUE 0x10), loops +28 */
static void wo_notify(CPU* c, uint32_t msg, uint64_t hdr)
{
    if (wo.cb_type == 3 && wo.cb) { uint64_t a[5] = { 0x7C000001, msg, wo.inst, hdr, 0 }; rt_invoke(c, wo.cb, 5, a); }
    else if (wo.cb_type == 5 && wo.cb) sync_set_event(wo.cb);
    else if ((wo.cb_type == 1 || wo.cb_type == 2) && wo.cb) user32_post(wo.cb_type == 1 ? wo.cb : 0, msg, 0x7C000001, hdr);
}
static float wo_sample(const uint8_t* p, uint32_t i, uint32_t ch)
{
    uint32_t k = i * wo.ch + (ch < wo.ch ? ch : 0);
    if (wo.flt) { float f; memcpy(&f, p + 4 * k, 4); return f; }
    if (wo.bits == 8) return (p[k] - 128) / 128.0f;
    if (wo.bits == 16) { int16_t s; memcpy(&s, p + 2 * k, 2); return s / 32768.0f; }
    if (wo.bits == 24) { int32_t s = (int32_t)((uint32_t)p[3 * k] << 8 | (uint32_t)p[3 * k + 1] << 16 | (uint32_t)p[3 * k + 2] << 24); return s / 2147483648.0f; }
    int32_t s; memcpy(&s, p + 4 * k, 4); return s / 2147483648.0f;
}
static void wo_loop(CPU* c)
{
    static float out[2 * PASS];
    sched_set_prio();
    for (;;) {
        if (!wo.open || wo.paused || !wo.qn || host_audio_queued() > RATE / 10) { sched_sleep(c, 5); continue; }
        int n = 0;
        double step = (double)wo.rate / RATE;
        uint32_t fs = wo.ch * (wo.flt ? 4 : wo.bits / 8);
        while (n < PASS && wo.qn) {
            uint64_t h = wo.q[0];
            uint32_t frames = fs ? (uint32_t)LD32(h + 8) / fs : 0;
            const uint8_t* p = G2H(LD64(h));
            while (n < PASS && wo.pos < frames) {
                uint32_t i = (uint32_t)wo.pos;
                out[2 * n] = wo_sample(p, i, 0) * wo.vol;
                out[2 * n + 1] = wo_sample(p, i, 1) * wo.vol;
                n++;
                wo.pos += step;
            }
            if (wo.pos >= frames) {
                wo.pos -= frames;
                memmove(wo.q, wo.q + 1, sizeof wo.q[0] * (size_t)--wo.qn);
                ST32(h + 24, ((uint32_t)LD32(h + 24) & ~0x10u) | 1);
                wo_notify(c, 0x3BD, h);   /* WOM_DONE */
            }
        }
        if (n) { host_audio_write(out, n); wo.played += (uint64_t)n; }
    }
}
/* waveOutOpen(&h, device, WAVEFORMATEX*, callback, instance, flags) */
HLE(waveOutOpen)
{
    uint64_t fmt = A3;
    uint32_t flags = (uint32_t)ARG(6);
    if (!fmt) { RET(11); return; }   /* MMSYSERR_INVALPARAM */
    uint16_t tag = (uint16_t)LD16(fmt);
    uint32_t ch = (uint32_t)LD16(fmt + 2), rate = (uint32_t)LD32(fmt + 4), bits = (uint32_t)LD16(fmt + 14);
    int flt = tag == 3 || (tag == 0xFFFE && LD32(fmt + 24) == 3);   /* IEEE float, or extensible with the float subformat */
    if (!ch || !rate || (bits != 8 && bits != 16 && bits != 24 && bits != 32) || (tag != 1 && tag != 3 && tag != 0xFFFE)) { RET(32); return; }   /* WAVERR_BADFORMAT */
    if (flags & 1) { RET(0); return; }   /* WAVE_FORMAT_QUERY: supported */
    memset(&wo, 0, sizeof wo);
    wo.open = 1; wo.ch = ch; wo.rate = rate; wo.bits = bits; wo.flt = flt; wo.vol = 1;
    wo.cb = A4; wo.inst = ARG(5); wo.cb_type = (flags >> 16) & 7;
    if (A1) ST64(A1, 0x7C000001);
    host_audio_open(RATE);
    if (!wo_thread) { wo_thread = 1; sched_create(rt_dyn_thunk(wo_loop, "waveout"), 0, 0); }
    wo_notify(c, 0x3BB, 0);   /* WOM_OPEN */
    RET(0);
}
HLE(waveOutPrepareHeader) { if (A2) ST32(A2 + 24, (uint32_t)LD32(A2 + 24) | 2); RET(0); }
HLE(waveOutUnprepareHeader) { if (A2) ST32(A2 + 24, (uint32_t)LD32(A2 + 24) & ~2u); RET(0); }
HLE(waveOutWrite)
{
    if (!wo.open || !A2) { RET(5); return; }   /* MMSYSERR_INVALHANDLE */
    if (wo.qn == 64) { RET(4); return; }      /* MMSYSERR_ALLOCATED: queue full */
    ST32(A2 + 24, ((uint32_t)LD32(A2 + 24) & ~1u) | 0x10);
    wo.q[wo.qn++] = A2;
    RET(0);
}
HLE(waveOutReset)
{
    while (wo.qn) { uint64_t h = wo.q[0]; memmove(wo.q, wo.q + 1, sizeof wo.q[0] * (size_t)--wo.qn); ST32(h + 24, ((uint32_t)LD32(h + 24) & ~0x10u) | 1); wo_notify(c, 0x3BD, h); }
    wo.pos = 0;
    RET(0);
}
HLE(waveOutClose) { if (wo.open) { hle_waveOutReset(c); wo_notify(c, 0x3BC, 0); } wo.open = 0; RET(0); }   /* WOM_CLOSE */
HLE(waveOutPause) { wo.paused = 1; RET(0); }
HLE(waveOutRestart) { wo.paused = 0; RET(0); }
/* MMTIME: type +0, value +4 (TIME_BYTES 4, TIME_SAMPLES 2, TIME_MS 1) */
HLE(waveOutGetPosition)
{
    if (!A2) { RET(11); return; }
    uint64_t frames = wo.played * wo.rate / RATE;
    uint32_t type = (uint32_t)LD32(A2);
    if (type == 2) ST32(A2 + 4, (uint32_t)frames);
    else if (type == 1) ST32(A2 + 4, (uint32_t)(frames * 1000 / (wo.rate ? wo.rate : 1)));
    else { ST32(A2, 4); ST32(A2 + 4, (uint32_t)(frames * wo.ch * (wo.flt ? 4 : wo.bits / 8))); }
    RET(0);
}
HLE(waveOutGetNumDevs) { RET(1); }
/* WAVEOUTCAPSW: mid, pid, version, name[32], formats, channels, reserved, support */
HLE(waveOutGetDevCapsW)
{
    if (!A2) { RET(11); return; }
    memset(G2H(A2), 0, (size_t)(uint32_t)A3);
    const char* n = "Speakers";
    for (int i = 0; n[i]; i++) ST16(A2 + 8 + 2 * i, (uint8_t)n[i]);
    ST32(A2 + 72, 0xFFFFF); ST16(A2 + 76, 2); ST32(A2 + 80, 0x2C);
    RET(0);
}
HLE(waveOutGetVolume) { if (A2) { uint32_t v = (uint32_t)(wo.vol * 0xFFFF); ST32(A2, v | v << 16); } RET(0); }
HLE(waveOutSetVolume) { wo.vol = ((uint32_t)A2 & 0xFFFF) / 65535.0f; RET(0); }
HLE(waveOutGetID) { if (A2) ST32(A2, 0); RET(0); }
HLE(waveOutMessage) { RET(8); }   /* MMSYSERR_NOTSUPPORTED */
