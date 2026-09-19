#include "gui/discord.h"
#include <windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <urlmon.h>
#include <cstdio>
#include <string>
#include <cstring>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

// ID не нужен: id+хэш берём из локальных файлов дискорда (ниже).

namespace discord {
namespace {

// то что притащил фоновый поток (под мьютексом)
std::mutex g_mtx;
bool g_done = false;
bool g_ok = false;
std::string g_name;
std::vector<uint8_t> g_rgba;
int g_pw = 0, g_ph = 0;

ID3D11Device* g_dev = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
bool g_started = false;

// достаём "key":"value" из плоского JSON (без либы, нам хватает).
// \uXXXX разворачиваем в UTF-8 чтобы русские ники не сыпались.
std::string JsonStr(const std::string& js, const char* key)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = js.find(k);
    if (p == std::string::npos)
        return "";
    p = js.find(':', p + k.size());
    if (p == std::string::npos)
        return "";
    p++;
    while (p < js.size() && (js[p] == ' ' || js[p] == '\t'))
        p++;
    if (p + 4 <= js.size() && js.compare(p, 4, "null") == 0)
        return "";
    if (p >= js.size() || js[p] != '"')
        return "";
    p++;
    std::string out;
    while (p < js.size() && js[p] != '"') {
        if (js[p] == '\\' && p + 1 < js.size()) {
            char e = js[p + 1];
            if (e == 'u' && p + 5 < js.size()) {
                unsigned cp = 0;
                for (int i = 0; i < 4; i++) {
                    char h = js[p + 2 + i];
                    cp <<= 4;
                    if (h >= '0' && h <= '9')
                        cp |= (unsigned)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        cp |= (unsigned)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        cp |= (unsigned)(h - 'A' + 10);
                }
                // суррогатные пары (эмодзи в нике)
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 11 < js.size() &&
                    js[p + 6] == '\\' && js[p + 7] == 'u') {
                    unsigned lo = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = js[p + 8 + i];
                        lo <<= 4;
                        if (h >= '0' && h <= '9')
                            lo |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f')
                            lo |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F')
                            lo |= (unsigned)(h - 'A' + 10);
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                if (cp < 0x80)
                    out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                p += 6;
                continue;
            }
            out += e;
            p += 2;
            continue;
        }
        out += js[p++];
    }
    return out;
}

std::mutex g_resMtx;
bool g_resDone = false;
std::string g_resName;
std::vector<uint8_t> g_resRgba;
int g_resW = 0, g_resH = 0;

static bool ReadVarint(const uint8_t* d, size_t n, size_t& p, uint64_t& v)
{
    v = 0;
    int shift = 0;
    while (p < n) {
        uint8_t b = d[p++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80))
            return true;
        shift += 7;
        if (shift >= 64)
            return false;
    }
    return false;
}

// распаковка Snappy (только то что кладёт leveldb)
static bool SnappyDecompress(const uint8_t* d, size_t n, std::vector<uint8_t>& out)
{
    size_t p = 0;
    uint64_t total = 0;
    if (!ReadVarint(d, n, p, total))
        return false;
    out.clear();
    while (p < n) {
        uint8_t tag = d[p] & 0x03;
        if (tag == 0) {
            uint64_t l = d[p] >> 2;
            p++;
            if (l < 60) {
                l++;
            } else {
                int nb = (int)l - 59;
                if (p + (size_t)nb > n)
                    return false;
                l = 0;
                for (int i = 0; i < nb; i++)
                    l |= (uint64_t)d[p + i] << (8 * i);
                l++;
                p += nb;
            }
            if (p + (size_t)l > n)
                return false;
            out.insert(out.end(), d + p, d + p + (size_t)l);
            p += (size_t)l;
        } else {
            uint64_t l = 0, off = 0;
            if (tag == 1) {
                if (p + 2 > n)
                    return false;
                l = ((d[p] >> 2) & 0x7) + 4;
                off = ((uint64_t)(d[p] >> 5) << 8) | d[p + 1];
                p += 2;
            } else if (tag == 2) {
                if (p + 3 > n)
                    return false;
                l = (d[p] >> 2) + 1;
                off = d[p + 1] | ((uint64_t)d[p + 2] << 8);
                p += 3;
            } else {
                if (p + 5 > n)
                    return false;
                l = (d[p] >> 2) + 1;
                off = (uint64_t)d[p + 1] | ((uint64_t)d[p + 2] << 8) |
                      ((uint64_t)d[p + 3] << 16) | ((uint64_t)d[p + 4] << 24);
                p += 5;
            }
            if (off == 0 || off > out.size())
                return false;
            size_t at = out.size();
            out.resize(at + (size_t)l);
            for (uint64_t i = 0; i < l; i++)
                out[at + (size_t)i] = out[at + (size_t)i - (size_t)off];
        }
    }
    return true;
}

