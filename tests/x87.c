/* x87 floating point as 32-bit MSVC code uses it (Unreal Engine 3, Scaleform and the CRT's maths helpers): loads and
   stores in every width, integer conversion under each rounding mode, compares read through fnstsw/sahf and fcomi,
   the transcendental and scaling instructions, and the control word. Built only as a 32-bit program
   (python tests/run_tests.py --x86 x87); the 64-bit build prints a single line. Results are printed rounded to 12
   significant digits, as the recompiled code computes in double precision. */
#include <stdio.h>
#include <math.h>

#if defined(_M_IX86)
static const double vals[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 2.5, -2.5, 3.75, -3.75, 1e10, -1e-10, 123456.789, 0.1, 7.0, 1e300 };
#define NV (sizeof vals / sizeof vals[0])

static void p(const char* what, double v) { printf("%s %.12g\n", what, v); }
static void pi(const char* what, long long v) { printf("%s %lld\n", what, v); }

/* fistp under each rounding mode (nearest, down, up, truncate) */
static void rounding(double x)
{
    for (int rc = 0; rc < 4; rc++) {
        unsigned short cw, ncw;
        int r32;
        long long r64;
        __asm {
            fnstcw cw
            mov ax, cw
            and ax, 0F3FFh
            mov cx, word ptr rc
            shl cx, 10
            or ax, cx
            mov ncw, ax
            fldcw ncw
            fld x
            fist r32
            fistp r64
            fldcw cw
        }
        printf("round rc%d %.12g -> %d %lld\n", rc, x, x > 2e9 || x < -2e9 ? 0 : r32, x > 9e18 || x < -9e18 ? 0LL : r64);
    }
}

/* compares: fcomp + fnstsw + sahf (the MSVC pattern), test ah masks, and fcomi */
static void compares(double a, double b)
{
    unsigned short sw;
    int c_lt = 0, c_eq = 0, c_gt = 0, c_ilt = 0;
    __asm {
        fld b
        fld a
        fcompp
        fnstsw ax
        mov sw, ax
        sahf
        setb byte ptr c_lt
        sete byte ptr c_eq
        seta byte ptr c_gt
    }
    __asm {
        fld b
        fld a
        fcomip st, st(1)
        fstp st(0)
        setb byte ptr c_ilt
    }
    printf("cmp %.12g %.12g: sw %04x b%d e%d a%d i%d test41 %d test05 %d\n", a, b, sw & 0x4500, c_lt & 1, c_eq & 1, c_gt & 1, c_ilt & 1,
           (sw & 0x4100) != 0, (sw & 0x0500) != 0);
}

static double un(const char* op, double x)
{
    double r = 0;
    switch (op[0]) {
    case 'c': __asm { fld x
                      fchs
                      fstp r } break;
    case 'a': __asm { fld x
                      fabs
                      fstp r } break;
    case 's': __asm { fld x
                      fabs
                      fsqrt
                      fstp r } break;
    case 'r': __asm { fld x
                      frndint
                      fstp r } break;
    case 'n': __asm { fld x
                      fsin
                      fstp r } break;
    case 'o': __asm { fld x
                      fcos
                      fstp r } break;
    case 't': __asm { fld x
                      fptan
                      fstp st(0)
                      fstp r } break;
    case 'l': __asm { fld1
                      fld x
                      fabs
                      fld1
                      faddp st(1), st
                      fyl2x
                      fstp r } break;
    case 'e': __asm { fld x
                      f2xm1
                      fstp r } break;
    }
    return r;
}

/* Scaleform's GMatrix3D(const GMatrix2D&): fst stores of a 0 and a 1 kept on the stack between plain loads and stores */
static void m2d_to_3d(const float* m, float* o)
{
    __asm {
        mov eax, o
        mov ecx, m
        fld dword ptr [ecx]
        fstp dword ptr [eax]
        fld dword ptr [ecx+4]
        fstp dword ptr [eax+10h]
        fldz
        fst dword ptr [eax+20h]
        fld dword ptr [ecx+8]
        fstp dword ptr [eax+30h]
        fld dword ptr [ecx+0Ch]
        fstp dword ptr [eax+4]
        fld dword ptr [ecx+10h]
        fstp dword ptr [eax+14h]
        fst dword ptr [eax+24h]
        fld dword ptr [ecx+14h]
        fstp dword ptr [eax+34h]
        fst dword ptr [eax+8]
        fst dword ptr [eax+18h]
        fld1
        fst dword ptr [eax+28h]
        fstp dword ptr [eax+3Ch]
        fst dword ptr [eax+38h]
        fst dword ptr [eax+0Ch]
        fst dword ptr [eax+1Ch]
        fstp dword ptr [eax+2Ch]
    }
}

