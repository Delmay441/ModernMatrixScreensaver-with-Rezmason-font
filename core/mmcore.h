#ifndef MMCORE_H
#define MMCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ Settings -- */

typedef struct {
    double density;
    double speed;
    double bloomIntensity;
    int    fog, waves, panning, textured, wireframe, showFPS, bloom, hdr;
    float  glyphScale;
    double lengthBias;
    float  current_exponent;
    double crtDistort;   // 0..1 — screen-curvature / chromatic-aberration strength
    double depthAmount;
    double cameraSpeed;   // 0..1 — dolly speed through the depth field (0.5 == old fixed default)
    double mutationRate;  // 0..1 — independent multiplier on base glyph mutation rate (0.5 == old fixed default)
    int    easterEggs;    // 1 = allow rare red glitch columns, 0 = disable them entirely
	int    batterySaver;  // 1 = cap presentation to ~60 Hz regardless of display refresh rate
	float  mainColorR, mainColorG, mainColorB;
    float  glitchColorR, glitchColorG, glitchColorB;
    int    flipXEnabled;  // 1 = allow random per-glyph horizontal mirroring, 0 = never flip horizontally
    int    flipYEnabled;  // 1 = allow random per-glyph vertical mirroring, 0 = never flip vertically
    int    binaryMode;    // 1 = restrict glyph selection to only "0"/"1" (classic binary rain look)
    int    columnGaps;    // 1 = allow random blank cells within a column, 0 = dense/no gaps
    int    extraContrastHeads; // 1 = recolor column heads (and near-head glyphs) toward white
                               // at constant luminance -- a "simulated bloom" independent of
                               // the real bloom pipeline; does not affect actual brightness.
    int    crtEmulation;  // 1 = emulate a late-90s beige shadow-mask CRT (scanlines, mask
                           // triads, phosphor glow bleed, convergence fringing, black lift).
                           // Independent of crtDistort (which is just barrel warp + a
                           // simple radial chromatic-aberration slider); this is a whole
                           // separate post pass, toggled on/off rather than blended by amount.
} MMSettings;

#define MM_CAMERA_EYE_Z 48.0f

// Fixed indices of the '0' and '1' glyphs within atlas.cpp's cps[] codepoint
// table (0x0030 and 0x0031 respectively, the 4th/5th entries). Binary mode
// restricts glyph selection to just these two cells instead of the full set.
#define MM_BINARY_GLYPH_ZERO_INDEX 3
#define MM_BINARY_GLYPH_ONE_INDEX  4

// Fixed index (within atlas.cpp's cps[] table) of the colon glyph, used
// together with MM_BINARY_GLYPH_ONE_INDEX above by the "11:11" easter egg
// (see mm_sim_advance / eleven_pattern_glyph in mmcore.c). Both glyphs
// happen to need no flip correction to read correctly, so unlike the old
// embedded-clock feature this needs no per-character flip table.
#define MM_GLYPH_COLON_INDEX 12
// sim->glyphCount must exceed this for the pattern to be safely used
// against the current atlas.
#define MM_ELEVEN_MAX_GLYPH_INDEX MM_GLYPH_COLON_INDEX

MMSettings mm_settings_default(void);

int   mm_strip_count(const MMSettings *s);
float mm_fall_speed(const MMSettings *s);
float mm_mutation_rate(const MMSettings *s);

// Reference aspect ratio (16:9) the kNarrowWidth/kWideWidth grid-width
// constants in mm_strip_count() were tuned against. mm_sim_create/
// mm_sim_update widen the lane grid proportionally above this ratio so
// ultra-widescreen displays (21:9, 32:9, triple-monitor spans, etc.) get a
// rain field that fills the frustum instead of leaving black bars on the
// sides. Below this ratio (portrait/narrow) the grid is left at its
// normal width rather than narrowed, since a sparser-than-designed field
// looks worse than the existing minor letterboxing.
#define MM_REFERENCE_ASPECT (16.0f / 9.0f)


/* ------------------------------------------------------------ World constants -- */

typedef struct {
    float halfWidth, topY, bottomY, spacing, depthNear;
    float colWidth;      /* lane-to-lane horizontal spacing */
    int   slotCount;
} MMWorld;

MMWorld mm_world(const MMSettings *s);

/* -------------------------------------------------------------- Glyph instance -- */

typedef struct {
    float px, py, pz;
    float cell;
    float flipX;
    float flipY;
    float bright;
    float cr, cg, cb;
} MMGlyphInstance;

/* ----------------------------------------------------------------- Simulation -- */

typedef struct MMSim MMSim;

typedef struct {
    float x, z, headY, speedVariation, baseSpeed, wavePhase, yOff, hueBias;
    int   length;
    int   lane;
    int   isGlitch;  // 1 = red glitch column, 0 = normal
} MMColumn;

// `aspect` is the renderer's current width/height (e.g. 16.0f/9.0f). Pass
// <= 0 to fall back to MM_REFERENCE_ASPECT (no widening) -- e.g. for
// callers like _linktest.cpp that have no real viewport.
MMSim *mm_sim_create(const MMSettings *s, int glyphCount, uint64_t seed, float aspect);
void   mm_sim_destroy(MMSim *sim);
void   mm_sim_update(MMSim *sim, const MMSettings *s, int glyphCount, float aspect);
void   mm_sim_advance(MMSim *sim, float dt);
int    mm_sim_max_instances(const MMSim *sim);

// Feed the current wall-clock time in (hour 0-23, minute 0-59, second 0-59).
// Harmless to call every frame, and harmless regardless of the easterEggs
// setting (the value is just cached). mmcore.c uses this only to detect
// 11:11 AM/PM for the "11:11" rain easter egg -- the host is responsible for
// sourcing real time so mmcore.c stays platform-agnostic and does not call
// any OS time API itself.
void   mm_sim_set_clock(MMSim *sim, int hour, int minute, int second);

/* Keep newly spawned depth tiers aligned with the renderer's moving camera. */
void   mm_sim_set_camera_travel(MMSim *sim, float travel);
/* Shift every active column after the renderer recenters its camera. */
void   mm_sim_rebase_depth(MMSim *sim, float zShift);

/* Pass `renderAlpha` to interpolate column positions between discrete physics ticks. */
int    mm_sim_write_instances(MMSim *sim, MMGlyphInstance *out, int cap, float renderAlpha);

#ifdef __cplusplus
}
#endif

#endif /* MMCORE_H */