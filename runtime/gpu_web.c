#if defined(__EMSCRIPTEN__) && defined(UEW_WEB_GPU)
#include "dxbc.h"
#include "gpu.h"

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <emscripten/html5.h>
#include <stdio.h>
#include <stdlib.h>

#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT 0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT 0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT 0x83F3
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT 0x8C4D
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT 0x8C4E
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#define GL_COMPRESSED_RED_RGTC1_EXT 0x8DBB
#define GL_COMPRESSED_SIGNED_RED_RGTC1_EXT 0x8DBC
#define GL_COMPRESSED_RED_GREEN_RGTC2_EXT 0x8DBD
#define GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT 0x8DBE
#define GL_COMPRESSED_RGBA_BPTC_UNORM_EXT 0x8E8C
#define GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT 0x8E8D
#define GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT 0x8E8E
#define GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT 0x8E8F
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE

typedef struct { GLenum ifmt, fmt, type; int compressed, depth, stencil, bgra, integer; } GlFmt;

static GlFmt glfmt(uint32_t f)
{
    switch (f) {
    case 1: case 2: return (GlFmt){ GL_RGBA32F, GL_RGBA, GL_FLOAT };
    case 3: return (GlFmt){ GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, 0, 1 };
    case 4: return (GlFmt){ GL_RGBA32I, GL_RGBA_INTEGER, GL_INT, 0, 0, 0, 0, 1 };
    case 5: case 6: return (GlFmt){ GL_RGB32F, GL_RGB, GL_FLOAT };
    case 9: case 10: return (GlFmt){ GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
    case 11: case 13: return (GlFmt){ GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT };
    case 12: return (GlFmt){ GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, 0, 0, 0, 0, 1 };
    case 14: return (GlFmt){ GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, 0, 0, 0, 0, 1 };
    case 15: case 16: return (GlFmt){ GL_RG32F, GL_RG, GL_FLOAT };
    case 17: return (GlFmt){ GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, 0, 1 };
    case 19: case 20: case 21: case 22: return (GlFmt){ GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 0, 1, 1 };
    case 23: case 24: return (GlFmt){ GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV };
    case 25: return (GlFmt){ GL_RGB10_A2UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT_2_10_10_10_REV, 0, 0, 0, 0, 1 };
    case 26: return (GlFmt){ GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV };
    case 27: case 28: return (GlFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
    case 29: return (GlFmt){ GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE };
    case 30: return (GlFmt){ GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, 0, 0, 0, 0, 1 };
    case 31: return (GlFmt){ GL_RGBA8_SNORM, GL_RGBA, GL_BYTE };
    case 32: return (GlFmt){ GL_RGBA8I, GL_RGBA_INTEGER, GL_BYTE, 0, 0, 0, 0, 1 };
    case 33: case 34: case 35: case 37: return (GlFmt){ GL_RG16F, GL_RG, GL_HALF_FLOAT };
    case 36: return (GlFmt){ GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, 0, 0, 0, 0, 1 };
    case 38: return (GlFmt){ GL_RG16I, GL_RG_INTEGER, GL_SHORT, 0, 0, 0, 0, 1 };
    case 39: case 40: return (GlFmt){ GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 0, 1 };
    case 41: return (GlFmt){ GL_R32F, GL_RED, GL_FLOAT };
    case 42: return (GlFmt){ GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, 0, 0, 0, 0, 1 };
    case 43: return (GlFmt){ GL_R32I, GL_RED_INTEGER, GL_INT, 0, 0, 0, 0, 1 };
    case 44: case 45: case 46: case 47: return (GlFmt){ GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0, 1, 1 };
    case 48: case 49: return (GlFmt){ GL_RG8, GL_RG, GL_UNSIGNED_BYTE };
    case 50: return (GlFmt){ GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, 0, 0, 0, 0, 1 };
    case 51: return (GlFmt){ GL_RG8_SNORM, GL_RG, GL_BYTE };
    case 53: case 55: return (GlFmt){ GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0, 1 };
    case 54: case 56: case 58: return (GlFmt){ GL_R16F, GL_RED, GL_HALF_FLOAT };
    case 57: return (GlFmt){ GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, 0, 0, 0, 0, 1 };
    case 59: return (GlFmt){ GL_R16I, GL_RED_INTEGER, GL_SHORT, 0, 0, 0, 0, 1 };
    case 60: case 61: case 65: return (GlFmt){ GL_R8, GL_RED, GL_UNSIGNED_BYTE };
    case 62: return (GlFmt){ GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, 0, 0, 0, 0, 1 };
    case 63: return (GlFmt){ GL_R8_SNORM, GL_RED, GL_BYTE };
    case 64: return (GlFmt){ GL_R8I, GL_RED_INTEGER, GL_BYTE, 0, 0, 0, 0, 1 };
    case 67: return (GlFmt){ GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV };
    case 70: case 71: return (GlFmt){ GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, 0, 0, 1 };
    case 72: return (GlFmt){ GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, 0, 0, 1 };
    case 73: case 74: return (GlFmt){ GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, 0, 0, 1 };
    case 75: return (GlFmt){ GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, 0, 0, 1 };
    case 76: case 77: return (GlFmt){ GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 0, 0, 1 };
    case 78: return (GlFmt){ GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, 0, 0, 1 };
    case 79: case 80: return (GlFmt){ GL_COMPRESSED_RED_RGTC1_EXT, 0, 0, 1 };
    case 81: return (GlFmt){ GL_COMPRESSED_SIGNED_RED_RGTC1_EXT, 0, 0, 1 };
    case 82: case 83: return (GlFmt){ GL_COMPRESSED_RED_GREEN_RGTC2_EXT, 0, 0, 1 };
    case 84: return (GlFmt){ GL_COMPRESSED_SIGNED_RED_GREEN_RGTC2_EXT, 0, 0, 1 };
    case 85: case 86: return (GlFmt){ GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5 };
    case 87: case 88: case 90: case 92: return (GlFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, 0, 0, 0, 1 };
    case 91: case 93: return (GlFmt){ GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, 0, 0, 0, 1 };
    case 94: case 95: return (GlFmt){ GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT, 0, 0, 1 };
    case 96: return (GlFmt){ GL_COMPRESSED_RGB_BPTC_SIGNED_FLOAT_EXT, 0, 0, 1 };
    case 97: case 98: return (GlFmt){ GL_COMPRESSED_RGBA_BPTC_UNORM_EXT, 0, 0, 1 };
    case 99: return (GlFmt){ GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT, 0, 0, 1 };
    default: return (GlFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
    }
}

typedef struct { GLuint buf, ibuf, tex; GLenum target; GlFmt f; int buf_tex_w; } GlRes;
typedef struct { GLuint vs, fs; DxShader dx; int ok; } GlShader;
typedef struct Prog { GlShader *vs, *ps; uint32_t bgra; GLuint prog; GLint mips_loc[2], size_loc[2]; struct Prog* next; } Prog;

static EMSCRIPTEN_WEBGL_CONTEXT_HANDLE glctx;
static GLuint vao, blit_read_fbo;
static Prog* progs[4096];
static int has_s3tc, has_bptc, has_rgtc, has_aniso, has_cbf;
static uint64_t frames, draws;

static void init_gl(void)
{
    if (glctx) return;
    EmscriptenWebGLContextAttributes a;
    emscripten_webgl_init_context_attributes(&a);
    a.majorVersion = 2;
    a.minorVersion = 0;
    a.alpha = 0;
    a.depth = 0;
    a.stencil = 0;
    a.antialias = 0;
    a.preserveDrawingBuffer = 0;
    a.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    glctx = emscripten_webgl_create_context("#canvas", &a);
    if (glctx <= 0) { host_log("[gl] WebGL2 context creation failed"); return; }
    emscripten_webgl_make_context_current(glctx);
    has_s3tc = emscripten_webgl_enable_extension(glctx, "WEBGL_compressed_texture_s3tc");
    emscripten_webgl_enable_extension(glctx, "WEBGL_compressed_texture_s3tc_srgb");
    has_bptc = emscripten_webgl_enable_extension(glctx, "EXT_texture_compression_bptc");
    has_rgtc = emscripten_webgl_enable_extension(glctx, "EXT_texture_compression_rgtc");
    has_aniso = emscripten_webgl_enable_extension(glctx, "EXT_texture_filter_anisotropic");
    has_cbf = emscripten_webgl_enable_extension(glctx, "EXT_color_buffer_float");
    emscripten_webgl_enable_extension(glctx, "EXT_color_buffer_half_float");
    emscripten_webgl_enable_extension(glctx, "OES_texture_float_linear");
    emscripten_webgl_enable_extension(glctx, "EXT_float_blend");
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenFramebuffers(1, &blit_read_fbo);
    host_log("[gl] WebGL2 ready (s3tc %d, bptc %d, rgtc %d, float targets %d)", has_s3tc, has_bptc, has_rgtc, has_cbf);
}

static GlRes* GR(GpuRes* r) { return r ? (GlRes*)r->backend : NULL; }

static GLenum tex_target(GpuRes* r)
{
    if (r->type == RES_TEX3D) return GL_TEXTURE_3D;
    if (r->misc & 4) return GL_TEXTURE_CUBE_MAP;
    if (r->array > 1) return GL_TEXTURE_2D_ARRAY;
    return GL_TEXTURE_2D;
}

static void res_create(GpuRes* r, uint64_t init)
{
    (void)init;
    init_gl();
    GlRes* g = calloc(1, sizeof *g);
    r->backend = g;
    if (r->type == RES_BUFFER) {
        if (r->bind & 2) {
            glGenBuffers(1, &g->ibuf);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibuf);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, r->width, NULL, r->usage == 2 ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        }
        if (r->bind & ~2u) {
            glGenBuffers(1, &g->buf);
            glBindBuffer(GL_COPY_WRITE_BUFFER, g->buf);
            glBufferData(GL_COPY_WRITE_BUFFER, (r->width + 15) & ~15u, NULL, r->usage == 2 ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        }
        if (r->bind & 8) {
            g->buf_tex_w = 4096;
            glGenTextures(1, &g->tex);
            glBindTexture(GL_TEXTURE_2D, g->tex);
            uint32_t words = (r->width + 3) / 4, rows = (words + 4095) / 4096;
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_R32UI, 4096, rows ? rows : 1);
        }
        return;
    }
    g->f = glfmt(r->format);
    g->target = tex_target(r);
    if (g->f.compressed && ((!has_s3tc && r->format <= 78) || (!has_bptc && r->format >= 94) || (!has_rgtc && r->format >= 79 && r->format <= 84)))
        g->f = (GlFmt){ GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE };
    glGenTextures(1, &g->tex);
    glBindTexture(g->target, g->tex);
    if (g->target == GL_TEXTURE_3D) glTexStorage3D(g->target, r->mips, g->f.ifmt, r->width, r->height, r->depth);
    else if (g->target == GL_TEXTURE_2D_ARRAY) glTexStorage3D(g->target, r->mips, g->f.ifmt, r->width, r->height, r->array);
    else glTexStorage2D(g->target, r->mips, g->f.ifmt, r->width, r->height);
    glTexParameteri(g->target, GL_TEXTURE_MIN_FILTER, r->mips > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
}

static uint8_t* swizzle_bgra(const uint8_t* src, size_t n)
{
    uint8_t* d = malloc(n);
    for (size_t i = 0; i + 3 < n; i += 4) { d[i] = src[i + 2]; d[i + 1] = src[i + 1]; d[i + 2] = src[i]; d[i + 3] = src[i + 3]; }
    return d;
}

static void res_upload(GpuRes* r, uint32_t sub, const int32_t* box, uint64_t src, uint32_t pitch, uint32_t slice)
{
    GlRes* g = GR(r);
    if (!g) return;
    const uint8_t* p = G2H(src);
    if (r->type == RES_BUFFER) {
        uint32_t off = box ? (uint32_t)box[0] : 0, n = box ? (uint32_t)(box[3] - box[0]) : r->width;
        if (g->ibuf) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g->ibuf); glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, off, n, p); }
        if (g->buf) { glBindBuffer(GL_COPY_WRITE_BUFFER, g->buf); glBufferSubData(GL_COPY_WRITE_BUFFER, off, n, p); }
        if (g->tex && r->backing) {
            uint32_t words = (r->width + 3) / 4, rows = (words + 4095) / 4096;
            glBindTexture(GL_TEXTURE_2D, g->tex);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            uint32_t full = words / 4096;
            if (full) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4096, full, GL_RED_INTEGER, GL_UNSIGNED_INT, G2H(r->backing));
            if (words % 4096) glTexSubImage2D(GL_TEXTURE_2D, 0, 0, full, words % 4096, 1, GL_RED_INTEGER, GL_UNSIGNED_INT, G2H(r->backing + 16384ull * full));
            (void)rows;
        }
        return;
    }
    uint32_t mip = sub % r->mips, layer = sub / r->mips;
    uint32_t w = r->width >> mip, h = r->height >> mip, d = r->type == RES_TEX3D ? r->depth >> mip : 1;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    int32_t x0 = 0, y0 = 0, z0 = 0;
    if (box) { x0 = box[0]; y0 = box[1]; z0 = box[2]; w = (uint32_t)(box[3] - box[0]); h = (uint32_t)(box[4] - box[1]); d = (uint32_t)(box[5] - box[2]); }
    glBindTexture(g->target, g->tex);
    if (g->f.compressed) {
        uint32_t block;
        uint32_t bpb = dxgi_format_bytes(r->format, &block);
        uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, tight = bw * bpb;
        uint8_t* tmp = NULL;
        const uint8_t* data = p;
        if (pitch != tight) {
            tmp = malloc((size_t)tight * bh * d);
            for (uint32_t z = 0; z < d; z++)
                for (uint32_t y = 0; y < bh; y++) memcpy(tmp + ((size_t)z * bh + y) * tight, p + (size_t)z * slice + (size_t)y * pitch, tight);
            data = tmp;
        }
        GLsizei size = (GLsizei)(tight * bh * d);
        if (g->target == GL_TEXTURE_2D) glCompressedTexSubImage2D(GL_TEXTURE_2D, mip, x0, y0, w, h, g->f.ifmt, size, data);
        else if (g->target == GL_TEXTURE_CUBE_MAP) glCompressedTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (layer % 6), mip, x0, y0, w, h, g->f.ifmt, size, data);
        else glCompressedTexSubImage3D(g->target, mip, x0, y0, g->target == GL_TEXTURE_3D ? z0 : (GLint)layer, w, h, d, g->f.ifmt, size, data);
        free(tmp);
        return;
    }
    if (g->f.depth) return;
    uint32_t block;
    uint32_t bpp = dxgi_format_bytes(r->format, &block);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, bpp ? pitch / bpp : 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, pitch ? slice / pitch : 0);
    uint8_t* tmp = NULL;
    if (g->f.bgra) { tmp = swizzle_bgra(p, (size_t)slice * d ? (size_t)slice * d : (size_t)pitch * h); p = tmp; }
    if (g->target == GL_TEXTURE_2D) glTexSubImage2D(GL_TEXTURE_2D, mip, x0, y0, w, h, g->f.fmt, g->f.type, p);
    else if (g->target == GL_TEXTURE_CUBE_MAP) glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + (layer % 6), mip, x0, y0, w, h, g->f.fmt, g->f.type, p);
    else glTexSubImage3D(g->target, mip, x0, y0, g->target == GL_TEXTURE_3D ? z0 : (GLint)layer, w, h, d, g->f.fmt, g->f.type, p);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
    free(tmp);
}

