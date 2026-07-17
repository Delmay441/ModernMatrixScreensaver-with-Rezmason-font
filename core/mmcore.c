#include "mmcore.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================ Settings ======= */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
#define LUT_SIZE 256

MMSettings mm_settings_default(void) {
    MMSettings s;
    s.density = 0.90;
    s.speed = 0.25;
    s.bloomIntensity = 0.90;
    s.glyphScale = 0.3f;
    s.lengthBias = 0.25;
    s.crtDistort = 0.0; 	
    s.depthAmount = 0.0;
    s.cameraSpeed = 1.0;     
    s.mutationRate = 0.10;     
    s.easterEggs = 1;        
    s.current_exponent = 1.0f;
    s.fog = 1; s.waves = 1; s.panning = 0; s.textured = 1;
    s.wireframe = 0; s.showFPS = 0; s.bloom = 1; s.hdr = 1;
	s.batterySaver = 0;
    s.mainColorR = 0.05f; s.mainColorG = 0.85f; s.mainColorB = 0.25f;   // Matrix Green default
    s.glitchColorR = 0.85f; s.glitchColorG = 0.05f; s.glitchColorB = 0.05f;
    s.flipXEnabled = 1;
    s.flipYEnabled = 1;
    s.binaryMode = 0;
    s.columnGaps = 1;
    s.extraContrastHeads = 0;
    s.crtEmulation = 0;
    return s;
}

int mm_strip_count(const MMSettings *s) {
    float scale = (s && s->glyphScale > 0.01f) ? s->glyphScale : 0.5f;
    float dens = (s && s->density > 0.05f) ? (float)s->density : 0.05f;
    float col_width = (scale * 1.4f) / dens;

    const float kNarrowWidth = 64.0f;
    const float kWideWidth   = 76.0f;
    const float kBlendRange  = 0.05f;

    float grid_width = kWideWidth;
    if (s) {
        float d = (float)s->depthAmount;
        if (d < 0.0f) d = 0.0f;
        if (d < kBlendRange) {
            float t = d / kBlendRange;
            t = t * t * (3.0f - 2.0f * t); /* smoothstep */
            grid_width = kNarrowWidth + (kWideWidth - kNarrowWidth) * t;
        }
    }

    return (int)(grid_width / col_width);
}

// Aspect-ratio-aware lane count: mm_strip_count()'s kNarrowWidth/kWideWidth
// constants were tuned to fill a MM_REFERENCE_ASPECT (16:9) frustum. On
// wider viewports (ultra-widescreen, triple-monitor spans, etc.) the
// perspective frustum is wider than that at the rain's depth plane, so the
// fixed-width lane grid no longer reaches the screen edges. Scaling the
// lane count by aspect / reference (never shrinking below the base count)
// keeps the field filling the frustum edge-to-edge.
static int mm_strip_count_for_aspect(const MMSettings *s, float aspect) {
    int base = mm_strip_count(s);
    if (aspect <= 0.0f || aspect <= MM_REFERENCE_ASPECT) return base;
    float widen = aspect / MM_REFERENCE_ASPECT;
    int widened = (int)ceilf((float)base * widen);
    return widened > base ? widened : base;
}

float mm_fall_speed(const MMSettings *s)    { return lerpf(1.5f, 34.0f, (float)s->speed); }
float mm_mutation_rate(const MMSettings *s) {
    float base = lerpf(1.5f, 7.0f, (float)s->speed);
    float mult = lerpf(0.0f, 2.0f, (float)(s ? s->mutationRate : 0.5));
    return base * mult;
}

/* ============================================================ World =========== */

