#include "gui/actions.h"
#include "gui/config.h"
#include "gui/media.h"
#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <UIAutomation.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <thread>

namespace actions {
namespace {

// нижний регистр для поиска (ASCII хватает — имена ярлыков такие)
std::string lowerAscii(const std::string& s)
{
    std::string o = s;
    for (char& c : o)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
    return o;
}

bool EndsWith(const std::string& s, const char* suf)
{
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// собираем *.lnk рекурсивно
void CollectLnk(const std::string& dir, std::vector<std::string>& out)
{
    std::string mask = dir + "\\*";
    WIN32_FIND_DATAA ff = {};
    HANDLE h = FindFirstFileA(mask.c_str(), &ff);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (ff.cFileName[0] == '.')
            continue;
        std::string full = dir + "\\" + ff.cFileName;
        if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectLnk(full, out);
        } else if (EndsWith(lowerAscii(full), ".lnk")) {
            out.push_back(full);
        }
    } while (FindNextFileA(h, &ff));
    FindClose(h);
}

struct CloseNeedle {
    std::string needle;
    bool closed = false;
};

// ниже по файлу (CloseEnum их зовёт раньше объявления)
std::string lowerRu(const std::string& s);
void KeyCombo(WORD mod, WORD vk);

bool IsBrowserClass(const char* cls)
{
    std::string c = lowerAscii(cls);
    return c == "chrome_widgetwin_1" || c == "mozillawindowclass";
}

BOOL CALLBACK CloseEnum(HWND hwnd, LPARAM lp)
{
    CloseNeedle* nd = (CloseNeedle*)lp;
    if (nd->closed)
        return TRUE; // одну цель за раз — предсказуемо (верхнюю)
    if (!IsWindowVisible(hwnd))
        return TRUE;
    char title[256];
    if (GetWindowTextA(hwnd, title, sizeof(title)) <= 0)
        return TRUE;
    if (lowerRu(title).find(nd->needle) == std::string::npos)
        return TRUE;
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (IsBrowserClass(cls)) {
        // браузер: закрываем только вкладку (Ctrl+W), а не всё окно
        if (::SetForegroundWindow(hwnd)) {
            Sleep(350);
            if (::GetForegroundWindow() == hwnd) {
                KeyCombo(VK_CONTROL, 'W');
                printf("actions: close tab in [%s]\n", title);
                fflush(stdout);
                nd->closed = true;
                return TRUE;
            }
        }
    }
    PostMessageA(hwnd, WM_CLOSE, 0, 0);
    nd->closed = true;
    return TRUE;
}

void MediaKey(WORD vk)
{
    INPUT in[2] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void KeyCombo(WORD mod, WORD vk)
{
    INPUT in[4] = {};
    for (int i = 0; i < 4; i++)
        in[i].type = INPUT_KEYBOARD;
    in[0].ki.wVk = mod;
    in[1].ki.wVk = vk;
    in[2].ki.wVk = vk;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.wVk = mod;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

// нижний регистр UTF-8 (ASCII + кириллица) — для имён и алиасов
std::string lowerRu(const std::string& s)
{
    std::string o;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            o += (char)tolower(c);
            i++;
        } else if (c == 0xD0 && i + 1 < s.size()) {
            unsigned char d = (unsigned char)s[i + 1];
            if (d >= 0x90 && d <= 0x9F) {
                o += (char)0xD0;
                o += (char)(d + 0x20);
            } else if (d >= 0xA0 && d <= 0xAF) {
                o += (char)0xD1;
                o += (char)(d - 0x20);
            } else if (d == 0x81) {
                o += (char)0xD1;
                o += (char)0x91;
            } else {
                o += (char)c;
                o += (char)d;
            }
            i += 2;
        } else {
            o += (char)c;
            i++;
        }
    }
    return o;
}

std::string TrimSp(const std::string& s)
{
    size_t a = 0;
    while (a < s.size() && (unsigned char)s[a] <= ' ')
        a++;
    size_t b = s.size();
    while (b > a && (unsigned char)s[b - 1] <= ' ')
        b--;
    return s.substr(a, b - a);
}

// папки рекурсивно (глубина <= 2, кап — чтобы не гулять по всему диску)
void CollectDirs(const std::string& dir, int depth, std::vector<std::string>& out)
{
    if (depth < 0 || out.size() > 2000)
        return;
    std::string mask = dir + "\\*";
    WIN32_FIND_DATAA ff = {};
    HANDLE h = FindFirstFileA(mask.c_str(), &ff);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (ff.cFileName[0] == '.')
            continue;
        if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::string full = dir + "\\" + ff.cFileName;
            out.push_back(full);
            CollectDirs(full, depth - 1, out);
        }
    } while (FindNextFileA(h, &ff));
    FindClose(h);
}

