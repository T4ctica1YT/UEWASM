// Direct3D 8 smoke test (built 32-bit, like UT2004's D3DDrv): device creation, caps and creation parameters, a managed
// texture filled through LockRect, a vertex and index buffer, a declaration-only vertex shader, and an indexed triangle.
// D3D8 has no SDK header on current Windows SDKs, so the few vtables used here are declared directly; the 32-bit
// stdcall calls and vtable indices are the ones this test is meant to check.
#include <windows.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "user32.lib")

#define D8API __stdcall
#define D3D_SDK_VERSION 220
#define D3DDEVTYPE_HAL 1
#define D3DCREATE_SOFTWARE_VERTEXPROCESSING 0x20
#define D3DFMT_A8R8G8B8 21
#define D3DFMT_X8R8G8B8 22
#define D3DFMT_D24S8 75
#define D3DFMT_INDEX16 101
#define D3DSWAPEFFECT_DISCARD 1
#define D3DPOOL_DEFAULT 0
#define D3DPOOL_MANAGED 1
#define D3DCLEAR_TARGET 1
#define D3DCLEAR_ZBUFFER 2
#define D3DPT_TRIANGLELIST 4
#define D3DRS_ZENABLE 7
#define D3DRS_ZWRITEENABLE 14
#define D3DRS_CULLMODE 22
#define D3DRS_LIGHTING 137
#define D3DCULL_NONE 1
#define D3DTS_VIEW 2
#define D3DTS_PROJECTION 3
#define D3DTS_WORLD 256
#define D3DTSS_COLOROP 1
#define D3DTSS_COLORARG1 2
#define D3DTSS_COLORARG2 3
#define D3DTSS_ALPHAOP 4
#define D3DTSS_ADDRESSU 13
#define D3DTSS_ADDRESSV 14
#define D3DTSS_MAGFILTER 16
#define D3DTSS_MINFILTER 17
#define D3DTSS_MIPFILTER 18
#define D3DTSS_ADDRESSW 25
#define D3DTOP_MODULATE 4
#define D3DTOP_DISABLE 1
#define D3DTA_DIFFUSE 0
#define D3DTA_TEXTURE 2
#define D3DTEXF_POINT 1
#define D3DTEXF_LINEAR 2
#define D3DTADDRESS_CLAMP 3
#define D3DVSD_TOKEN_STREAM 1
#define D3DVSD_TOKEN_STREAMDATA 2
#define D3DVSD_MAKETOKENTYPE(t) ((DWORD)(t) << 29)
#define D3DVSD_STREAM(n) (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_STREAM) | (DWORD)(n))
#define D3DVSD_REG(r, t) (D3DVSD_MAKETOKENTYPE(D3DVSD_TOKEN_STREAMDATA) | ((DWORD)(t) << 16) | (DWORD)(r))
#define D3DVSD_END() 0xFFFFFFFFu
#define D3DVSDT_FLOAT2 1
#define D3DVSDT_FLOAT3 2
#define D3DVSDT_D3DCOLOR 4
#define D3DVSDE_POSITION 0
#define D3DVSDE_DIFFUSE 5
#define D3DVSDE_TEXCOORD0 7

typedef struct { void** vtbl; } Obj8;

typedef struct {
    UINT BackBufferWidth, BackBufferHeight;
    DWORD BackBufferFormat, BackBufferCount, MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed, EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat, Flags;
    UINT FullScreen_RefreshRateInHz, FullScreen_PresentationInterval;
} PP8;

typedef struct {
    UINT AdapterOrdinal;
    DWORD DeviceType;
    HWND hFocusWindow;
    DWORD BehaviorFlags;
} Creation8;

typedef struct {
    DWORD Format, Type, Usage, Pool, Size, MultiSampleType;
    UINT Width, Height;
} SurfaceDesc8;

