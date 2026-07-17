// shaders.hlsl — Direct3D 11 port of Resources/Shaders.metal.
cbuffer Uniforms : register(b0)
{
    row_major float4x4 viewProj;
    float camX, camY, camZ;
    float camRX, camRY, camRZ;
    float camUX, camUY, camUZ;

    float glyphHalf, atlasCols, atlasRows, time;
    float fogEnabled, fogStartDist, fogEndDist;
    float textured, wireframe, extraContrastHeads, pad1;
};

struct GlyphInstance
{
    float px, py, pz;
    float cell;
    float flipX;
    float flipY;
    float bright;
    float cr, cg, cb;
};

StructuredBuffer<GlyphInstance> insts : register(t1);
Texture2D    atlasTex : register(t0);
SamplerState samp     : register(s0);

struct VSOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float2 local    : TEXCOORD1;
    float3 color    : TEXCOORD2;
    float  bright   : TEXCOORD3;
    float  fog      : TEXCOORD4;
    float  shimmer  : TEXCOORD5; 
};

VSOut glyph_vertex(uint vid : SV_VertexID, uint iid : SV_InstanceID)
{
    GlyphInstance g = insts[iid];
    float2 corner = float2((vid == 1 || vid == 3) ? 1.0 : -1.0,
                           (vid >= 2) ? 1.0 : -1.0);
    
    float3 pos   = float3(g.px, g.py, g.pz);
    float3 right = float3(camRX, camRY, camRZ);
    float3 up    = float3(camUX, camUY, camUZ);
    float3 world = pos + right * (corner.x * glyphHalf)
                       + up    * (corner.y * glyphHalf);
    
    VSOut o;
    o.position = mul(float4(world, 1.0), viewProj);

    // Apply flip to the generated local corners before creating UVs
    float flippedCornerX = corner.x * g.flipX;
    float flippedCornerY = corner.y * g.flipY;
    
    float2 cellUV = float2(flippedCornerX * 0.5 + 0.5, 0.5 - flippedCornerY * 0.5);
    o.local = cellUV;

    float col = fmod(g.cell, atlasCols);
    float row = floor(g.cell / atlasCols);
    o.uv = (float2(col, row) + cellUV) / float2(atlasCols, atlasRows);

    float3 cam  = float3(camX, camY, camZ);
    float  dist = length(pos - cam);

    o.fog   = (fogEnabled > 0.5) ?
        (1.0 - smoothstep(fogStartDist, fogEndDist, dist)) : 1.0;
    o.color = float3(g.cr, g.cg, g.cb);
    o.bright = g.bright;
    
    // SMART SHIMMER: Calculated based on the glyph root (g.py).
    // Uniform pulsing across the character, completely free for the GPU.
    o.shimmer = 0.9 + 0.1 * sin(time * 5.0 + g.py * 0.1);
    
    return o;
}

float4 glyph_fragment(VSOut i) : SV_Target
{
    float coverage;
    if (wireframe > 0.5) {
        float2 d = min(i.local, 1.0 - i.local);
        float edge = min(d.x, d.y);
        coverage = 1.0 - smoothstep(0.04, 0.06, edge);
    } else if (textured > 0.5) {
        coverage = atlasTex.Sample(samp, i.uv).r;
    } else {
        coverage = 1.0;
    }

    // SAFE CULLING: 0.01 preserves soft anti-aliasing edges 
    // but still drops dead space to save massive GPU fill-rate.
    if (coverage < 0.01) discard;

    float b = pow(max(i.bright, 0.0), 1.2) * i.fog * i.shimmer;
    
    // Base color now comes from the per-instance color computed in
    // mmcore.c (near-white/green for the head flash, green with a faint
    // per-column hue tint for the trail) instead of a fixed constant --
    // this is what makes the per-column hue variation actually visible.
    float3 base = i.color * b;

    // "Extra-contrast heads" is a pure recolor: it desaturates the head
    // (and very-near-head trailing glyphs) toward white while preserving
    // luminance, so it does NOT pump extra energy into the bloom buffer.
    // This is deliberately a wider/softer mask than headMask below --
    // it's meant to reach a few glyphs into the column, not just the head.
    //
    // NOTE: i.bright is scaled by 0.80 in mm_sim_write_instances (mmcore.c),
    // so it never reaches 1.0 -- the mask's top edge must sit at or below
    // that real ceiling (~0.80), not at 1.0, or this is always zero. Gated
    // entirely by the extraContrastHeads multiplier, so toggling this off
    // leaves the base look completely unchanged.
    float contrastMask = smoothstep(0.76, 0.80, i.bright) * extraContrastHeads;
    float lum = dot(base, float3(0.299, 0.587, 0.114));
    float3 whiteAtSameLum = float3(lum, lum, lum);
    base = lerp(base, whiteAtSameLum, contrastMask);

    // headMask / whiteFlash is the existing additive "hot" term that bloom
    // reads from -- untouched, so normal glow behavior is unchanged.
    float headMask = smoothstep(0.95, 1.0, i.bright);
    float3 whiteFlash = float3(0.35, 0.35, 0.3) * headMask;
    
    float3 rgb = (base + whiteFlash) * coverage * 1.5;
    float a = coverage * smoothstep(0.0, 0.15, b);
    
    return float4(rgb, a);
}

