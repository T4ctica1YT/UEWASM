#include "host.h"
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>

typedef struct { uint64_t proc; char name[128]; uint64_t atom; } WClass;
typedef struct { uint64_t hwnd, proc; int w, h, x, y, visible; uint64_t style, exstyle, userdata; } Wnd;
typedef struct { uint64_t hwnd, wparam, lparam; uint32_t msg, time; } Msg;

static WClass classes[64];
static int nclasses;
static Wnd wins[64];
static int nwins;
static Msg queue[1024];
static int qhead, qtail;
static uint8_t keys[256];
static int mouse_x, mouse_y, cursor_count;
/* the "desktop" is the canvas, so games default to the render size the page asked for */
static int screen_dim(int i) { int w, h; host_canvas_size(&w, &h); return i ? h : w; }
#define screen_w screen_dim(0)
#define screen_h screen_dim(1)
static int quit_posted;
static uint64_t raw_pending_dx, raw_pending_dy;
static int raw_x, raw_y, raw_buttons;

#define HWND_BASE 0x20000ull
#define WM_SIZE 0x0005
#define WM_CHAR 0x0102
#define WM_INPUT 0x00FF

static Wnd* wnd(uint64_t h)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i].hwnd == h) return &wins[i];
    return nwins ? &wins[0] : NULL;
}

static void post(uint64_t hwnd, uint32_t msg, uint64_t wp, uint64_t lp)
{
    int next = (qtail + 1) % 1024;
    if (next == qhead) return;
    queue[qtail] = (Msg){ hwnd, wp, lp, msg, (uint32_t)(host_ticks_ns() / 1000000) };
    qtail = next;
}

/* input goes to the window the game last showed or focused (UE makes helper windows first) */
static uint64_t focus_hwnd;
static uint64_t main_hwnd(void) { return focus_hwnd ? focus_hwnd : nwins ? wins[0].hwnd : 0; }

void user32_event(int type, int a, int b, int d1, int d2)
{
    uint64_t h = main_hwnd();
    if (!h) return;
    switch (type) {
    case HOST_EV_KEY_DOWN: keys[a & 255] = 0x80; post(h, a == 0x12 ? 0x104 : 0x100, (uint64_t)a, 1 | ((uint64_t)b << 16)); break;
    case HOST_EV_KEY_UP: keys[a & 255] = 0; post(h, a == 0x12 ? 0x105 : 0x101, (uint64_t)a, 0xC0000001ull | ((uint64_t)b << 16)); break;
    case HOST_EV_CHAR: post(h, WM_CHAR, (uint64_t)a, 1); break;
    case HOST_EV_MOUSE_MOVE:
        mouse_x = a; mouse_y = b;
        post(h, 0x200, (uint64_t)raw_buttons, (uint64_t)(uint16_t)a | ((uint64_t)(uint16_t)b << 16));
        if (d1 || d2) { raw_x += d1; raw_y += d2; post(h, WM_INPUT, 0, 0x1); }
        break;
    case HOST_EV_MOUSE_BUTTON: {
        static const uint32_t down[] = { 0x201, 0x204, 0x207, 0x20B, 0x20B }, up[] = { 0x202, 0x205, 0x208, 0x20C, 0x20C };
        static const int vk[] = { 1, 2, 4, 5, 6 };
        int btn = a < 5 ? a : 0;
        keys[vk[btn]] = b ? 0x80 : 0;
        raw_buttons = b ? raw_buttons | (1 << btn) : raw_buttons & ~(1 << btn);
        uint64_t wp = btn >= 3 ? ((uint64_t)(btn - 2) << 16) : 0;
        post(h, b ? down[btn] : up[btn], wp, (uint64_t)(uint16_t)mouse_x | ((uint64_t)(uint16_t)mouse_y << 16));
        break;
    }
    case HOST_EV_WHEEL: post(h, 0x20A, (uint64_t)(uint16_t)(int16_t)(a * 120) << 16, (uint64_t)(uint16_t)mouse_x | ((uint64_t)(uint16_t)mouse_y << 16)); break;
    case HOST_EV_RESIZE: {
        Wnd* w = wnd(h);
        w->w = a; w->h = b;
        post(h, WM_SIZE, 0, (uint64_t)(uint16_t)a | ((uint64_t)(uint16_t)b << 16));
        break;
    }
    case HOST_EV_FOCUS: post(h, 0x0006, a ? 1 : 0, 0); post(h, a ? 0x0007 : 0x0008, 0, 0); break;
    case HOST_EV_CLOSE: post(h, 0x0010, 0, 0); break;
    }
}

