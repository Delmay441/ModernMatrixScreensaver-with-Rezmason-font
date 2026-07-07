#include "mmcore.h"
#include <stdlib.h>
#include <math.h>

/* ============================================================ Encodings ====== */

int mm_encoding_codepoints(int enc, uint32_t *out, int cap) {
    int n = 0;
    /* Helper macros that respect the optional NULL/cap query mode. */
    #define PUT(cp)            do { if (out) { if (n < cap) out[n] = (uint32_t)(cp); } n++; } while (0)
    #define PUT_RANGE(a, b)    do { for (uint32_t _c = (a); _c <= (b); ++_c) PUT(_c); } while (0)
    #define PUT_DIGITS()       PUT_RANGE(0x30, 0x39)            /* '0'..'9' */

    switch (enc) {
        case MM_ENCODING_BINARY:
            PUT('0'); PUT('1');
            break;
        case MM_ENCODING_DECIMAL:
            PUT_DIGITS();
            break;
        case MM_ENCODING_HEXADECIMAL:
            PUT_DIGITS();
            PUT_RANGE(0x41, 0x46);                              /* 'A'..'F' */
            break;
        case MM_ENCODING_DNA:
            PUT('A'); PUT('C'); PUT('G'); PUT('T');
            break;
        case MM_ENCODING_UNICODE:
            PUT_RANGE(0x30A1, 0x30F6);                          /* katakana block */
            PUT_RANGE(0xFF66, 0xFF9D);                          /* half-width katakana */
            PUT_DIGITS();
            break;
        case MM_ENCODING_MATRIX:
        default:
            PUT_RANGE(0xFF66, 0xFF9D);                          /* half-width katakana */
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

MMSettings mm_settings_default(void) {
    MMSettings s;
    s.density = 0.42;
    s.speed = 0.08;          /* deliberately a slow drip */
    s.bloomIntensity = 0.53;
    s.encoding = MM_ENCODING_MATRIX;
    s.fog = 1; s.waves = 1; s.panning = 0; s.textured = 1;
    s.wireframe = 0; s.showFPS = 0; s.bloom = 1; s.hdr = 1;
    return s;
}

int mm_strip_count(const MMSettings *s) {
    /* round-half-away-from-zero, matching Swift's Float.rounded(). density >= 0. */
    return (int)(lerpf(60.0f, 900.0f, (float)s->density) + 0.5f);
}
float mm_fall_speed(const MMSettings *s)    { return lerpf(1.5f, 34.0f, (float)s->speed); }
float mm_mutation_rate(const MMSettings *s) { return lerpf(1.5f, 7.0f,  (float)s->speed); }

/* ============================================================ World =========== */

MMWorld mm_world(void) {
    MMWorld w;
    w.halfWidth = 38.0f;
    w.topY      = 28.0f;
    w.bottomY   = -28.0f;
    w.spacing   = 1.0f;
    w.depthNear = 10.0f;
    w.depthFar  = -42.0f;
    w.slotCount = (int)((w.topY - w.bottomY) / w.spacing) + 2;   /* == 38 */
    return w;
}

/* ============================================================ PRNG ============ */
/* Matches Sources/Core/Simulation.swift's XorShift so behaviour is identical. */

static uint64_t xs_next(uint64_t *st) {
    uint64_t x = *st;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *st = x;
    return x;
}
/* Uniform float in [0, 1). 24 random bits of mantissa. */
static float frand01(uint64_t *st) {
    return (float)((xs_next(st) >> 40) * (1.0 / 16777216.0));
}
static float frange(uint64_t *st, float a, float b) { return a + (b - a) * frand01(st); }
/* Inclusive integer range [lo, hi]. */
static int irange(uint64_t *st, int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int)(xs_next(st) % (uint64_t)(hi - lo + 1));
}

/* ============================================================ Simulation ===== */

typedef struct {
    float x, z, headY, speedVariation, wavePhase, yOff;
    int   length;
} MMColumn;

struct MMSim {
    MMSettings s;
    int        glyphCount;
    int        slotCount;
    float      time;
    uint64_t   rng;
    int        count;     /* active columns                 */
    int        cap;       /* allocated capacity (cols/cells) */
    MMColumn  *cols;
    int       *cells;     /* flat: count * slotCount         */
    MMWorld    world;
};

static void spawn_column(MMSim *sim, int i, int initial) {
    MMColumn *c = &sim->cols[i];
    const MMWorld *w = &sim->world;
    c->x = frange(&sim->rng, -w->halfWidth, w->halfWidth);
    c->z = frange(&sim->rng, w->depthFar, w->depthNear);
    c->speedVariation = frange(&sim->rng, 0.7f, 1.35f);
    int hi = sim->slotCount > 10 ? sim->slotCount : 10;
    c->length = irange(&sim->rng, 9, hi);

    int *cells = &sim->cells[i * sim->slotCount];
    for (int j = 0; j < sim->slotCount; ++j)
        cells[j] = irange(&sim->rng, 0, sim->glyphCount - 1);

    c->wavePhase = frange(&sim->rng, 0.0f, 6.2831853f);   /* 0..2π */
    c->yOff = frange(&sim->rng, 0.0f, w->spacing);        /* breaks inter-column alignment */

    if (initial) {
        /* Start ABOVE the top edge, widely staggered, so the screensaver opens on a
         * black screen and the rain cascades in — not already-fallen. */
        c->headY = w->topY + frand01(&sim->rng) * (float)sim->slotCount * w->spacing;
    } else {
        c->headY = w->topY + frand01(&sim->rng) * 8.0f * w->spacing;
    }
}

static void rebuild_columns(MMSim *sim, int count) {
    if (count < 0) count = 0;
    if (count > sim->cap) {
        free(sim->cols);
        free(sim->cells);
        sim->cap   = count;
        sim->cols  = (MMColumn *)malloc(sizeof(MMColumn) * (size_t)count);
        sim->cells = (int *)malloc(sizeof(int) * (size_t)count * (size_t)sim->slotCount);
    }
    sim->count = count;
    for (int i = 0; i < count; ++i) spawn_column(sim, i, 1);
}

MMSim *mm_sim_create(const MMSettings *s, int glyphCount, uint64_t seed) {
    MMSim *sim = (MMSim *)calloc(1, sizeof(MMSim));
    sim->s = *s;
    sim->glyphCount = glyphCount < 1 ? 1 : glyphCount;
    sim->world = mm_world();
    sim->slotCount = sim->world.slotCount;
    sim->time = 0.0f;
    sim->rng = seed == 0 ? 0x9E3779B97F4A7C15ULL : seed;
    sim->count = 0;
    sim->cap = 0;
    sim->cols = NULL;
    sim->cells = NULL;
    rebuild_columns(sim, mm_strip_count(s));
    return sim;
}

void mm_sim_destroy(MMSim *sim) {
    if (!sim) return;
    free(sim->cols);
    free(sim->cells);
    free(sim);
}

void mm_sim_update(MMSim *sim, const MMSettings *s, int glyphCount) {
    int newCount = mm_strip_count(s);
    int oldCount = mm_strip_count(&sim->s);
    int gc = glyphCount < 1 ? 1 : glyphCount;
    int glyphsChanged = (gc != sim->glyphCount);

    sim->s = *s;
    sim->glyphCount = gc;

    if (newCount != oldCount) {
        rebuild_columns(sim, newCount);
    } else if (glyphsChanged) {
        for (int i = 0; i < sim->count; ++i)
            for (int j = 0; j < sim->slotCount; ++j)
                sim->cells[i * sim->slotCount + j] = irange(&sim->rng, 0, sim->glyphCount - 1);
    }
}

void mm_sim_advance(MMSim *sim, float dt) {
    sim->time += dt;
    const float fall = mm_fall_speed(&sim->s);
    const float mutateProb = mm_mutation_rate(&sim->s) * dt;
    const MMWorld *w = &sim->world;

    for (int i = 0; i < sim->count; ++i) {
        MMColumn *c = &sim->cols[i];
        c->headY -= fall * c->speedVariation * dt;   /* read fallSpeed LIVE every frame */

        int headSlot = (int)((w->topY - c->yOff - c->headY) / w->spacing);
        if (headSlot - c->length > sim->slotCount) {
            spawn_column(sim, i, 0);                 /* respawn off the bottom */
            continue;
        }
        if (frand01(&sim->rng) < mutateProb) {
            int lo = headSlot - c->length + 1; if (lo < 0) lo = 0;
            int hi = headSlot;                 if (hi > sim->slotCount - 1) hi = sim->slotCount - 1;
            if (hi >= lo) {
                int slot = irange(&sim->rng, lo, hi);
                sim->cells[i * sim->slotCount + slot] = irange(&sim->rng, 0, sim->glyphCount - 1);
            }
        }
    }
}

int mm_sim_max_instances(const MMSim *sim) {
    return sim->count * sim->slotCount;
}

int mm_sim_write_instances(MMSim *sim, MMGlyphInstance *out, int cap) {
    int n = 0;
    const int   wavesOn   = sim->s.waves;
    const float waveK     = 0.5f;
    const float waveSpeed = 2.4f;
    const MMWorld *w = &sim->world;

    for (int i = 0; i < sim->count; ++i) {
        const MMColumn *c = &sim->cols[i];
        int headSlot = (int)((w->topY - c->yOff - c->headY) / w->spacing);
        int lo = headSlot - c->length + 1; if (lo < 0) lo = 0;
        int hi = headSlot;                 if (hi > sim->slotCount - 1) hi = sim->slotCount - 1;
        if (hi < lo) continue;

        const int *cells = &sim->cells[i * sim->slotCount];
        for (int slot = lo; slot <= hi; ++slot) {
            if (n >= cap) return n;
            int d = headSlot - slot;                       /* 0 == head */
            float y = w->topY - c->yOff - (float)slot * w->spacing;

            float bright, cr, cg, cb;
            if (d == 0) {
                bright = 1.55f;                            /* bright white-green leader */
                cr = 0.78f; cg = 1.0f; cb = 0.82f;
            } else {
                float t = 1.0f - (float)d / (float)c->length;
                bright = t * t * 1.15f;
                cr = 0.10f; cg = 1.0f; cb = 0.26f;
            }
            if (wavesOn) {
                float phase = c->wavePhase + (float)slot * waveK - sim->time * waveSpeed;
                bright *= 0.55f + 0.45f * sinf(phase);
            }
            if (bright <= 0.01f) continue;

            MMGlyphInstance *g = &out[n++];
            g->px = c->x; g->py = y; g->pz = c->z;
            g->cell = (float)cells[slot];
            g->bright = bright;
            g->cr = cr; g->cg = cg; g->cb = cb;
        }
    }
    return n;
}
