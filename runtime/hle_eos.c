/* EOS SDK (Epic Online Services): the runtime stands in for EOSSDK-Win64/Win32-Shipping.dll, which can't reach Epic
   from a page. Games load it themselves and look its functions up by name (Unreal's EOS subsystems and Redpoint's
   plugin both do); every name in the C API resolves here (eos_api.h, generated from the SDK headers).
   The player signs in at once with a lasting identity (the browser's device key): Connect and Auth logins succeed,
   the platform and its interfaces exist, and calls complete their callbacks during EOS_Platform_Tick, as the SDK
   does. What needs Epic's servers answers the way an empty account would: queries succeed with nothing found, copies
   find nothing, joining someone else's lobby or session reports no connection. Lobbies are kept in the page, the
   player's name and linked account come from the WebWorks / Steam session, and player data storage (cloud saves)
   keeps its files in the save folder. ?eosdebug=1 logs each function used. */
#include "sched.h"
#include "eos_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EOS_Success 0
#define EOS_NoConnection 1
#define EOS_InvalidParameters 10
#define EOS_NotFound 18
#define EOS_LimitExceeded 22

#ifdef __EMSCRIPTEN__
int ww_device(char* buf, int cap);
int ww_self(char* buf, int cap);
int ww_name(const char* id, char* buf, int cap);
#else
static int ww_device(char* b, int n) { snprintf(b, (size_t)n, "0123456789abcdef0123456789abcdef"); return 32; }
static int ww_self(char* b, int n) { if (n) b[0] = 0; return 0; }
static int ww_name(const char* i, char* b, int n) { (void)i; if (n) b[0] = 0; return 0; }
#endif

static int eos_dbg(void)
{
    static int d = -1;
    if (d < 0) d = host_query_int("eosdebug");
    return d;
}

/* ---- identities: a product user id and an Epic account id, each a small block holding its 32-hex-digit string ---- */

static uint64_t puid, eaid;
static int connect_in, auth_in;

static uint64_t id_block(const char* s)
{
    uint64_t p = g_calloc(48);
    ST32(p, 0x5EED1D);
    snprintf((char*)G2H(p + 8), 33, "%s", s);
    return p;
}
static int id_valid(uint64_t p) { return p && LD32(p) == 0x5EED1D; }

static void make_ids(void)
{
    if (puid) return;
    char k[40];
    if (ww_device(k, sizeof k) < 32) snprintf(k, sizeof k, "0123456789abcdef0123456789abcdef");
    k[32] = 0;
    puid = id_block(k);
    char e[33];
    for (int i = 0; i < 32; i++) e[i] = k[31 - i];   /* a different id for the Epic account */
    e[32] = 0;
    eaid = id_block(e);
}

static uint32_t id_to_string(uint64_t id, uint64_t out, uint64_t inout)
{
    if (!id_valid(id) || !inout) return EOS_InvalidParameters;
    const char* s = (const char*)G2H(id + 8);
    int32_t cap = (int32_t)LD32(inout), n = (int32_t)strlen(s);
    ST32(inout, (uint32_t)(n + 1));
    if (!out || cap < n + 1) return EOS_LimitExceeded;
    memcpy(G2H(out), s, (size_t)n + 1);
    return EOS_Success;
}

static uint64_t id_from_string(uint64_t str)
{
    if (!str) return 0;
    const char* s = GSTR(str);
    make_ids();
    if (!strcmp(s, (const char*)G2H(puid + 8))) return puid;
    if (!strcmp(s, (const char*)G2H(eaid + 8))) return eaid;
    if (strlen(s) >= 1 && strlen(s) <= 32) return id_block(s);
    if (eos_dbg()) host_log("[eos] id string not taken: \"%.64s\"", s);
    return 0;
}

/* ---- handles: the platform and one block per interface ---- */

static uint64_t platform;
static uint64_t handle_for(const char* name)
{
    static struct { char n[48]; uint64_t h; } hs[48];
    static int nh;
    for (int i = 0; i < nh; i++) if (!strcmp(hs[i].n, name)) return hs[i].h;
    if (nh == 48) return 0;
    snprintf(hs[nh].n, sizeof hs[nh].n, "%s", name);
    hs[nh].h = g_calloc(64);
    ST32(hs[nh].h, 0x5EED0E);
    return hs[nh++].h;
}

/* ---- callbacks run on the next EOS_Platform_Tick, with a callback-info struct: ResultCode, ClientData, then fields ---- */

typedef struct { uint64_t fn; uint8_t info[64]; uint32_t size; } Pending;
static Pending queue[256];
static int nqueue;

static uint8_t* complete(uint64_t fn, uint32_t result, uint64_t client)
{
    if (!fn || nqueue == 256) return NULL;
    Pending* p = &queue[nqueue++];
    memset(p, 0, sizeof *p);
    p->fn = fn;
    p->size = 64;
    memcpy(p->info, &result, 4);
    if (RT_G32) { uint32_t c32 = (uint32_t)client; memcpy(p->info + 4, &c32, 4); }
    else memcpy(p->info + 8, &client, 8);
    return p->info;
}
/* the i-th pointer-sized field after ClientData */
static void set_field(uint8_t* info, int i, uint64_t v)
{
    if (!info) return;
    if (RT_G32) { uint32_t x = (uint32_t)v; memcpy(info + 8 + 4 * i, &x, 4); }
    else memcpy(info + 16 + 8 * i, &v, 8);
}

static void run_queue(CPU* c)
{
    static int busy;
    if (busy) return;
    busy = 1;
    int n = nqueue;
    Pending batch[256];
    memcpy(batch, queue, sizeof(Pending) * n);
    memmove(queue, queue + n, sizeof(Pending) * (nqueue - n));
    nqueue -= n;
    for (int i = 0; i < n; i++) {
        uint64_t buf = g_calloc(batch[i].size);
        memcpy(G2H(buf), batch[i].info, batch[i].size);
        uint64_t a[1] = { buf };
        rt_invoke(c, batch[i].fn, 1, a);
        g_free(buf);
    }
    busy = 0;
}

/* ---- the functions ---- */

static uint64_t arg(CPU* c, int i) { return i == 0 ? A1 : i == 1 ? A2 : i == 2 ? A3 : i == 3 ? A4 : ARG(i + 1); }

static int starts(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }
static int has(const char* s, const char* p) { return strstr(s, p) != NULL; }

