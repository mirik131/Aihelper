#include "gui/adskip.h"
#include "gui/media.h"
#include "gui/actions.h"
#include <windows.h>
#include <winhttp.h>
#include <UIAutomation.h>
#include <cstdio>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cmath>

namespace adskip {
namespace {

std::thread g_thr;
std::atomic<bool> g_stop{ false };
std::atomic<bool> g_on{ false };
// кэш сегментов текущего видео
std::string g_segVid;
std::vector<std::pair<double, double>> g_segs;
unsigned long long g_lastSeekMs = 0;

struct WinList {
    HWND v[8];
    int n = 0;
};

BOOL CALLBACK EnumBrowsers(HWND hwnd, LPARAM lp)
{
    WinList* wl = (WinList*)lp;
    if (wl->n >= 8)
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
    // Chromium (Chrome/Edge/Brave/Opera/Яндекс) + Firefox
    if (c == "chrome_widgetwin_1" || c == "mozillawindowclass")
        wl->v[wl->n++] = hwnd;
    return TRUE;
}

// точные имена кнопки скипа (RU+EN) — UIA умеет только exact-match
const wchar_t* kNames[] = {
    L"Skip Ad", L"Skip Ads", L"Пропустить рекламу", L"Пропустить", L"Пропустить объявление",
};

bool ClickSkip(HWND hwnd, IUIAutomation* aut, IUIAutomationCondition* cond)
{
    IUIAutomationElement* root = nullptr;
    if (FAILED(aut->ElementFromHandle(hwnd, &root)) || !root)
        return false;
    IUIAutomationElementArray* all = nullptr;
    HRESULT hr = root->FindAll(TreeScope_Descendants, cond, &all);
    root->Release();
    if (FAILED(hr) || !all)
        return false;
    int n = 0;
    all->get_Length(&n);
    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    bool clicked = false;
    for (int i = 0; i < n && !clicked; i++) {
        IUIAutomationElement* el = nullptr;
        if (FAILED(all->GetElement(i, &el)) || !el)
            continue;
        // позиция: скип живёт справа-снизу плеера, шапочные «Пропустить» режем
        bool posOk = false;
        RECT br = {};
        if (SUCCEEDED(el->get_CurrentBoundingRectangle(&br))) {
            double cx = (br.left + br.right) * 0.5;
            double cy = (br.top + br.bottom) * 0.5;
            double ww = wr.right - wr.left, wh = wr.bottom - wr.top;
            posOk = ww > 0 && wh > 0 &&
                    cx > wr.left + ww * 0.5 && cy > wr.top + wh * 0.4;
        }
        BSTR nm = nullptr;
        char b[128] = {};
        if (SUCCEEDED(el->get_CurrentName(&nm)) && nm) {
            WideCharToMultiByte(CP_UTF8, 0, nm, -1, b, sizeof(b), nullptr, nullptr);
            SysFreeString(nm);
        }
        BOOL enabled = FALSE;
        el->get_CurrentIsEnabled(&enabled);
        printf("adskip: candidate [%s] pos=%d en=%d\n", b, posOk ? 1 : 0, enabled ? 1 : 0);
        fflush(stdout);
        if (posOk && enabled) {
            IUIAutomationInvokePattern* inv = nullptr;
            if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv) {
                if (SUCCEEDED(inv->Invoke())) {
                    printf("adskip: clicked\n");
                    fflush(stdout);
                    clicked = true;
                }
                inv->Release();
            }
        }
        el->Release();
    }
    all->Release();
    return clicked;
}

void SponsorTick(IUIAutomation* aut);

void Worker()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* aut = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&aut));
    // общее условие: Button И (имя из списка)
    IUIAutomationCondition* cond = nullptr;
    if (SUCCEEDED(hr) && aut) {
        VARIANT vt;
        VariantInit(&vt);
        vt.vt = VT_I4;
        vt.lVal = UIA_ButtonControlTypeId;
        IUIAutomationCondition* cType = nullptr;
        hr = aut->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &cType);
        IUIAutomationCondition* cOr = nullptr;
        for (auto nm : kNames) {
            VARIANT vn;
            VariantInit(&vn);
            vn.vt = VT_BSTR;
            vn.bstrVal = SysAllocString(nm);
            IUIAutomationCondition* cN = nullptr;
            if (SUCCEEDED(aut->CreatePropertyCondition(UIA_NamePropertyId, vn, &cN))) {
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
        if (SUCCEEDED(hr) && cType && cOr)
            hr = aut->CreateAndCondition(cType, cOr, &cond);
        if (cType)
            cType->Release();
        if (cOr)
            cOr->Release();
    }
    if (FAILED(hr) || !cond)
        printf("adskip: uia init fail\n");
    while (!g_stop) {
        if (!g_on.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            continue;
        }
        bool hit = false;
        if (cond) {
            WinList wl;
            EnumWindows(EnumBrowsers, (LPARAM)&wl);
            for (int i = 0; i < wl.n && !hit; i++)
                hit = ClickSkip(wl.v[i], aut, cond);
        }
        SponsorTick(aut);
        std::this_thread::sleep_for(std::chrono::milliseconds(hit ? 2500 : 1000));
    }
    if (cond)
        cond->Release();
    if (aut)
        aut->Release();
    CoUninitialize();
}

