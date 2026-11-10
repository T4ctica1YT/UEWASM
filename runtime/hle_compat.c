/* APIs found across a scan of many shipped games (UE, Unity, GameMaker, custom engines): rectangle helpers, ANSI
   variants, wsprintf, OLE automation arrays and variants, path helpers, SIDs, UUIDs, base64, and the services a
   browser doesn't have (LDAP, audio capture, HID devices, Media Foundation), which fail the way Windows does when
   they are absent. */
#include "host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void hle_set_error(uint32_t e);
void hle_GetWindowLongW(CPU*);
void hle_SetWindowLongW(CPU*);
void hle_SystemParametersInfoW(CPU*);
void hle_SendMessageW(CPU*);
void hle_CallWindowProcW(CPU*);
void hle_CopyFileW(CPU*);
void hle_MoveFileW(CPU*);
void hle_GetModuleFileNameW(CPU*);
void hle_GetModuleFileNameA(CPU*);
void hle_DeleteFileW(CPU*);
void hle_RemoveDirectoryW(CPU*);

static void rd_a(uint64_t p, char* out, size_t cap) { size_t i = 0; for (; p && i + 1 < cap; i++) { out[i] = (char)LD8(p + i); if (!out[i]) return; } out[i] = 0; }
static void rd_w(uint64_t p, char* out, size_t cap) { if (!p) { out[0] = 0; return; } wstr_to_utf8(p, out, cap); }
static uint64_t a2w(uint64_t a)
{
    if (!a) return 0;
    char s[4096];
    rd_a(a, s, sizeof s);
    uint64_t w = g_alloc(2 * strlen(s) + 2);
    utf8_to_wstr(s, w, strlen(s) + 1);
    return w;
}
/* a resource name: an integer id below 0x10000, otherwise an ANSI string turned wide */
static uint64_t res_a2w(uint64_t a) { return a < 0x10000 ? a : a2w(a); }
static void res_free(uint64_t orig, uint64_t w) { if (orig >= 0x10000 && w) g_free(w); }
static void put_a(uint64_t dst, size_t cap, const char* s) { if (!dst || !cap) return; size_t n = strlen(s); if (n + 1 > cap) n = cap - 1; memcpy(G2H(dst), s, n); ST8(dst + n, 0); }

/* ---- rectangles ---- */

static int32_t rl(uint64_t r, int i) { return (int32_t)LD32(r + 4ull * i); }
static void rs(uint64_t r, int32_t l, int32_t t, int32_t rr, int32_t b) { ST32(r, (uint32_t)l); ST32(r + 4, (uint32_t)t); ST32(r + 8, (uint32_t)rr); ST32(r + 12, (uint32_t)b); }
static int rempty(uint64_t r) { return rl(r, 0) >= rl(r, 2) || rl(r, 1) >= rl(r, 3); }

HLE(SetRect) { if (!A1) { RET(0); return; } rs(A1, (int32_t)A2, (int32_t)A3, (int32_t)A4, (int32_t)ARG(5)); RET(1); }
HLE(SetRectEmpty) { if (!A1) { RET(0); return; } rs(A1, 0, 0, 0, 0); RET(1); }
HLE(CopyRect) { if (!A1 || !A2) { RET(0); return; } memmove(G2H(A1), G2H(A2), 16); RET(1); }
HLE(IsRectEmpty) { RET(!A1 || rempty(A1)); }
HLE(EqualRect) { RET(A1 && A2 && !memcmp(G2H(A1), G2H(A2), 16)); }
HLE(OffsetRect)
{
    if (!A1) { RET(0); return; }
    int32_t dx = (int32_t)A2, dy = (int32_t)A3;
    rs(A1, rl(A1, 0) + dx, rl(A1, 1) + dy, rl(A1, 2) + dx, rl(A1, 3) + dy);
    RET(1);
}
HLE(InflateRect)
{
    if (!A1) { RET(0); return; }
    int32_t dx = (int32_t)A2, dy = (int32_t)A3;
    rs(A1, rl(A1, 0) - dx, rl(A1, 1) - dy, rl(A1, 2) + dx, rl(A1, 3) + dy);
    RET(1);
}
HLE(UnionRect)
{
    if (!A1 || !A2 || !A3) { RET(0); return; }
    int ea = rempty(A2), eb = rempty(A3);
    if (ea && eb) { rs(A1, 0, 0, 0, 0); RET(0); return; }
    if (ea) { memmove(G2H(A1), G2H(A3), 16); RET(1); return; }
    if (eb) { memmove(G2H(A1), G2H(A2), 16); RET(1); return; }
    int32_t l = rl(A2, 0) < rl(A3, 0) ? rl(A2, 0) : rl(A3, 0), t = rl(A2, 1) < rl(A3, 1) ? rl(A2, 1) : rl(A3, 1);
    int32_t r = rl(A2, 2) > rl(A3, 2) ? rl(A2, 2) : rl(A3, 2), b = rl(A2, 3) > rl(A3, 3) ? rl(A2, 3) : rl(A3, 3);
    rs(A1, l, t, r, b);
    RET(1);
}
HLE(SubtractRect) { if (!A1 || !A2) { RET(0); return; } memmove(G2H(A1), G2H(A2), 16); RET(!rempty(A1)); }

/* ---- wsprintf: integers, characters and strings (no floating point, as on Windows); at most 1024 characters ---- */

typedef struct VArgs { CPU* c; uint64_t va; int n; } VArgs;
static uint64_t va_next(VArgs* a)
{
    CPU* c = a->c;
    int i = a->n++;
    if (a->va) return LD64(a->va + 8ull * i);
    return i == 0 ? A3 : i == 1 ? A4 : ARG(5 + i - 2);
}

