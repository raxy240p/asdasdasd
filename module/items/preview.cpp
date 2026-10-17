// module/items/preview.cpp — static icon fallback + live DX11 panorama.
//
// LivePreviewImpl: an off-screen 1-pass render target. Each DrawPanorama call
// rebinds a textured quad with a Y-axis rotation driven by GetTickCount64,
// renders into an ID3D11Texture2D, exposes the SRV to ImGui::Image.
// No new threads, no VEH, no VMT; everything is standard D3D11 calls against
// the game's already-adopted device.
#include "preview.hpp"
#include "../render/icon_cache.hpp"
#include "../render/dx11_setup.hpp"

#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace cheat::items {

namespace {

// ---- shared math (column-major DX layout) ----
struct Mat4 { float m[16]; };

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i*4+j] =
                a.m[i*4+0]*b.m[0*4+j] +
                a.m[i*4+1]*b.m[1*4+j] +
                a.m[i*4+2]*b.m[2*4+j] +
                a.m[i*4+3]*b.m[3*4+j];
    return r;
}
Mat4 RotY(float a) {
    const float c = std::cos(a), s = std::sin(a);
    return { c, 0, s, 0,  0, 1, 0, 0,  -s, 0, c, 0,  0, 0, 0, 1 };
}
Mat4 Translate(float x, float y, float z) {
    return { 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 };
}
Mat4 Perspective(float fovY, float aspect, float znear, float zfar) {
    const float f = 1.0f / std::tan(fovY * 0.5f);
    Mat4 r{};
    r.m[0]  = f / aspect;
    r.m[5]  = f;
    r.m[10] = zfar / (zfar - znear);
    r.m[11] = 1.0f;
    r.m[14] = -znear * zfar / (zfar - znear);
    return r;
}

const char* kVS = R"(
// row_major: our C++ Mat4 is laid out row-first; HLSL defaults to column-major
// so we annotate here instead of transposing on upload.
cbuffer CB : register(b0) { row_major float4x4 mvp; }
struct V2P { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
V2P main(float3 p : POSITION, float2 uv : TEXCOORD) {
    V2P o;
    o.pos = mul(float4(p,1), mvp);
    o.uv  = uv;
    return o;
}
)";

const char* kPS = R"(
Texture2D    tex0  : register(t0);
SamplerState smp0  : register(s0);
struct V2P { float4 pos : SV_POSITION; float2 uv : TEXCOORD; };
float4 main(V2P i) : SV_TARGET {
    float4 c = tex0.Sample(smp0, i.uv);
    // tilt-shade: slight dim toward the vertical edges to sell the 3D plane
    float edge = 1.0 - abs(i.uv.x - 0.5) * 0.4;
    return float4(c.rgb * edge, c.a);
}
)";

struct Quad {
    float px, py, pz;
    float u,  v;
};
constexpr Quad kQuad[6] = {
    {-0.75f,  1.0f, 0, 0, 0},
    { 0.75f,  1.0f, 0, 1, 0},
    { 0.75f, -1.0f, 0, 1, 1},
    {-0.75f,  1.0f, 0, 0, 0},
    { 0.75f, -1.0f, 0, 1, 1},
    {-0.75f, -1.0f, 0, 0, 1},
};

class LivePreviewImpl final : public IItemPreview {
public:
    explicit LivePreviewImpl(const std::filesystem::path& iconDir)
        : cache_(iconDir) {}
    ~LivePreviewImpl() { Release(); }

