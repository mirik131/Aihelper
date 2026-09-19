#include "gui/helpers.h"
#include "gui/adskip.h"
#include "gui/config.h"
#include <cmath>

namespace helpers {
namespace {

bool g_open = false;

ImU32 LerpCol(ImU32 a, ImU32 b, float t)
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

float Smooth01(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 1.0f)
        v = 1.0f;
    return v * v * (3.0f - 2.0f * v);
}

void Toggle(ImDrawList* dl, ImVec2 pos, bool& on, float& av, float dt, int alpha = 255)
{
    const float w = 30.0f, h = 18.0f;
    bool hov = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + h), false);
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        on = !on;
    float sp = dt * 12.0f;
    if (sp > 1.0f)
        sp = 1.0f;
    av += ((on ? 1.0f : 0.0f) - av) * sp;
    float e = av * av * (3.0f - 2.0f * av);
    auto WithA = [&](ImU32 c) -> ImU32 {
        int al = (int)(((c >> 24) & 255) * alpha / 255);
        return (c & 0x00FFFFFF) | ((ImU32)al << 24);
    };
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h),
        WithA(LerpCol(IM_COL32(255, 255, 255, 38), IM_COL32(90, 140, 255, 255), e)), h * 0.5f);
    float kr = (h - 6.0f) * 0.5f * (1.0f + 0.16f * sinf(3.14159265f * e));
    dl->AddCircleFilled(ImVec2(pos.x + 3.0f + kr + (w - h) * e, pos.y + h * 0.5f), kr,
        WithA(IM_COL32(245, 245, 248, 255)));
}

} // namespace

void ToggleOpen()
{
    g_open = !g_open;
}

