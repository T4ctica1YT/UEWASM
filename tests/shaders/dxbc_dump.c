#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/dxbc.h"

#ifdef _WIN32
#include <io.h>
#endif

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*.dxbc", argv[1]);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 1;
    int total = 0, ok = 0;
    char errs[256][64];
    int errn[256] = { 0 }, nerr = 0;
    do {
        char path[1200], outp[1200];
        snprintf(path, sizeof path, "%s\\%s", argv[1], fd.name);
        FILE* f = fopen(path, "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        unsigned char* buf = malloc((size_t)n);
        fread(buf, 1, (size_t)n, f);
        fclose(f);
        if (getenv("DXBC_TRACE")) { fprintf(stderr, "%s\n", fd.name); fflush(stderr); }
        DxShader sh;
        int r = dxbc_translate(buf, (size_t)n, &sh);
        total++;
        if (r && sh.glsl && !sh.error[0]) {
            ok++;
            snprintf(outp, sizeof outp, "%s\\%.*s.glsl", argv[2], (int)(strlen(fd.name) - 5), fd.name);
            FILE* o = fopen(outp, "wb");
            if (o) { fputs(sh.glsl, o); fclose(o); }
        } else {
            int k = 0;
            for (; k < nerr; k++) if (!strcmp(errs[k], sh.error)) break;
            if (k == nerr && nerr < 256) { snprintf(errs[nerr], 64, "%s", sh.error); nerr++; }
            if (k < 256) errn[k]++;
        }
        dxbc_free(&sh);
        free(buf);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    printf("translated %d / %d\n", ok, total);
    for (int k = 0; k < nerr; k++) printf("  %6d  %s\n", errn[k], errs[k]);
    return 0;
}
