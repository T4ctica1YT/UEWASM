#pragma once
#define XFN(name) static inline void x_##name
#define LANES(n) for (int i = 0; i < (n); i++)
/* with wasm SIMD the ops below get vector versions at the end of this file; the scalar ones stay as the reference
   (tests/sse compares the two) */
#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#define XS(name) static inline __attribute__((unused)) void xs_##name
#define XSR(ret, name) static inline __attribute__((unused)) ret xs_##name
static inline void x_ADDPS(XMM* d, XMM s);
static inline void x_SUBPS(XMM* d, XMM s);
static inline void x_MULPS(XMM* d, XMM s);
static inline void x_DIVPS(XMM* d, XMM s);
static inline void x_MINPS(XMM* d, XMM s);
static inline void x_MAXPS(XMM* d, XMM s);
static inline void x_SQRTPS(XMM* d, XMM s);
static inline void x_ADDPD(XMM* d, XMM s);
static inline void x_SUBPD(XMM* d, XMM s);
static inline void x_MULPD(XMM* d, XMM s);
static inline void x_DIVPD(XMM* d, XMM s);
static inline void x_MINPD(XMM* d, XMM s);
static inline void x_MAXPD(XMM* d, XMM s);
static inline void x_SQRTPD(XMM* d, XMM s);
static inline void x_HADDPS(XMM* d, XMM s);
static inline void x_HADDPD(XMM* d, XMM s);
static inline void x_ANDPS(XMM* d, XMM s);
static inline void x_ANDNPS(XMM* d, XMM s);
static inline void x_ORPS(XMM* d, XMM s);
static inline void x_XORPS(XMM* d, XMM s);
static inline void x_PADDB(XMM* d, XMM s);
static inline void x_PADDW(XMM* d, XMM s);
static inline void x_PADDD(XMM* d, XMM s);
static inline void x_PADDQ(XMM* d, XMM s);
static inline void x_PSUBB(XMM* d, XMM s);
static inline void x_PSUBW(XMM* d, XMM s);
static inline void x_PSUBD(XMM* d, XMM s);
static inline void x_PSUBQ(XMM* d, XMM s);
static inline void x_PADDUSB(XMM* d, XMM s);
static inline void x_PSUBUSB(XMM* d, XMM s);
static inline void x_PADDUSW(XMM* d, XMM s);
static inline void x_PSUBUSW(XMM* d, XMM s);
static inline void x_PMULLW(XMM* d, XMM s);
static inline void x_PMULHW(XMM* d, XMM s);
static inline void x_PMULHUW(XMM* d, XMM s);
static inline void x_PMULLD(XMM* d, XMM s);
static inline void x_PMULUDQ(XMM* d, XMM s);
static inline void x_PMULDQ(XMM* d, XMM s);
static inline void x_PMADDWD(XMM* d, XMM s);
static inline void x_PAVGB(XMM* d, XMM s);
static inline void x_PAVGW(XMM* d, XMM s);
static inline void x_PMINUB(XMM* d, XMM s);
static inline void x_PMAXUB(XMM* d, XMM s);
static inline void x_PMINSW(XMM* d, XMM s);
static inline void x_PMAXSW(XMM* d, XMM s);
static inline void x_PMINSD(XMM* d, XMM s);
static inline void x_PMAXSD(XMM* d, XMM s);
static inline void x_PMINUD(XMM* d, XMM s);
static inline void x_PMAXUD(XMM* d, XMM s);
static inline void x_PABSD(XMM* d, XMM s);
static inline void x_PCMPEQB(XMM* d, XMM s);
static inline void x_PCMPEQW(XMM* d, XMM s);
static inline void x_PCMPEQD(XMM* d, XMM s);
static inline void x_PCMPEQQ(XMM* d, XMM s);
static inline void x_PCMPGTB(XMM* d, XMM s);
static inline void x_PCMPGTW(XMM* d, XMM s);
static inline void x_PCMPGTD(XMM* d, XMM s);
static inline void x_PCMPGTQ(XMM* d, XMM s);
static inline void x_PSLLW(XMM* d, XMM s);
static inline void x_PSLLD(XMM* d, XMM s);
static inline void x_PSLLQ(XMM* d, XMM s);
static inline void x_PSRLW(XMM* d, XMM s);
static inline void x_PSRLD(XMM* d, XMM s);
static inline void x_PSRLQ(XMM* d, XMM s);
static inline void x_PSRAW(XMM* d, XMM s);
static inline void x_PSRAD(XMM* d, XMM s);
static inline void x_PSHUFB(XMM* d, XMM s);
static inline void x_UNPCKLPS(XMM* d, XMM s);
static inline void x_UNPCKHPS(XMM* d, XMM s);
static inline void x_UNPCKLPD(XMM* d, XMM s);
static inline void x_UNPCKHPD(XMM* d, XMM s);
static inline void x_PUNPCKLBW(XMM* d, XMM s);
static inline void x_PUNPCKHBW(XMM* d, XMM s);
static inline void x_PUNPCKLWD(XMM* d, XMM s);
static inline void x_PUNPCKHWD(XMM* d, XMM s);
static inline void x_PUNPCKLQDQ(XMM* d, XMM s);
static inline void x_PUNPCKHQDQ(XMM* d, XMM s);
static inline void x_PACKSSWB(XMM* d, XMM s);
static inline void x_PACKUSWB(XMM* d, XMM s);
static inline void x_PACKSSDW(XMM* d, XMM s);
static inline void x_PACKUSDW(XMM* d, XMM s);
static inline void x_PMOVZXBW(XMM* d, XMM s);
static inline void x_PMOVZXBD(XMM* d, XMM s);
static inline void x_PMOVZXWD(XMM* d, XMM s);
static inline void x_PMOVZXDQ(XMM* d, XMM s);
static inline void x_PMOVSXBW(XMM* d, XMM s);
static inline void x_PMOVSXBD(XMM* d, XMM s);
static inline void x_PMOVSXWD(XMM* d, XMM s);
static inline void x_PMOVSXDQ(XMM* d, XMM s);
static inline void x_CVTDQ2PS(XMM* d, XMM s);
static inline void x_MOVSHDUP(XMM* d, XMM s);
static inline void x_MOVSLDUP(XMM* d, XMM s);
static inline void x_MOVDDUP(XMM* d, XMM s);
static inline void x_PADDSB(XMM* d, XMM s);
static inline void x_PSUBSB(XMM* d, XMM s);
static inline void x_PADDSW(XMM* d, XMM s);
static inline void x_PSUBSW(XMM* d, XMM s);
static inline void x_PMINSB(XMM* d, XMM s);
static inline void x_PMAXSB(XMM* d, XMM s);
static inline void x_PMINUW(XMM* d, XMM s);
static inline void x_PMAXUW(XMM* d, XMM s);
static inline void x_PABSB(XMM* d, XMM s);
static inline void x_PABSW(XMM* d, XMM s);
static inline void x_PHADDW(XMM* d, XMM s);
static inline void x_PHADDD(XMM* d, XMM s);
static inline void x_PHSUBW(XMM* d, XMM s);
static inline void x_PHSUBD(XMM* d, XMM s);
static inline void x_PMADDUBSW(XMM* d, XMM s);
static inline void x_PMULHRSW(XMM* d, XMM s);
static inline void x_HSUBPS(XMM* d, XMM s);
static inline void x_HSUBPD(XMM* d, XMM s);
#else
#define XS(name) XFN(name)
#define XSR(ret, name) static inline ret x_##name
#endif

