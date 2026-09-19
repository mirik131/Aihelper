#pragma once
#include "imgui.h"

// Нижняя пилюля «Открыть» + всплывающее меню помощников (пока: пропуск рекламы).
// Рисует само на foreground (поверх всего). Кнопка — настоящий InvisibleButton
// (чтобы не конфликтовать с полем ввода), панель и тогл — ручным хит-тестом.
namespace helpers {

void Render(ImDrawList* fg, float cxScreen, float pillBottomScreenY, ImVec2 winPos, float dt);
void ToggleOpen();

} // namespace helpers