static uint32_t RdU32(const std::vector<uint8_t>& b, size_t p)
{
    return (uint32_t)b[p] | ((uint32_t)b[p + 1] << 8) |
           ((uint32_t)b[p + 2] << 16) | ((uint32_t)b[p + 3] << 24);
}

// записи блока таблицы: shared/non_shared/value_len + key + value.
// fn(key, keyLen, val, valLen), key включает 8 байт internal-трейлера.
template <typename Fn>
static bool WalkRecords(const std::vector<uint8_t>& blk, Fn&& fn)
{
    if (blk.size() < 4)
        return false;
    uint32_t nrest = RdU32(blk, blk.size() - 4);
    if (4 + 4 * (size_t)nrest > blk.size())
        return false;
    std::string last;
    for (uint32_t ri = 0; ri < nrest; ri++) {
        size_t ro = RdU32(blk, blk.size() - 4 - 4 * nrest + 4 * ri);
        size_t rend = (ri + 1 < nrest) ? RdU32(blk, blk.size() - 4 - 4 * nrest + 4 * (ri + 1))
                                       : blk.size() - 4 - 4 * nrest;
        size_t q = ro;
        while (q < rend) {
            uint64_t sh = 0, ns = 0, vl = 0;
            if (!ReadVarint(blk.data(), rend, q, sh) ||
                !ReadVarint(blk.data(), rend, q, ns) ||
                !ReadVarint(blk.data(), rend, q, vl))
                break;
            if (q + (size_t)ns + (size_t)vl > rend)
                break;
            std::string key = last.substr(0, (size_t)sh);
            key.append((const char*)blk.data() + q, (size_t)ns);
            q += (size_t)ns;
            fn(key, blk.data() + q, (size_t)vl);
            q += (size_t)vl;
            last.swap(key);
        }
    }
    return true;
}

static bool ReadBlock(const std::vector<uint8_t>& f, uint64_t off, uint64_t size, std::vector<uint8_t>& blk)
{
    if (off + size + 5 > f.size())
        return false;
    uint8_t ctype = f[(size_t)(off + size)];
    if (ctype == 0) {
        blk.assign(f.begin() + (size_t)off, f.begin() + (size_t)(off + size));
        return true;
    }
    if (ctype == 1)
        return SnappyDecompress(f.data() + (size_t)off, (size_t)size, blk);
    return false;
}

static bool IsIdStr(const std::string& s) // 17-19 цифр
{
    if (s.size() < 17 || s.size() > 19)
        return false;
    for (char c : s)
        if (c < '0' || c > '9')
            return false;
    return true;
}