static size_t gfmt(VArgs* va, int wide, uint64_t fmt, char* out, size_t cap)
{
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < cap) out[o++] = (char)(ch); } while (0)
    for (uint64_t p = fmt;; p += wide ? 2 : 1) {
        uint32_t ch = wide ? LD16(p) : LD8(p);
        if (!ch) break;
        if (ch != '%') {
            if (ch < 0x80) PUT(ch);
            else if (ch < 0x800) { PUT(0xC0 | (ch >> 6)); PUT(0x80 | (ch & 63)); }
            else { PUT(0xE0 | (ch >> 12)); PUT(0x80 | ((ch >> 6) & 63)); PUT(0x80 | (ch & 63)); }
            continue;
        }
        p += wide ? 2 : 1;
        ch = wide ? LD16(p) : LD8(p);
        if (ch == '%') { PUT('%'); continue; }
        char flags[8] = { 0 };
        int nf = 0, width = 0, prec = -1, len = 0;
        while ((ch == '-' || ch == '0' || ch == '#' || ch == '+' || ch == ' ') && nf < 6) { flags[nf++] = (char)ch; p += wide ? 2 : 1; ch = wide ? LD16(p) : LD8(p); }
        while (ch >= '0' && ch <= '9') { width = width * 10 + (int)(ch - '0'); p += wide ? 2 : 1; ch = wide ? LD16(p) : LD8(p); }
        if (ch == '.') {
            prec = 0;
            p += wide ? 2 : 1; ch = wide ? LD16(p) : LD8(p);
            while (ch >= '0' && ch <= '9') { prec = prec * 10 + (int)(ch - '0'); p += wide ? 2 : 1; ch = wide ? LD16(p) : LD8(p); }
        }
        int narrow = -1;
        for (;;) {
            if (ch == 'l') { narrow = 0; len = len ? 2 : 1; }
            else if (ch == 'h') narrow = 1;
            else if (ch == 'I') {
                uint32_t c1 = wide ? LD16(p + 2) : LD8(p + 1), c2 = wide ? LD16(p + 4) : LD8(p + 2);
                if (c1 == '6' && c2 == '4') { len = 2; p += wide ? 4 : 2; }
                else if (c1 == '3' && c2 == '2') { len = 0; p += wide ? 4 : 2; }
                else len = 2;
            } else break;
            p += wide ? 2 : 1;
            ch = wide ? LD16(p) : LD8(p);
        }
        if (!ch) break;
        char spec[32], tmp[2048];
        int k = snprintf(spec, sizeof spec, "%%%s", flags);
        if (width) k += snprintf(spec + k, sizeof spec - (size_t)k, "%d", width);
        if (prec >= 0 && ch != 's' && ch != 'S') k += snprintf(spec + k, sizeof spec - (size_t)k, ".%d", prec);
        tmp[0] = 0;
        switch (ch) {
        case 'd': case 'i': {
            uint64_t v = va_next(va);
            long long x = len == 2 ? (long long)v : (long long)(int32_t)v;
            snprintf(spec + k, sizeof spec - (size_t)k, "lld");
            snprintf(tmp, sizeof tmp, spec, x);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            uint64_t v = va_next(va);
            unsigned long long x = len == 2 ? v : (uint32_t)v;
            snprintf(spec + k, sizeof spec - (size_t)k, "ll%c", (char)ch);
            snprintf(tmp, sizeof tmp, spec, x);
            break;
        }
        case 'p': snprintf(tmp, sizeof tmp, "%016llX", (unsigned long long)va_next(va)); break;
        case 'c': case 'C': {
            uint32_t v = (uint32_t)va_next(va);
            snprintf(spec + k, sizeof spec - (size_t)k, "c");
            snprintf(tmp, sizeof tmp, spec, (int)(v < 0x80 ? v : '?'));
            break;
        }
        case 's': case 'S': {
            uint64_t s = va_next(va);
            /* %s follows the function's width, %S the other one; h and l force narrow and wide */
            int sw = narrow == 1 ? 0 : narrow == 0 ? 1 : (ch == 's') == (wide != 0);
            char str[1500];
            if (!s) strcpy(str, "(null)");
            else if (sw) rd_w(s, str, sizeof str);
            else rd_a(s, str, sizeof str);
            if (prec >= 0 && (size_t)prec < strlen(str)) str[prec] = 0;
            snprintf(spec + k, sizeof spec - (size_t)k, "s");
            snprintf(tmp, sizeof tmp, spec, str);
            break;
        }
        default: tmp[0] = (char)ch; tmp[1] = 0; break;
        }
        for (char* q = tmp; *q; q++) PUT(*q);
    }
#undef PUT
    out[o] = 0;
    return o;
}

static void do_wsprintf(CPU* c, int wide, uint64_t va)
{
    char s[1100];
    VArgs a = { c, va, 0 };
    gfmt(&a, wide, A2, s, 1025);
    if (wide) { utf8_to_wstr(s, A1, 1025); size_t n = 0; while (LD16(A1 + 2 * n)) n++; RET(n); return; }
    size_t n = strlen(s);
    memcpy(G2H(A1), s, n + 1);
    RET(n);
}
HLE(wsprintfW) { do_wsprintf(c, 1, 0); }
HLE(wsprintfA) { do_wsprintf(c, 0, 0); }
HLE(wvsprintfW) { do_wsprintf(c, 1, A3); }
HLE(wvsprintfA) { do_wsprintf(c, 0, A3); }

/* ---- user32 ---- */

HLE(GetWindowLongA) { hle_GetWindowLongW(c); }
HLE(SetWindowLongA) { hle_SetWindowLongW(c); }
HLE(GetWindowLongPtrA) { hle_GetWindowLongW(c); }
HLE(SetWindowLongPtrA) { hle_SetWindowLongW(c); }
HLE(SystemParametersInfoA) { hle_SystemParametersInfoW(c); }
HLE(CallWindowProcA) { hle_CallWindowProcW(c); }
HLE(LoadImageA) { RET(0x50004); }
HLE(LoadIconA) { RET(0x50003); }
HLE(LoadCursorA) { RET(0x50001); }
HLE(LoadBitmapW) { RET(0x50005); }
HLE(LoadBitmapA) { RET(0x50005); }
HLE(CreateIconFromResourceEx) { RET(0x50006); }
HLE(CreateIconFromResource) { RET(0x50006); }
HLE(LookupIconIdFromDirectoryEx) { RET(1); }
HLE(LookupIconIdFromDirectory) { RET(1); }
HLE(GetIconInfo) { if (A2) memset(G2H(A2), 0, 32); hle_set_error(1402); RET(0); }
HLE(GetIconInfoExW) { hle_set_error(1402); RET(0); }
HLE(SendMessageTimeoutW) { uint64_t out = ARG(7); hle_SendMessageW(c); if (out) ST64(out, R(0)); RET(1); }
HLE(SendMessageTimeoutA) { uint64_t out = ARG(7); hle_SendMessageW(c); if (out) ST64(out, R(0)); RET(1); }
HLE(SendNotifyMessageW) { hle_SendMessageW(c); RET(1); }
HLE(GetWindowTextA) { if (A2 && (int32_t)A3 > 0) ST8(A2, 0); RET(0); }
HLE(GetWindowTextLengthA) { RET(0); }
HLE(GetClassNameA) { put_a(A2, (size_t)(int32_t)A3, "UnrealWindow"); RET(A2 && (int32_t)A3 > 0 ? strlen("UnrealWindow") : 0); }
HLE(GetWindow) { RET(0); }
HLE(GetShellWindow) { RET(0); }
HLE(GetMessageTime) { RET((uint32_t)(host_ticks_ns() / 1000000)); }
HLE(keybd_event) { RET(0); }
HLE(mouse_event) { RET(0); }
static uint32_t clip_formats = 0xC100;
HLE(RegisterClipboardFormatW) { RET(clip_formats++); }
HLE(RegisterClipboardFormatA) { RET(clip_formats++); }
HLE(GetClipboardSequenceNumber) { RET(1); }
HLE(GetRawInputBuffer) { if (A2) ST32(A2, 0); RET(0); }
HLE(AttachThreadInput) { RET(1); }
HLE(SetClassLongPtrW) { RET(0); }
HLE(SetClassLongPtrA) { RET(0); }
HLE(SetClassLongW) { RET(0); }
HLE(GetClassInfoExW) { hle_set_error(1411); RET(0); }
HLE(GetClassInfoExA) { hle_set_error(1411); RET(0); }
HLE(GetClassInfoW) { hle_set_error(1411); RET(0); }
HLE(SetDlgItemTextA) { hle_set_error(1421); RET(0); }
HLE(SendDlgItemMessageW) { RET(0); }
HLE(SendDlgItemMessageA) { RET(0); }
HLE(IsDlgButtonChecked) { RET(0); }
HLE(CheckDlgButton) { RET(1); }
HLE(IsDialogMessageA) { RET(0); }
HLE(DialogBoxParamA) { RET(1); }
HLE(DragDetect) { RET(0); }
HLE(CheckMenuItem) { RET(0xFFFFFFFF); }
HLE(DrawMenuBar) { RET(1); }
HLE(GetMenu) { RET(0); }
HLE(GetUpdateRect) { if (A2) memset(G2H(A2), 0, 16); RET(0); }
HLE(GetWindowDC) { RET(0x70001); }
HLE(GetThreadDesktop) { RET(0x7D000001); }
HLE(OpenInputDesktop) { RET(0x7D000001); }
HLE(CloseDesktop) { RET(1); }
/* UOI_FLAGS: visible (not a service); UOI_NAME: "Default" */
static void user_object_info(CPU* c, int wide)
{
    uint32_t idx = (uint32_t)A2, cap = (uint32_t)A4;
    uint64_t buf = A3, need = ARG(5);
    if (idx == 1) {
        if (need) ST32(need, 12);
        if (!buf || cap < 12) { hle_set_error(122); RET(0); return; }
        ST32(buf, 0); ST32(buf + 4, 0); ST32(buf + 8, 1);
        RET(1);
        return;
    }
    if (idx == 2) {
        const char* n = A1 == 0x80001 ? "WinSta0" : "Default";
        uint32_t bytes = (uint32_t)(strlen(n) + 1) * (wide ? 2 : 1);
        if (need) ST32(need, bytes);
        if (!buf || cap < bytes) { hle_set_error(122); RET(0); return; }
        if (wide) utf8_to_wstr(n, buf, strlen(n) + 1); else put_a(buf, cap, n);
        RET(1);
        return;
    }
    hle_set_error(87);
    RET(0);
}
HLE(GetUserObjectInformationA) { user_object_info(c, 0); }
HLE(GetUserObjectInformationW) { user_object_info(c, 1); }
HLE(GetKeyboardLayoutNameW) { if (A1) utf8_to_wstr("00000409", A1, 9); RET(1); }
HLE(GetKeyboardLayoutNameA) { put_a(A1, 9, "00000409"); RET(1); }
HLE(GetCaretBlinkTime) { RET(530); }
HLE(SetCaretBlinkTime) { RET(1); }
static uint64_t devnotify = 0x7B000000;
HLE(RegisterDeviceNotificationW) { RET(++devnotify); }
HLE(RegisterDeviceNotificationA) { RET(++devnotify); }
HLE(UnregisterDeviceNotification) { RET(1); }

