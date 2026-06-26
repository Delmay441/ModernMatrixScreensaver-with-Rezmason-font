// atlas.cpp — DirectWrite/Direct2D rasterisation of the glyph set into an R8 atlas.
//
// Mirrors Sources/Core/GlyphAtlas.swift: cols x rows cells of `cellPx`, each glyph
// drawn centered, mirrored horizontally (the film look), single-channel coverage,
// with a full mip chain so distant glyphs don't shimmer. Cell index == glyph index.
#include "atlas.h"
#include "../core/mmcore.h"
#include "log.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

// Note: windows.h #defines DrawText -> DrawTextW. Because it is included before
// d2d1.h, ID2D1RenderTarget's method is declared as DrawTextW too, so we just call
// rt->DrawText(...) and let the same macro expand our call to match. (Do NOT #undef
// it here — that would desync our call from the header.)

using Microsoft::WRL::ComPtr;

// Fonts with good half-width katakana coverage, best first.
static const wchar_t* kFonts[] = { L"MS Gothic", L"Yu Gothic", L"Meiryo", L"Segoe UI" };

bool GlyphAtlas::Build(ID3D11Device* device, ID3D11DeviceContext* ctx,
                       int encoding, int cellPx, int cols)
{
    uint32_t cps[MM_MAX_CODEPOINTS];
    int count = mm_encoding_codepoints(encoding, cps, MM_MAX_CODEPOINTS);
    if (count < 1) { count = 1; cps[0] = L'?'; }
    int rows = (count + cols - 1) / cols;
    const UINT W = (UINT)(cols * cellPx);
    const UINT H = (UINT)(rows * cellPx);

    // --- Direct2D render target backed by a WIC bitmap ---
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic)))) { MMLog("atlas: WIC factory failed"); return false; }
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, &bmp))) { MMLog("atlas: WIC bitmap failed"); return false; }

    ComPtr<ID2D1Factory> d2d;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
        MMLog("atlas: D2D factory failed"); return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), rtp, &rt))) {
        MMLog("atlas: D2D RT failed"); return false;
    }

    ComPtr<IDWriteFactory> dw;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   (IUnknown**)dw.GetAddressOf()))) { MMLog("atlas: DWrite failed"); return false; }

    ComPtr<IDWriteTextFormat> fmt;
    for (const wchar_t* font : kFonts) {
        if (SUCCEEDED(dw->CreateTextFormat(font, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                cellPx * 0.82f, L"", &fmt))) {
            MMLog("atlas: using font '%ls'", font);
            break;
        }
    }
    if (!fmt) { MMLog("atlas: no usable font"); return false; }
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 1));   // black, opaque
    ComPtr<ID2D1SolidColorBrush> white;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    for (int i = 0; i < count; ++i) {
        int col = i % cols, row = i / cols;
        float x0 = (float)(col * cellPx), y0 = (float)(row * cellPx);
        float cx = x0 + cellPx * 0.5f;
        // Mirror each glyph horizontally about its own cell centre.
        rt->SetTransform(D2D1::Matrix3x2F(-1, 0, 0, 1, 2 * cx, 0));
        wchar_t ch = (wchar_t)cps[i];
        D2D1_RECT_F r = D2D1::RectF(x0, y0, x0 + cellPx, y0 + cellPx);
        rt->DrawText(&ch, 1, fmt.Get(), r, white.Get());
    }
    rt->SetTransform(D2D1::Matrix3x2F::Identity());
    HRESULT hr = rt->EndDraw();
    if (FAILED(hr)) { MMLogHR("atlas EndDraw", hr); return false; }

    // --- Extract single-channel coverage (premultiplied white -> B == coverage) ---
    ComPtr<IWICBitmapLock> lock;
    WICRect full = { 0, 0, (INT)W, (INT)H };
    if (FAILED(bmp->Lock(&full, WICBitmapLockRead, &lock))) { MMLog("atlas: WIC lock failed"); return false; }
    UINT stride = 0, cb = 0; BYTE* src = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&cb, &src);
    std::vector<BYTE> r8((size_t)W * H);
    for (UINT y = 0; y < H; ++y) {
        const BYTE* row = src + (size_t)y * stride;
        for (UINT x = 0; x < W; ++x) r8[(size_t)y * W + x] = row[x * 4];   // B channel
    }
    lock.Reset();

    // --- Upload as an R8 texture with a full mip chain ---
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 0; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ComPtr<ID3D11Texture2D> tex;
    if (FAILED(hr = device->CreateTexture2D(&td, nullptr, &tex))) { MMLogHR("atlas CreateTexture2D", hr); return false; }
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, r8.data(), W, 0);
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(hr = device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) { MMLogHR("atlas SRV", hr); return false; }
    ctx->GenerateMips(srv.Get());

    tex_ = tex; srv_ = srv;
    cols_ = cols; rows_ = rows; glyphCount_ = count; encoding_ = encoding;
    MMLog("atlas built: %ux%u  %d glyphs  (%dx%d cells)", W, H, count, cols, rows);
    return true;
}