/* Scaleform's GMatrix2D::Prepend: five-deep stack with fmulp st(4)/st(3), fxch st(3) and faddp st(2) for the translation */
static void m2d_prepend(float* m, const float* b)
{
    float t[6];
    __asm {
        mov eax, m
        mov ecx, b
        lea edx, t
        fld dword ptr [eax]
        fstp dword ptr [edx]
        fld dword ptr [eax+4]
        fstp dword ptr [edx+4]
        fld dword ptr [eax+8]
        fstp dword ptr [edx+8]
        fld dword ptr [eax+0Ch]
        fstp dword ptr [edx+0Ch]
        fld dword ptr [eax+10h]
        fstp dword ptr [edx+10h]
        fld dword ptr [eax+14h]
        fstp dword ptr [edx+14h]
        fld dword ptr [ecx+0Ch]
        fld dword ptr [edx+4]
        fld st(0)
        fmulp st(2), st
        fld dword ptr [ecx]
        fld dword ptr [edx]
        fld st(0)
        fmulp st(2), st
        fxch st(3)
        faddp st(1), st
        fstp dword ptr [eax]
        fld dword ptr [ecx+0Ch]
        fld dword ptr [edx+10h]
        fld st(0)
        fmulp st(2), st
        fld dword ptr [ecx]
        fld dword ptr [edx+0Ch]
        fld st(0)
        fmulp st(2), st
        fxch st(3)
        faddp st(1), st
        fstp dword ptr [eax+0Ch]
        fld dword ptr [ecx+4]
        fmul st, st(4)
        fld st(3)
        fmul dword ptr [ecx+10h]
        faddp st(1), st
        fstp dword ptr [eax+4]
        fld dword ptr [ecx+4]
        fmul st, st(2)
        fld st(1)
        fmul dword ptr [ecx+10h]
        faddp st(1), st
        fstp dword ptr [eax+10h]
        fld dword ptr [ecx+8]
        fmulp st(4), st
        fld dword ptr [ecx+14h]
        fmulp st(3), st
        fxch st(3)
        faddp st(2), st
        fld dword ptr [edx+8]
        faddp st(2), st
        fxch st(1)
        fstp dword ptr [eax+8]
        fmul dword ptr [ecx+8]
        fld dword ptr [ecx+14h]
        fmulp st(2), st
        faddp st(1), st
        fadd dword ptr [edx+14h]
        fstp dword ptr [eax+14h]
    }
}

int main(void)
{
    {
        float m[6] = { 1.f, 0.f, 12800.f, 0.f, 1.f, 7079.f }, b[6] = { 0.949f, -0.033f, 7.f, 0.033f, 0.949f, -2335.f };
        m2d_prepend(m, b);
        printf("m2d prepend %g %g %g %g %g %g\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    {
        float m[6] = { 0.95f, 0.03f, 12807.f, -0.03f, 0.95f, 4744.f }, o[16];
        m2d_to_3d(m, o);
        printf("m2d->3d");
        for (int i = 0; i < 16; i++) printf(" %g", o[i]);
        printf("\n");
    }
    for (unsigned i = 0; i < NV; i++) {
        double x = vals[i];
        if (fabs(x) < 1e9) rounding(x);
        p("chs", un("c", x)); p("abs", un("a", x)); p("sqrt", un("s", x)); p("rndint", un("r", x));
        if (fabs(x) < 1e6) { p("sin", un("n", x)); p("cos", un("o", x)); p("tan", un("t", x)); }
        p("log2(1+|x|)", un("l", x));
        if (fabs(x) <= 1) p("2^x-1", un("e", x));
        for (unsigned j = 0; j < NV; j += 3) compares(x, vals[j]);
    }
    /* binary operations in both directions and with memory operands */
    double a = 7.25, b = -2.5, r1, r2, r3, r4, r5, r6, r7;
    float fa = 3.5f;
    int ia = -7;
    short sa = 9;
    __asm {
        fld a
        fld b
        fsubp st(1), st
        fstp r1
        fld a
        fld b
        fsubrp st(1), st
        fstp r2
        fld a
        fdiv b
        fstp r3
        fld a
        fdivr b
        fstp r4
        fld a
        fimul ia
        fiadd sa
        fsub fa
        fstp r5
        fld a
        fld b
        fprem
        fstp r6
        fstp st(0)
        fld b
        fld a
        fscale
        fstp r7
        fstp st(0)
    }
    p("sub", r1); p("subr", r2); p("div", r3); p("divr", r4); p("mixed", r5); p("prem", r6); p("scale", r7);
    /* fxch, fist(p) widths, fld/fst float and the stack */
    double s0, s1, s2;
    int i32;
    short i16;
    long long i64;
    float f32;
    __asm {
        fld1
        fldpi
        fldl2e
        fxch st(2)
        fstp s0
        fstp s1
        fst f32
        fistp i64
        fldln2
        fldlg2
        fmulp st(1), st
        fstp s2
        fld a
        fistp i16
        fld b
        fistp i32
    }
    p("xch0", s0); p("xch1", s1); p("f32", f32); pi("i64", i64); p("ln2lg2", s2); pi("i16", i16); pi("i32", i32);
    /* status word condition codes from fxam/ftst, as code branching on NaN and infinity reads them */
    double probes[] = { 0.0, -1.0, 1.0, 1e308 * 10.0, 0.0 / 1.0 };
    for (int i = 0; i < 5; i++) {
        double v = probes[i];
        unsigned short sw1, sw2;
        __asm {
            fld v
            fxam
            fnstsw ax
            mov sw1, ax
            ftst
            fnstsw ax
            mov sw2, ax
            fstp st(0)
        }
        printf("fxam %.12g: %04x ftst %04x\n", v, sw1 & 0x4700, sw2 & 0x4500);
    }
    return 0;
}
#else
int main(void) { puts("x87: 32-bit only"); return 0; }
#endif
