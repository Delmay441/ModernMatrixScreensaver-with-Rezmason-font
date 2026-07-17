#include "renderer.h"
#include "log.h"
#include <DirectXMath.h>
#include <dxgi1_6.h>
#include <wincodec.h>
#include <string>
#include <cstring>
#include <cstdio>
#include <cmath>
#include "shaders_blob.h"
#include <vector>
#include <algorithm>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

static UINT maxu(UINT a, UINT b) { return a > b ? a : b; }

static UINT ComputePresentInterval(bool batterySaver)
{
    if (!batterySaver) return 1;
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    UINT hz = 60;
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
        hz = dm.dmDisplayFrequency;
    UINT interval = (hz + 59) / 60;   // round up so effective fps stays at or below 60
    return interval < 1 ? 1 : interval;
}

static bool OutputIsHDR(IDXGIFactory2* factory, HWND hwnd)
{
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) == S_OK; ++a) {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) == S_OK; ++o) {
            DXGI_OUTPUT_DESC od{};
            if (SUCCEEDED(output->GetDesc(&od)) && od.Monitor == mon) {
                ComPtr<IDXGIOutput6> o6;
                DXGI_OUTPUT_DESC1 od1{};
                if (SUCCEEDED(output.As(&o6)) && SUCCEEDED(o6->GetDesc1(&od1)))
                    return od1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return false;
}

bool Renderer::Init(HWND hwnd, const MMSettings& settings)
{
    hwnd_ = hwnd; headless_ = false; settings_ = settings;
    RECT rc{}; GetClientRect(hwnd, &rc);
    width_  = maxu(1, rc.right - rc.left);
    height_ = maxu(1, rc.bottom - rc.top);
    return InitCore();
}

bool Renderer::InitHeadless(UINT width, UINT height, const MMSettings& settings)
{
    hwnd_ = nullptr; headless_ = true; settings_ = settings;
    width_ = maxu(1, width); height_ = maxu(1, height);
    return InitCore();
}

bool Renderer::InitCore()
{
    if (!CreateDeviceAndSwapChain(hwnd_)) return false;
    if (headless_) { if (!CreateHeadlessFinal()) return false; }
    else           { if (!CreateBackbufferRTV()) return false; }
    if (!CreateSceneTargets(width_, height_)) return false;
    if (!CreateShaders())                return false;
    if (!CreateBuffersAndStates())       return false;
    if (!CreateOverlay()) MMLog("overlay (FPS) unavailable — continuing without it");

    if (!atlas_.Build(device_.Get(), ctx_.Get()))
        MMLog("atlas build failed — falling back to untextured quads");
    MMSettings ms = settings_;

    uint64_t seed = headless_ ? 0 : GetTickCount64();
    float aspect = (float)width_ / (float)maxu(1, height_);
    sim_ = mm_sim_create(&ms, atlas_.GlyphCount(), seed, aspect);
    if (!sim_) { MMLog("mm_sim_create failed"); return false; }

    EnsureInstanceBufferCapacity(mm_sim_max_instances(sim_));
	presentInterval_ = ComputePresentInterval(settings_.batterySaver != 0);
    ready_ = true;
    return true;
}

void Renderer::Shutdown()
{
    if (sim_) { mm_sim_destroy(sim_); sim_ = nullptr; }
    if (frameWaitable_) { CloseHandle(frameWaitable_); frameWaitable_ = nullptr; }
    d2dTargets_[0].Reset();
    d2dTargets_[1].Reset();
    instanceBuf_.Reset();
    instanceSRV_.Reset();
    ready_ = false;
}

bool Renderer::CreateDeviceAndSwapChain(HWND hwnd)
{
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    MM_CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                               levels, _countof(levels), D3D11_SDK_VERSION,
                               &device_, &got, &ctx_));

    if (headless_) return true;

    ComPtr<IDXGIDevice> dxgiDev;
    MM_CHECK(device_.As(&dxgiDev));
    ComPtr<IDXGIAdapter> adapter;
    MM_CHECK(dxgiDev->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory;
    MM_CHECK(adapter->GetParent(IID_PPV_ARGS(&factory)));

    swapFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (settings_.hdr && OutputIsHDR(factory.Get(), hwnd)) {
        swapFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
        MMLog("HDR display detected -> FP16 scRGB swapchain");
    }

    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width  = width_;
    sc.Height = height_;
    sc.Format = swapFormat_;
    sc.SampleDesc.Count = 1;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc.BufferCount = 3;
    sc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc.Scaling     = DXGI_SCALING_STRETCH;
    sc.AlphaMode   = DXGI_ALPHA_MODE_IGNORE;
    sc.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    MM_CHECK(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &sc, nullptr, nullptr, &swap_));

    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(swap_.As(&swap2))) {
        swap2->SetMaximumFrameLatency(1);
        frameWaitable_ = swap2->GetFrameLatencyWaitableObject();
    }

    if (swapFormat_ == DXGI_FORMAT_R16G16B16A16_FLOAT) {
        ComPtr<IDXGISwapChain3> sc3;
        UINT support = 0;
        if (SUCCEEDED(swap_.As(&sc3)) &&
            SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
            (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
        }
    }

    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool Renderer::CreateBackbufferRTV()
{
    ComPtr<ID3D11Texture2D> bb;
    MM_CHECK(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)));
    MM_CHECK(device_->CreateRenderTargetView(bb.Get(), nullptr, &backRTV_));
    return true;
}

