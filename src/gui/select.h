#pragma once
#include <d3d11.h>

// Подсветка выбранного пункта шейдером: полоса света пробегает слева
// направо (~0.6 сек), потом остаётся мягкое свечение.
// Init/Shutdown дёргает main, Frame — каждый кадр для выбранного пункта.
// progress: -0.3 (слева за краем) .. 1.3 (ушла вправо), дальше статика.

namespace selglow {

bool Init(ID3D11Device* device);
void Shutdown();
void* Frame(float progress, float base); // SRV с полосой или nullptr

} // namespace selglow