MMWorld mm_world(const MMSettings *s) {
    MMWorld w;
    float scale;
    float dens;
    float depthAmt;
    float farthestZ;
    float perspectiveBottom;
    float perspectiveTop;

    w.halfWidth = 38.0f;
    scale       = (s && s->glyphScale > 0.01f) ? s->glyphScale : 0.5f;
    dens        = (s && s->density > 0.05f) ? (float)s->density : 0.05f;
    w.spacing   = scale * 1.6f; 
    w.colWidth  = (scale * 1.4f) / dens;
    w.depthNear = 10.0f;

    depthAmt = s ? (float)s->depthAmount : 0.0f;
    if (depthAmt < 0.0f) depthAmt = 0.0f;
    if (depthAmt > 1.5f) depthAmt = 1.5f;
    farthestZ = w.depthNear - 80.0f * depthAmt - 0.5f;
    perspectiveBottom = -(MM_CAMERA_EYE_Z - farthestZ) * 0.424475f - 2.0f * w.spacing;
    w.bottomY = (perspectiveBottom < -28.0f) ? perspectiveBottom : -28.0f;

    perspectiveTop = (MM_CAMERA_EYE_Z - farthestZ) * 0.424475f + 2.0f * w.spacing;
    w.topY = (perspectiveTop > 28.0f) ? perspectiveTop : 28.0f;

    w.slotCount = (int)((w.topY - w.bottomY) / w.spacing) + 2; 
    return w;
}

/* ============================================================ PRNG ============ */

static uint64_t xs_next(uint64_t *st) {
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *st = x;
    return x;
}
static float frand01(uint64_t *st) {
    return (float)((xs_next(st) >> 40) * (1.0 / 16777216.0));
}
static float frange(uint64_t *st, float a, float b) { return a + (b - a) * frand01(st); }
static int irange(uint64_t *st, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(xs_next(st) % (uint64_t)(hi - lo + 1));
}

// How long between glitch-column spawns (see spawn_column()'s isGlitch roll
// and mm_sim_create()'s initial seed below). Both use this same range --
// there is deliberately no separate short "startup" timer.
#define MM_GLITCH_TIMER_MIN_SEC (20.0f * 60.0f)
#define MM_GLITCH_TIMER_MAX_SEC (40.0f * 60.0f)

#define MM_BLANK_GLYPH_PROB 0.05f
// When gaps are disabled we return immediately without touching the RNG --
// this keeps the RNG stream (and thus flip/mutation rolls elsewhere) from
// shifting just because gaps are toggled off, and avoids pointless work.
static int roll_blank_glyph(uint64_t *st, int gapsEnabled) {
    if (!gapsEnabled) return 0;
    return frand01(st) < MM_BLANK_GLYPH_PROB;
}

/* ============================================================ Simulation ===== */

typedef struct {
    int index;
    float z;
} MMColSortItem;

struct MMSim {
    int        glyphCount;
    int        slotCount;
    int        num_lanes;
    float      time;
    uint64_t   rng;
    int        count;     
    int        cap;       
    int        cellCap;
    MMColumn  *cols;
    int       *cells;     
    int       *cellBlank; 
    float     *cellFlipsX; 
    float     *cellFlipsY; 
    float     *lane_tails;
    MMColSortItem *colSortBuf;
    MMSettings settings;
    float      lengthLUT[LUT_SIZE]; 
    MMWorld    world;
    float      cameraTravel;
    float      glitchTimer; 
    int        wallHour, wallMinute, wallSecond; // wall-clock time fed in by the host; drives the 11:11 easter egg
    float      aspect; // last aspect ratio (width/height) passed in; drives ultra-widescreen lane widening
};

void refresh_sim_settings(MMSim *sim) {
    sim->settings.current_exponent = powf(10.0f, (0.5f - sim->settings.lengthBias) * 2.0f);
    for (int i = 0; i < LUT_SIZE; ++i) {
        float r = (float)i / (float)(LUT_SIZE - 1);
        sim->lengthLUT[i] = powf(r, sim->settings.current_exponent);
    }
}

static float get_lane_tail_y(MMSim *sim, int lane) {
    return sim->lane_tails[lane];
}

// Picks a glyph cell index for a rain character. In binary mode this
// restricts selection to just the '0'/'1' cells (a single coin-flip instead
// of a full irange over the symbol set); otherwise it's the normal uniform
// pick across every glyph in the atlas.
static int pick_glyph_cell(MMSim *sim) {
    if (sim->settings.binaryMode && sim->glyphCount > MM_BINARY_GLYPH_ONE_INDEX) {
        return (frand01(&sim->rng) < 0.5f) ? MM_BINARY_GLYPH_ZERO_INDEX : MM_BINARY_GLYPH_ONE_INDEX;
    }
    return irange(&sim->rng, 0, sim->glyphCount - 1);
}

