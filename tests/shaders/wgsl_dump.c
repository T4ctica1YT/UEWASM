#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/dxbc.h"

int main(int argc, char** argv)
{
    if (argc < 3) return 2;
    char pat[1024];
    snprintf(pat, sizeof pat, "%s\\*.dxbc", argv[1]);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return 1;
    int total = 0, ok = 0;
    char errs[256][96];
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
        DxWgsl sh;
        /* UEW_LIB=1: prefixed library form wrapped in a compute shader, as the geometry shader emulation uses it */
        int lib = getenv("UEW_LIB") != NULL;
        DxWgslOpts op = { 0 };
        op.lib = lib;
        op.prefix = "a_";
        /* UEW_INTERP=1: honour pixel input interpolation, and give vertex outputs noperspective centroid */
        static const char all5[32] = { 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5 };
        if (getenv("UEW_INTERP")) { op.use_interp = 1; op.out_interp = all5; }
        int r = dxbc_to_wgsl_ex(buf, (size_t)n, &op, &sh);
        if (r && lib && sh.wgsl) {
            size_t len = strlen(dxbc_wgsl_helpers()) + strlen(sh.wgsl) + 256;
            char* t = malloc(len);
            snprintf(t, len, "%s%sfn uew_emit() {}\nfn uew_cut() {}\n@compute @workgroup_size(1) fn main() { a_uew_body(); }\n", dxbc_wgsl_helpers(), sh.wgsl);
            free(sh.wgsl);
            sh.wgsl = t;
        }
        total++;
        if (r && sh.wgsl && !sh.error[0]) {
            ok++;
            snprintf(outp, sizeof outp, "%s\\%.*s.wgsl", argv[2], (int)(strlen(fd.name) - 5), fd.name);
            FILE* o = fopen(outp, "wb");
            if (o) { fputs(sh.wgsl, o); fclose(o); }
        } else {
            int k = 0;
            for (; k < nerr; k++) if (!strcmp(errs[k], sh.error)) break;
            if (k == nerr && nerr < 256) { snprintf(errs[nerr], 96, "%s", sh.error); nerr++; }
            if (k < 256) errn[k]++;
        }
        dxbc_wgsl_free(&sh);
        free(buf);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    printf("translated %d / %d\n", ok, total);
    for (int k = 0; k < nerr; k++) printf("  %6d  %s\n", errn[k], errs[k]);
    return 0;
}