static bool IsAvStr(const std::string& s) // 32 hex или a_+32 hex
{
    size_t o = (s.size() > 2 && s[0] == 'a' && s[1] == '_') ? 2 : 0;
    if (s.size() - o != 32)
        return false;
    for (size_t i = o; i < s.size(); i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

// значение "key":"value" с позиции from (кавычки/пробелы терпим)
static bool ScanValue(const std::string& hay, size_t from, const char* key, std::string& val, size_t& endPos)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = hay.find(k, from);
    if (p == std::string::npos)
        return false;
    p = hay.find(':', p + k.size());
    if (p == std::string::npos)
        return false;
    p++;
    while (p < hay.size() && (hay[p] == ' ' || hay[p] == '\t'))
        p++;
    if (p >= hay.size() || hay[p] != '"')
        return false;
    p++;
    size_t e = hay.find('"', p);
    if (e == std::string::npos || e - p == 0 || e - p > 200)
        return false;
    val.assign(hay, p, e - p);
    endPos = e + 1;
    return true;
}

struct Cand {
    std::string uid, av, name;
    int emailDist;
    int score;
    uint64_t sourceStamp; // время файла: свежий кэш почти всегда текущая сессия
};

static const int kFar = 1 << 30;

// один кусок текста (значение из базы или целый .log): все пары avatar+id.
// Свой объект ищем по якорю email, остальных оцениваем очками.
static void ScanText(const std::string& blob, uint64_t sourceStamp, std::vector<Cand>& out)
{
    std::vector<size_t> emails;
    {
        size_t ep = 0;
        while (true) {
            size_t kp = blob.find("\"email\"", ep);
            if (kp == std::string::npos)
                break;
            std::string ev;
            size_t ee = 0;
            if (ScanValue(blob, kp, "email", ev, ee) && ev.find('@') != std::string::npos)
                emails.push_back(kp);
            ep = kp + 7;
        }
    }
    size_t pos = 0;
    while (true) {
        size_t kp = blob.find("\"avatar\"", pos);
        if (kp == std::string::npos)
            break;
        std::string av;
        size_t avEnd = 0;
        if (!ScanValue(blob, kp, "avatar", av, avEnd) || !IsAvStr(av)) {
            pos = kp + 8;
            continue;
        }
        size_t lo = kp > 2048 ? kp - 2048 : 0;
        size_t hi = avEnd + 2048 < blob.size() ? avEnd + 2048 : blob.size();
        std::string around = blob.substr(lo, hi - lo);
        std::string id, raw;
        size_t unused = 0;
        if (!(ScanValue(around, 0, "id", id, unused) && IsIdStr(id))) {
            pos = avEnd;
            continue;
        }
        if (!ScanValue(around, 0, "global_name", raw, unused) || raw.empty())
            ScanValue(around, 0, "username", raw, unused);
        std::string nice = JsonStr(std::string("{\"v\":\"") + raw + "\"}", "v");
        int dist = kFar;
        for (size_t ep : emails) {
            if (ep < lo || ep > hi)
                continue;
            int d = (int)(ep > kp ? ep - kp : kp - ep);
            if (d < dist)
                dist = d;
        }
        int score = 1;
        if (around.find("\"locale\"") != std::string::npos)
            score++;
        if (around.find("\"verified\"") != std::string::npos)
            score++;
        Cand c;
        c.uid = id;
        c.av = av;
        c.name = nice.empty() ? raw : nice;
        c.emailDist = dist;
        c.score = score;
        c.sourceStamp = sourceStamp;
        out.push_back(c);
        pos = avEnd;
    }
}

// проход по .ldb таблице: индекс -> data-блоки -> значения в ScanText
static void ScanLdb(const std::string& path, uint64_t sourceStamp, std::vector<Cand>& out)
{
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return;
    DWORD sz = GetFileSize(f, nullptr);
    std::vector<uint8_t> data;
    if (sz > 48 && sz < 256 * 1024 * 1024) {
        data.resize(sz);
        DWORD r = 0;
        if (!ReadFile(f, data.data(), sz, &r, nullptr) || r != sz)
            data.clear();
    }
    CloseHandle(f);
    if (data.size() < 48)
        return;
    static const uint8_t kMagic[8] = { 0x57, 0xfb, 0x80, 0x8b, 0x24, 0x75, 0x47, 0xdb };
    if (memcmp(data.data() + data.size() - 8, kMagic, 8) != 0)
        return;
    const uint8_t* foot = data.data() + data.size() - 48;
    size_t p = 0;
    uint64_t dummy = 0, ioff = 0, isz = 0;
    if (!ReadVarint(foot, 48, p, dummy) || !ReadVarint(foot, 48, p, dummy) ||
        !ReadVarint(foot, 48, p, ioff) || !ReadVarint(foot, 48, p, isz))
        return;
    std::vector<uint8_t> iblock;
    if (!ReadBlock(data, ioff, isz, iblock))
        return;
    WalkRecords(iblock, [&](const std::string& ikey, const uint8_t* ival, size_t ilen) {
        (void)ikey;
        size_t q = 0;
        uint64_t doff = 0, dsz = 0;
        if (!ReadVarint(ival, ilen, q, doff) || !ReadVarint(ival, ilen, q, dsz))
            return;
        std::vector<uint8_t> dblock;
        if (!ReadBlock(data, doff, dsz, dblock))
            return;
        WalkRecords(dblock, [&](const std::string& k2, const uint8_t* v2, size_t l2) {
            (void)k2;
            ScanText(std::string((const char*)v2, l2), sourceStamp, out);
        });
    });
}

// сырые .log тоже смотрим как есть (там записи почти целые)
static void ScanFiles(const std::string& dir, const char* pat, bool parsed, std::vector<Cand>& out)
{
    WIN32_FIND_DATAA ff = {};
    HANDLE h = FindFirstFileA((dir + pat).c_str(), &ff);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        std::string full = dir + ff.cFileName;
        uint64_t stamp = ((uint64_t)ff.ftLastWriteTime.dwHighDateTime << 32) |
            ff.ftLastWriteTime.dwLowDateTime;
        if (parsed) {
            ScanLdb(full, stamp, out);
            continue;
        }
        HANDLE f = CreateFileA(full.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE)
            continue;
        DWORD sz = GetFileSize(f, nullptr);
        if (sz > 0 && sz < 64 * 1024 * 1024) {
            std::string data(sz, '\0');
            DWORD r = 0;
            if (ReadFile(f, &data[0], sz, &r, nullptr) && r == sz)
                ScanText(data, stamp, out);
        }
        CloseHandle(f);
    } while (FindNextFileA(h, &ff));
    FindClose(h);
}

void Worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string name, uid, avhash;

    char appdata[300];
    DWORD dn = GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata));
    std::vector<Cand> cands;
    if (dn > 0 && dn < 250) {
        std::string dir = std::string(appdata) + "\\discord\\Local Storage\\leveldb\\";
        ScanFiles(dir, "*.ldb", true, cands);
        ScanFiles(dir, "*.log", false, cands);
    }
    // В LevelDB годами лежат объекты от старых аккаунтов. Раньше при равных
    // score побеждал первый найденный — отсюда чужие ник и аватар. Выбираем
    // объект текущей сессии: email — самый сильный якорь, затем свежесть файла.
    const Cand* pick = nullptr;
    for (const auto& c : cands) {
        if (c.name.empty())
            continue;
        if (!pick) {
            pick = &c;
            continue;
        }
        const bool own = c.emailDist < 8192;
        const bool pickedOwn = pick->emailDist < 8192;
        if (own != pickedOwn) {
            if (own)
                pick = &c;
            continue;
        }
        if (c.sourceStamp != pick->sourceStamp) {
            if (c.sourceStamp > pick->sourceStamp)
                pick = &c;
            continue;
        }
        if (c.score > pick->score ||
            (c.score == pick->score && c.emailDist < pick->emailDist))
            pick = &c;
    }
    if (pick) {
        uid = pick->uid;
        avhash = pick->av;
        name = pick->name;
    }
    std::vector<uint8_t> rgba;
    int pw = 0, ph = 0;
    if (!uid.empty() && !avhash.empty()) {
        // гифка если хэш на a_ — WIC первый кадр отдаст, нам хватит
        const char* ext = (avhash.size() > 2 && avhash[0] == 'a' && avhash[1] == '_') ? "gif" : "png";
        char link[256];
        sprintf_s(link, "https://cdn.discordapp.com/avatars/%s/%s.%s?size=128", uid.c_str(), avhash.c_str(), ext);
        wchar_t tmp[300], wlink[300];
        GetTempPathW(300, tmp);
        wcscat_s(tmp, L"helper_avatar.png");
        MultiByteToWideChar(CP_UTF8, 0, link, -1, wlink, 300);
        if (SUCCEEDED(URLDownloadToFileW(nullptr, wlink, tmp, 0, nullptr))) {
            IWICImagingFactory* fac = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac)))) {
                IWICBitmapDecoder* dec = nullptr;
                if (SUCCEEDED(fac->CreateDecoderFromFilename(tmp, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
                    IWICBitmapFrameDecode* fr = nullptr;
                    IWICFormatConverter* cv = nullptr;
                    UINT wdt = 0, hgt = 0;
                    HRESULT hr = dec->GetFrame(0, &fr);
                    if (SUCCEEDED(hr))
                        hr = fac->CreateFormatConverter(&cv);
                    if (SUCCEEDED(hr))
                        hr = cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
                    if (SUCCEEDED(hr))
                        hr = cv->GetSize(&wdt, &hgt);
                    if (SUCCEEDED(hr) && wdt > 0 && hgt > 0 && wdt <= 512 && hgt <= 512) {
                        rgba.resize((size_t)wdt * hgt * 4);
                        hr = cv->CopyPixels(nullptr, wdt * 4, (UINT)rgba.size(), rgba.data());
                        if (SUCCEEDED(hr)) {
                            pw = (int)wdt;
                            ph = (int)hgt;
                        } else {
                            rgba.clear();
                        }
                    }
                    if (cv)
                        cv->Release();
                    if (fr)
                        fr->Release();
                    dec->Release();
                }
                fac->Release();
            }
        }
        DeleteFileW(tmp);
    }

    // кладём результат под мьютекс
    {
        std::lock_guard<std::mutex> lk(g_resMtx);
        g_resName = name;
        g_resRgba = std::move(rgba);
        g_resW = pw;
        g_resH = ph;
        g_resDone = true;
    }
    printf("discord scan: %s\n", !uid.empty() ? uid.c_str() : "nothing");
    fflush(stdout);
    CoUninitialize();
}

} // namespace