static float pick_flip(MMSim *sim, int enabled) {
    if (!enabled) return 1.0f;
    return (frand01(&sim->rng) < 0.5f) ? -1.0f : 1.0f;
}

/* ------------------------------------------------------- "11:11" easter egg -- */
//
// At 11:11 AM or PM (gated behind the easterEggs toggle), the rain's normal
// glyph selection is replaced by a fixed vertical repeating pattern --
// "11:11 " (six cells: '1' '1' ':' '1' '1' <gap>) -- keyed purely off each
// cell's slot (row) index, so every lane lands on the same pattern in
// lockstep. Existing columns "fizzle into" the pattern through the ordinary
// mutation pass in mm_sim_advance (nothing special needed there beyond
// swapping what glyph gets picked), and newly spawned columns come in
// showing the pattern from the start. The moment the wall clock ticks over
// to 11:12, this simply stops being selected and the very same mutation /
// spawn logic washes the pattern back out into full randomization.
#define MM_ELEVEN_PATTERN_LEN 6
static const int kElevenPattern[MM_ELEVEN_PATTERN_LEN] = {
    MM_BINARY_GLYPH_ONE_INDEX, MM_BINARY_GLYPH_ONE_INDEX, MM_GLYPH_COLON_INDEX,
    MM_BINARY_GLYPH_ONE_INDEX, MM_BINARY_GLYPH_ONE_INDEX, -1 /* the gap */
};

static int mm_eleven_eleven_active(const MMSim *sim) {
    if (!sim->settings.easterEggs) return 0;
    if (sim->glyphCount <= MM_ELEVEN_MAX_GLYPH_INDEX) return 0; // atlas too small for ':'
    int h = sim->wallHour, m = sim->wallMinute;
    return (h == 11 || h == 23) && m == 11;
}

// Resolves one cell's glyph/blank state, keyed only off its row (slot)
// index, so the whole field agrees on where each part of "11:11 " lands.
static int eleven_pattern_glyph(int slot, int *outBlank) {
    int m = ((slot % MM_ELEVEN_PATTERN_LEN) + MM_ELEVEN_PATTERN_LEN) % MM_ELEVEN_PATTERN_LEN;
    int g = kElevenPattern[m];
    *outBlank = (g < 0);
    return (g < 0) ? 0 : g;
}

// Shared by spawn_column's initial fill, mm_sim_update's glyph-set refresh,
// and the per-tick mutation pass in mm_sim_advance: picks either the next
// "11:11" pattern cell (forced, no flip -- both glyphs it uses already read
// correctly unflipped) or falls back to the normal random glyph/blank/flip
// selection.
static void fill_glyph_cell(MMSim *sim, int idx, int slot) {
    if (mm_eleven_eleven_active(sim)) {
        int isBlank = 0;
        int glyph = eleven_pattern_glyph(slot, &isBlank);
        sim->cellBlank[idx]  = isBlank;
        sim->cells[idx]      = isBlank ? 0 : glyph;
        sim->cellFlipsX[idx] = 1.0f;
        sim->cellFlipsY[idx] = 1.0f;
        return;
    }
    if (roll_blank_glyph(&sim->rng, sim->settings.columnGaps)) {
        sim->cellBlank[idx] = 1;
    } else {
        sim->cellBlank[idx] = 0;
        sim->cells[idx] = pick_glyph_cell(sim);
    }
    sim->cellFlipsX[idx] = pick_flip(sim, sim->settings.flipXEnabled);
    sim->cellFlipsY[idx] = pick_flip(sim, sim->settings.flipYEnabled);
}