static void put_code(CPU* c, uint64_t out, uint64_t inout, const char* v)
{
    int32_t cap = inout ? (int32_t)LD32(inout) : 0, n = (int32_t)strlen(v);
    if (inout) ST32(inout, (uint32_t)(n + 1));
    if (!out || cap < n + 1) { RET(EOS_LimitExceeded); return; }
    memcpy(G2H(out), v, (size_t)n + 1);
    RET(EOS_Success);
}

static uint64_t gstr_cached(const char* s)
{
    static struct { char s[48]; uint64_t p; } cache[32];
    static int n;
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].s, s)) return cache[i].p;
    uint64_t p = g_calloc(strlen(s) + 1);
    memcpy(G2H(p), s, strlen(s) + 1);
    if (n < 32) { snprintf(cache[n].s, sizeof cache[n].s, "%s", s); cache[n++].p = p; }
    return p;
}

/* ---- struct layouts: EOS packs its structs to 8 on every Windows target. A layout string names each field:
   'i' int32/enum/EOS_Bool, 'p' pointer, 'q' int64/double/the attribute value union ---- */

static uint32_t lay(const char* L, int idx)
{
    uint32_t o = 0;
    for (int k = 0; L[k]; k++) {
        uint32_t sz = L[k] == 'p' ? GP : L[k] == 'q' ? 8 : 4;
        o = (o + sz - 1) & ~(sz - 1);
        if (k == idx) return o;
        o += sz;
    }
    return (o + 7) & ~7u;
}
static uint64_t rd(uint64_t base, const char* L, int idx)
{
    uint32_t o = lay(L, idx);
    return L[idx] == 'p' ? LDP(base + o) : L[idx] == 'q' ? LD64(base + o) : LD32(base + o);
}
static void wr(uint64_t base, const char* L, int idx, uint64_t v)
{
    uint32_t o = lay(L, idx);
    if (L[idx] == 'p') STP(base + o, v);
    else if (L[idx] == 'q') ST64(base + o, v);
    else ST32(base + o, (uint32_t)v);
}
static uint64_t alloc_struct(const char* L) { return g_calloc(lay(L, (int)strlen(L))); }

/* a callback-info struct built by layout, run on the next tick */
static void post_info(uint64_t fn, const char* L, const uint64_t* v)
{
    if (!fn || nqueue == 256) return;
    Pending* p = &queue[nqueue++];
    memset(p, 0, sizeof p->info);
    p->fn = fn;
    p->size = lay(L, (int)strlen(L));
    if (p->size > sizeof p->info) p->size = sizeof p->info;
    for (int k = 0; L[k]; k++) {
        uint32_t o = lay(L, k);
        if (L[k] == 'p' && GP == 4) { uint32_t x = (uint32_t)v[k]; memcpy(p->info + o, &x, 4); }
        else if (L[k] == 'i') { uint32_t x = (uint32_t)v[k]; memcpy(p->info + o, &x, 4); }
        else memcpy(p->info + o, &v[k], 8);
    }
}

/* ---- notifications: AddNotify* registrations, fired by what they watch ---- */

typedef struct { char what[64]; uint64_t id, fn, cd; } Notify;
static Notify notes[128];
static int nnotes;

static void notify(const char* what, const char* L, uint64_t a, uint64_t b, uint64_t c3)
{
    for (int k = 0; k < nnotes; k++)
        if (!strcmp(notes[k].what, what)) {
            uint64_t v[4] = { notes[k].cd, a, b, c3 };
            post_info(notes[k].fn, L, v);
        }
}

/* ---- lobbies, kept in the page: create, modify, read back, leave ---- */

enum { AT_BOOL, AT_INT64, AT_DOUBLE, AT_STRING };
typedef struct { char key[64]; int type, vis; uint64_t num; char s[256]; } Attr;
typedef struct { uint64_t user; Attr a[16]; int na; } Member;
typedef struct {
    int used;
    char id[33];
    uint64_t idstr, owner, bucketstr;
    uint32_t max;
    int perm, invites;
    Attr a[48];
    int na;
    Member m[16];
    int nm;
} Lobby;
static Lobby lobbies[8];
typedef struct { uint64_t handle; int lobby; Lobby staged; uint64_t user; } Mod;
static Mod mods[4];

static Lobby* lobby_by_id(uint64_t idstr)
{
    if (!idstr) return NULL;
    for (int k = 0; k < 8; k++) if (lobbies[k].used && !strcmp(lobbies[k].id, GSTR(idstr))) return &lobbies[k];
    return NULL;
}
static Lobby* lobby_of_handle(uint64_t h) { return h && LD32(h) == 0x5EED41 && LD32(h + 4) < 8 ? &lobbies[LD32(h + 4)] : NULL; }

static void attr_set(Attr* list, int* n, int cap, uint64_t data, int vis)
{
    static const char* const L = "ipqi";
    uint64_t key = rd(data, L, 1);
    if (!key) return;
    Attr* a = NULL;
    for (int k = 0; k < *n; k++) if (!strcmp(list[k].key, GSTR(key))) a = &list[k];
    if (!a) { if (*n == cap) return; a = &list[(*n)++]; }
    snprintf(a->key, sizeof a->key, "%s", GSTR(key));
    a->type = (int)rd(data, L, 3);
    a->vis = vis;
    a->num = rd(data, L, 2);
    a->s[0] = 0;
    if (a->type == AT_STRING) {
        uint64_t sp = (uint64_t)(RT_G32 ? (uint32_t)a->num : a->num);
        if (sp) snprintf(a->s, sizeof a->s, "%s", GSTR(sp));
    }
}
static void attr_del(Attr* list, int* n, const char* key)
{
    for (int k = 0; k < *n; k++) if (!strcmp(list[k].key, key)) { list[k] = list[--*n]; return; }
}
/* an EOS_Lobby_Attribute copy for the game (freed by EOS_Lobby_Attribute_Release) */
static uint64_t attr_copy(const Attr* a)
{
    uint64_t at = alloc_struct("ipi"), d = alloc_struct("ipqi"), key = g_calloc(strlen(a->key) + 1);
    memcpy(G2H(key), a->key, strlen(a->key) + 1);
    uint64_t val = a->num;
    if (a->type == AT_STRING) { val = g_calloc(strlen(a->s) + 1); memcpy(G2H(val), a->s, strlen(a->s) + 1); }
    wr(d, "ipqi", 0, 1); wr(d, "ipqi", 1, key); wr(d, "ipqi", 2, val); wr(d, "ipqi", 3, (uint64_t)a->type);
    wr(at, "ipi", 0, 1); wr(at, "ipi", 1, d); wr(at, "ipi", 2, (uint64_t)a->vis);
    return at;
}
static Member* member(Lobby* l, uint64_t user)
{
    for (int k = 0; k < l->nm; k++) if (l->m[k].user == user) return &l->m[k];
    return NULL;
}

