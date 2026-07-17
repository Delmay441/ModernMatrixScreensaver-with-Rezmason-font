// atlas.h — single-channel (R8) glyph atlas built from a fixed glyph set.
//
// Rasterises a fixed codepoint table into a cols x rows grid of cells (cell
// index == glyph index) so the renderer can address a glyph by the `cell`
// field of an MMGlyphInstance. Mirrors Sources/Core/GlyphAtlas.swift (72px
// cells, 16 cols, mirrored horizontally, mipmapped) but uses DirectWrite/
// Direct2D instead of Core Text.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

class GlyphAtlas {
public:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    // (Re)build the atlas. `cellPx` and `cols` match the macOS atlas defaults.
    bool Build(ID3D11Device* device, ID3D11DeviceContext* ctx,
               int cellPx = 72, int cols = 16);

    ID3D11ShaderResourceView* SRV() const { return srv_.Get(); }
    bool   HasTexture() const { return srv_ != nullptr; }
    int    Cols() const { return cols_; }
    int    Rows() const { return rows_; }
    int    GlyphCount() const { return glyphCount_; }

private:
    ComPtr<ID3D11Texture2D>          tex_;
    ComPtr<ID3D11ShaderResourceView> srv_;
    int cols_ = 16, rows_ = 1, glyphCount_ = 1;
};
