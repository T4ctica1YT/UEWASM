// OpenSSL's hand-written AES (libcrypto-3.dll from the Python install, the same assembly the EOS SDK carries): AES-256
// GCM, CBC and CTR through EVP and one block through AES_encrypt. Its internal routines don't keep the x64 ABI (the AES
// cores hand the state back in ebx, vpaes keeps its constants in xmm9-15 across calls). An argument sets
// OPENSSL_ia32cap, to force the path a recompiled game takes (the runtime reports no AES-NI).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef void* (*fn0)(void);
typedef int (*fninit)(void*, const void*, void*, const unsigned char*, const unsigned char*);
typedef int (*fnupd)(void*, unsigned char*, int*, const unsigned char*, int);
typedef int (*fnfin)(void*, unsigned char*, int*);
typedef int (*fnctrl)(void*, int, int, void*);
typedef int (*fnkey)(const unsigned char*, int, void*);
typedef void (*fnblk)(const unsigned char*, unsigned char*, const void*);

static unsigned h32(const unsigned char* p, int n)
{
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

int main(int argc, char** argv)
{
    if (argc > 1) { static char e[96]; snprintf(e, sizeof e, "OPENSSL_ia32cap=%s", argv[1]); _putenv(e); }
    HMODULE m = LoadLibraryA("libcrypto-3.dll");
    if (!m) { printf("no libcrypto-3.dll\n"); return 1; }
    fn0 ctx_new = (fn0)GetProcAddress(m, "EVP_CIPHER_CTX_new"), gcm = (fn0)GetProcAddress(m, "EVP_aes_256_gcm");
    fn0 cbc = (fn0)GetProcAddress(m, "EVP_aes_256_cbc"), ctr = (fn0)GetProcAddress(m, "EVP_aes_128_ctr");
    fninit ei = (fninit)GetProcAddress(m, "EVP_EncryptInit_ex"), di = (fninit)GetProcAddress(m, "EVP_DecryptInit_ex");
    fnupd eu = (fnupd)GetProcAddress(m, "EVP_EncryptUpdate"), du = (fnupd)GetProcAddress(m, "EVP_DecryptUpdate");
    fnfin ef = (fnfin)GetProcAddress(m, "EVP_EncryptFinal_ex"), df = (fnfin)GetProcAddress(m, "EVP_DecryptFinal_ex");
    fnctrl ctrl = (fnctrl)GetProcAddress(m, "EVP_CIPHER_CTX_ctrl");
    fnkey sk = (fnkey)GetProcAddress(m, "AES_set_encrypt_key");
    fnblk be = (fnblk)GetProcAddress(m, "AES_encrypt");
    static unsigned char key[32], iv[16], pt[2803], ct[3000], back[3000], tag[16], ks[512], blk[16];
    for (int i = 0; i < 32; i++) key[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < 16; i++) iv[i] = (unsigned char)(i * 13 + 5);
    for (int i = 0; i < (int)sizeof pt; i++) pt[i] = (unsigned char)(i * 31 + (i >> 5));
    int n, f;
    if (sk && be) {
        sk(key, 256, ks);
        be(iv, blk, ks);
        printf("AES_encrypt %08x\n", h32(blk, 16));
    }
    void* c = ctx_new();
    ei(c, gcm(), NULL, NULL, NULL); ctrl(c, 0x9, 12, NULL); ei(c, NULL, NULL, key, iv);   /* EVP_CTRL_GCM_SET_IVLEN */
    eu(c, NULL, &n, (const unsigned char*)"aad", 3); eu(c, ct, &n, pt, (int)sizeof pt); ef(c, ct + n, &f);
    ctrl(c, 0x10, 16, tag);   /* EVP_CTRL_GCM_GET_TAG */
    printf("gcm %08x tag %08x\n", h32(ct, (int)sizeof pt), h32(tag, 16));
    c = ctx_new();
    di(c, gcm(), NULL, NULL, NULL); ctrl(c, 0x9, 12, NULL); di(c, NULL, NULL, key, iv);
    du(c, NULL, &n, (const unsigned char*)"aad", 3); du(c, back, &n, ct, (int)sizeof pt);
    ctrl(c, 0x11, 16, tag);   /* EVP_CTRL_GCM_SET_TAG */
    int ok = df(c, back + n, &f);
    printf("gcm decrypt %s, %s\n", ok > 0 ? "tag ok" : "tag FAILED", memcmp(back, pt, sizeof pt) ? "plain differs" : "plain back");
    c = ctx_new();
    ei(c, cbc(), NULL, key, iv); eu(c, ct, &n, pt, (int)sizeof pt); ef(c, ct + n, &f);
    printf("cbc %08x (%d bytes)\n", h32(ct, n + f), n + f);
    c = ctx_new();
    ei(c, ctr(), NULL, key, iv); eu(c, ct, &n, pt, (int)sizeof pt);
    printf("ctr %08x\n", h32(ct, n));
    return 0;
}
