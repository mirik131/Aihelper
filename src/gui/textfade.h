#pragma once
#include "imgui.h"

struct ID3D11Device;

// Пиксельное растворение правого края текста. Используется и в острове,
// и в карточке пользователя, чтобы строки не обрывались и не вылезали.
namespace textfade {

void Init(ID3D11Device* device);
void Shutdown();

// fadeStart/fadeEnd — экранные X-координаты начала и конца растушёвки.
void AddText(ImDrawList* dl, const ImVec2& pos, ImU32 color, const char* text,
             float fadeStart, float fadeEnd);

} // namespace textfade