static uint64_t call_wndproc(CPU* c, Wnd* w, uint32_t msg, uint64_t wp, uint64_t lp)
{
    if (!w || !w->proc) return 0;
    uint64_t a[4] = { w->hwnd, msg, wp, lp };
    return rt_invoke(c, w->proc, 4, a);
}

static void wcopy_name(uint64_t p, char* out, size_t cap)
{
    if (p < 0x10000) snprintf(out, cap, "#%llu", (unsigned long long)p);
    else wstr_to_utf8(p, out, cap);
}

HLE(RegisterClassW)
{
    if (nclasses == 64) { RET(0); return; }
    WClass* k = &classes[nclasses++];
    k->proc = LD64(A1 + 8);
    wcopy_name(LD64(A1 + 64), k->name, sizeof k->name);
    k->atom = 0xC000 + (uint64_t)nclasses;
    RET(k->atom);
}
HLE(RegisterClassExW)
{
    if (nclasses == 64) { RET(0); return; }
    WClass* k = &classes[nclasses++];
    k->proc = LD64(A1 + 8);
    wcopy_name(LD64(A1 + 64), k->name, sizeof k->name);
    k->atom = 0xC000 + (uint64_t)nclasses;
    RET(k->atom);
}
STUB(UnregisterClassW, 1)

HLE(CreateWindowExW)
{
    char cls[128];
    wcopy_name(A2, cls, sizeof cls);
    WClass* k = NULL;
    for (int i = 0; i < nclasses; i++)
        if (!strcmp(classes[i].name, cls) || (A2 < 0x10000 && classes[i].atom == A2)) k = &classes[i];
    if (nwins == 64) { RET(0); return; }
    Wnd* w = &wins[nwins];
    memset(w, 0, sizeof *w);
    w->hwnd = HWND_BASE + 0x10ull * (uint64_t)(nwins + 1);
    w->proc = k ? k->proc : 0;
    w->style = A4;
    w->exstyle = A1;
    int x = (int)(int32_t)ARG(5), y = (int)(int32_t)ARG(6), cw = (int)(int32_t)ARG(7), ch = (int)(int32_t)ARG(8);
    host_canvas_size(&w->w, &w->h);
    if (cw > 0 && cw != (int)0x80000000 && !w->w) { w->w = cw; w->h = ch; }
    w->x = x == (int)0x80000000 ? 0 : x;
    w->y = y == (int)0x80000000 ? 0 : y;
    nwins++;
    uint64_t cs = g_calloc(80);
    ST64(cs, ARG(12));
    ST64(cs + 8, ARG(11));
    ST64(cs + 16, ARG(10));
    ST64(cs + 24, ARG(9));
    ST32(cs + 32, (uint32_t)w->h);
    ST32(cs + 36, (uint32_t)w->w);
    ST32(cs + 40, (uint32_t)w->y);
    ST32(cs + 44, (uint32_t)w->x);
    ST32(cs + 48, (uint32_t)A4);
    ST64(cs + 56, A3);
    ST64(cs + 64, A2);
    ST32(cs + 72, (uint32_t)A1);    uint64_t hwnd = w->hwnd;
    int idx = nwins - 1;
    call_wndproc(c, &wins[idx], 0x0081, 0, cs);
    call_wndproc(c, &wins[idx], 0x0001, 0, cs);
    call_wndproc(c, &wins[idx], WM_SIZE, 0, (uint64_t)(uint16_t)wins[idx].w | ((uint64_t)(uint16_t)wins[idx].h << 16));
    RET(hwnd);
}

HLE(DestroyWindow) { Wnd* w = wnd(A1); if (w) { call_wndproc(c, w, 0x0002, 0, 0); call_wndproc(c, w, 0x0082, 0, 0); } RET(1); }
HLE(DefWindowProcW)
{
    uint32_t msg = (uint32_t)A2;
    if (msg == 0x0010) { hle_DestroyWindow(c); RET(0); return; }
    if (msg == 0x0081) { RET(1); return; }
    RET(0);
}
HLE(CallWindowProcW) { uint64_t a[4] = { A2, A3, A4, ARG(5) }; RET(rt_invoke(c, A1, 4, a)); }

