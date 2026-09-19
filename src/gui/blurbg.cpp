#include "gui/blurbg.h"
#include "gui/gui.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>

// Треугольник на весь экран без буферов (только SV_VertexID).
static const char* kVS =
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
"VSOut VSMain(uint id : SV_VertexID)"
"{"
"    VSOut o;"
"    o.uv = float2((id << 1) & 2, id & 2);"
"    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);"
"    return o;"
"};";

// Гауссиана 9 точек за 5 выборок + опциональная форма в альфе.
// shape = 0: обычная заливка. shape = n: сквиркл (та же формула что
// рисует панель), край сглажен через fwidth — без лесенок.
static const char* kPS =
"Texture2D srcTex : register(t0);"
"SamplerState srcSmp : register(s0);"
"cbuffer BlurCB : register(b0) { float2 stepDir; float dim; float shapeN; };"
"float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target"
"{"
"    float3 acc = srcTex.Sample(srcSmp, uv).rgb * 0.2270270270;"
"    float2 o1 = stepDir * 1.3846153846;"
"    float2 o2 = stepDir * 3.2307692308;"
"    acc += srcTex.Sample(srcSmp, uv + o1).rgb * 0.3162162162;"
"    acc += srcTex.Sample(srcSmp, uv - o1).rgb * 0.3162162162;"
"    acc += srcTex.Sample(srcSmp, uv + o2).rgb * 0.0702702703;"
"    acc += srcTex.Sample(srcSmp, uv - o2).rgb * 0.0702702703;"
"    float alpha = 1.0;"
"    if (shapeN > 0.5)"
"    {"
"        float2 p = uv * 2.0 - 1.0;"
"        float d = pow(pow(abs(p.x), shapeN) + pow(abs(p.y), shapeN), 1.0 / shapeN);"
"        float aa = fwidth(d) * 1.5 + 1e-4;"
"        alpha = (1.0 - smoothstep(1.0 - aa, 1.0 + aa, d)) * 0.9f;"
"    }"
"    return float4(acc * dim, alpha);"
"};";

struct BlurCB {
    float dx, dy, dim, shape;
};

static bool Compile(const char* src, const char* entry, const char* target, ID3DBlob** out)
{
    ID3DBlob* err = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, flags, 0, out, &err);
    if (FAILED(hr) && err)
        printf("shader %s: %s\n", entry, (const char*)err->GetBufferPointer());
    if (err)
        err->Release();
    return SUCCEEDED(hr);
}

bool BlurBG::Init(ID3D11Device* device, HWND hwnd)
{
    Shutdown();
    dev_ = device;
    hwnd_ = hwnd;
    dev_->GetImmediateContext(&ctx_);

    RECT wr;
    GetWindowRect(hwnd, &wr);
    win_w_ = wr.right - wr.left;
    win_h_ = wr.bottom - wr.top;
    if (win_w_ < 8 || win_h_ < 8)
        return false;
    hw_ = (win_w_ + 3) / 4;
    hh_ = (win_h_ + 3) / 4;

    // нас нет в захватах — снимаем чистый стол без самих себя
    SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = win_w_;
    bi.bmiHeader.biHeight = -win_h_; // сверху вниз
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    memdc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!memdc_)
        return false;
    dib_ = CreateDIBSection(memdc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_ || !bits_) {
        Shutdown();
        return false;
    }
    old_ = (HBITMAP)SelectObject(memdc_, dib_); // без этого BitBlt рисует мимо фотки

    // фотка на GPU
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = win_w_;
    td.Height = win_h_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev_->CreateTexture2D(&td, nullptr, &up_))) {
        Shutdown();
        return false;
    }
    // явный дескриптор вида текстуры — нулевые мипы и прочее не отдаём на откуп
    D3D11_SHADER_RESOURCE_VIEW_DESC svd = {};
    svd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    svd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    svd.Texture2D.MipLevels = 1;
    if (FAILED(dev_->CreateShaderResourceView(up_, &svd, &upSrv_))) {
        Shutdown();
        return false;
    }

    // две мишени пинг-понг под блюр
    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = hw_;
    rd.Height = hh_;
    rd.MipLevels = 1;
    rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev_->CreateTexture2D(&rd, nullptr, &rtA_))) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreateRenderTargetView(rtA_, nullptr, &rtvA_))) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreateShaderResourceView(rtA_, &svd, &srvA_))) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreateTexture2D(&rd, nullptr, &rtB_))) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreateRenderTargetView(rtB_, nullptr, &rtvB_))) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreateShaderResourceView(rtB_, &svd, &srvB_))) {
        Shutdown();
        return false;
    }

    // шейдеры
    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    if (!Compile(kVS, "VSMain", "vs_4_0", &vsb)) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_))) {
        vsb->Release();
        Shutdown();
        return false;
    }
    vsb->Release();
    if (!Compile(kPS, "PSMain", "ps_4_0", &psb)) {
        Shutdown();
        return false;
    }
    if (FAILED(dev_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_))) {
        psb->Release();
        Shutdown();
        return false;
    }
    psb->Release();

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = 0.0f;
    sd.MaxAnisotropy = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.0f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev_->CreateSamplerState(&sd, &sampler_))) {
        Shutdown();
        return false;
    }

    D3D11_RASTERIZER_DESC rz = {};
    rz.FillMode = D3D11_FILL_SOLID;
    rz.CullMode = D3D11_CULL_NONE; // треугольник виден с любой стороны
    if (FAILED(dev_->CreateRasterizerState(&rz, &rstate_))) {
        Shutdown();
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(BlurCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cbuf_))) {
        Shutdown();
        return false;
    }

    ready_ = true;
    printf("blur shaders ok\n");
    return true;
}

