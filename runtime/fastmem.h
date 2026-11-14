/* memcmp 16 bytes a step: the C library's compares a byte at a time, and the GPU layer compares megabytes a frame
   (constant buffers rewritten with the same contents) */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __wasm_simd128__
#include <wasm_simd128.h>
#endif

static inline int fast_memcmp(const void* a, const void* b, size_t n)
{
    const uint8_t *p = a, *q = b;
#ifdef __wasm_simd128__
    while (n >= 16) {
        if (wasm_v128_any_true(wasm_v128_xor(wasm_v128_load(p), wasm_v128_load(q)))) break;
        p += 16; q += 16; n -= 16;
    }
#else
    while (n >= 8) {
        uint64_t x, y;
        memcpy(&x, p, 8); memcpy(&y, q, 8);
        if (x != y) break;
        p += 8; q += 8; n -= 8;
    }
#endif
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