static int pump(CPU* c)
{
    host_poll_events();
    (void)c;
    return qhead != qtail;
}

static void write_msg(uint64_t p, Msg* m)
{
    ST64(p, m->hwnd);
    ST32(p + 8, m->msg);
    ST64(p + 16, m->wparam);
    ST64(p + 24, m->lparam);
    ST32(p + 32, m->time);
    ST32(p + 36, (uint32_t)mouse_x);
    ST32(p + 40, (uint32_t)mouse_y);
}

HLE(PeekMessageW)
{
    if (quit_posted == 1) {
        Msg q = { main_hwnd(), 0, 0, 0x12, 0 };
        write_msg(A1, &q);
        if (ARG(5) & 1) quit_posted = 2;
        RET(1);
        return;
    }
    if (!pump(c)) { RET(0); return; }
    write_msg(A1, &queue[qhead]);
    if (ARG(5) & 1) qhead = (qhead + 1) % 1024;
    RET(1);
}
HLE(PeekMessageA) { hle_PeekMessageW(c); }
HLE(GetMessageW)
{
    while (!pump(c) && !quit_posted) {
        sched_wait_frame(c);
    }
    if (quit_posted) { RET(0); return; }
    write_msg(A1, &queue[qhead]);
    qhead = (qhead + 1) % 1024;
    RET(1);
}
HLE(TranslateMessage) { RET(0); }
HLE(DispatchMessageW)
{
    Wnd* w = wnd(LD64(A1));
    RET(call_wndproc(c, w, (uint32_t)LD32(A1 + 8), LD64(A1 + 16), LD64(A1 + 24)));
}
HLE(DispatchMessageA) { hle_DispatchMessageW(c); }
HLE(PostMessageW) { post(A1 ? A1 : main_hwnd(), (uint32_t)A2, A3, A4); RET(1); }
HLE(SendMessageW) { RET(call_wndproc(c, wnd(A1), (uint32_t)A2, A3, A4)); }
HLE(PostQuitMessage) { quit_posted = 1; RET(0); }
HLE(PostThreadMessageW) { post(main_hwnd(), (uint32_t)A2, A3, A4); RET(1); }
STUB(WaitMessage, 1)
STUB(MsgWaitForMultipleObjects, 0x102)
STUB(GetQueueStatus, 0)

static void rect(uint64_t p, int l, int t, int r, int b) { ST32(p, (uint32_t)l); ST32(p + 4, (uint32_t)t); ST32(p + 8, (uint32_t)r); ST32(p + 12, (uint32_t)b); }

