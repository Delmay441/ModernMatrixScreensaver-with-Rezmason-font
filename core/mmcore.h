#ifndef MMCORE_H
#define MMCORE_H

#include <stdint.h>

/*
 * mmcore — the portable, platform-agnostic engine shared by the macOS (.saver/.app)
 * and Windows (.scr) builds of Modern Matrix. Pure C99, no OS or GPU dependencies:
 * the digital-rain simulation, the settings model + derived values, the glyph
 * encodings, and the world constants all live here so a tweak to the *behaviour* of
 * the rain is made ONCE and both platforms pick it up.
 *
 * Each platform keeps its own renderer (Metal vs Direct3D), glyph-atlas rasteriser
 * (Core Text vs DirectWrite), settings UI, and host shell — only this engine is shared.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- Encodings -- */

typedef enum {
    MM_ENCODING_MATRIX = 0,   /* half-width katakana + digits (the film look) */
    MM_ENCODING_BINARY,       /* 0 1                                          */
    MM_ENCODING_HEXADECIMAL,  /* 0-9 A-F                                      */
    MM_ENCODING_DECIMAL,      /* 0-9                                          */
    MM_ENCODING_DNA,          /* A C G T                                      */
    MM_ENCODING_UNICODE,      /* full katakana block + half-width + digits    */
    MM_ENCODING_COUNT
} MMEncoding;

/* Largest glyph set any encoding produces (UNICODE) — handy for fixed buffers. */
#define MM_MAX_CODEPOINTS 160

/* Fill `out` (capacity `cap`) with the Unicode code points for `enc`, in glyph-cell
 * order, and return the count. Pass out=NULL (cap ignored) to just query the count.
 * The atlas rasteriser draws these in order, so cell index == index here. */
int mm_encoding_codepoints(int enc, uint32_t *out, int cap);

/* ------------------------------------------------------------------ Settings -- */

typedef struct {
    double density;        /* 0..1 -> strip count               */
    double speed;          /* 0..1 -> fall speed + mutation rate */
    double bloomIntensity; /* 0..1 (renderer-only; carried here for one shared model) */
    int    encoding;       /* MMEncoding                         */
    int    fog, waves, panning, textured, wireframe, showFPS, bloom, hdr; /* bools 0/1 */
} MMSettings;

MMSettings mm_settings_default(void);

/* Derived values — the shared slider->world mappings (lerp(a,b,t)=a+(b-a)*t). */
int   mm_strip_count(const MMSettings *s);    /* round(lerp(60, 900, density)) */
float mm_fall_speed(const MMSettings *s);     /* lerp(1.5, 34, speed)          */
float mm_mutation_rate(const MMSettings *s);  /* lerp(1.5, 7, speed)           */

/* ------------------------------------------------------------ World constants -- */

/* The 3D volume the camera frames (see Renderer / PORTING.md §4). */
typedef struct {
    float halfWidth, topY, bottomY, spacing, depthNear, depthFar;
    int   slotCount;       /* vertical glyph slots = floor((topY-bottomY)/spacing)+2 */
} MMWorld;

MMWorld mm_world(void);

/* -------------------------------------------------------------- Glyph instance -- */

/* One textured billboard quad. Field-for-field identical to the Swift/Metal
 * GlyphInstance, so the renderer can write these straight into its vertex buffer. */
typedef struct {
    float px, py, pz;   /* world position        */
    float cell;         /* glyph atlas cell index */
    float bright;       /* brightness multiplier (leaders may exceed 1) */
    float cr, cg, cb;   /* base tint             */
} MMGlyphInstance;

/* ----------------------------------------------------------------- Simulation -- */

typedef struct MMSim MMSim;

/* Create a rain simulation. `glyphCount` is the number of atlas cells for the current
 * encoding; `seed` seeds the PRNG (pass 0 for a fixed default seed). */
MMSim *mm_sim_create(const MMSettings *s, int glyphCount, uint64_t seed);
void   mm_sim_destroy(MMSim *sim);

/* Re-apply settings: rebuilds columns if the strip count changed, otherwise re-rolls
 * the glyphs if the glyph count (encoding) changed. */
void   mm_sim_update(MMSim *sim, const MMSettings *s, int glyphCount);

/* Advance the model by `dt` seconds. */
void   mm_sim_advance(MMSim *sim, float dt);

/* Upper bound on instances the sim can currently emit (for vertex-buffer sizing). */
int    mm_sim_max_instances(const MMSim *sim);

/* Emit up to `cap` visible glyph instances into `out`; returns the number written. */
int    mm_sim_write_instances(MMSim *sim, MMGlyphInstance *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* MMCORE_H */
