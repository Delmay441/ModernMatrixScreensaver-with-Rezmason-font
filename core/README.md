# mmcore — the shared engine

Portable **C99** (no OS, no GPU, no dependencies) holding the parts of Modern Matrix that
are identical on every platform:

- the **digital-rain simulation** (falling columns, heads/trails, mutation, brightness waves,
  fall-in-from-black spawn, respawn);
- the **settings model** (`MMSettings`) and its **derived values** (`mm_strip_count`,
  `mm_fall_speed`, `mm_mutation_rate`);
- the six **glyph encodings** (`mm_encoding_codepoints`);
- the **world constants** (`mm_world`) and the **glyph-instance layout** (`MMGlyphInstance`).

The goal: **tweak the rain's behaviour once, here, and every platform picks it up.**

## Who links it

| Platform | Links `mmcore.c`? | Provides itself |
| --- | --- | --- |
| macOS `.saver` + `.app` | yes (clang → object, bridged into Swift via `mmcore.h`) | Metal renderer, Core Text atlas, SwiftUI settings, host shell |
| Windows `.scr` *(planned)* | yes (MSVC/clang) | Direct3D 11 renderer, DirectWrite atlas, Win32 config dialog, `.scr` shell |

Each platform keeps its own **renderer, glyph-atlas rasteriser, settings UI, and host shell** —
only this engine is shared. See `../PORTING.md` §0.

## API sketch

```c
MMSettings s = mm_settings_default();
int glyphCount = mm_encoding_codepoints(s.encoding, NULL, 0);   // query count
/* ...rasterise those code points into your atlas... */
MMSim *sim = mm_sim_create(&s, glyphCount, seed);

/* each frame */
mm_sim_advance(sim, dt);
int n = mm_sim_write_instances(sim, instanceBuffer, capacity);  // -> MMGlyphInstance[n]
/* ...upload instanceBuffer, draw n instanced quads... */

/* on a settings change */
mm_sim_update(sim, &newSettings, newGlyphCount);

mm_sim_destroy(sim);
```

`MMGlyphInstance` is 8 floats — `px, py, pz, cell, bright, cr, cg, cb` — laid out to drop
straight into a GPU instance buffer (it matches the Metal vertex struct in
`../Resources/Shaders.metal`; mirror the same layout in HLSL on Windows).

## Building / testing

The macOS `build.sh` compiles `mmcore.c` automatically. To exercise it standalone:

```sh
clang -std=c99 -I core your_test.c core/mmcore.c -o /tmp/t -lm && /tmp/t
```