HLE(GetClientRect) { Wnd* w = wnd(A1); rect(A2, 0, 0, w ? w->w : screen_w, w ? w->h : screen_h); RET(1); }
HLE(GetWindowRect) { Wnd* w = wnd(A1); if (w) rect(A2, w->x, w->y, w->x + w->w, w->y + w->h); else rect(A2, 0, 0, screen_w, screen_h); RET(1); }
STUB(AdjustWindowRectEx, 1)
STUB(AdjustWindowRect, 1)
/* the canvas shows the main window's client area: screen = window origin + canvas position */
static void win_origin(uint64_t h, int* x, int* y) { Wnd* w = wnd(h ? h : main_hwnd()); *x = w ? w->x : 0; *y = w ? w->y : 0; }
HLE(ClientToScreen) { int x, y; win_origin(A1, &x, &y); if (A2) { ST32(A2, LD32(A2) + (uint32_t)x); ST32(A2 + 4, LD32(A2 + 4) + (uint32_t)y); } RET(1); }
HLE(ScreenToClient) { int x, y; win_origin(A1, &x, &y); if (A2) { ST32(A2, LD32(A2) - (uint32_t)x); ST32(A2 + 4, LD32(A2 + 4) - (uint32_t)y); } RET(1); }
HLE(SetWindowPos)
{
    Wnd* w = wnd(A1);
    if (w && !(ARG(7) & 1)) {
        int nw = (int)(int32_t)ARG(5), nh = (int)(int32_t)ARG(6);
        int cw, ch;
        host_canvas_size(&cw, &ch);
        if (nw > 0 && nh > 0 && !cw) { w->w = nw; w->h = nh; }
    }
    if (w && !(ARG(7) & 2)) { w->x = (int)(int32_t)A3; w->y = (int)(int32_t)A4; }
    RET(1);
}
HLE(MoveWindow) { RET(1); }
HLE(GetSystemMetrics)
{
    switch ((int)A1) {
    case 0: case 16: case 78: RET(screen_w); return;
    case 1: case 17: case 79: RET(screen_h); return;
    case 76: case 77: RET(0); return;
    case 80: RET(1); return;
    case 32: case 33: RET(4); return;
    case 4: RET(23); return;
    case 11: case 12: RET(32); return;
    case 43: RET(3); return;
    case 19: RET(1); return;
    default: RET(0); return;
    }
}
HLE(MonitorFromWindow) { RET(0x30001); }
HLE(MonitorFromPoint) { RET(0x30001); }
HLE(MonitorFromRect) { RET(0x30001); }
HLE(GetMonitorInfoW)
{
    uint32_t size = (uint32_t)LD32(A2);
    rect(A2 + 4, 0, 0, screen_w, screen_h);
    rect(A2 + 20, 0, 0, screen_w, screen_h);
    ST32(A2 + 36, 1);
    if (size >= 104) utf8_to_wstr("\\\\.\\DISPLAY1", A2 + 40, 32);
    RET(1);
}
HLE(EnumDisplayMonitors)
{
    uint64_t r = g_calloc(16);
    rect(r, 0, 0, screen_w, screen_h);
    uint64_t a[4] = { 0x30001, A1, r, A4 };
    rt_invoke(c, A3, 4, a);
    RET(1);
}
HLE(EnumDisplayDevicesW)
{
    uint32_t i = (uint32_t)A2;
    if (i > 0) { RET(0); return; }
    uint64_t d = A3;
    uint32_t cb = (uint32_t)LD32(d);
    memset(G2H(d + 4), 0, cb - 4);
    utf8_to_wstr(A1 ? "Web Monitor" : "\\\\.\\DISPLAY1", d + 4, 32);
    utf8_to_wstr(A1 ? "Generic Monitor" : "WebGL Display Adapter", d + 68, 128);
    ST32(d + 324, A1 ? 3 : 5);
    utf8_to_wstr("PCI\\VEN_1234&DEV_5678", d + 328, 128);
    RET(1);
}
HLE(EnumDisplayDevicesA)
{
    if ((uint32_t)A2 > 0) { RET(0); return; }
    uint64_t d = A3;
    uint32_t cb = (uint32_t)LD32(d);
    memset(G2H(d + 4), 0, cb - 4);
    snprintf((char*)G2H(d + 4), 32, "%s", A1 ? "Web Monitor" : "\\\\.\\DISPLAY1");
    snprintf((char*)G2H(d + 36), 128, "WebGL Display Adapter");
    ST32(d + 164, 5);
    RET(1);
}
static const int modes[][2] = { { 0, 0 }, { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 1024, 768 }, { 800, 600 } };
HLE(EnumDisplaySettingsW)
{
    int32_t idx = (int32_t)A2;
    int w = screen_w, h = screen_h;
    if (idx >= 1 && idx < 6) { w = modes[idx][0]; h = modes[idx][1]; }
    else if (idx >= 6) { RET(0); return; }
    uint64_t d = A3;
    uint16_t sz = (uint16_t)LD16(d + 68);
    memset(G2H(d), 0, sz ? sz : 220);
    utf8_to_wstr("WebDisplay", d, 32);
    ST16(d + 68, sz ? sz : 220);
    ST32(d + 72, 0x5C0000);
    ST32(d + 168, 32);
    ST32(d + 172, (uint32_t)w);
    ST32(d + 176, (uint32_t)h);
    ST32(d + 184, 60);
    RET(1);
}
HLE(EnumDisplaySettingsExW) { hle_EnumDisplaySettingsW(c); }
STUB(ChangeDisplaySettingsExW, 0)
STUB(ChangeDisplaySettingsW, 0)

