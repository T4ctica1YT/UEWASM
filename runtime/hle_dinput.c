/* DirectInput 8: the system keyboard and mouse, from the browser's input (immediate state and buffered events).
   No joysticks are listed; games read pads through XInput. */
#include "com.h"

#include <string.h>

int user32_key(int vk);
void user32_di_mouse(int* dx, int* dy, int* dz);

static const uint8_t GUID_SysMouse[16] = { 0x60, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t GUID_SysKeyboard[16] = { 0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11, 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 };
static const uint8_t IID_IDirectInput8W[16] = { 0x00, 0x2F, 0x43, 0xBF, 0x9E, 0x56, 0xDA, 0x47, 0x9A, 0x1F, 0xE0, 0x72, 0xE5, 0x7D, 0x6E, 0x3E };
static const uint8_t IID_IDirectInput8A[16] = { 0x30, 0x80, 0x79, 0xBF, 0x3C, 0x22, 0xD3, 0x47, 0x9F, 0x1B, 0x9F, 0x91, 0x75, 0xB2, 0x5B, 0x3D };
static const uint8_t iids_di[2][16] = {
    { 0x00, 0x2F, 0x43, 0xBF, 0x9E, 0x56, 0xDA, 0x47, 0x9A, 0x1F, 0xE0, 0x72, 0xE5, 0x7D, 0x6E, 0x3E },
    { 0x30, 0x80, 0x79, 0xBF, 0x3C, 0x22, 0xD3, 0x47, 0x9F, 0x1B, 0x9F, 0x91, 0x75, 0xB2, 0x5B, 0x3D } };
static ComKind K_DI = { "IDirectInput8", 11, {0}, iids_di, 2 };
static ComKind K_Dev = { "IDirectInputDevice8", 32, {0} };

/* DIK scan code of each virtual key (0 = none) */
static uint8_t vk2dik[256];
static void init_keys(void)
{
    static const uint8_t m[][2] = {
        { 0x1B, 0x01 }, { '1', 0x02 }, { '2', 0x03 }, { '3', 0x04 }, { '4', 0x05 }, { '5', 0x06 }, { '6', 0x07 }, { '7', 0x08 }, { '8', 0x09 },
        { '9', 0x0A }, { '0', 0x0B }, { 0xBD, 0x0C }, { 0xBB, 0x0D }, { 0x08, 0x0E }, { 0x09, 0x0F }, { 'Q', 0x10 }, { 'W', 0x11 }, { 'E', 0x12 },
        { 'R', 0x13 }, { 'T', 0x14 }, { 'Y', 0x15 }, { 'U', 0x16 }, { 'I', 0x17 }, { 'O', 0x18 }, { 'P', 0x19 }, { 0xDB, 0x1A }, { 0xDD, 0x1B },
        { 0x0D, 0x1C }, { 0xA2, 0x1D }, { 0x11, 0x1D }, { 'A', 0x1E }, { 'S', 0x1F }, { 'D', 0x20 }, { 'F', 0x21 }, { 'G', 0x22 }, { 'H', 0x23 },
        { 'J', 0x24 }, { 'K', 0x25 }, { 'L', 0x26 }, { 0xBA, 0x27 }, { 0xDE, 0x28 }, { 0xC0, 0x29 }, { 0xA0, 0x2A }, { 0x10, 0x2A }, { 0xDC, 0x2B },
        { 'Z', 0x2C }, { 'X', 0x2D }, { 'C', 0x2E }, { 'V', 0x2F }, { 'B', 0x30 }, { 'N', 0x31 }, { 'M', 0x32 }, { 0xBC, 0x33 }, { 0xBE, 0x34 },
        { 0xBF, 0x35 }, { 0xA1, 0x36 }, { 0x6A, 0x37 }, { 0xA4, 0x38 }, { 0x12, 0x38 }, { 0x20, 0x39 }, { 0x14, 0x3A }, { 0x70, 0x3B }, { 0x71, 0x3C },
        { 0x72, 0x3D }, { 0x73, 0x3E }, { 0x74, 0x3F }, { 0x75, 0x40 }, { 0x76, 0x41 }, { 0x77, 0x42 }, { 0x78, 0x43 }, { 0x79, 0x44 }, { 0x90, 0x45 },
        { 0x91, 0x46 }, { 0x67, 0x47 }, { 0x68, 0x48 }, { 0x69, 0x49 }, { 0x6D, 0x4A }, { 0x64, 0x4B }, { 0x65, 0x4C }, { 0x66, 0x4D }, { 0x6B, 0x4E },
        { 0x61, 0x4F }, { 0x62, 0x50 }, { 0x63, 0x51 }, { 0x60, 0x52 }, { 0x6E, 0x53 }, { 0x7A, 0x57 }, { 0x7B, 0x58 }, { 0xA3, 0x9D }, { 0x6F, 0xB5 },
        { 0x2C, 0xB7 }, { 0xA5, 0xB8 }, { 0x13, 0xC5 }, { 0x24, 0xC7 }, { 0x26, 0xC8 }, { 0x21, 0xC9 }, { 0x25, 0xCB }, { 0x27, 0xCD }, { 0x23, 0xCF },
        { 0x28, 0xD0 }, { 0x22, 0xD1 }, { 0x2D, 0xD2 }, { 0x2E, 0xD3 }, { 0x5B, 0xDB }, { 0x5C, 0xDC }, { 0x5D, 0xDD } };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (!vk2dik[m[i][0]]) vk2dik[m[i][0]] = m[i][1];
}

/* per device: +0 kind (1 keyboard, 2 mouse), +4 acquired, +8 buffer size, +12 sequence, +16 previous key states (256),
   +272 previous mouse buttons */
enum { D_KIND = 0, D_ACQ = 4, D_BUFSZ = 8, D_SEQ = 12, D_PREV = 16, D_PREVBTN = 272, D_SIZE = 280 };
#define DEV(o) ((o) + COM_HDR)

static void ok(CPU* c) { RET(0); }
static void keyboard_state(uint8_t* out)
{
    memset(out, 0, 256);
    for (int vk = 1; vk < 256; vk++) if (vk2dik[vk] && user32_key(vk)) out[vk2dik[vk]] = 0x80;
}
static int buttons(void) { return (user32_key(1) ? 1 : 0) | (user32_key(2) ? 2 : 0) | (user32_key(4) ? 4 : 0) | (user32_key(5) ? 8 : 0) | (user32_key(6) ? 16 : 0); }

static void di_create_device(CPU* c)
{
    int kind = iid_eq(A2, GUID_SysKeyboard) ? 1 : iid_eq(A2, GUID_SysMouse) ? 2 : 0;
    if (!kind) { if (A3) ST64(A3, 0); RET(0x80040154); return; }   /* REGDB_E_CLASSNOTREG: no such device */
    uint64_t o = com_new(&K_Dev, NULL, D_SIZE);
    memset(G2H(DEV(o)), 0, D_SIZE);
    ST32(DEV(o) + D_KIND, (uint32_t)kind);
    ST64(A3, o);
    RET(0);
}
/* DIDEVICEINSTANCEW: size, instance guid, product guid, type, instance name[260], product name[260], ff guid, usage page, usage */
static void fill_instance(uint64_t p, int kind)
{
    memset(G2H(p), 0, 1100);
    ST32(p, 1100);
    memcpy(G2H(p + 4), kind == 1 ? GUID_SysKeyboard : GUID_SysMouse, 16);
    memcpy(G2H(p + 20), kind == 1 ? GUID_SysKeyboard : GUID_SysMouse, 16);
    ST32(p + 36, kind == 1 ? 0x0413 : 0x0212);   /* DI8DEVTYPE_KEYBOARD/PCENH, DI8DEVTYPE_MOUSE/UNKNOWN */
    const char* name = kind == 1 ? "Keyboard" : "Mouse";
    for (int i = 0; name[i]; i++) { ST16(p + 40 + 2 * i, (uint8_t)name[i]); ST16(p + 560 + 2 * i, (uint8_t)name[i]); }
    ST16(p + 1096, 1); ST16(p + 1098, kind == 1 ? 6 : 2);
}
static void di_enum_devices(CPU* c)
{
    uint32_t cls = (uint32_t)A2;
    uint64_t cb = A3, ref = A4, inst = g_alloc(1100);
    for (int kind = 1; kind <= 2; kind++) {
        if (cls && !(cls == 1 || (cls == 3 && kind == 1) || (cls == 2 && kind == 2))) continue;
        fill_instance(inst, kind);
        uint64_t a[2] = { inst, ref };
        if (!rt_invoke(c, cb, 2, a)) break;   /* DIENUM_STOP */
    }
    g_free(inst);
    RET(0);
}
static void di_status(CPU* c) { RET(iid_eq(A2, GUID_SysKeyboard) || iid_eq(A2, GUID_SysMouse) ? 0 : 1); }   /* DI_OK / DI_NOTATTACHED */

/* DIDEVCAPS: size, flags, devtype, axes, buttons, povs, ... */
static void dev_caps(CPU* c)
{
    int kind = (int)LD32(DEV(THIS) + D_KIND);
    uint64_t p = A2;
    uint32_t sz = (uint32_t)LD32(p);
    memset(G2H(p + 4), 0, sz > 4 ? sz - 4 : 0);
    ST32(p + 4, 5);   /* DIDC_ATTACHED | DIDC_EMULATED */
    ST32(p + 8, kind == 1 ? 0x0413 : 0x0212);
    ST32(p + 12, kind == 1 ? 0 : 3);
    ST32(p + 16, kind == 1 ? 128 : 5);
    RET(0);
}
/* DIPROPHEADER: size, header size, object, how; DIPROPDWORD adds the value at +16 */
static const uint64_t DIPROP_BUFFERSIZE = 1, DIPROP_AXISMODE = 2;
static void dev_set_prop(CPU* c)
{
    if (A2 == DIPROP_BUFFERSIZE) ST32(DEV(THIS) + D_BUFSZ, (uint32_t)LD32(A3 + 16));
    RET(0);
}
static void dev_get_prop(CPU* c)
{
    if (A2 == DIPROP_BUFFERSIZE) ST32(A3 + 16, (uint32_t)LD32(DEV(THIS) + D_BUFSZ));
    else if (A2 == DIPROP_AXISMODE) ST32(A3 + 16, 0);   /* DIPROPAXISMODE_REL */
    else { RET(0x80070002); return; }
    RET(0);
}
static void dev_acquire(CPU* c)
{
    uint64_t d = DEV(THIS);
    if (LD32(d + D_KIND) == 1) keyboard_state(G2H(d + D_PREV));
    else { int dx, dy, dz; user32_di_mouse(&dx, &dy, &dz); ST32(d + D_PREVBTN, (uint32_t)buttons()); }
    ST32(d + D_ACQ, 1);
    RET(0);
}
static void dev_unacquire(CPU* c) { ST32(DEV(THIS) + D_ACQ, 0); RET(0); }
static void dev_state(CPU* c)
{
    uint64_t d = DEV(THIS), out = A3;
    uint32_t n = (uint32_t)A2;
    if (!LD32(d + D_ACQ)) { RET(0x8007000C); return; }   /* DIERR_NOTACQUIRED */
    if (LD32(d + D_KIND) == 1) {
        uint8_t k[256];
        keyboard_state(k);
        memcpy(G2H(out), k, n < 256 ? n : 256);
    } else {
        int dx, dy, dz, b = buttons();
        user32_di_mouse(&dx, &dy, &dz);
        memset(G2H(out), 0, n);
        if (n >= 12) { ST32(out, (uint32_t)dx); ST32(out + 4, (uint32_t)dy); ST32(out + 8, (uint32_t)dz); }
        for (uint32_t i = 0; i < 8 && 12 + i < n; i++) ST8(out + 12 + i, (b >> i) & 1 ? 0x80 : 0);
    }
    RET(0);
}
/* DIDEVICEOBJECTDATA (x64): offset, data, timestamp, sequence, app data (8): 24 bytes */
static void dev_data(CPU* c)
{
    uint64_t d = DEV(THIS), out = A3, inout = A4;
    uint32_t each = (uint32_t)A2 ? (uint32_t)A2 : 24, cap = inout ? (uint32_t)LD32(inout) : 0, n = 0, peek = (uint32_t)ARG(5) & 1;
    if (!LD32(d + D_ACQ)) { if (inout) ST32(inout, 0); RET(0x8007000C); return; }
    uint32_t seq = (uint32_t)LD32(d + D_SEQ), t = (uint32_t)(host_ticks_ns() / 1000000);
#define EVENT(ofs, val) do { if (n < cap && out) { uint64_t e_ = out + (uint64_t)each * n; memset(G2H(e_), 0, each); ST32(e_, (ofs)); ST32(e_ + 4, (val)); ST32(e_ + 8, t); ST32(e_ + 12, seq++); } n++; } while (0)
    if (LD32(d + D_KIND) == 1) {
        uint8_t k[256], *prev = G2H(d + D_PREV);
        keyboard_state(k);
        for (int i = 0; i < 256; i++) if (k[i] != prev[i]) { EVENT((uint32_t)i, k[i]); if (!peek && n <= cap) prev[i] = k[i]; }
    } else {
        int dx, dy, dz, b = buttons(), pb = (int)LD32(d + D_PREVBTN);
        user32_di_mouse(&dx, &dy, &dz);
        if (dx) EVENT(0, (uint32_t)dx);
        if (dy) EVENT(4, (uint32_t)dy);
        if (dz) EVENT(8, (uint32_t)dz);
        for (int i = 0; i < 5; i++) if (((b ^ pb) >> i) & 1) EVENT(12 + (uint32_t)i, (b >> i) & 1 ? 0x80 : 0);
        if (!peek) ST32(d + D_PREVBTN, (uint32_t)b);
    }
#undef EVENT
    if (!peek) ST32(d + D_SEQ, seq);
    if (inout) ST32(inout, n < cap ? n : cap);
    RET(n > cap ? 1 : 0);   /* DI_BUFFEROVERFLOW */
}
static void dev_info(CPU* c) { fill_instance(A2, (int)LD32(DEV(THIS) + D_KIND)); RET(0); }
static void dev_enum_none(CPU* c) { RET(0); }
static void dev_unsupported(CPU* c) { RET(0x80004001); }

static void setup(void)
{
    static int done;
    if (done) return;
    done = 1;
    init_keys();
    com_iunknown(&K_DI);
    K_DI.slots[3] = di_create_device; K_DI.slots[4] = di_enum_devices; K_DI.slots[5] = di_status; K_DI.slots[6] = ok; K_DI.slots[7] = ok;
    K_DI.slots[8] = dev_unsupported; K_DI.slots[9] = dev_enum_none; K_DI.slots[10] = dev_unsupported;
    com_iunknown(&K_Dev);
    GuestFn s[32] = { 0, 0, 0, dev_caps, dev_enum_none, dev_get_prop, dev_set_prop, dev_acquire, dev_unacquire, dev_state, dev_data, ok, ok, ok,
                      dev_unsupported, dev_info, ok, ok, dev_unsupported, dev_enum_none, dev_unsupported, dev_unsupported, dev_unsupported,
                      dev_enum_none, dev_unsupported, ok, dev_unsupported, dev_enum_none, dev_unsupported, dev_unsupported, dev_unsupported, dev_unsupported };
    for (int i = 3; i < 32; i++) K_Dev.slots[i] = s[i];
}

HLE(DirectInput8Create)
{
    setup();
    uint64_t out = A4;
    if (!out) { RET(0x80070057); return; }
    if (A3 && !iid_eq(A3, IID_IDirectInput8W) && !iid_eq(A3, IID_IDirectInput8A)) { ST64(out, 0); RET(0x80004002); return; }
    ST64(out, com_new(&K_DI, NULL, 0));
    RET(0);
}
