#pragma once
#include <d3d11.h>
#include <windows.h>
#include <stdint.h>
#include <vector>

// Блюр отдельным шейдерным ректом. Как работает:
//  1. окно прячем от захватов — в кадр попадает чистый стол без нас
//     (без этого окно снимает само себя и блюр сходится в черноту);
//  2. заливаем в текстуру, гауссиана в шейдере (3 круга H+V);
//  3. gui кладёт этот рект на всё окно, а сверху светлый + чёрный для серости.
// В углах честный блюр как у матового стекла, шва-артефакта нет.
// Не завелось — Update отдаёт nullptr и gui рисует плоскую панель.
class BlurBG {
public:
    bool Init(ID3D11Device* device, HWND hwnd);
    void* Update(ID3D11RenderTargetView* back); // SRV с блюром или nullptr
    void Shutdown();

private:
    // один проход: src -> dst, блюр вдоль (dx,dy), яркость x dim,
    // shape > 0: запечь форму сквиркла в альфу (финальный проход)
    void Blit(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
              int vw, int vh, float dx, float dy, float dim, float shape);

    ID3D11Device* dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    HWND hwnd_ = nullptr;
    int win_w_ = 0, win_h_ = 0; // размер окна
    int hw_ = 0, hh_ = 0;       // четверть (в ней блюрим)

    HDC memdc_ = nullptr;   // память под фотку стола
    HBITMAP dib_ = nullptr;
    HBITMAP old_ = nullptr;
    void* bits_ = nullptr;  // пиксели фотки (BGRA сверху вниз)

    ID3D11Texture2D* up_ = nullptr; // фотка на GPU
    ID3D11ShaderResourceView* upSrv_ = nullptr;
    ID3D11Texture2D* rtA_ = nullptr; // две мишени пинг-понг
    ID3D11RenderTargetView* rtvA_ = nullptr;
    ID3D11ShaderResourceView* srvA_ = nullptr;
    ID3D11Texture2D* rtB_ = nullptr;
    ID3D11RenderTargetView* rtvB_ = nullptr;
    ID3D11ShaderResourceView* srvB_ = nullptr; // тут итог

    ID3D11VertexShader* vs_ = nullptr;
    ID3D11PixelShader* ps_ = nullptr;
    ID3D11SamplerState* sampler_ = nullptr;
    ID3D11RasterizerState* rstate_ = nullptr;
    ID3D11Buffer* cbuf_ = nullptr;

    bool ready_ = false;
    bool have_frame_ = false;
};