/* window properties */
typedef struct Prop { uint64_t hwnd; char name[64]; uint64_t val; } Prop;
static Prop props[256];
static void prop_name(uint64_t s, int wide, char* out)
{
    if (s < 0x10000) snprintf(out, 64, "#%llu", (unsigned long long)s);
    else if (wide) rd_w(s, out, 64);
    else rd_a(s, out, 64);
}
static Prop* prop_find(uint64_t hwnd, const char* n)
{
    for (int i = 0; i < 256; i++) if (props[i].hwnd == hwnd && !strcmp(props[i].name, n)) return &props[i];
    return NULL;
}
static void set_prop(CPU* c, int wide)
{
    char n[64];
    prop_name(A2, wide, n);
    Prop* p = prop_find(A1, n);
    for (int i = 0; !p && i < 256; i++) if (!props[i].hwnd) { p = &props[i]; p->hwnd = A1; strcpy(p->name, n); }
    if (!p) { RET(0); return; }
    p->val = A3;
    RET(1);
}
static void get_prop(CPU* c, int wide, int remove)
{
    char n[64];
    prop_name(A2, wide, n);
    Prop* p = prop_find(A1, n);
    uint64_t v = p ? p->val : 0;
    if (p && remove) memset(p, 0, sizeof *p);
    RET(v);
}
HLE(SetPropW) { set_prop(c, 1); }
HLE(SetPropA) { set_prop(c, 0); }
HLE(GetPropW) { get_prop(c, 1, 0); }
HLE(GetPropA) { get_prop(c, 0, 0); }
HLE(RemovePropW) { get_prop(c, 1, 1); }
HLE(RemovePropA) { get_prop(c, 0, 1); }

/* ---- gdi32 / msimg32 (nothing is drawn through GDI in a browser build) ---- */

HLE(BitBlt) { RET(1); }
HLE(StretchBlt) { RET(1); }
HLE(PatBlt) { RET(1); }
HLE(AlphaBlend) { RET(1); }
HLE(TransparentBlt) { RET(1); }
HLE(SetStretchBltMode) { RET(1); }
HLE(SelectClipRgn) { RET(2); }
HLE(OffsetRgn) { RET(2); }
HLE(CreateHatchBrush) { RET(0x7A000101); }
HLE(GetObjectA) { if (A3 && A2) memset(G2H(A3), 0, (size_t)(uint32_t)A2); RET(0); }
HLE(GetPixelFormat) { RET(0); }
HLE(DescribePixelFormat) { RET(0); }
HLE(SetDeviceGammaRamp) { RET(0); }
HLE(GetDeviceGammaRamp)
{
    if (!A2) { RET(0); return; }
    for (int ch = 0; ch < 3; ch++) for (int i = 0; i < 256; i++) ST16(A2 + 2ull * (ch * 256 + i), (uint16_t)(i * 257));
    RET(1);
}

/* ---- shell32 / shlwapi / comctl32 / comdlg32 ---- */

HLE(DragAcceptFiles) { RET(0); }
HLE(DragQueryFileW) { if (A3 && A4) ST16(A3, 0); RET(0); }
HLE(DragQueryFileA) { if (A3 && A4) ST8(A3, 0); RET(0); }
HLE(DragQueryPoint) { if (A2) { ST32(A2, 0); ST32(A2 + 4, 0); } RET(0); }
HLE(DragFinish) { RET(0); }
HLE(ExtractIconExW) { RET(0); }
HLE(ExtractIconW) { RET(0); }
HLE(InitCommonControlsEx) { RET(1); }
HLE(InitCommonControls) { RET(0); }
HLE(GetOpenFileNameW) { RET(0); }
HLE(GetOpenFileNameA) { RET(0); }
HLE(GetSaveFileNameW) { RET(0); }
HLE(GetSaveFileNameA) { RET(0); }
HLE(ChooseFontW) { RET(0); }
HLE(ChooseColorW) { RET(0); }

/* SHFILEOPSTRUCTW: wFunc at +8, pFrom (double-NUL list) at +16, fAnyOperationsAborted at +36; delete is carried out */
HLE(SHFileOperationW)
{
    uint64_t op = A1;
    if (!op) { RET(0x57); return; }
    uint32_t fn = LD32(op + 8);
    uint64_t from = LD64(op + 16);
    if (fn == 3 && from) {
        while (LD16(from)) {
            uint64_t save[3] = { R(1) };
            R(1) = from;
            hle_DeleteFileW(c);
            R(1) = from;
            hle_RemoveDirectoryW(c);
            R(1) = save[0];
            while (LD16(from)) from += 2;
            from += 2;
        }
    }
    ST32(op + 36, 0);
    RET(0);
}
HLE(SHFileOperationA) { if (A1) ST32(A1 + 36, 0); RET(0); }