static void spawn_column(MMSim *sim, int i, int initial) {
    MMColumn *c = &sim->cols[i];
    const MMWorld *w = &sim->world;
    float cameraZ = MM_CAMERA_EYE_Z - sim->cameraTravel;
    float depthNear = w->depthNear - sim->cameraTravel;
    int lane = -1;
    int start_lane = irange(&sim->rng, 0, sim->num_lanes - 1);
    for (int l = 0; l < sim->num_lanes; ++l) {
        int check_lane = (start_lane + l) % sim->num_lanes;
        if (get_lane_tail_y(sim, check_lane) < (w->topY - 5.0f)) {
            lane = check_lane;
            break;
        }
    }
    if (lane == -1) {
        c->lane = -1;
        c->headY = w->topY + 1000.0f;
        return;
    }

    c->lane = lane;
    c->yOff = 0.0f;

    c->isGlitch = 0;
    if (sim->settings.easterEggs && !initial && sim->glitchTimer <= 0.0f) {
        c->isGlitch = 1;
        sim->glitchTimer = frange(&sim->rng, MM_GLITCH_TIMER_MIN_SEC, MM_GLITCH_TIMER_MAX_SEC);
    }

    float depthAmt = (float)sim->settings.depthAmount;
    if (depthAmt < 0.0f) depthAmt = 0.0f;
    if (depthAmt > 1.5f) depthAmt = 1.5f;

    float max_depth_range = 80.0f; 
    float z_variance = max_depth_range * depthAmt;
    float r = frand01(&sim->rng);
    float depth_curve;
    
    if (r < 0.15f) {
        depth_curve = 0.0f;
    } else {
        float remapped_r = (r - 0.15f) / 0.85f;
        depth_curve = powf(remapped_r, 0.5f); 
    }
    
    if (c->isGlitch) {
        c->z = depthNear + 1.0f * depthAmt;
    } else {
        c->z = depthNear - (depth_curve * z_variance) + frange(&sim->rng, -0.5f, 0.5f) * depthAmt;
    }
    
    int lutIndex = (int)(r * (float)(LUT_SIZE - 1));
    float curve = sim->lengthLUT[lutIndex];

    float totalGridWidth = (sim->num_lanes - 1) * w->colWidth;
    float startX = -(totalGridWidth * 0.5f);
    float xRef = startX + (lane * w->colWidth);
    float depthScale = (cameraZ - c->z) / (cameraZ - depthNear);
    c->x = xRef * depthScale;

    if (c->isGlitch) {
        int jitter = irange(&sim->rng, -3, 3);
        c->length = 10 + jitter; 
    } else {
        float glyphScale = (sim->settings.glyphScale > 0.01f) ? sim->settings.glyphScale : 0.5f;
        const int MIN_COL_LEN = 12;
        int baseLength = MIN_COL_LEN + (int)(curve * (50.0f - MIN_COL_LEN));
        c->length = (int)(baseLength / glyphScale);

        if (c->length > 75) c->length = 75;
        if (c->length < MIN_COL_LEN) c->length = MIN_COL_LEN;
    }

    c->baseSpeed = frange(&sim->rng, 0.90f, 1.10f);
    c->speedVariation = c->baseSpeed;

    int rowBase = i * sim->slotCount;
    for (int j = 0; j < sim->slotCount; ++j) {
        fill_glyph_cell(sim, rowBase + j, j);
    }

    c->wavePhase = (float)i * 0.35f + frange(&sim->rng, -0.12f, 0.12f);
    c->hueBias = frange(&sim->rng, -1.0f, 1.0f) * 0.15f;

    if (initial) {
        const float kBuildInSeconds = 2.5f;
        float startupR = frand01(&sim->rng);
        float biased = 1.0f - powf(1.0f - startupR, 3.0f);
        float enterTime = biased * kBuildInSeconds;
        float fallRate = mm_fall_speed(&sim->settings) * c->speedVariation;
        if (fallRate < 0.01f) fallRate = 0.01f;
        float delaySlots = (enterTime * fallRate) / w->spacing;
        c->headY = w->topY + delaySlots * w->spacing;
    } else {
        float randomDelay = frange(&sim->rng, 2.0f, 40.0f);
        c->headY = w->topY + (randomDelay * w->spacing);
    }
    
    float tailY = c->headY + (c->length * w->spacing);
    if (tailY > sim->lane_tails[c->lane]) {
        sim->lane_tails[c->lane] = tailY;
    }
}