std::string g_provider;   // где последний раз включали музыку
bool g_musicOpened = false;
std::string g_lastVid; // videoId последнего включённого видео (SponsorBlock)
unsigned long long g_lastVidTick = 0;

} // namespace

std::string SvcSel(const std::string& cat)
{
    if (cat == "work")
        return config::Get("svc.work", "https://mail.google.com/");
    if (cat == "fun")
        return config::Get("svc.fun", "https://www.twitch.tv/");
    // страховка от старого выбора (ютуб/тикток когда-то лежали в музыке):
    // музыкальный дефолт — только музыкальные хосты
    std::string u = config::Get("svc.music", "https://open.spotify.com/");
    if (u.find("spotify") != std::string::npos || u.find("yandex") != std::string::npos ||
        u.find("soundcloud") != std::string::npos || u.find("piped") != std::string::npos)
        return u;
    return "https://open.spotify.com/";
}

void SetSvcSel(const std::string& cat, const std::string& id)
{
    if (cat != "music" && cat != "work" && cat != "fun")
        return;
    if (id.empty() || id.size() > 200)
        return;
    config::Set("svc." + cat, id);
}

bool screenshot(const char* path)
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0)
        return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!bmp) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    // забираем пиксели
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> px((size_t)w * h * 4);
    int rows = GetDIBits(mem, bmp, 0, h, px.data(), &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    if (rows != h)
        return false;
    // пишем BMP (BGRA как есть)
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f)
        return false;
    int rowPad = (4 - (w * 3) % 4) % 4;
    uint32_t imgSize = (uint32_t)((w * 3 + rowPad) * h);
    uint32_t fileSize = 54 + imgSize;
    uint8_t hdr[54] = {};
    hdr[0] = 'B';
    hdr[1] = 'M';
    memcpy(hdr + 2, &fileSize, 4);
    uint32_t off = 54;
    memcpy(hdr + 10, &off, 4);
    uint32_t hsz = 40;
    memcpy(hdr + 14, &hsz, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1;
    memcpy(hdr + 26, &planes, 2);
    uint16_t bpp = 24;
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &imgSize, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t pad[3] = {};
    for (int yy = h - 1; yy >= 0; yy--) {
        uint8_t* row = px.data() + (size_t)yy * w * 4;
        for (int xx = 0; xx < w; xx++)
            fwrite(row + xx * 4, 1, 3, f); // BGR, альфу роняем
        fwrite(pad, 1, rowPad, f);
    }
    fclose(f);
    return true;
}

bool openSite(const std::string& url)
{
    std::string u = url;
    if (u.find("://") == std::string::npos)
        u = "https://" + u;
    return (INT_PTR)ShellExecuteA(nullptr, "open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
}

bool openApp(const std::string& name)
{
    std::string needle = lowerRu(TrimSp(name));
    if (needle.empty())
        return false;
    char appdata[300], progdata[300];
    std::vector<std::string> roots;
    if (GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)) > 0)
        roots.push_back(std::string(appdata) + "\\Microsoft\\Windows\\Start Menu\\Programs");
    if (GetEnvironmentVariableA("ProgramData", progdata, sizeof(progdata)) > 0)
        roots.push_back(std::string(progdata) + "\\Microsoft\\Windows\\Start Menu\\Programs");
    for (auto& r : roots) {
        std::vector<std::string> lnks;
        CollectLnk(r, lnks);
        for (auto& l : lnks) {
            size_t slash = l.find_last_of("\\/");
            std::string base = (slash == std::string::npos) ? l : l.substr(slash + 1);
            if (EndsWith(lowerAscii(base), ".lnk"))
                base.resize(base.size() - 4);
            if (lowerRu(base).find(needle) != std::string::npos)
                return (INT_PTR)ShellExecuteA(nullptr, "open", l.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
        }
    }
    return false;
}

