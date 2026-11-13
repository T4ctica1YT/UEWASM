#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>
#ifdef _MSC_VER
#include <intrin.h>
#endif

typedef union {
    uint8_t b[16];
    int8_t sb[16];
    uint16_t w[8];
    int16_t sw[8];
    uint32_t d[4];
    int32_t sd[4];
    uint64_t q[2];
    int64_t sq[2];
    float f[4];
    double g[2];
} XMM;

typedef struct Flags {
    uint64_t fa, fb, fr, fcin, rawfl;
    int fop, fsz;
} Flags;

typedef struct CPU {
    uint64_t r[16];
    XMM x[16];
    uint32_t mxcsr;
    uint64_t gsbase;
    int tid;
    /* x87 (32-bit guests): the register stack as doubles, top index, status and control words */
    double st[8];
    uint32_t ftop, fsw, fcw;
    uint32_t df;   /* direction flag: string instructions step backwards */
} CPU;

typedef void (*GuestFn)(CPU* __restrict);
typedef struct { uint64_t addr; GuestFn fn; } FnEntry;

extern uint8_t* MEM;
extern uint64_t MEM_SIZE;

#ifdef LIFTED
/* lifted functions keep the guest registers in locals (rN_); build.py writes them back to the CPU struct
   before anything that can see it and reloads them after */
#define R(i) r##i##_
#else
#define R(i) (c->r[i])
#endif
#define G2H(a) (MEM + (uint32_t)(a))

#ifdef LIFTED
#define MEMB mem_
#else
#define MEMB MEM
#endif
#ifdef UEW_THREADS
/* guest threads on several cores: every guest access is a volatile one, so the compiler keeps x86's order of loads and
   stores (code that publishes data with a plain store of a flag relies on it) and never caches memory in registers */
