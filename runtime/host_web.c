#ifdef __EMSCRIPTEN__
#include "host.h"

#include <emscripten.h>

void web_input_init(void);

/* standard-mapping pad -> XInput: buttons, triggers 0-255, sticks -32768..32767 (up positive) */
int host_gamepad(int index, uint16_t* buttons, uint8_t* triggers, int16_t* sticks);

/* WebAudio: an AudioWorklet plays chunks posted from here; the context starts on the first key or click */
int js_audio_open(int rate);

int js_audio_queued(void);

void js_audio_restart(void);
void js_audio_write(const float* frames, int count);

/* fast-forward: the output is paced by the game clock and the samples are dropped, so audio-timed engines keep up */
double host_speed(void);
static int audio_rate = 48000, ff_on;
static uint64_t ff_pushed, ff_start;
int host_audio_open(int rate) { audio_rate = rate; return js_audio_open(rate); }
int host_audio_queued(void)
{
    if (host_speed() == 1.0) {
        if (ff_on) { ff_on = 0; js_audio_restart(); }
        return js_audio_queued();
    }
    uint64_t now = host_ticks_ns();
    if (!ff_on) { ff_on = 1; ff_pushed = 0; ff_start = now; }
    uint64_t played = (now - ff_start) * (uint64_t)audio_rate / 1000000000ull;
    if (ff_pushed < played) { ff_start = now; ff_pushed = 0; return 0; }
    return (int)(ff_pushed - played);
}
void host_audio_write(const float* frames, int count)
{
    if (host_speed() == 1.0 && !ff_on) { js_audio_write(frames, count); return; }
    if (host_speed() == 1.0) { host_audio_queued(); js_audio_write(frames, count); return; }
    if (!ff_on) host_audio_queued();
    ff_pushed += (uint64_t)count;
}

void host_rumble(int index, int lo, int hi);

int web_next_event(int* out);

void host_poll_events(void)
{
    static int inited;
    if (!inited) { web_input_init(); inited = 1; }
    int e[5];
    while (web_next_event(e)) user32_event(e[0], e[1], e[2], e[3], e[4]);
}

/* ?name=1 in the page address: switches for comparing behaviours while debugging */
int host_query_flag(const char* name);
/* a number the page sets on Module (build settings such as the memory budget), or 0 */
int host_page_int(const char* name);
/* ?name=N in the page address, or 0 */
int host_query_int(const char* name);
char* web_trace_filter(void);
const char* host_trace_filter(void) { return web_trace_filter(); }

EM_ASYNC_JS(void, host_frame_yield, (void), {
    await new Promise(r => (typeof requestAnimationFrame !== "undefined" ? requestAnimationFrame : setTimeout)(r));
});

int web_canvas_w(void);
int web_canvas_h(void);
void host_canvas_size(int* w, int* h) { *w = web_canvas_w(); *h = web_canvas_h(); }

void host_show_cursor(int show);
void host_lock_pointer(int lock);
#endif
