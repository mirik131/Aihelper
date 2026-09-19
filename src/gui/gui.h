#pragma once
#include "imgui.h"

// Всё что рисуется в окне — здесь.
// bg — рект с шейдерным блюром от BlurBG (или nullptr если стол
// не дался — тогда плоская панель чтобы не было чёрного экрана).

namespace gui {

void Render(ImTextureID bg);

// Форма панели (сквиркл): 2 = эллипс, больше = углы слабее.
inline constexpr float kSquircleN = 16.0f;

} // namespace gui