typedef uint16_t __attribute__((aligned(1), may_alias)) u16u_;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u_;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u_;
static inline uint64_t ld8_(uint8_t* m, uint64_t a) { return *(volatile uint8_t*)(m + (uint32_t)a); }
static inline uint64_t ld16_(uint8_t* m, uint64_t a) { return *(volatile u16u_*)(m + (uint32_t)a); }
static inline uint64_t ld32_(uint8_t* m, uint64_t a) { return *(volatile u32u_*)(m + (uint32_t)a); }
static inline uint64_t ld64_(uint8_t* m, uint64_t a) { return *(volatile u64u_*)(m + (uint32_t)a); }
static inline void st8_(uint8_t* m, uint64_t a, uint64_t v) { *(volatile uint8_t*)(m + (uint32_t)a) = (uint8_t)v; }
static inline void st16_(uint8_t* m, uint64_t a, uint64_t v) { *(volatile u16u_*)(m + (uint32_t)a) = (uint16_t)v; }
static inline void st32_(uint8_t* m, uint64_t a, uint64_t v) { *(volatile u32u_*)(m + (uint32_t)a) = (uint32_t)v; }
static inline void st64_(uint8_t* m, uint64_t a, uint64_t v) { *(volatile u64u_*)(m + (uint32_t)a) = v; }
static inline XMM xld_(uint8_t* m, uint64_t a, int n)
{
    XMM x; memset(&x, 0, 16);
    if (n == 16) { x.q[0] = ld64_(m, a); x.q[1] = ld64_(m, a + 8); }
    else if (n == 8) x.q[0] = ld64_(m, a);
    else if (n == 4) x.d[0] = (uint32_t)ld32_(m, a);
    else for (int i = 0; i < n; i++) x.b[i] = (uint8_t)ld8_(m, a + i);
    return x;
}
static inline void xst_(uint8_t* m, uint64_t a, XMM x, int n)
{
    if (n == 16) { st64_(m, a, x.q[0]); st64_(m, a + 8, x.q[1]); }
    else if (n == 8) st64_(m, a, x.q[0]);
    else if (n == 4) st32_(m, a, x.d[0]);
    else for (int i = 0; i < n; i++) st8_(m, a + i, x.b[i]);
}
#else
static inline uint64_t ld8_(uint8_t* m, uint64_t a) { return m[(uint32_t)a]; }
static inline uint64_t ld16_(uint8_t* m, uint64_t a) { uint16_t v; memcpy(&v, m + (uint32_t)a, 2); return v; }
static inline uint64_t ld32_(uint8_t* m, uint64_t a) { uint32_t v; memcpy(&v, m + (uint32_t)a, 4); return v; }
static inline uint64_t ld64_(uint8_t* m, uint64_t a) { uint64_t v; memcpy(&v, m + (uint32_t)a, 8); return v; }
static inline void st8_(uint8_t* m, uint64_t a, uint64_t v) { m[(uint32_t)a] = (uint8_t)v; }
static inline void st16_(uint8_t* m, uint64_t a, uint64_t v) { uint16_t t = (uint16_t)v; memcpy(m + (uint32_t)a, &t, 2); }
static inline void st32_(uint8_t* m, uint64_t a, uint64_t v) { uint32_t t = (uint32_t)v; memcpy(m + (uint32_t)a, &t, 4); }
static inline void st64_(uint8_t* m, uint64_t a, uint64_t v) { memcpy(m + (uint32_t)a, &v, 8); }
static inline XMM xld_(uint8_t* m, uint64_t a, int n) { XMM x; memset(&x, 0, 16); memcpy(&x, m + (uint32_t)a, n); return x; }
static inline void xst_(uint8_t* m, uint64_t a, XMM x, int n) { memcpy(m + (uint32_t)a, &x, n); }
#endif
#define LD8(a) ld8_(MEMB, (a))
#define LD16(a) ld16_(MEMB, (a))
#define LD32(a) ld32_(MEMB, (a))
#define LD64(a) ld64_(MEMB, (a))
#if defined(UEW_STORE_CHECK) && defined(LIFTED)
/* debug builds: stores with high address bits (a fault on Windows, wrapped here) or into read-only image ranges */
void rt_bad_store(uint64_t a, const char* fn);
#define SCHK(a) (((((uint64_t)(a) - UEW_RO_LO) < (UEW_RO_HI - UEW_RO_LO)) | ((uint64_t)(a) >> 32 != 0)) ? rt_bad_store((a), __func__) : (void)0)
#define ST8(a, v) (SCHK(a), st8_(MEMB, (a), (v)))
#define ST16(a, v) (SCHK(a), st16_(MEMB, (a), (v)))
#define ST32(a, v) (SCHK(a), st32_(MEMB, (a), (v)))
#define ST64(a, v) (SCHK(a), st64_(MEMB, (a), (v)))
#elif defined(UEW_PTRCHECK) && defined(LIFTED)
/* debug builds: 64-bit stores of values shaped like a guest pointer with a stray bit 32 (64 KB aligned, 4-8 GB) */
void rt_ptr_store(uint64_t a, uint64_t v, const char* fn);
static inline uint64_t pchk_(uint64_t a, uint64_t v, const char* fn) { if ((v >> 32) == 1 && !(v & 0xFFFF)) rt_ptr_store(a, v, fn); return v; }
#define ST8(a, v) st8_(MEMB, (a), (v))
#define ST16(a, v) st16_(MEMB, (a), (v))
#define ST32(a, v) st32_(MEMB, (a), (v))
#define ST64(a, v) st64_(MEMB, (a), pchk_((a), (v), __func__))
#else
#define ST8(a, v) st8_(MEMB, (a), (v))
#define ST16(a, v) st16_(MEMB, (a), (v))
#define ST32(a, v) st32_(MEMB, (a), (v))
#define ST64(a, v) st64_(MEMB, (a), (v))
#endif
#define XLD(a, n) xld_(MEMB, (a), (n))
#if defined(UEW_STORE_CHECK) && defined(LIFTED)
#define XST(a, x, n) (SCHK(a), xst_(MEMB, (a), (x), (n)))
#else
#define XST(a, x, n) xst_(MEMB, (a), (x), (n))
#endif

/* locked instructions: one lock per address stripe when guest threads run on several cores, nothing otherwise */
#ifdef UEW_THREADS
void rt_atomic_begin(uint64_t a);
void rt_atomic_end(uint64_t a);
#define ATOMIC_BEGIN(a) rt_atomic_begin(a)
#define ATOMIC_END(a) rt_atomic_end(a)
#else
#define ATOMIC_BEGIN(a) ((void)0)
#define ATOMIC_END(a) ((void)0)
#endif

/* locked xchg / cmpxchg / xadd / or / and / xor on 1-8 bytes, returning the old value. Real threads use the hardware's
   atomics when the address is aligned (wasm traps on unaligned ones) and the address-stripe lock otherwise */
