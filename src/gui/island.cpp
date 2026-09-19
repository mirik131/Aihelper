#include "gui/island.h"
#include "gui/media.h"
#include "gui/textfade.h"
#include <cmath>
#include <string>

namespace island {
namespace {

// секунды -> "m:ss"
std::string Ts(double s)
{
    if (s < 0)
        s = 0;
    int total = (int)s;
    char b[16];
    snprintf(b, sizeof(b), "%d:%02d", total / 60, total % 60);
    return b;
}

} // namespace

void Render(ImDrawList* dl, ImVec2 panelL, ImVec2 panelR, float dt)
{
    if (!dl)
        return;
    media::Info mi;
    media::Snapshot(mi);

    // === ПОЗИЦИЯ ОСТРОВА (крути тут) ===
    // panelL/panelR — весь прямоугольник окна на экране (gui.cpp шлёт окно целиком).
    // cx — центр по горизонтали, top — отступ сверху.
    float cx = (panelL.x + panelR.x) * 0.5f;
    float top = panelL.y + 8.0f;

    static float curW = 140.0f, curH = 30.0f;
    static std::string tKey;    // трек, под который держим таймер
    static double shownPos = 0; // показанное время
    static unsigned long long shownTick = 0;
    static double srcBase = 0;  // последнее принятое значение источника
    static double tDur = 0; // запомненная длина (тоже дёргается — не мигаем)
    // строка и время считаем заранее — от них зависит ширина пилюли
    std::string line = mi.title.empty() ? "Неизвестный трек" : mi.title;
    if (!mi.artist.empty())
        line += " — " + mi.artist;
    std::string key = mi.title + "\n" + mi.artist;
    unsigned long long nowMs = GetTickCount64();
    if (key != tKey) {
        tKey = key;
        shownPos = mi.pos;
        srcBase = mi.pos;
        shownTick = nowMs;
        tDur = 0;
    }
    if (mi.dur > 1.0)
        tDur = mi.dur;
    // таймер ведём сами: источник (SoundCloud и ко) стоит или дёргается —
    // верим ему только когда он реально движется, иначе идём своими часами
    double cand = mi.pos + (mi.playing ? (double)(nowMs - mi.tickMs) / 1000.0 : 0.0);
    double fwd = cand - srcBase;
    double cur;
    if (fwd < -3.0 || fwd > 0.25) {
        shownPos = cand; // сик или живой источник — верим ему
        srcBase = cand;
        shownTick = nowMs;
        cur = cand;
    } else {
        cur = shownPos + (mi.playing ? (double)(nowMs - shownTick) / 1000.0 : 0.0);
    }
    if (tDur > 1.0 && cur > tDur)
        cur = tDur;
    if (cur < 0)
        cur = 0;
    std::string timeStr = Ts(cur);
    if (tDur > 1.0)
        timeStr += "/" + Ts(tDur);
    // ширина по контенту, вся пилюля ещё уже (макс 170)
    float fullTW = ImGui::CalcTextSize(line.c_str()).x;
    if (fullTW > 70.0f)
        fullTW = 70.0f;
    float timeW0 = ImGui::CalcTextSize(timeStr.c_str()).x;
    float tW = mi.has ? (5.0f + 22.0f + 5.0f + fullTW + 5.0f + timeW0 + 5.0f + 20.0f + 10.0f) : 140.0f;
    if (tW < 140.0f)
        tW = 140.0f;
    if (tW > 170.0f)
        tW = 170.0f;
    // После удаления нижней полосы остров остаётся компактным и симметричным.
    float tH = mi.has ? 32.0f : 30.0f;
    float panelW = panelR.x - panelL.x - 24.0f;
    if (tW > panelW)
        tW = panelW;
    float k = dt * 7.0f;
    if (k > 1.0f)
        k = 1.0f;
    if (k < 0.0f)
        k = 0.0f;
    curW += (tW - curW) * k;
    curH += (tH - curH) * k;

    const ImU32 VIOLET = IM_COL32(139, 92, 246, 255); // акцент прогресс-кольца
    ImVec2 a(cx - curW * 0.5f, top), b(cx + curW * 0.5f, top + curH);
    float rad = curH * 0.5f; // полностью круглые торцы — «овальный»

    // liquid glass: полупрозрачная заливка + светлая обводка + верхний блик + тень.
    // (живого блюра под пилюлей нет — блюрим только фон окна, так что стекло
    // имитируем translucency + specular; настоящий backdrop-blur тут не снять дёшево)
    auto GlassPill = [&](ImU32 fill) {
        dl->AddRectFilled(ImVec2(a.x, a.y + 2.0f), ImVec2(b.x, b.y + 3.0f),
            IM_COL32(0, 0, 0, 70), rad);
        dl->AddRectFilled(a, b, fill, rad);
        // обводка едва-едва — яркая читалась белой каймой, верхний блик убрали совсем:
        // именно он давал странное белое «что-то» сверху пилюли
        dl->AddRect(a, b, IM_COL32(255, 255, 255, 24), rad, 0, 1.0f);
    };
    GlassPill(IM_COL32(16, 20, 32, 150));

    float midY = (a.y + b.y) * 0.5f;
    if (!mi.has) {
        tKey.clear(); // музыки нет — следующий трек начнёт таймер с нуля
        srcBase = 0;
        // тихо: пульс + AiHelper
        float t = (float)ImGui::GetTime();
        float p = 0.5f + 0.5f * sinf(t * 3.0f);
        ImVec2 dc(a.x + 16.0f, midY);
        dl->AddCircleFilled(dc, 4.0f, IM_COL32(139, 92, 246, (int)(120 + 100 * p)));
        dl->AddText(ImVec2(dc.x + 11.0f, midY - 8.0f), IM_COL32(235, 235, 240, 255), "AiHelper");
        return;
    }
    // раскрытие: контент проявляется по мере роста
    float ca = (curW - 120.0f) / 50.0f;
    if (ca < 0.0f)
        ca = 0.0f;
    if (ca > 1.0f)
        ca = 1.0f;
    if (ca <= 0.0f)
        return;
    auto AL = [&](ImU32 c) -> ImU32 {
        int al = (int)(((c >> 24) & 255) * ca);
        return (c & 0x00FFFFFF) | ((ImU32)al << 24);
    };

    // обложка (или нота если плеер арт не отдал)
    ImVec2 c0(a.x + 5.0f, midY - 11.0f), c1(c0.x + 22.0f, c0.y + 22.0f);
    if (void* cov = media::Cover()) {
        dl->AddImageRounded((ImTextureID)cov, c0, c1, ImVec2(0, 0), ImVec2(1, 1), AL(IM_COL32_WHITE), 7.0f);
    } else {
        dl->AddRectFilled(c0, c1, AL(IM_COL32(255, 255, 255, 26)), 7.0f);
        ImVec2 hd(c0.x + 8.0f, c0.y + 15.0f);
        dl->AddLine(ImVec2(hd.x + 2.6f, hd.y), ImVec2(hd.x + 2.6f, c0.y + 5.0f),
            AL(IM_COL32(235, 235, 240, 255)), 1.5f);
        dl->AddLine(ImVec2(hd.x + 2.6f, c0.y + 5.0f), ImVec2(hd.x + 7.0f, c0.y + 7.0f),
            AL(IM_COL32(235, 235, 240, 255)), 1.5f);
        dl->AddCircleFilled(hd, 2.6f, AL(IM_COL32(235, 235, 240, 255)));
    }

    // прогресс-кольцо справа (круглый «слайдер»): дуга + бегущая дуга пока играет
    double frac = 0.0;
    if (tDur > 1.0)
        frac = cur / tDur;
    ImVec2 rc(b.x - 13.0f, midY);
    const float rr = 7.5f;
    dl->PathArcTo(rc, rr, 0.0f, 6.2831853f);
    dl->PathStroke(AL(IM_COL32(255, 255, 255, 22)), 0, 2.0f);
    if (frac > 0.003) {
        dl->PathArcTo(rc, rr, -1.5707963f, -1.5707963f + (float)(frac * 6.2831853f));
        dl->PathStroke(AL(mi.playing ? VIOLET : IM_COL32(150, 150, 160, 255)), 0, 2.0f);
    }
    float nowT = (float)ImGui::GetTime();
    if (mi.playing) {
        // бегунок подлиннее: короткая светлая дуга бежит по кольцу
        float ang = nowT * 4.5f;
        dl->PathArcTo(rc, rr, ang - 0.35f, ang + 0.35f);
        dl->PathStroke(AL(IM_COL32(196, 181, 253, 255)), 0, 2.0f);
    }
    // одна строка «название — артист» + время «1:23/3:45» (без пробелов — больше места тексту)
    float timeW = ImGui::CalcTextSize(timeStr.c_str()).x;
    float timeX1 = rc.x - rr - 5.0f; // правый край времени
    dl->AddText(ImVec2(timeX1 - timeW, midY - 8.0f), AL(IM_COL32(150, 150, 158, 255)), timeStr.c_str());
    float tx = c1.x + 5.0f;
    float textEnd = timeX1 - timeW - 0.0f; // где текст должен закончиться
    float lineH = ImGui::CalcTextSize("Ag").y;
    float ty = midY - lineH * 0.5f;
    float fullW = ImGui::CalcTextSize(line.c_str()).x;
    if (fullW > textEnd - tx) {
        // Длинное — клип + настоящий пиксельный шейдер: альфа каждого пикселя
        // мягко стремится к нулю в последних 30 px, без резкой границы.
        dl->PushClipRect(ImVec2(tx, a.y + 2.0f), ImVec2(textEnd, b.y - 2.0f), true);
        textfade::AddText(dl, ImVec2(tx, ty), AL(IM_COL32(245, 245, 248, 255)),
            line.c_str(), textEnd - 30.0f, textEnd);
        dl->PopClipRect();
    } else {
        dl->AddText(ImVec2(tx, ty), AL(IM_COL32(245, 245, 248, 255)), line.c_str());
    }
}

} // namespace island