static inline float rcpf(float v) { return 1.0f / v; }
static inline float rsqrtf_(float v) { return 1.0f / sqrtf(v); }
static inline float minf_(float a, float b) { return a < b ? a : b; }
static inline float maxf_(float a, float b) { return a > b ? a : b; }
static inline double mind_(double a, double b) { return a < b ? a : b; }
static inline double maxd_(double a, double b) { return a > b ? a : b; }

XS(ADDPS)(XMM* d, XMM s) { LANES(4) d->f[i] += s.f[i]; }
XS(SUBPS)(XMM* d, XMM s) { LANES(4) d->f[i] -= s.f[i]; }
XS(MULPS)(XMM* d, XMM s) { LANES(4) d->f[i] *= s.f[i]; }
XS(DIVPS)(XMM* d, XMM s) { LANES(4) d->f[i] /= s.f[i]; }
XS(MINPS)(XMM* d, XMM s) { LANES(4) d->f[i] = minf_(d->f[i], s.f[i]); }
XS(MAXPS)(XMM* d, XMM s) { LANES(4) d->f[i] = maxf_(d->f[i], s.f[i]); }
XS(SQRTPS)(XMM* d, XMM s) { LANES(4) d->f[i] = sqrtf(s.f[i]); }
XFN(RSQRTPS)(XMM* d, XMM s) { LANES(4) d->f[i] = rsqrtf_(s.f[i]); }
XFN(RCPPS)(XMM* d, XMM s) { LANES(4) d->f[i] = rcpf(s.f[i]); }
XS(ADDPD)(XMM* d, XMM s) { LANES(2) d->g[i] += s.g[i]; }
XS(SUBPD)(XMM* d, XMM s) { LANES(2) d->g[i] -= s.g[i]; }
XS(MULPD)(XMM* d, XMM s) { LANES(2) d->g[i] *= s.g[i]; }
XS(DIVPD)(XMM* d, XMM s) { LANES(2) d->g[i] /= s.g[i]; }
XS(MINPD)(XMM* d, XMM s) { LANES(2) d->g[i] = mind_(d->g[i], s.g[i]); }
XS(MAXPD)(XMM* d, XMM s) { LANES(2) d->g[i] = maxd_(d->g[i], s.g[i]); }
XS(SQRTPD)(XMM* d, XMM s) { LANES(2) d->g[i] = sqrt(s.g[i]); }
XFN(ADDSS)(XMM* d, XMM s) { d->f[0] += s.f[0]; }
XFN(SUBSS)(XMM* d, XMM s) { d->f[0] -= s.f[0]; }
XFN(MULSS)(XMM* d, XMM s) { d->f[0] *= s.f[0]; }
XFN(DIVSS)(XMM* d, XMM s) { d->f[0] /= s.f[0]; }
XFN(MINSS)(XMM* d, XMM s) { d->f[0] = minf_(d->f[0], s.f[0]); }
XFN(MAXSS)(XMM* d, XMM s) { d->f[0] = maxf_(d->f[0], s.f[0]); }
XFN(SQRTSS)(XMM* d, XMM s) { d->f[0] = sqrtf(s.f[0]); }
XFN(RSQRTSS)(XMM* d, XMM s) { d->f[0] = rsqrtf_(s.f[0]); }
XFN(RCPSS)(XMM* d, XMM s) { d->f[0] = rcpf(s.f[0]); }
XFN(ADDSD)(XMM* d, XMM s) { d->g[0] += s.g[0]; }
XFN(SUBSD)(XMM* d, XMM s) { d->g[0] -= s.g[0]; }
XFN(MULSD)(XMM* d, XMM s) { d->g[0] *= s.g[0]; }
XFN(DIVSD)(XMM* d, XMM s) { d->g[0] /= s.g[0]; }
XFN(MINSD)(XMM* d, XMM s) { d->g[0] = mind_(d->g[0], s.g[0]); }
XFN(MAXSD)(XMM* d, XMM s) { d->g[0] = maxd_(d->g[0], s.g[0]); }
XFN(SQRTSD)(XMM* d, XMM s) { d->g[0] = sqrt(s.g[0]); }
XS(HADDPS)(XMM* d, XMM s) { XMM t; t.f[0] = d->f[0] + d->f[1]; t.f[1] = d->f[2] + d->f[3]; t.f[2] = s.f[0] + s.f[1]; t.f[3] = s.f[2] + s.f[3]; *d = t; }
XS(HADDPD)(XMM* d, XMM s) { XMM t; t.g[0] = d->g[0] + d->g[1]; t.g[1] = s.g[0] + s.g[1]; *d = t; }

XS(ANDPS)(XMM* d, XMM s) { d->q[0] &= s.q[0]; d->q[1] &= s.q[1]; }
XS(ANDNPS)(XMM* d, XMM s) { d->q[0] = ~d->q[0] & s.q[0]; d->q[1] = ~d->q[1] & s.q[1]; }
XS(ORPS)(XMM* d, XMM s) { d->q[0] |= s.q[0]; d->q[1] |= s.q[1]; }
XS(XORPS)(XMM* d, XMM s) { d->q[0] ^= s.q[0]; d->q[1] ^= s.q[1]; }
XFN(ANDPD)(XMM* d, XMM s) { x_ANDPS(d, s); }
XFN(ANDNPD)(XMM* d, XMM s) { x_ANDNPS(d, s); }
XFN(ORPD)(XMM* d, XMM s) { x_ORPS(d, s); }
XFN(XORPD)(XMM* d, XMM s) { x_XORPS(d, s); }
XFN(PAND)(XMM* d, XMM s) { x_ANDPS(d, s); }
XFN(PANDN)(XMM* d, XMM s) { x_ANDNPS(d, s); }
XFN(POR)(XMM* d, XMM s) { x_ORPS(d, s); }
XFN(PXOR)(XMM* d, XMM s) { x_XORPS(d, s); }