#ifdef UEW_THREADS
#define AT_OP(N, T, name, body_atomic, body_plain) \
static inline uint64_t at_##name##N(uint8_t* m, uint64_t a, uint64_t x, uint64_t y) { \
    T* p = (T*)(m + (uint32_t)a); (void)y; \
    if (!((uint32_t)a & (sizeof(T) - 1))) return (uint64_t)(body_atomic); \
    rt_atomic_begin(a); T o; memcpy(&o, p, sizeof o); { T n_ = (body_plain); memcpy(p, &n_, sizeof n_); } rt_atomic_end(a); return o; }
#else
#define AT_OP(N, T, name, body_atomic, body_plain) \
static inline uint64_t at_##name##N(uint8_t* m, uint64_t a, uint64_t x, uint64_t y) { \
    T* p = (T*)(m + (uint32_t)a); (void)y; T o; memcpy(&o, p, sizeof o); { T n_ = (body_plain); memcpy(p, &n_, sizeof n_); } return o; }
#endif
#define AT_ALL(N, T) \
    AT_OP(N, T, xchg, __atomic_exchange_n(p, (T)x, __ATOMIC_SEQ_CST), (T)x) \
    AT_OP(N, T, add, __atomic_fetch_add(p, (T)x, __ATOMIC_SEQ_CST), (T)(o + (T)x)) \
    AT_OP(N, T, or, __atomic_fetch_or(p, (T)x, __ATOMIC_SEQ_CST), (T)(o | (T)x)) \
    AT_OP(N, T, and, __atomic_fetch_and(p, (T)x, __ATOMIC_SEQ_CST), (T)(o & (T)x)) \
    AT_OP(N, T, xor, __atomic_fetch_xor(p, (T)x, __ATOMIC_SEQ_CST), (T)(o ^ (T)x)) \
    AT_OP(N, T, cas, ({ T e_ = (T)x; __atomic_compare_exchange_n(p, &e_, (T)y, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); e_; }), (o == (T)x ? (T)y : o))
AT_ALL(8, uint8_t)
AT_ALL(16, uint16_t)
AT_ALL(32, uint32_t)
AT_ALL(64, uint64_t)
#define ATXCHG8(a, v) at_xchg8(MEMB, (a), (v), 0)
#define ATXCHG16(a, v) at_xchg16(MEMB, (a), (v), 0)
#define ATXCHG32(a, v) at_xchg32(MEMB, (a), (v), 0)
#define ATXCHG64(a, v) at_xchg64(MEMB, (a), (v), 0)
#define ATADD8(a, v) at_add8(MEMB, (a), (v), 0)
#define ATADD16(a, v) at_add16(MEMB, (a), (v), 0)
#define ATADD32(a, v) at_add32(MEMB, (a), (v), 0)
#define ATADD64(a, v) at_add64(MEMB, (a), (v), 0)
#define ATOR8(a, v) at_or8(MEMB, (a), (v), 0)
#define ATAND8(a, v) at_and8(MEMB, (a), (v), 0)
#define ATXOR8(a, v) at_xor8(MEMB, (a), (v), 0)
#define ATCAS8(a, e, n) at_cas8(MEMB, (a), (e), (n))
#define ATCAS16(a, e, n) at_cas16(MEMB, (a), (e), (n))
#define ATCAS32(a, e, n) at_cas32(MEMB, (a), (e), (n))
#define ATCAS64(a, e, n) at_cas64(MEMB, (a), (e), (n))
#define ATCASOK8(a, e, n) (at_cas8(MEMB, (a), (e), (n)) == (uint8_t)(e))
#define ATCASOK16(a, e, n) (at_cas16(MEMB, (a), (e), (n)) == (uint16_t)(e))
#define ATCASOK32(a, e, n) (at_cas32(MEMB, (a), (e), (n)) == (uint32_t)(e))
#define ATCASOK64(a, e, n) (at_cas64(MEMB, (a), (e), (n)) == (uint64_t)(e))

#define XLANE8(x, i) ((uint64_t)(x).b[i])
#define XLANE16(x, i) ((uint64_t)(x).w[i])
#define XLANE32(x, i) ((uint64_t)(x).d[i])
#define XLANE64(x, i) ((x).q[i])
#define XSETLANE8(x, i, v) ((x).b[i] = (uint8_t)(v))
#define XSETLANE16(x, i, v) ((x).w[i] = (uint16_t)(v))
#define XSETLANE32(x, i, v) ((x).d[i] = (uint32_t)(v))
#define XSETLANE64(x, i, v) ((x).q[i] = (uint64_t)(v))

