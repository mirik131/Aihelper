#include "gui/gui.h"
#include "gui/anim.h"
#include "gui/widgets.h"
#include "gui/select.h"
#include "gui/photo.h"
#include "gui/user.h"
#include "gui/brain.h"
#include "gui/actions.h"
#include "gui/mic.h"
#include "gui/stt.h"
#include "gui/llm.h"
#include "gui/island.h"
#include "gui/ears.h"
#include "gui/config.h"
#include "gui/textfade.h"
#include "gui/svg.h"
#include "gui/helpers.h"
#include "imgui.h"
#include <cmath>
#include <vector>

// Слои во всё окно: блюр, светлый, чёрный (в сумме серое стекло).
// Дальше сюда встанет содержимое: шапка, кнопки, анимации.

static void Squircle(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col)
{
    const int N = 64;
    const float n = gui::kSquircleN;
    const float PI = 3.14159265f;
    ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    ImVec2 r((max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f);

    dl->PathClear();
    for (int i = 0; i <= N; i++) {
        float t = (float)i / N * PI * 2.0f;
        float ct = cosf(t), st = sinf(t);
        float x = c.x + r.x * (ct >= 0 ? 1.0f : -1.0f) * powf(fabsf(ct), 2.0f / n);
        float y = c.y + r.y * (st >= 0 ? 1.0f : -1.0f) * powf(fabsf(st), 2.0f / n);
        dl->PathLineTo(ImVec2(x, y));
    }
    dl->PathFillConvex(col);
}

// Тот же сквиркл, но обводка (для карточек — та же кривизна что у главной панели)
static void SquircleStroke(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col, float thick = 1.0f)
{
    const int N = 64;
    const float n = gui::kSquircleN;
    const float PI = 3.14159265f;
    ImVec2 c((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    ImVec2 r((max.x - min.x) * 0.5f, (max.y - min.y) * 0.5f);

    dl->PathClear();
    for (int i = 0; i <= N; i++) {
        float t = (float)i / N * PI * 2.0f;
        float ct = cosf(t), st = sinf(t);
        float x = c.x + r.x * (ct >= 0 ? 1.0f : -1.0f) * powf(fabsf(ct), 2.0f / n);
        float y = c.y + r.y * (st >= 0 ? 1.0f : -1.0f) * powf(fabsf(st), 2.0f / n);
        dl->PathLineTo(ImVec2(x, y));
    }
    dl->PathStroke(col, ImDrawFlags_Closed, thick);
}
// как у фона. Рёбра прямые, стыки по касательной — без заломов.
static void PanelMixed(ImDrawList* dl, ImVec2 min, ImVec2 max, ImU32 col)
{
    const float r = 14.0f; // обычный радиус слева
    const float n = gui::kSquircleN;
    const float PI = 3.14159265f;
    float x0 = min.x, y0 = min.y, x1 = max.x, y1 = max.y;
    float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
    float rx = (x1 - x0) * 0.5f, ry = (y1 - y0) * 0.5f;

    dl->PathClear();
    dl->PathLineTo(ImVec2(x0 + r, y0));
    dl->PathLineTo(ImVec2(cx, y0));
    // правая половина сквиркла: верх (-90°) -> право (0°) -> низ (+90°)
    const int N = 40;
    for (int i = 0; i <= N; i++) {
        float t = (-90.0f + 180.0f * i / N) * PI / 180.0f;
        float ct = cosf(t), st = sinf(t);
        float x = cx + rx * (ct >= 0 ? 1.0f : -1.0f) * powf(fabsf(ct), 2.0f / n);
        float y = cy + ry * (st >= 0 ? 1.0f : -1.0f) * powf(fabsf(st), 2.0f / n);
        dl->PathLineTo(ImVec2(x, y));
    }
    dl->PathLineTo(ImVec2(x0 + r, y1));
    dl->PathArcTo(ImVec2(x0 + r, y1 - r), r, PI * 0.5f, PI); // низ-лево
    dl->PathArcTo(ImVec2(x0 + r, y0 + r), r, PI, PI * 1.5f); // верх-лево
    dl->PathFillConvex(col);
}

// режем строку под ширину по кодпоинтам (UTF-8 не ломаем), в конце "..."
static std::string FitText(const std::string& s, float maxW, bool* cut = nullptr)
{
    if (cut)
        *cut = false;
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW)
        return s;
    std::string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = (unsigned char)s[i];
        size_t len = 1;
        if ((c & 0x80) == 0)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        if (ImGui::CalcTextSize((out + s.substr(i, len) + "...").c_str()).x > maxW)
            break;
        out += s.substr(i, len);
        i += len;
    }
    if (cut)
        *cut = true;
    return out + "...";
}

static float Smooth01(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v * v * (3.0f - 2.0f * v);
}

// Векторная шестерёнка (WIC не декодирует SVG, а файл может и отсутствовать —
// рисуем всегда сами, один кодпас). Силуэт как у настоящей: 8 зубьев + обод +
// отверстие без заливки. Цвет спокойный серый, не белый.
static void DrawSettingsGear(ImDrawList* dl, ImVec2 c, float scale, int alpha)
{
    ImU32 col = IM_COL32(200, 202, 210, alpha);
    for (int i = 0; i < 8; i++) {
        float a = 0.78539816f * i + 0.39269908f; // зубья между осями
        ImVec2 d(cosf(a), sinf(a));
        dl->AddLine(ImVec2(c.x + d.x * 7.0f * scale, c.y + d.y * 7.0f * scale),
            ImVec2(c.x + d.x * 10.4f * scale, c.y + d.y * 10.4f * scale), col, 3.2f * scale);
    }
    dl->AddCircle(c, 7.0f * scale, col, 24, 2.4f * scale); // обод
    dl->AddCircle(c, 3.1f * scale, col, 20, 2.0f * scale); // отверстие
}

static void DrawCloseIcon(ImDrawList* dl, ImVec2 c, float scale, int alpha)
{
    ImU32 col = IM_COL32(245, 245, 248, alpha);
    float r = 7.0f * scale;
    float w = 2.0f * scale;
    ImVec2 a(c.x - r, c.y - r), b(c.x + r, c.y + r);
    ImVec2 d(c.x + r, c.y - r), e(c.x - r, c.y + r);
    dl->AddLine(a, b, col, w);
    dl->AddLine(d, e, col, w);
    // AddLine в ImGui имеет плоские концы; круги делают крестик округлым.
    dl->AddCircleFilled(a, w * 0.5f, col);
    dl->AddCircleFilled(b, w * 0.5f, col);
    dl->AddCircleFilled(d, w * 0.5f, col);
    dl->AddCircleFilled(e, w * 0.5f, col);
}

// Настройки рисуются поверх ручного draw-list, поэтому их hit-test делаем
// напрямую по экранному прямоугольнику — без зависимости от cursor/layout
// виджетов, которые были отрисованы раньше в этом же окне.
static ImU32 LerpCol(ImU32 a, ImU32 b, float t)
{
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    int ar = (a >> 0) & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255, aa = (a >> 24) & 255;
    int br = (b >> 0) & 255, bg2 = (b >> 8) & 255, bb = (b >> 16) & 255, ba = (b >> 24) & 255;
    return IM_COL32(ar + (int)((br - ar) * t), ag + (int)((bg2 - ag) * t),
        ab + (int)((bb - ab) * t), aa + (int)((ba - aa) * t));
}

static void DrawSettingToggle(ImDrawList* dl, const ImVec2& pos, bool& on, Fade& anim)
{
    const float w = 30.0f, h = 18.0f;
    const ImVec2 end(pos.x + w, pos.y + h);
    const bool hover = ImGui::IsMouseHoveringRect(pos, end, false);
    if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        on = !on;
    anim.speed = 12.0f; // шустрый отклик
    anim.target = on ? 1.0f : 0.0f;
    anim.update(ImGui::GetIO().DeltaTime);
    float e = Smooth01(anim.value); // мягкие концы вместо линейности
    // трек плавно перекрашивается серый->акцент, кругляш с попом в середине хода
    dl->AddRectFilled(pos, end,
        LerpCol(IM_COL32(255, 255, 255, 38), IM_COL32(90, 140, 255, 255), e), h * 0.5f);
    const float kr = (h - 6.0f) * 0.5f * (1.0f + 0.16f * sinf(3.14159265f * e));
    const float knobX = pos.x + 3.0f + kr + (w - h) * e;
    dl->AddCircleFilled(ImVec2(knobX, pos.y + h * 0.5f), kr, IM_COL32(245, 245, 248, 255));
}

void gui::Render(ImTextureID bg)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("panel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBackground); // фон не нужен — рисуем сами
    // Без NoInputs: пунктам меню нужны клики. Пустое место всё равно
    // таскает окно — ворота стоят в WndProc по IsAnyItemHovered.

    ImDrawList* dl = ImGui::GetWindowDrawList();
    // палитра чата (подробно: skils/chat-ui/SKILL.md)
    const ImU32 ACCENT = IM_COL32(90, 140, 255, 255);
    const ImU32 ACCENT_HOV = IM_COL32(112, 158, 255, 255);
    const ImU32 INK = IM_COL32(20, 22, 28, 255); // стрелка на акценте
    if (bg)
        dl->AddImage(bg, ImVec2(0, 0), io.DisplaySize); // рект с блюром
    Squircle(dl, ImVec2(0, 0), io.DisplaySize,
        IM_COL32(238, 238, 244, 120)); // светлый рект, блюр просвечивает
    Squircle(dl, ImVec2(0, 0), io.DisplaySize,
        IM_COL32(0, 0, 0, 195)); // чёрный рект сверху — итог серый
    const float pad = -0.0f;
    const float stripW = 138.0f;
    ImVec2 p0(pad + stripW, pad);
    ImVec2 p1(io.DisplaySize.x - pad, io.DisplaySize.y - pad);
    PanelMixed(dl, p0, p1, IM_COL32(0, 0, 0, 50));

    // --- фото в левом верхнем углу: меньше, края круглые ---
    float photoBoxH = 0.0f;
    {
        void* img = photo::Get();
        int iw = 0, ih = 0;
        if (img)
            photo::Size(&iw, &ih);
        if (img && iw > 0 && ih > 0) {
            const float m = 12.0f;
            float phh = 52.0f;
            float pww = phh * (float)iw / (float)ih;
            if (pww > 150.0f)
                pww = 150.0f;
            ImVec2 f0(m, m);
            ImVec2 f1(m + pww + 6, m + phh + 6);
            photoBoxH = (f1.y - f0.y) + 10.0f;
            dl->AddRectFilled(f0, f1, IM_COL32(0, 0, 0, 115), 10.0f);
            dl->AddImageRounded((ImTextureID)img,
                ImVec2(f0.x + 3, f0.y + 3), ImVec2(f1.x - 3, f1.y - 3),
                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 7.0f);
            dl->AddText(ImVec2(f1.x + 8, f0.y + 6), IM_COL32(255, 255, 255, 255), "FixAi");
        }
    }

    const char* cats[] = { "Главная", "AiHelper", "Файлы" };
    const int ncat = 3;
    static int selected = 0;
    static Fade hov[3];
    static bool settingsOpen = false;
    static bool smoothUi = config::GetBool("smooth", true);
    static bool islandVisible = config::GetBool("island", true);
    static bool generalOn = config::GetBool("general", false);
    static bool pcOn = config::GetBool("pc", false);
    static bool musOn = config::GetBool("mus", false);
    static bool nameOn = config::GetBool("name", true);
    static Fade fGen, fPc, fMus, fName;
    // конфиг пишем только по изменению (иначе файл дёргали бы 60 раз в секунду)
    {
        static bool lg = generalOn, lp = pcOn, lm = musOn, ln = nameOn;
        static bool ls = smoothUi, li = islandVisible;
        if (lg != generalOn) { lg = generalOn; config::SetBool("general", generalOn); }
        if (lp != pcOn) { lp = pcOn; config::SetBool("pc", pcOn); }
        if (lm != musOn) { lm = musOn; config::SetBool("mus", musOn); }
        if (ln != nameOn) { ln = nameOn; config::SetBool("name", nameOn); }
        if (ls != smoothUi) { ls = smoothUi; config::SetBool("smooth", smoothUi); }
        if (li != islandVisible) { li = islandVisible; config::SetBool("island", islandVisible); }
    }
    static Fade settingsAnim;
    settingsAnim.target = settingsOpen ? 1.0f : 0.0f;
    settingsAnim.speed = smoothUi ? 5.5f : 1000.0f;
    settingsAnim.update(io.DeltaTime);

    // (кнопка настроек переехала в карточку пользователя, на место стрелки)
    // чёрточка между фото и категориями
    dl->AddLine(ImVec2(pad + 10, pad + photoBoxH + 2), ImVec2(pad + stripW - 10, pad + photoBoxH + 2),
        IM_COL32(255, 255, 255, 7), 1.0f);
    ImGui::SetCursorPos(ImVec2(pad + 8, pad + 10 + photoBoxH + 8));
    static int lastSel = -1;
    static float selT0 = 0.0f;
    ImVec2 selA, selB;
    bool hasSel = false;
    for (int i = 0; i < ncat; i++) {
        ImGui::SetCursorPosX(pad + 12);
        if (widgets::MenuItem(i, cats[i], ImVec2(stripW - 16, 29), hov[i].value, selected == i))
            selected = i; // пока только подсветка, ничего не открываем
        if (i == selected) {
            selA = ImGui::GetItemRectMin();
            selB = ImGui::GetItemRectMax();
            hasSel = true;
        }
        hov[i].target = ImGui::IsItemHovered() ? 1.0f : 0.0f;
        hov[i].update(io.DeltaTime);
    }
    if (selected != lastSel) {
        lastSel = selected;
        selT0 = (float)ImGui::GetTime();
    }
    if (hasSel) {
        // живее: пульс подложки + пробег побыстрее (период 1.4)
        float now = (float)ImGui::GetTime();
        float t = fmodf(now - selT0, 1.4f);
        float p = (t < 0.5f) ? (-0.3f + (t / 0.5f) * 1.6f) : 10.0f;
        float base = 0.10f + 0.06f * (0.5f + 0.5f * sinf(now * 5.0f));
        if (void* g = selglow::Frame(p, base))
            dl->AddImage((ImTextureID)g, selA, selB);
    }

    // --- чат с FixAi (только на Главной, остальное пока заглушки) ---
    if (selected == 0) {
        struct Msg { bool mine; std::string text; };
        static std::vector<Msg> hist;
        static char input[256] = "";
        static bool scrollBottom = false;
        static bool transcribing = false;
        static bool streaming = false;
        static std::vector<std::string> swords;
        static size_t shown = 0;
        static double nextWordAt = 0;
        static bool llmPending = false; // ждём ответ Groq
        static std::string pendingQ;    // вопрос, ушедший в модель
        static size_t pendingIdx = 0;   // какой пузырь ждёт ответ

        auto trim = [](const std::string& s) {
            size_t a = 0;
            while (a < s.size() && (unsigned char)s[a] <= ' ')
                a++;
            size_t b = s.size();
            while (b > a && (unsigned char)s[b - 1] <= ' ')
                b--;
            return s.substr(a, b - a);
        };
        auto startStream = [&](const std::string& a) {
            // режем ответ на слова — появятся по очереди как стрим
            swords.clear();
            {
                size_t i = 0;
                while (i < a.size()) {
                    while (i < a.size() && a[i] == ' ')
                        i++;
                    size_t j = i;
                    while (j < a.size() && a[j] != ' ')
                        j++;
                    if (j > i)
                        swords.push_back(a.substr(i, j - i));
                    i = j;
                }
            }
            shown = 0;
            streaming = true;
            nextWordAt = ImGui::GetTime() + 0.4;
            hist.push_back({ false, "" });
        };
        auto sendIt = [&]() {
            std::string t = trim(input);
            if (t.empty())
                return;
            hist.push_back({ true, t });
            input[0] = '\0';
            scrollBottom = true;
            ImGui::SetKeyboardFocusHere(-1);
            if (llm::AskAsync(t)) {
                // умный ответ прилетит позже — пока точки
                llmPending = true;
                pendingQ = t;
                startStream("");
                pendingIdx = hist.size() - 1;
            } else {
                // модель занята или без ключа — локальная заглушка
                startStream(brain::Answer(t));
            }
        };

        const float chL = p0.x + 14, chR = p1.x - 14;
        const float chT = p0.y + 12, chB = p1.y - 12 - 52;
        ImGui::SetNextWindowPos(ImVec2(chL, chT));
        ImGui::SetNextWindowSize(ImVec2(chR - chL, chB - chT));
        ImGui::BeginChild("chat", ImVec2(0, 0), false, ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 5.0f);
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(255, 255, 255, 40));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(255, 255, 255, 70));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(255, 255, 255, 100));
        float availW = ImGui::GetContentRegionAvail().x;
        ImDrawList* cdl = ImGui::GetWindowDrawList();
        cdl->ChannelsSplit(2);
        if (hist.empty() && !streaming) {
            cdl->ChannelsSetCurrent(1);
            const char* hint = "Спроси что-нибудь…";
            ImVec2 tw = ImGui::CalcTextSize(hint);
            ImGui::SetCursorPos(ImVec2((availW - tw.x) * 0.5f, 30));
            ImGui::TextDisabled("%s", hint);
        }
        auto sanitize = [](const std::string& s) {
            // TextWrapped жрёт % как формат — удваиваем
            std::string o;
            o.reserve(s.size());
            for (char c : s) {
                o += c;
                if (c == '%')
                    o += '%';
            }
            return o;
        };
        // раскладка в оконных координатах: ML/MR поля чтобы не резало,
        // справа всегда место под листалу чтобы ширина не прыгала
        const float ML = 16.0f, MR = 16.0f;
        float contentW = availW - ML - MR - 10.0f;
        for (auto& m : hist) {
            // пустой хвост стрима не рисуем — вместо него точки ниже
            if (streaming && shown == 0 && !m.mine && m.text.empty() && &m == &hist.back())
                continue;
            cdl->ChannelsSetCurrent(1);
            float wrapW = contentW * 0.78f;
            // короткие в одну строку и жмём к своему краю, длинные как обычно
            ImVec2 tsz0 = ImGui::CalcTextSize(m.text.c_str());
            bool oneLine = tsz0.x <= wrapW && m.text.find('\n') == std::string::npos;
            float padB = 12.0f;
            float cx;
            if (oneLine) {
                cx = m.mine ? (ML + contentW - tsz0.x - padB * 2.0f) : ML;
                ImGui::SetCursorPosX(cx);
            } else {
                cx = m.mine ? (ML + contentW - wrapW) : ML;
                ImGui::SetCursorPosX(cx);
                ImGui::PushTextWrapPos(cx + wrapW);
            }
            ImGui::PushStyleColor(ImGuiCol_Text,
                m.mine ? IM_COL32(255, 255, 255, 255) : IM_COL32(230, 230, 236, 255));
            // жёсткая рамка по колонке: дальше неё текст не вылезет никак
            ImVec2 wq0(ImGui::GetCursorScreenPos());
            ImVec2 wq1(wq0.x + wrapW, wq0.y + 10000.0f);
            ImGui::PushClipRect(wq0, wq1, true);
            if (oneLine)
                ImGui::TextUnformatted(m.text.c_str());
            else
                ImGui::TextWrapped("%s", sanitize(m.text).c_str());
            ImGui::PopClipRect();
            ImGui::PopStyleColor();
            if (!oneLine)
                ImGui::PopTextWrapPos();
            ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
            cdl->ChannelsSetCurrent(0);
            ImU32 bc = m.mine ? IM_COL32(90, 140, 255, 70) : IM_COL32(255, 255, 255, 16);
            cdl->AddRectFilled(ImVec2(r0.x - padB, r0.y - 8), ImVec2(r1.x + padB, r1.y + 8), bc, 12.0f);
            // каретка пока дописываем
            if (streaming && shown > 0 && !m.mine && &m == &hist.back())
                cdl->AddRectFilled(ImVec2(r1.x + 5, r1.y - 13), ImVec2(r1.x + 7, r1.y + 1),
                    IM_COL32(230, 230, 236, 255), 1.0f);
            cdl->ChannelsSetCurrent(1);
            ImGui::Dummy(ImVec2(0, 20)); // промежуток больше суммы полей пузырей — не наезжают
        }
        if (streaming) {
            if (ImGui::GetTime() >= nextWordAt && shown < swords.size()) {
                if (!hist.empty()) {
                    if (shown > 0)
                        hist.back().text += " ";
                    hist.back().text += swords[shown];
                }
                shown++;
                nextWordAt = ImGui::GetTime() + 0.06; // пауза между словами
                scrollBottom = true;
            }
            if (shown >= swords.size() && !llmPending) {
                streaming = false;
            } else if (shown == 0) {
                // точки пока первое слово не подъехало
                cdl->ChannelsSetCurrent(1);
                ImGui::SetCursorPosX(0);
                ImVec2 dp = ImGui::GetCursorScreenPos();
                float t0 = (float)ImGui::GetTime();
                for (int i = 0; i < 3; i++) {
                    float ph = t0 * 6.0f + (float)i * 0.9f;
                    float k = 0.5f + 0.5f * sinf(ph);
                    cdl->AddCircleFilled(ImVec2(dp.x + 6 + i * 12, dp.y + 8 - 3.0f * k),
                        3.0f, IM_COL32(230, 230, 236, (int)(255 * (0.4f + 0.6f * k))));
                }
                ImGui::Dummy(ImVec2(36, 16));
            }
        }
        if (transcribing) {
            std::string heard;
            if (stt::Poll(heard)) {
                transcribing = false;
                if (heard.empty()) {
                    hist.push_back({ false, "Не расслышал, повтори громче." });
                } else {
                    size_t n = heard.size();
                    if (n > sizeof(input) - 1)
                        n = sizeof(input) - 1;
                    for (size_t i = 0; i < n; i++)
                        input[i] = heard[i];
                    input[n] = '\0';
                    sendIt();
                }
                scrollBottom = true;
            } else {
                cdl->ChannelsSetCurrent(1);
                ImGui::SetCursorPosX(0);
                ImVec2 dp = ImGui::GetCursorScreenPos();
                float t0 = (float)ImGui::GetTime();
                for (int i = 0; i < 3; i++) {
                    float ph = t0 * 6.0f + (float)i * 0.9f;
                    float k = 0.5f + 0.5f * sinf(ph);
                    cdl->AddCircleFilled(ImVec2(dp.x + 6 + i * 12, dp.y + 8 - 3.0f * k),
                        3.0f, IM_COL32(230, 230, 236, (int)(255 * (0.4f + 0.6f * k))));
                }
                ImGui::Dummy(ImVec2(36, 16));
            }
        }
        if (llmPending) {
            std::string ans;
            if (llm::Poll(ans)) {
                llmPending = false;
                // поздний ответ не должен затирать чужой пузырь
                if (!hist.empty() && pendingIdx + 1 == hist.size() && hist.back().text.empty()) {
                    std::string a = ans.empty() ? brain::Answer(pendingQ) : ans;
                    if (!ans.empty() && a.rfind("CMD:", 0) == 0) {
                        size_t c1 = a.find(':', 4);
                        std::string act = (c1 == std::string::npos) ? a.substr(4) : a.substr(4, c1 - 4);
                        std::string prm = (c1 == std::string::npos) ? "" : a.substr(c1 + 1);
                        if (act == "musicSearch") {
                            size_t c2 = prm.find(':');
                            std::string pv = (c2 == std::string::npos) ? "youtube" : prm.substr(0, c2);
                            std::string qq = (c2 == std::string::npos) ? prm : prm.substr(c2 + 1);
                            prm = brain::FixMusicProvider(pendingQ, pv) + ":" + qq;
                        }
                        std::string done = brain::ExecCmd(act, prm);
                        if (!done.empty())
                            a = done;
                    }
                    swords.clear();
                    {
                        size_t i = 0;
                        while (i < a.size()) {
                            while (i < a.size() && a[i] == ' ')
                                i++;
                            size_t j = i;
                            while (j < a.size() && a[j] != ' ')
                                j++;
                            if (j > i)
                                swords.push_back(a.substr(i, j - i));
                            i = j;
                        }
                    }
                    shown = 0;
                    nextWordAt = ImGui::GetTime();
                    scrollBottom = true;
                }
            }
        }
        cdl->ChannelsMerge();
        if (scrollBottom) {
            ImGui::SetScrollHereY(1.0f);
            scrollBottom = false;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
        ImGui::EndChild();

        // строка ввода: поле + микрофон + отправка
        float inY = chB + 8;
        ImGui::SetCursorPos(ImVec2(chL, inY));
        ImGui::PushItemWidth(chR - chL - 96);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(255, 255, 255, 18));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 8));
        bool go = ImGui::InputText("##in", input, sizeof(input), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::PopItemWidth();
        if (go)
            sendIt();

        // кружок микрофона: жмём — пишет, жмём ещё — стоп и файл
        ImGui::SetCursorPos(ImVec2(chR - 78, inY + 1));
        bool micHov = false;
        if (ImGui::InvisibleButton("##mic", ImVec2(32, 32))) {
            if (mic::IsRecording()) {
                mic::Stop();
                const char* vp = mic::LastPath();
                if (vp && *vp && stt::TranscribeFile(vp)) {
                    transcribing = true;
                } else {
                    hist.push_back({ false, "Не смог начать распознавание." });
                }
            } else {
                mic::Start();
            }
            scrollBottom = true;
        }
        micHov = ImGui::IsItemHovered();
        {
            ImVec2 mc = ImGui::GetItemRectMin();
            ImVec2 cc(mc.x + 16, inY + 17);
            ImU32 micBg = mic::IsRecording() ? IM_COL32(255, 90, 90, 90) :
                (micHov ? IM_COL32(255, 255, 255, 36) : IM_COL32(255, 255, 255, 18));
            dl->AddCircleFilled(cc, 16.0f, micBg);
            dl->AddRectFilled(ImVec2(cc.x - 4, inY + 9), ImVec2(cc.x + 4, inY + 19),
                IM_COL32(230, 230, 236, 255), 4.0f);
            dl->PathArcTo(cc, 7.0f, 0.0f, 3.14159265f);
            dl->PathStroke(IM_COL32(230, 230, 236, 255), 0, 2.0f);
            dl->AddLine(ImVec2(cc.x, inY + 25), ImVec2(cc.x, inY + 28),
                IM_COL32(230, 230, 236, 255), 2.0f);
            dl->AddLine(ImVec2(cc.x - 4, inY + 28), ImVec2(cc.x + 4, inY + 28),
                IM_COL32(230, 230, 236, 255), 2.0f);
        }
        // кружок отправки
        ImGui::SetCursorPos(ImVec2(chR - 38, inY + 1));
        if (ImGui::InvisibleButton("##send", ImVec2(32, 32)))
            sendIt();
        {
            ImVec2 mc = ImGui::GetItemRectMin();
            ImVec2 cc(mc.x + 16, inY + 17);
            bool hov = ImGui::IsItemHovered();
            dl->AddCircleFilled(cc, 16.0f, hov ? ACCENT_HOV : ACCENT);
            // шеврон из пака, зеркалим вправо; нет файла — треугольник
            if (void* ar = photo::LoadIcon("Icon/icons/arrow.png"))
                dl->AddImage(ar, ImVec2(cc.x - 9, inY + 8), ImVec2(cc.x + 9, inY + 26),
                    ImVec2(1, 0), ImVec2(0, 1), INK);
            else
                dl->AddTriangleFilled(ImVec2(cc.x - 6, inY + 23), ImVec2(cc.x - 6, inY + 11),
                    ImVec2(cc.x + 8, inY + 17), INK);
        }
    } else if (selected == 1) {
        // AiHelper: три iOS-карточки. Верх едет за островом: остров скрыт —
        // карточки поднимаются к самому верху, показан — плавно сползают ниже.
        static float cardsTop = -1.0f;
        float wantTop = p0.y + (islandVisible ? 58.0f : 12.0f);
        if (cardsTop < 0.0f)
            cardsTop = wantTop;
        float ck = io.DeltaTime * 6.0f;
        if (ck > 1.0f)
            ck = 1.0f;
        cardsTop += (wantTop - cardsTop) * ck;
        // появление вкладки: слайд снизу + проявление (easeOutCubic)
        static int seenSel = -1;
        static float enterA = 1.0f;
        if (seenSel != selected) {
            seenSel = selected;
            enterA = 0.0f;
        }
        enterA += io.DeltaTime * 4.0f;
        if (enterA > 1.0f)
            enterA = 1.0f;
        float ee = 1.0f - (1.0f - enterA) * (1.0f - enterA) * (1.0f - enterA);
        const float ct = cardsTop + (1.0f - ee) * 18.0f;

        const float cm = 14.0f, cgap = 10.0f; // поля и щель между карточками
        float cb = p1.y - 12.0f;
        float cx0 = p0.x + cm, cx1 = p1.x - cm;
        float midX = (cx0 + cx1) * 0.5f;
        float topH = (cb - cardsTop) * 0.44f;
        if (topH < 40.0f)
            topH = 40.0f;
        auto Card = [&](ImVec2 a, ImVec2 b) {
            // обратно круги r14; стек стекла тот же (блюр + светлый + тёмный).
            // Альфа едет за появлением вкладки (ee).
            int la = (int)(120 * ee), ba = (int)(215 * ee), ta = (int)(255 * ee);
            if (bg) {
                ImVec2 uv0(a.x / io.DisplaySize.x, a.y / io.DisplaySize.y);
                ImVec2 uv1(b.x / io.DisplaySize.x, b.y / io.DisplaySize.y);
                dl->AddImageRounded(bg, a, b, uv0, uv1, IM_COL32(255, 255, 255, ta), 14.0f);
            }
            dl->AddRectFilled(a, b, IM_COL32(238, 238, 244, la), 14.0f);
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, ba), 14.0f);
        };
        Card(ImVec2(cx0, ct), ImVec2(midX - cgap * 0.5f, ct + topH));
        Card(ImVec2(midX + cgap * 0.5f, ct), ImVec2(cx1, ct + topH));
        Card(ImVec2(cx0, ct + topH + cgap), ImVec2(cx1, cb));
        // --- наполнение: General / Control PC / Music + статус слушателя ---
        {
            float colL1 = midX - cgap * 0.5f;
            float rx0 = cx0 + 12.0f;
            const float tgW = 30.0f;
            // левая верхняя: General + разделитель + Control PC + статус
            dl->AddText(ImVec2(rx0, ct + 12.0f), IM_COL32(245, 245, 248, 255), "General");
            DrawSettingToggle(dl, ImVec2(colL1 - 12.0f - tgW, ct + 12.0f), generalOn, fGen);
            float dy = ct + 12.0f + 28.0f;
            dl->AddLine(ImVec2(rx0, dy), ImVec2(colL1 - 12.0f, dy), IM_COL32(255, 255, 255, 16), 1.0f);
            dl->AddText(ImVec2(rx0, dy + 10.0f), IM_COL32(245, 245, 248, 255), "Control PC");
            DrawSettingToggle(dl, ImVec2(colL1 - 12.0f - tgW, dy + 10.0f), pcOn, fPc);
            dl->AddText(ImVec2(rx0, dy + 40.0f), IM_COL32(245, 245, 248, 255), "Music");
            DrawSettingToggle(dl, ImVec2(colL1 - 12.0f - tgW, dy + 40.0f), musOn, fMus);
            dl->AddText(ImVec2(rx0, dy + 70.0f), IM_COL32(245, 245, 248, 255), "По имени");
            DrawSettingToggle(dl, ImVec2(colL1 - 12.0f - tgW, dy + 70.0f), nameOn, fName);
            ears::SetGeneral(generalOn);
            ears::SetControlPc(pcOn);
            ears::SetMusic(musOn);
            ears::SetRequireName(nameOn);
            ears::Snap sn;
            ears::Snapshot(sn);
            std::string st;
            ImU32 stCol = IM_COL32(150, 150, 158, 255);
            if (!generalOn)
                st = "выключено";
            else if (!pcOn && !musOn)
                st = "включи Control PC или Music";
            else if (!sn.running)
                st = "запускаюсь…";
            else if (sn.phase == 1) {
                if (sn.maybeMuted) {
                    st = "микрофон молчит — мьют?";
                    stCol = IM_COL32(200, 180, 120, 255);
                } else {
                    st = "слушаю…";
                    stCol = IM_COL32(139, 92, 246, 255);
                }
            } else if (sn.phase == 2)
                st = "думаю: " + sn.heard;
            else if (sn.phase == 3)
                st = sn.result;
            dl->AddText(ImVec2(rx0, dy + 100.0f), stCol, FitText(st, (colL1 - 12.0f) - rx0).c_str());
            // правая верхняя: сервисы по категориям, плавный скролл, клик открывает
            // (музыкальный ряд кликом ещё и становится выбором для «включи музыку»)
            {
                struct Svc {
                    const char* cat;
                    const char* name;
                    const char* url;
                    const char* key; // music/work/fun — выбор в категории свой
                    bool sel;        // музыкальный ряд: клик только выбирает
                };
                static const Svc svcs[] = {
                    { "Музыка", "Spotify", "https://open.spotify.com/", "music", true },
                    { "Музыка", "Яндекс Музыка", "https://music.yandex.ru/", "music", true },
                    { "Музыка", "SoundCloud", "https://soundcloud.com/", "music", true },
                    { "Музыка", "Piped (без рекламы)", "https://piped.video/", "music", true },
                    { "Работа", "Gmail", "https://mail.google.com/", "work", false },
                    { "Работа", "Google Docs", "https://docs.google.com/", "work", false },
                    { "Работа", "Notion", "https://www.notion.so/", "work", false },
                    { "Работа", "GitHub", "https://github.com/", "work", false },
                    { "Развлечения", "YouTube", "https://www.youtube.com/", "fun", false },
                    { "Развлечения", "TikTok", "https://www.tiktok.com/", "fun", false },
                    { "Развлечения", "Twitch", "https://www.twitch.tv/", "fun", false },
                    { "Развлечения", "Steam", "https://store.steampowered.com/", "fun", false },
                    { "Развлечения", "Rutube", "https://rutube.ru/", "fun", false },
                };
                float rx0 = midX + cgap * 0.5f, rx1 = cx1;
                ImVec2 cA(rx0, ct), cB(rx1, ct + topH);
                static float scrCur = 0.0f, scrTgt = 0.0f;
                const float rowH = 26.0f, headH = 22.0f;
                float contentH = 16.0f;
                std::string lastCat;
                for (auto& s : svcs) {
                    if (lastCat != s.cat) {
                        contentH += headH;
                        lastCat = s.cat;
                    }
                    contentH += rowH;
                }
                float maxScr = contentH - (cB.y - cA.y);
                if (maxScr < 0.0f)
                    maxScr = 0.0f;
                if (ImGui::IsMouseHoveringRect(cA, cB, false) && io.MouseWheel != 0.0f) {
                    scrTgt -= io.MouseWheel * 36.0f;
                    if (scrTgt < 0.0f)
                        scrTgt = 0.0f;
                    if (scrTgt > maxScr)
                        scrTgt = maxScr;
                }
                float sk = io.DeltaTime * 8.0f;
                if (sk > 1.0f)
                    sk = 1.0f;
                scrCur += (scrTgt - scrCur) * sk;
                ImFont* smallFo2 = io.Fonts->Fonts.size() > 1 ? io.Fonts->Fonts[1] : nullptr;
                if (smallFo2)
                    ImGui::PushFont(smallFo2);
                dl->PushClipRect(cA, cB, true);
                std::string selMus = actions::SvcSel("music");
                std::string selWork = actions::SvcSel("work");
                std::string selFun = actions::SvcSel("fun");
                float y = cA.y + 8.0f - scrCur;
                lastCat.clear();
                for (auto& s : svcs) {
                    if (lastCat != s.cat) {
                        if (y + headH >= cA.y && y <= cB.y)
                            dl->AddText(ImVec2(rx0 + 10.0f, y + 3.0f),
                                IM_COL32(150, 150, 158, 255), s.cat);
                        y += headH;
                        lastCat = s.cat;
                    }
                    if (y + rowH >= cA.y && y <= cB.y) {
                        bool hov = ImGui::IsMouseHoveringRect(
                            ImVec2(rx0, y), ImVec2(rx1, y + rowH), false);
                        if (hov)
                            dl->AddRectFilled(ImVec2(rx0 + 4.0f, y), ImVec2(rx1 - 4.0f, y + rowH),
                                IM_COL32(255, 255, 255, 14), 8.0f);
                        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            // только выбор — сайты открывает голос, а не клик
                            actions::SetSvcSel(s.key, s.url);
                        }
                        std::string want = (s.key[0] == 'w') ? selWork : (s.key[0] == 'f' ? selFun : selMus);
                        if (want == s.url)
                            dl->AddCircleFilled(ImVec2(rx0 + 12.0f, y + rowH * 0.5f),
                                3.0f, IM_COL32(139, 92, 246, 255));
                        dl->AddText(ImVec2(rx0 + 22.0f, y + 5.0f),
                            IM_COL32(235, 235, 240, 255), s.name);
                    }
                    y += rowH;
                }
                dl->PopClipRect();
                if (smallFo2)
                    ImGui::PopFont();
            }
            // пилюля помощников — вверху слева нижней карточки
            {
                ImVec2 wp = ImGui::GetWindowPos();
                float pillX = cx0 + 12.0f;
                float pillY = ct + topH + cgap + 10.0f;
                helpers::Render(ImGui::GetForegroundDrawList(),
                    wp.x + pillX + 55.0f, wp.y + pillY + 28.0f, wp, io.DeltaTime);
            }
        }
    } else {
        const char* hint2 = "Раздел скоро будет";
        ImVec2 tw2 = ImGui::CalcTextSize(hint2);
        dl->AddText(ImVec2((p0.x + p1.x - tw2.x) * 0.5f, (p0.y + p1.y) * 0.5f),
            IM_COL32(255, 255, 255, 70), hint2);
    }

    // Настройки — компактный попап. Фон сплошной (17,19,27): цвет всегда один.
    // Открытие — морфингом из точки шестерёнки (easeOutBack, лёгкий перелёт).
    if (settingsAnim.value > 0.001f) {
        // та же точка что у кнопки (считаем тут — панель рисуется раньше карточки)
        const ImVec2 gearC(pad + stripW - 8.0f - 21.0f, io.DisplaySize.y - pad - 8.0f - 56.0f + 28.0f);
        const float panelW = 190.0f, panelH = 96.0f;
        ImVec2 fullB(p0.x + 12.0f + panelW, p1.y - 12.0f - 52.0f - 10.0f);
        ImVec2 fullA(fullB.x - panelW, fullB.y - panelH);
        float e;
        if (settingsOpen) {
            float d = settingsAnim.value - 1.0f;
            e = 1.0f + 2.70158f * d * d * d + 1.70158f * d * d;
        } else {
            e = Smooth01(settingsAnim.value);
        }
        if (e < 0.0f)
            e = 0.0f;
        if (e > 1.12f)
            e = 1.12f;
        ImVec2 sa(gearC.x + (fullA.x - gearC.x) * e, gearC.y + (fullA.y - gearC.y) * e);
        ImVec2 sb(gearC.x + (fullB.x - gearC.x) * e, gearC.y + (fullB.y - gearC.y) * e);
        dl->AddRectFilled(ImVec2(sa.x, sa.y + 3.0f), ImVec2(sb.x, sb.y + 4.0f),
            IM_COL32(0, 0, 0, 90), 14.0f); // тень
        dl->AddRectFilled(sa, sb, IM_COL32(17, 19, 27, 255), 14.0f);
        dl->AddRect(sa, sb, IM_COL32(255, 255, 255, 30), 14.0f, 0, 1.0f);
        dl->PushClipRect(sa, sb, true);
        float av = settingsAnim.value;
        if (av > 1.0f)
            av = 1.0f;
        if (av < 0.0f)
            av = 0.0f;
        int a = (int)(255 * av);
        ImFont* smallFo = io.Fonts->Fonts.size() > 1 ? io.Fonts->Fonts[1] : nullptr;
        if (smallFo)
            ImGui::PushFont(smallFo);
        dl->AddText(ImVec2(sa.x + 12.0f, sa.y + 10.0f), IM_COL32(245, 245, 248, a), "Настройки");
        static Fade smoothAnim, islandAnim;
        DrawSettingToggle(dl, ImVec2(sb.x - 12.0f - 30.0f, sa.y + 38.0f), smoothUi, smoothAnim);
        DrawSettingToggle(dl, ImVec2(sb.x - 12.0f - 30.0f, sa.y + 64.0f), islandVisible, islandAnim);
        dl->AddText(ImVec2(sa.x + 12.0f, sa.y + 39.0f), IM_COL32(224, 226, 232, a), "Плавные анимации");
        dl->AddText(ImVec2(sa.x + 12.0f, sa.y + 65.0f), IM_COL32(224, 226, 232, a), "Показывать остров");
        if (smallFo)
            ImGui::PopFont();
        dl->PopClipRect();
    }

    // --- юзер снизу как на референсе: аватарка, имя, подписка, стрелка ---
    {
        const UserCard& u = user::Get();
        const float ch = 56.0f;
        ImVec2 u0(pad + 8, io.DisplaySize.y - pad - 8 - ch);
        ImVec2 u1(pad + stripW - 8, io.DisplaySize.y - pad - 8);

        const float asz = 36.0f;
        float ax = u0.x + 10, ay = u0.y + 10;
        if (u.avatarTex) {
            dl->AddImageRounded((ImTextureID)u.avatarTex,
                ImVec2(ax, ay), ImVec2(ax + asz, ay + asz),
                ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 18.0f);
        } else {
            // заглушка: круг + первая буква имени
            dl->AddCircleFilled(ImVec2(ax + asz / 2, ay + asz / 2), asz / 2,
                IM_COL32(120, 120, 130, 255));
            std::string ch1 = "?";
            if (!u.name.empty()) {
                unsigned char c = (unsigned char)u.name[0];
                size_t len = 1;
                if ((c & 0x80) == 0)
                    len = 1;
                else if ((c & 0xE0) == 0xC0)
                    len = 2;
                else if ((c & 0xF0) == 0xE0)
                    len = 3;
                else if ((c & 0xF8) == 0xF0)
                    len = 4;
                ch1 = u.name.substr(0, len);
            }
            ImVec2 tsz = ImGui::CalcTextSize(ch1.c_str());
            dl->AddText(ImVec2(ax + (asz - tsz.x) * 0.5f, ay + (asz - tsz.y) * 0.5f),
                IM_COL32(30, 30, 34, 255), ch1.c_str());
        }

        // Ник не режем троеточием: он клипится и растворяется через тот же
        // пиксельный шейдер, что и название трека.
        float tx = ax + asz + 10;
        float nameEnd = u1.x - 40.0f; // клип до шестерёнки
        dl->PushClipRect(ImVec2(tx, ay), ImVec2(nameEnd, ay + 18.0f), true);
        textfade::AddText(dl, ImVec2(tx, ay + 1), IM_COL32(245, 245, 248, 255),
            u.name.c_str(), nameEnd - 24.0f, nameEnd);
        dl->PopClipRect();
        dl->AddText(ImVec2(tx, ay + 20), IM_COL32(150, 150, 158, 255), u.sub.c_str());
        // шестерёнка настроек вместо мёртвой стрелки (красный кружок на скрине)
        ImVec2 gearPos(u1.x - 34.0f, ay + 5.0f);
        ImGui::SetCursorPos(gearPos);
        if (ImGui::InvisibleButton("##settings_toggle", ImVec2(26.0f, 26.0f)))
            settingsOpen = !settingsOpen;
        bool settingsHover = ImGui::IsItemHovered();
        ImVec2 gearC(gearPos.x + 13.0f, gearPos.y + 13.0f);
        if (settingsHover)
            dl->AddCircleFilled(gearC, 13.0f, IM_COL32(255, 255, 255, 18));
        float swap = Smooth01(settingsAnim.value);
        // точная иконка из твоего SVG (рендер x2 для чёткости), morph в крестик
        if (void* gearTex = svg::Load("Icon/icons/font/mainmenu/settings.svg", 48)) {
            float r = 9.0f;
            dl->AddImage((ImTextureID)gearTex, ImVec2(gearC.x - r, gearC.y - r),
                ImVec2(gearC.x + r, gearC.y + r), ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32(255, 255, 255, (int)(255 * (1.0f - swap))));
        } else {
            DrawSettingsGear(dl, gearC, 0.18f + 0.64f * (1.0f - swap), (int)(255 * (1.0f - swap)));
        }
        DrawCloseIcon(dl, gearC, 0.20f + 0.65f * swap, (int)(255 * swap));

    // --- динамический остров сверху-центр (музыка / AiHelper), поверх всего ---
    {
        ImVec2 winPos = ImGui::GetWindowPos();
        if (islandVisible)
            island::Render(ImGui::GetForegroundDrawList(),
                ImVec2(winPos.x, winPos.y),
                ImVec2(winPos.x + io.DisplaySize.x, winPos.y + io.DisplaySize.y), io.DeltaTime);
    }

    ImGui::End();
    }
}
