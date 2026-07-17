#include "atlas.h"
#include "../core/mmcore.h"
#include "log.h"
#include "font_data.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <vector>
#include <cstdint>

using Microsoft::WRL::ComPtr;

static const wchar_t* kFonts[] = { L"Matrix-Code" };

bool GlyphAtlas::Build(ID3D11Device* device, ID3D11DeviceContext* ctx,
                       int cellPx, int cols)
{
    static const uint32_t cps[] = {
        0x0022, 0x002A, 0x002B, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0037, 0x0038, 0x0039, 0x003A, 0x003C, 0x003E, 0x007A, 0x007C, 0x00A6, 0x00A9,
        0x254C, 0x25AA, 0x30A2, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30B1, 0x30B3, 0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD, 0x30BF, 0x30C4, 0x30C6, 0x30CA, 
        0x30CB, 0x30CC, 0x30CD, 0x30CF, 0x30D2, 0x30DB, 0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2, 0x30E4, 0x30E8, 0x30E9, 0x30EA, 0x30EF, 0x30FC, 0xA78A, 0xE937
    };
    const int count = sizeof(cps) / sizeof(cps[0]);

    int rows = (count + cols - 1) / cols;
    const UINT W = (UINT)(cols * cellPx);
    const UINT H = (UINT)(rows * cellPx);

    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    
    ComPtr<IWICBitmap> bmp;
    if (FAILED(wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp))) return false;

    ComPtr<ID2D1Factory> d2d;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) return false;
    
    D2D1_RENDER_TARGET_PROPERTIES rtp = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    
    ComPtr<ID2D1RenderTarget> rt;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(bmp.Get(), rtp, &rt))) return false;

    ComPtr<IDWriteFactory> dw;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.GetAddressOf()))) return false;

    // Embed the font straight from font_data.h into a private, in-memory
    // DirectWrite font collection. This is the reliable way to do this:
    // AddFontMemResourceEx only registers the font with GDI's font table, and
    // whether DirectWrite's *system* collection notices that is unpredictable
    // across Windows versions/configurations -- which is exactly why this used
    // to silently fall back to Arial on some machines. Feeding DirectWrite the
    // bytes directly through its own in-memory loader sidesteps OS-wide font
    // visibility entirely: no install, no registration anyone else can see.
    ComPtr<IDWriteFactory5> dw5;
    ComPtr<IDWriteInMemoryFontFileLoader> memLoader;
    ComPtr<IDWriteFontCollection1> customCollection;
    if (SUCCEEDED(dw.As(&dw5))) {
        if (SUCCEEDED(dw5->CreateInMemoryFontFileLoader(&memLoader)) &&
            SUCCEEDED(dw5->RegisterFontFileLoader(memLoader.Get()))) {
            ComPtr<IDWriteFontFile> fontFile;
            ComPtr<IDWriteFontFaceReference> faceRef;
            ComPtr<IDWriteFontSetBuilder1> setBuilder;
            ComPtr<IDWriteFontSet> fontSet;
            if (SUCCEEDED(memLoader->CreateInMemoryFontFileReference(
                    dw5.Get(), Matrix_Code_ttf, (UINT32)Matrix_Code_ttf_len, nullptr, &fontFile)) &&
                SUCCEEDED(dw5->CreateFontFaceReference(fontFile.Get(), 0, DWRITE_FONT_SIMULATIONS_NONE, &faceRef)) &&
                SUCCEEDED(dw5->CreateFontSetBuilder(&setBuilder)) &&
                SUCCEEDED(setBuilder->AddFontFaceReference(faceRef.Get())) &&
                SUCCEEDED(setBuilder->CreateFontSet(&fontSet))) {
                dw5->CreateFontCollectionFromFontSet(fontSet.Get(), &customCollection);
            }
        }
    } else {
        MMLog("atlas: IDWriteFactory5 unavailable (needs Windows 10 1703+) -- falling back to Arial");
    }

    ComPtr<IDWriteTextFormat> fmt;

    // 1. Try the embedded font from our private collection first.
    if (customCollection) {
        for (const wchar_t* font : kFonts) {
            if (SUCCEEDED(dw->CreateTextFormat(font, customCollection.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, cellPx * 0.82f, L"", &fmt))) {
                MMLog("atlas: successfully loaded embedded font from in-memory collection");
                break;
            }
        }
        if (!fmt) MMLog("atlas: embedded font present but family name lookup failed");
    }

    // 2. If that failed for any reason, fall back to Arial (system collection).
    if (!fmt) {
        MMLog("atlas: falling back to Arial");
        dw->CreateTextFormat(L"Arial", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, cellPx * 0.82f, L"", &fmt);
    }
    
    if (!fmt) {
        if (memLoader) dw5->UnregisterFontFileLoader(memLoader.Get());
        return false;
    }
    
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 1));
    ComPtr<ID2D1SolidColorBrush> white;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);
    
    for (int i = 0; i < count; ++i) {
        int col = i % cols, row = i / cols;
        float x0 = (float)(col * cellPx), y0 = (float)(row * cellPx);
        wchar_t ch = (wchar_t)cps[i];
        rt->DrawText(&ch, 1, fmt.Get(), D2D1::RectF(x0, y0, x0 + cellPx, y0 + cellPx), white.Get());
    }
    rt->EndDraw();

    // The font file/face/collection each hold their own reference into the
    // loader, and we're done resolving this font once the atlas bitmap is
    // drawn, so unregister it now -- mirrors the old code's symmetric
    // AddFontMemResourceEx/RemoveFontMemResourceEx pattern.
    if (memLoader) dw5->UnregisterFontFileLoader(memLoader.Get());

    ComPtr<IWICBitmapLock> lock;
    WICRect full = { 0, 0, (INT)W, (INT)H };
    if (FAILED(bmp->Lock(&full, WICBitmapLockRead, &lock))) return false;
    
    UINT stride = 0, cb = 0; BYTE* src = nullptr;
    lock->GetStride(&stride);
    lock->GetDataPointer(&cb, &src);
    std::vector<BYTE> r8((size_t)W * H);
    for (UINT y = 0; y < H; ++y) {
        const BYTE* row = src + (size_t)y * stride;
        for (UINT x = 0; x < W; ++x) r8[(size_t)y * W + x] = row[x * 4];
    }
    lock.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = W; td.Height = H; td.MipLevels = 0; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = device->CreateTexture2D(&td, nullptr, &tex);
    if (FAILED(hr)) return false;
    
    ctx->UpdateSubresource(tex.Get(), 0, nullptr, r8.data(), W, 0);
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
    ctx->GenerateMips(srv.Get());

    tex_ = tex; srv_ = srv;
    cols_ = cols; rows_ = rows; glyphCount_ = count;
    return true;
}