static void rebuild_columns(MMSim *sim, int count) {
    if (count < 0) count = 0;
    int neededCells = count * sim->slotCount;
    
    if (count > sim->cap || neededCells > sim->cellCap) {
        if (sim->cols) free(sim->cols);
        if (sim->cells) free(sim->cells);
        if (sim->cellBlank) free(sim->cellBlank);
        if (sim->cellFlipsX) free(sim->cellFlipsX); 
        if (sim->cellFlipsY) free(sim->cellFlipsY); 
        if (sim->lane_tails) free(sim->lane_tails);
        if (sim->colSortBuf) free(sim->colSortBuf); 
        
        sim->cap = count;
        sim->cellCap = neededCells;
        
        sim->cols  = (MMColumn *)malloc(sizeof(MMColumn) * (size_t)sim->cap);
        sim->cells = (int *)malloc(sizeof(int) * (size_t)sim->cellCap);
        sim->cellBlank = (int *)malloc(sizeof(int) * (size_t)sim->cellCap);
        sim->cellFlipsX = (float *)malloc(sizeof(float) * (size_t)sim->cellCap); 
        sim->cellFlipsY = (float *)malloc(sizeof(float) * (size_t)sim->cellCap); 
        sim->lane_tails = (float *)malloc(sizeof(float) * sim->num_lanes);
        sim->colSortBuf = (MMColSortItem *)malloc(sizeof(MMColSortItem) * (size_t)sim->cap); 
    }
    
    for (int l = 0; l < sim->num_lanes; ++l) {
        sim->lane_tails[l] = -9999.0f;
    }
    
    sim->count = 0;
    for (int i = 0; i < count; ++i) {
        spawn_column(sim, i, 1);
        sim->count++;
    }
}

MMSim *mm_sim_create(const MMSettings *s, int glyphCount, uint64_t seed, float aspect) {
    MMSim *sim = (MMSim *)calloc(1, sizeof(MMSim));
    sim->glyphCount = glyphCount < 1 ? 1 : glyphCount;
    sim->world = mm_world(s);
    sim->slotCount = sim->world.slotCount;
    sim->time = 0.0f;
    sim->rng = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;

    sim->glitchTimer = frange(&sim->rng, MM_GLITCH_TIMER_MIN_SEC, MM_GLITCH_TIMER_MAX_SEC);

    sim->count = 0;
    sim->cap = 0;
    sim->cols = NULL;
    sim->cells = NULL;
    sim->cellBlank = NULL;
    sim->cellFlipsX = NULL; 
    sim->cellFlipsY = NULL; 
    sim->lane_tails = NULL;
    sim->colSortBuf = NULL;
    sim->settings = *s; 
    refresh_sim_settings(sim);
    sim->aspect = aspect;
    sim->num_lanes = mm_strip_count_for_aspect(s, aspect);
    int total_columns = sim->num_lanes * 2; 
    rebuild_columns(sim, total_columns);
    
    return sim;
}

void mm_sim_destroy(MMSim *sim) {
    if (!sim) return;
    free(sim->cols);
    free(sim->cells);
    free(sim->cellBlank);
    free(sim->cellFlipsX); 
    free(sim->cellFlipsY); 
    free(sim->lane_tails);
    free(sim->colSortBuf);
    free(sim);
}

void mm_sim_update(MMSim *sim, const MMSettings *s, int glyphCount, float aspect) {
    int aspectChanged = (aspect != sim->aspect);
    int new_lanes = mm_strip_count_for_aspect(s, aspect);
    int old_lanes = sim->num_lanes;
    
    int scaleChanged = (sim->settings.glyphScale != s->glyphScale);
    int densityChanged = (sim->settings.density != s->density);
    int biasChanged = (sim->settings.lengthBias != s->lengthBias);
    int depthChanged = (sim->settings.depthAmount != s->depthAmount);
    int binaryModeChanged = (sim->settings.binaryMode != s->binaryMode);
    int flipXChanged = (sim->settings.flipXEnabled != s->flipXEnabled);
    int flipYChanged = (sim->settings.flipYEnabled != s->flipYEnabled);
    
    int gc = glyphCount < 1 ? 1 : glyphCount;
    int glyphsChanged = (gc != sim->glyphCount);

    sim->settings = *s;
    sim->glyphCount = gc;
    sim->aspect = aspect;

    if (biasChanged) {
        refresh_sim_settings(sim);
    }
    
    if (scaleChanged || depthChanged) {
        sim->world = mm_world(s);
        sim->slotCount = sim->world.slotCount;
    }

    if (new_lanes != old_lanes || scaleChanged || densityChanged || depthChanged || aspectChanged) {
        sim->num_lanes = new_lanes;
        rebuild_columns(sim, new_lanes * 2);
    } else if (glyphsChanged || binaryModeChanged || flipXChanged || flipYChanged) {
        int eleven = mm_eleven_eleven_active(sim);
        for (int i = 0; i < sim->count; ++i) {
            for (int j = 0; j < sim->slotCount; ++j) {
                int idx = i * sim->slotCount + j;
                if (eleven) {
                    int isBlank = 0;
                    int glyph = eleven_pattern_glyph(j, &isBlank);
                    sim->cells[idx] = isBlank ? 0 : glyph;
                    sim->cellFlipsX[idx] = 1.0f;
                    sim->cellFlipsY[idx] = 1.0f;
                } else {
                    sim->cells[idx] = pick_glyph_cell(sim);
                    sim->cellFlipsX[idx] = pick_flip(sim, sim->settings.flipXEnabled);
                    sim->cellFlipsY[idx] = pick_flip(sim, sim->settings.flipYEnabled);
                }
            }
        }
    }
}