static GLuint compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        host_log("[gl] shader compile failed: %.300s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static void shader_create(GpuShader* s)
{
    init_gl();
    GlShader* g = calloc(1, sizeof *g);
    s->backend = g;
    if (s->stage != SH_VS && s->stage != SH_PS) return;
    if (!dxbc_translate(s->code, s->size, &g->dx) || g->dx.error[0]) {
        static int warned;
        if (warned++ < 20) host_log("[gl] shader not translated: %s", g->dx.error);
        return;
    }
    GLuint o = compile(s->stage == SH_VS ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER, g->dx.glsl);
    if (s->stage == SH_VS) g->vs = o;
    else g->fs = o;
    g->ok = o != 0;
}

static GLuint vs_variant(GlShader* v, uint32_t bgra)
{
    char* src = strdup(v->dx.glsl);
    size_t extra = 0;
    for (int i = 0; i < 16; i++) if (bgra & (1u << i)) extra += 8;
    char* out = malloc(strlen(src) + extra + 1);
    char* o = out;
    for (const char* q = src; *q;) {
        int reg;
        if (sscanf(q, "(in_%d);", &reg) == 1 && reg < 16 && (bgra & (1u << reg)) && !strncmp(q, "(in_", 4)) {
            const char* e = strchr(q, ')');
            memcpy(o, q, (size_t)(e - q + 1));
            o += e - q + 1;
            memcpy(o, ".zyxw", 5);
            o += 5;
            q = e + 1;
            continue;
        }
        *o++ = *q++;
    }
    *o = 0;
    GLuint sh = compile(GL_VERTEX_SHADER, out);
    free(out);
    free(src);
    return sh;
}

static Prog* get_prog(GpuShader* vs, GpuShader* ps, uint32_t bgra)
{
    GlShader *v = vs ? vs->backend : NULL, *p = ps ? ps->backend : NULL;
    if (!v || !v->ok) return NULL;
    uintptr_t key = ((uintptr_t)v * 31u) ^ (uintptr_t)p ^ bgra;
    Prog** slot = &progs[(key >> 4) & 4095];
    for (Prog* x = *slot; x; x = x->next)
        if (x->vs == v && x->ps == p && x->bgra == bgra) return x->prog ? x : NULL;
    Prog* x = calloc(1, sizeof *x);
    x->vs = v;
    x->ps = p;
    x->bgra = bgra;
    x->next = *slot;
    *slot = x;
    GLuint fs = p && p->ok ? p->fs : 0;
    if (!fs) {
        static GLuint empty;
        if (!empty) empty = compile(GL_FRAGMENT_SHADER, "#version 300 es\nprecision highp float;\nvoid main() {}\n");
        fs = empty;
    }
    GLuint prog = glCreateProgram();
    GLuint vso = bgra ? vs_variant(v, bgra) : v->vs;
    if (!vso) return NULL;
    glAttachShader(prog, vso);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof log, NULL, log);
        static int warned;
        if (warned++ < 20) host_log("[gl] link failed: %.300s", log);
        glDeleteProgram(prog);
        return NULL;
    }
    glUseProgram(prog);
    char name[32];
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof name, "vs_CB%d", i);
        GLuint bi = glGetUniformBlockIndex(prog, name);
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(prog, bi, i);
        snprintf(name, sizeof name, "ps_CB%d", i);
        bi = glGetUniformBlockIndex(prog, name);
        if (bi != GL_INVALID_INDEX) glUniformBlockBinding(prog, bi, 12 + i);
    }
    for (int i = 0; i < v->dx.npairs && i < 16; i++) { snprintf(name, sizeof name, "vs_tex%d", i); glUniform1i(glGetUniformLocation(prog, name), i); }
    if (p) for (int i = 0; i < p->dx.npairs && i < 16; i++) { snprintf(name, sizeof name, "ps_tex%d", i); glUniform1i(glGetUniformLocation(prog, name), 16 + i); }
    x->mips_loc[0] = glGetUniformLocation(prog, "vs_mips");
    x->mips_loc[1] = glGetUniformLocation(prog, "ps_mips");
    x->size_loc[0] = glGetUniformLocation(prog, "vs_bufsize");
    x->size_loc[1] = glGetUniformLocation(prog, "ps_bufsize");
    x->prog = prog;
    return x;
}