/* "a\.\b\..\c" -> "a\c"; separators normalised to backslashes */
static void canon(const char* in, char* out, size_t cap)
{
    char tmp[1200];
    size_t n = 0;
    snprintf(tmp, sizeof tmp, "%s", in);
    for (char* q = tmp; *q; q++) if (*q == '/') *q = '\\';
    char* parts[256];
    int np = 0;
    char prefix[8] = { 0 };
    char* s = tmp;
    if (s[0] && s[1] == ':') { prefix[0] = s[0]; prefix[1] = ':'; s += 2; }
    int root = *s == '\\';
    if (root && s[1] == '\\') { strcat(prefix, "\\"); s++; }
    for (char* t = strtok(s, "\\"); t && np < 256; t = strtok(NULL, "\\")) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np) np--; continue; }
        parts[np++] = t;
    }
    n = (size_t)snprintf(out, cap, "%s%s", prefix, root ? "\\" : "");
    for (int i = 0; i < np && n < cap; i++) n += (size_t)snprintf(out + n, cap - n, "%s%s", i ? "\\" : "", parts[i]);
    if (!out[0]) snprintf(out, cap, "\\");
}
HLE(PathCanonicalizeW) { char a[1200], b[1200]; rd_w(A2, a, sizeof a); canon(a, b, sizeof b); utf8_to_wstr(b, A1, 260); RET(1); }
HLE(PathCanonicalizeA) { char a[1200], b[1200]; rd_a(A2, a, sizeof a); canon(a, b, sizeof b); put_a(A1, 260, b); RET(1); }
static void combine(const char* dir, const char* file, char* out, size_t cap)
{
    char t[2400];
    if (!file[0]) snprintf(t, sizeof t, "%s", dir);
    else if (file[0] == '\\' || file[0] == '/' || (file[0] && file[1] == ':') || !dir[0]) snprintf(t, sizeof t, "%s", file);
    else {
        size_t n = strlen(dir);
        snprintf(t, sizeof t, "%s%s%s", dir, n && (dir[n - 1] == '\\' || dir[n - 1] == '/') ? "" : "\\", file);
    }
    canon(t, out, cap);
}
HLE(PathCombineW)
{
    char d[1200], f[1200], o[1200];
    rd_w(A2, d, sizeof d); rd_w(A3, f, sizeof f);
    if (!A2 && !A3) { RET(0); return; }
    combine(d, f, o, sizeof o);
    utf8_to_wstr(o, A1, 260);
    RET(A1);
}
HLE(PathCombineA)
{
    char d[1200], f[1200], o[1200];
    rd_a(A2, d, sizeof d); rd_a(A3, f, sizeof f);
    if (!A2 && !A3) { RET(0); return; }
    combine(d, f, o, sizeof o);
    put_a(A1, 260, o);
    RET(A1);
}
HLE(SHDeleteKeyW) { RET(2); }
HLE(SHDeleteKeyA) { RET(2); }

/* ---- kernel32 ---- */

HLE(CancelIo) { RET(1); }
HLE(CancelIoEx) { hle_set_error(1168); RET(0); }
HLE(CancelSynchronousIo) { hle_set_error(1168); RET(0); }
HLE(IsWow64Process) { if (A2) ST32(A2, 0); RET(1); }
HLE(IsWow64Process2) { if (A2) ST16(A2, 0); if (A3) ST16(A3, 0x8664); RET(1); }
HLE(Wow64GetThreadContext) { hle_set_error(50); RET(0); }
HLE(GlobalSize) { RET(A1 ? g_size(A1) : 0); }
HLE(SetThreadExecutionState) { RET(0x80000000u); }
HLE(SetProcessShutdownParameters) { RET(1); }
HLE(RtlAddFunctionTable) { RET(1); }
HLE(RtlDeleteFunctionTable) { RET(1); }
HLE(RtlInstallFunctionTableCallback) { RET(1); }
HLE(VirtualLock) { RET(1); }
HLE(CopyFileExW) { R(8) = (uint32_t)ARG(6) & 1; hle_CopyFileW(c); }
HLE(CopyFileA)
{
    uint64_t a = A1, b = A2, wa = a2w(a), wb = a2w(b);
    R(1) = wa; R(2) = wb;
    hle_CopyFileW(c);
    g_free(wa); g_free(wb);
}
HLE(MoveFileA)
{
    uint64_t wa = a2w(A1), wb = a2w(A2);
    R(1) = wa; R(2) = wb;
    hle_MoveFileW(c);
    g_free(wa); g_free(wb);
}
HLE(SetFileAttributesA) { RET(1); }
HLE(EnumResourceNamesA) { hle_set_error(1813); RET(0); }
HLE(EnumResourceNamesW) { hle_set_error(1813); RET(0); }
HLE(EnumResourceNamesExW) { hle_set_error(1813); RET(0); }
HLE(ReadDirectoryChangesW) { hle_set_error(50); RET(0); }
HLE(ReadDirectoryChangesExW) { hle_set_error(50); RET(0); }
HLE(OpenFileMappingW) { hle_set_error(2); RET(0); }
HLE(OpenFileMappingA) { hle_set_error(2); RET(0); }
HLE(WaitNamedPipeA) { hle_set_error(2); RET(0); }
HLE(FileTimeToDosDateTime)
{
    if (!A1) { RET(0); return; }
    uint64_t ft = LD64(A1);
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm* g = gmtime(&t);
    if (!g || g->tm_year < 80 || g->tm_year > 207) { hle_set_error(87); RET(0); return; }
    if (A2) ST16(A2, (uint16_t)(((g->tm_year - 80) << 9) | ((g->tm_mon + 1) << 5) | g->tm_mday));
    if (A3) ST16(A3, (uint16_t)((g->tm_hour << 11) | (g->tm_min << 5) | (g->tm_sec / 2)));
    RET(1);
}
HLE(DosDateTimeToFileTime)
{
    uint32_t d = (uint16_t)A1, t = (uint16_t)A2;
    int64_t y = (int64_t)(d >> 9) + 1980, m = (d >> 5) & 15, dd = d & 31;
    /* days since 1970-01-01 (proleptic Gregorian) */
    y -= m <= 2;
    int64_t era = y / 400, yoe = y - era * 400, mp = (m + 9) % 12, doy = (153 * mp + 2) / 5 + dd - 1;
    int64_t days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    int64_t secs = days * 86400 + (int64_t)(t >> 11) * 3600 + (int64_t)((t >> 5) & 63) * 60 + (int64_t)(t & 31) * 2;
    if (A3) ST64(A3, (uint64_t)(secs + 11644473600ll) * 10000000ull);
    RET(1);
}
HLE(FillConsoleOutputCharacterW) { if (ARG(5)) ST32(ARG(5), (uint32_t)A3); RET(1); }
HLE(FillConsoleOutputCharacterA) { if (ARG(5)) ST32(ARG(5), (uint32_t)A3); RET(1); }
HLE(FillConsoleOutputAttribute) { if (ARG(5)) ST32(ARG(5), (uint32_t)A3); RET(1); }
HLE(SetConsoleCursorPosition) { RET(1); }
HLE(ReadConsoleInputA) { if (A4) ST32(A4, 0); hle_set_error(6); RET(0); }

/* psapi: the process is the caller itself */
HLE(GetModuleFileNameExW) { R(1) = A2; R(2) = A3; R(8) = A4; hle_GetModuleFileNameW(c); }
HLE(GetModuleFileNameExA) { R(1) = A2; R(2) = A3; R(8) = A4; hle_GetModuleFileNameA(c); }
static void base_name(CPU* c, int wide)
{
    uint64_t buf = A3;
    uint32_t cap = (uint32_t)A4;
    R(1) = A2; R(2) = buf; R(8) = cap;
    if (wide) hle_GetModuleFileNameW(c); else hle_GetModuleFileNameA(c);
    if (!buf || !cap) return;
    char s[600];
    if (wide) rd_w(buf, s, sizeof s); else rd_a(buf, s, sizeof s);
    const char* b = strrchr(s, '\\');
    b = b ? b + 1 : s;
    if (wide) utf8_to_wstr(b, buf, cap); else put_a(buf, cap, b);
    RET(strlen(b) < cap ? strlen(b) : cap);
}
HLE(GetModuleBaseNameW) { base_name(c, 1); }
HLE(GetModuleBaseNameA) { base_name(c, 0); }
HLE(K32GetModuleBaseNameW) { base_name(c, 1); }
HLE(K32GetModuleBaseNameA) { base_name(c, 0); }