bool closeApp(const std::string& name)
{
    CloseNeedle nd;
    nd.needle = lowerRu(TrimSp(name));
    if (nd.needle.empty())
        return false;
    EnumWindows(CloseEnum, (LPARAM)&nd);
    return nd.closed;
}

void mediaPlayPause()
{
    MediaKey(VK_MEDIA_PLAY_PAUSE);
}

void mediaNext()
{
    MediaKey(VK_MEDIA_NEXT_TRACK);
}

void mediaPrev()
{
    MediaKey(VK_MEDIA_PREV_TRACK);
}

std::string UrlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~')
            o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string openFolder(const std::string& name)
{
    std::string n = lowerRu(TrimSp(name));
    if (n.empty())
        return "";
    auto tryOpen = [&](const std::string& p) -> std::string {
        DWORD a = GetFileAttributesA(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
            return ((INT_PTR)ShellExecuteA(nullptr, "open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32) ? p : "";
        return "";
    };
    char up[300] = {};
    GetEnvironmentVariableA("USERPROFILE", up, sizeof(up));
    std::string U = up;
    if (!U.empty()) {
        const char* keys[][2] = {
            { "рабочий стол", "\\Desktop" }, { "документы", "\\Documents" },
            { "загрузки", "\\Downloads" }, { "музыка", "\\Music" },
            { "картинки", "\\Pictures" }, { "фото", "\\Pictures" },
            { "видео", "\\Videos" },
        };
        for (auto& k : keys) {
            if (n == k[0]) {
                std::string p = tryOpen(U + k[1]);
                return p; // пусто если нет такой
            }
        }
        if (n == "диск с" || n == "диск ц" || n == "c" || n == "c:")
            return tryOpen("C:\\");
        if (n == "диск д" || n == "диск d" || n == "d" || n == "d:")
            return tryOpen("D:\\");
        // поиск по имени в пользовательских местах
        std::vector<std::string> roots = { U + "\\Desktop", U + "\\Documents", U + "\\Downloads", U };
        for (auto& r : roots) {
            std::vector<std::string> dirs;
            CollectDirs(r, 2, dirs);
            for (auto& d : dirs) {
                size_t slash = d.find_last_of("\\/");
                std::string base = (slash == std::string::npos) ? d : d.substr(slash + 1);
                if (lowerAscii(base).find(n) != std::string::npos) {
                    std::string p = tryOpen(d);
                    if (!p.empty())
                        return p;
                }
            }
        }
    }
    return "";
}

bool typeText(const std::string& text)
{
    if (text.empty() || text.size() > 4000)
        return false;
    int wlen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wlen <= 1)
        return false;
    std::vector<wchar_t> w(wlen);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), wlen);
    std::vector<INPUT> ins;
    ins.reserve((size_t)(wlen - 1) * 2);
    for (int i = 0; i < wlen - 1; i++) {
        INPUT d = {}, u = {};
        d.type = u.type = INPUT_KEYBOARD;
        if (w[i] == L'\n') {
            d.ki.wVk = u.ki.wVk = VK_RETURN;
            u.ki.dwFlags = KEYEVENTF_KEYUP;
        } else {
            d.ki.wScan = u.ki.wScan = w[i];
            d.ki.dwFlags = KEYEVENTF_UNICODE;
            u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        }
        ins.push_back(d);
        ins.push_back(u);
    }
    return SendInput((UINT)ins.size(), ins.data(), sizeof(INPUT)) == ins.size();
}