XS(PADDB)(XMM* d, XMM s) { LANES(16) d->b[i] += s.b[i]; }
XS(PADDW)(XMM* d, XMM s) { LANES(8) d->w[i] += s.w[i]; }
XS(PADDD)(XMM* d, XMM s) { LANES(4) d->d[i] += s.d[i]; }
XS(PADDQ)(XMM* d, XMM s) { LANES(2) d->q[i] += s.q[i]; }
XS(PSUBB)(XMM* d, XMM s) { LANES(16) d->b[i] -= s.b[i]; }
XS(PSUBW)(XMM* d, XMM s) { LANES(8) d->w[i] -= s.w[i]; }
XS(PSUBD)(XMM* d, XMM s) { LANES(4) d->d[i] -= s.d[i]; }
XS(PSUBQ)(XMM* d, XMM s) { LANES(2) d->q[i] -= s.q[i]; }
XS(PADDUSB)(XMM* d, XMM s) { LANES(16) { int v = d->b[i] + s.b[i]; d->b[i] = (uint8_t)(v > 255 ? 255 : v); } }
XS(PSUBUSB)(XMM* d, XMM s) { LANES(16) { int v = d->b[i] - s.b[i]; d->b[i] = (uint8_t)(v < 0 ? 0 : v); } }
XS(PADDUSW)(XMM* d, XMM s) { LANES(8) { int v = d->w[i] + s.w[i]; d->w[i] = (uint16_t)(v > 65535 ? 65535 : v); } }
XS(PSUBUSW)(XMM* d, XMM s) { LANES(8) { int v = d->w[i] - s.w[i]; d->w[i] = (uint16_t)(v < 0 ? 0 : v); } }
XS(PMULLW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)(d->sw[i] * s.sw[i]); }
XS(PMULHW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)((d->sw[i] * s.sw[i]) >> 16); }
XS(PMULHUW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)(((uint32_t)d->w[i] * s.w[i]) >> 16); }
XS(PMULLD)(XMM* d, XMM s) { LANES(4) d->d[i] = d->d[i] * s.d[i]; }
XS(PMULUDQ)(XMM* d, XMM s) { d->q[0] = (uint64_t)d->d[0] * s.d[0]; d->q[1] = (uint64_t)d->d[2] * s.d[2]; }
XS(PMULDQ)(XMM* d, XMM s) { d->sq[0] = (int64_t)d->sd[0] * s.sd[0]; d->sq[1] = (int64_t)d->sd[2] * s.sd[2]; }
XS(PMADDWD)(XMM* d, XMM s) { XMM t; LANES(4) t.sd[i] = d->sw[2 * i] * s.sw[2 * i] + d->sw[2 * i + 1] * s.sw[2 * i + 1]; *d = t; }
XS(PAVGB)(XMM* d, XMM s) { LANES(16) d->b[i] = (uint8_t)((d->b[i] + s.b[i] + 1) >> 1); }
XS(PAVGW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)((d->w[i] + s.w[i] + 1) >> 1); }
XFN(PSADBW)(XMM* d, XMM s) { XMM t; memset(&t, 0, 16); LANES(16) { int v = d->b[i] - s.b[i]; t.q[i / 8] += (uint64_t)(v < 0 ? -v : v); } *d = t; }
XS(PMINUB)(XMM* d, XMM s) { LANES(16) if (s.b[i] < d->b[i]) d->b[i] = s.b[i]; }
XS(PMAXUB)(XMM* d, XMM s) { LANES(16) if (s.b[i] > d->b[i]) d->b[i] = s.b[i]; }
XS(PMINSW)(XMM* d, XMM s) { LANES(8) if (s.sw[i] < d->sw[i]) d->sw[i] = s.sw[i]; }
XS(PMAXSW)(XMM* d, XMM s) { LANES(8) if (s.sw[i] > d->sw[i]) d->sw[i] = s.sw[i]; }
XS(PMINSD)(XMM* d, XMM s) { LANES(4) if (s.sd[i] < d->sd[i]) d->sd[i] = s.sd[i]; }
XS(PMAXSD)(XMM* d, XMM s) { LANES(4) if (s.sd[i] > d->sd[i]) d->sd[i] = s.sd[i]; }
XS(PMINUD)(XMM* d, XMM s) { LANES(4) if (s.d[i] < d->d[i]) d->d[i] = s.d[i]; }
XS(PMAXUD)(XMM* d, XMM s) { LANES(4) if (s.d[i] > d->d[i]) d->d[i] = s.d[i]; }
XS(PABSD)(XMM* d, XMM s) { LANES(4) d->sd[i] = s.sd[i] < 0 ? -s.sd[i] : s.sd[i]; }
XS(PCMPEQB)(XMM* d, XMM s) { LANES(16) d->b[i] = d->b[i] == s.b[i] ? 0xFF : 0; }
XS(PCMPEQW)(XMM* d, XMM s) { LANES(8) d->w[i] = d->w[i] == s.w[i] ? 0xFFFF : 0; }
XS(PCMPEQD)(XMM* d, XMM s) { LANES(4) d->d[i] = d->d[i] == s.d[i] ? 0xFFFFFFFFu : 0; }
XS(PCMPEQQ)(XMM* d, XMM s) { LANES(2) d->q[i] = d->q[i] == s.q[i] ? ~0ull : 0; }
XS(PCMPGTB)(XMM* d, XMM s) { LANES(16) d->b[i] = d->sb[i] > s.sb[i] ? 0xFF : 0; }
XS(PCMPGTW)(XMM* d, XMM s) { LANES(8) d->w[i] = d->sw[i] > s.sw[i] ? 0xFFFF : 0; }
XS(PCMPGTD)(XMM* d, XMM s) { LANES(4) d->d[i] = d->sd[i] > s.sd[i] ? 0xFFFFFFFFu : 0; }
XS(PCMPGTQ)(XMM* d, XMM s) { LANES(2) d->q[i] = d->sq[i] > s.sq[i] ? ~0ull : 0; }

static inline unsigned shcount(XMM s) { return s.q[0] > 255 ? 255 : (unsigned)s.q[0]; }
XS(PSLLW)(XMM* d, XMM s) { unsigned n = shcount(s); LANES(8) d->w[i] = n > 15 ? 0 : (uint16_t)(d->w[i] << n); }
XS(PSLLD)(XMM* d, XMM s) { unsigned n = shcount(s); LANES(4) d->d[i] = n > 31 ? 0 : d->d[i] << n; }
XS(PSLLQ)(XMM* d, XMM s) { unsigned n = shcount(s); LANES(2) d->q[i] = n > 63 ? 0 : d->q[i] << n; }
XS(PSRLW)(XMM* d, XMM s) { unsigned n = shcount(s); LANES(8) d->w[i] = n > 15 ? 0 : (uint16_t)(d->w[i] >> n); }
XS(PSRLD)(XMM* d, XMM s) { unsigned n = shcount(s); LANES(4) d->d[i] = n > 31 ? 0 : d->d[i] >> n; }
XS(PSRLQ)(XMM* d, XMM s) { unsigned n = shcount(s); LANES(2) d->q[i] = n > 63 ? 0 : d->q[i] >> n; }
XS(PSRAW)(XMM* d, XMM s) { unsigned n = shcount(s); if (n > 15) n = 15; LANES(8) d->sw[i] = (int16_t)(d->sw[i] >> n); }
XS(PSRAD)(XMM* d, XMM s) { unsigned n = shcount(s); if (n > 31) n = 31; LANES(4) d->sd[i] = d->sd[i] >> n; }
static inline XMM immx(int imm) { XMM t; memset(&t, 0, 16); t.q[0] = (uint64_t)imm; return t; }
XFN(PSLLDQ)(XMM* d, XMM s, int imm) { XMM t; memset(&t, 0, 16); if (imm < 16) memcpy(t.b + imm, s.b, 16 - imm); *d = t; }
XFN(PSRLDQ)(XMM* d, XMM s, int imm) { XMM t; memset(&t, 0, 16); if (imm < 16) memcpy(t.b, s.b + imm, 16 - imm); *d = t; }

