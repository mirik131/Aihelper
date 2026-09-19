#include "gui/media.h"
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>

namespace wmc = winrt::Windows::Media::Control;
namespace ws = winrt::Windows::Storage::Streams;

namespace media {
namespace {

ID3D11Device* g_dev = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
int g_covW = 0, g_covH = 0;

std::thread g_thr;
std::mutex g_mtx;
std::atomic<bool> g_stop{ false };
Info g_info;
std::string g_artKey; // title\nartist текущей обложки
std::vector<uint8_t> g_art; // RGBA
int g_artW = 0, g_artH = 0;
unsigned g_artVer = 0; // растёт при смене арта
unsigned g_upVer = 0;  // что уже залито в SRV
std::atomic<bool> g_dirty{ false }; // события сессии: перепроверить сейчас же
wmc::GlobalSystemMediaTransportControlsSession g_subSes{ nullptr };
winrt::event_token g_tokT{};
winrt::event_token g_tokP{};
winrt::event_token g_tokM{};
std::string g_logKey;
double g_logPos = -1;
bool g_logPlay = false;

// сырые байты картинки -> RGBA (WIC смотрит внутрь, формат любой)
bool DecodeArt(const std::vector<uint8_t>& raw, std::vector<uint8_t>& rgba, int& w, int& h)
{
    rgba.clear();
    w = 0;
    h = 0;
    if (raw.empty())
        return false;
    IWICImagingFactory* fac = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac))))
        return false;
    IWICStream* stm = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    HRESULT hr = fac->CreateStream(&stm);
    if (SUCCEEDED(hr))
        hr = stm->InitializeFromMemory((WICInProcPointer)raw.data(), (DWORD)raw.size());
    if (SUCCEEDED(hr))
        hr = fac->CreateDecoderFromStream(stm, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr))
        hr = dec->GetFrame(0, &fr);
    if (SUCCEEDED(hr))
        hr = fac->CreateFormatConverter(&cv);
    if (SUCCEEDED(hr))
        hr = cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                            nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT wdt = 0, hgt = 0;
    if (SUCCEEDED(hr))
        hr = cv->GetSize(&wdt, &hgt);
    if (SUCCEEDED(hr) && (wdt == 0 || hgt == 0 || wdt > 1024 || hgt > 1024))
        hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        rgba.resize((size_t)wdt * hgt * 4);
        hr = cv->CopyPixels(nullptr, wdt * 4, (UINT)rgba.size(), rgba.data());
    }
    if (cv)
        cv->Release();
    if (fr)
        fr->Release();
    if (dec)
        dec->Release();
    if (stm)
        stm->Release();
    fac->Release();
    if (FAILED(hr)) {
        rgba.clear();
        return false;
    }
    w = (int)wdt;
    h = (int)hgt;
    return true;
}

