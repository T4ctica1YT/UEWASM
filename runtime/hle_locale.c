#include "host.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wctype.h>

static int is_utf8(uint32_t cp) { return cp == 65001 || cp <= 3 || cp == 42; }

HLE(GetACP) { RET(65001); }
HLE(GetOEMCP) { RET(65001); }
HLE(IsValidCodePage) { uint32_t cp = (uint32_t)A1; RET(cp == 65001 || cp == 1252 || cp == 437 || cp == 20127 || cp == 28591); }
HLE(GetCPInfo)
{
    memset(G2H(A2), 0, 20);
    ST32(A2, is_utf8((uint32_t)A1) ? 4 : 1);
    ST8(A2 + 4, '?');
    RET(1);
}
HLE(GetCPInfoExW)
{
    memset(G2H(A3), 0, 544);
    ST32(A3, is_utf8((uint32_t)A1) ? 4 : 1);
    ST8(A3 + 4, '?');
    ST16(A3 + 18, '?');
    ST32(A3 + 20, (uint32_t)A1);
    RET(1);
}

static uint32_t utf8_next(const uint8_t** pp, const uint8_t* end)
{
    const uint8_t* p = *pp;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) c &= 0x3F >> extra;
    while (extra-- && p < end) c = (c << 6) | (*p++ & 0x3F);
    *pp = p;
    return c;
}

HLE(MultiByteToWideChar)
{
    uint32_t cp = (uint32_t)A1;
    const uint8_t* s = (const uint8_t*)G2H(A3);
    int64_t len = (int32_t)A4;
    uint64_t dst = ARG(5);
    int64_t cap = (int32_t)ARG(6);
    if (len < 0) len = (int64_t)strlen((const char*)s) + 1;
    const uint8_t* end = s + len;
    int64_t n = 0;
    while (s < end) {
        uint32_t ch = is_utf8(cp) ? utf8_next(&s, end) : *s++;
        int units = ch >= 0x10000 ? 2 : 1;
        if (cap) {
            if (n + units > cap) { ST32(g_teb + 0x68, 122); RET(0); return; }
            if (units == 2) { ch -= 0x10000; ST16(dst + 2 * n, 0xD800 + (ch >> 10)); ST16(dst + 2 * n + 2, 0xDC00 + (ch & 1023)); }
            else ST16(dst + 2 * n, ch);
        }
        n += units;
    }
    RET(n);
}

HLE(WideCharToMultiByte)
{
    uint32_t cp = (uint32_t)A1;
    uint64_t src = A3;
    int64_t len = (int32_t)A4;
    uint64_t dst = ARG(5);
    int64_t cap = (int32_t)ARG(6);
    uint64_t used_def = ARG(8);
    if (len < 0) { len = 0; while (LD16(src + 2 * len)) len++; len++; }
    int64_t n = 0;
    uint8_t buf[4];
    for (int64_t i = 0; i < len; i++) {
        uint32_t w = (uint32_t)LD16(src + 2 * i);
        if (w >= 0xD800 && w < 0xDC00 && i + 1 < len) { w = 0x10000 + ((w - 0xD800) << 10) + ((uint32_t)LD16(src + 2 * i + 2) - 0xDC00); i++; }
        int k;
        if (!is_utf8(cp)) { buf[0] = w < 256 ? (uint8_t)w : '?'; k = 1; if (w >= 256 && used_def) ST32(used_def, 1); }
        else if (w < 0x80) { buf[0] = (uint8_t)w; k = 1; }
        else if (w < 0x800) { buf[0] = 0xC0 | (w >> 6); buf[1] = 0x80 | (w & 63); k = 2; }
        else if (w < 0x10000) { buf[0] = 0xE0 | (w >> 12); buf[1] = 0x80 | ((w >> 6) & 63); buf[2] = 0x80 | (w & 63); k = 3; }
        else { buf[0] = 0xF0 | (w >> 18); buf[1] = 0x80 | ((w >> 12) & 63); buf[2] = 0x80 | ((w >> 6) & 63); buf[3] = 0x80 | (w & 63); k = 4; }
        if (cap) {
            if (n + k > cap) { ST32(g_teb + 0x68, 122); RET(0); return; }
            memcpy(G2H(dst + n), buf, (size_t)k);
        }
        n += k;
    }
    RET(n);
}