// YouTube: поиск -> первый videoId через Piped API (ключа не надо).
// Нужно чтобы «включи трек» реально включал, а не открывал выдачу.
// HTTPS-сессия одна на поток (без нового TLS-хендшейка каждый раз)
HINTERNET PipedSession()
{
    thread_local HINTERNET s = nullptr;
    if (!s) {
        s = WinHttpOpen(L"helper_bot", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
        if (s)
            WinHttpSetTimeouts(s, 5000, 5000, 5000, 15000);
    }
    return s;
}

std::string ResolveYouTube(const std::string& query)
{
    const char* hosts[] = { "pipedapi.kavin.rocks", "pipedapi.adminforge.de" };
    std::string path = "/search?q=" + UrlEncode(query) + "&filter=videos";
    for (auto host : hosts) {
        std::string id;
        HINTERNET ses = PipedSession();
        if (!ses)
            continue;
        int wl = MultiByteToWideChar(CP_UTF8, 0, host, -1, nullptr, 0);
        std::vector<wchar_t> wh(wl);
        MultiByteToWideChar(CP_UTF8, 0, host, -1, wh.data(), wl);
        int pl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wp(pl);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), pl);
        HINTERNET con = WinHttpConnect(ses, wh.data(), 443, 0);
        HINTERNET req = nullptr;
        if (con)
            req = WinHttpOpenRequest(con, L"GET", wp.data(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
        std::string resp;
        if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(req, nullptr)) {
            while (resp.size() < 2 * 1024 * 1024) {
                DWORD av = 0;
                if (!WinHttpQueryDataAvailable(req, &av) || av == 0)
                    break;
                size_t at = resp.size();
                resp.resize(at + av);
                DWORD rd = 0;
                if (!WinHttpReadData(req, &resp[at], av, &rd)) {
                    resp.resize(at);
                    break;
                }
                resp.resize(at + rd);
                if (rd == 0)
                    break;
            }
            // первый watch?v=XXXXXXXXXXX (11 символов)
            size_t p = resp.find("watch?v=");
            while (p != std::string::npos) {
                std::string cand = resp.substr(p + 8, 11);
                bool ok = cand.size() == 11;
                for (char c : cand) {
                    if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
                        !(c >= '0' && c <= '9') && c != '-' && c != '_')
                        ok = false;
                }
                if (ok) {
                    id = cand;
                    break;
                }
                p = resp.find("watch?v=", p + 1);
            }
        }
        if (req)
            WinHttpCloseHandle(req);
        if (con)
            WinHttpCloseHandle(con);
        if (!id.empty()) {
            printf("actions: yt resolve '%s' -> %s\n", query.c_str(), id.c_str());
            fflush(stdout);
            return id;
        }
    }
    return "";
}

void closeTab()
{
    KeyCombo(VK_CONTROL, 'W');
}

void EnsurePlaying();

