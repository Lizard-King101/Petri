# Petri graphics and evolution plan

**Status:** November 18, 2026. Follows the dual-processor cutover
([DUAL_PROCESSOR.md](DUAL_PROCESSOR.md)). Graphics step 1 and the weather part
of step 2 are done, and simulation steps 1 and 2 are merged (see Progress and
the step notes). Morphology is not yet drawn or enabled in the observatory.

With the world on the ESP32 and the SAMD51 drawing from a render stream, each
chip has room for its own job. This plan spends that room on two tracks that
meet at one interface: the creature `Look`. Biology decides how a creature
looks, and the renderer only draws it.

## Measured budgets

| | Measured | Source |
| --- | --- | --- |
| Panel refresh (Protomatter) | 235 Hz at depth 4, 233 Hz at depth 5, 162 Hz at depth 6 | `firmware/gfxbench` |
| SAMD51 CPU left after refresh | 82–85% at every depth | `firmware/gfxbench` |
| Frame push (`show()`) | 4.3 ms at depth 4 or 5, 6.2 ms at depth 6 | `firmware/gfxbench` |
| Close-view render | about 10.5 ms per frame; life view 2.4 ms | SAMD51 self-test |
| SAMD51 free RAM | about 100 KB between heap and stack | display firmware |
| ESP32 simulation | about 242 ticks/s for the observatory world (4.1 ms per step) | ESP32 self-test |
| Highest display speed | 8 ticks/s, which leaves core 1 about 97% idle | display firmware |
| ESP32 encode | 4.8 ms per update | world app telemetry |
| ESP32 heap | 128 KB free beside the static world | world app telemetry |
| ESP32 static DRAM | 115.7 of 180.7 KB; Bluetooth shrinks the segment to 124.6 KB | `idf.py size` |
| Link | 721 KB/s each way; the stream uses at most 33% | link soak, stream tests |

Depth 5 costs the same as today's depth 4 and doubles the color levels.
Depth 6 still refreshes without flicker. At display speeds the simulation's
limit is RAM, not CPU. CPU matters only for fast-forward.

## Graphics track (SAMD51)

Everything here is presentation and never feeds back into the biology.

1. **Renderer core.**
   - Move to Protomatter depth 5, then test depth 6 on the panel.
   - Pace frames for 30 fps.
   - Cache the terrain layer, which takes most of the close-view cost, and
     redraw it only when the camera or the cells under it change.
   - Gate: 30 fps sustained at depth 5 with at least 30% CPU spare.
2. **Light and atmosphere.**
   - A day/night cycle from the simulation's daylight, sent in the update
     header.
   - Creature eyes and fruit that glow at night.
   - Rain and drought particles driven by the coarse rainfall field, sent at
     a low rate.
   - Shimmering water and plants that sway in the wind.
3. **Procedural morphology.** `Look` grows to carry body size, pigment,
   pattern, and appendages. The SAMD51 generates sprites for each zoom
   level from the look and caches them per distinct look. Depends on
   simulation step 2.
4. **Camera director.**
   - Automatic framing of the interesting events: hunts, births, the last
     members of a lineage, and newly successful mutants.
   - Cooldowns so the camera does not jump around constantly.
   - The direction comes from the spec's automatic camera section.
5. **Data views.** Lineage colors and maps of density or traits, as extra
   text-free views.

### Progress

- **Renderer core: done.**
  - A native profile put 78% of close-view drawing in `Panel::ground_pixel`,
    the dual-grid terrain.
  - `Panel` now keeps an exact terrain cache. A pan shifts the cached pixels
    and draws the exposed strips. A changed cell redraws only the pixels its
    dual-grid tiles touch, tracked through a 240-entry ring of the cells
    seen in view.
  - In `tests/render_stream_test.cpp`, 4,800 cached frames per world size
    match full draws, while drawing 81.5% fewer terrain pixels in 64×32 and
    90.2% fewer in 128×64.
  - The display firmware runs Protomatter depth 5 at 30 fps
    (`PETRI_COLOR_DEPTH` and `PETRI_FRAME_RATE` override both). It writes
    frames straight into the canvas.
  - On the board: a steady 30 fps and 8 updates/s at 8 ticks/s. The worst
    frame including the push is 9–15 ms, against 19 ms before at 20 fps.
  - Depth 6 is built but not yet judged on the panel.