/* ============================================================ Enhancements === */

static float mm_column_brightness_curve(float t) {
    const float kBrightLifetime = 0.35f;  
    const float kTailDecayRate  = 3.0f;   
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    if (t <= kBrightLifetime) {
        return 1.0f - 0.10f * (t / kBrightLifetime);
    }
    float u = (t - kBrightLifetime) / (1.0f - kBrightLifetime);
    return 0.90f * expf(-kTailDecayRate * u);
}

static float mm_column_speed_noise(float time, int lane, float wavePhase) {
    float standingWave = sinf((float)lane * 0.35f) * cosf(time * 0.18f + wavePhase);
    float temporalBreathing = sinf(time * 0.15f + wavePhase);
    return 0.85f + 0.25f * temporalBreathing + 0.15f * standingWave;
}

void mm_sim_set_clock(MMSim *sim, int hour, int minute, int second) {
    if (!sim) return;
    if (hour   < 0) hour   = 0;
    if (hour   > 23) hour   = 23;
    if (minute < 0) minute = 0;
    if (minute > 59) minute = 59;
    if (second < 0) second = 0;
    if (second > 59) second = 59;
    sim->wallHour   = hour;
    sim->wallMinute = minute;
    sim->wallSecond = second;
}

void mm_sim_advance(MMSim *sim, float dt) {
    sim->time += dt;
    sim->glitchTimer -= dt; 

    const float fall = mm_fall_speed(&sim->settings);
    const MMWorld *w = &sim->world;

    for (int l = 0; l < sim->num_lanes; ++l) sim->lane_tails[l] = -9999.0f;
    for (int i = 0; i < sim->count; ++i) {
        MMColumn *c = &sim->cols[i];
        if (c->lane != -1) {
            float tailY = c->headY + (c->length * w->spacing);
            if (tailY > sim->lane_tails[c->lane]) {
                sim->lane_tails[c->lane] = tailY;
            }
        }
    }

    for (int i = 0; i < sim->count; ++i) {
        MMColumn *c = &sim->cols[i];
        
        float speedNoise = mm_column_speed_noise(sim->time, c->lane, c->wavePhase);
        c->headY -= fall * c->speedVariation * speedNoise * dt;

        int headSlot = (int)((w->topY - c->yOff - c->headY) / w->spacing);
        if (headSlot - c->length > sim->slotCount) {
            spawn_column(sim, i, 0);
            continue;
        }

        if (c->lane == -1) {
            spawn_column(sim, i, 0);
            continue;
        }

        int lo = headSlot - c->length + 1; if (lo < 0) lo = 0;
        int hi = headSlot;                 if (hi > sim->slotCount - 1) hi = sim->slotCount - 1;
        
        if (hi >= lo) {
            float flicker_multiplier = 1.0f;

            const float mutFloor = 0.9f;
            const float mutSwing = 0.6f;
            float wave01 = 0.5f * (sinf(sim->time * 0.05f + c->wavePhase * 0.9f + 1.0f) + 1.0f);
            float mutNoise = mutFloor + mutSwing * wave01;

            float baseProb = mm_mutation_rate(&sim->settings) * flicker_multiplier * mutNoise * dt;
            
            float exactHeadSlot = (w->topY - c->yOff - c->headY) / w->spacing;
            int currentHeadSlot = (int)exactHeadSlot;

            for (int slot = lo; slot <= hi; ++slot) {
                float perCharProb = baseProb;
                
                float distToHead = (float)currentHeadSlot - (float)slot;
                float t = distToHead / (float)c->length;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                
                float visualEnergy = mm_column_brightness_curve(t);

                if (visualEnergy < 0.08f) {
                    perCharProb = 0.0f;
                } else {
                    if (c->isGlitch) {
                        perCharProb *= 20.0f;
                    } else {
                        float normalizedEnergy = (visualEnergy - 0.08f) / 0.92f;
                        perCharProb *= powf(normalizedEnergy, 3.0f) * 4.0f; 
                    }
                }

                // perCharProb is 0 for most slots on any given tick (anything
                // below the visualEnergy floor above), so skip the RNG draw
                // entirely rather than spending it on a guaranteed-false roll.
                if (perCharProb > 0.0f && frand01(&sim->rng) < perCharProb) {
                    int idx = i * sim->slotCount + slot;
                    fill_glyph_cell(sim, idx, slot);
                }
            }
        } 
    } 
}