void PollOnce()
{
    Info ni;
    ni.tickMs = GetTickCount64();
    double rawPosDbg = 0;
    unsigned long long updAgeS = 9999;
    // подписки висят на объекте сессии, а он каждый опрос новый — старые снимаем тут
    try {
        if (g_subSes) {
            try { g_subSes.TimelinePropertiesChanged(g_tokT); } catch (...) {}
            try { g_subSes.PlaybackInfoChanged(g_tokP); } catch (...) {}
            try { g_subSes.MediaPropertiesChanged(g_tokM); } catch (...) {}
        }
    } catch (...) {
    }
    g_subSes = nullptr;
    std::string key;
    std::vector<uint8_t> art;
    int artW = 0, artH = 0;
    try {
        auto mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto ses = mgr.GetCurrentSession();
        if (ses) {
            // события сессии будят опрос мгновенно (сик/пауза/смена трека без ожидания таймера)
            try {
                g_subSes = ses;
                g_tokT = ses.TimelinePropertiesChanged([](auto&&, auto&&) { g_dirty = true; });
                g_tokP = ses.PlaybackInfoChanged([](auto&&, auto&&) { g_dirty = true; });
                g_tokM = ses.MediaPropertiesChanged([](auto&&, auto&&) { g_dirty = true; });
            } catch (...) {
                g_subSes = nullptr;
            }
            auto props = ses.TryGetMediaPropertiesAsync().get();
            ni.title = winrt::to_string(props.Title());
            ni.artist = winrt::to_string(props.Artist());
            if (!ni.title.empty() || !ni.artist.empty()) {
                ni.has = true;
                try {
                    auto pi = ses.GetPlaybackInfo();
                    ni.playing = (pi.PlaybackStatus() ==
                        wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                } catch (...) {
                }
                try {
                    auto tl = ses.GetTimelineProperties();
                    double rawPos = (double)tl.Position().count() / 10000000.0;
                    ni.dur = (double)tl.EndTime().count() / 10000000.0;
                    if (ni.dur < 0)
                        ni.dur = 0;
                    // Position актуален на момент LastUpdatedTime — дотягиваем до «сейчас» сами.
                    // Без дотяжки плееры-лентяи (SoundCloud в браузере) дают стоящие 0:00.
                    double stale = 0;
                    try {
                        auto upd = tl.LastUpdatedTime();
                        FILETIME ftn;
                        GetSystemTimeAsFileTime(&ftn);
                        ULARGE_INTEGER u;
                        u.LowPart = ftn.dwLowDateTime;
                        u.HighPart = ftn.dwHighDateTime;
                        long long d100 = (long long)(u.QuadPart - (uint64_t)upd.time_since_epoch().count());
                        if (d100 < 0)
                            d100 = 0;
                        stale = (double)d100 / 10000000.0;
                        if (stale > 300.0)
                            stale = 300.0;
                        updAgeS = (unsigned long long)stale;
                    } catch (...) {
                    }
                    rawPosDbg = rawPos;
                    ni.pos = rawPos + (ni.playing ? stale : 0.0);
                    if (ni.pos < 0)
                        ni.pos = 0;
                    if (ni.dur > 1.0 && ni.pos > ni.dur)
                        ni.pos = ni.dur;
                } catch (...) {
                }
                key = ni.title + "\n" + ni.artist;
                bool need = false;
                {
                    std::lock_guard<std::mutex> lk(g_mtx);
                    need = (key != g_artKey);
                }
                if (need) {
                    try {
                        auto thumb = props.Thumbnail();
                        if (thumb) {
                            auto stream = thumb.OpenReadAsync().get();
                            uint64_t sz = stream.Size();
                            if (sz > 0 && sz <= 20 * 1024 * 1024) {
                                ws::DataReader r(stream);
                                r.LoadAsync((uint32_t)sz).get();
                                std::vector<uint8_t> raw((size_t)sz);
                                r.ReadBytes(winrt::array_view<uint8_t>(raw));
                                r.Close();
                                DecodeArt(raw, art, artW, artH);
                            }
                            stream.Close();
                        }
                    } catch (...) {
                    }
                }
            }
        }
    } catch (...) {
        ni.has = false;
    }
    // диагностика сырых значений — видно в консоли отладки (трек/смена/прыжки)
    {
        double jump = (g_logPos < 0 || !ni.has) ? 0 : ni.pos - g_logPos;
        if (jump < 0)
            jump = -jump;
        if (key != g_logKey || ni.playing != g_logPlay || jump > 5.0) {
            printf("media: %s pos=%.1f(raw %.1f) dur=%.1f updAge=%llus playing=%d\n",
                key.empty() ? "(none)" : key.c_str(), ni.pos, rawPosDbg, ni.dur,
                updAgeS, ni.playing ? 1 : 0);
            fflush(stdout);
            g_logKey = key;
            g_logPos = ni.pos;
            g_logPlay = ni.playing;
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_info = ni;
        if (ni.has && key != g_artKey) {
            g_artKey = key;
            g_art = std::move(art);
            g_artW = artW;
            g_artH = artH;
            g_artVer++;
        } else if (!ni.has && !g_artKey.empty()) {
            g_artKey.clear();
            g_art.clear();
            g_artW = g_artH = 0;
            g_artVer++;
        }
    }
}

void Worker()
{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    PollOnce(); // первый замер сразу, без паузы
    while (!g_stop) {
        // ~0.5с между опросами, события сессии будят раньше
        for (int i = 0; i < 10 && !g_stop; i++) {
            if (g_dirty.exchange(false))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_stop)
            break;
        PollOnce();
    }
    winrt::uninit_apartment();
}

} // namespace

bool Init(ID3D11Device* dev)
{
    if (!dev)
        return false;
    Shutdown();
    g_dev = dev;
    g_stop = false;
    g_thr = std::thread(Worker);
    return true;
}

void Shutdown()
{
    g_stop = true;
    if (g_thr.joinable())
        g_thr.join();
    try {
        if (g_subSes) {
            try { g_subSes.TimelinePropertiesChanged(g_tokT); } catch (...) {}
            try { g_subSes.PlaybackInfoChanged(g_tokP); } catch (...) {}
            try { g_subSes.MediaPropertiesChanged(g_tokM); } catch (...) {}
        }
    } catch (...) {
    }
    g_subSes = nullptr;
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    g_covW = g_covH = 0;
    g_art.clear();
    g_artKey.clear();
    g_info = Info();
    g_dev = nullptr;
}

bool Snapshot(Info& out)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    out = g_info;
    return out.has;
}

void* Cover()
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_art.empty() || g_artW <= 0 || g_artH <= 0 || !g_dev) {
        if (g_srv) {
            g_srv->Release();
            g_srv = nullptr;
        }
        g_upVer = g_artVer;
        g_covW = g_covH = 0;
        return nullptr;
    }
    if (g_srv && g_upVer == g_artVer)
        return g_srv;
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)g_artW;
    td.Height = (UINT)g_artH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = g_art.data();
    sd.SysMemPitch = (UINT)(g_artW * 4);
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex)))
        return nullptr;
    HRESULT hr = g_dev->CreateShaderResourceView(tex, nullptr, &g_srv);
    tex->Release();
    if (FAILED(hr)) {
        g_srv = nullptr;
        return nullptr;
    }
    g_upVer = g_artVer;
    g_covW = g_artW;
    g_covH = g_artH;
    return g_srv;
}

void CoverSize(int& w, int& h)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    w = g_covW;
    h = g_covH;
}

} // namespace media