bool Renderer::CreateHeadlessFinal()
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = width_; td.Height = height_; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    MM_CHECK(device_->CreateTexture2D(&td, nullptr, &finalTex_));
    MM_CHECK(device_->CreateRenderTargetView(finalTex_.Get(), nullptr, &backRTV_));

    D3D11_TEXTURE2D_DESC sd = td;
    sd.BindFlags = 0;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    MM_CHECK(device_->CreateTexture2D(&sd, nullptr, &stagingTex_));
    return true;
}

bool Renderer::CreateSceneTargets(UINT width, UINT height)
{
    auto mk = [&](UINT w, UINT h, ComPtr<ID3D11Texture2D>& t,
                  ComPtr<ID3D11RenderTargetView>& rtv,
                  ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
        t.Reset(); rtv.Reset(); srv.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        MM_CHECK(device_->CreateTexture2D(&td, nullptr, &t));
        MM_CHECK(device_->CreateRenderTargetView(t.Get(), nullptr, &rtv));
        MM_CHECK(device_->CreateShaderResourceView(t.Get(), nullptr, &srv));
        return true;
    };
    
    if (!mk(width, height, sceneTex_, sceneRTV_, sceneSRV_)) return false;
    if (!mk(width, height, crtInputTex_, crtInputRTV_, crtInputSRV_)) return false;
    
    UINT bw = maxu(1, width / 2);
    UINT bh = maxu(1, height / 2);
    for (int i = 0; i < kBloomMips; ++i) {
        bloomMipW_[i] = bw;
        bloomMipH_[i] = bh;
        if (!mk(bw, bh, bloomMipTex_[i], bloomMipRTV_[i], bloomMipSRV_[i])) return false;
        bw = maxu(1, bw / 2);
        bh = maxu(1, bh / 2);
    }
    return true;
}

bool Renderer::CreateShaders()
{
    MM_CHECK(device_->CreateVertexShader(kBlob_glyph_vertex, sizeof(kBlob_glyph_vertex), nullptr, &glyphVS_));
    MM_CHECK(device_->CreatePixelShader(kBlob_glyph_fragment, sizeof(kBlob_glyph_fragment), nullptr, &glyphPS_));
    MM_CHECK(device_->CreateVertexShader(kBlob_fullscreen_vertex, sizeof(kBlob_fullscreen_vertex), nullptr, &fsVS_));
    MM_CHECK(device_->CreatePixelShader(kBlob_bloom_threshold, sizeof(kBlob_bloom_threshold), nullptr, &thresholdPS_));
    MM_CHECK(device_->CreatePixelShader(kBlob_bloom_downsample, sizeof(kBlob_bloom_downsample), nullptr, &downsamplePS_));
    MM_CHECK(device_->CreatePixelShader(kBlob_bloom_upsample, sizeof(kBlob_bloom_upsample), nullptr, &upsamplePS_));
    MM_CHECK(device_->CreatePixelShader(kBlob_bloom_composite, sizeof(kBlob_bloom_composite), nullptr, &compositePS_));
    MM_CHECK(device_->CreatePixelShader(kBlob_crt_filter, sizeof(kBlob_crt_filter), nullptr, &crtFilterPS_));
    return true;
}