void mm_sim_set_camera_travel(MMSim *sim, float travel) {
    if (!sim) return;
    sim->cameraTravel = travel;
}

void mm_sim_rebase_depth(MMSim *sim, float zShift) {
    if (!sim || zShift == 0.0f) return;
    for (int i = 0; i < sim->count; ++i)
        sim->cols[i].z += zShift;
}

int mm_sim_max_instances(const MMSim *sim) {
    return sim->count * sim->slotCount;
}

static void phosphor_color(float br, float hue, int isGlitch,
                            float mR, float mG, float mB,
                            float gR, float gG, float gB,
                            float *r, float *g, float *b) {
    const float midPoint = 0.45f;
    const float kPaleBlend = 0.6f; // how far the head-flash color drifts toward white
    float trueC[3], paleC[3];

    if (isGlitch) { trueC[0] = gR; trueC[1] = gG; trueC[2] = gB; }
    else          { trueC[0] = mR; trueC[1] = mG; trueC[2] = mB; }

    paleC[0] = trueC[0] + (1.0f - trueC[0]) * kPaleBlend;
    paleC[1] = trueC[1] + (1.0f - trueC[1]) * kPaleBlend;
    paleC[2] = trueC[2] + (1.0f - trueC[2]) * kPaleBlend;

    float cr, cg, cb;
    if (br <= midPoint) {
        float t = br / midPoint;
        float e = t * t * (3.0f - 2.0f * t);
        cr = trueC[0] * e;
        cg = trueC[1] * e;
        cb = trueC[2] * e;
    } else {
        float t = (br - midPoint) / (1.0f - midPoint);
        if (t > 1.0f) t = 1.0f;
        float e = t * t * (3.0f - 2.0f * t);
        cr = trueC[0] + (paleC[0] - trueC[0]) * e;
        cg = trueC[1] + (paleC[1] - trueC[1]) * e;
        cb = trueC[2] + (paleC[2] - trueC[2]) * e;
    }

    if (!isGlitch) {
        if (hue > 0.0f) cr += hue * 0.5f * br;
        else            cb += (-hue) * 0.5f * br;
    }

    if (cr > 2.0f) cr = 2.0f;
    if (cg > 2.0f) cg = 2.0f;
    if (cb > 2.0f) cb = 2.0f;

    *r = cr; *g = cg; *b = cb;
}

static int compare_cols(const void *a, const void *b) {
    const MMColSortItem *ca = (const MMColSortItem*)a;
    const MMColSortItem *cb = (const MMColSortItem*)b;
    if (ca->z < cb->z) return -1;
    if (ca->z > cb->z) return 1;
    return (ca->index < cb->index) ? -1 : ((ca->index > cb->index) ? 1 : 0);
}