HLE(ShowCursor) { cursor_count += A1 ? 1 : -1; host_show_cursor(cursor_count >= 0); RET((uint64_t)(int64_t)cursor_count); }
STUB(SetCursor, 0x50001)
STUB(LoadCursorW, 0x50001)
STUB(LoadCursorFromFileW, 0x50002)
STUB(LoadIconW, 0x50003)
STUB(LoadImageW, 0x50004)
STUB(CreateIconIndirect, 0x50005)
STUB(DestroyCursor, 1)
STUB(DestroyIcon, 1)
HLE(GetCursorPos) { int x, y; win_origin(0, &x, &y); ST32(A1, (uint32_t)(mouse_x + x)); ST32(A1 + 4, (uint32_t)(mouse_y + y)); RET(1); }
HLE(SetCursorPos) { int x, y; win_origin(0, &x, &y); mouse_x = (int)A1 - x; mouse_y = (int)A2 - y; RET(1); }
HLE(ClipCursor) { host_lock_pointer(A1 != 0); RET(1); }
HLE(GetClipCursor) { rect(A1, 0, 0, screen_w, screen_h); RET(1); }
HLE(GetKeyState) { RET((uint64_t)(int64_t)(int16_t)(keys[A1 & 255] ? 0x8000 : 0)); }
HLE(GetAsyncKeyState) { RET((uint64_t)(int64_t)(int16_t)(keys[A1 & 255] ? 0x8000 : 0)); }
HLE(GetKeyboardState) { memcpy(G2H(A1), keys, 256); RET(1); }
static const uint8_t vk_to_sc[256] = {
    [0x1B] = 0x01, ['1'] = 0x02, ['2'] = 0x03, ['3'] = 0x04, ['4'] = 0x05, ['5'] = 0x06, ['6'] = 0x07, ['7'] = 0x08, ['8'] = 0x09, ['9'] = 0x0A, ['0'] = 0x0B,
    [0x08] = 0x0E, [0x09] = 0x0F, ['Q'] = 0x10, ['W'] = 0x11, ['E'] = 0x12, ['R'] = 0x13, ['T'] = 0x14, ['Y'] = 0x15, ['U'] = 0x16, ['I'] = 0x17, ['O'] = 0x18,
    ['P'] = 0x19, [0x0D] = 0x1C, [0x11] = 0x1D, ['A'] = 0x1E, ['S'] = 0x1F, ['D'] = 0x20, ['F'] = 0x21, ['G'] = 0x22, ['H'] = 0x23, ['J'] = 0x24, ['K'] = 0x25,
    ['L'] = 0x26, [0x10] = 0x2A, ['Z'] = 0x2C, ['X'] = 0x2D, ['C'] = 0x2E, ['V'] = 0x2F, ['B'] = 0x30, ['N'] = 0x31, ['M'] = 0x32, [0x12] = 0x38, [0x20] = 0x39,
    [0x14] = 0x3A, [0x70] = 0x3B, [0x71] = 0x3C, [0x72] = 0x3D, [0x73] = 0x3E, [0x74] = 0x3F, [0x75] = 0x40, [0x76] = 0x41, [0x77] = 0x42, [0x78] = 0x43,
    [0x79] = 0x44, [0x7A] = 0x57, [0x7B] = 0x58, [0x26] = 0x48, [0x25] = 0x4B, [0x27] = 0x4D, [0x28] = 0x50, [0x24] = 0x47, [0x23] = 0x4F, [0x21] = 0x49,
    [0x22] = 0x51, [0x2D] = 0x52, [0x2E] = 0x53, [0xA0] = 0x2A, [0xA1] = 0x36, [0xA2] = 0x1D, [0xA4] = 0x38, [0xC0] = 0x29, [0xBD] = 0x0C, [0xBB] = 0x0D,
    [0xDB] = 0x1A, [0xDD] = 0x1B, [0xBA] = 0x27, [0xDE] = 0x28, [0xBC] = 0x33, [0xBE] = 0x34, [0xBF] = 0x35, [0xDC] = 0x2B,
};
HLE(MapVirtualKeyW)
{
    uint32_t code = (uint32_t)A1, type = (uint32_t)A2;
    if (type == 0 || type == 4) { RET(vk_to_sc[code & 255]); return; }
    if (type == 1 || type == 3) { for (int i = 0; i < 256; i++) if (vk_to_sc[i] == code && i) { RET(i); return; } RET(0); return; }
    if (type == 2) {
        /* the unshifted character of a key on a US layout; engines build their key names from this (UE: ` = Tilde) */
        static const uint8_t oem[256] = { [0x08] = 8, [0x09] = 9, [0x0D] = 13, [0x1B] = 27, [0x20] = ' ', [0xC0] = '`', [0xBD] = '-', [0xBB] = '=',
            [0xDB] = '[', [0xDD] = ']', [0xDC] = '\\', [0xBA] = ';', [0xDE] = '\'',[0xBC] = ',', [0xBE] = '.', [0xBF] = '/', [0x6A] = '*', [0x6B] = '+',
            [0x6D] = '-', [0x6E] = '.', [0x6F] = '/' };
        uint32_t k = code & 255;
        RET(k >= 'A' && k <= 'Z' ? k : k >= '0' && k <= '9' ? k : k >= 0x60 && k <= 0x69 ? '0' + (k - 0x60) : oem[k]);
        return;
    }
    RET(0);
}
HLE(MapVirtualKeyA) { hle_MapVirtualKeyW(c); }
STUB(GetKeyboardLayout, 0x04090409)
HLE(GetKeyNameTextW) { RET(0); }
HLE(ToUnicode) { RET(0); }
HLE(VkKeyScanW) { RET(A1 >= 'a' && A1 <= 'z' ? A1 - 32 : A1); }

