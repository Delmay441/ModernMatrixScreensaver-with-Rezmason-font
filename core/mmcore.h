#ifndef MMCORE_H
#define MMCORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------- Encodings -- */

typedef enum {
    MM_ENCODING_MATRIX = 0,
    MM_ENCODING_BINARY,
    MM_ENCODING_HEXADECIMAL,
    MM_ENCODING_DECIMAL,
    MM_ENCODING_DNA,
    MM_ENCODING_UNICODE,
    MM_ENCODING_COUNT
} MMEncoding;

#define MM_MAX_CODEPOINTS 160
int mm_encoding_codepoints(int enc, uint32_t *out, int cap);

/* ------------------------------------------------------------------ Settings -- */

typedef struct {
    double density;
    double speed;
    double bloomIntensity;
    int    encoding;
    int    fog, waves, panning, textured, wireframe, showFPS, bloom, hdr;
    float  glyphScale;
    double lengthBias;
    float  current_exponent;
    double crtDistort;   // 0..1 — screen-curvature / chromatic-aberration strength
	double depthAmount;
    double cameraSpeed;   // 0..1 — dolly speed through the depth field (0.5 == old fixed default)
    double mutationRate;  // 0..1 — independent multiplier on base glyph mutation rate (0.5 == old fixed default)
    int    easterEggs;    // 1 = allow rare red glitch columns, 0 = disable them entirely
} MMSettings;

#define MM_CAMERA_EYE_Z 48.0f

MMSettings mm_settings_default(void);

int   mm_strip_count(const MMSettings *s);
float mm_fall_speed(const MMSettings *s);
float mm_mutation_rate(const MMSettings *s);

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
    float bright;
    float cr, cg, cb;
} MMGlyphInstance;

/* ----------------------------------------------------------------- Simulation -- */

typedef struct MMSim MMSim;

typedef struct {
    float x, z, headY, speedVariation, baseSpeed, wavePhase, yOff, hueBias;
    int   length;
	int	  lane;
    int   isGlitch;  // 1 = red glitch column, 0 = normal
    int   hitBottom; // internal: 1 once this column's head has crossed bottomY this life
} MMColumn;

MMSim *mm_sim_create(const MMSettings *s, int glyphCount, uint64_t seed);
void   mm_sim_destroy(MMSim *sim);
void   mm_sim_update(MMSim *sim, const MMSettings *s, int glyphCount);
void   mm_sim_advance(MMSim *sim, float dt);
int    mm_sim_max_instances(const MMSim *sim);

/* Keep newly spawned depth tiers aligned with the renderer's moving camera. */
void   mm_sim_set_camera_travel(MMSim *sim, float travel);
/* Shift every active column after the renderer recenters its camera. */
void   mm_sim_rebase_depth(MMSim *sim, float zShift);

/* Pass `renderAlpha` to interpolate column positions between discrete physics ticks. */
int    mm_sim_write_instances(MMSim *sim, MMGlyphInstance *out, int cap, float renderAlpha);

/* --------------------------------------------------------- Bottom-hit events -- */
/* Fired once per column, the instant its head crosses the physical bottom of
   the viewport (world.bottomY) -- the visual moment a drop "lands". Consumers
   (the renderer) poll these once per fixed sim tick and turn them into sound. */
typedef struct {
    float pan;       /* -1 (left) .. +1 (right), from the column's screen-space x */
    float atten;     /* 0..1 loudness falloff for background/deep-field columns */
    int   isGlitch;  /* 1 if this was a red glitch column */
} MMBottomEvent;

#define MM_MAX_BOTTOM_EVENTS 16

/* Copies up to `cap` pending events into `out` and clears the internal queue.
   Safe to call every tick even if nothing happened (returns 0). */
int mm_sim_pop_bottom_events(MMSim *sim, MMBottomEvent *out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* MMCORE_H */