void* BlurBG::Update(ID3D11RenderTargetView* back)
{
    if (!ready_)
        return nullptr;
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    HDC screen = GetDC(nullptr);
    BOOL ok = BitBlt(memdc_, 0, 0, win_w_, win_h_, screen, wr.left, wr.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (!ok)
        return have_frame_ ? srvB_ : nullptr;
    have_frame_ = true;

    // фотка на GPU
    D3D11_MAPPED_SUBRESOURCE mu = {};
    HRESULT mapHr = ctx_->Map(up_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mu);
    if (FAILED(mapHr))
        return srvB_; // отдаём прошлый блюр
    // DIB хранит BGRA, текстуре нужен RGBA — меняем R и B местами.
    // Без этого тёплые обои синеют.
    uint8_t* src = (uint8_t*)bits_;
    for (int y = 0; y < win_h_; y++) {
        uint8_t* d = (uint8_t*)mu.pData + y * mu.RowPitch;
        uint8_t* s = src + y * win_w_ * 4;
        for (int x = 0; x < win_w_; x++) {
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255;
            d += 4; s += 4;
        }
    }
    ctx_->Unmap(up_, 0);

    // три круга гауссианы с даунсемплом: итог в B
    float fx = 1.0f / win_w_, fy = 1.0f / win_h_;
    float qx = 1.0f / hw_, qy = 1.0f / hh_;
    Blit(upSrv_, rtvA_, hw_, hh_, 2.0f * fx, 0.0f, 1.0f, 0.0f);
    Blit(srvA_, rtvB_, hw_, hh_, 0.0f, 2.0f * fy, 1.0f, 0.0f);
    Blit(srvB_, rtvA_, hw_, hh_, 1.5f * qx, 0.0f, 1.0f, 0.0f);
    Blit(srvA_, rtvB_, hw_, hh_, 0.0f, 1.5f * qy, 1.0f, 0.0f);
    Blit(srvB_, rtvA_, hw_, hh_, 1.5f * qx, 0.0f, 1.0f, 0.0f);
    Blit(srvA_, rtvB_, hw_, hh_, 0.0f, 1.5f * qy, 0.55f, gui::kSquircleN); // финал: форма+темнота

    // возвращаем контекст окну
    D3D11_VIEWPORT vw = {};
    vw.Width = (float)win_w_;
    vw.Height = (float)win_h_;
    vw.MinDepth = 0.0f;
    vw.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vw);
    ctx_->OMSetRenderTargets(1, &back, nullptr);
    return srvB_;
}

void BlurBG::Blit(ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
                  int vw, int vh, float dx, float dy, float dim, float shape)
{
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)vw;
    vp.Height = (float)vh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, &dst, nullptr);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    BlurCB cb = { dx, dy, dim, shape };
    ctx_->UpdateSubresource(cbuf_, 0, nullptr, &cb, 0, 0);
    ctx_->PSSetConstantBuffers(0, 1, &cbuf_);
    ctx_->PSSetShaderResources(0, 1, &src);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->RSSetState(rstate_);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nullsrv[1] = { nullptr };
    ctx_->PSSetShaderResources(0, 1, nullsrv);
}

void BlurBG::Shutdown()
{
    if (hwnd_)
        SetWindowDisplayAffinity(hwnd_, WDA_NONE); // возвращаем как было
    if (memdc_) {
        if (old_)
            SelectObject(memdc_, old_);
        DeleteDC(memdc_);
        memdc_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    old_ = nullptr;
    if (up_) { up_->Release(); up_ = nullptr; }
    if (upSrv_) { upSrv_->Release(); upSrv_ = nullptr; }
    if (rtA_) { rtA_->Release(); rtA_ = nullptr; }
    if (rtvA_) { rtvA_->Release(); rtvA_ = nullptr; }
    if (srvA_) { srvA_->Release(); srvA_ = nullptr; }
    if (rtB_) { rtB_->Release(); rtB_ = nullptr; }
    if (rtvB_) { rtvB_->Release(); rtvB_ = nullptr; }
    if (srvB_) { srvB_->Release(); srvB_ = nullptr; }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (ps_) { ps_->Release(); ps_ = nullptr; }
    if (sampler_) { sampler_->Release(); sampler_ = nullptr; }
    if (rstate_) { rstate_->Release(); rstate_ = nullptr; }
    if (cbuf_) { cbuf_->Release(); cbuf_ = nullptr; }
    if (ctx_) { ctx_->Release(); ctx_ = nullptr; }
    ready_ = false;
    have_frame_ = false;
}