bool musicSearch(const std::string& provider, const std::string& query)
{
    std::string pv = lowerAscii(TrimSp(provider));
    if (pv == "auto" || pv.empty()) {
        // выбор юзера — по URL понимаем провайдера
        std::string u = SvcSel("music");
        if (u.find("tiktok") != std::string::npos)
            pv = "tiktok";
        else if (u.find("spotify") != std::string::npos)
            pv = "spotify";
        else if (u.find("yandex") != std::string::npos)
            pv = "yandex";
        else if (u.find("soundcloud") != std::string::npos)
            pv = "soundcloud";
        else if (u.find("piped") != std::string::npos)
            pv = "piped";
        else
            pv = "youtube";
    }
    if (pv != "youtube" && pv != "tiktok" && pv != "spotify" && pv != "yandex" && pv != "soundcloud" && pv != "piped")
        pv = "youtube";
    std::string q = TrimSp(query);
    std::string url;
    if (pv == "tiktok")
        url = q.empty() ? "https://www.tiktok.com/" : "https://www.tiktok.com/search?q=" + UrlEncode(q);
    else if (pv == "spotify")
        url = q.empty() ? "https://open.spotify.com/" : "https://open.spotify.com/search/" + UrlEncode(q);
    else if (pv == "yandex")
        url = q.empty() ? "https://music.yandex.ru/" : "https://music.yandex.ru/search?text=" + UrlEncode(q);
    else if (pv == "soundcloud")
        url = q.empty() ? "https://soundcloud.com/" : "https://soundcloud.com/search?q=" + UrlEncode(q);
    else if (pv == "piped") {
        // Piped — прокси-плеер ютуба вообще без рекламы (прероллов нет в принципе)
        url = "https://piped.video/";
        if (!q.empty()) {
            std::string vid = ResolveYouTube(q);
            url = vid.empty() ? ("https://piped.video/search?q=" + UrlEncode(q))
                              : ("https://piped.video/watch?v=" + vid);
        }
    }
    else {
        url = "https://www.youtube.com/";
        if (!q.empty()) {
            // резолвим в прямое видео — watch-страница сама играет
            std::string vid = ResolveYouTube(q);
            url = vid.empty() ? ("https://www.youtube.com/results?search_query=" + UrlEncode(q))
                              : ("https://www.youtube.com/watch?v=" + vid);
        }
    }
    if (g_musicOpened) {
        closeTab(); // прошлый сайт закрываем, новый открываем
        Sleep(400);
    }
    // запоминаем videoId для SponsorBlock (только прямое видео)
    {
        std::string vid;
        size_t p = url.find("watch?v=");
        if (p != std::string::npos) {
            std::string c = url.substr(p + 8, 11);
            bool okid = c.size() == 11;
            for (char ch : c) {
                if (!(ch >= 'A' && ch <= 'Z') && !(ch >= 'a' && ch <= 'z') &&
                    !(ch >= '0' && ch <= '9') && ch != '-' && ch != '_')
                    okid = false;
            }
            if (okid)
                vid = c;
        }
        SetLastVideoId(vid);
    }
    bool ok = openSite(url);
    if (ok) {
        g_musicOpened = true;
        g_provider = pv;
        if (url.find("watch?v=") != std::string::npos)
            std::thread([]() { EnsurePlaying(); }).detach(); // фоном, UI не виснет
    }
    return ok;
}

void SetLastVideoId(const std::string& id)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    g_lastVid = id;
    g_lastVidTick = GetTickCount64();
}

bool GetLastVideoIdFresh(std::string& out, unsigned long long maxAgeMs)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (g_lastVid.empty())
        return false;
    if (GetTickCount64() - g_lastVidTick > maxAgeMs)
        return false;
    out = g_lastVid;
    return true;
}

// громкость конкретного плеера (НЕ мастера): ищем активную сессию нужного
// процесса и крутим её. Мастер-системную громкость не трогаем.
bool AdjustSessions(const char* exeList[], int nList, float delta, std::string& usedName)
{
    bool changed = false;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioSessionManager2* mgr = nullptr;
    IAudioSessionEnumerator* sen = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&en);
    if (SUCCEEDED(hr))
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    if (SUCCEEDED(hr))
        hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&mgr);
    if (SUCCEEDED(hr))
        hr = mgr->GetSessionEnumerator(&sen);
    int count = 0;
    if (SUCCEEDED(hr))
        hr = sen->GetCount(&count);
    for (int i = 0; SUCCEEDED(hr) && i < count; i++) {
        IAudioSessionControl* sc = nullptr;
        if (FAILED(sen->GetSession(i, &sc)) || !sc)
            continue;
        IAudioSessionControl2* c2 = nullptr;
        std::string exe;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IAudioSessionControl2), (void**)&c2)) && c2) {
            DWORD pid = 0;
            if (SUCCEEDED(c2->GetProcessId(&pid)) && pid) {
                HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (h) {
                    char p[MAX_PATH] = {};
                    DWORD sz = sizeof(p);
                    if (QueryFullProcessImageNameA(h, 0, p, &sz)) {
                        std::string full = p;
                        size_t s = full.find_last_of("\\/");
                        exe = (s == std::string::npos) ? full : full.substr(s + 1);
                    }
                    CloseHandle(h);
                }
            }
            c2->Release();
        }
        std::string le = lowerAscii(exe);
        bool want = false;
        for (int k = 0; k < nList; k++) {
            if (le == exeList[k]) {
                want = true;
                break;
            }
        }
        if (want) {
            ISimpleAudioVolume* vol = nullptr;
            if (SUCCEEDED(sc->QueryInterface(__uuidof(ISimpleAudioVolume), (void**)&vol)) && vol) {
                float cur = 0;
                if (SUCCEEDED(vol->GetMasterVolume(&cur))) {
                    float nv = cur + delta;
                    if (nv < 0.0f)
                        nv = 0.0f;
                    if (nv > 1.0f)
                        nv = 1.0f;
                    if (SUCCEEDED(vol->SetMasterVolume(nv, nullptr))) {
                        changed = true;
                        usedName = exe;
                    }
                }
                vol->Release();
            }
        }
        sc->Release();
    }
    if (sen)
        sen->Release();
    if (mgr)
        mgr->Release();
    if (dev)
        dev->Release();
    if (en)
        en->Release();
    CoUninitialize();
    return changed;
}