void Ensure()
{
    if (!g_thr.joinable()) {
        g_stop = false;
        g_thr = std::thread(Worker);
    }
}

// foreground — браузер? (фокус не трогаем, чужие окна не дёргаем)
bool IsBrowserFg(HWND fg)
{
    if (!fg)
        return false;
    char cls[64] = {};
    GetClassNameA(fg, cls, sizeof(cls));
    std::string c = cls;
    for (char& ch : c) {
        if (ch >= 'A' && ch <= 'Z')
            ch += 32;
    }
    return c == "chrome_widgetwin_1" || c == "mozillawindowclass";
}

// videoId из URL (watch?v=, youtu.be/, shorts/live/embed)
std::string ExtractVideoId(const std::string& url)
{
    auto take11 = [&](size_t p) -> std::string {
        if (p == std::string::npos || p + 11 > url.size())
            return "";
        std::string c = url.substr(p, 11);
        for (char ch : c) {
            if (!(ch >= 'A' && ch <= 'Z') && !(ch >= 'a' && ch <= 'z') &&
                !(ch >= '0' && ch <= '9') && ch != '-' && ch != '_')
                return "";
        }
        return c;
    };
    const char* marks[] = { "watch?v=", "youtu.be/", "/shorts/", "/live/", "/embed/" };
    for (auto m : marks) {
        size_t p = url.find(m);
        if (p != std::string::npos) {
            std::string id = take11(p + strlen(m));
            if (!id.empty())
                return id;
        }
    }
    return "";
}

// URL из адресной строки foreground-браузера (Chromium стабильно отдаёт)
std::string BrowserUrl(HWND hwnd, IUIAutomation* aut)
{
    std::string out;
    if (!hwnd || !aut)
        return out;
    IUIAutomationElement* root = nullptr;
    if (FAILED(aut->ElementFromHandle(hwnd, &root)) || !root)
        return out;
    const wchar_t* barNames[] = { L"Address and search bar", L"Address bar", L"Search or enter address" };
    for (auto bn : barNames) {
        VARIANT vt;
        VariantInit(&vt);
        vt.vt = VT_I4;
        vt.lVal = UIA_EditControlTypeId;
        VARIANT vb;
        VariantInit(&vb);
        vb.vt = VT_BSTR;
        vb.bstrVal = SysAllocString(bn);
        IUIAutomationCondition *cT = nullptr, *cN = nullptr, *cAnd = nullptr;
        IUIAutomationElement* bar = nullptr;
        if (SUCCEEDED(aut->CreatePropertyCondition(UIA_ControlTypePropertyId, vt, &cT)) && cT &&
            SUCCEEDED(aut->CreatePropertyCondition(UIA_NamePropertyId, vb, &cN)) && cN &&
            SUCCEEDED(aut->CreateAndCondition(cT, cN, &cAnd)) && cAnd) {
            root->FindFirst(TreeScope_Descendants, cAnd, &bar);
        }
        if (cAnd)
            cAnd->Release();
        if (cN)
            cN->Release();
        if (cT)
            cT->Release();
        VariantClear(&vb);
        if (bar) {
            IUIAutomationValuePattern* val = nullptr;
            if (SUCCEEDED(bar->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&val))) && val) {
                BSTR u = nullptr;
                if (SUCCEEDED(val->get_CurrentValue(&u)) && u) {
                    char b[2048] = {};
                    WideCharToMultiByte(CP_UTF8, 0, u, -1, b, sizeof(b), nullptr, nullptr);
                    out = b;
                    SysFreeString(u);
                }
                val->Release();
            }
            bar->Release();
            if (!out.empty())
                break;
        }
    }
    root->Release();
    return out;
}

