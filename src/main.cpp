// Чёрный квадрат. Больше ничего.
// Рисование потом переедет в gui::Render().

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "gui/gui.h"
#include "gui/blurbg.h"
#include "gui/select.h"
#include "gui/photo.h"
#include "gui/user.h"
#include "gui/mic.h"
#include "gui/stt.h"
#include "gui/llm.h"
#include "gui/media.h"
#include "gui/svg.h"
#include "gui/ears.h"
#include "gui/adskip.h"
#include "gui/config.h"
#include "gui/island.h"
#include "gui/textfade.h"
#include <d3d11.h>
#include <tchar.h>
#include <windows.h>
#include <dwmapi.h>
#include <cstdio>

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapchain = nullptr;
static ID3D11RenderTargetView* g_target = nullptr;
static BlurBG g_blur;

bool CreateDevice(HWND hwnd, int w, int h);
void CleanupDevice();
void CreateTarget();
void CleanupTarget();
void EnableBlur(HWND hwnd);
LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// Подложку от винды гасим: блюр рисуем сами (BlurBG) из захваченного кадра —
// никаких квадратов от композита, углы просто пустые.
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
void EnableBlur(HWND hwnd)
{
    // Без ExtendFrame: он давал стеклянную светлую окантовку по периметру.
    // Прозрачность держит вызов из бэкенда ImGui, этого хватает.
    int none = DWMSBT_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof(none));

    // Рамку винды в чёрный: на тёмном она светится белой линией по периметру.
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
    COLORREF border = RGB(0, 0, 0);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
}