typedef struct { DWORD Format, Type, Usage, Pool, Size, FVF; } VBDesc8;
typedef struct { INT Pitch; void* pBits; } LockedRect8;
typedef struct { DWORD X, Y, Width, Height; float MinZ, MaxZ; } Viewport8;
typedef struct { float m[4][4]; } Matrix8;

typedef HRESULT (D8API *PFN_CREATE_DEVICE)(Obj8*, UINT, DWORD, HWND, DWORD, PP8*, Obj8**);
typedef HRESULT (D8API *PFN_GET_CAPS)(Obj8*, DWORD*);
typedef HRESULT (D8API *PFN_GET_CREATION)(Obj8*, Creation8*);
typedef HRESULT (D8API *PFN_CREATE_TEXTURE)(Obj8*, UINT, UINT, UINT, DWORD, DWORD, DWORD, Obj8**);
typedef HRESULT (D8API *PFN_LEVEL_DESC)(Obj8*, UINT, SurfaceDesc8*);
typedef HRESULT (D8API *PFN_LOCK_RECT)(Obj8*, UINT, LockedRect8*, const RECT*, DWORD);
typedef HRESULT (D8API *PFN_UNLOCK_RECT)(Obj8*, UINT);
typedef HRESULT (D8API *PFN_CREATE_VB)(Obj8*, UINT, DWORD, DWORD, DWORD, Obj8**);
typedef HRESULT (D8API *PFN_BUFFER_LOCK)(Obj8*, UINT, UINT, void**, DWORD);
typedef HRESULT (D8API *PFN_BUFFER_UNLOCK)(Obj8*);
typedef HRESULT (D8API *PFN_BUFFER_DESC)(Obj8*, VBDesc8*);
typedef HRESULT (D8API *PFN_CREATE_IB)(Obj8*, UINT, DWORD, DWORD, DWORD, Obj8**);
typedef HRESULT (D8API *PFN_CREATE_VS)(Obj8*, const DWORD*, const DWORD*, DWORD*, DWORD);
typedef HRESULT (D8API *PFN_SET_VS)(Obj8*, DWORD);
typedef HRESULT (D8API *PFN_GET_VS)(Obj8*, DWORD*);
typedef HRESULT (D8API *PFN_GET_VS_DECL)(Obj8*, DWORD, void*, DWORD*);
typedef HRESULT (D8API *PFN_DELETE_VS)(Obj8*, DWORD);
typedef HRESULT (D8API *PFN_SET_STREAM)(Obj8*, UINT, Obj8*, UINT);
typedef HRESULT (D8API *PFN_SET_INDICES)(Obj8*, Obj8*, UINT);
typedef HRESULT (D8API *PFN_GET_INDICES)(Obj8*, Obj8**, UINT*);
typedef HRESULT (D8API *PFN_SET_TEXTURE)(Obj8*, DWORD, Obj8*);
typedef HRESULT (D8API *PFN_SET_TSS)(Obj8*, DWORD, DWORD, DWORD);
typedef HRESULT (D8API *PFN_SET_RS)(Obj8*, DWORD, DWORD);
typedef HRESULT (D8API *PFN_SET_TRANSFORM)(Obj8*, DWORD, const Matrix8*);
typedef HRESULT (D8API *PFN_SET_VIEWPORT)(Obj8*, const Viewport8*);
typedef HRESULT (D8API *PFN_CLEAR)(Obj8*, DWORD, const RECT*, DWORD, DWORD, float, DWORD);
typedef HRESULT (D8API *PFN_BEGIN_END)(Obj8*);
typedef HRESULT (D8API *PFN_DRAW_INDEXED)(Obj8*, DWORD, UINT, UINT, UINT, UINT);
typedef HRESULT (D8API *PFN_PRESENT)(Obj8*, const RECT*, const RECT*, HWND, const void*);
typedef ULONG (D8API *PFN_RELEASE)(Obj8*);
typedef Obj8* (D8API *PFN_CREATE8)(UINT);