/* 1 when n (after "EOS_") is a lobby call handled here; its result is in R(0) */
static int lobby_call(CPU* c, const EosFn* f, const char* n)
{
    uint64_t o = A2;   /* every lobby call takes (handle, options, ...) */
    if (!strcmp(n, "Lobby_CreateLobby")) {
        static const char* const L = "ipiiiip";
        int k = 0;
        while (k < 8 && lobbies[k].used) k++;
        uint64_t v[3] = { k < 8 ? EOS_Success : EOS_LimitExceeded, arg(c, f->cd), 0 };
        if (k < 8) {
            Lobby* l = &lobbies[k];
            static uint32_t seq;
            memset(l, 0, sizeof *l);
            l->used = 1;
            snprintf(l->id, sizeof l->id, "%08x%.24s", 0x5EED0000u + ++seq, (const char*)G2H(puid + 8));
            l->idstr = g_calloc(33);
            memcpy(G2H(l->idstr), l->id, 33);
            l->owner = o && rd(o, L, 1) ? rd(o, L, 1) : puid;
            l->max = o ? (uint32_t)rd(o, L, 2) : 4;
            l->perm = o ? (int)rd(o, L, 3) : 0;
            l->invites = o ? (int)rd(o, L, 5) : 1;
            l->bucketstr = g_calloc(64);
            uint64_t b = o && LD32(o) >= 2 ? rd(o, L, 6) : 0;
            if (b) snprintf((char*)G2H(l->bucketstr), 64, "%s", GSTR(b));
            l->nm = 1;
            l->m[0].user = l->owner;
            v[2] = l->idstr;
        }
        post_info(arg(c, f->cb), "ipp", v);
        RET(0);
        return 1;
    }
    if (!strcmp(n, "Lobby_UpdateLobbyModification")) {
        Lobby* l = o ? lobby_by_id(rd(o, "ipp", 2)) : NULL;
        int k = 0;
        while (k < 4 && mods[k].handle) k++;
        if (!l || k == 4 || !A3) { RET(EOS_NotFound); return 1; }
        mods[k].handle = g_calloc(16);
        ST32(mods[k].handle, 0x5EED40);
        mods[k].lobby = (int)(l - lobbies);
        mods[k].staged = *l;
        mods[k].user = rd(o, "ipp", 1) ? rd(o, "ipp", 1) : puid;
        STP(A3, mods[k].handle);
        RET(EOS_Success);
        return 1;
    }
    if (starts(n, "LobbyModification_")) {
        Mod* md = NULL;
        for (int k = 0; k < 4; k++) if (mods[k].handle && mods[k].handle == A1) md = &mods[k];
        if (!md) { RET(EOS_InvalidParameters); return 1; }
        Lobby* s = &md->staged;
        const char* op = n + 18;
        if (!strcmp(op, "Release")) { g_free(md->handle); md->handle = 0; RET(0); return 1; }
        if (!strcmp(op, "SetMaxMembers")) s->max = (uint32_t)rd(o, "ii", 1);
        else if (!strcmp(op, "SetPermissionLevel")) s->perm = (int)rd(o, "ii", 1);
        else if (!strcmp(op, "SetInvitesAllowed")) s->invites = (int)rd(o, "ii", 1);
        else if (!strcmp(op, "SetBucketId") && rd(o, "ip", 1)) snprintf((char*)G2H(s->bucketstr), 64, "%s", GSTR(rd(o, "ip", 1)));
        else if (!strcmp(op, "AddAttribute") && rd(o, "ipi", 1)) attr_set(s->a, &s->na, 48, rd(o, "ipi", 1), (int)rd(o, "ipi", 2));
        else if (!strcmp(op, "RemoveAttribute") && rd(o, "ip", 1)) attr_del(s->a, &s->na, GSTR(rd(o, "ip", 1)));
        else if (!strcmp(op, "AddMemberAttribute") || !strcmp(op, "RemoveMemberAttribute")) {
            Member* m = member(s, md->user);
            if (m && !strcmp(op, "AddMemberAttribute") && rd(o, "ipi", 1)) attr_set(m->a, &m->na, 16, rd(o, "ipi", 1), (int)rd(o, "ipi", 2));
            if (m && !strcmp(op, "RemoveMemberAttribute") && rd(o, "ip", 1)) attr_del(m->a, &m->na, GSTR(rd(o, "ip", 1)));
        }
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "Lobby_UpdateLobby")) {
        Mod* md = NULL;
        uint64_t h = o ? rd(o, "ip", 1) : 0;
        for (int k = 0; k < 4; k++) if (mods[k].handle && mods[k].handle == h) md = &mods[k];
        uint64_t v[3] = { md ? EOS_Success : EOS_InvalidParameters, arg(c, f->cd), 0 };
        if (md) {
            Lobby* l = &lobbies[md->lobby];
            *l = md->staged;
            v[2] = l->idstr;
            notify("LobbyUpdateReceived", "pp", l->idstr, 0, 0);
            notify("LobbyMemberUpdateReceived", "ppp", l->idstr, md->user, 0);
        }
        post_info(arg(c, f->cb), "ipp", v);
        RET(0);
        return 1;
    }
    if (!strcmp(n, "Lobby_CopyLobbyDetailsHandle")) {
        Lobby* l = o ? lobby_by_id(rd(o, "ipp", 1)) : NULL;
        if (!l || !A3) { RET(EOS_NotFound); return 1; }
        uint64_t d = g_calloc(16);
        ST32(d, 0x5EED41);
        ST32(d + 4, (uint32_t)(l - lobbies));
        STP(A3, d);
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "Lobby_LeaveLobby") || !strcmp(n, "Lobby_DestroyLobby")) {
        Lobby* l = o ? lobby_by_id(rd(o, "ipp", 2)) : NULL;
        uint64_t v[3] = { l ? EOS_Success : EOS_NotFound, arg(c, f->cd), o ? rd(o, "ipp", 2) : 0 };
        post_info(arg(c, f->cb), "ipp", v);
        if (l) l->used = 0;
        RET(0);
        return 1;
    }
    if (starts(n, "LobbyDetails_")) {
        const char* op = n + 13;
        if (!strcmp(op, "Release") || !strcmp(op, "Info_Release") || !strcmp(op, "MemberInfo_Release")) { if (A1) g_free(A1); RET(0); return 1; }
        Lobby* l = lobby_of_handle(A1);
        if (!l || !l->used) { RET(!strcmp(op, "GetLobbyOwner") || has(op, "Count") ? 0 : EOS_NotFound); return 1; }
        if (!strcmp(op, "GetLobbyOwner")) { RET(l->owner); return 1; }
        if (!strcmp(op, "GetAttributeCount")) { RET((uint32_t)l->na); return 1; }
        if (!strcmp(op, "GetMemberCount")) { RET((uint32_t)l->nm); return 1; }
        if (!strcmp(op, "GetMemberByIndex")) { uint32_t k = (uint32_t)rd(o, "ii", 1); RET(k < (uint32_t)l->nm ? l->m[k].user : 0); return 1; }
        if (!strcmp(op, "GetMemberAttributeCount")) { Member* m = member(l, rd(o, "ip", 1)); RET(m ? (uint32_t)m->na : 0); return 1; }
        uint64_t out = A3;
        if (!out) { RET(EOS_InvalidParameters); return 1; }
        if (!strcmp(op, "CopyInfo")) {
            static const char* const L = "ippiiiipiiiiipi";
            uint64_t in = alloc_struct(L);
            wr(in, L, 0, 3); wr(in, L, 1, l->idstr); wr(in, L, 2, l->owner); wr(in, L, 3, (uint64_t)l->perm);
            wr(in, L, 4, l->max > (uint32_t)l->nm ? l->max - (uint32_t)l->nm : 0); wr(in, L, 5, l->max); wr(in, L, 6, (uint64_t)l->invites);
            wr(in, L, 7, l->bucketstr); wr(in, L, 8, 1); wr(in, L, 12, 1);
            STP(out, in);
            RET(EOS_Success);
            return 1;
        }
        if (!strcmp(op, "CopyMemberInfo")) {
            Member* m = member(l, rd(o, "ip", 1));
            if (!m) { RET(EOS_NotFound); return 1; }
            uint64_t mi = alloc_struct("ipii");
            wr(mi, "ipii", 0, 1); wr(mi, "ipii", 1, m->user); wr(mi, "ipii", 3, 1);
            STP(out, mi);
            RET(EOS_Success);
            return 1;
        }
        const Attr* a = NULL;
        if (!strcmp(op, "CopyAttributeByIndex")) { uint32_t k = (uint32_t)rd(o, "ii", 1); if (k < (uint32_t)l->na) a = &l->a[k]; }
        else if (!strcmp(op, "CopyAttributeByKey") && rd(o, "ip", 1)) {
            for (int k = 0; k < l->na; k++) if (!strcmp(l->a[k].key, GSTR(rd(o, "ip", 1)))) a = &l->a[k];
        } else if (!strcmp(op, "CopyMemberAttributeByIndex")) {
            Member* m = member(l, rd(o, "ipi", 1));
            uint32_t k = (uint32_t)rd(o, "ipi", 2);
            if (m && k < (uint32_t)m->na) a = &m->a[k];
        } else if (!strcmp(op, "CopyMemberAttributeByKey")) {
            Member* m = member(l, rd(o, "ipp", 1));
            if (m && rd(o, "ipp", 2)) for (int k = 0; k < m->na; k++) if (!strcmp(m->a[k].key, GSTR(rd(o, "ipp", 2)))) a = &m->a[k];
        } else return 0;
        if (!a) { RET(EOS_NotFound); return 1; }
        STP(out, attr_copy(a));
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "Lobby_Attribute_Release")) { if (A1) g_free(A1); RET(0); return 1; }
    return 0;
}

