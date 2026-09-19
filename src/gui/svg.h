#pragma once
struct ID3D11Device;

// Точный рендер SVG через Direct2D (ID2D1SvgDocument) в SRV.
// WIC SVG не декодирует, поэтому вектор грузим так. Кэш как у photo.
namespace svg {

bool Init(ID3D11Device* dev);
void Shutdown();
void* Load(const char* path, int px = 24); // px — сторона в пикселях, null тоже кэшируем

} // namespace svg