typedef struct { GLuint fbo; GLuint att[9]; uint32_t key[9]; } Fbo;
static Fbo fbos[64];
static int nfbo;

static uint32_t view_key(GpuView* v) { return v && v->res ? (GR(v->res)->tex * 4096u) ^ (v->first_mip * 64u) ^ v->first_slice : 0; }

static void attach(GLenum point, GpuView* v)
{
    GlRes* g = GR(v->res);
    uint32_t mip = 0, slice = 0;
    if (v->dim == 4 || v->dim == 3) { mip = (uint32_t)v->desc[8]; slice = 0; }
    else if (v->dim == 5 || v->dim == 7) { mip = v->desc[8]; slice = v->desc[12]; }
    if (g->target == GL_TEXTURE_2D) glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, point, GL_TEXTURE_2D, g->tex, mip);
    else if (g->target == GL_TEXTURE_CUBE_MAP) glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, point, GL_TEXTURE_CUBE_MAP_POSITIVE_X + slice % 6, g->tex, mip);
    else glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, point, g->tex, mip, slice);
}

static void bind_targets(GpuView** rtv, int n, GpuView* dsv)
{
    uint32_t key[9] = { 0 };
    for (int i = 0; i < n && i < 8; i++) key[i] = view_key(rtv[i]);
    key[8] = view_key(dsv);
    for (int i = 0; i < nfbo; i++)
        if (!memcmp(fbos[i].key, key, sizeof key)) { glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbos[i].fbo); return; }
    Fbo* f = &fbos[nfbo < 64 ? nfbo++ : 63];
    if (f->fbo) glDeleteFramebuffers(1, &f->fbo);
    memcpy(f->key, key, sizeof key);
    glGenFramebuffers(1, &f->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, f->fbo);
    GLenum bufs[8];
    for (int i = 0; i < 8; i++) {
        bufs[i] = i < n && rtv[i] && rtv[i]->res ? GL_COLOR_ATTACHMENT0 + i : GL_NONE;
        if (bufs[i]) attach(GL_COLOR_ATTACHMENT0 + i, rtv[i]);
    }
    glDrawBuffers(8, bufs);
    if (dsv && dsv->res) attach(GR(dsv->res)->f.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, dsv);
}