/* ---- who the local user is: the WebWorks / Steam name, and the Steam id as the linked account ---- */

static const char* my_name(void)
{
    static char nm[128];
    if (!nm[0] && (ww_name("0", nm, sizeof nm) <= 0 || !nm[0])) snprintf(nm, sizeof nm, "Player");
    return nm;
}
static const char* my_account(int* type)
{
    static char acc[40];
    static int t;
    if (!acc[0]) {
        if (ww_self(acc, sizeof acc) > 0 && acc[0] && strcmp(acc, "0")) t = 1;   /* EOS_EAT_STEAM */
        else { snprintf(acc, sizeof acc, "%s", (const char*)G2H(eaid + 8)); t = 0; }   /* EOS_EAT_EPIC */
    }
    if (type) *type = t;
    return acc;
}
static uint64_t gdup(const char* s)
{
    uint64_t p = g_calloc(strlen(s) + 1);
    memcpy(G2H(p), s, strlen(s) + 1);
    return p;
}
/* EOS_Connect_ExternalAccountInfo for the local user */
static uint64_t account_info(int want_type)
{
    static const char* const L = "ipppiq";
    int t;
    const char* acc = my_account(&t);
    uint64_t s = alloc_struct(L);
    wr(s, L, 0, 1); wr(s, L, 1, puid); wr(s, L, 2, gdup(my_name()));
    wr(s, L, 3, gdup(acc)); wr(s, L, 4, (uint64_t)(want_type >= 0 ? want_type : t)); wr(s, L, 5, (uint64_t)-1);
    return s;
}