- **Weather from the real climate: done.**
  - `RenderState` carries a `RenderClimate`: the 45 coarse rainfall nodes,
    the seasonal bias, and whether the weather is active. A new stream
    record sends it in every update, about 50 bytes.
  - `rain_at()` reproduces `World::weather_at()` exactly, which a test
    checks at every cell.
  - The panel draws:
    - rain as streaks on fixed diagonal lanes in world pixels, denser where
      the cell's rain is higher;
    - a warm grade in the dry season and a cool one in the wet season;
    - glints on wetland ground.
  - Rain also shows in the habitat view.
  - On the board with weather, the worst frame is 15–19 ms at 30 fps, and
    the fingerprints still match native.
- **Storm fronts in the biology** (`--storms`, on in the observatory world).
  - **What changed.** The weather used to be a slow supply bias that rained
    somewhere almost all the time: 13–71% of the map was above the rain
    threshold over a season, and never 0%. Now up to four storms cross the
    map. Each epoch sets a prevailing wind from one of eight directions.
    Storms move at 0.19–0.43 cells a tick, have a radius of 8–21 cells, and
    are born about 13 times as often in the wet season.
  - **Water.** A storm rains only where the regional climate is wet. Its
    rain recharges a soil-water store at each of the 45 climate nodes, which
    drains with a time constant of 1,024 ticks. Wet ground reads that store
    instead of the steady bias, and drought still dries steadily.
    `storm_unit` (155) is calibrated so storms deliver the old water: across
    eight seeds the mean ratio was 1.01 (0.68–1.41).
  - **Coverage.** The whole map was rain-free 10–26% of the time, and at most
    16–25% rained at once.
  - **Ecology,** at tick 50,000 on eight matched seeds, steady rain vs storms:

    | Measure | Steady rain | Storms |
    | --- | ---: | ---: |
    | Population | 58 | 52 |
    | Births | 21,426 | 19,481 |
    | Scrub / grassland / wetland cells | 4,821 / 2,411 / 959 | 5,280 / 1,734 / 1,179 |
    | Fruit eaten | 11,006 | 11,048 |
    | Extinctions | 0 | 0 |
    | Blocked births (mean) | 111 | 447 (one seed 3,535) |

    Storms polarize the habitat into wetter wetlands and less grassland. The
    booms after storms hit the 160-creature limit more often.
  - **Checkpoints and fingerprints.** Checkpoint version 16 stores the flag,
    the storms, and the soil water. Version 15 resumes with steady rain. With
    storms off, all 98 baseline outputs are unchanged, and v15 checkpoints
    resume with identical fingerprints. A storm world's save and resume is
    byte-identical.
  - **Rendering.** The renderer receives the storms in the climate record and
    draws exactly `World::storm_rain_at` (tested at every cell). Rain uses
    diagonal lanes, one pixel right for every two down, with a phase per lane.
    On the board, fingerprints matched native at ticks 299, 323, and 374.
  - **Known issues on the board.** A QSPI save drops the panel to about
    16 fps for 1.8 s, because sector erases run in the display loop. The
    `restores` counter counts a restore when it is offered, even if the ESP32
    rejects it.
- **Water reflections.** Each 9×6 patch of world pixels holds one short
  light blue-white horizontal glint. It fades in, sways a pixel either way,
  and fades out on its own 1.4–2.3 s cycle. `W` over USB, and `--rain 0` in
  `petri_snapshot`, hide rain so the water can be judged alone.
- **Plants, first pass.**
  - Each cell's plants are placed from a hash of the cell, so tufts no longer
    sit at the same spot in every cell. Bases spread over the lower part of
    the cell, and plants that reach into the next cell are drawn even when
    their own cell is just outside the view.
  - Density shows as tufts: up to three per cell at zoom 5 and two at zoom 4.
  - Each biome has its own forms. Grassland grows tussocks whose blades arch
    outward, with seed heads when lush. Grass turns to straw as moisture
    falls toward the scrub edge. Wetland grows reeds with cattails, plus
    lily pads and blooms on open water (moisture 184 and up). Scrub plants
    are stiff and spiky.
  - Scrub ground carries stones and, where driest, cracks. Both are part of
    the cached terrain, because scrub detritus is nonzero in only about 1% of
    cells and its plants grow only at the grassland edge. Twigs appear where
    there is detritus.
  - Wind follows the storms. Inside a storm plants lean up to three pixels
    downwind and shake. Elsewhere a breeze gust rolls across the field every
    3.4 s, in the prevailing direction.
  - At zoom 3, plants stay on the ring of pixels the creature and carrion
    leave free.
  - On the board: 30 fps, worst frame 23–27 ms, unchanged from before.
  - `petri_snapshot --biome 0|1|2`, `--x/--y`, and `--storm 1` hold the
    camera over a biome, a cell, or the strongest storm.
