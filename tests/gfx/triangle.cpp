#include <stdio.h>
#include <windows.h>
#include <d3d11.h>

#include "triangle_vs.h"
#include "triangle_ps.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

struct Vertex { float x, y; unsigned color; float u, v; };

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"tri";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"tri", L"tri", WS_POPUP, 0, 0, 640, 360, NULL, NULL, NULL, NULL);

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 640;
    sd.BufferDesc.Height = 360;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 1;
    sd.OutputWindow = hwnd;
    sd.Windowed = TRUE;
    IDXGISwapChain* sc = NULL;
    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx);
    printf("device %s\n", SUCCEEDED(hr) ? "ok" : "failed");
    if (FAILED(hr)) return 1;

    ID3D11Texture2D* bb = NULL;
    sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb);
    ID3D11RenderTargetView* rtv = NULL;
    dev->CreateRenderTargetView(bb, NULL, &rtv);

    ID3D11VertexShader* vs = NULL;
    ID3D11PixelShader* ps = NULL;
    dev->CreateVertexShader(g_VSMain, sizeof g_VSMain, NULL, &vs);
    dev->CreatePixelShader(g_PSMain, sizeof g_PSMain, NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout* layout = NULL;
    dev->CreateInputLayout(el, 3, g_VSMain, sizeof g_VSMain, &layout);

    Vertex verts[3] = {
        { 0.0f, 0.8f, 0xFFFF0000, 0.5f, 0.0f },
        { 0.7f, -0.6f, 0xFF00FF00, 1.0f, 1.0f },
        { -0.7f, -0.6f, 0xFF0000FF, 0.0f, 1.0f },
    };
    D3D11_BUFFER_DESC bd = { sizeof verts, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA init = { verts };
    ID3D11Buffer* vb = NULL;
    dev->CreateBuffer(&bd, &init, &vb);

    float params[8] = { 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    D3D11_BUFFER_DESC cbd = { sizeof params, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    ID3D11Buffer* cb = NULL;
    dev->CreateBuffer(&cbd, NULL, &cb);

    unsigned texels[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
    D3D11_TEXTURE2D_DESC td = { 2, 2, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE };
    D3D11_SUBRESOURCE_DATA tinit = { texels, 8 };
    ID3D11Texture2D* tex = NULL;
    dev->CreateTexture2D(&td, &tinit, &tex);
    ID3D11ShaderResourceView* srv = NULL;
    dev->CreateShaderResourceView(tex, NULL, &srv);
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* ss = NULL;
    dev->CreateSamplerState(&smp, &ss);

    int frames = 0;
    for (; frames < 120; frames++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        memcpy(m.pData, params, sizeof params);
        ctx->Unmap(cb, 0);
        float clear[4] = { 0.05f, 0.1f, 0.3f, 1.0f };
        ctx->ClearRenderTargetView(rtv, clear);
        ctx->OMSetRenderTargets(1, &rtv, NULL);
        D3D11_VIEWPORT vp = { 0, 0, 640, 360, 0, 1 };
        ctx->RSSetViewports(1, &vp);
        UINT stride = sizeof(Vertex), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        ctx->IASetInputLayout(layout);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs, NULL, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShader(ps, NULL, 0);
        ctx->PSSetConstantBuffers(0, 1, &cb);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 1, &ss);
        ctx->Draw(3, 0);
        sc->Present(0, 0);
    }
    printf("presented %d frames\n", frames);
    DestroyWindow(hwnd);
    return 0;
}