/* functions run before main */
#ifdef _MSC_VER
#pragma section(".CRT$XCU", read)
#define INIT_FN(f) static void f(void); __declspec(allocate(".CRT$XCU")) void (*f##_init_)(void) = f; static void f(void)
#else
#define INIT_FN(f) __attribute__((constructor)) static void f(void)
#endif

/* flag helpers must inline: next to the instruction that set the flags they fold to a plain compare */
#ifdef _MSC_VER
#define FLAGFN static __forceinline
#else
#define FLAGFN static inline __attribute__((always_inline))
#endif

enum { F_ADD, F_SUB, F_LOGIC, F_INC, F_DEC, F_SHL, F_SHR, F_SAR, F_MUL, F_ADC, F_SBB, F_RAW };
#define FL(op, sz, a, b, r) (F.fop = (op), F.fsz = (sz), F.fa = (a), F.fb = (b), F.fr = (r))

FLAGFN uint64_t SEXT(uint64_t v, int sz) { return sz == 64 ? v : (uint64_t)(((int64_t)(v << (64 - sz))) >> (64 - sz)); }
FLAGFN int MSB(uint64_t v, int sz) { return (int)((v >> (sz - 1)) & 1); }

FLAGFN int CF(Flags* c)
{
    switch (c->fop) {
    case F_ADD: return c->fr < c->fa;
    case F_ADC: return c->fr < c->fa || (c->fcin && c->fr == c->fa);
    case F_SUB: return c->fa < c->fb;
    case F_SBB: return c->fa < c->fb || (c->fcin && c->fa == c->fb);
    case F_LOGIC: return 0;
    case F_INC: case F_DEC: case F_MUL: return (int)c->fcin;
    case F_SHL: return c->fb <= (uint64_t)c->fsz ? (int)((c->fa >> (c->fsz - c->fb)) & 1) : 0;
    case F_SHR: return (int)((c->fa >> (c->fb - 1)) & 1);
    case F_SAR: return (int)(((int64_t)SEXT(c->fa, c->fsz) >> (c->fb > 63 ? 63 : c->fb - 1)) & 1);
    default: return (int)(c->rawfl & 1);
    }
}

FLAGFN int ZF(Flags* c) { return c->fop == F_RAW ? (int)((c->rawfl >> 6) & 1) : c->fr == 0; }
FLAGFN int SF(Flags* c) { return c->fop == F_RAW ? (int)((c->rawfl >> 7) & 1) : MSB(c->fr, c->fsz); }

FLAGFN int OF(Flags* c)
{
    int s = c->fsz;
    switch (c->fop) {
    case F_ADD: case F_ADC: return MSB((c->fa ^ c->fr) & (c->fb ^ c->fr), s);
    case F_SUB: case F_SBB: return MSB((c->fa ^ c->fb) & (c->fa ^ c->fr), s);
    case F_LOGIC: case F_SAR: return 0;
    case F_INC: return c->fr == (1ull << (s - 1));
    case F_DEC: return c->fa == (1ull << (s - 1));
    case F_SHL: return MSB(c->fr, s) ^ CF(c);
    case F_SHR: return MSB(c->fa, s);
    case F_MUL: return (int)c->fcin;
    default: return (int)((c->rawfl >> 11) & 1);
    }
}

FLAGFN int PF(Flags* c)
{
    if (c->fop == F_RAW) return (int)((c->rawfl >> 2) & 1);
    uint8_t v = (uint8_t)c->fr;
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return !(v & 1);
}

#define CC_O(c) OF(c)
#define CC_NO(c) (!OF(c))
#define CC_B(c) CF(c)
#define CC_AE(c) (!CF(c))
#define CC_E(c) ZF(c)
#define CC_NE(c) (!ZF(c))
#define CC_BE(c) (CF(c) || ZF(c))
#define CC_A(c) (!CF(c) && !ZF(c))
#define CC_S(c) SF(c)
#define CC_NS(c) (!SF(c))
#define CC_P(c) PF(c)
#define CC_NP(c) (!PF(c))
FLAGFN int CC_L(Flags* c) { if (c->fop == F_SUB) return (int64_t)SEXT(c->fa, c->fsz) < (int64_t)SEXT(c->fb, c->fsz); return SF(c) != OF(c); }
FLAGFN int CC_GE(Flags* c) { return !CC_L(c); }
FLAGFN int CC_LE(Flags* c) { if (c->fop == F_SUB) return (int64_t)SEXT(c->fa, c->fsz) <= (int64_t)SEXT(c->fb, c->fsz); return ZF(c) || SF(c) != OF(c); }
FLAGFN int CC_G(Flags* c) { return !CC_LE(c); }