    void DrawIcon(const ItemConfig& item, ImVec2 size) override {
        auto* dev = cheat::render::GameDevice();
        if (dev) cache_.Bind(dev);
        char key[64];
        std::snprintf(key, sizeof(key), "%u_%d", item.definitionIndex, item.paintKit);
        // Scope ImGui IDs by instance address so duplicate def/kit pairs
        // (same skin twice in inventory) don't collide.
        ImGui::PushID(&item);
        if (auto* tex = cache_.Get(key))
            ImGui::Image((ImTextureID)(intptr_t)tex, size);
        else {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, 0xFF333333);
            ImGui::BeginChild(key, size, true, ImGuiWindowFlags_NoScrollbar);
            ImGui::TextDisabled("no icon\n%u/%d", item.definitionIndex, item.paintKit);
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    void DrawPanorama(const ItemConfig& item, ImVec2 size) override {
        auto* dev = cheat::render::GameDevice();
        auto* ctx = cheat::render::GameContext();
        if (!dev || !ctx) { DrawIcon(item, size); return; }

        if (!EnsureInit(dev)) { DrawIcon(item, size); return; }
        cache_.Bind(dev);

        if (!EnsureRT(dev, (UINT)size.x, (UINT)size.y)) { DrawIcon(item, size); return; }

        char key[64];
        std::snprintf(key, sizeof(key), "%u_%d", item.definitionIndex, item.paintKit);
        auto* srv = cache_.Get(key);
        if (!srv) { DrawIcon(item, size); return; }

        RenderPass(ctx, srv, size);
        ImGui::Image((ImTextureID)(intptr_t)rtSrv_, size);
    }

private:
    cheat::render::IconCache cache_;

    ID3D11VertexShader* vs_  = nullptr;
    ID3D11PixelShader*  ps_  = nullptr;
    ID3D11InputLayout*  il_  = nullptr;
    ID3D11Buffer*       vb_  = nullptr;
    ID3D11Buffer*       cb_  = nullptr;
    ID3D11SamplerState* smp_ = nullptr;
    ID3D11RasterizerState*  rs_  = nullptr;
    ID3D11DepthStencilState* ds_ = nullptr;
    ID3D11BlendState*       bs_ = nullptr;

    ID3D11Texture2D*          rt_    = nullptr;
    ID3D11RenderTargetView*   rtRtv_ = nullptr;
    ID3D11ShaderResourceView* rtSrv_ = nullptr;
    UINT rtW_ = 0, rtH_ = 0;

    bool inited_ = false;

    bool EnsureInit(ID3D11Device* dev) {
        if (inited_) return true;

        ID3DBlob* bytecode = nullptr;
        ID3DBlob* err = nullptr;
        if (FAILED(D3DCompile(kVS, std::strlen(kVS), nullptr, nullptr, nullptr,
                              "main", "vs_4_0", 0, 0, &bytecode, &err))) {
            if (err) err->Release();
            return false;
        }
        if (FAILED(dev->CreateVertexShader(bytecode->GetBufferPointer(),
                                           bytecode->GetBufferSize(), nullptr, &vs_))) {
            bytecode->Release(); return false;
        }
        D3D11_INPUT_ELEMENT_DESC ied[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        dev->CreateInputLayout(ied, 2, bytecode->GetBufferPointer(),
                               bytecode->GetBufferSize(), &il_);
        bytecode->Release();

        if (FAILED(D3DCompile(kPS, std::strlen(kPS), nullptr, nullptr, nullptr,
                              "main", "ps_4_0", 0, 0, &bytecode, &err))) {
            if (err) err->Release();
            return false;
        }
        dev->CreatePixelShader(bytecode->GetBufferPointer(),
                               bytecode->GetBufferSize(), nullptr, &ps_);
        bytecode->Release();

        D3D11_BUFFER_DESC vbd{ sizeof(kQuad), D3D11_USAGE_IMMUTABLE,
                               D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sr{ kQuad, 0, 0 };
        dev->CreateBuffer(&vbd, &sr, &vb_);

        D3D11_BUFFER_DESC cbd{ sizeof(Mat4), D3D11_USAGE_DYNAMIC,
                               D3D11_BIND_CONSTANT_BUFFER,
                               D3D11_CPU_ACCESS_WRITE, 0, 0 };
        dev->CreateBuffer(&cbd, nullptr, &cb_);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        // ComparisonFunc only applies to comparison filters; omitted here.
        dev->CreateSamplerState(&sd, &smp_);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        dev->CreateRasterizerState(&rd, &rs_);

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dev->CreateDepthStencilState(&dsd, &ds_);

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable           = TRUE;
        bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev->CreateBlendState(&bd, &bs_);

        inited_ = (vs_ && ps_ && il_ && vb_ && cb_ && smp_);
        return inited_;
    }

    bool EnsureRT(ID3D11Device* dev, UINT w, UINT h) {
        if (w == 0 || h == 0) return false;
        if (rt_ && rtW_ == w && rtH_ == h) return true;
        if (rtRtv_) { rtRtv_->Release(); rtRtv_ = nullptr; }
        if (rtSrv_) { rtSrv_->Release(); rtSrv_ = nullptr; }
        if (rt_)    { rt_->Release();    rt_    = nullptr; }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &rt_))) return false;
        if (FAILED(dev->CreateRenderTargetView(rt_, nullptr, &rtRtv_))) return false;
        if (FAILED(dev->CreateShaderResourceView(rt_, nullptr, &rtSrv_))) return false;
        rtW_ = w; rtH_ = h;
        return true;
    }

