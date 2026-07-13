#include "mmcore.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================ Encodings ====== */

int mm_encoding_codepoints(int enc, uint32_t *out, int cap) {
    int n = 0;
    #define PUT(cp)            do { if (out) { if (n < cap) out[n] = (uint32_t)(cp); } n++; } while (0)
    #define PUT_RANGE(a, b)    do { for (uint32_t _c = (a); _c <= (b); ++_c) PUT(_c); } while (0)
    #define PUT_DIGITS()       PUT_RANGE(0x30, 0x39)
    
    switch (enc) {
        case MM_ENCODING_BINARY:
            PUT('0'); PUT('1');
            break;
        case MM_ENCODING_DECIMAL:
            PUT_DIGITS();
            break;
        case MM_ENCODING_HEXADECIMAL:
            PUT_DIGITS();
            PUT_RANGE(0x41, 0x46);
            break;
        case MM_ENCODING_DNA:
            PUT('A'); PUT('C'); PUT('G'); PUT('T');
            break;
        case MM_ENCODING_UNICODE:
            PUT_RANGE(0x30A1, 0x30F6);
            PUT_RANGE(0xFF66, 0xFF9D);
            PUT_DIGITS();
            break;
        case MM_ENCODING_MATRIX:
        default:
            PUT_RANGE(0xFF66, 0xFF9D);
            PUT_DIGITS();
            break;
    }

    #undef PUT
    #undef PUT_RANGE
    #undef PUT_DIGITS
    return n;
}

/* ============================================================ Settings ======= */

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
#define LUT_SIZE 256
static float lengthLUT[LUT_SIZE];

MMSettings mm_settings_default(void) {
    MMSettings s;
    s.density = 0.90;
    s.speed = 0.10;
    s.bloomIntensity = 0.90;
    s.encoding = MM_ENCODING_MATRIX;
    s.glyphScale = 0.3f;
    s.lengthBias = 0.5;
	s.crtDistort = 0.0; 	
    s.depthAmount = 0.0;
    s.cameraSpeed = 1.0;     
    s.mutationRate = 0.3;     
    s.easterEggs = 1;        
	s.current_exponent = 1.0f;
    s.fog = 1; s.waves = 1; s.panning = 0; s.textured = 1;
    s.wireframe = 0; s.showFPS = 0; s.bloom = 1; s.hdr = 1;
    return s;
}

