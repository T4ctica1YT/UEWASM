#include "gpu.h"

static uint64_t frames, t_last;

static void null_present(GpuRes* bb, uint32_t w, uint32_t h)
{
    (void)bb;
    frames++;
    uint64_t t = host_ticks_ns();
    if (frames <= 5 || frames % 100 == 0) {
        host_log("[gpu] frame %llu (%ux%u, null renderer, %.1f ms, %llu MB guest)", (unsigned long long)frames, w, h,
                 t_last ? (t - t_last) / 1e6 : 0.0, (unsigned long long)(g_mem_used() >> 20));
    }
    t_last = t;
}

static GpuBackend null_backend = { "Null Renderer", .present = null_present };

GpuBackend* gpu_null_backend(void) { return &null_backend; }

#ifdef UEW_WEB_GPU
GpuBackend* gpu_gl_backend(void);
GpuBackend* gpu_wgpu_backend(void);
GpuBackend* gpu_web_backend(void)
{
#ifdef UEW_WEBGPU
    GpuBackend* b = gpu_wgpu_backend();
    if (b) return b;
    host_log("[gpu] WebGPU unavailable, using WebGL 2");
#endif
    return gpu_gl_backend();
}
#else
GpuBackend* gpu_web_backend(void) { return NULL; }
#endif

uint32_t dxgi_format_bytes(uint32_t f, uint32_t* block)
{
    *block = 1;
    switch (f) {
    case 1: case 2: case 3: case 4: return 16;
    case 5: case 6: case 7: case 8: return 12;
    case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22: return 8;
    case 23: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 35: case 36:
    case 37: case 38: case 39: case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47: return 4;
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57: case 58: case 59: return 2;
    case 60: case 61: case 62: case 63: case 64: case 65: return 1;
    case 67: case 68: case 69: return 4;
    case 70: case 71: case 72: case 79: case 80: case 81: *block = 4; return 8;
    case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84: case 94: case 95: case 96: case 97: case 98: case 99: *block = 4; return 16;
    case 85: case 86: return 2;
    case 87: case 88: case 89: case 90: case 91: case 92: case 93: return 4;
    case 115: return 2;
    default: return 4;
    }
}
