/* Unreal Engine developer console for shipping builds, the way console unlocker mods do it: shipping games compile the
   console's creation out but keep the UConsole class, so one is attached to the game viewport (GameViewportClient's
   ViewportConsole). Everything is found through the engine's own reflection data, and every layout detail is probed
   rather than assumed, so any UE4/UE5 version works:
   - names: GNames chunks of entry pointers (4.22 and older) or FNamePool blocks of 2-byte-header entries (4.23 and newer)
   - objects: the array of {UObject*, flags...} items, flat or in chunks, found from the first object (/Script/CoreUObject)
   - UObject: class at +0x10, name at +0x18, outer at +0x20 (unchanged since UE4.0)
   - SuperStruct and the property offset field are located by checking known relationships. */
#include "host.h"
#include <string.h>
#include <stdio.h>

/* debug tooling: only in builds with the UEWASM debug panel */
#ifdef UEW_DEBUG_TOOLS

static uint64_t objs;           /* flat: the item array; chunked: the chunk table */
static int chunked, stride;
static uint32_t per_chunk = 65536;   /* items per chunk: read from the first object of chunk 1 */
static uint32_t nobj;
static uint64_t names;          /* GNames chunk table, or &Blocks[0] of the FNamePool */
static int pool, name_off;      /* name_off: where the characters start in a GNames entry */
static int super_off;

static int heap_ptr(uint64_t p) { return p >= 0x10000 && p < MEM_SIZE - 64 && !(p & 7); }
static const ModuleInfo* exe(void) { return rt_module_at(g_exe_base); }

/* ---- names ---- */

static int find_names(void)
{
    const ModuleInfo* m = exe();
    if (!m) return 0;
    for (uint64_t x = m->base + 0x1000; x + 16 < m->base + m->size; x += 8) {
        uint64_t p = LD64(x);
        if (!heap_ptr(p)) continue;
        /* FNamePool block 0: header (length << 6, bit 0 = wide) + "None", then "ByteProperty" */
        uint16_t h = (uint16_t)LD16(p);
        if ((h >> 6) == 4 && !(h & 1) && !memcmp(G2H(p + 2), "None", 4)) {
            uint16_t h2 = (uint16_t)LD16(p + 6);
            if ((h2 >> 6) == 12 && !memcmp(G2H(p + 8), "ByteProperty", 12)) { names = x; pool = 1; return 1; }
        }
        /* GNames: chunk table -> entry pointers; the characters sit a few bytes into each entry */
        uint64_t c0 = LD64(p);
        if (!heap_ptr(c0)) continue;
        uint64_t e0 = LD64(c0), e1 = LD64(c0 + 8);
        if (!heap_ptr(e0) || !heap_ptr(e1)) continue;
        for (int o = 4; o <= 0x18; o += 2)
            if (!memcmp(G2H(e0 + o), "None", 5) && !memcmp(G2H(e1 + o), "ByteProperty", 13)) { names = p; pool = 0; name_off = o; return 1; }
    }
    return 0;
}

static const char* name_of(uint32_t id, char* buf, int cap)
{
    buf[0] = 0;
    if (pool) {
        uint64_t blk = LD64(names + 8ull * (id >> 16));
        if (!heap_ptr(blk)) return buf;
        uint64_t e = blk + 2ull * (id & 0xFFFF);
        uint16_t h = (uint16_t)LD16(e);
        int len = h >> 6;
        if (h & 1 || len >= cap) return buf;
        memcpy(buf, G2H(e + 2), (size_t)len);
        buf[len] = 0;
    } else {
        uint64_t chunk = LD64(names + 8ull * (id / 16384));
        if (!heap_ptr(chunk)) return buf;
        uint64_t e = LD64(chunk + 8ull * (id % 16384));
        if (!heap_ptr(e)) return buf;
        const char* s = (const char*)G2H(e + (uint64_t)name_off);
        int n = 0;
        while (n < cap - 1 && s[n]) { buf[n] = s[n]; n++; }
        buf[n] = 0;
    }
    return buf;
}

/* ---- objects ---- */