STUB(RegisterRawInputDevices, 1)
HLE(GetRawInputData)
{
    uint64_t data = A3, size_p = A4;
    if (!data) { ST32(size_p, 48); RET(0); return; }
    memset(G2H(data), 0, 48);
    ST32(data, 0);
    ST32(data + 4, 48);
    ST64(data + 8, 0x60001);
    ST32(data + 36, (uint32_t)raw_x);
    ST32(data + 40, (uint32_t)raw_y);
    raw_x = raw_y = 0;
    RET(48);
}
HLE(GetRawInputDeviceList) { if (!A1) { ST32(A2, 1); RET(0); return; } ST64(A1, 0x60001); ST32(A1 + 8, 0); RET(1); }
HLE(GetRawInputDeviceInfoA) { if (A3) ST32(A4, 0); RET(0); }
HLE(GetRawInputDeviceInfoW) { if (A3) ST32(A4, 0); RET(0); }

HLE(GetForegroundWindow) { RET(main_hwnd()); }
HLE(GetActiveWindow) { RET(main_hwnd()); }
HLE(GetFocus) { RET(main_hwnd()); }
HLE(SetFocus) { uint64_t old = main_hwnd(); if (A1) focus_hwnd = A1; RET(old); }
HLE(SetActiveWindow) { uint64_t old = main_hwnd(); if (A1) focus_hwnd = A1; RET(old); }
HLE(SetForegroundWindow) { if (A1) focus_hwnd = A1; RET(1); }
HLE(GetDesktopWindow) { RET(0x10010); }
HLE(GetTopWindow) { RET(main_hwnd()); }
HLE(WindowFromPoint) { RET(main_hwnd()); }
HLE(GetCapture) { RET(0); }
HLE(SetCapture) { RET(0); }
STUB(ReleaseCapture, 1)
HLE(IsWindow) { RET(A1 >= HWND_BASE && A1 < HWND_BASE + 0x10ull * 65); }
STUB(IsWindowVisible, 1)
STUB(IsWindowEnabled, 1)
STUB(EnableWindow, 0)
STUB(IsIconic, 0)
STUB(IsZoomed, 0)
HLE(ShowWindow) { Wnd* w = wnd(A1); if (w) w->visible = A2 != 0; if (A2 && A2 != 7 && A2 != 8 && A2 != 4) focus_hwnd = A1; RET(1); }
STUB(UpdateWindow, 1)
STUB(SetWindowTextW, 1)
STUB(SetWindowTextA, 1)
HLE(GetWindowTextW) { if (A3) ST16(A2, 0); RET(0); }
HLE(GetWindowLongW)
{
    Wnd* w = wnd(A1);
    int32_t idx = (int32_t)A2;
    RET(!w ? 0 : idx == -16 ? w->style : idx == -20 ? w->exstyle : idx == -21 ? w->userdata : idx == -4 ? w->proc : 0);
}
HLE(SetWindowLongW)
{
    Wnd* w = wnd(A1);
    int32_t idx = (int32_t)A2;
    uint64_t old = 0;
    if (w) {
        if (idx == -16) { old = w->style; w->style = A3; }
        else if (idx == -20) { old = w->exstyle; w->exstyle = A3; }
        else if (idx == -21) { old = w->userdata; w->userdata = A3; }
        else if (idx == -4) { old = w->proc; w->proc = A3; }
    }
    RET(old);
}
HLE(GetWindowLongPtrW) { hle_GetWindowLongW(c); }
HLE(SetWindowLongPtrW) { hle_SetWindowLongW(c); }
HLE(GetDC) { RET(0x70001); }
STUB(ReleaseDC, 1)
HLE(BeginPaint) { memset(G2H(A2), 0, 72); ST64(A2, 0x70001); RET(0x70001); }
STUB(EndPaint, 1)
STUB(InvalidateRect, 1)
STUB(ValidateRect, 1)
HLE(GetWindowThreadProcessId) { if (A2) ST32(A2, 0x1000); RET(0x1004); }
STUB(SetWindowRgn, 1)
STUB(SetParent, 0)
STUB(GetParent, 0)
STUB(IsChild, 0)
STUB(EnumWindows, 1)
STUB(GetSystemMenu, 0)
STUB(EnableMenuItem, 0)
STUB(DrawStateW, 1)
STUB(SetLayeredWindowAttributes, 1)
HLE(GetWindowInfo)
{
    Wnd* w = wnd(A1);
    memset(G2H(A2 + 4), 0, 56);
    rect(A2 + 4, 0, 0, w ? w->w : screen_w, w ? w->h : screen_h);
    rect(A2 + 20, 0, 0, w ? w->w : screen_w, w ? w->h : screen_h);
    ST32(A2 + 36, w ? (uint32_t)w->style : 0);
    RET(1);
}
HLE(GetWindowPlacement) { Wnd* w = wnd(A1); memset(G2H(A2 + 4), 0, 40); ST32(A2 + 8, 1); rect(A2 + 28, 0, 0, w ? w->w : screen_w, w ? w->h : screen_h); RET(1); }
STUB(SetWindowPlacement, 1)
STUB(RegisterHotKey, 1)
STUB(UnregisterHotKey, 1)
STUB(DisableProcessWindowsGhosting, 0)
STUB(GetProcessWindowStation, 0x80001)
STUB(GetUserObjectInformationW, 0)
STUB(SendInput, 0)
STUB(GetTouchInputInfo, 0)
STUB(CloseTouchInputHandle, 1)
STUB(RegisterTouchWindow, 1)
STUB(OpenClipboard, 0)
STUB(CloseClipboard, 1)
STUB(EmptyClipboard, 1)
STUB(GetClipboardData, 0)
STUB(SetClipboardData, 0)
STUB(IsClipboardFormatAvailable, 0)
HLE(SetTimer) { RET(A2 ? A2 : 1); }
STUB(KillTimer, 1)
HLE(MessageBoxW) { char t[1024], m[2048]; wstr_to_utf8(A3, t, sizeof t); wstr_to_utf8(A2, m, sizeof m); host_log("[messagebox] %s: %s", t, m); RET(1); }
HLE(MessageBoxA) { host_log("[messagebox] %s: %s", A3 ? GSTR(A3) : "", A2 ? GSTR(A2) : ""); RET(1); }
STUB(DialogBoxParamW, 1)
STUB(EndDialog, 1)
STUB(GetDlgItem, 0)
STUB(SetDlgItemTextW, 1)
HLE(SystemParametersInfoW)
{
    uint32_t action = (uint32_t)A1;
    if (action == 0x30 && A3) { rect(A3, 0, 0, screen_w, screen_h); RET(1); return; }
    if (A3 && action == 0x68) { ST32(A3, 3); RET(1); return; }
    RET(0);
}
STUB(SetProcessDPIAware, 1)
STUB(SetProcessDpiAwareness, 0)
STUB(SetProcessDpiAwarenessContext, 1)
STUB(GetDpiForWindow, 96)
STUB(IsProcessDPIAware, 1)
STUB(GetDoubleClickTime, 500)
STUB(MessageBeep, 1)
STUB(FlashWindowEx, 1)
STUB(TrackMouseEvent, 1)
STUB(BringWindowToTop, 1)
STUB(RedrawWindow, 1)
