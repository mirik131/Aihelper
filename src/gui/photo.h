#pragma once
#include <d3d11.h>

// Фото в правом верхнем углу. Грузим через WIC — ему всё равно что
// внутри (jpg/png/webp): tin.png на деле webp с чужим расширением.
// Init ищет tin.png, иначе fallback.jpg. Get — SRV, Size — размер.

namespace photo {

bool Init(ID3D11Device* device);
void Shutdown();
void* Get();
void Size(int* w, int* h);
void* LoadIcon(const char* path); // иконка с кэшем (null если не далась)

} // namespace photo