static uint64_t item(uint64_t base, uint32_t i)
{
    if (!chunked) return LD64(base + (uint64_t)stride * i);
    uint64_t chunk = LD64(base + 8ull * (i / per_chunk));
    return heap_ptr(chunk) ? LD64(chunk + (uint64_t)stride * (i % per_chunk)) : 0;
}
static int is_obj(uint64_t o, uint32_t i) { return heap_ptr(o) && LD32(o + 12) == i; }

static int find_objects(void)
{
    const ModuleInfo* m = exe();
    char b[64];
    if (!m) return 0;
    for (uint64_t x = m->base + 0x1000; x + 8 < m->base + m->size; x += 8) {
        uint64_t p = LD64(x);
        if (!heap_ptr(p)) continue;
        for (int c = 0; c < 2; c++) {
            uint64_t q = c ? LD64(p) : p;
            if (!heap_ptr(q)) continue;
            uint64_t o = LD64(q);
            if (!is_obj(o, 0) || strcmp(name_of((uint32_t)LD32(o + 0x18), b, sizeof b), "/Script/CoreUObject")) continue;
            for (int s = 16; s <= 64; s += 8) {
                if (!is_obj(LD64(q + (uint64_t)s), 1) || !is_obj(LD64(q + 2ull * s), 2)) continue;
                objs = p; chunked = c; stride = s;
                if (c && heap_ptr(LD64(p + 8)) && heap_ptr(LD64(LD64(p + 8)))) {
                    uint32_t first = (uint32_t)LD32(LD64(LD64(p + 8)) + 12);
                    if (first >= 1024 && first <= (1u << 20)) per_chunk = first;
                }
                /* the count: up to a long run of empty slots (fields holding it moved between versions) */
                uint32_t last = 0, gap = 0;
                for (uint32_t i = 0; i < (1u << 24) && gap < 65536; i++) {
                    uint64_t oi = item(objs, i);
                    if (is_obj(oi, i)) { last = i; gap = 0; } else gap++;
                    if (chunked && i % per_chunk == 0 && !heap_ptr(LD64(objs + 8ull * (i / per_chunk)))) break;
                }
                nobj = last + 1;
                return 1;
            }
        }
    }
    return 0;
}

static const char* obj_name(uint64_t o, char* buf, int cap) { return name_of((uint32_t)LD32(o + 0x18), buf, cap); }
static uint64_t obj_class(uint64_t o) { return LD64(o + 0x10); }
static uint64_t obj_outer(uint64_t o) { return LD64(o + 0x20); }
static uint64_t obj_at(uint32_t i) { uint64_t o = item(objs, i); return is_obj(o, i) ? o : 0; }
static int is(uint64_t o, const char* name)
{
    char b[128];
    return o && heap_ptr(o) && !strcmp(obj_name(o, b, sizeof b), name);
}
static uint64_t find(const char* name, const char* cls)
{
    for (uint32_t i = 0; i < nobj; i++) {
        uint64_t o = obj_at(i);
        if (o && is(o, name) && (!cls || is(obj_class(o), cls))) return o;
    }
    return 0;
}

/* ---- reflection ---- */

static int derives(uint64_t cls, uint64_t base)
{
    for (int d = 0; cls && heap_ptr(cls) && d < 32; d++, cls = LD64(cls + (uint64_t)super_off))
        if (cls == base) return 1;
    return 0;
}

/* a property's byte offset: in every version ArrayDim and ElementSize sit 0x14 and 0x10 before Offset_Internal; this
   property is an object pointer, so ArrayDim 1 and ElementSize 8 locate the field */
static int offset_field(uint64_t prop, uint32_t size)
{
    for (int o = 0x40; o <= 0x60; o += 4)
        if (LD32(prop + (uint64_t)o - 0x14) == 1 && LD32(prop + (uint64_t)o - 0x10) == size) {
            int off = (int)LD32(prop + (uint64_t)o);
            if (off >= 0x28 && off < 0x4000 && !(off & 7)) return off;
        }
    return -1;
}
/* UProperty objects (up to 4.24) or the FField list (4.25 and UE5): FField name at +0x28, next at +0x20 */
static int prop_offset(uint64_t cls, const char* prop, uint32_t size)
{
    for (uint32_t i = 0; i < nobj; i++) {
        uint64_t o = obj_at(i);
        if (o && obj_outer(o) == cls && is(o, prop)) { int off = offset_field(o, size); if (off > 0) return off; }
    }
    for (int co = 0x40; co <= 0x80; co += 8) {
        uint64_t f = LD64(cls + (uint64_t)co);
        for (int k = 0; f && heap_ptr(f) && k < 400; k++, f = LD64(f + 0x20)) {
            char b[128];
            if (!strcmp(name_of((uint32_t)LD32(f + 0x28), b, sizeof b), prop)) { int off = offset_field(f, size); if (off > 0) return off; }
        }
    }
    return -1;
}