/* ---- advapi32 ---- */

HLE(GetTokenInformation) { if (ARG(5)) ST32(ARG(5), 0); hle_set_error(6); RET(0); }
/* SID: revision, sub-authority count, 6-byte authority, then 32-bit sub-authorities */
HLE(AllocateAndInitializeSid)
{
    uint32_t n = (uint8_t)A2;
    uint64_t out = ARG(11);
    if (n > 8 || !out || !A1) { hle_set_error(87); RET(0); return; }
    uint64_t s = g_calloc(8 + 4 * 8);
    ST8(s, 1); ST8(s + 1, (uint8_t)n);
    memcpy(G2H(s + 2), G2H(A1), 6);
    uint64_t sub[8] = { A3, A4, ARG(5), ARG(6), ARG(7), ARG(8), ARG(9), ARG(10) };
    for (uint32_t i = 0; i < n; i++) ST32(s + 8 + 4 * i, (uint32_t)sub[i]);
    ST64(out, s);
    RET(1);
}
HLE(FreeSid) { if (A1) g_free(A1); RET(0); }
HLE(GetSidSubAuthority) { RET(A1 + 8 + 4ull * (uint32_t)A2); }
HLE(GetSidSubAuthorityCount) { RET(A1 + 1); }
HLE(GetLengthSid) { RET(A1 ? 8 + 4ull * LD8(A1 + 1) : 0); }
HLE(IsValidSid) { RET(A1 && LD8(A1) == 1 && LD8(A1 + 1) <= 15); }
HLE(EqualSid) { RET(A1 && A2 && LD8(A1 + 1) == LD8(A2 + 1) && !memcmp(G2H(A1), G2H(A2), 8 + 4 * LD8(A1 + 1))); }
HLE(CheckTokenMembership) { if (A3) ST32(A3, 0); RET(1); }
HLE(RegSetValueExA) { RET(0); }
HLE(RegCreateKeyW) { if (A3) ST64(A3, 0x80000100); RET(0); }
HLE(RegCreateKeyA) { if (A3) ST64(A3, 0x80000100); RET(0); }
HLE(RegCreateKeyExA) { if (ARG(8)) ST64(ARG(8), 0x80000100); if (ARG(9)) ST32(ARG(9), 1); RET(0); }
HLE(RegOpenKeyA) { if (A3) ST64(A3, 0); RET(2); }
HLE(RegOpenKeyW) { if (A3) ST64(A3, 0); RET(2); }
HLE(RegDeleteValueA) { RET(2); }
HLE(RegDeleteKeyExW) { RET(2); }
HLE(RegDeleteKeyExA) { RET(2); }
HLE(CryptEncrypt) { hle_set_error(0x80090003); RET(0); }
HLE(CryptImportKey) { hle_set_error(0x80090003); RET(0); }
HLE(GetNamedSecurityInfoW) { RET(50); }
HLE(SetNamedSecurityInfoW) { RET(50); }
HLE(SetEntriesInAclW) { if (ARG(4)) {} RET(50); }
HLE(ImpersonateNamedPipeClient) { hle_set_error(50); RET(0); }

/* ---- ole32 / combase / oleaut32 ---- */

HLE(CoSetProxyBlanket) { RET(0); }
HLE(GetErrorInfo) { if (A2) ST64(A2, 0); RET(1); }
HLE(SetErrorInfo) { RET(0); }
HLE(CreateErrorInfo) { if (A1) ST64(A1, 0); RET(0x8007000E); }
HLE(RoGetAgileReference) { if (A4) ST64(A4, 0); RET(0x80004001); }
HLE(RoActivateInstance) { if (A2) ST64(A2, 0); RET(0x80040154); }
HLE(WindowsCompareStringOrdinal)
{
    uint32_t na = A1 ? LD32(A1 + 4) : 0, nb = A2 ? LD32(A2 + 4) : 0;
    uint64_t pa = A1 ? LD64(A1 + 8) : 0, pb = A2 ? LD64(A2 + 8) : 0;
    int r = 0;
    for (uint32_t i = 0; !r && i < na && i < nb; i++) { uint16_t x = LD16(pa + 2ull * i), y = LD16(pb + 2ull * i); r = x < y ? -1 : x > y; }
    if (!r) r = na < nb ? -1 : na > nb;
    if (A3) ST32(A3, (uint32_t)r);
    RET(0);
}

/* SAFEARRAY: cDims u16, fFeatures u16, cbElements u32, cLocks u32, pvData at +16, bounds {count u32, lbound i32} from
   +24 (the last dimension first, as Windows stores them); the VARTYPE sits 4 bytes before the header */
