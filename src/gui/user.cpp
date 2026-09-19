#include "gui/user.h"
#include "gui/discord.h"
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <cstdio>

namespace user {
namespace {

UserCard g_card;
bool g_loaded = false;
bool g_discordPic = false; // аватарку уже взяли из дискорда
ID3D11ShaderResourceView* g_srv = nullptr;
bool g_com = false;

// грузим первую картинку что декодируется (расширение не важно, WIC смотрит внутрь)
bool TryAvatar(ID3D11Device* device, const wchar_t* path)
{
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))))
        return false;
    IWICBitmapDecoder* dec = nullptr;
    HRESULT hr = fac->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
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
    if (SUCCEEDED(hr) && (wdt < 32 || hgt < 32))
        hr = E_FAIL; // мелочь не берём
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
        hr = device->CreateTexture2D(&td, nullptr, &tex);
    }
    if (SUCCEEDED(hr)) {
        device->GetImmediateContext(&ctx);
        hr = ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    }
    if (SUCCEEDED(hr)) {
        mapped = true;
        hr = cv->CopyPixels(nullptr, ms.RowPitch, ms.RowPitch * hgt, (BYTE*)ms.pData);
    }
    if (mapped)
        ctx->Unmap(tex, 0);
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(tex, nullptr, &g_srv);
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
    return SUCCEEDED(hr);
}

} // namespace

bool Init(ID3D11Device* device)
{
    Shutdown();
    discord::Start(device); // сам подтянет ник+аватарку когда сможет (нужен ID в discord.cpp)

    // имя винды — без разницы где запущено
    wchar_t name[257];
    DWORD n = 257;
    if (GetUserNameW(name, &n) && n > 1) {
        char out[257];
        if (WideCharToMultiByte(CP_UTF8, 0, name, -1, out, sizeof(out), nullptr, nullptr) > 1)
            g_card.name = out;
    }
    g_card.sub = "\xE2\x88\x9E"; // бесконечность

    // картинка учётки винды если есть
    g_com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    wchar_t base[300];
    DWORD dn = GetEnvironmentVariableW(L"APPDATA", base, 300);
    if (dn == 0 || dn >= 250)
        return true; // имя уже есть, аватарки не будет — не страшно
    wcscat_s(base, L"\\Microsoft\\Windows\\AccountPictures\\");
    wchar_t mask[310];
    wcscpy_s(mask, base);
    wcscat_s(mask, L"*");
    WIN32_FIND_DATAW ff = {};
    HANDLE h = FindFirstFileW(mask, &ff);
    if (h == INVALID_HANDLE_VALUE)
        return true;
    do {
        if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        wchar_t full[340];
        wcscpy_s(full, base);
        wcscat_s(full, ff.cFileName);
        if (TryAvatar(device, full)) {
            g_card.avatarTex = g_srv;
            printf("userpic ok\n");
            break;
        }
    } while (FindNextFileW(h, &ff));
    FindClose(h);
    g_loaded = true;
    return true;
}

const UserCard& Get()
{
    // дискорд подоспел — забираем ник и аватарку оттуда
    if (const char* dn = discord::Name()) {
        if (g_card.name != dn)
            g_card.name = dn;
    }
    if (!g_discordPic) {
        if (void* da = discord::Avatar()) {
            g_discordPic = true;
            g_card.avatarTex = da;
            printf("discord pic ok: %s\n", g_card.name.c_str());
            fflush(stdout);
        }
    }
    return g_card;
}

void Shutdown()
{
    discord::Shutdown();
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    if (g_com) {
        CoUninitialize();
        g_com = false;
    }
    g_card = UserCard();
    g_loaded = false;
    g_discordPic = false;
}

} // namespace user
