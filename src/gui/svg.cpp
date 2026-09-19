#include "gui/svg.h"
#include <windows.h>
#include <d3d11.h>
#include <d2d1_3.h>
#include <d2d1helper.h>
#include <dxgi.h>
#include <cstdio>
#include <string>
#include <map>
#include <vector>

namespace svg {
namespace {

ID3D11Device* g_dev = nullptr;
ID2D1Factory1* g_fac = nullptr;
std::map<std::string, void*> g_cache; // путь#px -> SRV (null тоже кэшируем)

void ReleaseAll()
{
    for (auto& kv : g_cache) {
        if (kv.second)
            ((ID3D11ShaderResourceView*)kv.second)->Release();
    }
    g_cache.clear();
    if (g_fac) {
        g_fac->Release();
        g_fac = nullptr;
    }
}

// файл -> SRV px*px (B8G8R8A8), null при любой беде
void* RenderSvg(const char* path, int px)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return nullptr;
    }
    std::vector<char> buf((size_t)sz);
    size_t got = fread(buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz)
        return nullptr;
    // IStream без shlwapi: через HGLOBAL (ole32 уже в линковке по умолчанию)
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)sz);
    if (!hg)
        return nullptr;
    memcpy(GlobalLock(hg), buf.data(), (size_t)sz);
    GlobalUnlock(hg);
    IStream* stm = nullptr;
    if (FAILED(CreateStreamOnHGlobal(hg, TRUE, &stm)))
        return nullptr;

    if (!g_fac) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                     __uuidof(ID2D1Factory1), nullptr, (void**)&g_fac))) {
            stm->Release();
            return nullptr;
        }
    }
    IDXGIDevice* dxgiDev = nullptr;
    ID2D1Device* d2dDev = nullptr;
    ID2D1DeviceContext* dctx = nullptr;
    ID2D1DeviceContext5* dc5 = nullptr;
    ID2D1SvgDocument* doc = nullptr;
    ID3D11Texture2D* tex = nullptr;
    IDXGISurface* surf = nullptr;
    ID2D1Bitmap1* bmp = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;

    HRESULT hr = g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    int step = 1;
    if (SUCCEEDED(hr)) {
        step = 2;
        hr = g_fac->CreateDevice(dxgiDev, &d2dDev);
    }
    if (SUCCEEDED(hr)) {
        step = 3;
        hr = d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dctx);
    }
    if (SUCCEEDED(hr)) {
        step = 4;
        hr = dctx->QueryInterface(__uuidof(ID2D1DeviceContext5), (void**)&dc5);
    }
    if (SUCCEEDED(hr)) {
        step = 5;
        hr = dc5->CreateSvgDocument(stm, D2D1::SizeF((float)px, (float)px), &doc);
    }
    if (SUCCEEDED(hr)) {
        // Файлы иконок мелкие (width=6) — D2D рисует их 1:1 в углу.
        // Форсируем корень и вьюпорт под нужный px, иначе будет мелочь.
        step = 6;
        ID2D1SvgElement* root = nullptr;
        doc->GetRoot(&root); // void-геттер, не HRESULT
        if (root) {
            D2D1_SVG_LENGTH len;
            len.value = (float)px;
            len.units = D2D1_SVG_LENGTH_UNITS_NUMBER; // без единиц = px
            hr = root->SetAttributeValue(L"width", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH,
                                         &len, sizeof(len));
            if (SUCCEEDED(hr))
                hr = root->SetAttributeValue(L"height", D2D1_SVG_ATTRIBUTE_POD_TYPE_LENGTH,
                                             &len, sizeof(len));
            root->Release();
        }
        if (SUCCEEDED(hr))
            hr = doc->SetViewportSize(D2D1::SizeF((float)px, (float)px));
    }
    D3D11_TEXTURE2D_DESC td = {};
    if (SUCCEEDED(hr)) {
        td.Width = td.Height = (UINT)px;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        hr = g_dev->CreateTexture2D(&td, nullptr, &tex);
    }
    if (SUCCEEDED(hr))
        hr = tex->QueryInterface(__uuidof(IDXGISurface), (void**)&surf);
    if (SUCCEEDED(hr)) {
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        hr = dc5->CreateBitmapFromDxgiSurface(surf, &bp, &bmp);
    }
    if (SUCCEEDED(hr)) {
        dc5->SetTarget(bmp);
        dc5->BeginDraw();
        dc5->Clear(D2D1::ColorF(0, 0));
        dc5->DrawSvgDocument(doc);
        hr = dc5->EndDraw();
    }
    if (SUCCEEDED(hr))
        hr = g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    if (bmp)
        bmp->Release();
    if (surf)
        surf->Release();
    if (tex)
        tex->Release();
    if (doc)
        doc->Release();
    if (dc5)
        dc5->Release();
    if (dctx)
        dctx->Release();
    if (d2dDev)
        d2dDev->Release();
    if (dxgiDev)
        dxgiDev->Release();
    stm->Release();
    if (FAILED(hr)) {
        printf("svg miss: %s step=%d hr=%08x\n", path, step, (unsigned)hr);
        fflush(stdout);
        return nullptr;
    }
    printf("svg ok: %s\n", path);
    fflush(stdout);
    return srv;
}

} // namespace

bool Init(ID3D11Device* dev)
{
    if (!dev)
        return false;
    Shutdown();
    g_dev = dev;
    return true;
}

void Shutdown()
{
    ReleaseAll();
    g_dev = nullptr;
}

void* Load(const char* path, int px)
{
    if (!path || !*path || !g_dev || px <= 0)
        return nullptr;
    std::string key = std::string(path) + "#" + std::to_string(px);
    auto it = g_cache.find(key);
    if (it != g_cache.end())
        return it->second;
    void* srv = RenderSvg(path, px);
    if (!srv) {
        std::string alt = std::string("../../") + path;
        srv = RenderSvg(alt.c_str(), px);
    }
    g_cache[key] = srv;
    return srv;
}

} // namespace svg