static int user_call(CPU* c, const char* n)
{
    if (!strcmp(n, "Connect_CopyProductUserInfo") || !strcmp(n, "Connect_CopyProductUserExternalAccountByIndex")
        || !strcmp(n, "Connect_CopyProductUserExternalAccountByAccountId")) {
        if (!A2 || rd(A2, "ip", 1) != puid || !A3 || (has(n, "ByIndex") && rd(A2, "ipi", 2))) { RET(EOS_NotFound); return 1; }
        STP(A3, account_info(-1));
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "Connect_CopyProductUserExternalAccountByAccountType")) {
        int t;
        my_account(&t);
        if (!A2 || rd(A2, "ipi", 1) != puid || !A3 || (int)rd(A2, "ipi", 2) != t) { RET(EOS_NotFound); return 1; }
        STP(A3, account_info(t));
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "Connect_GetProductUserExternalAccountCount")) { RET(A2 && rd(A2, "ip", 1) == puid ? 1 : 0); return 1; }
    if (!strcmp(n, "Connect_ExternalAccountInfo_Release")) {
        if (A1) { g_free(rd(A1, "ipppiq", 2)); g_free(rd(A1, "ipppiq", 3)); g_free(A1); }
        RET(0);
        return 1;
    }
    if (!strcmp(n, "Connect_GetProductUserIdMapping")) {
        /* (handle, {ApiVersion, LocalUserId, AccountIdType, TargetProductUserId}, out, in/out length) */
        if (!A2 || rd(A2, "ipip", 3) != puid) { RET(EOS_NotFound); return 1; }
        put_code(c, A3, A4, my_account(NULL));
        return 1;
    }
    if (!strcmp(n, "Connect_GetExternalAccountMapping")) {
        /* {ApiVersion, LocalUserId, AccountIdType, TargetExternalUserId}: the local user's own account maps to them */
        RET(A2 && rd(A2, "ipip", 3) && !strcmp(GSTR(rd(A2, "ipip", 3)), my_account(NULL)) ? puid : 0);
        return 1;
    }
    if (!strcmp(n, "UserInfo_CopyUserInfo")) {
        static const char* const L = "ipppppp";
        if (!A2 || !A3 || rd(A2, "ipp", 2) != eaid) { RET(EOS_NotFound); return 1; }
        uint64_t s = alloc_struct(L);
        wr(s, L, 0, 3); wr(s, L, 1, eaid); wr(s, L, 2, gdup("US")); wr(s, L, 3, gdup(my_name()));
        wr(s, L, 4, gdup("en")); wr(s, L, 5, gdup(my_name())); wr(s, L, 6, gdup(my_name()));
        STP(A3, s);
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "UserInfo_Release")) {
        if (A1) { for (int k = 2; k < 7; k++) g_free(rd(A1, "ipppppp", k)); g_free(A1); }
        RET(0);
        return 1;
    }
    if (!strcmp(n, "UserInfo_GetExternalUserInfoCount")) { RET(A2 && rd(A2, "ipp", 2) == eaid ? 1 : 0); return 1; }
    if (!strcmp(n, "UserInfo_CopyExternalUserInfoByIndex") || !strcmp(n, "UserInfo_CopyExternalUserInfoByAccountType")
        || !strcmp(n, "UserInfo_CopyExternalUserInfoByAccountId")) {
        static const char* const L = "iippp";   /* ApiVersion, AccountType, AccountId, DisplayName, DisplayNameSanitized */
        int t;
        const char* acc = my_account(&t);
        if (!A2 || !A3 || rd(A2, "ippi", 2) != eaid || (has(n, "ByIndex") && rd(A2, "ippi", 3))
            || (has(n, "AccountType") && (int)rd(A2, "ippi", 3) != t)) { RET(EOS_NotFound); return 1; }
        uint64_t s = alloc_struct(L);
        wr(s, L, 0, 2); wr(s, L, 1, (uint64_t)t); wr(s, L, 2, gdup(acc)); wr(s, L, 3, gdup(my_name())); wr(s, L, 4, gdup(my_name()));
        STP(A3, s);
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(n, "UserInfo_ExternalUserInfo_Release")) {
        if (A1) { for (int k = 2; k < 5; k++) g_free(rd(A1, "iippp", k)); g_free(A1); }
        RET(0);
        return 1;
    }
    if (!strcmp(n, "UserInfo_QueryUserInfo") || !strcmp(n, "UserInfo_QueryUserInfoByDisplayName")) {
        /* {ResultCode, ClientData, LocalUserId, TargetUserId ...}: the target is the local user */
        uint8_t* info = complete(A4, EOS_Success, A3);
        set_field(info, 0, eaid);
        set_field(info, 1, eaid);
        RET(0);
        return 1;
    }
    return 0;
}

/* ---- player data storage (cloud saves): the files live in the save folder, under EOSPlayerData ---- */

#define EOS_Canceled 17
#define EOS_RequestInProgress 39
#define EOS_UnexpectedError 0x7FFFFFFF

static void md5_hex(const uint8_t* d, size_t len, char out[33])
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1,
        0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453,
        0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942,
        0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d,
        0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t S[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t* m = (uint8_t*)calloc(total, 1);
    if (len) memcpy(m, d, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    memcpy(m + total - 8, &bits, 8);
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[16], a = h[0], b = h[1], cc = h[2], dd = h[3];
        memcpy(w, m + off, 64);
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) f = (b & cc) | (~b & dd), g = (uint32_t)i;
            else if (i < 32) f = (dd & b) | (~dd & cc), g = (5u * i + 1) & 15;
            else if (i < 48) f = b ^ cc ^ dd, g = (3u * i + 5) & 15;
            else f = cc ^ (b | ~dd), g = (7u * i) & 15;
            uint32_t t = dd, x = a + f + K[i] + w[g], s = S[(i / 16) * 4 + (i & 3)];
            dd = cc;
            cc = b;
            b += (x << s) | (x >> (32 - s));
            a = t;
        }
        h[0] += a, h[1] += b, h[2] += cc, h[3] += dd;
    }
    free(m);
    for (int i = 0; i < 16; i++) snprintf(out + 2 * i, 3, "%02x", ((uint8_t*)h)[i]);
}

