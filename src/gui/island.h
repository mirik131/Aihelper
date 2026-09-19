#pragma once
#include "imgui.h"

// Динамический остров сверху-центр панели:
// музыка — обложка + название + эквалайзер + прогресс,
// без музыки — slim-пилюля AiHelper.
// Рисует в переданный draw-list (звать с foreground — поверх всего), кликов не ест.
namespace island {

void Render(ImDrawList* dl, ImVec2 panelL, ImVec2 panelR, float dt);

} // namespace island
