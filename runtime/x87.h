#pragma once
/* x87 for 32-bit guests: ST(i) is st[(top + i) & 7]; doubles stand in for the 80-bit registers */

#define FST_(i) (c->st[(c->ftop + (i)) & 7])

static inline void fpush(CPU* c, double v) { c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v; }
static inline void fpop(CPU* c) { c->ftop = (c->ftop + 1) & 7; }

static inline double F32B(uint64_t b) { uint32_t u = (uint32_t)b; float f; memcpy(&f, &u, 4); return f; }
static inline double F64B(uint64_t b) { double d; memcpy(&d, &b, 8); return d; }
static inline uint64_t B32F(double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); return u; }
static inline uint64_t B64F(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }

static inline double F80B(uint64_t m, uint64_t se)
{
    int e = (int)(se & 0x7FFF), neg = (int)((se >> 15) & 1);
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7FFF) v = (m << 1) ? NAN : INFINITY;
    else v = ldexp((double)m, e - 16383 - 63);
    return neg ? -v : v;
}

static inline void F80S(double v, uint64_t* m, uint16_t* se)
{
    uint16_t s = signbit(v) ? 0x8000 : 0;
    if (v == 0.0) { *m = 0; *se = s; return; }
    if (isinf(v)) { *m = 1ull << 63; *se = s | 0x7FFF; return; }
    if (isnan(v)) { *m = 3ull << 62; *se = s | 0x7FFF; return; }
    int e;
    double f = frexp(fabs(v), &e);
    *m = (uint64_t)ldexp(f, 64);
    *se = s | (uint16_t)(e - 1 + 16383);
}

static inline double frnd(CPU* c, double v)
{
    switch ((c->fcw >> 10) & 3) {
    case 0: return nearbyint(v);
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}

/* FIST: out of range (or NaN) gives the integer indefinite */
static inline uint64_t fist(CPU* c, double v, int bits, int trunc_)
{
    double r = trunc_ ? trunc(v) : frnd(c, v);
    if (bits == 16) return (r != r || r >= 32768.0 || r < -32768.0) ? 0x8000 : (uint64_t)(uint16_t)(int16_t)r;
    if (bits == 32) return (r != r || r >= 2147483648.0 || r < -2147483648.0) ? 0x80000000ull : (uint64_t)(uint32_t)(int32_t)r;
    return (r != r || r >= 9223372036854775808.0 || r < -9223372036854775808.0) ? 0x8000000000000000ull : (uint64_t)(int64_t)r;
}

/* C0 (less), C2 (unordered), C3 (equal) */
static inline void fcom(CPU* c, double a, double b)
{
    uint32_t f = c->fsw & ~0x4500u;
    if (a != a || b != b) f |= 0x4500;
    else if (a < b) f |= 0x100;
    else if (a == b) f |= 0x4000;
    c->fsw = f;
}

static inline void fxam(CPU* c)
{
    double v = FST_(0);
    uint32_t f = c->fsw & ~0x4700u;
    if (signbit(v)) f |= 0x200;
    if (v != v) f |= 0x100;
    else if (isinf(v)) f |= 0x500;
    else if (v == 0.0) f |= 0x4000;
    else if (fabs(v) < 2.2250738585072014e-308) f |= 0x4400;
    else f |= 0x400;
    c->fsw = f;
}

static inline uint64_t fstsw(CPU* c) { return (c->fsw & ~0x3800u) | ((c->ftop & 7) << 11); }

void rt_fnstenv(CPU* c, uint64_t a);
void rt_fldenv(CPU* c, uint64_t a);
void rt_fnsave(CPU* c, uint64_t a);
void rt_frstor(CPU* c, uint64_t a);
