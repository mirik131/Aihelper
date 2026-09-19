#pragma once
#include "imgui.h"

// Пункт бокового меню: хит-тест через невидимую кнопку,
// рисуем сами — подсветка наведения, нажатие, выбранный.
// Возвращает true в кадр клика (как обычный Button).

namespace widgets {

// id — номер пункта (для уникальности), label — текст,
// size — размер, hover01 — подсветка 0..1 (из anim::Fade),
// selected — выбран ли сейчас.
inline bool MenuItem(int id, const char* label, const ImVec2& size, float hover01, bool selected)
{
    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("item", size);
    bool held = ImGui::IsItemActive(); // держим кнопку
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();

    float glow = hover01 * 0.14f + (held ? 0.12f : 0.0f) + (selected ? 0.10f : 0.0f);
    if (glow > 0.01f)
        dl->AddRectFilled(a, b, IM_COL32(255, 255, 255, (int)(glow * 255)), 10.0f);

    if (selected) {
        // полоска-индикатор слева (приглушена чтобы не кричала)
        dl->AddRectFilled(ImVec2(a.x + 4, a.y + 10), ImVec2(a.x + 7, b.y - 10),
            IM_COL32(255, 255, 255, 120), 2.0f);
    }

    ImVec2 ts = ImGui::CalcTextSize(label);
    // обычный тусклее на 10%, выбранный — яркий белый
    ImU32 textCol = selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(216, 216, 220, 255);
    dl->AddText(ImVec2(a.x + 16, a.y + (size.y - ts.y) * 0.5f), textCol, label);

    ImGui::PopID();
    return clicked;
}

// Тумблер-заглушка: дорожка + кружок. anim01 — положение кружка 0..1
// (гони через Fade для плавности), on — включён ли (цвет дорожки).
// Возвращает true в кадр клика. pos задан в экранных координатах, поэтому
// элемент корректно ловит клик и в оверлеях, и при сдвинутом окне.
inline bool Toggle(int id, const ImVec2& pos, float anim01, bool on)
{
    const float w = 38.0f, h = 22.0f;
    ImGui::PushID(id);
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton("tg", ImVec2(w, h));
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImVec2(a.x + w, a.y + h);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImU32 track = on ? IM_COL32(90, 140, 255, 255) : IM_COL32(255, 255, 255, 40);
    dl->AddRectFilled(a, b, track, h * 0.5f);
    float kx = a.x + 3 + (w - 6 - (h - 6)) * anim01;
    dl->AddCircleFilled(ImVec2(kx + (h - 6) * 0.5f, a.y + h * 0.5f), (h - 6) * 0.5f,
        IM_COL32(245, 245, 248, 255));

    ImGui::PopID();
    return clicked;
}

} // namespace widgets
