// Direct3D 9 version of triangle.cpp (built 32-bit, like most D3D9 games): vertex buffer, vertex declaration,
// a managed texture filled through LockRect, shader constants, Clear and Present.
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include <d3d9.h>

#include "triangle9_vs.h"
#include "triangle9_ps.h"

#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

struct Vertex { float x, y; DWORD color; float u, v; };

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"tri9";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"tri9", L"tri9", WS_POPUP, 0, 0, 640, 360, NULL, NULL, NULL, NULL);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 360;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev) : E_FAIL;
    printf("device %s\n", SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) return 1;

    IDirect3DVertexShader9* vs = NULL;
    IDirect3DPixelShader9* ps = NULL;
    dev->CreateVertexShader((const DWORD*)g_VSMain, &vs);
    dev->CreatePixelShader((const DWORD*)g_PSMain, &ps);
    D3DVERTEXELEMENT9 el[] = {
        { 0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 },
        { 0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9* decl = NULL;
    dev->CreateVertexDeclaration(el, &decl);

    Vertex verts[3] = {
        { 0.0f, 0.8f, 0xFFFF0000, 0.5f, 0.0f },
        { 0.7f, -0.6f, 0xFF00FF00, 1.0f, 1.0f },
        { -0.7f, -0.6f, 0xFF0000FF, 0.0f, 1.0f },
    };
    IDirect3DVertexBuffer9* vb = NULL;
    dev->CreateVertexBuffer(sizeof verts, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, NULL);
    void* p = NULL;
    vb->Lock(0, 0, &p, 0);
    memcpy(p, verts, sizeof verts);
    vb->Unlock();

    IDirect3DTexture9* tex = NULL;
    dev->CreateTexture(2, 2, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
    D3DLOCKED_RECT lr;
    tex->LockRect(0, &lr, NULL, 0);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) ((DWORD*)((BYTE*)lr.pBits + y * lr.Pitch))[x] = 0xFFFFFFFF;
    tex->UnlockRect(0);

    float tint[4] = { 1.0f, 1.0f, 1.0f, 1.0f }, offset[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    int frames = 0;
    for (; frames < 120; frames++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_COLORVALUE(0.05f, 0.1f, 0.3f, 1.0f), 1.0f, 0);
        dev->BeginScene();
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->SetVertexDeclaration(decl);
        dev->SetStreamSource(0, vb, 0, sizeof(Vertex));
        dev->SetVertexShaderConstantF(4, offset, 1);
        dev->SetPixelShaderConstantF(0, tint, 1);
        dev->SetTexture(0, tex);
        dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 1);
        dev->EndScene();
        dev->Present(NULL, NULL, NULL, NULL);
    }
    printf("presented %d frames\n", frames);
    dev->Release();
    d3d->Release();
    DestroyWindow(hwnd);
    return 0;
}