struct Vertex8 { float x, y, z; DWORD color; float u, v; };

static HRESULT create_device(Obj8* d3d, HWND w, PP8* pp, Obj8** out)
{
    return ((PFN_CREATE_DEVICE)d3d->vtbl[15])(d3d, 0, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, pp, out);
}
static HRESULT get_caps(Obj8* dev, DWORD* caps) { return ((PFN_GET_CAPS)dev->vtbl[7])(dev, caps); }
static HRESULT get_creation(Obj8* dev, Creation8* cp) { return ((PFN_GET_CREATION)dev->vtbl[9])(dev, cp); }
static HRESULT create_texture(Obj8* dev, Obj8** out)
{
    return ((PFN_CREATE_TEXTURE)dev->vtbl[20])(dev, 2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, out);
}
static HRESULT level_desc(Obj8* tex, SurfaceDesc8* d) { return ((PFN_LEVEL_DESC)tex->vtbl[14])(tex, 0, d); }
static HRESULT lock_rect(Obj8* tex, LockedRect8* lr) { return ((PFN_LOCK_RECT)tex->vtbl[16])(tex, 0, lr, NULL, 0); }
static HRESULT unlock_rect(Obj8* tex) { return ((PFN_UNLOCK_RECT)tex->vtbl[17])(tex, 0); }
static HRESULT create_vb(Obj8* dev, Obj8** out)
{
    return ((PFN_CREATE_VB)dev->vtbl[23])(dev, 3 * sizeof(Vertex8), 0x8 /* WRITEONLY */, 0, D3DPOOL_MANAGED, out);
}
static HRESULT buf_lock(Obj8* b, UINT size, void** p) { return ((PFN_BUFFER_LOCK)b->vtbl[11])(b, 0, size, p, 0); }
static HRESULT buf_unlock(Obj8* b) { return ((PFN_BUFFER_UNLOCK)b->vtbl[12])(b); }
static HRESULT buf_desc(Obj8* b, VBDesc8* d) { return ((PFN_BUFFER_DESC)b->vtbl[13])(b, d); }
static HRESULT create_ib(Obj8* dev, Obj8** out)
{
    return ((PFN_CREATE_IB)dev->vtbl[24])(dev, 3 * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, out);
}
static HRESULT create_vs(Obj8* dev, const DWORD* decl, DWORD* out)
{
    return ((PFN_CREATE_VS)dev->vtbl[75])(dev, decl, NULL, out, 0);
}
static HRESULT set_vs(Obj8* dev, DWORD h) { return ((PFN_SET_VS)dev->vtbl[76])(dev, h); }
static HRESULT get_vs(Obj8* dev, DWORD* h) { return ((PFN_GET_VS)dev->vtbl[77])(dev, h); }
static HRESULT get_vs_decl(Obj8* dev, DWORD h, void* p, DWORD* n) { return ((PFN_GET_VS_DECL)dev->vtbl[81])(dev, h, p, n); }
static HRESULT delete_vs(Obj8* dev, DWORD h) { return ((PFN_DELETE_VS)dev->vtbl[78])(dev, h); }
static HRESULT set_stream(Obj8* dev, Obj8* vb) { return ((PFN_SET_STREAM)dev->vtbl[83])(dev, 0, vb, sizeof(Vertex8)); }
static HRESULT set_indices(Obj8* dev, Obj8* ib, UINT base) { return ((PFN_SET_INDICES)dev->vtbl[85])(dev, ib, base); }
static HRESULT get_indices(Obj8* dev, Obj8** ib, UINT* base) { return ((PFN_GET_INDICES)dev->vtbl[86])(dev, ib, base); }
static HRESULT set_texture(Obj8* dev, Obj8* t) { return ((PFN_SET_TEXTURE)dev->vtbl[61])(dev, 0, t); }
static HRESULT set_tss(Obj8* dev, DWORD type, DWORD value) { return ((PFN_SET_TSS)dev->vtbl[63])(dev, 0, type, value); }
static HRESULT set_rs(Obj8* dev, DWORD state, DWORD value) { return ((PFN_SET_RS)dev->vtbl[50])(dev, state, value); }
static HRESULT set_transform(Obj8* dev, DWORD state, const Matrix8* m) { return ((PFN_SET_TRANSFORM)dev->vtbl[37])(dev, state, m); }
static HRESULT set_viewport(Obj8* dev, const Viewport8* vp) { return ((PFN_SET_VIEWPORT)dev->vtbl[40])(dev, vp); }
static HRESULT clear(Obj8* dev) { return ((PFN_CLEAR)dev->vtbl[36])(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF0D194C, 1.0f, 0); }
static HRESULT begin_scene(Obj8* dev) { return ((PFN_BEGIN_END)dev->vtbl[34])(dev); }
static HRESULT end_scene(Obj8* dev) { return ((PFN_BEGIN_END)dev->vtbl[35])(dev); }
static HRESULT draw_indexed(Obj8* dev) { return ((PFN_DRAW_INDEXED)dev->vtbl[71])(dev, D3DPT_TRIANGLELIST, 0, 3, 0, 1); }
static HRESULT present(Obj8* dev) { return ((PFN_PRESENT)dev->vtbl[15])(dev, NULL, NULL, NULL, NULL); }
static ULONG release(Obj8* o) { return ((PFN_RELEASE)o->vtbl[2])(o); }

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"uewasm_d3d8_smoke";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"d3d8", WS_POPUP, 0, 0, 640, 360, NULL, NULL, wc.hInstance, NULL);

    HMODULE dll = LoadLibraryA("d3d8.dll");
    PFN_CREATE8 create8 = dll ? (PFN_CREATE8)GetProcAddress(dll, "Direct3DCreate8") : NULL;
    printf("d3d8 smoke\n");
    printf("library %s\n", create8 ? "ok" : "failed");
    if (!create8) return 1;
    Obj8* d3d = create8(D3D_SDK_VERSION);
    printf("interface %s\n", d3d ? "ok" : "failed");
    if (!d3d) return 1;

    PP8 pp = {};
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 360;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    Obj8* dev = NULL;
    HRESULT hr = create_device(d3d, hwnd, &pp, &dev);
    printf("device %s\n", SUCCEEDED(hr) && dev ? "ok" : "failed");
    if (FAILED(hr) || !dev) return 1;

    DWORD caps[53] = {};
    get_caps(dev, caps);
    Creation8 cp = {};
    get_creation(dev, &cp);
    printf("caps %u creation %u/%u\n", caps[0], cp.AdapterOrdinal, cp.DeviceType);

    Obj8* tex = NULL;
    hr = create_texture(dev, &tex);
    if (SUCCEEDED(hr) && tex) {
        LockedRect8 lr = {};
        if (SUCCEEDED(lock_rect(tex, &lr))) {
            for (int y = 0; y < 2; y++)
                for (int x = 0; x < 2; x++)
                    *(DWORD*)((BYTE*)lr.pBits + y * lr.Pitch + x * 4) = 0xFFFFFFFF;
            unlock_rect(tex);
        }
        SurfaceDesc8 sd = {};
        level_desc(tex, &sd);
        printf("texture %ux%u fmt %u\n", sd.Width, sd.Height, sd.Format);
    } else {
        printf("texture failed %08lx\n", (unsigned long)hr);
    }

    Vertex8 verts[3] = {
        { 0.0f, 0.8f, 0.5f, 0xFFFF0000, 0.5f, 0.0f },
        { 0.7f, -0.6f, 0.5f, 0xFF00FF00, 1.0f, 1.0f },
        { -0.7f, -0.6f, 0.5f, 0xFF0000FF, 0.0f, 1.0f },
    };
    Obj8* vb = NULL;
    hr = create_vb(dev, &vb);
    if (SUCCEEDED(hr) && vb) {
        void* p = NULL;
        if (SUCCEEDED(buf_lock(vb, sizeof verts, &p))) {
            memcpy(p, verts, sizeof verts);
            buf_unlock(vb);
        }
        VBDesc8 vd = {};
        buf_desc(vb, &vd);
        printf("buffer %u fvf %u\n", vd.Size, vd.FVF);
    } else {
        printf("buffer failed %08lx\n", (unsigned long)hr);
    }

    Obj8* ib = NULL;
    hr = create_ib(dev, &ib);
    if (SUCCEEDED(hr) && ib) {
        void* p = NULL;
        if (SUCCEEDED(buf_lock(ib, 3 * sizeof(WORD), &p))) {
            WORD idx[3] = { 0, 1, 2 };
            memcpy(p, idx, sizeof idx);
            buf_unlock(ib);
        }
    }

    DWORD decl[] = {
        D3DVSD_STREAM(0),
        D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
        D3DVSD_REG(D3DVSDE_DIFFUSE, D3DVSDT_D3DCOLOR),
        D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2),
        D3DVSD_END(),
    };
    DWORD vs = 0;
    hr = create_vs(dev, decl, &vs);
    printf("vertex shader %s\n", SUCCEEDED(hr) && vs ? "ok" : "failed");
    if (SUCCEEDED(hr) && vs) {
        set_vs(dev, vs);
        DWORD bound = 0;
        get_vs(dev, &bound);
        DWORD declbuf[16] = {};
        DWORD dsize = sizeof declbuf;
        HRESULT dh = get_vs_decl(dev, vs, declbuf, &dsize);
        printf("declaration %s %u bytes\n", SUCCEEDED(dh) ? "ok" : "failed", (unsigned)dsize);
        printf("bound %s\n", bound == vs ? "ok" : "failed");
    }

    if (vb && ib) {
        UINT base = 999;
        Obj8* gotib = NULL;
        set_indices(dev, ib, 0);
        get_indices(dev, &gotib, &base);
        printf("indices base %u\n", base);
        set_stream(dev, vb);
    }
    if (tex) set_texture(dev, tex);
    set_tss(dev, D3DTSS_COLOROP, D3DTOP_MODULATE);
    set_tss(dev, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    set_tss(dev, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    set_tss(dev, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    set_tss(dev, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    set_tss(dev, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    set_tss(dev, D3DTSS_ADDRESSW, D3DTADDRESS_CLAMP);
    set_tss(dev, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    set_tss(dev, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    set_tss(dev, D3DTSS_MIPFILTER, D3DTEXF_POINT);
    set_rs(dev, D3DRS_ZENABLE, 1);
    set_rs(dev, D3DRS_ZWRITEENABLE, 1);
    set_rs(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    set_rs(dev, D3DRS_LIGHTING, 0);
    Matrix8 ident = {};
    ident.m[0][0] = ident.m[1][1] = ident.m[2][2] = ident.m[3][3] = 1.0f;
    set_transform(dev, D3DTS_WORLD, &ident);
    set_transform(dev, D3DTS_VIEW, &ident);
    set_transform(dev, D3DTS_PROJECTION, &ident);
    Viewport8 vp = { 0, 0, 640, 360, 0.0f, 1.0f };
    set_viewport(dev, &vp);

    hr = begin_scene(dev);
    clear(dev);
    hr = draw_indexed(dev);
    end_scene(dev);
    printf("draw %s\n", SUCCEEDED(hr) ? "ok" : "failed");
    hr = present(dev);
    printf("present %s\n", SUCCEEDED(hr) ? "ok" : "failed");

    if (vs) delete_vs(dev, vs);
    if (ib) release(ib);
    if (vb) release(vb);
    if (tex) release(tex);
    release(dev);
    release(d3d);
    if (dll) FreeLibrary(dll);
    DestroyWindow(hwnd);
    return 0;
}
