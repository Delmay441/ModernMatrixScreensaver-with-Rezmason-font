import simd

// These structs are mirrored field-for-field in Resources/Shaders.metal.
// Keep them as plain Float members (no SIMD3 padding surprises) so the layout
// is identical on both sides without a bridging header. The only 16-byte-aligned
// member (the matrix) is first, so there is no interior padding.
//
// The per-glyph instance struct now lives in the shared C engine as `MMGlyphInstance`
// (core/mmcore.h) — same 8-float layout — so the simulation writes instances straight
// into the Metal vertex buffer. `Uniforms` stays here (it's Metal-renderer-specific).

struct Uniforms {
    var viewProj = matrix_identity_float4x4

    var camX: Float = 0, camY: Float = 0, camZ: Float = 0
    var camRX: Float = 1, camRY: Float = 0, camRZ: Float = 0   // camera right
    var camUX: Float = 0, camUY: Float = 1, camUZ: Float = 0   // camera up

    var glyphHalf: Float = 0.5
    var atlasCols: Float = 16
    var atlasRows: Float = 1
    var time: Float = 0

    var fogEnabled: Float = 1
    var fogStartDist: Float = 30
    var fogEndDist: Float = 90

    var textured: Float = 1
    var wireframe: Float = 0
    var pad0: Float = 0
    var pad1: Float = 0
}