    void RenderPass(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* iconSrv,
                    ImVec2 size)
    {
        const float t = static_cast<float>(GetTickCount64()) * 0.001f;
        const float angle = std::fmod(t * 0.4f, 6.2831853f);

        Mat4 proj  = Perspective(0.8f, size.x / (size.y + 0.0001f), 0.1f, 100.f);
        Mat4 view  = Translate(0, 0, 3.0f);
        Mat4 world = RotY(angle);
        Mat4 mvp   = Mul(world, Mul(view, proj));

        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(ctx->Map(cb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &mvp, sizeof(mvp));
            ctx->Unmap(cb_, 0);
        }

        // Capture the full set of pipeline state we touch, so neither the
        // next ImGui draw nor the game's own rendering sees our overrides.
        // Each Get* AddRefs — we Release in Restore().
        struct Saved {
            ID3D11InputLayout*        il     = nullptr;
            D3D11_PRIMITIVE_TOPOLOGY  topo   = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            ID3D11Buffer*             vb     = nullptr;
            UINT                      stride = 0;
            UINT                      offset = 0;
            ID3D11VertexShader*       vs     = nullptr;
            ID3D11Buffer*             vscb   = nullptr;
            ID3D11PixelShader*        ps     = nullptr;
            ID3D11SamplerState*       pssmp  = nullptr;
            ID3D11ShaderResourceView* pssrv  = nullptr;
            ID3D11RasterizerState*    rs     = nullptr;
            ID3D11DepthStencilState*  ds     = nullptr;
            UINT                      sref   = 0;
            ID3D11BlendState*         bs     = nullptr;
            float                     bf[4]  = {0,0,0,0};
            UINT                      bmask  = 0;
            ID3D11RenderTargetView*   rtv    = nullptr;
            ID3D11DepthStencilView*   dsv    = nullptr;
            D3D11_VIEWPORT            vp[8]  {};
            UINT                      nVp    = 8;
        } s;

        ctx->IAGetInputLayout(&s.il);
        ctx->IAGetPrimitiveTopology(&s.topo);
        ctx->IAGetVertexBuffers(0, 1, &s.vb, &s.stride, &s.offset);
        ctx->VSGetShader(&s.vs, nullptr, nullptr);
        ctx->VSGetConstantBuffers(0, 1, &s.vscb);
        ctx->PSGetShader(&s.ps, nullptr, nullptr);
        ctx->PSGetSamplers(0, 1, &s.pssmp);
        ctx->PSGetShaderResources(0, 1, &s.pssrv);
        ctx->RSGetState(&s.rs);
        ctx->OMGetDepthStencilState(&s.ds, &s.sref);
        ctx->OMGetBlendState(&s.bs, s.bf, &s.bmask);
        ctx->OMGetRenderTargets(1, &s.rtv, &s.dsv);
        ctx->RSGetViewports(&s.nVp, s.vp);

        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->ClearRenderTargetView(rtRtv_, clear);
        ctx->OMSetRenderTargets(1, &rtRtv_, nullptr);

        D3D11_VIEWPORT vp{};
        vp.Width = size.x; vp.Height = size.y; vp.MaxDepth = 1.f;
        ctx->RSSetViewports(1, &vp);

        UINT stride = sizeof(Quad), offset = 0;
        ctx->IASetInputLayout(il_);
        ctx->IASetVertexBuffers(0, 1, &vb_, &stride, &offset);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs_, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &cb_);
        ctx->PSSetShader(ps_, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &smp_);
        ctx->PSSetShaderResources(0, 1, &iconSrv);
        ctx->RSSetState(rs_);
        ctx->OMSetDepthStencilState(ds_, 0);
        const float blendF[4] = {0,0,0,0};
        ctx->OMSetBlendState(bs_, blendF, 0xFFFFFFFF);

        ctx->Draw(6, 0);

        // Restore. Put each slot back exactly as captured, then drop refs.
        ctx->IASetInputLayout(s.il);
        ctx->IASetPrimitiveTopology(s.topo);
        ctx->IASetVertexBuffers(0, 1, &s.vb, &s.stride, &s.offset);
        ctx->VSSetShader(s.vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &s.vscb);
        ctx->PSSetShader(s.ps, nullptr, 0);
        ctx->PSSetSamplers(0, 1, &s.pssmp);
        ctx->PSSetShaderResources(0, 1, &s.pssrv);
        ctx->RSSetState(s.rs);
        ctx->OMSetDepthStencilState(s.ds, s.sref);
        ctx->OMSetBlendState(s.bs, s.bf, s.bmask);
        ctx->OMSetRenderTargets(1, &s.rtv, s.dsv);
        ctx->RSSetViewports(s.nVp, s.vp);

        if (s.il)    s.il->Release();
        if (s.vb)    s.vb->Release();
        if (s.vs)    s.vs->Release();
        if (s.vscb)  s.vscb->Release();
        if (s.ps)    s.ps->Release();
        if (s.pssmp) s.pssmp->Release();
        if (s.pssrv) s.pssrv->Release();
        if (s.rs)    s.rs->Release();
        if (s.ds)    s.ds->Release();
        if (s.bs)    s.bs->Release();
        if (s.rtv)   s.rtv->Release();
        if (s.dsv)   s.dsv->Release();
    }

    void Release() {
        if (rtSrv_) rtSrv_->Release();
        if (rtRtv_) rtRtv_->Release();
        if (rt_)    rt_->Release();
        if (bs_)    bs_->Release();
        if (ds_)    ds_->Release();
        if (rs_)    rs_->Release();
        if (smp_)   smp_->Release();
        if (cb_)    cb_->Release();
        if (vb_)    vb_->Release();
        if (il_)    il_->Release();
        if (ps_)    ps_->Release();
        if (vs_)    vs_->Release();
    }
};

} // namespace

std::unique_ptr<IItemPreview> MakeIconPreview(const std::filesystem::path& iconDir) {
    return std::unique_ptr<IItemPreview>(new LivePreviewImpl(iconDir));
}

} // namespace cheat::items