// сегменты видео с базы SponsorBlock (404 = нет — нормально)
void FetchSegments(const std::string& vid)
{
    g_segs.clear();
    std::string path = "/api/skipSegments?videoID=" + vid +
        "&categories=%5B%22sponsor%22,%22selfpromo%22,%22intro%22,%22outro%22,%22interaction_reminder%22,%22preview%22%5D";
    HINTERNET ses = WinHttpOpen(L"helper_bot", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses)
        return;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, 12000);
    HINTERNET con = WinHttpConnect(ses, L"sponsor.ajay.app", 443, 0);
    HINTERNET req = nullptr;
    if (con) {
        int pl = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::vector<wchar_t> wp(pl);
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wp.data(), pl);
        req = WinHttpOpenRequest(con, L"GET", wp.data(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    }
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD code = 0, csz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            NULL, &code, &csz, NULL);
        if (code == 200) {
            std::string resp;
            while (resp.size() < 512 * 1024) {
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
            // тянем пары "segment":[start,end]
            size_t p = resp.find("\"segment\"");
            while (p != std::string::npos) {
                size_t b = resp.find('[', p);
                if (b == std::string::npos)
                    break;
                const char* s1 = resp.c_str() + b + 1;
                char* e1 = nullptr;
                double t0 = strtod(s1, &e1);
                double t1 = 0;
                if (e1 && *e1 == ',')
                    t1 = strtod(e1 + 1, nullptr);
                if (t1 > t0 && t0 >= 0 && t1 - t0 < 3600)
                    g_segs.emplace_back(t0, t1);
                p = resp.find("\"segment\"", b + 1);
            }
        }
    }
    if (req)
        WinHttpCloseHandle(req);
    if (con)
        WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    printf("adskip: sponsor segs=%d for %s\n", (int)g_segs.size(), vid.c_str());
    fflush(stdout);
}

void PressL(int n)
{
    for (int i = 0; i < n && i < 8; i++) {
        INPUT in[2] = {};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wVk = 'L';
        in[1].type = INPUT_KEYBOARD;
        in[1].ki.wVk = 'L';
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        Sleep(80);
    }
}

// тик интеграций: позиция из SMTC, мотаем L если внутри сегмента
void SponsorTick(IUIAutomation* aut)
{
    HWND fg = GetForegroundWindow();
    if (!IsBrowserFg(fg))
        return; // фон не трогаем — фокус святой
    std::string vid;
    std::string url = BrowserUrl(fg, aut);
    if (!url.empty())
        vid = ExtractVideoId(url);
    if (vid.empty() && !actions::GetLastVideoIdFresh(vid, 10 * 60 * 1000))
        return;
    if (vid.empty())
        return;
    if (vid != g_segVid) {
        FetchSegments(vid);
        g_segVid = vid;
    }
    if (g_segs.empty())
        return;
    unsigned long long now = GetTickCount64();
    if (now - g_lastSeekMs < 4000)
        return; // кулдаун: позиция SMTC догоняет с лагом
    media::Info mi;
    media::Snapshot(mi);
    if (!mi.has || !mi.playing || mi.dur <= 0 || mi.dur > 10800)
        return;
    double age = (double)(now - mi.tickMs) / 1000.0;
    double pos = mi.pos + age;
    if (pos < 0)
        pos = 0;
    if (pos > mi.dur)
        pos = mi.dur;
    for (auto& sg : g_segs) {
        if (pos >= sg.first - 0.5 && pos < sg.second) {
            int presses = (int)ceil((sg.second - pos) / 10.0) + 1;
            if (presses < 1)
                presses = 1;
            PressL(presses);
            g_lastSeekMs = now;
            printf("adskip: sponsor seek %.0f->%.0f (%dxL)\n", pos, sg.second, presses);
            fflush(stdout);
            break;
        }
    }
}

} // namespace

void SetEnabled(bool on)
{
    g_on = on;
    Ensure();
}

bool IsEnabled()
{
    return g_on.load();
}

void Shutdown()
{
    g_stop = true;
    if (g_thr.joinable())
        g_thr.join();
}

} // namespace adskip