int main(int, char**)
{
    // окно 625x495 по центру экрана, без рамки
    const int W = 625, H = 495;
    const int sx = GetSystemMetrics(SM_CXSCREEN);
    const int sy = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"HelperBot", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Helper",
        WS_POPUP | WS_VISIBLE, (sx - W) / 2, (sy - H) / 2, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);

    // F12 видно отовсюду: скрины/демка работают без фокуса на окне
    if (!::RegisterHotKey(hwnd, 1, 0, VK_F12))
        printf("hotkey F12 busy\n");

    if (!CreateDevice(hwnd, W, H)) {
        CleanupDevice();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    EnableBlur(hwnd); // подложку гасим, блюр рисуем сами

    config::Init(); // C:\FixAi — тоглы, выборы, настройки

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.IniFilename = nullptr;

    // Шрифт по умолчанию на весь гуи: SF Pro Medium покрупнее чтобы читалось.
    // Первый загруженный и есть дефолт — им весь текст.
    const char* fontPaths[] = {
        "Icon/fonts/medium.otf",        // запуск из студии (F5)
        "../../Icon/fonts/medium.otf",  // запуск exe из x64/Debug
        "c:\\Windows\\Fonts\\segoeui.ttf",
    };
    const char* fontFile = nullptr;
    for (const char* p : fontPaths) {
        FILE* f = nullptr;
        if (fopen_s(&f, p, "rb") != 0 || !f)
            continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(p, 17.0f, nullptr, io.Fonts->GetGlyphRangesCyrillic())) {
            printf("font: %s\n", p);
            fontFile = p;
            break;
        }
    }
    // мелкий шрифт для компактных мест (меню настроек): второй в списке — gui берёт Fonts[1]
    if (fontFile && io.Fonts->AddFontFromFileTTF(fontFile, 13.5f, nullptr, io.Fonts->GetGlyphRangesCyrillic()))
        printf("font small: %s\n", fontFile);

    // чёрная панель на 80% + скруглённые углы (имгуи сглаживает их сам)
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 16.5f;
    style.WindowBorderSize = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0.75f);

    ImGui_ImplWin32_Init(hwnd);
    // прозрачность окна чтобы углы за панелью просвечивали на рабочий стол
    ImGui_ImplWin32_EnableAlphaCompositing(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);
    textfade::Init(g_device);
    g_blur.Init(g_device, hwnd); // не завёлся — фон будет плоским, окно всё равно встанет
    selglow::Init(g_device);
    photo::Init(g_device);
    svg::Init(g_device);
    media::Init(g_device);
    user::Init(g_device);

    bool done = false;
    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        gui::Render((ImTextureID)g_blur.Update(g_target));

        // Таскание окна по кадрам, а не по сообщениям. Кнопка читается напрямую
        // (GetAsyncKeyState) — переживает даже съеденные UP/MOVE (дребезг мыши,
        // клик-фиксы): фронт нажатия мимо виджетов — взводим, сдвиг >4px — едем
        // через SetWindowPos. Модальный HTCAPTION не используем: он стопает цикл.
        // Границы: таскаем ТОЛЬКО за верхнюю полосу (как тайтлбар), а не за
        // любую точку — иначе окно едет от случайных кликов по чату/фону.
        {
            static bool armed = false;
            static bool dragging = false;
            static bool prevDown = true;
            static POINT press = {};
            static POINT grab = {};
            const int kDragH = 36; // высота зоны таскания сверху (крути тут)
            bool down = (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            if (down && !prevDown && !ImGui::IsAnyItemHovered()) {
                ::GetCursorPos(&press);
                POINT lc = press;
                ::ScreenToClient(hwnd, &lc);
                if (lc.y >= 0 && lc.y < kDragH) {
                    RECT r;
                    ::GetWindowRect(hwnd, &r);
                    grab.x = press.x - r.left;
                    grab.y = press.y - r.top;
                    armed = true;
                }
            }
            if (!down) {
                armed = false;
                dragging = false;
            }
            if (armed && !dragging && down) {
                POINT p;
                ::GetCursorPos(&p);
                if (abs(p.x - press.x) + abs(p.y - press.y) > 4) {
                    dragging = true;
                    printf("drag start\n");
                    fflush(stdout);
                }
            }
            if (dragging && down) {
                POINT p;
                ::GetCursorPos(&p);
                // кламп: окно нельзя утащить за экран и потерять
                int nx = p.x - grab.x, ny = p.y - grab.y;
                int sw = ::GetSystemMetrics(SM_CXSCREEN);
                int sh = ::GetSystemMetrics(SM_CYSCREEN);
                RECT wr;
                ::GetWindowRect(hwnd, &wr);
                int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
                if (nx < -(ww - 100))
                    nx = -(ww - 100);
                if (nx > sw - 100)
                    nx = sw - 100;
                if (ny < 0)
                    ny = 0;
                if (ny > sh - 60)
                    ny = sh - 60;
                ::SetWindowPos(hwnd, nullptr, nx, ny, 0, 0,
                    SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            prevDown = down;
        }

        ImGui::Render();
        const float clear[4] = { 0, 0, 0, 0 }; // прозрачный фон, видна только панель
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_swapchain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    textfade::Shutdown();
    g_blur.Shutdown();
    selglow::Shutdown();
    photo::Shutdown();
    user::Shutdown();
    ears::Shutdown();
    adskip::Shutdown();
    config::Shutdown();
    mic::Shutdown();
    stt::Shutdown();
    llm::Shutdown();
    svg::Shutdown();
    media::Shutdown();
    CleanupDevice();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

bool CreateDevice(HWND hwnd, int w, int h)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = w;
    sd.BufferDesc.Height = h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got = {};
    // BGRA-флаг обязателен для Direct2D-интеропа (SVG-иконки через svg.*)
    const UINT devFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr_hw = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, devFlags, levels, 2, D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got, &g_context);
    if (hr_hw == S_OK) {
        CreateTarget();
        return true;
    }
    HRESULT hr_warp = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, devFlags, levels, 2, D3D11_SDK_VERSION, &sd, &g_swapchain, &g_device, &got, &g_context);
    if (hr_warp == S_OK) {
        CreateTarget();
        return true;
    }

    // обе попытки упали — показываем коды, иначе гадать можно вечно
    char msg[160];
    sprintf_s(msg, "D3D11 не создалось:\nvideo = 0x%08X\nwarp = 0x%08X\n\nСкинь это окошко мне.",
        (unsigned)hr_hw, (unsigned)hr_warp);
    MessageBoxA(hwnd, msg, "helper_bot", MB_OK | MB_ICONERROR);
    return false;
}

void CleanupDevice()
{
    CleanupTarget();
    if (g_swapchain) { g_swapchain->Release(); g_swapchain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

void CreateTarget()
{
    ID3D11Texture2D* back = nullptr;
    g_swapchain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (!back)
        return;
    g_device->CreateRenderTargetView(back, nullptr, &g_target);
    back->Release();
}

void CleanupTarget()
{
    if (g_target) { g_target->Release(); g_target = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

// F12: окно видно в захватах и наоборот (для скринов/демки).
static void ToggleShot(HWND hwnd)
{
    static bool shot = false;
    shot = !shot;
    ::SetWindowDisplayAffinity(hwnd, shot ? WDA_NONE : WDA_EXCLUDEFROMCAPTURE);
    printf("screenshot mode: %s (F12 чтобы вернуть)\n", shot ? "ON" : "OFF");
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // Драг переехал в главный цикл (по кадрам): сообщениям мыши тут больше
    // делать нечего — клики/ховеры целиком на имгуи-бэкенде.
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
        return true;

    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_F12) {
            ToggleShot(hwnd);
            return 0;
        }
        break;
    case WM_HOTKEY:
        if (wp == 1) {
            ToggleShot(hwnd);
            return 0;
        }
        break;
    case WM_DESTROY:
        ::UnregisterHotKey(hwnd, 1);
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wp, lp);
}