static uint32_t vt_size(uint32_t vt)
{
    switch (vt & 0xFFF) {
    case 16: case 17: return 1;                    /* I1 UI1 */
    case 2: case 18: case 11: return 2;            /* I2 UI2 BOOL */
    case 3: case 19: case 4: case 10: case 22: case 23: return 4;   /* I4 UI4 R4 ERROR INT UINT */
    case 5: case 6: case 7: case 20: case 21: return 8;             /* R8 CY DATE I8 UI8 */
    case 8: case 9: case 13: return 8;             /* BSTR DISPATCH UNKNOWN */
    case 12: return 24;                            /* VARIANT */
    case 14: return 16;                            /* DECIMAL */
    default: return 0;
    }
}
static uint64_t sa_new(uint32_t vt, uint32_t dims, const uint32_t* count, const int32_t* lb)
{
    uint32_t es = vt_size(vt);
    if (!es || !dims || dims > 16) return 0;
    uint64_t total = 1;
    for (uint32_t i = 0; i < dims; i++) total *= count[i];
    uint64_t base = g_calloc(8 + 24 + 8ull * dims);
    uint64_t sa = base + 8;
    ST32(sa - 4, vt);
    uint16_t feat = 0x80;                          /* FADF_HAVEVARTYPE */
    if ((vt & 0xFFF) == 8) feat |= 0x100;
    if ((vt & 0xFFF) == 13) feat |= 0x200;
    if ((vt & 0xFFF) == 9) feat |= 0x400;
    if ((vt & 0xFFF) == 12) feat |= 0x800;
    ST16(sa, (uint16_t)dims);
    ST16(sa + 2, feat);
    ST32(sa + 4, es);
    ST64(sa + 16, total ? g_calloc(total * es) : 0);
    for (uint32_t i = 0; i < dims; i++) { ST32(sa + 24 + 8ull * (dims - 1 - i), count[i]); ST32(sa + 28 + 8ull * (dims - 1 - i), (uint32_t)lb[i]); }
    return sa;
}
/* bounds of dimension d (1-based, as the API counts) */
static uint32_t sa_count(uint64_t sa, uint32_t d) { uint32_t n = LD16(sa); return LD32(sa + 24 + 8ull * (n - d)); }
static int32_t sa_lb(uint64_t sa, uint32_t d) { uint32_t n = LD16(sa); return (int32_t)LD32(sa + 28 + 8ull * (n - d)); }
static int64_t sa_index(uint64_t sa, uint64_t idx)
{
    uint32_t n = LD16(sa);
    int64_t off = 0, mul = 1;
    for (uint32_t d = 1; d <= n; d++) {
        int32_t i = (int32_t)LD32(idx + 4ull * (d - 1)) - sa_lb(sa, d);
        if (i < 0 || (uint32_t)i >= sa_count(sa, d)) return -1;
        off += i * mul;
        mul *= sa_count(sa, d);
    }
    return off;
}
HLE(SafeArrayCreateVector) { uint32_t n = (uint32_t)A3; int32_t lb = (int32_t)A2; RET(sa_new((uint32_t)A1, 1, &n, &lb)); }
HLE(SafeArrayCreate)
{
    uint32_t dims = (uint32_t)A2, cnt[16];
    int32_t lb[16];
    if (!dims || dims > 16 || !A3) { RET(0); return; }
    for (uint32_t i = 0; i < dims; i++) { cnt[i] = LD32(A3 + 8ull * i); lb[i] = (int32_t)LD32(A3 + 8ull * i + 4); }
    RET(sa_new((uint32_t)A1, dims, cnt, lb));
}
HLE(SafeArrayDestroy)
{
    uint64_t sa = A1;
    if (!sa) { RET(0); return; }
    if (LD32(sa + 8)) { RET(0x8002000D); return; }     /* DISP_E_ARRAYISLOCKED */
    uint64_t data = LD64(sa + 16);
    uint16_t feat = LD16(sa + 2);
    if (data && (feat & 0x100)) {
        uint64_t total = 1;
        for (uint32_t d = 1; d <= LD16(sa); d++) total *= sa_count(sa, d);
        for (uint64_t i = 0; i < total; i++) { uint64_t b = LD64(data + 8 * i); if (b) g_free(b - 4); }
    }
    if (data) g_free(data);
    g_free(sa - 8);
    RET(0);
}
HLE(SafeArrayDestroyData) { RET(0); }
HLE(SafeArrayAccessData) { if (!A1 || !A2) { RET(0x80070057); return; } ST32(A1 + 8, LD32(A1 + 8) + 1); ST64(A2, LD64(A1 + 16)); RET(0); }
HLE(SafeArrayUnaccessData) { if (!A1) { RET(0x80070057); return; } if (LD32(A1 + 8)) ST32(A1 + 8, LD32(A1 + 8) - 1); RET(0); }
HLE(SafeArrayLock) { if (!A1) { RET(0x80070057); return; } ST32(A1 + 8, LD32(A1 + 8) + 1); RET(0); }
HLE(SafeArrayUnlock) { if (!A1) { RET(0x80070057); return; } if (LD32(A1 + 8)) ST32(A1 + 8, LD32(A1 + 8) - 1); RET(0); }
HLE(SafeArrayGetDim) { RET(A1 ? LD16(A1) : 0); }
HLE(SafeArrayGetElemsize) { RET(A1 ? LD32(A1 + 4) : 0); }
HLE(SafeArrayGetVartype) { if (!A1 || !A2) { RET(0x80070057); return; } ST16(A2, (LD16(A1 + 2) & 0x80) ? (uint16_t)LD32(A1 - 4) : 0); RET(0); }
HLE(SafeArrayGetLBound)
{
    uint32_t d = (uint32_t)A2;
    if (!A1 || !A3) { RET(0x80070057); return; }
    if (!d || d > LD16(A1)) { RET(0x8002000B); return; }    /* DISP_E_BADINDEX */
    ST32(A3, (uint32_t)sa_lb(A1, d));
    RET(0);
}
HLE(SafeArrayGetUBound)
{
    uint32_t d = (uint32_t)A2;
    if (!A1 || !A3) { RET(0x80070057); return; }
    if (!d || d > LD16(A1)) { RET(0x8002000B); return; }
    ST32(A3, (uint32_t)(sa_lb(A1, d) + (int32_t)sa_count(A1, d) - 1));
    RET(0);
}
HLE(SafeArrayPtrOfIndex)
{
    int64_t i = A1 && A2 ? sa_index(A1, A2) : -1;
    if (i < 0 || !A3) { RET(0x8002000B); return; }
    ST64(A3, LD64(A1 + 16) + (uint64_t)i * LD32(A1 + 4));
    RET(0);
}
static uint64_t bstr_dup(uint64_t b)
{
    if (!b) return 0;
    uint32_t n = LD32(b - 4);
    uint64_t p = g_alloc(n + 6);
    memcpy(G2H(p), G2H(b - 4), n + 6);
    return p + 4;
}
HLE(SafeArrayPutElement)
{
    int64_t i = A1 && A2 ? sa_index(A1, A2) : -1;
    if (i < 0) { RET(0x8002000B); return; }
    uint32_t es = LD32(A1 + 4);
    uint64_t dst = LD64(A1 + 16) + (uint64_t)i * es;
    uint16_t feat = LD16(A1 + 2);
    if (feat & 0x100) { uint64_t old = LD64(dst); if (old) g_free(old - 4); ST64(dst, bstr_dup(A3)); }
    else if (feat & 0x600) ST64(dst, A3);
    else if (A3) memcpy(G2H(dst), G2H(A3), es);
    RET(0);
}
HLE(SafeArrayGetElement)
{
    int64_t i = A1 && A2 ? sa_index(A1, A2) : -1;
    if (i < 0 || !A3) { RET(0x8002000B); return; }
    uint32_t es = LD32(A1 + 4);
    uint64_t src = LD64(A1 + 16) + (uint64_t)i * es;
    if (LD16(A1 + 2) & 0x100) ST64(A3, bstr_dup(LD64(src)));
    else memcpy(G2H(A3), G2H(src), es);
    RET(0);
}
HLE(SafeArrayCopy)
{
    uint64_t sa = A1;
    if (!sa || !A2) { RET(0x80070057); return; }
    uint32_t n = LD16(sa), cnt[16];
    int32_t lb[16];
    for (uint32_t d = 1; d <= n && d <= 16; d++) { cnt[d - 1] = sa_count(sa, d); lb[d - 1] = sa_lb(sa, d); }
    uint32_t vt = (LD16(sa + 2) & 0x80) ? LD32(sa - 4) : 17;
    uint64_t out = sa_new(vt, n, cnt, lb);
    if (!out) { RET(0x8007000E); return; }
    uint64_t total = 1;
    for (uint32_t d = 0; d < n; d++) total *= cnt[d];
    uint32_t es = LD32(sa + 4);
    if (LD16(sa + 2) & 0x100) for (uint64_t i = 0; i < total; i++) ST64(LD64(out + 16) + 8 * i, bstr_dup(LD64(LD64(sa + 16) + 8 * i)));
    else if (total) memcpy(G2H(LD64(out + 16)), G2H(LD64(sa + 16)), total * es);
    ST64(A2, out);
    RET(0);
}
HLE(SafeArrayAllocDescriptor) { RET(0x80004001); }

