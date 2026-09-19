#include "gui/select.h"
#include <d3dcompiler.h>
#include <cstring>
#include <cstdio>

namespace selglow {
namespace {

// Треугольник на весь экран без буферов (только SV_VertexID).
const char* kVS =
"struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };"
"VSOut VSMain(uint id : SV_VertexID)"
"{"
"    VSOut o;"
"    o.uv = float2((id << 1) & 2, id & 2);"
"    o.pos = float4(o.uv.x * 2.0 - 1.0, 1.0 - o.uv.y * 2.0, 0.0, 1.0);"
"    return o;"
"};";

// Полоса света + мягкая подложка. Текстуры не надо — всё из uv.
const char* kPS =
"cbuffer GlowCB : register(b0) { float prog; float width; float soft; float base; };"
"float4 PSMain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target"
"{"
"    float d = abs(uv.x - prog);"
"    float band = 1.0 - smoothstep(width * 0.5 - soft, width * 0.5 + soft, d);"
"    float a = base + band * 0.45;"
"    float2 pp = uv - 0.5;"              // скругление подложки (текстура 160x48)
"    pp.x *= 3.3333;"
"    float rr = 0.21;"                   // ~10px как у самого пункта
"    float2 qq = abs(pp) - float2(3.3333 * 0.5 - rr, 0.5 - rr);"
"    float dd = length(max(qq, 0.0)) + min(max(qq.x, qq.y), 0.0) - rr;"
"    float aa = fwidth(dd) * 1.5 + 1e-4;"
"    a *= 1.0 - smoothstep(-aa, aa, dd);"
"    return float4(1.0, 1.0, 1.0, a);"
"};";

struct GlowCB {
    float prog, width, soft, base;
};

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_srv = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11RasterizerState* g_rs = nullptr;
ID3D11Buffer* g_cb = nullptr;
bool g_ok = false;
float g_lastProg = -1000.0f;

bool Compile(const char* src, const char* entry, const char* target, ID3DBlob** out)
{
    ID3DBlob* err = nullptr;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, flags, 0, out, &err);
    if (FAILED(hr) && err)
        printf("select shader %s: %s\n", entry, (const char*)err->GetBufferPointer());
    if (err)
        err->Release();
    return SUCCEEDED(hr);
}

} // namespace

bool Init(ID3D11Device* device)
{
    Shutdown();
    g_dev = device;
    g_dev->GetImmediateContext(&g_ctx);

    D3D11_TEXTURE2D_DESC rd = {};
    rd.Width = 160;
    rd.Height = 48;
    rd.MipLevels = 1;
    rd.ArraySize = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.SampleDesc.Count = 1;
    rd.Usage = D3D11_USAGE_DEFAULT;
    rd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&rd, nullptr, &g_rt))) {
        Shutdown();
        return false;
    }
    if (FAILED(g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv))) {
        Shutdown();
        return false;
    }
    if (FAILED(g_dev->CreateShaderResourceView(g_rt, nullptr, &g_srv))) {
        Shutdown();
        return false;
    }

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    if (!Compile(kVS, "VSMain", "vs_4_0", &vsb)) {
        Shutdown();
        return false;
    }
    if (FAILED(g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs))) {
        vsb->Release();
        Shutdown();
        return false;
    }
    vsb->Release();
    if (!Compile(kPS, "PSMain", "ps_4_0", &psb)) {
        Shutdown();
        return false;
    }
    if (FAILED(g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps))) {
        psb->Release();
        Shutdown();
        return false;
    }
    psb->Release();

    D3D11_RASTERIZER_DESC rz = {};
    rz.FillMode = D3D11_FILL_SOLID;
    rz.CullMode = D3D11_CULL_NONE;
    if (FAILED(g_dev->CreateRasterizerState(&rz, &g_rs))) {
        Shutdown();
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(GlowCB);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb))) {
        Shutdown();
        return false;
    }

    g_ok = true;
    return true;
}

void* Frame(float progress, float base)
{
    if (!g_ok)
        return nullptr;
    if (progress == g_lastProg)
        return g_srv; // статика — не пересчитываем
    g_lastProg = progress;

    // запоминаем чужое состояние, вернём после себя
    ID3D11RenderTargetView* savedRT = nullptr;
    ID3D11DepthStencilView* savedDS = nullptr;
    g_ctx->OMGetRenderTargets(1, &savedRT, &savedDS);
    D3D11_VIEWPORT savedVp = {};
    UINT nvp = 1;
    g_ctx->RSGetViewports(&nvp, &savedVp);

    D3D11_VIEWPORT vp = {};
    vp.Width = 160.0f;
    vp.Height = 48.0f;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0);
    g_ctx->PSSetShader(g_ps, nullptr, 0);
    GlowCB cb = { progress, 0.35f, 0.12f, base };
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->RSSetState(g_rs);
    g_ctx->Draw(3, 0);

    g_ctx->OMSetRenderTargets(1, &savedRT, savedDS);
    g_ctx->RSSetViewports(1, &savedVp);
    if (savedRT)
        savedRT->Release();
    if (savedDS)
        savedDS->Release();
    return g_srv;
}

void Shutdown()
{
    if (g_rt) { g_rt->Release(); g_rt = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_srv) { g_srv->Release(); g_srv = nullptr; }
    if (g_vs) { g_vs->Release(); g_vs = nullptr; }
    if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    if (g_rs) { g_rs->Release(); g_rs = nullptr; }
    if (g_cb) { g_cb->Release(); g_cb = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    g_ok = false;
    g_lastProg = -1000.0f;
}

} // namespace selglow
