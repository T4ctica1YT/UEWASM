/* uewasm-test: /MD (the CRT as a DLL, so its maths functions are the runtime's own implementations, not recompiled code)
   CRT maths called for real, through pointers so the compiler can't use intrinsics: x64 passes doubles in xmm registers,
   32-bit code passes them on the stack and takes the result from x87 st(0). Scaleform (Unreal Engine 3) sets a clip's
   _x/_y as _ftol2(floor(v * 20)). */
#include <math.h>
#include <stdio.h>

static double (*volatile p_floor)(double);
static double (*volatile p_ceil)(double);
static double (*volatile p_sin)(double);
static double (*volatile p_sqrt)(double);
static double (*volatile p_pow)(double, double);
static double (*volatile p_fmod)(double, double);
static double (*volatile p_atan2)(double, double);
static double (*volatile p_ldexp)(double, int);
static double (*volatile p_frexp)(double, int*);
static double (*volatile p_modf)(double, double*);
static float (*volatile p_floorf)(float);
static float (*volatile p_powf)(float, float);

static void p(const char* what, double v) { printf("%s %.12g\n", what, v); }

int main(void)
{
    double ip = 0;
    int e = 0;
    p_floor = floor;
    p_ceil = ceil;
    p_sin = sin;
    p_sqrt = sqrt;
    p_pow = pow;
    p_fmod = fmod;
    p_atan2 = atan2;
    p_ldexp = ldexp;
    p_frexp = frexp;
    p_modf = modf;
    p_floorf = floorf;
    p_powf = powf;
    p("floor", p_floor(2.75)); p("floor-", p_floor(-2.25)); p("ceil", p_ceil(-2.5)); p("sin", p_sin(1.0)); p("sqrt", p_sqrt(2.0));
    p("pow", p_pow(2.0, 10.0)); p("fmod", p_fmod(7.5, 2.0)); p("atan2", p_atan2(1.0, -1.0)); p("ldexp", p_ldexp(3.0, 4));
    p("frexp", p_frexp(48.0, &e)); printf("frexp.e %d\n", e);
    p("modf", p_modf(-3.25, &ip)); p("modf.i", ip);
    p("floorf", p_floorf(-1.5f)); p("powf", p_powf(3.0f, 2.0f));
    /* the result used straight away, as Scaleform does */
    printf("x_set %d %d\n", (int)p_floor(640.0 * 20.0), (int)p_floor(353.95 * 20.0));
    return 0;
}