static uint16_t ctype1(uint32_t w)
{
    uint16_t t = 0x200;
    if (w < 128) {
        if (isupper((int)w)) t |= 1;
        if (islower((int)w)) t |= 2;
        if (isdigit((int)w)) t |= 4;
        if (isspace((int)w)) t |= 8;
        if (ispunct((int)w)) t |= 0x10;
        if (iscntrl((int)w)) t |= 0x20;
        if (w == ' ' || w == '\t') t |= 0x40;
        if (isxdigit((int)w)) t |= 0x80;
        if (isalpha((int)w)) t |= 0x100;
    } else {
        if (iswupper(w)) t |= 1;
        if (iswlower(w)) t |= 2;
        if (iswspace(w)) t |= 8;
        if (iswpunct(w)) t |= 0x10;
        if (iswalpha(w) || w >= 0xC0) t |= 0x100;
    }
    return t;
}

HLE(GetStringTypeW)
{
    uint32_t type = (uint32_t)A1;
    int64_t len = (int32_t)A3;
    if (len < 0) { len = 0; while (LD16(A2 + 2 * len)) len++; len++; }
    for (int64_t i = 0; i < len; i++) {
        uint32_t w = (uint32_t)LD16(A2 + 2 * i);
        ST16(A4 + 2 * i, type == 1 ? ctype1(w) : type == 2 ? (w < 128 ? 1 : 0) : 0);
    }
    RET(1);
}
HLE(GetStringTypeExW) { R(1) = R(2); R(2) = R(8); R(8) = R(9); R(9) = ARG(5); hle_GetStringTypeW(c); }

static int64_t wlen_or(uint64_t p, int64_t n) { if (n >= 0) return n; int64_t k = 0; while (LD16(p + 2 * k)) k++; return k + (n < 0 ? 1 : 0); }

static uint64_t map_string(uint32_t flags, uint64_t src, int64_t len, uint64_t dst, int64_t cap)
{
    int64_t n = wlen_or(src, len);
    if (flags & 0x400) {
        if (cap) {
            int64_t k = 0;
            for (int64_t i = 0; i < n && k + 2 < cap; i++) {
                uint32_t w = (uint32_t)LD16(src + 2 * i);
                if (!w) break;
                w = (uint32_t)towlower(w);
                ST8(dst + k++, (w >> 8) + 1);
                ST8(dst + k++, w & 0xFF);
            }
            ST8(dst + k++, 0);
            return (uint64_t)k;
        }
        return (uint64_t)(2 * n + 1);
    }
    if (!cap) return (uint64_t)n;
    if (n > cap) return 0;
    for (int64_t i = 0; i < n; i++) {
        uint32_t w = (uint32_t)LD16(src + 2 * i);
        if (flags & 0x100) w = (uint32_t)towlower(w);
        if (flags & 0x200) w = (uint32_t)towupper(w);
        ST16(dst + 2 * i, w);
    }
    return (uint64_t)n;
}

HLE(LCMapStringW) { RET(map_string((uint32_t)A2, A3, (int32_t)A4, ARG(5), (int32_t)ARG(6))); }
HLE(LCMapStringEx) { RET(map_string((uint32_t)A2, A3, (int32_t)A4, ARG(5), (int32_t)ARG(6))); }

