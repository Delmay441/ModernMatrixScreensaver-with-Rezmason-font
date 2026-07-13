import Metal
import CoreText
import CoreGraphics

/// A single-channel (coverage) texture holding the glyph set for an encoding,
/// laid out row-major in `cols` × `rows` cells. Cell index == glyph index, so the
/// simulation can pick `Int.random(in: 0..<glyphCount)` directly.
final class GlyphAtlas {
    let texture: MTLTexture
    let cols: Int
    let rows: Int
    let glyphCount: Int

    init(device: MTLDevice, encoding: GlyphEncoding, mirrored: Bool = true,
         cellPx: Int = 72, cols: Int = 16)
    {
        let scalars = GlyphAtlas.scalars(for: encoding)
        let count = scalars.count
        let rowCount = max(1, (count + cols - 1) / cols)
        self.cols = cols
        self.rows = rowCount
        self.glyphCount = count

        let width = cols * cellPx
        let height = rowCount * cellPx
        let bytesPerRow = width

        let gray = CGColorSpaceCreateDeviceGray()
        let ctx = CGContext(data: nil, width: width, height: height,
                            bitsPerComponent: 8, bytesPerRow: bytesPerRow,
                            space: gray, bitmapInfo: CGImageAlphaInfo.none.rawValue)!
        ctx.setFillColor(gray: 0, alpha: 1)
        ctx.fill(CGRect(x: 0, y: 0, width: width, height: height))
        ctx.setFillColor(gray: 1, alpha: 1)
        ctx.setAllowsAntialiasing(true)
        ctx.setShouldAntialias(true)

        let font = GlyphAtlas.makeFont(size: CGFloat(cellPx) * 0.82)
        let cell = CGFloat(cellPx)

        for (i, scalar) in scalars.enumerated() {
            let col = i % cols
            let row = i / cols
            // CGContext is bottom-up; place row 0 at the top.
            let originX = CGFloat(col) * cell
            let originY = CGFloat(rowCount - 1 - row) * cell

            var glyph: CGGlyph = 0
            var ch = UTF16Char(scalar.value > 0xFFFF ? 0x3013 : UInt16(scalar.value))
            guard CTFontGetGlyphsForCharacters(font, &ch, &glyph, 1), glyph != 0 else { continue }

            var bbox = CGRect.zero
            withUnsafePointer(to: glyph) { gp in
                bbox = CTFontGetBoundingRectsForGlyphs(font, .horizontal, gp, nil, 1)
            }
            if bbox.isNull || bbox.isInfinite { continue }

            let gx = originX + (cell - bbox.width) / 2 - bbox.minX
            let gy = originY + (cell - bbox.height) / 2 - bbox.minY

            ctx.saveGState()
            if mirrored {
                let cx = originX + cell / 2
                ctx.translateBy(x: cx, y: 0)
                ctx.scaleBy(x: -1, y: 1)
                ctx.translateBy(x: -cx, y: 0)
            }
            let pos = CGPoint(x: gx, y: gy)
            withUnsafePointer(to: glyph) { gp in
                withUnsafePointer(to: pos) { pp in
                    CTFontDrawGlyphs(font, gp, pp, 1, ctx)
                }
            }
            ctx.restoreGState()
        }

        let desc = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r8Unorm, width: width, height: height, mipmapped: true)
        desc.usage = [.shaderRead]
        desc.storageMode = .shared
        let tex = device.makeTexture(descriptor: desc)!
        tex.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0,
                    withBytes: ctx.data!, bytesPerRow: bytesPerRow)
        self.texture = tex

        // Generate mipmaps so distant glyphs don't shimmer.
        if let queue = device.makeCommandQueue(),
           let cb = queue.makeCommandBuffer(),
           let blit = cb.makeBlitCommandEncoder() {
            blit.generateMipmaps(for: tex)
            blit.endEncoding()
            cb.commit()
            cb.waitUntilCompleted()
        }
    }

    private static func makeFont(size: CGFloat) -> CTFont {
        for name in ["Hiragino Sans", "Hiragino Kaku Gothic Pro", "Osaka", "Menlo"] {
            let f = CTFontCreateWithName(name as CFString, size, nil)
            if CTFontGetGlyphCount(f) > 0 { return f }
        }
        return CTFontCreateUIFontForLanguage(.system, size, nil)
            ?? CTFontCreateWithName("Helvetica" as CFString, size, nil)
    }

    /// The code points for each encoding come from the shared C engine
    /// (`mm_encoding_codepoints`), so the glyph set is defined once for both platforms.
    /// The atlas rasterises them in order, so cell index == index here.
    static func scalars(for encoding: GlyphEncoding) -> [Unicode.Scalar] {
        var buf = [UInt32](repeating: 0, count: Int(MM_MAX_CODEPOINTS))
        let n = Int(mm_encoding_codepoints(Int32(encoding.rawValue), &buf, Int32(buf.count)))
        return buf.prefix(n).compactMap { Unicode.Scalar($0) }
    }
}