void Render(ImDrawList* fg, float cxScreen, float pillBottomScreenY, ImVec2 winPos, float dt)
{
    if (!fg)
        return;
    static bool adskipOn = config::GetBool("adskip", false);
    static bool lastAd = adskipOn;
    static float tgA = 0.0f;
    static float mX = 0.0f, mV = 0.0f; // пружина меню (органика + перелёт)
    {
        float tgt = g_open ? 1.0f : 0.0f;
        mV += (tgt - mX) * 200.0f * dt;
        mV *= expf(-16.0f * dt);
        mX += mV * dt;
    }
    float e = mX;
    if (e < 0.0f)
        e = 0.0f;
    if (e > 1.08f)
        e = 1.08f;

    // кнопка-пилюля (настоящий виджет — поле ввода под ней не срабатывает)
    ImGui::SetCursorPos(ImVec2(cxScreen - 55.0f - winPos.x, pillBottomScreenY - 28.0f - winPos.y));
    if (ImGui::InvisibleButton("##helpers_open", ImVec2(110, 28)))
        g_open = !g_open;
    bool hov = ImGui::IsItemHovered();

    ImVec2 p0(cxScreen - 55.0f, pillBottomScreenY - 28.0f), p1(cxScreen + 55.0f, pillBottomScreenY);
    fg->AddRectFilled(p0, p1, hov ? IM_COL32(24, 26, 36, 235) : IM_COL32(17, 19, 27, 235), 14.0f);
    fg->AddRect(p0, p1, IM_COL32(255, 255, 255, 30), 14.0f, 0, 1.0f);
    const char* cap = g_open ? "Закрыть" : "Открыть";
    ImVec2 tsz = ImGui::CalcTextSize(cap);
    float tcy = (p0.y + p1.y - tsz.y) * 0.5f;
    ImVec2 tp0(p0.x + 14.0f, (p0.y + p1.y) * 0.5f - 4.0f);
    ImU32 triCol = IM_COL32(200, 202, 210, 255);
    if (g_open)
        fg->AddTriangleFilled(ImVec2(tp0.x, tp0.y + 8.0f), ImVec2(tp0.x + 8.0f, tp0.y + 8.0f),
            ImVec2(tp0.x + 4.0f, tp0.y), triCol);
    else
        fg->AddTriangleFilled(ImVec2(tp0.x, tp0.y), ImVec2(tp0.x + 8.0f, tp0.y),
            ImVec2(tp0.x + 4.0f, tp0.y + 8.0f), triCol);
    fg->AddText(ImVec2(cxScreen - tsz.x * 0.5f + 4.0f, tcy), IM_COL32(235, 235, 240, 255), cap);

    // менюшка вниз от пилюли (внутри нижней карточки): пружина + каскад строк
    if (mX > 0.002f) {
        const float pw = 210.0f, ph = 68.0f;
        ImVec2 fullA(p0.x, p1.y + 8.0f);
        ImVec2 fullB(fullA.x + pw, fullA.y + ph);
        ImVec2 org((p0.x + p1.x) * 0.5f, p1.y);
        ImVec2 sa(org.x + (fullA.x - org.x) * e, org.y + (fullA.y - org.y) * e);
        ImVec2 sb(org.x + (fullB.x - org.x) * e, org.y + (fullB.y - org.y) * e);
        fg->AddRectFilled(ImVec2(sa.x, sa.y + 3.0f), ImVec2(sb.x, sb.y + 4.0f),
            IM_COL32(0, 0, 0, 90), 14.0f);
        fg->AddRectFilled(sa, sb, IM_COL32(17, 19, 27, 255), 14.0f);
        fg->AddRect(sa, sb, IM_COL32(255, 255, 255, 30), 14.0f, 0, 1.0f);
        fg->PushClipRect(sa, sb, true);
        float av = e;
        if (av > 1.0f)
            av = 1.0f;
        if (av < 0.0f)
            av = 0.0f;
        int a = (int)(255 * av);
        // каскад: шапка первой, строка догоняет со сдвигом и проявлением
        float headA = Smooth01(av * 2.0f);
        float rowT = (e - 0.3f) / 0.7f;
        if (rowT < 0.0f)
            rowT = 0.0f;
        if (rowT > 1.0f)
            rowT = 1.0f;
        float rowE = Smooth01(rowT);
        float rowY = sa.y + 36.0f + (1.0f - rowE) * 10.0f;
        ImGuiIO& io = ImGui::GetIO();
        ImFont* smallFo = io.Fonts->Fonts.size() > 1 ? io.Fonts->Fonts[1] : nullptr;
        if (smallFo)
            ImGui::PushFont(smallFo);
        fg->AddText(ImVec2(sa.x + 12.0f, sa.y + 10.0f),
            IM_COL32(245, 245, 248, (int)(255 * headA)), "Помощники");
        Toggle(fg, ImVec2(sb.x - 12.0f - 30.0f, rowY), adskipOn, tgA, dt, (int)(255 * rowE));
        fg->AddText(ImVec2(sa.x + 12.0f, rowY + 1.0f),
            IM_COL32(224, 226, 232, (int)(255 * rowE)), "Пропуск рекламы");
        fg->PopClipRect();
        // подсказка функции: сверху по центру меню, плавное появление/уход + слайд
        {
            static float tipA = 0.0f;
            bool rowHov = mX > 0.6f && ImGui::IsMouseHoveringRect(
                ImVec2(sa.x, rowY - 4.0f), ImVec2(sb.x, rowY + 22.0f), false);
            float tgt = rowHov ? 1.0f : 0.0f;
            float spd = (tgt > tipA ? 10.0f : 6.0f) * dt;
            if (spd > 1.0f)
                spd = 1.0f;
            tipA += (tgt - tipA) * spd;
            if (tipA > 0.01f) {
                float e2 = tipA * tipA * (3.0f - 2.0f * tipA);
                const char* l1 = "Пропуск рекламы";
                const char* l2 = "Скип кнопки + вставки, даже на фоне";
                float w1 = ImGui::CalcTextSize(l1).x;
                float w2 = ImGui::CalcTextSize(l2).x;
                float tw = w1 > w2 ? w1 : w2;
                float bw = tw + 24.0f, bh = 52.0f;
                float mcx = (sa.x + sb.x) * 0.5f;
                float by1 = sa.y - 8.0f - bh + (1.0f - e2) * 6.0f;
                ImVec2 b0(mcx - bw * 0.5f, by1), b1(mcx + bw * 0.5f, by1 + bh);
                fg->AddRectFilled(b0, b1, IM_COL32(10, 12, 20, (int)(235 * e2)), 10.0f);
                fg->AddRect(b0, b1, IM_COL32(255, 255, 255, (int)(40 * e2)), 10.0f, 0, 1.0f);
                fg->AddText(ImVec2(b0.x + 12.0f, b0.y + 8.0f),
                    IM_COL32(235, 235, 240, (int)(255 * e2)), l1);
                fg->AddText(ImVec2(b0.x + 12.0f, b0.y + 28.0f),
                    IM_COL32(150, 150, 158, (int)(255 * e2)), l2);
            }
        }
        if (smallFo)
            ImGui::PopFont();
    }

    adskip::SetEnabled(adskipOn);
    if (lastAd != adskipOn) {
        lastAd = adskipOn;
        config::SetBool("adskip", adskipOn);
    }
}

} // namespace helpers