int mm_strip_count(const MMSettings *s) {
    float scale = (s && s->glyphScale > 0.01f) ? s->glyphScale : 0.5f;
    float dens = (s && s->density > 0.05f) ? (float)s->density : 0.05f;
    float col_width = (scale * 1.4f) / dens;
    
    // Base simulation width is 76.0f.
    float grid_width = 76.0f;
    
    // --- TERMINAL BEZEL DIAL ---
    if (s && s->depthAmount < 0.01f) {
        grid_width = 64.0f; 
    }
    
    return (int)(grid_width / col_width);
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

/* ============================================================ Simulation ===== */

struct MMSim {
    MMSettings s;
    int        glyphCount;
    int        slotCount;
	int		   num_lanes;
    float      time;
    uint64_t   rng;
    int        count;     
    int        cap;       
    int        cellCap;
    MMColumn  *cols;
    int       *cells;     
    float     *lane_tails;
    MMSettings settings; 
    MMWorld    world;
    float      cameraTravel;
    float      glitchTimer; 
    MMBottomEvent bottomEvents[MM_MAX_BOTTOM_EVENTS];
    int        bottomEventCount;
};

void refresh_sim_settings(MMSim *sim) {
    sim->settings.current_exponent = powf(10.0f, (0.5f - sim->settings.lengthBias) * 2.0f);
    for (int i = 0; i < LUT_SIZE; ++i) {
        float r = (float)i / (float)(LUT_SIZE - 1);
        lengthLUT[i] = powf(r, sim->settings.current_exponent);
    }
}

static float get_lane_tail_y(MMSim *sim, int lane) {
    return sim->lane_tails[lane];
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
    c->hitBottom = 0;

    c->isGlitch = 0;
    if (sim->settings.easterEggs && !initial && sim->glitchTimer <= 0.0f) {
        c->isGlitch = 1;
        const float kGlitchMin = 20.0f * 60.0f;
        const float kGlitchMax = 40.0f * 60.0f;
        sim->glitchTimer = frange(&sim->rng, kGlitchMin, kGlitchMax);
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
    float curve = lengthLUT[lutIndex];

    float totalGridWidth = (sim->num_lanes - 1) * w->colWidth;
    float startX = -(totalGridWidth * 0.5f);
    float xRef = startX + (lane * w->colWidth);
    float depthScale = (cameraZ - c->z) / (cameraZ - depthNear);
    c->x = xRef * depthScale;

    if (c->isGlitch) {
        c->baseSpeed = frange(&sim->rng, 2.5f, 3.5f);
        c->speedVariation = c->baseSpeed;
        int jitter = irange(&sim->rng, -6, 6);
        c->length = 20 + jitter; 
    } else {
        float glyphScale = (sim->settings.glyphScale > 0.01f) ? sim->settings.glyphScale : 0.5f;
        const int MIN_COL_LEN = 15;
        int baseLength = MIN_COL_LEN + (int)(curve * (100.0f - MIN_COL_LEN));
        c->length = (int)(baseLength / glyphScale);

        if (c->length > 150) c->length = 150;
        if (c->length < MIN_COL_LEN) c->length = MIN_COL_LEN;

        float depthRatio = (float)baseLength / 100.0f;
        c->baseSpeed = 0.5f + (depthRatio * 1.1f) + frange(&sim->rng, -0.15f, 0.15f);
        c->speedVariation = c->baseSpeed;
    }

    int *cells = &sim->cells[i * sim->slotCount];
    for (int j = 0; j < sim->slotCount; ++j)
        cells[j] = irange(&sim->rng, 0, sim->glyphCount - 1);

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
        if (sim->lane_tails) free(sim->lane_tails);
        
        sim->cap = count;
        sim->cellCap = neededCells;
        
        sim->cols  = (MMColumn *)malloc(sizeof(MMColumn) * (size_t)sim->cap);
        sim->cells = (int *)malloc(sizeof(int) * (size_t)sim->cellCap);
        sim->lane_tails = (float *)malloc(sizeof(float) * sim->num_lanes);
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

MMSim *mm_sim_create(const MMSettings *s, int glyphCount, uint64_t seed) {
    MMSim *sim = (MMSim *)calloc(1, sizeof(MMSim));
    sim->s = *s;
    sim->glyphCount = glyphCount < 1 ? 1 : glyphCount;
    sim->world = mm_world(s);
    sim->slotCount = sim->world.slotCount;
    sim->time = 0.0f;
    sim->rng = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;
    
    sim->glitchTimer = frange(&sim->rng, 30.0f, 120.0f);

    sim->count = 0;
    sim->cap = 0;
    sim->cols = NULL;
    sim->cells = NULL;
    sim->lane_tails = NULL;
    sim->settings = *s; 
    refresh_sim_settings(sim);
    sim->num_lanes = mm_strip_count(s);
    int total_columns = sim->num_lanes * 2; 
    rebuild_columns(sim, total_columns);
    
    return sim;
}

void mm_sim_destroy(MMSim *sim) {
    if (!sim) return;
    free(sim->cols);
    free(sim->cells);
    free(sim->lane_tails);
    free(sim);
}

void mm_sim_update(MMSim *sim, const MMSettings *s, int glyphCount) {
    int new_lanes = mm_strip_count(s);
    int old_lanes = sim->num_lanes;
    
    int scaleChanged = (sim->s.glyphScale != s->glyphScale);
    int densityChanged = (sim->s.density != s->density);
    int biasChanged = (sim->s.lengthBias != s->lengthBias);
    int depthChanged = (sim->s.depthAmount != s->depthAmount);
    
    int gc = glyphCount < 1 ? 1 : glyphCount;
    int glyphsChanged = (gc != sim->glyphCount);

    sim->s = *s;
    sim->settings = *s; 
    sim->glyphCount = gc;

    if (biasChanged) {
        refresh_sim_settings(sim);
    }
    
    if (scaleChanged || depthChanged) {
        sim->world = mm_world(s);
        sim->slotCount = sim->world.slotCount;
    }

    if (new_lanes != old_lanes || scaleChanged || densityChanged || depthChanged) {
        sim->num_lanes = new_lanes;
        rebuild_columns(sim, new_lanes * 2);
    } else if (glyphsChanged) {
        for (int i = 0; i < sim->count; ++i)
            for (int j = 0; j < sim->slotCount; ++j)
                sim->cells[i * sim->slotCount + j] = irange(&sim->rng, 0, sim->glyphCount - 1);
    }
}

/* ============================================================ Enhancements === */

static float mm_column_speed_noise(float time, int lane, float wavePhase) {
    float standingWave = sinf((float)lane * 0.35f) * cosf(time * 0.18f + wavePhase);
    float temporalBreathing = sinf(time * 0.15f + wavePhase);
    return 0.85f + 0.25f * temporalBreathing + 0.15f * standingWave;
}

void mm_sim_advance(MMSim *sim, float dt) {
    sim->time += dt;
    sim->glitchTimer -= dt; 

    const float fall = mm_fall_speed(&sim->s);
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
        int wasAboveBottom = c->headY > w->bottomY;
        c->headY -= fall * c->speedVariation * speedNoise * dt;

        if (!c->hitBottom && wasAboveBottom && c->headY <= w->bottomY) {
            c->hitBottom = 1;
            if (sim->bottomEventCount < MM_MAX_BOTTOM_EVENTS) {
                MMBottomEvent *ev = &sim->bottomEvents[sim->bottomEventCount++];
                float halfW = (w->halfWidth > 0.01f) ? w->halfWidth : 1.0f;
                ev->pan = c->x / halfW;
                if (ev->pan < -1.0f) ev->pan = -1.0f;
                if (ev->pan > 1.0f)  ev->pan = 1.0f;
                
                float depthFalloff = 1.0f - (w->depthNear - c->z) / 100.0f;
                if (depthFalloff < 0.15f) depthFalloff = 0.15f;
                if (depthFalloff > 1.0f)  depthFalloff = 1.0f;
                ev->atten = depthFalloff;
                ev->isGlitch = c->isGlitch;
            }
        }

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

            float baseProb = mm_mutation_rate(&sim->s) * flicker_multiplier * mutNoise * dt;
            
            float exactHeadSlot = (w->topY - c->yOff - c->headY) / w->spacing;
            int currentHeadSlot = (int)exactHeadSlot;

            // Evaluate individual cell energy metrics
            for (int slot = lo; slot <= hi; ++slot) {
                float perCharProb = baseProb;
                
                float distToHead = (float)currentHeadSlot - (float)slot;
                float t = distToHead / (float)c->length;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                
                // [FEATURE 3 UNIFIED: Visual-Matched Energy]
                // Match the renderer's exact brightness falloff curve (power of 2.8)
                // so mutation is driven by actual pixel brightness, not physical length.
                float visualEnergy = powf(1.0f - t, 2.8f);

                // Absolute freeze when visual brightness drops below 8%
                if (visualEnergy < 0.08f) {
                    perCharProb = 0.0f;
                } else {
                    if (c->isGlitch) {
                        // Glitch columns stay highly volatile, but we still clamp them 
                        // so they don't flutter invisibly in the dark.
                        perCharProb *= 20.0f;
                    } else {
                        // Rescale the visible window [0.08 -> 1.0] 
                        float normalizedEnergy = (visualEnergy - 0.08f) / 0.92f;
                        // Cubic punch right at the head
                        perCharProb *= powf(normalizedEnergy, 3.0f) * 4.0f; 
                    }
                }

                if (frand01(&sim->rng) < perCharProb) {
                    sim->cells[i * sim->slotCount + slot] = irange(&sim->rng, 0, sim->glyphCount - 1);
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

static void phosphor_color(float br, float hue, int isGlitch, float *r, float *g, float *b) {
    const float midPoint = 0.45f;
    float trueC[3], paleC[3];
    
    if (isGlitch) {
        trueC[0] = 0.85f; trueC[1] = 0.05f; trueC[2] = 0.05f;
        paleC[0] = 1.00f; paleC[1] = 0.65f; paleC[2] = 0.65f;
    } else {
        trueC[0] = 0.05f; trueC[1] = 0.85f; trueC[2] = 0.25f;
        paleC[0] = 0.55f; paleC[1] = 0.95f; paleC[2] = 0.65f;
    }

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

int mm_sim_write_instances(MMSim *sim, MMGlyphInstance *out, int max_instances, float renderAlpha) {
    int count = 0;
    const MMWorld *w = &sim->world;
    const float fall = mm_fall_speed(&sim->s);

    for (int i = 0; i < sim->count && count < max_instances; ++i) {
        MMColumn *c = &sim->cols[i];
        
        float speedNoise = mm_column_speed_noise(sim->time, c->lane, c->wavePhase);
        float interpolatedHeadY = c->headY - (fall * c->speedVariation * speedNoise * renderAlpha);
        
        float depthAmt = (float)sim->s.depthAmount;
        if (depthAmt < 0.0f) depthAmt = 0.0f;
        if (depthAmt > 1.5f) depthAmt = 1.5f;

        float jitterX = 0.0f;
        float jitterY = 0.0f;
        
        if (irange(&sim->rng, 0, 30) == 0) { 
            float intensity = 0.004f;
            jitterX = frange(&sim->rng, -0.5f, 0.5f) * intensity;
            jitterY = frange(&sim->rng, -0.5f, 0.5f) * intensity;
        }
        
        float maxSway = (w->colWidth * 0.18f) * depthAmt;
        float sway = sinf(sim->time * 0.07f + c->wavePhase * 1.7f) * maxSway;

        float finalPx = c->x + jitterX + sway;                       
		
		float exactHeadSlot = (w->topY - c->yOff - interpolatedHeadY) / w->spacing;
        int headSlot = (int)exactHeadSlot;
        float fraction = exactHeadSlot - (float)headSlot; 

        float hueDrift = sinf(sim->time * 0.04f + c->wavePhase * 1.3f) * 0.05f;
        float hue = c->hueBias + hueDrift;
        if (hue >  0.2f) hue =  0.2f;
        if (hue < -0.2f) hue = -0.2f;

        float glowNoise = 0.9f + 0.1f * sinf(sim->time * 0.09f + c->wavePhase * 2.3f);

        int lo = headSlot - c->length + 1; 
        if (lo < 0) lo = 0;
        
        int hi = headSlot;                 
        if (hi > sim->slotCount - 1) hi = sim->slotCount - 1;

        if (hi >= lo) {
            for (int slot = lo; slot <= hi && count < max_instances; ++slot) {
                MMGlyphInstance *inst = &out[count++];

                inst->px = finalPx;
                inst->py = (w->topY - slot * w->spacing - c->yOff) + jitterY;
                inst->pz = c->z;
                inst->cell = (float)sim->cells[i * sim->slotCount + slot];

                float distToHead = (float)headSlot - (float)slot;

                const float kFalloffPower = 2.8f;
                float t = (exactHeadSlot - (float)slot) / (float)c->length;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                float falloff = powf(1.0f - t, kFalloffPower);
                
                float baseBright = falloff * glowNoise;

                float flareWidth = c->isGlitch ? 2.5f : 1.0f;
                float headGlow = 1.0f - (fabsf(distToHead - fraction) / flareWidth);
                if (headGlow < 0.0f) headGlow = 0.0f;

                float bright = baseBright + headGlow * (1.0f - baseBright);

                float scale = 0.80f;
                bright *= scale;               

                inst->bright = bright;
                phosphor_color(bright, hue, c->isGlitch, &inst->cr, &inst->cg, &inst->cb);
            }
        }
    }
    return count;
}
int mm_sim_pop_bottom_events(MMSim *sim, MMBottomEvent *out, int cap) {
    if (!sim) return 0;
    int n = sim->bottomEventCount;
    if (n > cap) n = cap;
    for (int i = 0; i < n; ++i) out[i] = sim->bottomEvents[i];
    sim->bottomEventCount = 0; 
    return n;
}