/* VARIANT: vt u16 at +0, value at +8 */
HLE(VariantCopy)
{
    uint64_t d = A1, s = A2;
    if (!d || !s) { RET(0x80070057); return; }
    if (d == s) { RET(0); return; }
    if (LD16(d) == 8 && LD64(d + 8)) g_free(LD64(d + 8) - 4);
    memcpy(G2H(d), G2H(s), 24);
    if (LD16(s) == 8) ST64(d + 8, bstr_dup(LD64(s + 8)));
    RET(0);
}
HLE(VariantCopyInd) { R(1) = A1; R(2) = A2; hle_VariantCopy(c); }
static int vt_num(uint16_t vt, uint64_t v, double* out)
{
    switch (vt) {
    case 16: *out = (int8_t)v; return 1;
    case 17: *out = (uint8_t)v; return 1;
    case 2: *out = (int16_t)v; return 1;
    case 18: *out = (uint16_t)v; return 1;
    case 3: case 22: *out = (int32_t)v; return 1;
    case 19: case 23: *out = (uint32_t)v; return 1;
    case 20: *out = (double)(int64_t)v; return 1;
    case 21: *out = (double)v; return 1;
    case 11: *out = (int16_t)v ? -1 : 0; return 1;
    case 4: { float f; uint32_t u = (uint32_t)v; memcpy(&f, &u, 4); *out = f; return 1; }
    case 5: { double f; memcpy(&f, &v, 8); *out = f; return 1; }
    default: return 0;
    }
}
/* numeric types and strings convert among each other; anything else is a type mismatch */
static void change_type(CPU* c, uint64_t d, uint64_t s, uint16_t to)
{
    if (!d || !s) { RET(0x80070057); return; }
    uint16_t from = LD16(s);
    uint64_t v = LD64(s + 8);
    if (from == to) { R(1) = d; R(2) = s; hle_VariantCopy(c); return; }
    double x = 0;
    int ok = vt_num(from, v, &x);
    if (!ok && from == 8) {
        char t[256];
        rd_w(v, t, sizeof t);
        char* e;
        x = strtod(t, &e);
        ok = e != t;
        if (!ok && to != 8) { RET(0x80020005); return; }
    }
    if (!ok && from != 0) { RET(0x80020005); return; }
    uint8_t res[24] = { 0 };
    *(uint16_t*)res = to;
    uint64_t out = 0;
    switch (to) {
    case 16: case 17: out = (uint8_t)(int64_t)x; break;
    case 2: case 18: out = (uint16_t)(int64_t)x; break;
    case 3: case 19: case 22: case 23: out = (uint32_t)(int64_t)x; break;
    case 20: case 21: out = (uint64_t)(int64_t)x; break;
    case 11: out = x != 0 ? 0xFFFF : 0; break;
    case 4: { float f = (float)x; uint32_t u; memcpy(&u, &f, 4); out = u; break; }
    case 5: memcpy(&out, &x, 8); break;
    case 8: {
        char t[64];
        snprintf(t, sizeof t, "%.15g", x);
        size_t n = strlen(t);
        uint64_t b = g_alloc(2 * n + 6);
        ST32(b, (uint32_t)(2 * n));
        utf8_to_wstr(t, b + 4, n + 1);
        out = b + 4;
        break;
    }
    default: RET(0x80020005); return;
    }
    memcpy(res + 8, &out, 8);
    if (d != s && LD16(d) == 8 && LD64(d + 8)) g_free(LD64(d + 8) - 4);
    if (d == s && from == 8 && v) g_free(v - 4);
    memcpy(G2H(d), res, 24);
    RET(0);
}
HLE(VariantChangeType) { change_type(c, A1, A2, (uint16_t)A4); }
HLE(VariantChangeTypeEx) { change_type(c, A1, A2, (uint16_t)ARG(5)); }

/* ---- rpcrt4 ---- */

HLE(UuidCreate)
{
    if (!A1) { RET(1717); return; }
    for (int i = 0; i < 16; i++) ST8(A1 + i, (uint8_t)(rand() >> 7));
    ST8(A1 + 7, (uint8_t)((LD8(A1 + 7) & 0x0F) | 0x40));
    ST8(A1 + 8, (uint8_t)((LD8(A1 + 8) & 0x3F) | 0x80));
    RET(0);
}
HLE(UuidCreateSequential) { hle_UuidCreate(c); }
HLE(UuidCreateNil) { if (A1) memset(G2H(A1), 0, 16); RET(0); }
static void uuid_str(uint64_t u, char* s)
{
    snprintf(s, 40, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned)LD32(u), (unsigned)LD16(u + 4), (unsigned)LD16(u + 6), (unsigned)LD8(u + 8), (unsigned)LD8(u + 9),
             (unsigned)LD8(u + 10), (unsigned)LD8(u + 11), (unsigned)LD8(u + 12), (unsigned)LD8(u + 13), (unsigned)LD8(u + 14), (unsigned)LD8(u + 15));
}
HLE(UuidToStringW) { char s[40]; if (!A1 || !A2) { RET(1717); return; } uuid_str(A1, s); uint64_t w = g_alloc(80); utf8_to_wstr(s, w, 37); ST64(A2, w); RET(0); }
HLE(UuidToStringA) { char s[40]; if (!A1 || !A2) { RET(1717); return; } uuid_str(A1, s); uint64_t a = g_alloc(40); memcpy(G2H(a), s, 37); ST64(A2, a); RET(0); }
HLE(RpcStringFreeW) { if (A1 && LD64(A1)) { g_free(LD64(A1)); ST64(A1, 0); } RET(0); }
HLE(RpcStringFreeA) { if (A1 && LD64(A1)) { g_free(LD64(A1)); ST64(A1, 0); } RET(0); }

/* ---- crypt32: base64 decoding (certificates and keys the game embeds); the rest has no store to work on ---- */

static int b64v(int ch) { return ch >= 'A' && ch <= 'Z' ? ch - 'A' : ch >= 'a' && ch <= 'z' ? ch - 'a' + 26 : ch >= '0' && ch <= '9' ? ch - '0' + 52 : ch == '+' ? 62 : ch == '/' ? 63 : -1; }
static void str_to_bin(CPU* c, int wide)
{
    uint64_t s = A1;
    uint32_t n = (uint32_t)A2, flags = (uint32_t)A3;
    uint64_t out = A4, outlen = ARG(5);
    if (!s || !outlen) { hle_set_error(87); RET(0); return; }
    if (!n) while (wide ? LD16(s + 2ull * n) : LD8(s + n)) n++;
    uint8_t* tmp = malloc(n + 1);
    uint32_t m = 0, acc = 0, bits = 0;
    int in_header = 0;
    for (uint32_t i = 0; i < n; i++) {
        int ch = wide ? LD16(s + 2ull * i) : LD8(s + i);
        if (flags == 2) { tmp[m++] = (uint8_t)ch; continue; }        /* CRYPT_STRING_BINARY */
        if (ch == '-') { in_header = !in_header; while (i + 1 < n && (wide ? LD16(s + 2ull * (i + 1)) : LD8(s + i + 1)) == '-') i++; continue; }
        if (in_header) continue;
        int v = b64v(ch);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) { bits -= 8; tmp[m++] = (uint8_t)(acc >> bits); }
    }
    uint32_t cap = LD32(outlen);
    ST32(outlen, m);
    if (out) {
        if (cap < m) { free(tmp); hle_set_error(234); RET(0); return; }
        memcpy(G2H(out), tmp, m);
    }
    if (ARG(6)) ST32(ARG(6), 0);
    if (ARG(7)) ST32(ARG(7), flags == 2 ? 2 : 1);
    free(tmp);
    RET(1);
}
HLE(CryptStringToBinaryW) { str_to_bin(c, 1); }
HLE(CryptStringToBinaryA) { str_to_bin(c, 0); }
HLE(CryptDecodeObjectEx) { hle_set_error(0x80092004); RET(0); }
HLE(CertAddEncodedCertificateToStore) { if (ARG(6)) ST64(ARG(6), 0); RET(1); }
HLE(CertFindExtension) { RET(0); }
HLE(PFXImportCertStore) { hle_set_error(0x80092004); RET(0); }

