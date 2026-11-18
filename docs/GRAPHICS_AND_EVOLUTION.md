# Petri graphics and evolution plan

**Status:** Plan, November 18, 2026. Follows the dual-processor cutover
([DUAL_PROCESSOR.md](DUAL_PROCESSOR.md)). Simulation steps 1 and 2 are
implemented natively and in the ESP32 build, not yet run on the board (see
their notes below). The rest is not implemented yet.

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
     320 (checkpoint version 16, 92,938 bytes at 128×64), with 16-bit slots
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
     17). Pattern hides prey in vegetation and shows it on bare ground;
     limbs set foraging speed and chase odds. Matched runs with a neutral
     control are in the experiments log. `Look` bits 9–20 carry the genes;
     `render/render_state.hpp` documents the layout. The observatory
     configuration leaves morphology off until the renderer draws it.
3. **Neural controllers.**
   - A fixed-topology int8 network of about 12 inputs, 8 hidden units, and
     6 outputs, about 150 bytes per creature. It is inherited with mutation.
   - It chooses movement, feeding, attack, flight, and mating.
   - Benchmark the per-decision cost on the ESP32, and compare against the
     rule-based controls and a neutral control.
4. **Plants, eggs, and species.**
   - Plant genotypes that trade growth against toxicity.
   - Eggs and dormancy that predators can eat.
   - Mating limited by genetic compatibility, and detection of
     reproductively isolated clusters as candidate species.

## Order

The tracks can run side by side.

- **Graphics, steps 1 and 2:** board work on the SAMD51 side only, with no
  change to the stream except a small header field.
- **Simulation, steps 1 and 2:** native work first, then the board.

Graphics step 3 waits for simulation step 2. Both tracks then share the
`Look` format.