static uint64_t compare(uint32_t flags, uint64_t a, int64_t la, uint64_t b, int64_t lb)
{
    if (la < 0) { la = 0; while (LD16(a + 2 * la)) la++; }
    if (lb < 0) { lb = 0; while (LD16(b + 2 * lb)) lb++; }
    for (int64_t i = 0; i < la && i < lb; i++) {
        uint32_t x = (uint32_t)LD16(a + 2 * i), y = (uint32_t)LD16(b + 2 * i);
        if (flags & 0x11) { x = (uint32_t)towlower(x); y = (uint32_t)towlower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return la == lb ? 2 : la < lb ? 1 : 3;
}
HLE(CompareStringW) { RET(compare((uint32_t)A2, A3, (int32_t)A4, ARG(5), (int32_t)ARG(6))); }
HLE(CompareStringEx) { RET(compare((uint32_t)A2, A3, (int32_t)A4, ARG(5), (int32_t)ARG(6))); }
HLE(CompareStringOrdinal) { RET(compare(A4 ? 1 : 0, A1, (int32_t)A2, A3, (int32_t)LD32(R(4) + 0x28))); }

static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char* months[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };

static const char* locale_str(uint32_t t, char* tmp)
{
    t &= 0x0FFFFFFF;
    if (t >= 0x2A && t <= 0x30) return days[(t - 0x2A + 1) % 7];
    if (t >= 0x31 && t <= 0x37) { snprintf(tmp, 8, "%.3s", days[(t - 0x31 + 1) % 7]); return tmp; }
    if (t >= 0x38 && t <= 0x43) return months[t - 0x38];
    if (t >= 0x44 && t <= 0x4F) { snprintf(tmp, 8, "%.3s", months[t - 0x44]); return tmp; }
    switch (t) {
    case 0x0E: return ".";
    case 0x0F: return ",";
    case 0x10: return "3;0";
    case 0x14: return "$";
    case 0x15: return ".";
    case 0x16: return ",";
    case 0x18: return "3;0";
    case 0x19: return "2";
    case 0x1D: return "/";
    case 0x1E: return ":";
    case 0x1F: return "M/d/yyyy";
    case 0x20: return "dddd, MMMM d, yyyy";
    case 0x28: return "AM";
    case 0x29: return "PM";
    case 0x5C: case 0x7B: return "en-US";
    case 0x59: return "en";
    case 0x5A: return "US";
    case 0x67: return "eng";
    case 0x68: return "USA";
    case 0x1001: return "English";
    case 0x1002: return "United States";
    case 0x1003: return "h:mm:ss tt";
    case 0x1004: case 0x0B: return "65001";
    case 0x01: return "0409";
    case 0x0C: return "1";
    case 0x2: case 0x1007: return "English (United States)";
    case 0x3: return "ENU";
    case 0x6: return "United States";
    case 0x7: return "USA";
    case 0x0D: return "0";
    case 0x1009: return "0";
    case 0x100C: return "6";
    case 0x2A + 0x1000: return "0";
    default: return NULL;
    }
}

static uint64_t locale_info(CPU* c, uint32_t type, uint64_t out, int64_t cap)
{
    char tmp[16];
    if (type & 0x20000000) {
        const char* s = locale_str(type & ~0x20000000u, tmp);
        uint32_t v = s ? (uint32_t)strtoul(s, NULL, 10) : 0;
        if (cap >= 2 && out) ST32(out, v);
        return 2;
    }
    const char* s = locale_str(type, tmp);
    if (!s) { ST32(g_teb + 0x68, 1004); return 0; }
    size_t n = strlen(s) + 1;
    if (!cap) return n;
    if ((int64_t)n > cap) { ST32(g_teb + 0x68, 122); return 0; }
    utf8_to_wstr(s, out, (size_t)cap);
    (void)c;
    return n;
}
HLE(GetLocaleInfoW) { RET(locale_info(c, (uint32_t)A2, A3, (int32_t)A4)); }
HLE(GetLocaleInfoEx) { RET(locale_info(c, (uint32_t)A2, A3, (int32_t)A4)); }
HLE(GetLocaleInfoA)
{
    char tmp[16];
    const char* s = locale_str((uint32_t)A2, tmp);
    if (!s) { RET(0); return; }
    size_t n = strlen(s) + 1;
    if (A4) { if ((int64_t)n > (int32_t)A4) { RET(0); return; } memcpy(G2H(A3), s, n); }
    RET(n);
}
HLE(GetUserDefaultLCID) { RET(0x409); }
HLE(GetSystemDefaultLCID) { RET(0x409); }
HLE(GetUserDefaultLangID) { RET(0x409); }
HLE(GetSystemDefaultLangID) { RET(0x409); }
HLE(GetUserDefaultUILanguage) { RET(0x409); }
HLE(GetSystemDefaultUILanguage) { RET(0x409); }
HLE(GetThreadLocale) { RET(0x409); }
HLE(SetThreadLocale) { RET(1); }
HLE(GetUserDefaultLocaleName) { RET(utf8_to_wstr("en-US", A1, (size_t)A2) + 1); }
HLE(GetSystemDefaultLocaleName) { RET(utf8_to_wstr("en-US", A1, (size_t)A2) + 1); }
HLE(LocaleNameToLCID) { RET(0x409); }
HLE(LCIDToLocaleName) { RET(A3 ? utf8_to_wstr("en-US", A2, (size_t)A3) + 1 : 6); }
HLE(IsValidLocale) { RET(1); }
HLE(IsValidLocaleName) { RET(1); }
HLE(EnumSystemLocalesW) { RET(1); }
HLE(EnumSystemLocalesEx) { RET(1); }
HLE(GetUserGeoID) { RET(244); }
HLE(GetUserPreferredUILanguages) { ST32(A2, 1); if (A3) { uint64_t n = utf8_to_wstr("en-US", A3, 16); ST16(A3 + 2 * n + 2, 0); } ST32(A4, 7); RET(1); }

static void systime(uint64_t out, struct tm* t, int ms)
{
    ST16(out, t->tm_year + 1900);
    ST16(out + 2, t->tm_mon + 1);
    ST16(out + 4, t->tm_wday);
    ST16(out + 6, t->tm_mday);
    ST16(out + 8, t->tm_hour);
    ST16(out + 10, t->tm_min);
    ST16(out + 12, t->tm_sec);
    ST16(out + 14, ms);
}

static void ft_to_tm(uint64_t ft, struct tm* out, int* ms)
{
    time_t s = (time_t)(ft / 10000000ull - 11644473600ull);
    *ms = (int)((ft / 10000) % 1000);
#ifdef _WIN32
    gmtime_s(out, &s);
#else
    gmtime_r(&s, out);
#endif
}

HLE(GetSystemTime) { struct tm t; int ms; ft_to_tm(host_time_100ns(), &t, &ms); systime(A1, &t, ms); RET(0); }
HLE(GetLocalTime) { struct tm t; int ms; ft_to_tm(host_time_100ns(), &t, &ms); systime(A1, &t, ms); RET(0); }
HLE(SetLocalTime) { RET(0); }
HLE(FileTimeToSystemTime) { struct tm t; int ms; ft_to_tm(LD64(A1), &t, &ms); systime(A2, &t, ms); RET(1); }
HLE(SystemTimeToFileTime)
{
    struct tm t = { 0 };
    t.tm_year = (int)LD16(A1) - 1900;
    t.tm_mon = (int)LD16(A1 + 2) - 1;
    t.tm_mday = (int)LD16(A1 + 6);
    t.tm_hour = (int)LD16(A1 + 8);
    t.tm_min = (int)LD16(A1 + 10);
    t.tm_sec = (int)LD16(A1 + 12);
#ifdef _WIN32
    time_t s = _mkgmtime(&t);
#else
    time_t s = timegm(&t);
#endif
    ST64(A2, ((uint64_t)s + 11644473600ull) * 10000000ull + LD16(A1 + 14) * 10000ull);
    RET(1);
}
HLE(FileTimeToLocalFileTime) { ST64(A2, LD64(A1)); RET(1); }
HLE(LocalFileTimeToFileTime) { ST64(A2, LD64(A1)); RET(1); }
HLE(SystemTimeToTzSpecificLocalTime) { memcpy(G2H(A3), G2H(A2), 16); RET(1); }
HLE(TzSpecificLocalTimeToSystemTime) { memcpy(G2H(A3), G2H(A2), 16); RET(1); }
HLE(GetTimeZoneInformation) { memset(G2H(A1), 0, 172); utf8_to_wstr("UTC", A1 + 4, 32); utf8_to_wstr("UTC", A1 + 88, 32); RET(0); }
HLE(GetDynamicTimeZoneInformation) { memset(G2H(A1), 0, 432); utf8_to_wstr("UTC", A1 + 4, 32); RET(0); }

static uint64_t fmt_out(const char* s, uint64_t out, int64_t cap)
{
    size_t n = strlen(s) + 1;
    if (!cap) return n;
    if ((int64_t)n > cap) return 0;
    utf8_to_wstr(s, out, (size_t)cap);
    return n;
}
static void cur_time(CPU* c, uint64_t st, int* y, int* mo, int* d, int* h, int* mi, int* s)
{
    struct tm t;
    int ms;
    if (st) { *y = (int)LD16(st); *mo = (int)LD16(st + 2); *d = (int)LD16(st + 6); *h = (int)LD16(st + 8); *mi = (int)LD16(st + 10); *s = (int)LD16(st + 12); return; }
    ft_to_tm(host_time_100ns(), &t, &ms);
    *y = t.tm_year + 1900; *mo = t.tm_mon + 1; *d = t.tm_mday; *h = t.tm_hour; *mi = t.tm_min; *s = t.tm_sec;
    (void)c;
}
HLE(GetDateFormatW) { int y, mo, d, h, mi, s; char b[64]; cur_time(c, A3, &y, &mo, &d, &h, &mi, &s); snprintf(b, sizeof b, "%d/%d/%d", mo, d, y); RET(fmt_out(b, ARG(5), (int32_t)ARG(6))); }
HLE(GetTimeFormatW) { int y, mo, d, h, mi, s; char b[64]; cur_time(c, A3, &y, &mo, &d, &h, &mi, &s); snprintf(b, sizeof b, "%d:%02d:%02d %s", h % 12 ? h % 12 : 12, mi, s, h < 12 ? "AM" : "PM"); RET(fmt_out(b, ARG(5), (int32_t)ARG(6))); }
HLE(GetDateFormatEx) { int y, mo, d, h, mi, s; char b[64]; cur_time(c, A3, &y, &mo, &d, &h, &mi, &s); snprintf(b, sizeof b, "%d/%d/%d", mo, d, y); RET(fmt_out(b, ARG(5), (int32_t)ARG(6))); }
HLE(GetTimeFormatEx) { int y, mo, d, h, mi, s; char b[64]; cur_time(c, A3, &y, &mo, &d, &h, &mi, &s); snprintf(b, sizeof b, "%d:%02d:%02d %s", h % 12 ? h % 12 : 12, mi, s, h < 12 ? "AM" : "PM"); RET(fmt_out(b, ARG(5), (int32_t)ARG(6))); }