// +1/-1: сначала Spotify.exe, иначе браузер с активной сессией.
// Возвращает чем крутили ("Spotify"/"браузер") или "".
std::string PlayerVol(int dir)
{
    float d = dir > 0 ? 0.1f : -0.1f;
    std::string used;
    const char* spot[] = { "spotify.exe" };
    if (AdjustSessions(spot, 1, d, used))
        return "Spotify";
    const char* br[] = { "chrome.exe", "msedge.exe", "firefox.exe", "opera.exe",
                         "brave.exe", "vivaldi.exe", "yandex.exe" };
    if (AdjustSessions(br, 7, d, used))
        return "браузер";
    // внутриигровой звук (медиа-клавиши его не берут — крутим сессию игры)
    const char* gm[] = { "dota2.exe", "cs2.exe" };
    if (AdjustSessions(gm, 2, d, used))
        return "игра";
    return "";
}

bool openDota(bool clean)
{
    if (clean)
        return (INT_PTR)ShellExecuteA(nullptr, "open", "steam://rungameid/570",
                                      nullptr, nullptr, SW_SHOWNORMAL) > 32;
    const char* exe = "C:\\Users\\mirik\\Desktop\\ambrel\\UmbrellaLoader.exe";
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES)
        return false;
    return (INT_PTR)ShellExecuteA(nullptr, "open", exe, nullptr,
                                  "C:\\Users\\mirik\\Desktop\\ambrel", SW_SHOWNORMAL) > 32;
}