bool Renderer::EnsureInstanceBufferCapacity(int requiredInstances)
{
    if (requiredInstances <= bufferCapacity_ && instanceBuf_) return true;

    int newCapacity = ((requiredInstances + 4095) / 4096) * 4096;
    
    instanceBuf_.Reset();
    instanceSRV_.Reset();

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(MMGlyphInstance) * newCapacity;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(MMGlyphInstance);
    MM_CHECK(device_->CreateBuffer(&bd, nullptr, &instanceBuf_));

    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = newCapacity;
    MM_CHECK(device_->CreateShaderResourceView(instanceBuf_.Get(), &sd, &instanceSRV_));

    bufferCapacity_ = newCapacity;
    return true;
}

bool Renderer::CreateBuffersAndStates()
{
    D3D11_BUFFER_DESC cd{};
    cd.ByteWidth = sizeof(Uniforms);
    cd.Usage = D3D11_USAGE_DYNAMIC;
    cd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    MM_CHECK(device_->CreateBuffer(&cd, nullptr, &uniformCB_));
    cd.ByteWidth = 16;
    MM_CHECK(device_->CreateBuffer(&cd, nullptr, &postCB_));

    D3D11_BLEND_DESC bl{};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    MM_CHECK(device_->CreateBlendState(&bl, &blendPremul_));

    D3D11_BLEND_DESC ba{};
    ba.RenderTarget[0].BlendEnable = TRUE;
    ba.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    ba.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    ba.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    ba.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    MM_CHECK(device_->CreateBlendState(&ba, &blendAdditive_));

    D3D11_BLEND_DESC bo{};
    bo.RenderTarget[0].BlendEnable = FALSE;
    bo.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    MM_CHECK(device_->CreateBlendState(&bo, &blendOpaque_));

    D3D11_SAMPLER_DESC sm{};
    sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sm.MaxLOD = D3D11_FLOAT32_MAX;
    MM_CHECK(device_->CreateSamplerState(&sm, &sampLinear_));

    D3D11_RASTERIZER_DESC rs{};
    rs.FillMode = D3D11_FILL_SOLID;
    rs.CullMode = D3D11_CULL_NONE;
    rs.DepthClipEnable = TRUE;
    MM_CHECK(device_->CreateRasterizerState(&rs, &raster_));
    return true;
}

void Renderer::Apply(const MMSettings& settings)
{
    if (!ready_) { settings_ = settings; return; }
    settings_ = settings;
    presentInterval_ = ComputePresentInterval(settings_.batterySaver != 0);
    if (settings_.depthAmount <= 0.0) forwardTravel_ = 0.0f;
    mm_sim_set_camera_travel(sim_, forwardTravel_);
    MMSettings ms = settings_;
    float aspect = (float)width_ / (float)maxu(1, height_);
    mm_sim_update(sim_, &ms, atlas_.GlyphCount(), aspect);
    viewProjCached_ = false; 
}

void Renderer::Resize(UINT width, UINT height)
{
    if (!swap_ || (width == width_ && height == height_)) return;
    width_  = maxu(1, width);
    height_ = maxu(1, height);
    backRTV_.Reset();
    d2dTargets_[0].Reset();
    d2dTargets_[1].Reset();
    viewProjCached_ = false;

    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    HRESULT hr = swap_->ResizeBuffers(0, width_, height_, DXGI_FORMAT_UNKNOWN,
                                       DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT);
    if (FAILED(hr)) { ready_ = false; return; }
    if (frameWaitable_) { CloseHandle(frameWaitable_); frameWaitable_ = nullptr; }
    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(swap_.As(&swap2))) frameWaitable_ = swap2->GetFrameLatencyWaitableObject();
    if (!CreateBackbufferRTV() || !CreateSceneTargets(width_, height_)) {
        ready_ = false;
        return;
    }

    // Re-derive the lane grid for the new aspect ratio -- otherwise moving
    // the window to an ultra-wide monitor (or resizing the dev/preview
    // window) keeps the lane count picked at the old size.
    if (sim_) {
        MMSettings ms = settings_;
        float aspect = (float)width_ / (float)maxu(1, height_);
        mm_sim_update(sim_, &ms, atlas_.GlyphCount(), aspect);
    }
    ready_ = true;
}

