#include "gui/textfade.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

namespace textfade {
namespace {

// Совместим со входом стандартного пиксельного шейдера ImGui. SV_Position уже
// в экранных пикселях, поэтому край всегда мягкий и одинаковой ширины.
const char* kPixelShader =
"Texture2D fontTex : register(t0);"
"SamplerState fontSmp : register(s0);"
"cbuffer TextFade : register(b1) { float fadeStart; float fadeEnd; float pad0; float pad1; };"
"struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR0; float2 uv : TEXCOORD0; };"
"float4 PSMain(PSIn i) : SV_Target"
"{"
"    float4 color = i.col * fontTex.Sample(fontSmp, i.uv);"
"    color *= 1.0 - smoothstep(fadeStart, fadeEnd, i.pos.x);"
"    return color;"
"}";

struct FadeCB { float start, end, pad0, pad1; };
ID3D11PixelShader* g_ps = nullptr;
ID3D11Buffer* g_cb = nullptr;
float g_start = 0.0f;
float g_end = 0.0f;

void BindShader(const ImDrawList*, const ImDrawCmd*)
{
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(
        ImGui::GetPlatformIO().Renderer_RenderState);
    if (!state || !g_ps || !g_cb)
        return;
    const FadeCB data = { g_start, g_end, 0.0f, 0.0f };
    state->DeviceContext->UpdateSubresource(g_cb, 0, nullptr, &data, 0, 0);
    state->DeviceContext->PSSetConstantBuffers(1, 1, &g_cb);
    state->DeviceContext->PSSetShader(g_ps, nullptr, 0);
}

} // namespace

void Init(ID3D11Device* device)
{
    Shutdown();
    if (!device)
        return;
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(kPixelShader, strlen(kPixelShader), nullptr, nullptr, nullptr,
        "PSMain", "ps_4_0", flags, 0, &code, &errors);
    if (FAILED(hr)) {
        if (errors)
            std::printf("text fade shader: %s\n", static_cast<const char*>(errors->GetBufferPointer()));
        if (errors)
            errors->Release();
        return;
    }
    if (errors)
        errors->Release();
    if (FAILED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_ps))) {
        code->Release();
        Shutdown();
        return;
    }
    code->Release();
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(FadeCB);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&desc, nullptr, &g_cb)))
        Shutdown();
}

void Shutdown()
{
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
}

void AddText(ImDrawList* dl, const ImVec2& pos, ImU32 color, const char* text,
             float fadeStart, float fadeEnd)
{
    if (!dl || !text || !g_ps || !g_cb || fadeEnd <= fadeStart) {
        if (dl)
            dl->AddText(pos, color, text);
        return;
    }
    g_start = fadeStart;
    g_end = fadeEnd;
    dl->AddCallback(BindShader);
    dl->AddText(pos, color, text);
    dl->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState);
}

} // namespace textfade