static void clear_rtv(GpuView* v, const float* c)
{
    if (!v || !v->res) return;
    bind_targets(&v, 1, NULL);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(1, 1, 1, 1);
    if (GR(v->res)->f.integer) { GLuint u[4] = { (GLuint)c[0], (GLuint)c[1], (GLuint)c[2], (GLuint)c[3] }; glClearBufferuiv(GL_COLOR, 0, u); }
    else glClearBufferfv(GL_COLOR, 0, c);
}

static void clear_dsv(GpuView* v, uint32_t flags, float depth, uint8_t stencil)
{
    if (!v || !v->res) return;
    bind_targets(NULL, 0, v);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(1);
    glStencilMask(0xFF);
    if ((flags & 3) == 3 && GR(v->res)->f.stencil) glClearBufferfi(GL_DEPTH_STENCIL, 0, depth, stencil);
    else if (flags & 1) glClearBufferfv(GL_DEPTH, 0, &depth);
    else if ((flags & 2) && GR(v->res)->f.stencil) { GLint s = stencil; glClearBufferiv(GL_STENCIL, 0, &s); }
}

static GLenum blend_factor(uint32_t b)
{
    static const GLenum t[] = { GL_ZERO, GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA,
        GL_ONE_MINUS_DST_ALPHA, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR, GL_SRC_ALPHA_SATURATE, GL_ZERO, GL_ZERO, GL_CONSTANT_COLOR,
        GL_ONE_MINUS_CONSTANT_COLOR, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA };
    return b < 20 ? t[b] : GL_ONE;
}
static GLenum blend_op(uint32_t o) { return o == 2 ? GL_FUNC_SUBTRACT : o == 3 ? GL_FUNC_REVERSE_SUBTRACT : o == 4 ? GL_MIN : o == 5 ? GL_MAX : GL_FUNC_ADD; }
static GLenum cmp_func(uint32_t f) { return f >= 1 && f <= 8 ? GL_NEVER + f - 1 : GL_ALWAYS; }
static GLenum stencil_op(uint32_t o)
{
    static const GLenum t[] = { GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP };
    return o < 9 ? t[o] : GL_KEEP;
}

