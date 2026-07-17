// renderer.h — Direct3D 11 renderer for the Modern Matrix rain.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include "mmcore.h"
#include "atlas.h"
#include <DirectXMath.h>

struct Uniforms {
    float viewProj[16];                                   // row-major
    float camX, camY, camZ;
    float camRX, camRY, camRZ;
    float camUX, camUY, camUZ;
    float glyphHalf, atlasCols, atlasRows, time;
    float fogEnabled, fogStartDist, fogEndDist;
    float textured, wireframe, extraContrastHeads, pad1;
};
static_assert(sizeof(Uniforms) == 144, "Uniforms size mismatch");

class Renderer {
public:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool Init(HWND hwnd, const MMSettings& settings);
    bool InitHeadless(UINT width, UINT height, const MMSettings& settings);
    void Shutdown();
    void Resize(UINT width, UINT height);
    void Apply(const MMSettings& settings);
    bool RenderFrame(float dt);
    bool SaveScreenshot(const wchar_t* path);
    double Fps() const { return fps_; }

    HANDLE FrameWaitableHandle() const { return frameWaitable_; }

private:
    bool InitCore();
    bool CreateDeviceAndSwapChain(HWND hwnd);
    bool CreateBackbufferRTV();
    bool CreateHeadlessFinal();
    bool CreateSceneTargets(UINT width, UINT height);
    bool CreateShaders();
    bool CreateBuffersAndStates();
    bool EnsureInstanceBufferCapacity(int requiredInstances);
    bool CreateOverlay();
    void UpdateUniforms();
    void DrawScene();
    void DrawPost();
    void DrawOverlay();

    HWND hwnd_ = nullptr;
    UINT width_ = 1, height_ = 1;

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1>     swap_;
    ComPtr<ID3D11RenderTargetView> backRTV_;
    DXGI_FORMAT swapFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    HANDLE frameWaitable_ = nullptr;

    bool headless_ = false;
    ComPtr<ID3D11Texture2D> finalTex_;
    ComPtr<ID3D11Texture2D> stagingTex_;

    ComPtr<ID3D11Texture2D> sceneTex_;
    ComPtr<ID3D11RenderTargetView> sceneRTV_;
    ComPtr<ID3D11ShaderResourceView> sceneSRV_;

    // Full-res intermediate target holding bloom_composite's output when CRT
    // emulation is on, since the swapchain backbuffer (backRTV_'s usual
    // target) isn't created with D3D11_BIND_SHADER_RESOURCE and so can't be
    // read back from in the same frame the way crt_filter needs to. Unused
    // (and not allocated) when crtEmulation is off -- composite writes
    // straight to backRTV_ as before in that case.
    ComPtr<ID3D11Texture2D> crtInputTex_;
    ComPtr<ID3D11RenderTargetView> crtInputRTV_;
    ComPtr<ID3D11ShaderResourceView> crtInputSRV_;
    
    static const int kBloomMips = 5;
    ComPtr<ID3D11Texture2D> bloomMipTex_[kBloomMips];
    ComPtr<ID3D11RenderTargetView> bloomMipRTV_[kBloomMips];
    ComPtr<ID3D11ShaderResourceView> bloomMipSRV_[kBloomMips];
    UINT bloomMipW_[kBloomMips];
    UINT bloomMipH_[kBloomMips];

    ComPtr<ID3D11VertexShader> glyphVS_;
    ComPtr<ID3D11PixelShader>  glyphPS_;
    ComPtr<ID3D11VertexShader> fsVS_;
    ComPtr<ID3D11PixelShader>  thresholdPS_;
    ComPtr<ID3D11PixelShader>  downsamplePS_;
    ComPtr<ID3D11PixelShader>  upsamplePS_;
    ComPtr<ID3D11PixelShader>  compositePS_;
    ComPtr<ID3D11PixelShader>  crtFilterPS_;

    ComPtr<ID3D11Buffer> instanceBuf_;
    ComPtr<ID3D11ShaderResourceView> instanceSRV_;
    ComPtr<ID3D11Buffer> uniformCB_;
    ComPtr<ID3D11Buffer> postCB_;

    ComPtr<ID3D11BlendState> blendPremul_;
    ComPtr<ID3D11BlendState> blendOpaque_;
    ComPtr<ID3D11BlendState> blendAdditive_; 
    ComPtr<ID3D11SamplerState> sampLinear_;
    ComPtr<ID3D11RasterizerState> raster_;

    ComPtr<ID2D1Factory1>       d2dFactory_;
    ComPtr<ID2D1Device>         d2dDevice_;
    ComPtr<ID2D1DeviceContext>  d2dCtx_;
    ComPtr<IDWriteFactory>      dwrite_;
    ComPtr<IDWriteTextFormat>   textFormat_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;
    ComPtr<ID2D1Bitmap1>        d2dTargets_[2]; 

    GlyphAtlas atlas_;
    MMSim* sim_ = nullptr;
    MMSettings settings_{};

    std::vector<MMGlyphInstance> sortBuf_;

    int bufferCapacity_ = 0; 
	UINT presentInterval_ = 1;   // swapchain sync interval; >1 caps fps below the panel's refresh rate
    int instanceCount_ = 0;
    float time_ = 0.f, panTime_ = 0.f;
    float forwardTravel_ = 0.f;
    float simAccumulator_ = 0.f; 
    double fps_ = 0.0;

    bool viewProjCached_ = false;
    DirectX::XMMATRIX cachedViewProj_;
    DirectX::XMFLOAT3 cachedCamPos_;
    DirectX::XMFLOAT3 cachedCamRight_;
    DirectX::XMFLOAT3 cachedCamUp_;

    bool ready_ = false;
    bool isOccluded_ = false;
};