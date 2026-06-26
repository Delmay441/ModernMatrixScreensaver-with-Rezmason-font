// renderer.h — Direct3D 11 renderer for the Modern Matrix rain.
//
// Drives the shared simulation (core/mmcore.h) and draws it the same way the macOS
// Metal renderer does: instanced camera-facing billboards, premultiplied-over blend
// on a black clear, rendered to an HDR (R16F) scene target and composited (with
// optional bloom) to the swapchain. Camera / fog / bloom constants mirror
// Sources/Core/Renderer.swift.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <cstdint>
#include "../core/mmcore.h"
#include "atlas.h"

// Mirrors the `Uniforms` cbuffer in shaders.hlsl (and ShaderTypes.swift): scalar
// floats so the 144-byte layout is identical on both sides (no padding surprises).
struct Uniforms {
    float viewProj[16];                                   // row-major
    float camX, camY, camZ;
    float camRX, camRY, camRZ;
    float camUX, camUY, camUZ;
    float glyphHalf, atlasCols, atlasRows, time;
    float fogEnabled, fogStartDist, fogEndDist;
    float textured, wireframe, pad0, pad1;
};
static_assert(sizeof(Uniforms) == 144, "Uniforms must stay a tight 144 bytes");

class Renderer {
public:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool Init(HWND hwnd, const MMSettings& settings);
    bool InitHeadless(UINT width, UINT height, const MMSettings& settings); // offscreen capture
    void Shutdown();
    void Resize(UINT width, UINT height);
    void Apply(const MMSettings& settings);   // live settings change
    bool RenderFrame(float dt);               // advance sim + draw; false if device lost
    bool SaveScreenshot(const wchar_t* path); // headless: copy final target -> PNG
    double Fps() const { return fps_; }

private:
    bool InitCore();
    bool CreateDeviceAndSwapChain(HWND hwnd);
    bool CreateBackbufferRTV();
    bool CreateHeadlessFinal();
    bool CreateSceneTargets(UINT width, UINT height);
    bool CreateShaders();
    bool CreateBuffersAndStates();
    bool CreateOverlay();   // Direct2D/DirectWrite for the FPS text
    void UpdateUniforms();
    void DrawScene();
    void DrawPost();    // bloom (optional) + composite to backbuffer
    void DrawOverlay(); // FPS text (when showFPS)

    HWND hwnd_ = nullptr;
    UINT width_ = 1, height_ = 1;

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1>     swap_;
    ComPtr<ID3D11RenderTargetView> backRTV_;
    DXGI_FORMAT swapFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;  // FP16 scRGB when HDR display

    // Headless capture (no swapchain): composite into finalTex_, copy to staging, encode PNG.
    bool headless_ = false;
    ComPtr<ID3D11Texture2D> finalTex_;
    ComPtr<ID3D11Texture2D> stagingTex_;

    // HDR scene target + half-res bloom ping-pong (all R16G16B16A16_FLOAT).
    ComPtr<ID3D11Texture2D> sceneTex_;
    ComPtr<ID3D11RenderTargetView> sceneRTV_;
    ComPtr<ID3D11ShaderResourceView> sceneSRV_;
    ComPtr<ID3D11Texture2D> bloomTex_[2];
    ComPtr<ID3D11RenderTargetView> bloomRTV_[2];
    ComPtr<ID3D11ShaderResourceView> bloomSRV_[2];
    UINT bloomW_ = 1, bloomH_ = 1;

    // Pipeline.
    ComPtr<ID3D11VertexShader> glyphVS_;
    ComPtr<ID3D11PixelShader>  glyphPS_;
    ComPtr<ID3D11VertexShader> fsVS_;
    ComPtr<ID3D11PixelShader>  thresholdPS_;
    ComPtr<ID3D11PixelShader>  blurPS_;
    ComPtr<ID3D11PixelShader>  compositePS_;

    ComPtr<ID3D11Buffer> instanceBuf_;     // structured, dynamic
    ComPtr<ID3D11ShaderResourceView> instanceSRV_;
    ComPtr<ID3D11Buffer> uniformCB_;       // b0 for glyph pass
    ComPtr<ID3D11Buffer> postCB_;          // b0 for post passes (float4)

    ComPtr<ID3D11BlendState> blendPremul_; // ONE / INV_SRC_ALPHA
    ComPtr<ID3D11BlendState> blendOpaque_;
    ComPtr<ID3D11SamplerState> sampLinear_;
    ComPtr<ID3D11RasterizerState> raster_;

    // Direct2D / DirectWrite overlay (FPS text).
    ComPtr<ID2D1Factory1>       d2dFactory_;
    ComPtr<ID2D1Device>         d2dDevice_;
    ComPtr<ID2D1DeviceContext>  d2dCtx_;
    ComPtr<IDWriteFactory>      dwrite_;
    ComPtr<IDWriteTextFormat>   textFormat_;
    ComPtr<ID2D1SolidColorBrush> textBrush_;

    GlyphAtlas atlas_;
    MMSim* sim_ = nullptr;
    MMSettings settings_{};

    static const int kMaxInstances = 65536;
    int instanceCount_ = 0;
    float time_ = 0.f, panTime_ = 0.f;
    double fps_ = 0.0;

    bool ready_ = false;
};