static void pds_path(const char* file, char* out, size_t cap)
{
    snprintf(out, cap, "%s/EOSPlayerData/%s/%s", g_save[0] ? g_save : g_root, (const char*)G2H(puid + 8), file ? file : "");
}
static int pds_load(const char* file, uint8_t** data, uint32_t* len)
{
    char p[1400];
    pds_path(file, p, sizeof p);
    FILE* f = fopen(p, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    *data = (uint8_t*)malloc(n > 0 ? (size_t)n : 1);
    *len = n > 0 && fread(*data, 1, (size_t)n, f) == (size_t)n ? (uint32_t)n : 0;
    fclose(f);
    return 1;
}
static int pds_save(const char* file, const uint8_t* data, uint32_t len)
{
    char p[1400];
    pds_path(file, p, sizeof p);
    for (char* q = p + 1; *q; q++)
        if (*q == '/' || *q == '\\') { char k = *q; *q = 0; host_mkdir(p); *q = k; }
    FILE* f = fopen(p, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

/* the queried files, kept for CopyFileMetadata* as EOS keeps them */
typedef struct { char name[256]; uint32_t size; char md5[33]; uint64_t mtime; } PdsFile;
static PdsFile pds_files[64];
static int npds;

static PdsFile* pds_find(const char* file)
{
    for (int k = 0; k < npds; k++) if (!strcmp(pds_files[k].name, file)) return &pds_files[k];
    return NULL;
}
static void pds_note(const char* file)
{
    uint8_t* d = NULL;
    uint32_t len = 0;
    if (!pds_load(file, &d, &len)) return;
    PdsFile* e = pds_find(file);
    if (!e && npds < 64) e = &pds_files[npds++];
    if (e) {
        snprintf(e->name, sizeof e->name, "%s", file);
        e->size = len;
        md5_hex(d, len, e->md5);
        HostStat st;
        char p[1400];
        pds_path(file, p, sizeof p);
        e->mtime = host_stat(p, &st) ? st.mtime : 0;
    }
    free(d);
}
static void list_cb(void* ctx, const char* name, int is_dir, uint64_t size, uint64_t mtime)
{
    (void)ctx, (void)size, (void)mtime;
    if (!is_dir && name[0] != '.') pds_note(name);
}
static uint64_t pds_meta(const PdsFile* e)
{
    static const char* const L = "iippqi";
    uint64_t s = alloc_struct(L);
    wr(s, L, 0, 3); wr(s, L, 1, e->size); wr(s, L, 2, gdup(e->md5)); wr(s, L, 3, gdup(e->name));
    wr(s, L, 4, e->mtime ? e->mtime : (uint64_t)-1); wr(s, L, 5, e->size);
    return s;
}

/* reads and writes run on the next tick, chunk by chunk through the game's data callback, then complete */
typedef struct { int used, write, cancel, released; uint64_t handle, cd, done, data_fn, name; uint32_t chunk, state; } Transfer;
static Transfer xfers[16];

static uint32_t run_read(CPU* c, Transfer* t, uint64_t buf, uint32_t chunk)
{
    static const char* const L = "pppiiip";
    uint8_t* d = NULL;
    uint32_t len = 0, off = 0, result = EOS_Success;
    if (!pds_load(GSTR(t->name), &d, &len)) return EOS_NotFound;
    uint64_t info = alloc_struct(L);
    do {
        uint32_t n = len - off < chunk ? len - off : chunk;
        if (n) memcpy(G2H(buf), d + off, n);
        wr(info, L, 0, t->cd); wr(info, L, 1, puid); wr(info, L, 2, t->name); wr(info, L, 3, len);
        wr(info, L, 4, off + n >= len); wr(info, L, 5, n); wr(info, L, 6, buf);
        uint64_t a[1] = { info };
        uint32_t rr = t->data_fn ? (uint32_t)rt_invoke(c, t->data_fn, 1, a) : 1;   /* EOS_RR_ContinueReading */
        off += n;
        if (rr == 2) { result = EOS_UnexpectedError; break; }   /* FailRequest */
        if (rr == 3 || t->cancel) { result = EOS_Canceled; break; }
    } while (off < len);
    g_free(info);
    free(d);
    return result;
}
static uint32_t run_write(CPU* c, Transfer* t, uint64_t buf, uint32_t chunk)
{
    static const char* const L = "pppi";
    uint64_t info = alloc_struct(L), outlen = g_calloc(8);
    uint8_t* acc = NULL;
    uint32_t len = 0, result = EOS_Success;
    for (;;) {
        wr(info, L, 0, t->cd); wr(info, L, 1, puid); wr(info, L, 2, t->name); wr(info, L, 3, chunk);
        ST32(outlen, 0);
        uint64_t a[3] = { info, buf, outlen };
        uint32_t rr = t->data_fn ? (uint32_t)rt_invoke(c, t->data_fn, 3, a) : 2;
        uint32_t n = LD32(outlen) > chunk ? chunk : LD32(outlen);
        if (rr == 1 || rr == 2) {   /* ContinueWriting, CompleteRequest: this call's data counts */
            acc = (uint8_t*)realloc(acc, (size_t)len + n + 1);
            if (n) memcpy(acc + len, G2H(buf), n);
            len += n;
        }
        if (rr == 2) {
            if (!pds_save(GSTR(t->name), acc ? acc : (const uint8_t*)"", len)) result = EOS_UnexpectedError;
            else pds_note(GSTR(t->name));
            break;
        }
        if (rr != 1 || t->cancel) { result = rr == 4 || t->cancel ? EOS_Canceled : EOS_UnexpectedError; break; }
    }
    free(acc);
    g_free(info);
    g_free(outlen);
    return result;
}
static void run_transfers(CPU* c)
{
    for (int k = 0; k < 16; k++) {
        Transfer* t = &xfers[k];
        if (!t->used || t->state != 1) continue;
        t->state = 2;
        uint32_t chunk = t->chunk ? t->chunk : 4096;
        uint64_t buf = g_calloc(chunk + 16);
        uint32_t result = t->write ? run_write(c, t, buf, chunk) : run_read(c, t, buf, chunk);
        g_free(buf);
        t->state = result == EOS_Success ? 3 : 4;
        uint64_t v[4] = { result, t->cd, puid, t->name };
        post_info(t->done, "ippp", v);
        if (eos_dbg()) host_log("[eos] %s %s: %u", t->write ? "wrote" : "read", GSTR(t->name), result);
        if (t->released) { g_free(t->name); g_free(t->handle); t->used = 0; }
    }
}

static uint64_t start_transfer(int write, uint64_t opts, uint64_t cd, uint64_t done)
{
    static const char* const L = "ippipp";   /* ApiVersion, LocalUserId, Filename, ChunkLengthBytes, data callback, progress callback */
    for (int k = 0; k < 16; k++) {
        Transfer* t = &xfers[k];
        if (t->used) continue;
        memset(t, 0, sizeof *t);
        t->used = 1, t->write = write, t->cd = cd, t->done = done, t->state = 1;
        t->name = gdup(opts && rd(opts, L, 2) ? GSTR(rd(opts, L, 2)) : "");
        t->chunk = opts ? (uint32_t)rd(opts, L, 3) : 0;
        t->data_fn = opts ? rd(opts, L, 4) : 0;
        t->handle = g_calloc(16);
        ST32(t->handle, 0x5EED7F);
        ST32(t->handle + 4, (uint32_t)k);
        return t->handle;
    }
    uint64_t v[4] = { EOS_LimitExceeded, cd, puid, 0 };
    post_info(done, "ippp", v);
    return 0;
}
static Transfer* transfer_of(uint64_t h) { return h && LD32(h) == 0x5EED7F && LD32(h + 4) < 16 && xfers[LD32(h + 4)].used ? &xfers[LD32(h + 4)] : NULL; }

static const char* opt_str(uint64_t o, const char* L, int i) { return o && rd(o, L, i) ? GSTR(rd(o, L, i)) : ""; }

static int pds_call(CPU* c, const char* n)
{
    const char* op = n + 18;   /* after "PlayerDataStorage_" */
    if (!strcmp(op, "QueryFile")) {
        pds_note(opt_str(A2, "ipp", 2));
        uint64_t v[3] = { pds_find(opt_str(A2, "ipp", 2)) ? EOS_Success : EOS_NotFound, A3, puid };
        post_info(A4, "ipp", v);
        RET(0);
        return 1;
    }
    if (!strcmp(op, "QueryFileList")) {
        char p[1400];
        npds = 0;
        pds_path(NULL, p, sizeof p);
        host_list_dir(p, list_cb, NULL);
        uint64_t v[4] = { EOS_Success, A3, puid, (uint64_t)npds };
        post_info(A4, "ippi", v);
        RET(0);
        return 1;
    }
    if (!strcmp(op, "GetFileMetadataCount")) { if (A3) ST32(A3, (uint32_t)npds); RET(EOS_Success); return 1; }
    if (!strcmp(op, "CopyFileMetadataAtIndex")) {
        uint32_t i = A2 ? (uint32_t)rd(A2, "ipi", 2) : 0;
        if (i >= (uint32_t)npds || !A3) { RET(EOS_NotFound); return 1; }
        STP(A3, pds_meta(&pds_files[i]));
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(op, "CopyFileMetadataByFilename")) {
        PdsFile* e = pds_find(opt_str(A2, "ipp", 2));
        if (!e || !A3) { RET(EOS_NotFound); return 1; }
        STP(A3, pds_meta(e));
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(op, "FileMetadata_Release")) {
        if (A1) { g_free(rd(A1, "iippqi", 2)); g_free(rd(A1, "iippqi", 3)); g_free(A1); }
        RET(0);
        return 1;
    }
    if (!strcmp(op, "DeleteFile")) {
        char p[1400];
        const char* file = opt_str(A2, "ipp", 2);
        pds_path(file, p, sizeof p);
        int ok = host_remove(p);
        PdsFile* e = pds_find(file);
        if (e) *e = pds_files[--npds];
        uint64_t v[3] = { ok ? EOS_Success : EOS_NotFound, A3, puid };
        post_info(A4, "ipp", v);
        RET(0);
        return 1;
    }
    if (!strcmp(op, "DuplicateFile")) {
        uint8_t* d = NULL;
        uint32_t len = 0, r = EOS_NotFound;
        const char *src = opt_str(A2, "ippp", 2), *dst = opt_str(A2, "ippp", 3);
        if (src[0] && dst[0] && pds_load(src, &d, &len)) {
            r = pds_save(dst, d, len) ? EOS_Success : EOS_UnexpectedError;
            pds_note(dst);
            free(d);
        }
        uint64_t v[3] = { r, A3, puid };
        post_info(A4, "ipp", v);
        RET(0);
        return 1;
    }
    if (!strcmp(op, "DeleteCache")) { uint64_t v[3] = { EOS_Success, A3, puid }; post_info(A4, "ipp", v); RET(0); return 1; }
    if (!strcmp(op, "ReadFile") || !strcmp(op, "WriteFile")) { RET(start_transfer(op[0] == 'W', A2, A3, A4)); return 1; }
    if (!strcmp(op, "FileTransferRequest_GetFileRequestState")) {
        Transfer* t = transfer_of(A1);
        RET(!t ? EOS_InvalidParameters : t->state == 3 ? EOS_Success : t->state == 4 ? EOS_UnexpectedError : EOS_RequestInProgress);
        return 1;
    }
    if (!strcmp(op, "FileTransferRequest_GetFilename")) {
        /* (request, buffer size, out, out length) */
        Transfer* t = transfer_of(A1);
        if (!t) { RET(EOS_InvalidParameters); return 1; }
        const char* s = GSTR(t->name);
        uint32_t len = (uint32_t)strlen(s);
        if (A4) ST32(A4, len + 1);
        if (!A3 || (uint32_t)A2 < len + 1) { RET(EOS_LimitExceeded); return 1; }
        memcpy(G2H(A3), s, len + 1);
        RET(EOS_Success);
        return 1;
    }
    if (!strcmp(op, "FileTransferRequest_CancelRequest")) {
        Transfer* t = transfer_of(A1);
        if (t) t->cancel = 1;
        RET(t ? EOS_Success : EOS_NotFound);
        return 1;
    }
    if (!strcmp(op, "FileTransferRequest_Release")) {
        Transfer* t = transfer_of(A1);
        if (t && t->state >= 3) { g_free(t->name); g_free(t->handle); t->used = 0; }
        else if (t) t->released = 1;   /* releasing doesn't cancel: it finishes, then goes */
        RET(0);
        return 1;
    }
    return 0;
}

static void eos_call(CPU* c, int i)
{
    const EosFn* f = &eos_fns[i];
    const char* n = f->name + 4;   /* after "EOS_" */
    uint64_t r = 0;
    make_ids();

    if (!strcmp(n, "Initialize") || !strcmp(n, "Shutdown") || starts(n, "Logging_")) r = EOS_Success;
    else if (!strcmp(n, "Platform_Create")) { if (!platform) platform = handle_for("platform"); r = platform; }
    else if (!strcmp(n, "Platform_Tick")) { run_transfers(c); run_queue(c); RET(0); return; }
    else if (!strcmp(n, "Platform_Release")) r = 0;
    else if (starts(n, "Platform_Get") && has(n, "Interface")) r = handle_for(n + 12);
    else if (!strcmp(n, "Platform_GetActiveCountryCode")) { put_code(c, A3, A4, "US"); goto log; }
    else if (!strcmp(n, "Platform_GetActiveLocaleCode")) { put_code(c, A3, A4, "en"); goto log; }
    else if (!strcmp(n, "Platform_GetOverrideCountryCode")) { put_code(c, A2, A3, "US"); goto log; }
    else if (!strcmp(n, "Platform_GetOverrideLocaleCode")) { put_code(c, A2, A3, "en"); goto log; }
    else if (!strcmp(n, "Platform_GetApplicationStatus")) r = 2;   /* foreground */
    else if (!strcmp(n, "Platform_GetNetworkStatus")) r = 2;       /* online */
    else if (!strcmp(n, "Platform_CheckForLauncherAndRestart")) r = 20;   /* EOS_NoChange: no relaunch */
    else if (!strcmp(n, "ProductUserId_IsValid") || !strcmp(n, "EpicAccountId_IsValid")) r = id_valid(A1);
    else if (!strcmp(n, "ProductUserId_ToString") || !strcmp(n, "EpicAccountId_ToString")) r = id_to_string(A1, A2, A3);
    else if (!strcmp(n, "ProductUserId_FromString") || !strcmp(n, "EpicAccountId_FromString")) r = id_from_string(A1);
    else if (!strcmp(n, "EResult_ToString")) {
        static const char* const names[] = { "EOS_Success", "EOS_NoConnection", "EOS_InvalidCredentials", "EOS_InvalidUser", "EOS_InvalidAuth" };
        r = gstr_cached((uint32_t)A1 < 5 ? names[A1] : "EOS_Unknown");
    } else if (!strcmp(n, "EResult_IsOperationComplete")) r = 1;
    else if (!strcmp(n, "ByteArray_ToString")) {
        /* (bytes, length, out, in/out length): lowercase hex */
        uint32_t len = (uint32_t)A2, cap = A4 ? LD32(A4) : 0;
        if (A4) ST32(A4, 2 * len + 1);
        if (!A3 || cap < 2 * len + 1) r = EOS_LimitExceeded;
        else {
            for (uint32_t k = 0; k < len; k++) snprintf((char*)G2H(A3 + 2ull * k), 3, "%02x", LD8(A1 + k));
            ST8(A3 + 2ull * len, 0);
            r = EOS_Success;
        }
    }
    /* Connect: the product user signs in (or is created) at once */
    else if (!strcmp(n, "Connect_Login") || !strcmp(n, "Connect_CreateUser")) {
        connect_in = 1;
        set_field(complete(arg(c, f->cb), EOS_Success, arg(c, f->cd)), 0, puid);
        RET(0);
        goto log;
    } else if (!strcmp(n, "Connect_GetLoggedInUsersCount")) r = (uint64_t)connect_in;
    else if (!strcmp(n, "Connect_GetLoggedInUserByIndex")) r = connect_in && A2 == 0 ? puid : 0;
    else if (!strcmp(n, "Connect_GetLoginStatus")) r = connect_in && id_valid(A2) ? 2 : 0;
    /* Auth: the Epic account signs in at once */
    else if (!strcmp(n, "Auth_Login")) {
        auth_in = 1;
        set_field(complete(arg(c, f->cb), EOS_Success, arg(c, f->cd)), 0, eaid);
        RET(0);
        goto log;
    } else if (!strcmp(n, "Auth_GetLoggedInAccountsCount")) r = (uint64_t)auth_in;
    else if (!strcmp(n, "Auth_GetLoggedInAccountByIndex")) r = auth_in && A2 == 0 ? eaid : 0;
    else if (!strcmp(n, "Auth_GetLoginStatus")) r = auth_in && id_valid(A2) ? 2 : 0;
    else if ((starts(n, "Connect_") || starts(n, "UserInfo_")) && user_call(c, n)) goto log;
    else if (starts(n, "PlayerDataStorage_") && pds_call(c, n)) goto log;
    else if ((starts(n, "Lobby") || starts(n, "LobbyDetails_") || starts(n, "LobbyModification_")) && lobby_call(c, f, n)) goto log;
    else if (f->ret == EOS_R_NOTIFY) {
        /* AddNotify*: kept, so the event it watches can call it */
        static uint64_t next_notify = 0x5EED0000;
        r = ++next_notify;
        const char* what = strstr(n, "AddNotify");
        if (what && f->cb != 255 && nnotes < 128) {
            snprintf(notes[nnotes].what, sizeof notes[nnotes].what, "%s", what + 9);
            notes[nnotes].id = r;
            notes[nnotes].fn = arg(c, f->cb);
            notes[nnotes].cd = f->cd != 255 ? arg(c, f->cd) : 0;
            nnotes++;
        }
    } else if (strstr(n, "RemoveNotify")) {
        uint64_t id = RT_G32 ? (uint32_t)A2 | (uint64_t)(uint32_t)A3 << 32 : A2;
        for (int k = 0; k < nnotes; k++) if (notes[k].id == id) { notes[k] = notes[--nnotes]; break; }
        r = 0;
    } else if (f->cb != 255) {
        /* asynchronous: nothing on a server to join here, and no cloud or title files to read; everything else
           completes empty. Calls that also hand back a request (file transfers) get a handle that answers empty. */
        int fails = (starts(n, "Lobby_") && has(n, "JoinLobby")) || (starts(n, "Sessions_") && has(n, "JoinSession"));
        int missing = starts(n, "TitleStorage_") && has(n, "ReadFile");
        uint8_t* info = complete(arg(c, f->cb), fails ? EOS_NoConnection : missing ? EOS_NotFound : EOS_Success, arg(c, f->cd));
        /* logins and user lookups name the local user in their first field */
        if (info && (has(n, "Logout") || has(n, "Query") || has(n, "Request") || missing)) set_field(info, 0, starts(n, "Auth_") ? eaid : puid);
        RET(f->ret == EOS_R_PTR ? handle_for(n) : 0);
        goto log;
    } else {
        static uint64_t next_notify = 0x5EED0000;
        const char* op = strchr(n, '_') ? strchr(n, '_') + 1 : n;
        switch (f->ret) {
        case EOS_R_RESULT:
            r = starts(op, "Copy") || starts(op, "Get") ? EOS_NotFound : EOS_Success;
            /* Create*Search/Modification and the like hand back a handle: one that answers empty */
            if (starts(op, "Create") && f->nargs >= 3) STP(arg(c, f->nargs - 1), handle_for(n));
            break;
        case EOS_R_NOTIFY: r = ++next_notify; break;
        case EOS_R_PTR: r = has(op, "Interface") ? handle_for(n) : 0; break;   /* sub-interfaces (RTC audio, ...) */
        default: r = 0; break;
        }
    }
    RET(r);
log:
    if (eos_dbg()) {
        static uint8_t said[EOS_FN_COUNT];
        if (!said[i]) said[i] = 1, host_log("[eos] %s -> %llx", f->name, (unsigned long long)R(0));
    }
}

#define X(i) static void eos_e##i(CPU* c) { eos_call(c, i); }
EOS_ENTRIES
#undef X
#define X(i) eos_e##i,
static const GuestFn eos_entries[EOS_FN_COUNT] = { EOS_ENTRIES };
#undef X

/* GetProcAddress on the EOS module: each function's entry point (32-bit: it pops its stdcall arguments) */
uint64_t eos_proc(const char* name)
{
    int lo = 0, hi = EOS_FN_COUNT - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2, d = strcmp(eos_fns[m].name, name);
        if (!d) return rt_dyn_thunk_n(eos_entries[m], eos_fns[m].name, RT_G32 ? eos_fns[m].pops / 4 : 0);
        if (d < 0) lo = m + 1;
        else hi = m - 1;
    }
    return 0;
}