/* ---- the console ---- */

/* UStruct::PropertiesSize (the class's instance size): an int after SuperStruct that is 0x28 for UObject itself */
static int size_off(uint64_t object, uint64_t console)
{
    for (int o = super_off + 8; o <= super_off + 0x40; o += 4) {
        uint32_t a = (uint32_t)LD32(object + (uint64_t)o), b = (uint32_t)LD32(console + (uint64_t)o);
        if (a == 0x28 && b > 0x28 && b < 0x10000) return o;
    }
    return -1;
}

/* a live console: the class default object copied into new memory, made an ordinary object owned by the viewport.
   The engine's code needs that: a default object has no world. It keeps the default object's slot in the object array,
   which garbage collection always keeps, so the copy is never collected. */
static uint64_t new_console(uint64_t object, uint64_t console, uint64_t cdo, uint64_t viewport)
{
    int so = size_off(object, console);
    if (so < 0) return 0;
    uint32_t size = (uint32_t)LD32(console + (uint64_t)so);
    uint64_t o = g_alloc_zero_aligned(size, 16);
    if (!o) return 0;
    memcpy(G2H(o), G2H(cdo), size);
    ST32(o + 8, (uint32_t)LD32(o + 8) & ~0x30u);      /* RF_ClassDefaultObject | RF_ArchetypeObject */
    ST64(o + 0x20, viewport);
    return o;
}