int mm_sim_write_instances(MMSim *sim, MMGlyphInstance *out, int max_instances, float renderAlpha) {
    int count = 0;
    const MMWorld *w = &sim->world;
    const float fall = mm_fall_speed(&sim->settings);

    int col_count = 0;
    for (int i = 0; i < sim->count; ++i) {
        if (sim->cols[i].lane != -1) {
            sim->colSortBuf[col_count].index = i;
            sim->colSortBuf[col_count].z = sim->cols[i].z;
            col_count++;
        }
    }
    
    qsort(sim->colSortBuf, col_count, sizeof(MMColSortItem), compare_cols);

    for (int sc = 0; sc < col_count && count < max_instances; ++sc) {
        int i = sim->colSortBuf[sc].index;
        MMColumn *c = &sim->cols[i];
        
        float exactTime = sim->time + renderAlpha;
        float speedNoise = mm_column_speed_noise(exactTime, c->lane, c->wavePhase);
        float interpolatedHeadY = c->headY - (fall * c->speedVariation * speedNoise * renderAlpha);
        
        float depthAmt = (float)sim->settings.depthAmount;
        if (depthAmt < 0.0f) depthAmt = 0.0f;
        if (depthAmt > 1.5f) depthAmt = 1.5f;

        float jitterX = 0.0f;
        float jitterY = 0.0f;
        
        int pseudo_rand = (i * 73856 + (int)(exactTime * 15.0f)) % 30;
        if (pseudo_rand == 0) {
            float intensity = 0.004f;
            jitterX = sinf(exactTime * 50.0f + i) * intensity;
            jitterY = cosf(exactTime * 43.0f + i) * intensity;
        }
        
        float maxSway = (w->colWidth * 0.18f) * depthAmt;
        float sway = sinf(exactTime * 0.07f + c->wavePhase * 1.7f) * maxSway;

        float finalPx = c->x + jitterX + sway;                       
        
        float exactHeadSlot = (w->topY - c->yOff - interpolatedHeadY) / w->spacing;
        int headSlot = (int)exactHeadSlot;
        float fraction = exactHeadSlot - (float)headSlot; 

        float hueDrift = sinf(exactTime * 0.04f + c->wavePhase * 1.3f) * 0.05f;
        float hue = c->hueBias + hueDrift;
        if (hue >  0.2f) hue =  0.2f;
        if (hue < -0.2f) hue = -0.2f;

        float glowNoise = 0.9f + 0.1f * sinf(exactTime * 0.09f + c->wavePhase * 2.3f);

        int lo = headSlot - c->length + 1; 
        if (lo < 0) lo = 0;
        
        int hi = headSlot;                 
        if (hi > sim->slotCount - 1) hi = sim->slotCount - 1;

        if (hi >= lo) {
            for (int slot = lo; slot <= hi && count < max_instances; ++slot) {
                int idx = i * sim->slotCount + slot;
                if (sim->cellBlank[idx]) continue; 

                MMGlyphInstance *inst = &out[count++];

                inst->px = finalPx;
                inst->py = (w->topY - slot * w->spacing - c->yOff) + jitterY;
                inst->pz = c->z;
                inst->cell = (float)sim->cells[idx];
                inst->flipX = sim->cellFlipsX[idx]; 
                inst->flipY = sim->cellFlipsY[idx]; 

                float distToHead = (float)headSlot - (float)slot;

                float t = (exactHeadSlot - (float)slot) / (float)c->length;
                float falloff = mm_column_brightness_curve(t);

                float baseBright = falloff * glowNoise;

                float flareWidth = c->isGlitch ? 2.5f : 1.0f;
                float headGlow = 1.0f - (fabsf(distToHead - fraction) / flareWidth);
                if (headGlow < 0.0f) headGlow = 0.0f;

                float bright = baseBright + headGlow * (1.0f - baseBright);

                float scale = 0.80f;
                bright *= scale;               

                inst->bright = bright;
                phosphor_color(bright, hue, c->isGlitch,
                                sim->settings.mainColorR, sim->settings.mainColorG, sim->settings.mainColorB,
                                sim->settings.glitchColorR, sim->settings.glitchColorG, sim->settings.glitchColorB,
                                &inst->cr, &inst->cg, &inst->cb);
            }
        }
    }
    return count;
}