- **Plants, second pass.**
  - Each visible cell draws a plant amount that steps toward the real one
    every 70 ms, so a grazed clump shrinks over a few frames.
  - A plant meal makes the plant jerk for 450 ms, and bits of leaf fly from
    the creature's mouth.
  - The wide overview averages the tints of the four cells under each pixel,
    so biome edges blend. It shades open water bluer with depth and adds a
    fixed grain. Creature and carrion colors are unchanged.
- **Procedural morphology (graphics step 3): done.**
  - Creatures with morphology genes take their body color from pigment, on
    the ground-tone scale from rust (driest scrub) through ochre, green, and
    teal to blue (deepest water). The colors follow the ground's hues but
    stay brighter, so a matched creature blends in without vanishing.
  - Size class scales the sprite about its feet: 5 to 10 pixels wide at
    zoom 4, and 11/16 to 20/16 of the cell at zoom 5. Big creatures overlap
    their neighbours.
  - Limbs set leg length: 1 to 3 rows at zoom 4, and at zoom 5 they lift or
    lower the body by up to 3 pixels.
  - Pattern draws plain, spots, stripes, or bold blotches on the torso.
    Faction markers stay on top.
  - Looks without morphology genes draw as before.
  - The observatory configuration now enables morphology. On the ESP32 its
    fingerprints match native, at 152 ticks/s.
  - A world restored from an older checkpoint keeps morphology off until a
    fresh start.
- **Carcass remains.** `RenderCell` bit 7 marks a fresh carcass, at least
  5 of the 8 units a death leaves. At zoom 4 and 5 a fresh carcass is a red
  lump with ribs showing; below that it is a skeleton. After a carcass is
  eaten, faded bones stay briefly. Zoom 3 and the overview keep magenta
  carrion.
- **Day/night: deferred to the biology.** The simulation has no daylight,
  so lighting tied to time of day would only be decoration. It is a joint
  item: the simulation track adds daylight that affects growth and
  activity, then the renderer lights the world from it.
- **Not done yet:** wind sway and glowing eyes, which wait for daylight.
- **`apps/snapshot.cpp` (`petri_snapshot`)** renders a PNG contact sheet of
  every view over a few frames, for reviewing graphics without the board.

## Simulation track (ESP32)

Every change to the biology gets a new checkpoint version, fresh native
baselines, and matched-seed experiments, as the README's experiments do.

1. **Room and capacity.**
   - Move the `World` and the encoder shadow to the heap, so Bluetooth fits
     in static DRAM.
   - Add deterministic early-outs to the full-grid environment passes for
     fast-forward, with fingerprints unchanged.
   - Raise creature capacity above 160. The creature record then needs to
     span several stream records.
   - *Done natively, November 18, 2026.* The world app's static DRAM fell
     from 115,732 to 15,908 bytes, and a world build with NimBLE links.
     Early-outs in `recycle` and `grow_plants` are byte-identical across 68
     matched runs and speed the native observatory world by 8%. Capacity is
     320 (checkpoint version 17, 92,938 bytes at 128×64), with 16-bit slots
     and slot-range creature records in the stream. See the README's memory
     layout section and DUAL_PROCESSOR.md for the figures. Heap use with
     Bluetooth up is not yet measured on the board.
2. **Morphology genes with costs.**
   - Body size, which trades metabolism against bite, attack, and defense.
   - Pigment matched to the biome, which hides prey from hunters. This is
     causal camouflage, not decoration.
   - Pattern and limbs, which trade speed against cost.
   - Each gene maps to a visible part of `Look`.
   - *Done natively, November 18, 2026* (`--morphology`, checkpoint version
     18). Pattern hides prey in vegetation and shows it on bare ground;
     limbs set foraging speed and chase odds. Matched runs with a neutral
     control are in the experiments log. `Look` bits 9–20 carry the genes;
     `render/render_state.hpp` documents the layout. The observatory
     configuration leaves morphology off until the renderer draws it.
3. **Neural controllers, and everything after them,** moved to
   [EMERGENT_LIFE.md](EMERGENT_LIFE.md). Matched runs showed that the current
   rules cannot keep a food chain: carnivores die out on all 8 seeds, with
   or without adaptive hunting or richer soil. That document holds the gap
   analysis, a five-phase roadmap (brain and senses, trophic economics, media
   and body plans, society and species, life history), and the Phase 1–2
   design.

## Order

The tracks can run side by side.

- **Graphics, steps 1 and 2:** board work on the SAMD51 side only, with no
  change to the stream except a small header field.
- **Simulation, steps 1 and 2:** native work first, then the board.

Graphics step 3 waits for simulation step 2. Both tracks then share the
`Look` format.