/* ---- ws2_32 ---- */

HLE(WSARecvFrom) { hle_set_error(10045); RET((uint64_t)-1); }
HLE(WSASendTo) { hle_set_error(10045); RET((uint64_t)-1); }
HLE(getservbyport) { RET(0); }
HLE(WSAAsyncGetHostByName) { hle_set_error(10045); RET(0); }
HLE(WSACancelAsyncRequest) { hle_set_error(10038); RET((uint64_t)-1); }

/* ---- wldap32 (libcurl's LDAP support): there is no directory server ---- */

HLE(ldap_init) { hle_set_error(0x51); RET(0); }
HLE(ldap_initA) { hle_set_error(0x51); RET(0); }
HLE(ldap_initW) { hle_set_error(0x51); RET(0); }
HLE(ldap_sslinit) { hle_set_error(0x51); RET(0); }
HLE(ldap_set_option) { RET(0x59); }
HLE(ldap_simple_bind_s) { RET(0x51); }
HLE(ldap_search_s) { if (ARG(7)) ST64(ARG(7), 0); RET(0x51); }
HLE(ldap_first_entry) { RET(0); }
HLE(ldap_next_entry) { RET(0); }
HLE(ldap_first_attribute) { if (A3) ST64(A3, 0); RET(0); }
HLE(ldap_next_attribute) { RET(0); }
HLE(ldap_get_values_len) { RET(0); }
HLE(ldap_value_free_len) { RET(0); }
HLE(ldap_get_dn) { RET(0); }
HLE(ldap_memfree) { RET(0); }
HLE(ldap_msgfree) { RET(0); }
HLE(ldap_unbind_s) { RET(0); }
HLE(ldap_unbind) { RET(0); }
HLE(ber_free) { RET(0); }
HLE(LdapGetLastError) { RET(0x51); }
HLE(ldap_err2string)
{
    static uint64_t s;
    if (!s) { s = g_alloc(16); memcpy(G2H(s), "Server Down", 12); }
    RET(s);
}

/* ---- winmm: no capture devices ---- */

HLE(waveInGetNumDevs) { RET(0); }
HLE(waveInOpen) { if (A1) ST64(A1, 0); RET(2); }
HLE(waveInGetDevCapsW) { RET(2); }
HLE(waveInGetDevCapsA) { RET(2); }
HLE(waveInStart) { RET(5); }
HLE(waveInStop) { RET(5); }
HLE(waveInReset) { RET(5); }
HLE(waveInClose) { RET(5); }
HLE(waveInAddBuffer) { RET(5); }
HLE(waveInPrepareHeader) { RET(5); }
HLE(waveInUnprepareHeader) { RET(5); }
HLE(waveInGetPosition) { RET(5); }
/* WAVEOUTCAPSA: ids, version, 32-char name, formats, channels, support */
HLE(waveOutGetDevCapsA)
{
    uint32_t dev = (uint32_t)A1;
    if (dev != 0 && dev != 0xFFFFFFFF) { RET(2); return; }
    if (!A2 || (uint32_t)A3 < 52) { RET(11); return; }
    memset(G2H(A2), 0, 52);
    ST16(A2, 1); ST16(A2 + 2, 100); ST32(A2 + 4, 0x0100);
    memcpy(G2H(A2 + 8), "Speakers", 9);
    ST32(A2 + 40, 0xFFFFF); ST16(A2 + 44, 2); ST32(A2 + 48, 0x2C);
    RET(0);
}

/* ---- hid: no devices are ever opened, so every query fails ---- */

HLE(HidD_GetPreparsedData) { if (A2) ST64(A2, 0); RET(0); }
HLE(HidD_FreePreparsedData) { RET(1); }
HLE(HidD_GetManufacturerString) { RET(0); }
HLE(HidD_GetProductString) { RET(0); }
HLE(HidD_GetSerialNumberString) { RET(0); }
HLE(HidD_GetIndexedString) { RET(0); }
HLE(HidD_SetFeature) { RET(0); }
HLE(HidD_GetInputReport) { RET(0); }
HLE(HidD_SetOutputReport) { RET(0); }
HLE(HidD_SetNumInputBuffers) { RET(0); }
HLE(HidP_GetCaps) { RET(0xC0110001); }
HLE(HidP_GetValueCaps) { RET(0xC0110001); }
HLE(HidP_GetButtonCaps) { RET(0xC0110001); }
HLE(HidP_GetData) { RET(0xC0110001); }
HLE(HidP_SetUsages) { RET(0xC0110001); }
HLE(HidP_SetUsageValue) { RET(0xC0110001); }
HLE(HidP_GetUsageValue) { RET(0xC0110001); }
HLE(HidP_GetUsages) { RET(0xC0110001); }
HLE(HidP_MaxDataListLength) { RET(0); }
HLE(HidP_MaxUsageListLength) { RET(0); }

/* ---- Media Foundation: starts, but cannot build pipelines (games fall back to their own decoders) ---- */

HLE(MFStartup) { RET(0); }
HLE(MFShutdown) { RET(0); }
HLE(MFCreateMediaType) { if (A1) ST64(A1, 0); RET(0x80004001); }
HLE(MFCreateSourceResolver) { if (A1) ST64(A1, 0); RET(0x80004001); }
HLE(MFCreateAttributes) { if (A1) ST64(A1, 0); RET(0x80004001); }
HLE(MFCreateSample) { if (A1) ST64(A1, 0); RET(0x80004001); }
HLE(MFCreateTopology) { if (A1) ST64(A1, 0); RET(0x80004001); }
HLE(MFCreateMemoryBuffer) { if (A2) ST64(A2, 0); RET(0x80004001); }
HLE(MFCreateMediaSession) { if (A2) ST64(A2, 0); RET(0x80004001); }
HLE(MFCreateTopologyNode) { if (A2) ST64(A2, 0); RET(0x80004001); }
HLE(MFCreateSampleGrabberSinkActivate) { if (A3) ST64(A3, 0); RET(0x80004001); }
HLE(MFCreateSourceReaderFromByteStream) { if (A3) ST64(A3, 0); RET(0x80004001); }
HLE(MFCreateSourceReaderFromURL) { if (A3) ST64(A3, 0); RET(0x80004001); }
HLE(MFGetService) { if (A4) ST64(A4, 0); RET(0x80004001); }

/* ---- dwmapi / imm32 / dbghelp / misc ---- */

HLE(DwmGetWindowAttribute) { if (A3 && A4) memset(G2H(A3), 0, (size_t)(uint32_t)A4); RET(0x80070057); }
HLE(DwmExtendFrameIntoClientArea) { RET(0); }
HLE(DwmEnableBlurBehindWindow) { RET(0); }
HLE(ImmSetCompositionStringW) { RET(0); }
HLE(ImmAssociateContextEx) { RET(1); }
HLE(ImmGetConversionStatus) { if (A2) ST32(A2, 0); if (A3) ST32(A3, 0); RET(1); }
HLE(ImmSetConversionStatus) { RET(1); }
HLE(ImmGetIMEFileNameA) { RET(0); }
HLE(ImmGetCandidateListW) { RET(0); }
HLE(SymRegisterFunctionEntryCallback64) { RET(1); }
HLE(SymRegisterCallbackW64) { RET(1); }
HLE(SymRegisterCallback64) { RET(1); }
HLE(BCryptDeriveKeyPBKDF2) { RET(0xC00000BB); }
HLE(D3D11On12CreateDevice) { if (ARG(8)) ST64(ARG(8), 0); RET(0x80004001); }
