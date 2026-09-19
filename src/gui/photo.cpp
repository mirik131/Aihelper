#include "gui/photo.h"
#include <wincodec.h>
#include <cstdio>
#include <string>
#include <map>

namespace photo {
namespace {

ID3D11Device* g_dev = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
int g_w = 0, g_h = 0;
bool g_com = false;
std::map<std::string, void*> g_icons; // путь -> SRV (null тоже кэшируем)

bool ComInit()
{
    // общие COM-разговорчики один раз на всех
    if (!g_com)
        g_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    return g_com;
}

// декодит файл (формат любой — WIC смотрит внутрь) в SRV 1:1
bool DecodeToTexture(const char* path, ID3D11ShaderResourceView** outSrv, int* outW, int* outH)
{
    wchar_t w[260];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 260);
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))))
        return false;
    IWICBitmapDecoder* dec = nullptr;
    HRESULT hr = fac->CreateDecoderFromFilename(w, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    UINT wdt = 0, hgt = 0;
    D3D11_MAPPED_SUBRESOURCE ms = {};
    bool mapped = false;
    if (SUCCEEDED(hr))
        hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr))
        hr = fac->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr))
        hr = cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr))
        hr = cv->GetSize(&wdt, &hgt);
    if (SUCCEEDED(hr) && (wdt == 0 || hgt == 0))
        hr = E_FAIL;
    D3D11_TEXTURE2D_DESC td = {};
    if (SUCCEEDED(hr)) {
        td.Width = wdt;
        td.Height = hgt;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = g_dev->CreateTexture2D(&td, nullptr, &tex);
    }
    if (SUCCEEDED(hr)) {
        g_dev->GetImmediateContext(&ctx);
        hr = ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    }
    if (SUCCEEDED(hr)) {
        mapped = true;
        hr = cv->CopyPixels(nullptr, ms.RowPitch, ms.RowPitch * hgt, (BYTE*)ms.pData);
    }
    if (mapped)
        ctx->Unmap(tex, 0);
    if (SUCCEEDED(hr))
        hr = g_dev->CreateShaderResourceView(tex, nullptr, outSrv);
    if (tex)
        tex->Release();
    if (ctx)
        ctx->Release();
    if (cv)
        cv->Release();
    if (fr)
        fr->Release();
    if (dec)
        dec->Release();
    fac->Release();
    if (SUCCEEDED(hr)) {
        if (outW)
            *outW = (int)wdt;
        if (outH)
            *outH = (int)hgt;
    }
    return SUCCEEDED(hr);
}

} // namespace

bool Init(ID3D11Device* device)
{
    Shutdown();
    g_dev = device;
    ComInit();
    ComInit();

    if (DecodeToTexture("Icon/image/tin.png", &g_srv, &g_w, &g_h))
        printf("photo: Icon/image/tin.png %dx%d\n", g_w, g_h);
    else if (DecodeToTexture("../../Icon/image/tin.png", &g_srv, &g_w, &g_h))
        printf("photo: ../../Icon/image/tin.png %dx%d\n", g_w, g_h);
    else if (DecodeToTexture("Icon/image/mainmenu/fallback.jpg", &g_srv, &g_w, &g_h))
        printf("photo: fallback.jpg %dx%d\n", g_w, g_h);
    else if (DecodeToTexture("../../Icon/image/mainmenu/fallback.jpg", &g_srv, &g_w, &g_h))
        printf("photo: ../../fallback.jpg %dx%d\n", g_w, g_h);
    else
        return false;
    return true;
}

void* Get()
{
    return g_srv;
}

void Size(int* w, int* h)
{
    if (w)
        *w = g_w;
    if (h)
        *h = g_h;
}

void* LoadIcon(const char* path)
{
    auto it = g_icons.find(path);
    if (it != g_icons.end())
        return it->second; // уже грузили (null — значит не далась)
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    if (!DecodeToTexture(path, &srv, &w, &h)) {
        std::string alt = std::string("../../") + path;
        DecodeToTexture(alt.c_str(), &srv, &w, &h);
    }
    g_icons[path] = srv;
    if (srv)
        printf("icon: %s\n", path);
    return srv;
}

void Shutdown()
{
    for (auto& kv : g_icons) {
        if (kv.second)
            ((ID3D11ShaderResourceView*)kv.second)->Release();
    }
    g_icons.clear();
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    g_w = g_h = 0;
    g_dev = nullptr;
    if (g_com) {
        CoUninitialize();
        g_com = false;
    }
}

} // namespace photo