void Renderer::UpdateUniforms()
{
    if (settings_.panning || settings_.depthAmount > 0.0 || !viewProjCached_) {
        float aspect = (float)width_ / (float)maxu(1, height_);
        XMFLOAT3 eyeP(0, 0, 48), ctrP(0, 0, -8);
        if (settings_.panning) {
            static const XMFLOAT3 pe[6] = { {0,3,48}, {15,7,45}, {0,15,43}, {-15,6,45}, {0,-5,50}, {9,2,38} };
            static const XMFLOAT3 pc[6] = { {0,0,-8}, {-3,0,-8}, {0,-3,-10}, {3,0,-8}, {0,4,-6}, {-4,0,-10} };
            const float seg = 9.0f;
            float cycle = panTime_ / seg;
            int i = (int)floorf(cycle) % 6; if (i < 0) i += 6;
            int j = (i + 1) % 6;
            float f = cycle - floorf(cycle);
            float t = f / 0.45f; if (t > 1.0f) t = 1.0f;
            float e = t * t * (3.0f - 2.0f * t);
            auto L = [e](float a, float b) { return a + (b - a) * e; };
            eyeP = XMFLOAT3(L(pe[i].x, pe[j].x), L(pe[i].y, pe[j].y), L(pe[i].z, pe[j].z));
            ctrP = XMFLOAT3(L(pc[i].x, pc[j].x), L(pc[i].y, pc[j].y), L(pc[i].z, pc[j].z));
        }
        eyeP.z -= forwardTravel_;
        ctrP.z -= forwardTravel_;
        XMVECTOR eye = XMVectorSet(eyeP.x, eyeP.y, eyeP.z, 1);
        XMVECTOR ctr = XMVectorSet(ctrP.x, ctrP.y, ctrP.z, 1);
        XMVECTOR up  = XMVectorSet(0, 1, 0, 0);

        XMMATRIX view = XMMatrixLookAtRH(eye, ctr, up);
        XMMATRIX proj = XMMatrixPerspectiveFovRH(XMConvertToRadians(46.0f), aspect, 1.0f, 240.0f);
        cachedViewProj_ = XMMatrixMultiply(view, proj);

        XMVECTOR fwd   = XMVector3Normalize(XMVectorSubtract(ctr, eye));
        XMVECTOR right = XMVector3Normalize(XMVector3Cross(fwd, up));
        XMVECTOR camup = XMVector3Cross(right, fwd);
        
        XMStoreFloat3(&cachedCamPos_, eye);
        XMStoreFloat3(&cachedCamRight_, right);
        XMStoreFloat3(&cachedCamUp_, camup);

        if (!settings_.panning) {
            viewProjCached_ = true;
        }
    }

    Uniforms u{};
    XMFLOAT4X4 m; XMStoreFloat4x4(&m, cachedViewProj_);
    memcpy(u.viewProj, &m, sizeof(m));

    u.camX = cachedCamPos_.x;   u.camY = cachedCamPos_.y;   u.camZ = cachedCamPos_.z;
    u.camRX = cachedCamRight_.x; u.camRY = cachedCamRight_.y; u.camRZ = cachedCamRight_.z;
    u.camUX = cachedCamUp_.x;    u.camUY = cachedCamUp_.y;    u.camUZ = cachedCamUp_.z;

    u.glyphHalf = settings_.glyphScale;
    u.atlasCols = (float)atlas_.Cols();
    u.atlasRows = (float)atlas_.Rows();
    u.time = time_;
    u.fogEnabled   = settings_.fog ? 1.0f : 0.0f;

    const float kCameraToPlaneDist = 38.0f;
    u.fogStartDist = kCameraToPlaneDist - 4.0f;
    u.fogEndDist   = kCameraToPlaneDist + 148.0f;
    
    u.textured  = (atlas_.HasTexture() && settings_.textured) ? 1.0f : 0.0f;
    u.wireframe = settings_.wireframe ? 1.0f : 0.0f;
    u.extraContrastHeads = settings_.extraContrastHeads ? 1.0f : 0.0f;

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(uniformCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, &u, sizeof(u));
        ctx_->Unmap(uniformCB_.Get(), 0);
    }
}