FLAGFN uint64_t rt_flags(Flags* c)
{
    uint64_t af = c->fop == F_RAW ? (c->rawfl & 0x10) : ((c->fa ^ c->fb ^ c->fr) & 0x10);
    return (uint64_t)CF(c) | 2 | ((uint64_t)PF(c) << 2) | af | ((uint64_t)ZF(c) << 6) | ((uint64_t)SF(c) << 7) | ((uint64_t)OF(c) << 11);
}
FLAGFN void rt_setflags(Flags* c, uint64_t v) { c->rawfl = v; c->fop = F_RAW; }
FLAGFN void rt_setcf(Flags* c, int v) { rt_setflags(c, (rt_flags(c) & ~1ull) | (uint64_t)(v & 1)); }
FLAGFN void rt_setzf(Flags* c, int v) { rt_setflags(c, (rt_flags(c) & ~0x40ull) | ((uint64_t)(v & 1) << 6)); }
FLAGFN void rt_setcfzf(Flags* c, int cf, int zf) { rt_setflags(c, (rt_flags(c) & ~0x41ull) | (uint64_t)cf | ((uint64_t)zf << 6)); }
static inline void rt_rotflags(Flags* c, uint64_t r, int sz, int left)
{
    int cf = left ? (int)(r & 1) : MSB(r, sz);
    int of = left ? (MSB(r, sz) ^ cf) : (MSB(r, sz) ^ (int)((r >> (sz - 2)) & 1));
    rt_setflags(c, (rt_flags(c) & ~0x801ull) | (uint64_t)cf | ((uint64_t)of << 11));
}
static inline void rt_ucomi(Flags* c, double a, double b)
{
    uint64_t f = 2;
    if (a != a || b != b) f |= 0x45;
    else if (a < b) f |= 1;
    else if (a == b) f |= 0x40;
    rt_setflags(c, f);
}
static inline void rt_ptest(Flags* c, XMM d, XMM s)
{
    int zf = ((d.q[0] & s.q[0]) | (d.q[1] & s.q[1])) == 0;
    int cf = ((~d.q[0] & s.q[0]) | (~d.q[1] & s.q[1])) == 0;
    rt_setflags(c, 2 | (uint64_t)cf | ((uint64_t)zf << 6));
}

#define ROL8(v, n) ((uint64_t)(uint8_t)(((uint8_t)(v) << (n)) | ((uint8_t)(v) >> ((8 - (n)) & 7))))
#define ROR8(v, n) ((uint64_t)(uint8_t)(((uint8_t)(v) >> (n)) | ((uint8_t)(v) << ((8 - (n)) & 7))))
#define ROL16(v, n) ((uint64_t)(uint16_t)(((uint16_t)(v) << (n)) | ((uint16_t)(v) >> ((16 - (n)) & 15))))
#define ROR16(v, n) ((uint64_t)(uint16_t)(((uint16_t)(v) >> (n)) | ((uint16_t)(v) << ((16 - (n)) & 15))))
#define ROL32(v, n) ((uint64_t)(uint32_t)(((uint32_t)(v) << (n)) | ((uint32_t)(v) >> ((32 - (n)) & 31))))
#define ROR32(v, n) ((uint64_t)(uint32_t)(((uint32_t)(v) >> (n)) | ((uint32_t)(v) << ((32 - (n)) & 31))))
#define ROL64(v, n) (((uint64_t)(v) << (n)) | ((uint64_t)(v) >> ((64 - (n)) & 63)))
#define ROR64(v, n) (((uint64_t)(v) >> (n)) | ((uint64_t)(v) << ((64 - (n)) & 63)))