static void apply_state(GpuCtx* c)
{
    const uint8_t* b = c->blend ? c->blend->desc : NULL;
    uint32_t en = b ? *(const uint32_t*)(b + 8) : 0;
    if (en) {
        const uint32_t* rt = (const uint32_t*)(b + 8);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(blend_factor(rt[1]), blend_factor(rt[2]), blend_factor(rt[4]), blend_factor(rt[5]));
        glBlendEquationSeparate(blend_op(rt[3]), blend_op(rt[6]));
        glBlendColor(c->blend_factor[0], c->blend_factor[1], c->blend_factor[2], c->blend_factor[3]);
    } else glDisable(GL_BLEND);
    uint8_t mask = b ? b[8 + 28] : 0xF;
    glColorMask(mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);

    const uint32_t* d = c->depth ? (const uint32_t*)c->depth->desc : NULL;
    if (!d || d[0]) { glEnable(GL_DEPTH_TEST); glDepthFunc(d ? cmp_func(d[2]) : GL_LESS); }
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(!d || d[1] ? 1 : 0);
    if (d && d[3]) {
        const uint8_t* db = c->depth->desc;
        uint8_t rm = db[16], wm = db[17];
        const uint32_t* ff = (const uint32_t*)(db + 20);
        const uint32_t* bf = (const uint32_t*)(db + 36);
        glEnable(GL_STENCIL_TEST);
        glStencilFuncSeparate(GL_FRONT, cmp_func(ff[3]), (GLint)c->stencil_ref, rm);
        glStencilFuncSeparate(GL_BACK, cmp_func(bf[3]), (GLint)c->stencil_ref, rm);
        glStencilOpSeparate(GL_FRONT, stencil_op(ff[0]), stencil_op(ff[1]), stencil_op(ff[2]));
        glStencilOpSeparate(GL_BACK, stencil_op(bf[0]), stencil_op(bf[1]), stencil_op(bf[2]));
        glStencilMask(wm);
    } else glDisable(GL_STENCIL_TEST);

    const uint32_t* rs = c->raster ? (const uint32_t*)c->raster->desc : NULL;
    uint32_t cull = rs ? rs[1] : 3;
    if (cull == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glCullFace(cull == 2 ? GL_FRONT : GL_BACK); }
    glFrontFace(rs && rs[2] ? GL_CW : GL_CCW);
    int bias = rs ? (int32_t)rs[3] : 0;
    float slope = rs ? *(const float*)&rs[5] : 0.0f;
    if (bias || slope) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(slope, (float)bias); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    if (rs && rs[7] && c->nscissor) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(c->scissor[0][0], c->scissor[0][1], c->scissor[0][2] - c->scissor[0][0], c->scissor[0][3] - c->scissor[0][1]);
    } else glDisable(GL_SCISSOR_TEST);
    if (c->nvp) {
        glViewport((GLint)c->vp[0].x, (GLint)c->vp[0].y, (GLsizei)c->vp[0].w, (GLsizei)c->vp[0].h);
        glDepthRangef(c->vp[0].minz, c->vp[0].maxz);
    }
}