void Renderer::DrawScene()
{
    int max_instances = mm_sim_max_instances(sim_);
    EnsureInstanceBufferCapacity(max_instances);

    if (sortBuf_.size() < (size_t)max_instances) sortBuf_.resize(max_instances);

    instanceCount_ = mm_sim_write_instances(sim_, sortBuf_.data(), max_instances, simAccumulator_);

    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(instanceBuf_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        memcpy(ms.pData, sortBuf_.data(), sizeof(MMGlyphInstance) * instanceCount_);
        ctx_->Unmap(instanceBuf_.Get(), 0);
    }
    UpdateUniforms();

    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    ctx_->PSSetShaderResources(0, 2, nulls);

    const float black[4] = { 0, 0, 0, 1 };
    ctx_->OMSetRenderTargets(1, sceneRTV_.GetAddressOf(), nullptr);
    ctx_->ClearRenderTargetView(sceneRTV_.Get(), black);
    D3D11_VIEWPORT vp{ 0, 0, (float)width_, (float)height_, 0, 1 };
    ctx_->RSSetViewports(1, &vp);
    ctx_->RSSetState(raster_.Get());
    ctx_->OMSetBlendState(blendPremul_.Get(), nullptr, 0xffffffff);

    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx_->VSSetShader(glyphVS_.Get(), nullptr, 0);
    ctx_->PSSetShader(glyphPS_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, uniformCB_.GetAddressOf());
    ctx_->PSSetConstantBuffers(0, 1, uniformCB_.GetAddressOf());
    ctx_->VSSetShaderResources(1, 1, instanceSRV_.GetAddressOf());
    ID3D11ShaderResourceView* atlasSRV = atlas_.SRV();
    ctx_->PSSetShaderResources(0, 1, &atlasSRV);
    ctx_->PSSetSamplers(0, 1, sampLinear_.GetAddressOf());

    if (instanceCount_ > 0)
        ctx_->DrawInstanced(4, (UINT)instanceCount_, 0, 0);
}