// ---- Bloom / post-processing -------------------------------------------------

cbuffer PostParams : register(b0)
{
    float4 post;
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

float4 bloom_threshold(FSQuad i) : SV_Target
{
    // 4-tap box downsample instead of a single bilinear tap: at half-res,
    // one tap can straddle or miss thin bright glyph strokes entirely,
    // which is what reads as blocky/flickery bloom.
    // post.zw carries the full-res texel size, so this costs 3 extra samples, once, at half-res.
    float2 texel = post.zw;
    float3 c = postSrc.Sample(samp, i.uv + texel * float2(-0.5, -0.5)).rgb
             + postSrc.Sample(samp, i.uv + texel * float2( 0.5, -0.5)).rgb
             + postSrc.Sample(samp, i.uv + texel * float2(-0.5,  0.5)).rgb
             + postSrc.Sample(samp, i.uv + texel * float2( 0.5,  0.5)).rgb;
    c *= 0.25;

    float threshold = post.x * 0.8;
    float lum = max(max(c.r, c.g), c.b);
    
    float softKnee = 0.3;
    float k = smoothstep(threshold - softKnee, threshold + softKnee, lum);
    
    return float4(c * k * 1.5, 1.0);
}

float4 bloom_downsample(FSQuad i) : SV_Target
{
    float2 uv = i.uv;
    float x = post.z; // Source texel width
    float y = post.w; // Source texel height

    float3 a = postSrc.Sample(samp, float2(uv.x - 2*x, uv.y + 2*y)).rgb;
    float3 b = postSrc.Sample(samp, float2(uv.x,       uv.y + 2*y)).rgb;
    float3 c = postSrc.Sample(samp, float2(uv.x + 2*x, uv.y + 2*y)).rgb;

    float3 d = postSrc.Sample(samp, float2(uv.x - 2*x, uv.y)).rgb;
    float3 e = postSrc.Sample(samp, float2(uv.x,       uv.y)).rgb;
    float3 f = postSrc.Sample(samp, float2(uv.x + 2*x, uv.y)).rgb;
    
    float3 g = postSrc.Sample(samp, float2(uv.x - 2*x, uv.y - 2*y)).rgb;
    float3 h = postSrc.Sample(samp, float2(uv.x,       uv.y - 2*y)).rgb;
    float3 j = postSrc.Sample(samp, float2(uv.x + 2*x, uv.y - 2*y)).rgb;

    float3 k = postSrc.Sample(samp, float2(uv.x - x, uv.y + y)).rgb;
    float3 l = postSrc.Sample(samp, float2(uv.x + x, uv.y + y)).rgb;
    float3 m = postSrc.Sample(samp, float2(uv.x - x, uv.y - y)).rgb;
    float3 n = postSrc.Sample(samp, float2(uv.x + x, uv.y - y)).rgb;

    float3 color = e * 0.125;
    color += (a + c + g + j) * 0.03125;
    color += (b + d + f + h) * 0.0625;
    color += (k + l + m + n) * 0.125;

    return float4(color, 1.0);
}

float4 bloom_upsample(FSQuad i) : SV_Target
{
    float2 uv = i.uv;
    float x = post.z; // Source texel width (from the smaller mip)
    float y = post.w; // Source texel height

    float3 a = postSrc.Sample(samp, float2(uv.x - x, uv.y + y)).rgb;
    float3 b = postSrc.Sample(samp, float2(uv.x,     uv.y + y)).rgb;
    float3 c = postSrc.Sample(samp, float2(uv.x + x, uv.y + y)).rgb;

    float3 d = postSrc.Sample(samp, float2(uv.x - x, uv.y)).rgb;
    float3 e = postSrc.Sample(samp, float2(uv.x,     uv.y)).rgb;
    float3 f = postSrc.Sample(samp, float2(uv.x + x, uv.y)).rgb;
    
    float3 g = postSrc.Sample(samp, float2(uv.x - x, uv.y - y)).rgb;
    float3 h = postSrc.Sample(samp, float2(uv.x,     uv.y - y)).rgb;
    float3 j = postSrc.Sample(samp, float2(uv.x + x, uv.y - y)).rgb;

    float3 color = e * 4.0;
    color += (b + d + f + h) * 2.0;
    color += (a + c + g + j) * 1.0;
    
    color *= (1.0 / 16.0);

    return float4(color, 1.0);
}

// Warp a UV around the screen center. `amount` > 0 bows the image outward
// (barrel/CRT-glass curvature), strongest at the edges and ~0 at the center.
float2 BarrelDistort(float2 uv, float amount)
{
    float2 c = uv - 0.5;
    float r2 = dot(c, c);
    float2 warped = c * (1.0 + amount * r2);
    return warped + 0.5;
}

// ---- Beige-CRT emulation (separate from BarrelDistort/bloom_composite's ---------
// ---- barrel-warp + simple chromatic-aberration slider) --------------------------
//
// Models the specific visual signature of a late-1990s consumer shadow-mask CRT
// (the kind of beige box this rain was originally meant to run on), as opposed to
// generic "TV static" filters. Layered, in order, on top of whatever
// bloom_composite already produced:
//
//   1. Shadow-mask / aperture-grille RGB triads -- the sub-pixel structure that
//      gives real CRTs their characteristic soft-edged, slightly fuzzy look up
//      close, most visible in bright areas.
//   2. Horizontal scanlines -- gaps between raster lines, with a faint per-line
//      brightness ripple (CRTs are never perfectly uniform between lines).
//   3. Phosphor glow bleed -- a short horizontal-biased blur that stands in for
//      electron-beam spot spread / phosphor persistence, distinct from the
//      threshold-based emissive "bloom" pipeline (which only blooms very bright
//      pixels; this bleeds everything a little, the way real phosphor does).
//   4. Convergence fringing -- a small constant per-channel offset (not radial
//      like the existing chromatic-aberration slider) mimicking a shadow mask
//      whose three electron beams never perfectly re-converge, worst on
//      contrasty vertical edges (which is exactly what glyph strokes are).
//   5. Black-level lift + soft S-curve -- consumer CRTs rarely hit a true black
//      and roll off highlights themselves (independent of the HDR panel output
//      elsewhere in the pipeline).
//   6. Mask-edge vignette -- shadow masks were slightly domed, so brightness and
//      mask visibility both fall off a bit faster at the corners than a modern
//      flat panel's uniform backlight.
//
// post.xy = destination texel size (1/width, 1/height), post.z = time (for
// the phosphor-flicker term below), post.w unused -- same convention as the
// other fullscreen passes above, just reusing the existing PostParams cbuffer
// rather than binding a second one at the same register.
float4 crt_filter(FSQuad i) : SV_Target
{
    float2 texel = post.xy;
    float  t     = post.z;

    // --- 1. Shadow-mask / aperture-grille RGB triads --------------------------
    // A real mask's triad pitch is far finer than a scanline; ~2.2 output
    // pixels per triad reads as "CRT-close-up" without aliasing into moire
    // at typical screensaver viewing distances/resolutions.
    const float kTriadPx = 2.2;
    float triadPhase = frac(i.position.x / kTriadPx);
    float3 mask;
    if (triadPhase < 1.0 / 3.0)      mask = float3(1.08, 0.82, 0.82);
    else if (triadPhase < 2.0 / 3.0) mask = float3(0.82, 1.08, 0.82);
    else                              mask = float3(0.82, 0.82, 1.08);
    // Mask contrast is strongest on a lit phosphor and nearly invisible in
    // dark areas on a real tube (the black areas aren't lit by anything for
    // the mask to filter) -- softened further below once we have `src`.

    // --- 2. Horizontal scanlines -----------------------------------------------
    // Modeled directly in output-pixel space (not a fixed line count) so this
    // stays correct across window sizes/resolutions instead of only looking
    // right at one reference resolution.
    const float kScanlinePx = 2.0;
    float scanPhase = frac(i.position.y / kScanlinePx);
    float scanline = 0.72 + 0.28 * smoothstep(0.0, 0.5, 1.0 - abs(scanPhase * 2.0 - 1.0));
    // Faint per-line brightness ripple -- real CRTs are never perfectly even
    // line-to-line (deflection coil + PSU ripple).
    float lineRipple = 0.985 + 0.015 * sin(i.position.y * 1.7 + t * 0.6);
    scanline *= lineRipple;

    // --- 3. Phosphor glow bleed --------------------------------------------------
    // Short horizontal-biased 1D blur (electron beam spot spread is wider
    // horizontally than the scanline pitch enforces vertically). Cheap
    // 5-tap separable kernel sampled directly from postSrc; deliberately not
    // routed through the existing bloomMip chain, since that only picks up
    // pixels above the bloom threshold and this needs to soften everything
    // a little, the way phosphor persistence does regardless of brightness.
    float3 bleed =
        postSrc.Sample(samp, i.uv + float2(-2.0 * texel.x, 0)).rgb * 0.06 +
        postSrc.Sample(samp, i.uv + float2(-1.0 * texel.x, 0)).rgb * 0.18 +
        postSrc.Sample(samp, i.uv).rgb                             * 0.52 +
        postSrc.Sample(samp, i.uv + float2( 1.0 * texel.x, 0)).rgb * 0.18 +
        postSrc.Sample(samp, i.uv + float2( 2.0 * texel.x, 0)).rgb * 0.06;

    // --- 4. Convergence fringing -------------------------------------------------
    // Small constant (non-radial) per-channel offset -- a shadow mask's three
    // beams drift out of registration uniformly, not more at the edges the
    // way the existing barrel-distortion CA slider models lens aberration.
    const float kConvergePx = 0.6;
    float2 convR = float2( kConvergePx * texel.x, 0);
    float2 convB = float2(-kConvergePx * texel.x, 0);
    float3 converged;
    converged.r = postSrc.Sample(samp, i.uv + convR).r;
    converged.g = bleed.g;
    converged.b = postSrc.Sample(samp, i.uv + convB).b;
    // Blend the fringed sample in lightly against the un-fringed bleed --
    // full-strength constant fringing reads as broken alignment rather than
    // "old CRT"; real convergence error is subtle except on hard edges.
    float3 withFringe = lerp(bleed, converged, 0.5);

    float3 color = withFringe;

    // Apply mask (softened in dark regions -- see note above) and scanlines.
    float lum = dot(color, float3(0.299, 0.587, 0.114));
    float maskStrength = 0.85 * saturate(lum * 2.0);
    color *= lerp(1.0, mask, maskStrength);
    color *= scanline;

    // --- 5. Black-level lift + soft highlight rolloff ---------------------------
    // Consumer CRTs never reached a true 0 (residual phosphor persistence /
    // ambient tube glow) and compress highlights themselves rather than
    // clipping hard.
    const float kBlackLift = 0.02;
    color = kBlackLift + color * (1.0 - kBlackLift);
    color = color / (1.0 + color * 0.15); // soft highlight knee

    // --- 6. Mask-edge vignette ---------------------------------------------------
    // A physically domed shadow mask + electron-beam falloff toward the
    // corners -- gentler and differently-shaped than bloom_composite's own
    // vignette, since that one is modeling the glass/lens, not the mask.
    float2 c = i.uv - 0.5;
    float  cornerDist = dot(c, c);
    float  maskVignette = 1.0 - 0.22 * smoothstep(0.12, 0.5, cornerDist);
    color *= maskVignette;

    return float4(color, 1.0);
}

float4 bloom_composite(FSQuad i) : SV_Target
{
    float intensity   = post.x;
    float distortAmt  = post.y; // barrel warp strength
    float caAmt        = post.w * 0.5; // max per-channel UV offset near edges

    float2 center = i.uv - 0.5;
    float  edge   = dot(center, center);   // 0 at center, ~0.5 at corners

    float2 uvBase = BarrelDistort(i.uv, distortAmt);
    float2 dir  = normalize(center + 1e-5);
    float2 offR =  dir * caAmt * edge;
    float2 offB = -dir * caAmt * edge;

    float3 s;
    s.r = postSrc.Sample(samp, uvBase + offR).r;
    s.g = postSrc.Sample(samp, uvBase).g;
    s.b = postSrc.Sample(samp, uvBase + offB).b;

    float3 b = postBlur.Sample(samp, uvBase).rgb;

    float dist = length(center);
    float vignette = 1.0 - smoothstep(0.3, 1.0, dist); 
    
    float bloom_lum = dot(b, float3(0.299, 0.587, 0.114));
    float alpha = saturate(bloom_lum * intensity);

    // Fade to black BEFORE the distorted UV (or its CA-offset siblings) reach
    // the texture border.
    // Past that point the CLAMP sampler just repeats the
    // edge texel, which is what stretched into visible streaks.
    // Fading over a small margin hides the clamp region entirely instead of exposing it.
    float2 margin = 0.006 + distortAmt * 0.16 + abs(offR);
    // widen the margin by the max CA offset
    float2 m = smoothstep(0.0, margin, uvBase) * smoothstep(0.0, margin, 1.0 - uvBase);
    float edgeFade = m.x * m.y;

    return float4((s + b * intensity) * vignette * edgeFade, alpha * edgeFade);
}