XS(PSHUFD)(XMM* d, XMM s, int imm) { XMM t; LANES(4) t.d[i] = s.d[(imm >> (2 * i)) & 3]; *d = t; }
XS(PSHUFLW)(XMM* d, XMM s, int imm) { XMM t = s; LANES(4) t.w[i] = s.w[(imm >> (2 * i)) & 3]; *d = t; }
XS(PSHUFHW)(XMM* d, XMM s, int imm) { XMM t = s; LANES(4) t.w[4 + i] = s.w[4 + ((imm >> (2 * i)) & 3)]; *d = t; }
XS(SHUFPS)(XMM* d, XMM s, int imm) { XMM t; t.d[0] = d->d[imm & 3]; t.d[1] = d->d[(imm >> 2) & 3]; t.d[2] = s.d[(imm >> 4) & 3]; t.d[3] = s.d[(imm >> 6) & 3]; *d = t; }
XFN(SHUFPD)(XMM* d, XMM s, int imm) { XMM t; t.q[0] = d->q[imm & 1]; t.q[1] = s.q[(imm >> 1) & 1]; *d = t; }
XS(PSHUFB)(XMM* d, XMM s) { XMM t; LANES(16) t.b[i] = (s.b[i] & 0x80) ? 0 : d->b[s.b[i] & 15]; *d = t; }
XFN(PALIGNR)(XMM* d, XMM s, int imm) { uint8_t tmp[32]; memcpy(tmp, s.b, 16); memcpy(tmp + 16, d->b, 16); XMM t; LANES(16) t.b[i] = (imm + i < 32) ? tmp[imm + i] : 0; *d = t; }
XS(UNPCKLPS)(XMM* d, XMM s) { XMM t; t.d[0] = d->d[0]; t.d[1] = s.d[0]; t.d[2] = d->d[1]; t.d[3] = s.d[1]; *d = t; }
XS(UNPCKHPS)(XMM* d, XMM s) { XMM t; t.d[0] = d->d[2]; t.d[1] = s.d[2]; t.d[2] = d->d[3]; t.d[3] = s.d[3]; *d = t; }
XS(UNPCKLPD)(XMM* d, XMM s) { d->q[1] = s.q[0]; }
XS(UNPCKHPD)(XMM* d, XMM s) { d->q[0] = d->q[1]; d->q[1] = s.q[1]; }
XS(PUNPCKLBW)(XMM* d, XMM s) { XMM t; LANES(8) { t.b[2 * i] = d->b[i]; t.b[2 * i + 1] = s.b[i]; } *d = t; }
XS(PUNPCKHBW)(XMM* d, XMM s) { XMM t; LANES(8) { t.b[2 * i] = d->b[8 + i]; t.b[2 * i + 1] = s.b[8 + i]; } *d = t; }
XS(PUNPCKLWD)(XMM* d, XMM s) { XMM t; LANES(4) { t.w[2 * i] = d->w[i]; t.w[2 * i + 1] = s.w[i]; } *d = t; }
XS(PUNPCKHWD)(XMM* d, XMM s) { XMM t; LANES(4) { t.w[2 * i] = d->w[4 + i]; t.w[2 * i + 1] = s.w[4 + i]; } *d = t; }
XFN(PUNPCKLDQ)(XMM* d, XMM s) { x_UNPCKLPS(d, s); }
XFN(PUNPCKHDQ)(XMM* d, XMM s) { x_UNPCKHPS(d, s); }
XS(PUNPCKLQDQ)(XMM* d, XMM s) { d->q[1] = s.q[0]; }
XS(PUNPCKHQDQ)(XMM* d, XMM s) { d->q[0] = d->q[1]; d->q[1] = s.q[1]; }
static inline uint8_t satu8(int v) { return (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
static inline uint16_t satu16(int v) { return (uint16_t)(v > 65535 ? 65535 : v < 0 ? 0 : v); }
static inline int8_t sat8(int v) { return (int8_t)(v > 127 ? 127 : v < -128 ? -128 : v); }
static inline int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
XS(PACKSSWB)(XMM* d, XMM s) { XMM t; LANES(8) { t.sb[i] = sat8(d->sw[i]); t.sb[8 + i] = sat8(s.sw[i]); } *d = t; }
XS(PACKUSWB)(XMM* d, XMM s) { XMM t; LANES(8) { t.b[i] = satu8(d->sw[i]); t.b[8 + i] = satu8(s.sw[i]); } *d = t; }
XS(PACKSSDW)(XMM* d, XMM s) { XMM t; LANES(4) { t.sw[i] = sat16(d->sd[i]); t.sw[4 + i] = sat16(s.sd[i]); } *d = t; }
XS(PACKUSDW)(XMM* d, XMM s) { XMM t; LANES(4) { t.w[i] = satu16(d->sd[i]); t.w[4 + i] = satu16(s.sd[i]); } *d = t; }
XS(PMOVZXBW)(XMM* d, XMM s) { XMM t; LANES(8) t.w[i] = s.b[i]; *d = t; }
XS(PMOVZXBD)(XMM* d, XMM s) { XMM t; LANES(4) t.d[i] = s.b[i]; *d = t; }
XS(PMOVZXWD)(XMM* d, XMM s) { XMM t; LANES(4) t.d[i] = s.w[i]; *d = t; }
XS(PMOVZXDQ)(XMM* d, XMM s) { XMM t; LANES(2) t.q[i] = s.d[i]; *d = t; }
XS(PMOVSXBW)(XMM* d, XMM s) { XMM t; LANES(8) t.sw[i] = s.sb[i]; *d = t; }
XS(PMOVSXBD)(XMM* d, XMM s) { XMM t; LANES(4) t.sd[i] = s.sb[i]; *d = t; }
XS(PMOVSXWD)(XMM* d, XMM s) { XMM t; LANES(4) t.sd[i] = s.sw[i]; *d = t; }
XS(PMOVSXDQ)(XMM* d, XMM s) { XMM t; LANES(2) t.sq[i] = s.sd[i]; *d = t; }

XS(CVTDQ2PS)(XMM* d, XMM s) { LANES(4) d->f[i] = (float)s.sd[i]; }
XFN(CVTTPS2DQ)(XMM* d, XMM s) { LANES(4) d->d[i] = (uint32_t)CVTT32(s.f[i]); }
XFN(CVTPS2DQ)(XMM* d, XMM s) { LANES(4) d->d[i] = (uint32_t)CVTR32(s.f[i]); }
XFN(CVTDQ2PD)(XMM* d, XMM s) { XMM t; t.g[0] = s.sd[0]; t.g[1] = s.sd[1]; *d = t; }
XFN(CVTPS2PD)(XMM* d, XMM s) { XMM t; t.g[0] = s.f[0]; t.g[1] = s.f[1]; *d = t; }
XFN(CVTPD2PS)(XMM* d, XMM s) { XMM t; memset(&t, 0, 16); t.f[0] = (float)s.g[0]; t.f[1] = (float)s.g[1]; *d = t; }
XFN(CVTTPD2DQ)(XMM* d, XMM s) { XMM t; memset(&t, 0, 16); t.d[0] = (uint32_t)CVTT32(s.g[0]); t.d[1] = (uint32_t)CVTT32(s.g[1]); *d = t; }
XFN(CVTSS2SD)(XMM* d, XMM s) { d->g[0] = (double)s.f[0]; }
XFN(CVTSD2SS)(XMM* d, XMM s) { d->f[0] = (float)s.g[0]; }

static inline int fcmp(double a, double b, int p)
{
    switch (p & 7) {
    case 0: return a == b;
    case 1: return a < b;
    case 2: return a <= b;
    case 3: return a != a || b != b;
    case 4: return !(a == b);
    case 5: return !(a < b);
    case 6: return !(a <= b);
    default: return a == a && b == b;
    }
}
XS(CMPPS)(XMM* d, XMM s, int p) { LANES(4) d->d[i] = fcmp(d->f[i], s.f[i], p) ? 0xFFFFFFFFu : 0; }
XFN(CMPSS)(XMM* d, XMM s, int p) { d->d[0] = fcmp(d->f[0], s.f[0], p) ? 0xFFFFFFFFu : 0; }
XS(CMPPD)(XMM* d, XMM s, int p) { LANES(2) d->q[i] = fcmp(d->g[i], s.g[i], p) ? ~0ull : 0; }
XFN(CMPSD)(XMM* d, XMM s, int p) { d->q[0] = fcmp(d->g[0], s.g[0], p) ? ~0ull : 0; }

static inline double rnd(double v, int imm)
{
    switch ((imm & 4) ? 0 : (imm & 3)) {
    case 1: return floor(v);
    case 2: return ceil(v);
    case 3: return trunc(v);
    default: return nearbyint(v);
    }
}
XS(ROUNDPS)(XMM* d, XMM s, int imm) { LANES(4) d->f[i] = (float)rnd(s.f[i], imm); }
XS(ROUNDPD)(XMM* d, XMM s, int imm) { LANES(2) d->g[i] = rnd(s.g[i], imm); }
XFN(ROUNDSS)(XMM* d, XMM s, int imm) { d->f[0] = (float)rnd(s.f[0], imm); }
XFN(ROUNDSD)(XMM* d, XMM s, int imm) { d->g[0] = rnd(s.g[0], imm); }
XS(BLENDPS)(XMM* d, XMM s, int imm) { LANES(4) if (imm & (1 << i)) d->d[i] = s.d[i]; }
XS(BLENDPD)(XMM* d, XMM s, int imm) { LANES(2) if (imm & (1 << i)) d->q[i] = s.q[i]; }
XS(PBLENDW)(XMM* d, XMM s, int imm) { LANES(8) if (imm & (1 << i)) d->w[i] = s.w[i]; }
XFN(INSERTPS)(XMM* d, XMM s, int imm) { uint32_t v = s.d[(imm >> 6) & 3]; d->d[(imm >> 4) & 3] = v; LANES(4) if (imm & (1 << i)) d->d[i] = 0; }
XFN(DPPS)(XMM* d, XMM s, int imm) { float p[4]; LANES(4) p[i] = (imm & (16 << i)) ? d->f[i] * s.f[i] : 0.0f; float sum = (p[0] + p[1]) + (p[2] + p[3]); LANES(4) d->f[i] = (imm & (1 << i)) ? sum : 0.0f; }
XS(MOVSHDUP)(XMM* d, XMM s) { XMM t; t.d[0] = t.d[1] = s.d[1]; t.d[2] = t.d[3] = s.d[3]; *d = t; }
XS(MOVSLDUP)(XMM* d, XMM s) { XMM t; t.d[0] = t.d[1] = s.d[0]; t.d[2] = t.d[3] = s.d[2]; *d = t; }
XS(MOVDDUP)(XMM* d, XMM s) { d->q[0] = d->q[1] = s.q[0]; }

XSR(uint64_t, MOVMSKPS)(XMM s) { return (s.d[0] >> 31) | ((s.d[1] >> 31) << 1) | ((s.d[2] >> 31) << 2) | ((s.d[3] >> 31) << 3); }
XSR(uint64_t, MOVMSKPD)(XMM s) { return (s.q[0] >> 63) | ((s.q[1] >> 63) << 1); }
XSR(uint64_t, PMOVMSKB)(XMM s) { uint64_t m = 0; LANES(16) m |= (uint64_t)(s.b[i] >> 7) << i; return m; }

XS(PADDSB)(XMM* d, XMM s) { LANES(16) d->b[i] = (uint8_t)sat8((int8_t)d->b[i] + (int8_t)s.b[i]); }
XS(PSUBSB)(XMM* d, XMM s) { LANES(16) d->b[i] = (uint8_t)sat8((int8_t)d->b[i] - (int8_t)s.b[i]); }
XS(PADDSW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)sat16((int16_t)d->w[i] + (int16_t)s.w[i]); }
XS(PSUBSW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)sat16((int16_t)d->w[i] - (int16_t)s.w[i]); }
XS(PMINSB)(XMM* d, XMM s) { LANES(16) if ((int8_t)s.b[i] < (int8_t)d->b[i]) d->b[i] = s.b[i]; }
XS(PMAXSB)(XMM* d, XMM s) { LANES(16) if ((int8_t)s.b[i] > (int8_t)d->b[i]) d->b[i] = s.b[i]; }
XS(PMINUW)(XMM* d, XMM s) { LANES(8) if (s.w[i] < d->w[i]) d->w[i] = s.w[i]; }
XS(PMAXUW)(XMM* d, XMM s) { LANES(8) if (s.w[i] > d->w[i]) d->w[i] = s.w[i]; }
XS(PABSB)(XMM* d, XMM s) { LANES(16) { int v = (int8_t)s.b[i]; d->b[i] = (uint8_t)(v < 0 ? -v : v); } }
XS(PABSW)(XMM* d, XMM s) { LANES(8) { int v = (int16_t)s.w[i]; d->w[i] = (uint16_t)(v < 0 ? -v : v); } }
XFN(PSIGNB)(XMM* d, XMM s) { LANES(16) { int8_t k = (int8_t)s.b[i]; d->b[i] = k < 0 ? (uint8_t)-d->b[i] : k ? d->b[i] : 0; } }
XFN(PSIGNW)(XMM* d, XMM s) { LANES(8) { int16_t k = (int16_t)s.w[i]; d->w[i] = k < 0 ? (uint16_t)-d->w[i] : k ? d->w[i] : 0; } }
XFN(PSIGND)(XMM* d, XMM s) { LANES(4) { int32_t k = (int32_t)s.d[i]; d->d[i] = k < 0 ? (uint32_t)-d->d[i] : k ? d->d[i] : 0; } }
XS(PHADDW)(XMM* d, XMM s) { XMM t; LANES(4) { t.w[i] = (uint16_t)(d->w[2 * i] + d->w[2 * i + 1]); t.w[4 + i] = (uint16_t)(s.w[2 * i] + s.w[2 * i + 1]); } *d = t; }
XS(PHADDD)(XMM* d, XMM s) { XMM t; LANES(2) { t.d[i] = d->d[2 * i] + d->d[2 * i + 1]; t.d[2 + i] = s.d[2 * i] + s.d[2 * i + 1]; } *d = t; }
XS(PHSUBW)(XMM* d, XMM s) { XMM t; LANES(4) { t.w[i] = (uint16_t)(d->w[2 * i] - d->w[2 * i + 1]); t.w[4 + i] = (uint16_t)(s.w[2 * i] - s.w[2 * i + 1]); } *d = t; }
XS(PHSUBD)(XMM* d, XMM s) { XMM t; LANES(2) { t.d[i] = d->d[2 * i] - d->d[2 * i + 1]; t.d[2 + i] = s.d[2 * i] - s.d[2 * i + 1]; } *d = t; }
XS(PMADDUBSW)(XMM* d, XMM s) { XMM t; LANES(8) t.w[i] = (uint16_t)sat16(d->b[2 * i] * (int8_t)s.b[2 * i] + d->b[2 * i + 1] * (int8_t)s.b[2 * i + 1]); *d = t; }
XS(PMULHRSW)(XMM* d, XMM s) { LANES(8) d->w[i] = (uint16_t)((((int32_t)(int16_t)d->w[i] * (int16_t)s.w[i] >> 14) + 1) >> 1); }
XFN(PMOVZXBQ)(XMM* d, XMM s) { XMM t; LANES(2) t.q[i] = s.b[i]; *d = t; }
XFN(PMOVZXWQ)(XMM* d, XMM s) { XMM t; LANES(2) t.q[i] = s.w[i]; *d = t; }
XFN(PMOVSXBQ)(XMM* d, XMM s) { XMM t; LANES(2) t.q[i] = (uint64_t)(int64_t)(int8_t)s.b[i]; *d = t; }
XFN(PMOVSXWQ)(XMM* d, XMM s) { XMM t; LANES(2) t.q[i] = (uint64_t)(int64_t)(int16_t)s.w[i]; *d = t; }
XS(HSUBPS)(XMM* d, XMM s) { XMM t; t.f[0] = d->f[0] - d->f[1]; t.f[1] = d->f[2] - d->f[3]; t.f[2] = s.f[0] - s.f[1]; t.f[3] = s.f[2] - s.f[3]; *d = t; }
XS(HSUBPD)(XMM* d, XMM s) { XMM t; t.g[0] = d->g[0] - d->g[1]; t.g[1] = s.g[0] - s.g[1]; *d = t; }
XFN(ADDSUBPS)(XMM* d, XMM s) { d->f[0] -= s.f[0]; d->f[1] += s.f[1]; d->f[2] -= s.f[2]; d->f[3] += s.f[3]; }
XFN(ADDSUBPD)(XMM* d, XMM s) { d->g[0] -= s.g[0]; d->g[1] += s.g[1]; }
XFN(CVTPD2DQ)(XMM* d, XMM s) { XMM t; memset(&t, 0, 16); t.d[0] = (uint32_t)CVTR32(s.g[0]); t.d[1] = (uint32_t)CVTR32(s.g[1]); *d = t; }
XFN(MPSADBW)(XMM* d, XMM s, int imm)
{
    XMM t;
    int a = (imm >> 2 & 1) * 4, b = (imm & 3) * 4;
    LANES(8) { int sum = 0; for (int k = 0; k < 4; k++) { int x = d->b[a + i + k] - s.b[b + k]; sum += x < 0 ? -x : x; } t.w[i] = (uint16_t)sum; }
    *d = t;
}
XSR(void, BLENDVPS)(XMM* d, XMM s, XMM m) { LANES(4) if (m.d[i] >> 31) d->d[i] = s.d[i]; }
XSR(void, BLENDVPD)(XMM* d, XMM s, XMM m) { LANES(2) if (m.q[i] >> 63) d->q[i] = s.q[i]; }
XSR(void, PBLENDVB)(XMM* d, XMM s, XMM m) { LANES(16) if (m.b[i] >> 7) d->b[i] = s.b[i]; }

