#include <metal_stdlib>
using namespace metal;

// Mirrors Sources/Core/ShaderTypes.swift — keep field order identical.

struct GlyphInstance {
    float px, py, pz;
    float cell;
    float bright;
    float cr, cg, cb;
};

struct Uniforms {
    float4x4 viewProj;
    float camX, camY, camZ;
    float camRX, camRY, camRZ;
    float camUX, camUY, camUZ;
    float glyphHalf, atlasCols, atlasRows, time;
    float fogEnabled, fogStartDist, fogEndDist;
    float textured, wireframe, pad0, pad1;
};

struct VSOut {
    float4 position [[position]];
    float2 uv;       // atlas-space
    float2 local;    // cell-local [0,1]
    float3 color;
    float bright;
    float fog;
};

vertex VSOut glyph_vertex(uint vid [[vertex_id]],
                          uint iid [[instance_id]],
                          const device GlyphInstance *insts [[buffer(0)]],
                          constant Uniforms &U [[buffer(1)]])
{
    GlyphInstance g = insts[iid];

    // Camera-facing billboard quad (triangle strip: 0,1,2,3).
    float2 corner = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                           (vid >= 2) ? 1.0 : -1.0);
    float3 pos   = float3(g.px, g.py, g.pz);
    float3 right = float3(U.camRX, U.camRY, U.camRZ);
    float3 up    = float3(U.camUX, U.camUY, U.camUZ);
    float3 world = pos + right * (corner.x * U.glyphHalf)
                       + up    * (corner.y * U.glyphHalf);

    VSOut o;
    o.position = U.viewProj * float4(world, 1.0);

    float2 cellUV = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5); // v points down
    o.local = cellUV;
    float col = fmod(g.cell, U.atlasCols);
    float row = floor(g.cell / U.atlasCols);
    o.uv = (float2(col, row) + cellUV) / float2(U.atlasCols, U.atlasRows);

    float3 cam = float3(U.camX, U.camY, U.camZ);
    float dist = length(pos - cam);
    o.fog = (U.fogEnabled > 0.5) ? (1.0 - smoothstep(U.fogStartDist, U.fogEndDist, dist)) : 1.0;
    o.color  = float3(g.cr, g.cg, g.cb);
    o.bright = g.bright;
    return o;
}

fragment float4 glyph_fragment(VSOut in [[stage_in]],
                               constant Uniforms &U [[buffer(1)]],
                               texture2d<float> atlas [[texture(0)]],
                               sampler samp [[sampler(0)]])
{
    float coverage;
    if (U.wireframe > 0.5) {
        // Thin box outline of each glyph cell.
        float2 d = min(in.local, 1.0 - in.local);
        float edge = min(d.x, d.y);
        coverage = 1.0 - smoothstep(0.04, 0.06, edge);
    } else if (U.textured > 0.5) {
        coverage = atlas.sample(samp, in.uv).r;
    } else {
        coverage = 1.0;
    }

    float b = in.bright * in.fog;
    float3 rgb = in.color * b * coverage;
    float a = coverage * clamp(b, 0.0, 1.0);
    return float4(rgb, a);
}

// ---- Bloom / post-processing (used from the renderer's later passes) ----

struct FSQuad {
    float4 position [[position]];
    float2 uv;
};

vertex FSQuad fullscreen_vertex(uint vid [[vertex_id]])
{
    // Two-triangle fullscreen quad from a 4-vertex strip.
    float2 c = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                      (vid >= 2) ? 1.0 : -1.0);
    FSQuad o;
    o.position = float4(c, 0.0, 1.0);
    o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    return o;
}

// Bright-pass: keep only what exceeds the knee, for the glow source.
fragment float4 bloom_threshold(FSQuad in [[stage_in]],
                                texture2d<float> src [[texture(0)]],
                                sampler samp [[sampler(0)]],
                                constant float &knee [[buffer(0)]])
{
    float3 c = src.sample(samp, in.uv).rgb;
    float lum = max(max(c.r, c.g), c.b);
    float k = max(lum - knee, 0.0) / max(lum, 1e-4);
    return float4(c * k, 1.0);
}

// Separable Gaussian blur (direction passed as a texel-space offset).
fragment float4 bloom_blur(FSQuad in [[stage_in]],
                           texture2d<float> src [[texture(0)]],
                           sampler samp [[sampler(0)]],
                           constant float2 &dir [[buffer(0)]])
{
    const float w0 = 0.227027, w1 = 0.316216, w2 = 0.070270;
    const float o1 = 1.384615, o2 = 3.230769;
    float3 c = src.sample(samp, in.uv).rgb * w0;
    c += src.sample(samp, in.uv + dir * o1).rgb * w1;
    c += src.sample(samp, in.uv - dir * o1).rgb * w1;
    c += src.sample(samp, in.uv + dir * o2).rgb * w2;
    c += src.sample(samp, in.uv - dir * o2).rgb * w2;
    return float4(c, 1.0);
}

// Composite scene + blurred bloom into the final drawable.
fragment float4 bloom_composite(FSQuad in [[stage_in]],
                                texture2d<float> scene [[texture(0)]],
                                texture2d<float> blur  [[texture(1)]],
                                sampler samp [[sampler(0)]],
                                constant float &intensity [[buffer(0)]])
{
    float3 s = scene.sample(samp, in.uv).rgb;
    float3 b = blur.sample(samp, in.uv).rgb;
    return float4(s + b * intensity, 1.0);
}