static GLuint sampler_obj(GpuState* s)
{
    if (!s) return 0;
    if (s->backend) return (GLuint)(uintptr_t)s->backend;
    const uint32_t* d = (const uint32_t*)s->desc;
    GLuint o;
    glGenSamplers(1, &o);
    uint32_t f = d[0];
    int mag_lin = (f & 0x4) != 0, min_lin = (f & 0x10) != 0, mip_lin = (f & 0x1) != 0, aniso = (f & 0x40) != 0;
    if (aniso) mag_lin = min_lin = mip_lin = 1;
    glSamplerParameteri(o, GL_TEXTURE_MAG_FILTER, mag_lin ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(o, GL_TEXTURE_MIN_FILTER, min_lin ? (mip_lin ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST) : (mip_lin ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST));
    static const GLenum wrap[] = { GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT };
    glSamplerParameteri(o, GL_TEXTURE_WRAP_S, wrap[d[1] < 6 ? d[1] : 0]);
    glSamplerParameteri(o, GL_TEXTURE_WRAP_T, wrap[d[2] < 6 ? d[2] : 0]);
    glSamplerParameteri(o, GL_TEXTURE_WRAP_R, wrap[d[3] < 6 ? d[3] : 0]);
    if (aniso && has_aniso) glSamplerParameterf(o, GL_TEXTURE_MAX_ANISOTROPY_EXT, (float)(d[5] ? d[5] : 1));
    if (f & 0x80) { glSamplerParameteri(o, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE); glSamplerParameteri(o, GL_TEXTURE_COMPARE_FUNC, cmp_func(d[6])); }
    glSamplerParameterf(o, GL_TEXTURE_MIN_LOD, *(const float*)&d[11]);
    glSamplerParameterf(o, GL_TEXTURE_MAX_LOD, *(const float*)&d[12]);
    s->backend = (void*)(uintptr_t)o;
    return o;
}

static void bind_textures(GpuCtx* c, Prog* p)
{
    static GLint mips[32], sizes[32];
    for (int st = 0; st < 2; st++) {
        GlShader* sh = st == 0 ? p->vs : p->ps;
        if (!sh) continue;
        int stage = st == 0 ? SH_VS : SH_PS, base = st == 0 ? 0 : 16;
        for (int i = 0; i < sh->dx.npairs && i < 16; i++) {
            DxPair* pr = &sh->dx.pairs[i];
            GpuView* v = pr->tex >= 0 && pr->tex < 128 ? c->srv[stage][pr->tex] : NULL;
            glActiveTexture(GL_TEXTURE0 + base + i);
            GlRes* g = v && v->res ? GR(v->res) : NULL;
            GLenum target = pr->dim == 5 ? GL_TEXTURE_3D : pr->dim == 6 || pr->dim == 10 ? GL_TEXTURE_CUBE_MAP : pr->dim >= 7 && pr->dim <= 9 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
            if (g && g->tex && (v->res->type == RES_BUFFER || g->target == target)) {
                glBindTexture(target, g->tex);
                mips[base + i] = v->res->mips;
                sizes[base + i] = v->res->type == RES_BUFFER ? (int)(v->res->stride ? v->res->width / v->res->stride : v->res->width / 4) : 0;
            } else glBindTexture(target, 0);
            glBindSampler(base + i, pr->samp >= 0 && pr->samp < 16 ? sampler_obj(c->samp[stage][pr->samp]) : 0);
        }
        if (p->mips_loc[st] >= 0 && sh->dx.npairs) glUniform1iv(p->mips_loc[st], sh->dx.npairs, mips + base);
        if (p->size_loc[st] >= 0 && sh->dx.npairs) glUniform1iv(p->size_loc[st], sh->dx.npairs, sizes + base);
    }
}

static void bind_cbs(GpuCtx* c, Prog* p)
{
    for (int st = 0; st < 2; st++) {
        GlShader* sh = st == 0 ? p->vs : p->ps;
        if (!sh) continue;
        int stage = st == 0 ? SH_VS : SH_PS;
        for (int i = 0; i < 12; i++) {
            if (!sh->dx.cb_size[i]) continue;
            GpuRes* r = c->cb[stage][i];
            if (r && GR(r) && GR(r)->buf) glBindBufferRange(GL_UNIFORM_BUFFER, st * 12 + i, GR(r)->buf, 0, (sh->dx.cb_size[i] * 16 <= (int)((r->width + 15) & ~15u)) ? sh->dx.cb_size[i] * 16 : (GLsizeiptr)((r->width + 15) & ~15u));
        }
    }
}

typedef struct { GLint size; GLenum type; GLboolean norm, integer; } AttrFmt;
static AttrFmt attr_fmt(uint32_t f)
{
    switch (f) {
    case 2: return (AttrFmt){ 4, GL_FLOAT, 0, 0 };
    case 3: return (AttrFmt){ 4, GL_UNSIGNED_INT, 0, 1 };
    case 4: return (AttrFmt){ 4, GL_INT, 0, 1 };
    case 6: return (AttrFmt){ 3, GL_FLOAT, 0, 0 };
    case 7: return (AttrFmt){ 3, GL_UNSIGNED_INT, 0, 1 };
    case 10: return (AttrFmt){ 4, GL_HALF_FLOAT, 0, 0 };
    case 11: return (AttrFmt){ 4, GL_UNSIGNED_SHORT, 1, 0 };
    case 12: return (AttrFmt){ 4, GL_UNSIGNED_SHORT, 0, 1 };
    case 13: return (AttrFmt){ 4, GL_SHORT, 1, 0 };
    case 14: return (AttrFmt){ 4, GL_SHORT, 0, 1 };
    case 16: return (AttrFmt){ 2, GL_FLOAT, 0, 0 };
    case 17: return (AttrFmt){ 2, GL_UNSIGNED_INT, 0, 1 };
    case 24: return (AttrFmt){ 4, GL_UNSIGNED_INT_2_10_10_10_REV, 1, 0 };
    case 28: case 87: return (AttrFmt){ 4, GL_UNSIGNED_BYTE, 1, 0 };
    case 30: return (AttrFmt){ 4, GL_UNSIGNED_BYTE, 0, 1 };
    case 31: return (AttrFmt){ 4, GL_BYTE, 1, 0 };
    case 32: return (AttrFmt){ 4, GL_BYTE, 0, 1 };
    case 34: return (AttrFmt){ 2, GL_HALF_FLOAT, 0, 0 };
    case 35: return (AttrFmt){ 2, GL_UNSIGNED_SHORT, 1, 0 };
    case 36: return (AttrFmt){ 2, GL_UNSIGNED_SHORT, 0, 1 };
    case 37: return (AttrFmt){ 2, GL_SHORT, 1, 0 };
    case 38: return (AttrFmt){ 2, GL_SHORT, 0, 1 };
    case 41: return (AttrFmt){ 1, GL_FLOAT, 0, 0 };
    case 42: return (AttrFmt){ 1, GL_UNSIGNED_INT, 0, 1 };
    case 43: return (AttrFmt){ 1, GL_INT, 0, 1 };
    case 49: return (AttrFmt){ 2, GL_UNSIGNED_BYTE, 1, 0 };
    case 54: return (AttrFmt){ 1, GL_HALF_FLOAT, 0, 0 };
    case 57: return (AttrFmt){ 1, GL_UNSIGNED_SHORT, 0, 1 };
    default: return (AttrFmt){ 4, GL_FLOAT, 0, 0 };
    }
}

static int bind_vertex_input(GpuCtx* c, GlShader* vs, int32_t base_vertex, uint32_t first_instance)
{
    static int enabled[16];
    int want[16] = { 0 };
    GpuLayout* l = c->layout;
    if (l) {
        uint32_t offs[16] = { 0 };
        for (uint32_t i = 0; i < l->count; i++) {
            uint32_t off = l->el[i].offset == 0xFFFFFFFF ? offs[l->el[i].slot & 15] : l->el[i].offset;
            uint32_t blk;
            offs[l->el[i].slot & 15] = off + dxgi_format_bytes(l->el[i].format, &blk);
            int loc = -1;
            for (int k = 0; k < vs->dx.in.n; k++)
                if (!strcmp(vs->dx.in.e[k].name, l->el[i].semantic) && vs->dx.in.e[k].index == l->el[i].index) loc = (int)vs->dx.in.e[k].reg;
            if (loc < 0 || loc >= 16) continue;
            uint32_t slot = l->el[i].slot & 15;
            GpuRes* vb = c->vb[slot];
            if (!vb || !GR(vb) || !GR(vb)->buf) continue;
            AttrFmt f = attr_fmt(l->el[i].format);
            uint32_t stride = c->vb_stride[slot];
            uint64_t base = (uint64_t)c->vb_offset[slot] + off + (l->el[i].per_instance ? (uint64_t)first_instance * stride : (uint64_t)(int64_t)base_vertex * stride);
            glBindBuffer(GL_ARRAY_BUFFER, GR(vb)->buf);
            if (f.integer) glVertexAttribIPointer(loc, f.size, f.type, stride, (const void*)(uintptr_t)base);
            else glVertexAttribPointer(loc, f.size, f.type, f.norm, stride, (const void*)(uintptr_t)base);
            glVertexAttribDivisor(loc, l->el[i].per_instance ? (l->el[i].step ? l->el[i].step : 1) : 0);
            want[loc] = 1;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (want[i] && !enabled[i]) glEnableVertexAttribArray(i);
        if (!want[i] && enabled[i]) glDisableVertexAttribArray(i);
        enabled[i] = want[i];
        if (!want[i]) glVertexAttrib4f(i, 0, 0, 0, 1);
    }
    return 1;
}

static GLenum topology(uint32_t t)
{
    switch (t) {
    case 1: return GL_POINTS;
    case 2: return GL_LINES;
    case 3: return GL_LINE_STRIP;
    case 5: return GL_TRIANGLE_STRIP;
    default: return GL_TRIANGLES;
    }
}

static void draw(GpuCtx* c, uint32_t count, uint32_t first, int32_t base_vertex, uint32_t instances, uint32_t first_instance, int indexed)
{
    if (!glctx || count == ~0u) return;
    uint32_t bgra = 0;
    GlShader* vsh = c->sh[SH_VS] ? c->sh[SH_VS]->backend : NULL;
    if (c->layout && vsh && vsh->ok)
        for (uint32_t i = 0; i < c->layout->count; i++)
            if (c->layout->el[i].format == 87)
                for (int k = 0; k < vsh->dx.in.n; k++)
                    if (!strcmp(vsh->dx.in.e[k].name, c->layout->el[i].semantic) && vsh->dx.in.e[k].index == c->layout->el[i].index && vsh->dx.in.e[k].reg < 16)
                        bgra |= 1u << vsh->dx.in.e[k].reg;
    Prog* p = get_prog(c->sh[SH_VS], c->sh[SH_PS], bgra);
    if (!p) return;
    int nrt = 0;
    for (int i = 0; i < 8; i++) if (c->rtv[i]) nrt = i + 1;
    bind_targets(c->rtv, nrt, c->dsv);
    glUseProgram(p->prog);
    apply_state(c);
    bind_cbs(c, p);
    bind_textures(c, p);
    bind_vertex_input(c, p->vs, indexed ? base_vertex : 0, first_instance);
    GLenum mode = topology(c->topology);
    if (indexed) {
        if (!c->ib || !GR(c->ib) || !GR(c->ib)->ibuf) return;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GR(c->ib)->ibuf);
        int is32 = c->ib_format == 42;
        const void* off = (const void*)(uintptr_t)(c->ib_offset + first * (is32 ? 4u : 2u));
        if (instances > 1) glDrawElementsInstanced(mode, count, is32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, off, instances);
        else glDrawElements(mode, count, is32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, off);
    } else {
        if (instances > 1) glDrawArraysInstanced(mode, first, count, instances);
        else glDrawArrays(mode, first, count);
    }
    draws++;
}

static void copy(GpuRes* dst, uint32_t dsub, uint32_t dx, uint32_t dy, uint32_t dz, GpuRes* src, uint32_t ssub, const int32_t* box)
{
    GlRes *d = GR(dst), *s = GR(src);
    if (!d || !s) return;
    if (dst->type == RES_BUFFER) {
        if (s->buf && d->buf) {
            glBindBuffer(GL_COPY_READ_BUFFER, s->buf);
            glBindBuffer(GL_COPY_WRITE_BUFFER, d->buf);
            uint32_t so = box ? (uint32_t)box[0] : 0, n = box ? (uint32_t)(box[3] - box[0]) : (src->width < dst->width ? src->width : dst->width);
            glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, so, dx, n);
        }
        return;
    }
    if (s->f.compressed || d->f.compressed || s->f.depth) return;
    uint32_t smip = ssub % src->mips, slayer = ssub / src->mips, dmip = dsub % dst->mips, dlayer = dsub / dst->mips;
    uint32_t w = box ? (uint32_t)(box[3] - box[0]) : src->width >> smip, h = box ? (uint32_t)(box[4] - box[1]) : src->height >> smip;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_read_fbo);
    if (s->target == GL_TEXTURE_2D) glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s->tex, smip);
    else glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s->tex, smip, slayer);
    glBindTexture(d->target, d->tex);
    int sx = box ? box[0] : 0, sy = box ? box[1] : 0;
    if (d->target == GL_TEXTURE_2D) glCopyTexSubImage2D(GL_TEXTURE_2D, dmip, dx, dy, sx, sy, w ? w : 1, h ? h : 1);
    else if (d->target == GL_TEXTURE_CUBE_MAP) glCopyTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + dlayer % 6, dmip, dx, dy, sx, sy, w ? w : 1, h ? h : 1);
    else glCopyTexSubImage3D(d->target, dmip, dx, dy, d->target == GL_TEXTURE_3D ? dz : dlayer, sx, sy, w ? w : 1, h ? h : 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

static void present(GpuRes* bb, uint32_t w, uint32_t h)
{
    if (!glctx || !bb || !GR(bb)) return;
    int cw, ch;
    emscripten_get_canvas_element_size("#canvas", &cw, &ch);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, blit_read_fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GR(bb)->tex, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, (GLint)w, (GLint)h, 0, ch, cw, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    frames++;
    if (frames == 1 || frames % 600 == 0) host_log("[gl] frame %llu (%llu draws so far)", (unsigned long long)frames, (unsigned long long)draws);
}

static void view_create(GpuView* v) { (void)v; }
static void state_create(GpuState* s) { (void)s; }

static GpuBackend web = { "WebGL 2", res_create, res_upload, NULL, view_create, shader_create, state_create, NULL, clear_rtv, clear_dsv,
                          draw, NULL, copy, NULL, present, NULL };

GpuBackend* gpu_gl_backend(void) { init_gl(); return glctx > 0 ? &web : NULL; }
#endif