// UIA-кнопка Play в окнах браузеров (плеер ютуба/Piped): работает и в фоне,
// без координат. Возвращает true если нажалось.
bool PressPlayButton()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool clicked = false;
    IUIAutomation* aut = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&aut));
    const wchar_t* names[] = { L"Play", L"Воспроизвести", L"Смотреть" };
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(hr) && aut) {
        VARIANT vt;
        VariantInit(&vt);
        vt.vt = VT_I4;
        vt.lVal = UIA_ButtonControlTypeId;
        IUIAutomationCondition* cType = nullptr;
        IUIAutomationCondition* cOr = nullptr;
        if (SUCCEEDED(aut->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &cType)) && cType) {
            for (auto nm : names) {
                VARIANT vn;
                VariantInit(&vn);
                vn.vt = VT_BSTR;
                vn.bstrVal = SysAllocString(nm);
                IUIAutomationCondition* cN = nullptr;
                if (SUCCEEDED(aut->CreatePropertyCondition(UIA_NamePropertyId, vn, &cN)) && cN) {
                    if (!cOr) {
                        cOr = cN;
                    } else {
                        IUIAutomationCondition* nxt = nullptr;
                        if (SUCCEEDED(aut->CreateOrCondition(cOr, cN, &nxt))) {
                            cOr->Release();
                            cN->Release();
                            cOr = nxt;
                        } else {
                            cN->Release();
                        }
                    }
                }
                VariantClear(&vn);
            }
            if (cOr)
                aut->CreateAndCondition(cType, cOr, &cond);
        }
        if (cType)
            cType->Release();
        if (cOr)
            cOr->Release();
    }
    if (cond) {
        struct WL {
            HWND v[8];
            int n = 0;
        };
        WL wl;
        EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
            WL* w = (WL*)lp;
            if (w->n >= 8)
                return FALSE;
            if (!IsWindowVisible(hwnd))
                return TRUE;
            char cls[64] = {};
            GetClassNameA(hwnd, cls, sizeof(cls));
            std::string c = cls;
            for (char& ch : c) {
                if (ch >= 'A' && ch <= 'Z')
                    ch += 32;
            }
            if (c == "chrome_widgetwin_1" || c == "mozillawindowclass")
                w->v[w->n++] = hwnd;
            return TRUE;
        }, (LPARAM)&wl);
        for (int i = 0; i < wl.n && !clicked; i++) {
            IUIAutomationElement* root = nullptr;
            if (FAILED(aut->ElementFromHandle(wl.v[i], &root)) || !root)
                continue;
            IUIAutomationElementArray* all = nullptr;
            if (SUCCEEDED(root->FindAll(TreeScope_Descendants, cond, &all)) && all) {
                int n = 0;
                all->get_Length(&n);
                RECT wr = {};
                GetWindowRect(wl.v[i], &wr);
                for (int j = 0; j < n && !clicked; j++) {
                    IUIAutomationElement* el = nullptr;
                    if (FAILED(all->GetElement(j, &el)) || !el)
                        continue;
                    // кнопка плеера — слева-снизу (шапочный мусор режем)
                    bool posOk = false;
                    RECT br = {};
                    if (SUCCEEDED(el->get_CurrentBoundingRectangle(&br))) {
                        double cx = (br.left + br.right) * 0.5;
                        double cy = (br.top + br.bottom) * 0.5;
                        double ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                        posOk = ww > 0 && wh > 0 &&
                                cx < wr.left + ww * 0.6 && cy > wr.top + wh * 0.3;
                    }
                    BOOL en = FALSE;
                    el->get_CurrentIsEnabled(&en);
                    if (posOk && en) {
                        IUIAutomationInvokePattern* inv = nullptr;
                        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv) {
                            if (SUCCEEDED(inv->Invoke())) {
                                printf("actions: play pressed\n");
                                fflush(stdout);
                                clicked = true;
                            }
                            inv->Release();
                        }
                    }
                    el->Release();
                }
                all->Release();
            }
            root->Release();
        }
        cond->Release();
    }
    if (aut)
        aut->Release();
    CoUninitialize();
    return clicked;
}

// после открытия watch-страницы: 1) сама? 2) K в активное окно 3) UIA-Play.
// K, а не Space: пробел мог бы кликнуть сфокусированный элемент.
void EnsurePlaying()
{
    Sleep(3500);
    media::Info mi;
    media::Snapshot(mi);
    if (mi.has && mi.playing)
        return;
    // 1. K в активное окно браузера
    HWND fg = GetForegroundWindow();
    bool fgBrowser = false;
    if (fg) {
        char cls[64] = {};
        GetClassNameA(fg, cls, sizeof(cls));
        std::string c = lowerAscii(cls);
        fgBrowser = (c == "chrome_widgetwin_1" || c == "mozillawindowclass");
    }
    if (fgBrowser) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = 'K';
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = 'K';
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(1500);
        media::Snapshot(mi);
        if (mi.has && mi.playing)
            return;
    }
    // 2. UIA-кнопка Play (работает и в фоне)
    if (PressPlayButton()) {
        Sleep(1500);
        media::Snapshot(mi);
        if (mi.has && mi.playing) {
            printf("actions: playing via play button\n");
            fflush(stdout);
            return;
        }
    }
    // 3. Глобальная медиа-клавиша: для заблокированного автовоспроизведения
    // она считается user activation и стартует видео. Только если вообще
    // ничего не играет — иначе переключим чужое (например, спотифай на паузе).
    if (!mi.has) {
        MediaKey(VK_MEDIA_PLAY_PAUSE);
        Sleep(1500);
        media::Snapshot(mi);
        if (mi.has && mi.playing) {
            printf("actions: playing via media key\n");
            fflush(stdout);
            return;
        }
    }
    printf("actions: autoplay failed\n");
    fflush(stdout);
}

bool mediaLike()
{
    // L = лайк в TikTok web. На ютубе у L другое действие (перемотка) — не шлём.
    if (g_provider == "tiktok") {
        MediaKey('L');
        return true;
    }
    return false;
}

} // namespace actions