#ifdef _MSC_VER
static inline uint64_t MULU64(uint64_t a, uint64_t b, uint64_t* hi) { return _umul128(a, b, hi); }
static inline uint64_t MULS64(int64_t a, int64_t b, int64_t* hi) { return (uint64_t)_mul128(a, b, hi); }
static inline uint64_t CTZ64(uint64_t v) { unsigned long i; _BitScanForward64(&i, v); return i; }
static inline uint64_t CLZ64(uint64_t v) { unsigned long i; _BitScanReverse64(&i, v); return 63 - i; }
static inline uint64_t POPCNT64(uint64_t v) { return __popcnt64(v); }
static inline uint64_t BSWAP16(uint64_t v) { return _byteswap_ushort((uint16_t)v); }
static inline uint64_t BSWAP32(uint64_t v) { return _byteswap_ulong((uint32_t)v); }
static inline uint64_t BSWAP64(uint64_t v) { return _byteswap_uint64(v); }
#else
static inline uint64_t MULU64(uint64_t a, uint64_t b, uint64_t* hi) { unsigned __int128 p = (unsigned __int128)a * b; *hi = (uint64_t)(p >> 64); return (uint64_t)p; }
static inline uint64_t MULS64(int64_t a, int64_t b, int64_t* hi) { __int128 p = (__int128)a * b; *hi = (int64_t)(p >> 64); return (uint64_t)p; }
static inline uint64_t CTZ64(uint64_t v) { return (uint64_t)__builtin_ctzll(v); }
static inline uint64_t CLZ64(uint64_t v) { return (uint64_t)__builtin_clzll(v); }
static inline uint64_t POPCNT64(uint64_t v) { return (uint64_t)__builtin_popcountll(v); }
static inline uint64_t BSWAP16(uint64_t v) { return __builtin_bswap16((uint16_t)v); }
static inline uint64_t BSWAP32(uint64_t v) { return __builtin_bswap32((uint32_t)v); }
static inline uint64_t BSWAP64(uint64_t v) { return __builtin_bswap64(v); }
#endif

static inline uint64_t CVTT32(double v) { return (v != v || v >= 2147483648.0 || v < -2147483648.0) ? 0x80000000ull : (uint64_t)(uint32_t)(int32_t)v; }
static inline uint64_t CVTT64(double v) { return (v != v || v >= 9223372036854775808.0 || v < -9223372036854775808.0) ? 0x8000000000000000ull : (uint64_t)(int64_t)v; }
static inline uint64_t CVTR32(double v) { return CVTT32(nearbyint(v)); }
static inline uint64_t CVTR64(double v) { return CVTT64(nearbyint(v)); }

#ifdef UEW_THREADS
/* a call site's cache, filled once: the target is published after the function (an atomic read on the fast path) */
typedef struct IC { _Atomic uint64_t a; GuestFn f; } IC;
#else
typedef struct IC { uint64_t a; GuestFn f; } IC;
#endif
void rt_call(CPU* c, uint64_t target);
void rt_call_ic(CPU* c, uint64_t target, IC* ic);
void rt_jump(CPU* c, uint64_t target);
GuestFn rt_jump_fn(CPU* c, uint64_t target);
GuestFn rt_ic_fn(CPU* c, uint64_t target, IC* ic);
/* jumps between lifted functions must not grow the host stack (loops can span chained fragments) */
#if defined(__wasm_tail_call__)
#define TAIL ; __attribute__((musttail)) return
#else
#define TAIL
#endif
void rt_trap(CPU* c, uint64_t pc, const char* what);
void rt_cpuid(CPU* c);
extern uint64_t rt_div_ip;   /* guest address of the division being run (divide error reports) */
void rt_fxsave(CPU* c, uint64_t a);
void rt_fxrstor(CPU* c, uint64_t a);
void rt_pause(CPU* c);
void rt_xgetbv(CPU* c);
uint64_t rt_crc32(uint64_t crc, uint64_t v, int bytes);
uint64_t rt_pcmpstr(Flags* f, XMM* x0, XMM a, XMM b, int imm, int64_t la, int64_t lb, int expl, int mask);
void rt_rdtsc(CPU* c);
void rt_div8(CPU* c, uint64_t s);
void rt_div16(CPU* c, uint64_t s);
void rt_div32(CPU* c, uint64_t s);
void rt_div64(CPU* c, uint64_t s);
void rt_idiv8(CPU* c, uint64_t s);
void rt_idiv16(CPU* c, uint64_t s);
void rt_idiv32(CPU* c, uint64_t s);
void rt_idiv64(CPU* c, uint64_t s);
void rt_cmpxchg16b(CPU* c, Flags* f, uint64_t a);
void rt_movs(CPU* c, int n, int rep);
void rt_stos(CPU* c, int n, int rep);
void rt_lods(CPU* c, int n, int rep);
void rt_scas(CPU* c, Flags* f, int n, int rep);
void rt_cmps(CPU* c, Flags* f, int n, int rep);

#include "sse.h"
#include "x87.h"
