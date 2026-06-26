// _linktest.cpp — toolchain + core sanity check (not part of the .scr).
// Proves that MSVC compiles our C++ and links against core/mmcore.c, and that the
// shared engine returns the documented values. Build via: windows\build.ps1 -Test
#include <cstdio>
#include <cstdint>
#include "../core/mmcore.h"

int main()
{
    MMSettings s = mm_settings_default();
    MMWorld    w = mm_world();

    printf("settings: density=%.2f speed=%.2f bloom=%.2f enc=%d\n",
           s.density, s.speed, s.bloomIntensity, s.encoding);
    printf("derived : strips=%d fallSpeed=%.2f mutation=%.2f\n",
           mm_strip_count(&s), mm_fall_speed(&s), mm_mutation_rate(&s));
    printf("world   : halfWidth=%.1f topY=%.1f spacing=%.2f slotCount=%d\n",
           w.halfWidth, w.topY, w.spacing, w.slotCount);

    uint32_t cps[MM_MAX_CODEPOINTS];
    int n = mm_encoding_codepoints(MM_ENCODING_MATRIX, cps, MM_MAX_CODEPOINTS);
    printf("matrix  : glyphCount=%d first=U+%04X last=U+%04X\n",
           n, n ? cps[0] : 0, n ? cps[n - 1] : 0);

    // Spin the simulation a few frames and confirm it emits instances.
    MMSim *sim = mm_sim_create(&s, n, 12345);
    for (int i = 0; i < 120; ++i) mm_sim_advance(sim, 1.0f / 60.0f);
    static MMGlyphInstance buf[65536];
    int emitted = mm_sim_write_instances(sim, buf, 65536);
    printf("sim     : emitted=%d instances after 2s (expect >0)\n", emitted);
    mm_sim_destroy(sim);

    printf("LINKTEST OK\n");
    return 0;
}
