// _linktest.cpp — toolchain + core sanity check (not part of the .scr).
// Proves that MSVC compiles our C++ and links against core/mmcore.c, and that the
// shared engine returns the documented values. Build via: windows\build.ps1 -Test
#include <cstdio>
#include <cstdint>
#include "../core/mmcore.h"

int main()
{
    MMSettings s = mm_settings_default();
    MMWorld    w = mm_world(&s);

    printf("settings: density=%.2f speed=%.2f bloom=%.2f\n",
           s.density, s.speed, s.bloomIntensity);
    printf("derived : strips=%d fallSpeed=%.2f mutation=%.2f\n",
           mm_strip_count(&s), mm_fall_speed(&s), mm_mutation_rate(&s));
    printf("world   : halfWidth=%.1f topY=%.1f spacing=%.2f slotCount=%d\n",
           w.halfWidth, w.topY, w.spacing, w.slotCount);

    // Glyph selection now lives entirely in the renderer's GlyphAtlas (see
    // windows/atlas.cpp), which has no core-only equivalent to link-test here.
    // This core sanity check just needs *a* plausible glyph count to spin up
    // MMSim with; it matches the atlas's fixed symbol-table size.
    const int kGlyphCount = 58;

    // Spin the simulation a few frames and confirm it emits instances.
    // Aspect <= 0 means "no real viewport" -- falls back to the reference
    // 16:9 lane width (see MM_REFERENCE_ASPECT in mmcore.h) rather than the
    // ultra-widescreen widening a real renderer would pass in.
    MMSim *sim = mm_sim_create(&s, kGlyphCount, 12345, 0.0f);
    // Feeds wall-clock time into the sim; mmcore.c only acts on this at
    // 11:11 AM/PM (see the "11:11" easter egg in mmcore.c), so 13:37:42
    // here just exercises the plumbing without triggering it.
    mm_sim_set_clock(sim, 13, 37, 42);
    for (int i = 0; i < 120; ++i) mm_sim_advance(sim, 1.0f / 60.0f);
    static MMGlyphInstance buf[65536];
    int emitted = mm_sim_write_instances(sim, buf, 65536, 1.0f);
    printf("sim     : emitted=%d instances after 2s (expect >0)\n", emitted);
    mm_sim_destroy(sim);

    printf("LINKTEST OK\n");
    return 0;
}