static int attempt(char* msg, int cap)
{
    if (!names && !find_names()) { snprintf(msg, cap, "engine name table not found yet"); return 0; }
    if (!objs && !find_objects()) { snprintf(msg, cap, "engine object table not found yet"); return 0; }
    uint64_t object = find("Object", "Class"), console = find("Console", "Class"), gvc = find("GameViewportClient", "Class");
    uint64_t cdo = find("Default__Console", NULL);
    if (!object || !console || !gvc || !cdo) { snprintf(msg, cap, "console classes not loaded yet"); return 0; }
    if (!super_off)
        for (int off = 0x28; off <= 0x60; off += 8) if (LD64(console + (uint64_t)off) == object) { super_off = off; break; }
    if (!super_off) { snprintf(msg, cap, "unknown engine layout (no SuperStruct)"); return 0; }
    int off = prop_offset(gvc, "ViewportConsole", 8);
    if (off < 0) { snprintf(msg, cap, "ViewportConsole property not found"); return 0; }
    int done = 0;
    char b[128];
    /* what the console holds now (typed line), to tell missing input from missing drawing */
    int ts = prop_offset(console, "TypedStr", 16);
    if (ts > 0) {
        uint64_t d = LD64(cdo + (uint64_t)ts);
        int n = (int)LD32(cdo + (uint64_t)ts + 8), k = 0;
        for (; d && heap_ptr(d & ~7ull) && k < n && k < 100; k++) b[k] = (char)LD16(d + 2ull * (uint64_t)k);
        b[k] = 0;
        host_log("[ue console] typed so far: \"%s\"", b);
    }
    for (uint32_t i = 0; i < nobj; i++) {
        uint64_t o = obj_at(i);
        if (!o || !derives(obj_class(o), gvc)) continue;
        if (!strncmp(obj_name(o, b, sizeof b), "Default__", 9)) continue;
        if (!LD64(o + (uint64_t)off)) {
            uint64_t con = new_console(object, console, cdo, o);
            if (!con) { snprintf(msg, cap, "could not size the console object"); return 0; }
            /* the player it talks to (its world comes from there) */
            uint64_t lp_cls = find("LocalPlayer", "Class");
            int tp = prop_offset(console, "ConsoleTargetPlayer", 8);
            for (uint32_t j = 0; lp_cls && tp > 0 && j < nobj; j++) {
                uint64_t lp = obj_at(j);
                if (lp && derives(obj_class(lp), lp_cls) && strncmp(obj_name(lp, b, sizeof b), "Default__", 9)) { ST64(con + (uint64_t)tp, lp); break; }
            }
            ST64(o + (uint64_t)off, con);
        }
        done++;
    }
    if (!done) {
        int vc = 0;
        char cn[128] = "";
        for (uint32_t i = 0; i < nobj; i++) {
            uint64_t o = obj_at(i);
            if (!o) continue;
            obj_name(obj_class(o), b, sizeof b);
            if (strstr(b, "ViewportClient") && strncmp(obj_name(o, cn, sizeof cn), "Default__", 9)) {
                vc++;
                uint64_t k = obj_class(o);
                host_log("[ue console] viewport candidate %s (class %s), supers: %s / %s", obj_name(o, cn, sizeof cn), b,
                         obj_name(LD64(k + (uint64_t)super_off), cn, sizeof cn), obj_name(LD64(LD64(k + (uint64_t)super_off) + (uint64_t)super_off), b, sizeof b));
            }
        }
        snprintf(msg, cap, "no game viewport yet (super at 0x%x, ViewportConsole at 0x%x, %d candidates, %u objects, chunked %d stride %d, table %llx: %llx %llx %llx)", super_off, off, vc, nobj, chunked, stride, (unsigned long long)objs, (unsigned long long)LD64(objs), (unsigned long long)LD64(objs + 8), (unsigned long long)LD64(objs + 16));
        return 0;
    }
    /* the keys that open it (Input.ini ConsoleKeys; FKey = FName + shared pointer, 24 bytes) */
    uint64_t is_cls = find("InputSettings", "Class"), is_cdo = find("Default__InputSettings", NULL);
    int ko = -1;
    if (is_cls) {
        for (uint32_t i = 0; i < nobj && ko < 0; i++) {
            uint64_t o = obj_at(i);
            if (o && obj_outer(o) == is_cls && is(o, "ConsoleKeys"))
                for (int f = 0x40; f <= 0x60; f += 4)
                    if (LD32(o + (uint64_t)f - 0x14) == 1 && LD32(o + (uint64_t)f - 0x10) == 16) { ko = (int)LD32(o + (uint64_t)f); break; }
        }
        for (int co = 0x40; co <= 0x80 && ko < 0; co += 8)
            for (uint64_t fp = LD64(is_cls + (uint64_t)co); fp && heap_ptr(fp) && ko < 0; fp = LD64(fp + 0x20))
                if (!strcmp(name_of((uint32_t)LD32(fp + 0x28), b, sizeof b), "ConsoleKeys"))
                    for (int f = 0x40; f <= 0x60; f += 4)
                        if (LD32(fp + (uint64_t)f - 0x14) == 1 && LD32(fp + (uint64_t)f - 0x10) == 16) { ko = (int)LD32(fp + (uint64_t)f); break; }
    }
    int nkeys = is_cdo && ko > 0 ? (int)LD32(is_cdo + (uint64_t)ko + 8) : -1;
    char keys[128] = "";
    for (int k = 0, at = 0; k < nkeys && k < 4; k++)
        at += snprintf(keys + at, sizeof keys - at, "%s%s", k ? ", " : "", name_of((uint32_t)LD32(LD64(is_cdo + (uint64_t)ko) + 24ull * (uint64_t)k), b, sizeof b));
    snprintf(msg, cap, "console attached to %d viewport(s) (%s names, %u objects); open it with %s", done, pool ? "pool" : "chunked",
             nobj, nkeys > 0 ? keys : "` or ~");
    return 1;
}

#ifdef __EMSCRIPTEN__
__attribute__((used, visibility("default")))
#endif
int uew_ue_console(void)
{
    char msg[256];
    int ok = attempt(msg, sizeof msg);
    host_log("[ue console] %s", msg);
    return ok;
}
#endif