#ifdef __wasm_simd128__
#define VLD(p) wasm_v128_load(p)
#define VST(p, v) wasm_v128_store((p), (v))
XFN(ADDPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_add(a, b)); }
XFN(SUBPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_sub(a, b)); }
XFN(MULPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_mul(a, b)); }
XFN(DIVPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_div(a, b)); }
XFN(MINPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_pmin(b, a)); }
XFN(MAXPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_pmax(b, a)); }
XFN(SQRTPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_sqrt(b)); }
XFN(ADDPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_add(a, b)); }
XFN(SUBPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_sub(a, b)); }
XFN(MULPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_mul(a, b)); }
XFN(DIVPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_div(a, b)); }
XFN(MINPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_pmin(b, a)); }
XFN(MAXPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_pmax(b, a)); }
XFN(SQRTPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_sqrt(b)); }
XFN(HADDPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_add(wasm_i32x4_shuffle(a, b, 0, 2, 4, 6), wasm_i32x4_shuffle(a, b, 1, 3, 5, 7))); }
XFN(HSUBPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_sub(wasm_i32x4_shuffle(a, b, 0, 2, 4, 6), wasm_i32x4_shuffle(a, b, 1, 3, 5, 7))); }
XFN(HADDPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_add(wasm_i64x2_shuffle(a, b, 0, 2), wasm_i64x2_shuffle(a, b, 1, 3))); }
XFN(HSUBPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f64x2_sub(wasm_i64x2_shuffle(a, b, 0, 2), wasm_i64x2_shuffle(a, b, 1, 3))); }
XFN(ANDPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_v128_and(a, b)); }
XFN(ANDNPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_v128_andnot(b, a)); }
XFN(ORPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_v128_or(a, b)); }
XFN(XORPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_v128_xor(a, b)); }
XFN(PADDB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_add(a, b)); }
XFN(PADDW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_add(a, b)); }
XFN(PADDD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_add(a, b)); }
XFN(PADDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_add(a, b)); }
XFN(PSUBB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_sub(a, b)); }
XFN(PSUBW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_sub(a, b)); }
XFN(PSUBD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_sub(a, b)); }
XFN(PSUBQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_sub(a, b)); }
XFN(PADDUSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u8x16_add_sat(a, b)); }
XFN(PSUBUSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u8x16_sub_sat(a, b)); }
XFN(PADDUSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_add_sat(a, b)); }
XFN(PSUBUSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_sub_sat(a, b)); }
XFN(PADDSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_add_sat(a, b)); }
XFN(PSUBSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_sub_sat(a, b)); }
XFN(PADDSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_add_sat(a, b)); }
XFN(PSUBSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_sub_sat(a, b)); }
XFN(PMULLW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_mul(a, b)); }
XFN(PMULLD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_mul(a, b)); }
XFN(PMULHW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_shuffle(wasm_i32x4_extmul_low_i16x8(a, b), wasm_i32x4_extmul_high_i16x8(a, b), 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31)); }
XFN(PMULHUW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_shuffle(wasm_u32x4_extmul_low_u16x8(a, b), wasm_u32x4_extmul_high_u16x8(a, b), 2, 3, 6, 7, 10, 11, 14, 15, 18, 19, 22, 23, 26, 27, 30, 31)); }
XFN(PMULUDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u64x2_extmul_low_u32x4(wasm_i32x4_shuffle(a, a, 0, 2, 0, 2), wasm_i32x4_shuffle(b, b, 0, 2, 0, 2))); }
XFN(PMULDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_extmul_low_i32x4(wasm_i32x4_shuffle(a, a, 0, 2, 0, 2), wasm_i32x4_shuffle(b, b, 0, 2, 0, 2))); }
XFN(PMADDWD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_dot_i16x8(a, b)); }
XFN(PMULHRSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_v128_bitselect(wasm_i16x8_splat(-32768), wasm_i16x8_q15mulr_sat(a, b), wasm_v128_and(wasm_i16x8_eq(a, wasm_i16x8_splat(-32768)), wasm_i16x8_eq(b, wasm_i16x8_splat(-32768))))); }
XFN(PMADDUBSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_add_sat(wasm_i16x8_mul(wasm_v128_and(a, wasm_i16x8_splat(0xFF)), wasm_i16x8_shr(wasm_i16x8_shl(b, 8), 8)), wasm_i16x8_mul(wasm_u16x8_shr(a, 8), wasm_i16x8_shr(b, 8)))); }
XFN(PAVGB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u8x16_avgr(a, b)); }
XFN(PAVGW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_avgr(a, b)); }
XFN(PMINUB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u8x16_min(a, b)); }
XFN(PMAXUB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u8x16_max(a, b)); }
XFN(PMINSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_min(a, b)); }
XFN(PMAXSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_max(a, b)); }
XFN(PMINUW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_min(a, b)); }
XFN(PMAXUW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_max(a, b)); }
XFN(PMINSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_min(a, b)); }
XFN(PMAXSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_max(a, b)); }
XFN(PMINUD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u32x4_min(a, b)); }
XFN(PMAXUD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u32x4_max(a, b)); }
XFN(PMINSD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_min(a, b)); }
XFN(PMAXSD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_max(a, b)); }
XFN(PABSB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_abs(b)); }
XFN(PABSW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_abs(b)); }
XFN(PABSD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_abs(b)); }
XFN(PCMPEQB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_eq(a, b)); }
XFN(PCMPEQW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_eq(a, b)); }
XFN(PCMPEQD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_eq(a, b)); }
XFN(PCMPEQQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_eq(a, b)); }
XFN(PCMPGTB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_gt(a, b)); }
XFN(PCMPGTW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_gt(a, b)); }
XFN(PCMPGTD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_gt(a, b)); }
XFN(PCMPGTQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_gt(a, b)); }
XFN(PSHUFB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_swizzle(a, wasm_v128_and(b, wasm_i8x16_splat((int8_t)0x8F)))); }
XFN(UNPCKLPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_shuffle(a, b, 0, 4, 1, 5)); }
XFN(UNPCKHPS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_shuffle(a, b, 2, 6, 3, 7)); }
XFN(UNPCKLPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_shuffle(a, b, 0, 2)); }
XFN(UNPCKHPD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_shuffle(a, b, 1, 3)); }
XFN(PUNPCKLBW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_shuffle(a, b, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23)); }
XFN(PUNPCKHBW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_shuffle(a, b, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30, 15, 31)); }
XFN(PUNPCKLWD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_shuffle(a, b, 0, 8, 1, 9, 2, 10, 3, 11)); }
XFN(PUNPCKHWD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_shuffle(a, b, 4, 12, 5, 13, 6, 14, 7, 15)); }
XFN(PUNPCKLQDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_shuffle(a, b, 0, 2)); }
XFN(PUNPCKHQDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_shuffle(a, b, 1, 3)); }
XFN(PACKSSWB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i8x16_narrow_i16x8(a, b)); }
XFN(PACKUSWB)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u8x16_narrow_i16x8(a, b)); }
XFN(PACKSSDW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_narrow_i32x4(a, b)); }
XFN(PACKUSDW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_narrow_i32x4(a, b)); }
XFN(PMOVZXBW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u16x8_extend_low_u8x16(b)); }
XFN(PMOVZXWD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u32x4_extend_low_u16x8(b)); }
XFN(PMOVZXDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u64x2_extend_low_u32x4(b)); }
XFN(PMOVZXBD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_u32x4_extend_low_u16x8(wasm_u16x8_extend_low_u8x16(b))); }
XFN(PMOVSXBW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_extend_low_i8x16(b)); }
XFN(PMOVSXWD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_extend_low_i16x8(b)); }
XFN(PMOVSXDQ)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_extend_low_i32x4(b)); }
XFN(PMOVSXBD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_extend_low_i16x8(wasm_i16x8_extend_low_i8x16(b))); }
XFN(PHADDW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_add(wasm_i16x8_shuffle(a, b, 0, 2, 4, 6, 8, 10, 12, 14), wasm_i16x8_shuffle(a, b, 1, 3, 5, 7, 9, 11, 13, 15))); }
XFN(PHSUBW)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i16x8_sub(wasm_i16x8_shuffle(a, b, 0, 2, 4, 6, 8, 10, 12, 14), wasm_i16x8_shuffle(a, b, 1, 3, 5, 7, 9, 11, 13, 15))); }
XFN(PHADDD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_add(wasm_i32x4_shuffle(a, b, 0, 2, 4, 6), wasm_i32x4_shuffle(a, b, 1, 3, 5, 7))); }
XFN(PHSUBD)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_sub(wasm_i32x4_shuffle(a, b, 0, 2, 4, 6), wasm_i32x4_shuffle(a, b, 1, 3, 5, 7))); }
XFN(CVTDQ2PS)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_f32x4_convert_i32x4(b)); }
XFN(MOVSHDUP)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_shuffle(b, b, 1, 1, 3, 3)); }
XFN(MOVSLDUP)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i32x4_shuffle(b, b, 0, 0, 2, 2)); }
XFN(MOVDDUP)(XMM* d, XMM s) { v128_t a = VLD(d), b = VLD(&s); (void)a; (void)b; VST(d, wasm_i64x2_shuffle(b, b, 0, 0)); }
XFN(PSLLW)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, k > 15 ? wasm_i64x2_splat(0) : wasm_i16x8_shl(VLD(d), k)); }
XFN(PSLLD)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, k > 31 ? wasm_i64x2_splat(0) : wasm_i32x4_shl(VLD(d), k)); }
XFN(PSLLQ)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, k > 63 ? wasm_i64x2_splat(0) : wasm_i64x2_shl(VLD(d), k)); }
XFN(PSRLW)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, k > 15 ? wasm_i64x2_splat(0) : wasm_u16x8_shr(VLD(d), k)); }
XFN(PSRLD)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, k > 31 ? wasm_i64x2_splat(0) : wasm_u32x4_shr(VLD(d), k)); }
XFN(PSRLQ)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, k > 63 ? wasm_i64x2_splat(0) : wasm_u64x2_shr(VLD(d), k)); }
XFN(PSRAW)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, wasm_i16x8_shr(VLD(d), k > 15 ? 15 : k)); }
XFN(PSRAD)(XMM* d, XMM s) { unsigned k = shcount(s); VST(d, wasm_i32x4_shr(VLD(d), k > 31 ? 31 : k)); }
static inline v128_t lane32_idx(int a0, int a1, int a2, int a3)
{
    return wasm_i8x16_make(a0 * 4, a0 * 4 + 1, a0 * 4 + 2, a0 * 4 + 3, a1 * 4, a1 * 4 + 1, a1 * 4 + 2, a1 * 4 + 3,
                           a2 * 4, a2 * 4 + 1, a2 * 4 + 2, a2 * 4 + 3, a3 * 4, a3 * 4 + 1, a3 * 4 + 2, a3 * 4 + 3);
}
XFN(PSHUFD)(XMM* d, XMM s, int imm) { VST(d, wasm_i8x16_swizzle(VLD(&s), lane32_idx(imm & 3, imm >> 2 & 3, imm >> 4 & 3, imm >> 6 & 3))); }
XFN(PSHUFLW)(XMM* d, XMM s, int imm)
{
    int w0 = imm & 3, w1 = imm >> 2 & 3, w2 = imm >> 4 & 3, w3 = imm >> 6 & 3;
    VST(d, wasm_i8x16_swizzle(VLD(&s), wasm_i8x16_make(w0 * 2, w0 * 2 + 1, w1 * 2, w1 * 2 + 1, w2 * 2, w2 * 2 + 1, w3 * 2, w3 * 2 + 1,
                                                     8, 9, 10, 11, 12, 13, 14, 15)));
}
XFN(PSHUFHW)(XMM* d, XMM s, int imm)
{
    int w0 = 8 + (imm & 3) * 2, w1 = 8 + (imm >> 2 & 3) * 2, w2 = 8 + (imm >> 4 & 3) * 2, w3 = 8 + (imm >> 6 & 3) * 2;
    VST(d, wasm_i8x16_swizzle(VLD(&s), wasm_i8x16_make(0, 1, 2, 3, 4, 5, 6, 7, w0, w0 + 1, w1, w1 + 1, w2, w2 + 1, w3, w3 + 1)));
}
/* out-of-range swizzle indices give 0, so each source fills only its own half */
XFN(SHUFPS)(XMM* d, XMM s, int imm)
{
    v128_t lo = wasm_i8x16_swizzle(VLD(d), wasm_v128_or(lane32_idx(imm & 3, imm >> 2 & 3, 0, 0), wasm_i32x4_make(0, 0, -1, -1)));
    v128_t hi = wasm_i8x16_swizzle(VLD(&s), wasm_v128_or(lane32_idx(0, 0, imm >> 4 & 3, imm >> 6 & 3), wasm_i32x4_make(-1, -1, 0, 0)));
    VST(d, wasm_v128_or(lo, hi));
}
XFN(BLENDPS)(XMM* d, XMM s, int imm)
{
    v128_t m = wasm_i32x4_make(imm & 1 ? -1 : 0, imm & 2 ? -1 : 0, imm & 4 ? -1 : 0, imm & 8 ? -1 : 0);
    VST(d, wasm_v128_bitselect(VLD(&s), VLD(d), m));
}
XFN(BLENDPD)(XMM* d, XMM s, int imm)
{
    v128_t m = wasm_i64x2_make(imm & 1 ? -1 : 0, imm & 2 ? -1 : 0);
    VST(d, wasm_v128_bitselect(VLD(&s), VLD(d), m));
}
XFN(PBLENDW)(XMM* d, XMM s, int imm)
{
    v128_t m = wasm_i16x8_make(imm & 1 ? -1 : 0, imm & 2 ? -1 : 0, imm & 4 ? -1 : 0, imm & 8 ? -1 : 0,
                               imm & 16 ? -1 : 0, imm & 32 ? -1 : 0, imm & 64 ? -1 : 0, imm & 128 ? -1 : 0);
    VST(d, wasm_v128_bitselect(VLD(&s), VLD(d), m));
}
#define VCMP(T, a, b, p) \
    ((p & 7) == 0 ? wasm_##T##_eq(a, b) : (p & 7) == 1 ? wasm_##T##_lt(a, b) : (p & 7) == 2 ? wasm_##T##_le(a, b) \
     : (p & 7) == 3 ? wasm_v128_or(wasm_##T##_ne(a, a), wasm_##T##_ne(b, b)) : (p & 7) == 4 ? wasm_##T##_ne(a, b) \
     : (p & 7) == 5 ? wasm_v128_not(wasm_##T##_lt(a, b)) : (p & 7) == 6 ? wasm_v128_not(wasm_##T##_le(a, b)) \
     : wasm_v128_and(wasm_##T##_eq(a, a), wasm_##T##_eq(b, b)))
XFN(CMPPS)(XMM* d, XMM s, int p) { v128_t a = VLD(d), b = VLD(&s); VST(d, VCMP(f32x4, a, b, p)); }
XFN(CMPPD)(XMM* d, XMM s, int p) { v128_t a = VLD(d), b = VLD(&s); VST(d, VCMP(f64x2, a, b, p)); }
XFN(ROUNDPS)(XMM* d, XMM s, int imm)
{
    v128_t b = VLD(&s);
    int m = (imm & 4) ? 0 : (imm & 3);
    VST(d, m == 1 ? wasm_f32x4_floor(b) : m == 2 ? wasm_f32x4_ceil(b) : m == 3 ? wasm_f32x4_trunc(b) : wasm_f32x4_nearest(b));
}
XFN(ROUNDPD)(XMM* d, XMM s, int imm)
{
    v128_t b = VLD(&s);
    int m = (imm & 4) ? 0 : (imm & 3);
    VST(d, m == 1 ? wasm_f64x2_floor(b) : m == 2 ? wasm_f64x2_ceil(b) : m == 3 ? wasm_f64x2_trunc(b) : wasm_f64x2_nearest(b));
}
static inline uint64_t x_MOVMSKPS(XMM s) { return wasm_i32x4_bitmask(VLD(&s)); }
static inline uint64_t x_MOVMSKPD(XMM s) { return wasm_i64x2_bitmask(VLD(&s)); }
static inline uint64_t x_PMOVMSKB(XMM s) { return wasm_i8x16_bitmask(VLD(&s)); }
static inline void x_BLENDVPS(XMM* d, XMM s, XMM m) { VST(d, wasm_v128_bitselect(VLD(&s), VLD(d), wasm_i32x4_shr(VLD(&m), 31))); }
static inline void x_BLENDVPD(XMM* d, XMM s, XMM m) { VST(d, wasm_v128_bitselect(VLD(&s), VLD(d), wasm_i64x2_shr(VLD(&m), 63))); }
static inline void x_PBLENDVB(XMM* d, XMM s, XMM m) { VST(d, wasm_v128_bitselect(VLD(&s), VLD(d), wasm_i8x16_shr(VLD(&m), 7))); }
#endif
