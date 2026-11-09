#include <setjmp.h>
#include <stdio.h>

static jmp_buf outer, inner;
static int depth;

static void fail(int code)
{
    depth++;
    if (depth < 5) fail(code + 1);
    longjmp(inner, code);
}

static int guarded(int n)
{
    volatile int tries = 0;
    int r = setjmp(inner);
    if (r) {
        printf("caught %d after %d tries\n", r, tries);
        if (++tries < n) { depth = 0; fail(r * 10); }
        return r;
    }
    fail(1);
    return -1;
}

/* MSVC turns these into jumps between functions; the recompiled build must not grow its stack */
__declspec(noinline) unsigned odd(unsigned n, unsigned acc);
__declspec(noinline) unsigned even(unsigned n, unsigned acc) { return n ? odd(n - 1, acc * 31 + n) : acc; }
__declspec(noinline) unsigned odd(unsigned n, unsigned acc) { return n ? even(n - 1, acc ^ n) : acc + 1; }

int main(void)
{
    printf("tail chain %u\n", even(200000, 1));
    int r = setjmp(outer);
    if (r == 0) {
        printf("guarded -> %d\n", guarded(3));
        longjmp(outer, 7);
    }
    printf("outer resumed with %d\n", r);
    return 0;
}