void Start(void* device)
{
    if (g_started)
        return;
    g_started = true;
    g_dev = (ID3D11Device*)device;
    std::thread(Worker).detach();
}

const char* Name()
{
    std::lock_guard<std::mutex> lk(g_resMtx);
    if (!g_resDone || g_resName.empty())
        return nullptr;
    return g_resName.c_str();
}

void* Avatar()
{
    std::lock_guard<std::mutex> lk(g_resMtx);
    if (!g_resDone || g_resRgba.empty() || !g_dev)
        return nullptr;
    if (g_srv)
        return g_srv; // уже залили
    ID3D11Texture2D* tex = nullptr;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)g_resW;
    td.Height = (UINT)g_resH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = g_resRgba.data();
    sd.SysMemPitch = (UINT)g_resW * 4;
    if (FAILED(g_dev->CreateTexture2D(&td, &sd, &tex)))
        return nullptr;
    if (FAILED(g_dev->CreateShaderResourceView(tex, nullptr, &g_srv)))
        g_srv = nullptr;
    tex->Release();
    g_resRgba.clear();
    g_resRgba.shrink_to_fit();
    return g_srv;
}

void Shutdown()
{
    if (g_srv) {
        g_srv->Release();
        g_srv = nullptr;
    }
    g_dev = nullptr;
}

} // namespace discord
