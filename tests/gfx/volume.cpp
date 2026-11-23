// Fills a 32x32x32 volume with one instanced layered draw (as Unreal builds its colour-grading LUT), then shows its
// middle slice on screen. Prints the middle texel natively so the original and recompiled builds can be compared.
#include <stdio.h>
#include <windows.h>
#include <d3d11.h>

#include "volume_vs.h"
#include "volume_gs.h"
#include "volume_ps.h"
#include "volume_showvs.h"
#include "volume_showps.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int main()
{
    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"vol";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"vol", L"vol", WS_POPUP, 0, 0, 640, 360, NULL, NULL, NULL, NULL);
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
    ID3D11RenderTargetView* screen = NULL;
    dev->CreateRenderTargetView(bb, NULL, &screen);

    D3D11_TEXTURE3D_DESC td = { 32, 32, 32, 1, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE };
    ID3D11Texture3D* vol = NULL;
    dev->CreateTexture3D(&td, NULL, &vol);
    D3D11_RENDER_TARGET_VIEW_DESC rd = {};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
    rd.Texture3D.FirstWSlice = 0;
    rd.Texture3D.WSize = 32;
    ID3D11RenderTargetView* volrt = NULL;
    dev->CreateRenderTargetView(vol, &rd, &volrt);
    ID3D11ShaderResourceView* volsrv = NULL;
    dev->CreateShaderResourceView(vol, NULL, &volsrv);

    ID3D11VertexShader *vs = NULL, *showvs = NULL;
    ID3D11GeometryShader* gs = NULL;
    ID3D11PixelShader *ps = NULL, *showps = NULL;
    dev->CreateVertexShader(g_VSMain, sizeof g_VSMain, NULL, &vs);
    dev->CreateGeometryShader(g_GSMain, sizeof g_GSMain, NULL, &gs);
    dev->CreatePixelShader(g_PSMain, sizeof g_PSMain, NULL, &ps);
    dev->CreateVertexShader(g_VSShow, sizeof g_VSShow, NULL, &showvs);
    dev->CreatePixelShader(g_PSShow, sizeof g_PSShow, NULL, &showps);
    D3D11_INPUT_ELEMENT_DESC el[] = { { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    ID3D11InputLayout* layout = NULL;
    dev->CreateInputLayout(el, 1, g_VSMain, sizeof g_VSMain, &layout);
    float quad[8] = { -1, -1, -1, 1, 1, -1, 1, 1 };
    D3D11_BUFFER_DESC bd = { sizeof quad, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA init = { quad };
    ID3D11Buffer* vb = NULL;
    dev->CreateBuffer(&bd, &init, &vb);
    D3D11_SAMPLER_DESC smp = {};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState* ss = NULL;
    dev->CreateSamplerState(&smp, &ss);

    // the volume, once, as Unreal does
    ctx->OMSetRenderTargets(1, &volrt, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 32, 32, 0, 1 };
    ctx->RSSetViewports(1, &vp);
    UINT stride = 8, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(vs, NULL, 0);
    ctx->GSSetShader(gs, NULL, 0);
    ctx->PSSetShader(ps, NULL, 0);
    ctx->DrawInstanced(4, 32, 0, 0);
    ctx->GSSetShader(NULL, NULL, 0);

    int frames = 0;
    for (; frames < 120; frames++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        ctx->OMSetRenderTargets(1, &screen, NULL);
        D3D11_VIEWPORT sv = { 0, 0, 640, 360, 0, 1 };
        ctx->RSSetViewports(1, &sv);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->IASetInputLayout(NULL);
        ctx->VSSetShader(showvs, NULL, 0);
        ctx->PSSetShader(showps, NULL, 0);
        ctx->PSSetShaderResources(0, 1, &volsrv);
        ctx->PSSetSamplers(0, 1, &ss);
        ctx->Draw(3, 0);
        sc->Present(0, 0);
    }
    printf("presented %d frames\n", frames);
    DestroyWindow(hwnd);
    return 0;
}
