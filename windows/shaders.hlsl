// shaders.hlsl — Direct3D 11 port of Resources/Shaders.metal.
//
// Ported 1:1 from the macOS Metal shaders so the look matches exactly:
//   glyph_vertex / glyph_fragment  — instanced camera-facing billboards
//   fullscreen_vertex              — fullscreen triangle-strip quad
//   bloom_threshold / bloom_blur / bloom_composite — post-processing
//
// Conventions vs Metal:
//   * Metal uses column-vector math (viewProj * pos). Here we upload the matrix
//     row-major (XMFLOAT4X4, no transpose) and declare it `row_major`, then use
//     row-vector math: mul(pos, viewProj). Same clip-space result.
//   * XMMatrixLookAtRH / XMMatrixPerspectiveFovRH give RH + [0,1] depth, matching
//     Metal's NDC, so near/far/fovy map straight across.
//   * The per-glyph instance is fetched from a StructuredBuffer by SV_InstanceID,
//     mirroring Metal's `insts[iid]` (no input layout / vertex buffer needed).

// ----- shared with C++ host (windows/renderer.h Uniforms); field order identical.
// Scalar floats only, matching ShaderTypes.swift, so the cbuffer packs to the same
// 144-byte tight layout the host uploads (HLSL packs scalars 4-per-16B register).
cbuffer Uniforms : register(b0)
{
    row_major float4x4 viewProj;

    float camX, camY, camZ;       // camera eye
    float camRX, camRY, camRZ;    // camera right
    float camUX, camUY, camUZ;    // camera up

    float glyphHalf, atlasCols, atlasRows, time;
    float fogEnabled, fogStartDist, fogEndDist;
    float textured, wireframe, pad0, pad1;
};

struct GlyphInstance        // matches core/mmcore.h MMGlyphInstance (8 floats)
{
    float px, py, pz;
    float cell;
    float bright;
    float cr, cg, cb;
};

StructuredBuffer<GlyphInstance> insts : register(t1);
Texture2D    atlasTex : register(t0);
SamplerState samp     : register(s0);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;   // atlas-space
    float2 local    : TEXCOORD1;   // cell-local [0,1]
    float3 color    : TEXCOORD2;
    float  bright   : TEXCOORD3;
    float  fog      : TEXCOORD4;
};

VSOut glyph_vertex(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    GlyphInstance g = insts[iid];

    // Camera-facing billboard quad (triangle strip: 0,1,2,3).
    float2 corner = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                           (vid >= 2) ? 1.0 : -1.0);
    float3 pos   = float3(g.px, g.py, g.pz);
    float3 right = float3(camRX, camRY, camRZ);
    float3 up    = float3(camUX, camUY, camUZ);
    float3 world = pos + right * (corner.x * glyphHalf)
                       + up    * (corner.y * glyphHalf);

    VSOut o;
    o.position = mul(float4(world, 1.0), viewProj);

    float2 cellUV = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5); // v points down
    o.local = cellUV;
    float col = fmod(g.cell, atlasCols);
    float row = floor(g.cell / atlasCols);
    o.uv = (float2(col, row) + cellUV) / float2(atlasCols, atlasRows);

    float3 cam  = float3(camX, camY, camZ);
    float  dist = length(pos - cam);
    o.fog   = (fogEnabled > 0.5) ? (1.0 - smoothstep(fogStartDist, fogEndDist, dist)) : 1.0;
    o.color = float3(g.cr, g.cg, g.cb);
    o.bright = g.bright;
    return o;
}

float4 glyph_fragment(VSOut i) : SV_Target
{
    float coverage;
    if (wireframe > 0.5) {
        // Thin box outline of each glyph cell.
        float2 d = min(i.local, 1.0 - i.local);
        float edge = min(d.x, d.y);
        coverage = 1.0 - smoothstep(0.04, 0.06, edge);
    } else if (textured > 0.5) {
        coverage = atlasTex.Sample(samp, i.uv).r;
    } else {
        coverage = 1.0;
    }

    float  b   = i.bright * i.fog;
    float3 rgb = i.color * b * coverage;
    float  a   = coverage * clamp(b, 0.0, 1.0);
    return float4(rgb, a);   // premultiplied; blend = ONE / INV_SRC_ALPHA
}

// ---- Bloom / post-processing -------------------------------------------------

cbuffer PostParams : register(b0)
{
    float4 post;   // threshold: post.x = knee; blur: post.xy = dir; composite: post.x = intensity
};

Texture2D postSrc  : register(t0);
Texture2D postBlur : register(t1);

struct FSQuad
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

FSQuad fullscreen_vertex(uint vid : SV_VertexID)
{
    float2 c = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                      (vid >= 2) ? 1.0 : -1.0);
    FSQuad o;
    o.position = float4(c, 0.0, 1.0);
    o.uv = float2(c.x * 0.5 + 0.5, 0.5 - c.y * 0.5);
    return o;
}

// Bright-pass: keep only what exceeds the knee, for the glow source.
float4 bloom_threshold(FSQuad i) : SV_Target
{
    float knee = post.x;
    float3 c = postSrc.Sample(samp, i.uv).rgb;
    float lum = max(max(c.r, c.g), c.b);
    float k = max(lum - knee, 0.0) / max(lum, 1e-4);
    return float4(c * k, 1.0);
}

// Separable Gaussian blur (direction passed as a texel-space offset).
float4 bloom_blur(FSQuad i) : SV_Target
{
    float2 dir = post.xy;
    const float w0 = 0.227027, w1 = 0.316216, w2 = 0.070270;
    const float o1 = 1.384615, o2 = 3.230769;
    float3 c = postSrc.Sample(samp, i.uv).rgb * w0;
    c += postSrc.Sample(samp, i.uv + dir * o1).rgb * w1;
    c += postSrc.Sample(samp, i.uv - dir * o1).rgb * w1;
    c += postSrc.Sample(samp, i.uv + dir * o2).rgb * w2;
    c += postSrc.Sample(samp, i.uv - dir * o2).rgb * w2;
    return float4(c, 1.0);
}

// Composite scene + blurred bloom into the final target.
float4 bloom_composite(FSQuad i) : SV_Target
{
    float intensity = post.x;
    float3 s = postSrc.Sample(samp, i.uv).rgb;
    float3 b = postBlur.Sample(samp, i.uv).rgb;
    return float4(s + b * intensity, 1.0);
}