void Renderer::DrawPost()
{
    const float kBuildInDuration = 1.8f;
    float buildInT = (time_ < kBuildInDuration) ? (time_ / kBuildInDuration) : 1.0f;
    float buildInEase = buildInT * buildInT * (3.0f - 2.0f * buildInT);

    const float distortAmt    = (float)settings_.crtDistort * 0.25f * buildInEase;
    const float aberrationAmt = distortAmt * 0.05f;
    const float bloomAmt      = (float)settings_.bloomIntensity * buildInEase;

    auto setPost = [&](float x, float y, float z, float w) {
        float v[4] = { x, y, z, w };
        D3D11_MAPPED_SUBRESOURCE ms;
        if (SUCCEEDED(ctx_->Map(postCB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            memcpy(ms.pData, v, sizeof(v));
            ctx_->Unmap(postCB_.Get(), 0);
        }
    };
    auto drawFS = [&](ID3D11RenderTargetView* rtv, UINT w, UINT h,
                      ID3D11PixelShader* ps, ID3D11ShaderResourceView* a,
                      ID3D11ShaderResourceView* b) {
        ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
        ctx_->PSSetShaderResources(0, 2, nulls);
        ctx_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0, 1 };
        ctx_->RSSetViewports(1, &vp);
        ctx_->VSSetShader(fsVS_.Get(), nullptr, 0);
        ctx_->PSSetShader(ps, nullptr, 0);
        ID3D11ShaderResourceView* srvs[2] = { a, b };
        ctx_->PSSetShaderResources(0, 2, srvs);
        ctx_->PSSetSamplers(0, 1, sampLinear_.GetAddressOf());
        ctx_->PSSetConstantBuffers(0, 1, postCB_.GetAddressOf());
        ctx_->Draw(4, 0);
    };

    ctx_->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

    // When CRT emulation is on, bloom_composite writes to the intermediate
    // crtInput target instead of the backbuffer directly, so crt_filter can
    // read it back afterward (see crtInputRTV_ comment in renderer.h).
    const bool crtOn = settings_.crtEmulation != 0;
    ID3D11RenderTargetView* compositeTarget = crtOn ? crtInputRTV_.Get() : backRTV_.Get();

    if (settings_.bloom) {
        setPost(0.72f, 0, 1.0f / (float)width_, 1.0f / (float)height_);
        drawFS(bloomMipRTV_[0].Get(), bloomMipW_[0], bloomMipH_[0], thresholdPS_.Get(), sceneSRV_.Get(), nullptr);

        for (int i = 1; i < kBloomMips; ++i) {
            setPost(0, 0, 1.0f / bloomMipW_[i-1], 1.0f / bloomMipH_[i-1]);
            drawFS(bloomMipRTV_[i].Get(), bloomMipW_[i], bloomMipH_[i], downsamplePS_.Get(), bloomMipSRV_[i-1].Get(), nullptr);
        }

        ctx_->OMSetBlendState(blendAdditive_.Get(), nullptr, 0xffffffff);
        for (int i = kBloomMips - 2; i >= 0; --i) {
            setPost(0, 0, 1.0f / bloomMipW_[i+1], 1.0f / bloomMipH_[i+1]);
            drawFS(bloomMipRTV_[i].Get(), bloomMipW_[i], bloomMipH_[i], upsamplePS_.Get(), bloomMipSRV_[i+1].Get(), nullptr);
        }
        ctx_->OMSetBlendState(blendOpaque_.Get(), nullptr, 0xffffffff);

        setPost(bloomAmt, distortAmt, 0, aberrationAmt);
        drawFS(compositeTarget, width_, height_, compositePS_.Get(), sceneSRV_.Get(), bloomMipSRV_[0].Get());
    } else {
        setPost(0, distortAmt, 0, aberrationAmt);
        drawFS(compositeTarget, width_, height_, compositePS_.Get(), sceneSRV_.Get(), sceneSRV_.Get());
    }

    if (crtOn) {
        // texel size in xy, time in z (see crt_filter's post.xyz convention
        // in shaders.hlsl); w unused.
        setPost(1.0f / (float)width_, 1.0f / (float)height_, time_, 0);
        drawFS(backRTV_.Get(), width_, height_, crtFilterPS_.Get(), crtInputSRV_.Get(), nullptr);
    }

    ID3D11ShaderResourceView* nulls[2] = { nullptr, nullptr };
    ctx_->PSSetShaderResources(0, 2, nulls);
}

bool Renderer::RenderFrame(float dt)
{
    if (!ready_) return false;

    if (swap_ && isOccluded_) {
        HRESULT hr = swap_->Present(presentInterval_, 0);
        if (hr == DXGI_STATUS_OCCLUDED) {
            Sleep(10); 
            return true;
        }
        isOccluded_ = false;
    }

    time_ += dt;
    panTime_ += dt;

    if (settings_.depthAmount > 0.0) {
        const float kForwardSpeed = (float)(settings_.cameraSpeed * 2.0);
        static const float kRecycleDistance = 72.0f;
        forwardTravel_ += kForwardSpeed * dt;
        if (forwardTravel_ >= kRecycleDistance) {
            forwardTravel_ -= kRecycleDistance;
            mm_sim_rebase_depth(sim_, kRecycleDistance);
        }
        mm_sim_set_camera_travel(sim_, forwardTravel_);
        viewProjCached_ = false;
    } else {
        forwardTravel_ = 0.0f;
        mm_sim_set_camera_travel(sim_, 0.0f);
    }

    // Fed unconditionally -- mmcore.c only uses this to detect 11:11 AM/PM
    // for the rain easter egg (gated on the easterEggs setting over there),
    // so there's no separate toggle to check here.
    {
        SYSTEMTIME lt; GetLocalTime(&lt);
        mm_sim_set_clock(sim_, lt.wHour, lt.wMinute, lt.wSecond);
    }

    simAccumulator_ += dt;
    const float kTargetTimeStep = 1.0f / 60.0f;
    if (simAccumulator_ > 0.1f) simAccumulator_ = 0.1f;
    while (simAccumulator_ >= kTargetTimeStep) {
        mm_sim_advance(sim_, kTargetTimeStep);
        simAccumulator_ -= kTargetTimeStep;       
    }

    if (dt > 0) { double inst = 1.0 / dt; fps_ = (fps_ == 0) ? inst : fps_ * 0.9 + inst * 0.1; }

    DrawScene();
    DrawPost();
    DrawOverlay();
    if (headless_) { ctx_->Flush(); return true; }

    HRESULT hr = swap_->Present(1, 0);
    
    if (hr == DXGI_STATUS_OCCLUDED) {
        isOccluded_ = true;
        return true;
    }
    
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        ready_ = false;
        return false;
    }
    return true;
}

bool Renderer::CreateOverlay()
{
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 &fo, (void**)d2dFactory_.GetAddressOf()))) return false;
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(device_.As(&dxgi))) return false;
    if (FAILED(d2dFactory_->CreateDevice(dxgi.Get(), &d2dDevice_))) return false;
    if (FAILED(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx_))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dwrite_.GetAddressOf()))) return false;
    if (FAILED(dwrite_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 18.0f, L"", &textFormat_)))
        return false;
    d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.62f, 1.0f, 0.72f, 1.0f), &textBrush_);
    return textBrush_ != nullptr;
}

void Renderer::DrawOverlay()
{
    if (!settings_.showFPS || !d2dCtx_ || !textBrush_) return;

    UINT bufferIdx = 0;
    if (!headless_ && swap_) {
        ComPtr<IDXGISwapChain3> swap3;
        if (SUCCEEDED(swap_.As(&swap3))) {
            bufferIdx = swap3->GetCurrentBackBufferIndex();
        }
    }

    if (!d2dTargets_[bufferIdx]) {
        ComPtr<IDXGISurface> surface;
        if (headless_) { if (FAILED(finalTex_.As(&surface))) return; }
        else { if (!swap_ || FAILED(swap_->GetBuffer(bufferIdx, IID_PPV_ARGS(&surface)))) return; }

        DXGI_FORMAT fmt = headless_ ? DXGI_FORMAT_B8G8R8A8_UNORM : swapFormat_;
        D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(fmt, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(d2dCtx_->CreateBitmapFromDxgiSurface(surface.Get(), &props, &d2dTargets_[bufferIdx]))) return;
    }

    wchar_t buf[64];
    int n = swprintf(buf, 64, L"%.0f FPS", fps_);
    if (n <= 0) return;

    d2dCtx_->SetTarget(d2dTargets_[bufferIdx].Get());
    d2dCtx_->BeginDraw();
    textBrush_->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
    d2dCtx_->DrawText(buf, (UINT32)n, textFormat_.Get(), D2D1::RectF(13, 9, 260, 42), textBrush_.Get());
    textBrush_->SetColor(D2D1::ColorF(0.62f, 1.0f, 0.72f, 1.0f));
    d2dCtx_->DrawText(buf, (UINT32)n, textFormat_.Get(), D2D1::RectF(12, 8, 260, 42), textBrush_.Get());
    d2dCtx_->EndDraw();
    d2dCtx_->SetTarget(nullptr);
}

bool Renderer::SaveScreenshot(const wchar_t* path)
{
    if (!finalTex_ || !stagingTex_) { MMLog("SaveScreenshot: not headless"); return false; }
    ctx_->CopyResource(stagingTex_.Get(), finalTex_.Get());

    D3D11_MAPPED_SUBRESOURCE ms;
    MM_CHECK(ctx_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &ms));

    UINT w = width_, h = height_;
    UINT stride = w * 4;
    BYTE* bgra = (BYTE*)malloc(stride * h);
    if (!bgra) { ctx_->Unmap(stagingTex_.Get(), 0); return false; }

    for (UINT y = 0; y < h; ++y) {
        memcpy(bgra + y * stride, (BYTE*)ms.pData + y * ms.RowPitch, stride);
    }
    ctx_->Unmap(stagingTex_.Get(), 0);

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { free(bgra); return false; }

    ComPtr<IWICStream> stream;
    if (FAILED(factory->CreateStream(&stream)) || 
        FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE))) { free(bgra); return false; }

    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) { free(bgra); return false; }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    enc->CreateNewFrame(&frame, &props);
    frame->Initialize(props.Get());
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WritePixels(h, stride, stride * h, (BYTE*)bgra))) { free(bgra); return false; }
    frame->Commit();
    enc->Commit();
    free(bgra);
    return true;
}