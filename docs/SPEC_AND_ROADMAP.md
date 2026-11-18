# Petri — Artificial Life Ecosystem

**Technical specification and development roadmap · Version 0.1 · November 16, 2026**  
**Status:** Design baseline. P0–P2 are implemented, with much of P3–P5; see the README. Dual-processor work now follows [DUAL_PROCESSOR.md](DUAL_PROCESSOR.md).  
**Primary target:** Adafruit MatrixPortal M4 with a 64×32 HUB75 RGB LED matrix.

## 1. Executive summary

Petri is a persistent, autonomous artificial-life simulation presented through a small physical LED matrix. A viewer observes an alien biosphere in which plants and creatures inherit genes, compete for finite resources, reproduce with variation, and experience differential survival. Populations may adapt, diverge into lineages, occupy ecological niches, enter dormancy, recover from disasters, or become permanently extinct.

The display is a **camera into the world**, not necessarily the full world. Its 64×32 pixels can show the landscape at one pixel per world cell or zoom in to procedurally constructed 4×4, 8×8, and larger organism sprites. An optional desktop observatory exposes genetics, ancestry, simulation metrics, world navigation, and controls. The ecosystem must continue functioning without a connected desktop computer.

**Implementation strategy:** Prove a small, deterministic simulation on the SAMD51 and Linux first. Keep simulation, rendering, persistence, and transport decoupled so that the simulation may later migrate to custom firmware on the onboard dual-core ESP32 if hardware measurements justify the added complexity.

### Decisions already made

| Area | Decision |
| --- | --- |
| Physical display | Existing 64×32 RGB matrix; landscape orientation assumed until verified |
| First simulated world | 64×32 cells; expand only after validating ecology and performance |
| Initial processor | SAMD51; ESP32 retains stock Wi-Fi coprocessor firmware initially |
| Languages | Portable C++ simulation core; Arduino/C++ hardware adapter |
| Life model | Heredity, mutation, selection, resource competition, reproduction, death |
| Diets | Emerge from variable digestion traits and behavior; no immutable herbivore/carnivore classes |
| Extinction | Permanent when all living and viable dormant stages of a lineage are gone |
| Dormancy | Eggs, seeds, or spores persist as vulnerable biological entities, including potential food |
| Long-term world | Candidate 128×64 cells, with 256×128 reserved as a performance experiment |
| Zoom | World overview, 1×, 2×, 4×, 8×, and optional 16× microscope mode |
| Time control | Target 1×, 2×, 4×, and 8×; actual throughput measured, never faked |
| Input hardware | Two onboard buttons; no external components required |
| Desktop application | Optional, not required to run or save the world |
| Networking | Local-first; a Bluetooth LE phone controller on the ESP32 (decided November 18, 2026; see DUAL_PROCESSOR.md) |

### Non-goals for the first release

- Realistic fluid simulation, rigid-body physics, or 3D rendering.
- Unlimited world sizes, unlimited organisms, or unconstrained neural architecture growth.
- Online multiplayer, blockchain integration, accounts, monetization, or external APIs.
- A guaranteed stable ecosystem or guaranteed appearance of specific species.
- A claim of unlimited open-ended biological novelty. Organisms can evolve only within the heritable traits, sensors, actions, and ecological mechanisms the simulator makes possible.
- Reflashing either processor before the existing WIRE tranche tracker has been inventoried and backed up where feasible.

## 2. Design principles and simulation invariants

1. **Evolution must be causal.** A genome changes an organism's capabilities or decisions; those changes affect survival and reproductive success; offspring inherit altered genes.
2. **No scripted outcomes.** Do not spawn a predator lineage or force a species split because an event timer says to. A system can supply an environmental pressure; the resulting ecological outcome must arise from simulation rules.
3. **Extinction means something.** A lineage is recoverable only while viable individuals or dormant reproductive stages remain in the simulated biosphere. A permanently extinct lineage does not reappear under its old identity.
4. **Everything alive consumes resources.** Reproduction has energy and/or material costs, movement and maintenance consume energy, and every food source has a physical origin and renewal mechanism.
5. **External energy is explicit.** Sunlight and, in later alien biomes, geothermal/chemical energy can enter the ecosystem. Nutrients and biomass cycle approximately through plants, consumers, detritus, and decomposers; the simulation need not perfectly model chemistry, but it should not create unlimited food from nothing.
6. **The simulation is independent of display and wall-clock rendering.** An off-screen creature continues living. Zoom or camera changes do not change the biology.
7. **Fixed timestep and reproducibility.** Given the same initial state, parameters, random seed, and sequence of commands, a native simulation run should reproduce its state trajectory. Prefer explicit integer or fixed-point arithmetic where cross-platform determinism matters.
8. **Capacity must be observable.** Never silently kill or erase organisms to satisfy an array limit. Log capacity pressure and treat saturated simulations as invalid for evolutionary conclusions until tuned.
9. **A power interruption must not destroy months of history.** Use versioned, validated, rotating checkpoints, accepting only a bounded amount of progress loss between saves.

## 3. User experience

The default experience requires only power. The matrix displays a living landscape: vegetation grows, creatures move and hunt, organisms reproduce, dead matter decays, climate changes, and occasional rare events become visually noticeable.

### Display modes

- **Life view (default):** Terrain, vegetation, organisms, movement, combat, feeding, reproduction, and environmental effects.
- **Biome view:** Local substrate, light, moisture, temperature, and environmental gradients encoded in color.
- **Population view:** Species/lineage regions, relative densities, and active organism highlights.
- **Event view:** Temporary visual and short text notifications for significant events; never permanently obstruct the world.
- **Inspector (later):** Follows a selected organism, displaying its generated morphology and compact statistics.

### Proposed two-button controls

| Input | Action |
| --- | --- |
| Up tap | Cycle zoom or presentation modes, depending on current screen |
| Down tap | Cycle 1× → 2× → 4× → 8× simulation speed |
| Up hold | Change automatic camera target/mode |
| Down hold | Pause or resume |
| Both hold | Open minimal settings/selection menu |

The final button map should be validated on the physical board. The firmware must debounce buttons, distinguish short/long presses, and avoid accidental actions during startup. On-screen legends can appear briefly after an input.

### Automatic camera

When the world exceeds the panel dimensions, the camera may wander among habitats and follow high-interest events: new lineages, rare hunts, hatching, disasters, or last surviving organisms. Camera motion and zoom are presentation-only. Use event priorities and cooldowns to avoid a constantly jumping view. Manual camera control through the optional desktop observatory overrides automatic movement.

## 4. World and ecology model

### 4.1 Spatial representation

- Phase 1: 64×32 world cells, mapped directly to LEDs at 1×.
- Expansion target: 128×64 world cells; the 64×32 panel becomes a viewport.
- World coordinates belong to simulation entities; screen coordinates are computed by the renderer.
- Initially use a compact two-dimensional cell grid with a spatial index for mobile organisms and dormant stages.
- An environmental cell may contain substrate, vegetation, detritus, and an organism without collapsing those into one mutually exclusive object. Use a bounded local occupancy rule for mobile creatures during the MVP.
- Terrain should include traversability and environmental parameters, not only decorative colors.
- World-edge behavior is an explicit configuration choice: non-wrapping for initial tests; wrapping or migration boundaries deferred.

### 4.2 Per-cell environment

Candidate quantities:

| Field | Role | Initial implementation |
| --- | --- | --- |
| Substrate/terrain | Movement and nutrient/plant suitability | Small integer enum |
| Moisture | Water availability, drought response | 8-bit field |
| Nutrients | Growth resource | 8-bit field |
| Detritus | Dead biomass, recycled by decomposers | 8-bit field |
| Plant genotype index | Reference to a heritable plant template | Bounded integer ID |
| Plant biomass | Edible/competitive plant presence | 8-bit field |
| Light | Primary energy input | Derived from global day and local conditions when possible |
| Temperature/toxicity | Later environmental selection pressures | Derived or stored only if needed |

Aim for **4–6 bytes per cell** through compact layouts and derived fields; benchmark before finalizing field widths. A 128×64 world at six bytes/cell occupies approximately 48 KiB.

### 4.3 Energy and nutrient cycle

Initial loop: sunlight → plant growth → grazing → metabolism/reproduction/death → detritus → nutrients → plants. Add consumption of animal biomass, eggs, spores, and fungal material as ecological features mature.

Each update must maintain nonnegative resources and bounded state. Plant photosynthesis and later vent chemistry may add usable energy; metabolism, decay, and transfer inefficiencies dissipate it. Nutrients should be finite locally, recyclable, and capable of redistribution through diffusion or weather. Avoid computationally expensive full-grid diffusion at every creature tick; use multi-rate environmental updates.

### 4.4 Environmental events

**Initial:** Daylight cycle, rainfall and drought, gradual moisture and nutrient variation.  
**Later:** Seasons, storms, wildfire, flooding, pathogen outbreaks, anomalous chemical events, and climate drift.  
**Alien biome candidates:** Spore forest, toxic wetland, geothermal vent field, mineral garden.

Separate three categories:

1. **Ambient cycles:** Predictable or slowly drifting conditions.
2. **Stochastic disturbances:** Rare triggered environmental events that change conditions rather than predetermine ecological results.
3. **Emergent events:** Food shortages, population booms, collapses, extinctions, and adaptations arising directly from state.

Disasters should be rare enough that natural selection and ecological succession remain observable. Add new biomes only after the baseline food web is stable enough for meaningful experiments.

## 5. Organisms, genetics, and evolution

### 5.1 Creature lifecycle

Each active organism has an ID, coordinates, age, energy, health, parent/lineage references, genome, physiological state, and behavior state. It senses its surroundings, makes a decision, pays action/metabolic costs, interacts with the world, may reproduce, and eventually dies. Dead biomass enters the environment.

Representative actions: move, rest, feed on vegetation, consume detritus/animal matter, attack, evade, mate/reproduce, lay dormant stage. Expose actions gradually across milestones; do not put an unfinished action in a genome with no ecological effect.

### 5.2 Heritable traits

Start with a compact fixed-length scalar genome. Candidate normalized genes:

- Plant digestion efficiency
- Meat digestion efficiency
- Egg/spore digestion efficiency (later)
- Metabolic efficiency
- Movement rate and cost
- Sensory range
- Body size
- Defense/armor investment
- Aggression threshold
- Reproductive energy investment
- Dormancy propensity
- Toxin/heat tolerance (later)
- Pigmentation and morphological parameters (later)

Traits require tradeoffs. Greater body size, speed, sensory ability, armor, or reproductive output cannot all increase without some cost. Diet type is a consequence of digestion efficiency, available food, and decisions; **do not encode a permanent `HERBIVORE`/`CARNIVORE` class**.

### 5.3 Reproduction and mutation

- Proof of concept: Asexual reproduction with offspring genome copying, bounded mutation, and energy costs. This is enough to demonstrate heredity, variation, and selection.
- Future: Optional sexual reproduction, genome crossover, mate choice, compatibility thresholds, and reproductive isolation.
- Mutations: Mostly small numeric perturbations with rare larger changes; trait clamping and validation on every mutation.
- Do not let new offspring appear if parental energy/material requirements fail.
- Preserve ancestor IDs and generation depth in a bounded lineage record. Store long-term aggregate history separately from live entity state.

### 5.4 Evolvable behavior / tiny neural brains

After scalar-genome selection is validated, introduce a **fixed-topology, small neural controller** whose weights are heritable. Example sensors: hunger/energy, local plant biomass, prey and predator signals, nearby eggs, crowding, light, moisture, and proximity/direction. Example outputs: movement preference, feed, attack, evade, rest, reproduce.

Use compact signed weights, integer/fixed-point activation where practical, and a strict per-decision compute budget. The neural network is **not trained during the organism's lifetime** initially: selection across generations changes inherited weights. Complex behaviors may emerge, but only from the available sensor/action vocabulary.

An illustrative target is roughly 8–12 inputs, 6–8 hidden units, and 5–7 outputs, with quantized weights. Treat the final topology and genome size as benchmark decisions rather than locked facts.

### 5.5 Plant, fungi, and other producers

Plants inherit growth rate, resource efficiency, drought tolerance, defense/toxicity, dispersal, and investment in fruit/seed/spore production. In the MVP, vegetation may be represented by biomass in cells plus a compact genotype catalog, not a heavyweight object per blade of grass.

New food sources should come from mechanics that exist in the model: edible plant tissue, fruit, seeds, spores, detritus, fungi, eggs, animal tissue, and eventually chemical-energy producers. A lineage can evolve to exploit a source via traits and actions; genuinely novel food chemistry cannot appear without a supporting mechanism.

### 5.6 Dormancy, egg predation, and extinction

Dormant stages are biological entities with genotype, viability, energy reserves, age, and location. Candidate evolvable properties include dormancy duration, shell strength, burial depth, egg size, hatching thresholds, chemical camouflage, and parental reproductive investment. Each has a cost or risk.

Predators may evolve the ability to detect, excavate, crack, digest, or opportunistically consume eggs/spores using sensory, behavioral, and digestive traits. This creates selection pressure on both egg consumers and their prey. Eggs and spores are not immortal backups; they can be eaten, rot, or fail to hatch.

**Extinction rule:** A lineage is permanently extinct when no living individuals, viable dormant stages, or other modeled populations of that lineage remain. Later migrations must never recreate an extinct lineage by fiat; any external population must have a consistent modeled source and continuity of identity.

### 5.7 Lineage and species detection

From the first milestone, track immutable organism IDs, parentage, and founder lineage IDs. Asexual populations can be clustered by ancestry and genetic distance, but call these **provisional lineages** rather than claiming strict biological species. Once sexual reproduction and compatibility exist, identify persistent reproductively isolated clusters as candidate species.

The desktop observatory can generate memorable alien taxonomic labels, while keeping stable machine IDs. Names, colors, and visual markers must not affect ecology unless encoded as actual heritable traits. Detect lineage births, branches, extinction, and re-emergence from *viable* dormant stages.

## 6. Rendering, zoom, and generated morphology

The physical panel always renders **64×32 output pixels** regardless of world size. Zoom affects the number of world cells visible and the level of detail, not panel resolution.

| Zoom | World-cell viewport | Interpretation |
| --- | --- | --- |
| 0.5× | 128×64 | Full expanded world; aggregate groups of 2×2 cells into one LED pixel |
| 1× | 64×32 | One world cell per LED pixel; creatures often appear as 1-pixel markers |
| 2× | 32×16 | Coarse body shape and relative size |
| 4× | 16×8 | Simple silhouettes, primary appendages, major plant shapes |
| 8× | 8×4 | Procedurally generated anatomy and vegetation details |
| 16× | 4×2 | Optional single-organism/microscope inspection view |

The 0.5× full-world view applies to a 128×64 world. On the first 64×32 world, the default 1× view already shows the entire ecosystem.

**Morphology renderer:** Derive body outline, symmetry, pigmentation, appendages, mouths, armor, eyes, frills, and plant forms from relevant genotype/phenotype fields. Higher zoom must redraw from these traits, not merely enlarge a one-pixel sprite. Favor readable silhouettes over excessive detail on an LED panel. Use visual differentiation for feeding specialists and defense traits where reasonable; distinguish cosmetic diversity from biologically functional diversity.

Only the camera's visible area requires full-detail rendering. The simulation must continue updating offscreen cells and organisms. For large-world experiments, do not make unseen populations magically stop evolving; any coarse offscreen simulation must be documented and validated against full-detail results.

## 7. Simulation clock and time acceleration

- Use a **fixed simulation timestep** with deterministic state updates.
- Keep rendering cadence independent from simulation updates; interpolate motion for visual smoothness when practical.
- Target selectable 1×, 2×, 4×, 8× modes, plus pause. Multiple real simulation updates—not a large numerical timestep—advance fast-forwarded worlds.
- Update environmental fields and slow biological processes at coarser fixed intervals than creature actions.
- If requested 8× exceeds capacity, keep the system stable and report **achieved** throughput; never silently drop ecology updates or mislabel the speed.
- Absolute timing (day length, generation length, reproduction cost, plant growth) is subject to headless calibration, not fixed prematurely.

**Critical test:** Identical seeds and controls should produce the same final simulated state at a given tick count whether the visual app runs at 1× or 8×. Avoid feeding host elapsed time or frame rate into the biology.

## 8. Software architecture

```text
petri/
├── README.md
├── docs/
│   ├── SPEC_AND_ROADMAP.md
│   ├── BIOLOGY.md
│   └── PROTOCOL.md                # when dual-processor work begins
├── core/                         # Portable C++: no display, USB, Wi-Fi dependencies
│   ├── world/
│   ├── environment/
│   ├── organisms/
│   ├── genetics/
│   ├── ecology/
│   ├── evolution/
│   ├── telemetry/
│   └── persistence/
├── host/
│   ├── sim-cli/                  # Fast headless Linux runner
│   └── replay/                   # Deterministic replay and benchmark harness
├── firmware/
│   ├── matrixportal-m4/          # Protomatter renderer, buttons, board services
│   └── esp32/                    # Reserved until migration decision
├── desktop/                      # Optional Angular/Electron observatory
└── tests/
    ├── unit/
    ├── properties/
    ├── scenarios/
    └── longevity/
```

The `core` directory should compile natively on Linux and for the target MCU. No simulator rule should directly invoke a display, logging framework, clock, file system, USB, or Wi-Fi API. Use narrow adapters to provide random seed, clock-step commands, persistent storage, telemetry, and world presentation.

### SAMD51-first runtime

The SAMD51 executes simulation and display logic; stock ESP32 AirLift/NINA firmware provides optional networking. For the earliest prototype, disable networking entirely and verify the model, memory use, and LED output before adding Wi-Fi.

### Optional dual-processor runtime

If profiling warrants migration, run the authoritative world and time loop under custom ESP32 firmware. The SAMD51 becomes a renderer/input controller. Send camera requests, input commands, visible world snapshots/deltas, organism appearance traits, and telemetry over a **versioned, checksummed SPI protocol**. Define backpressure, packet-size limits, reconnect/reset behavior, and explicit state ownership.

The ESP32 originally hosts Wi-Fi coprocessor firmware; its CPU is **not automatically available** for arbitrary application code. Custom ESP32 firmware replaces or extends that role and must implement networking through its own stack or a modified coprocessor protocol. The two processors have separate RAM and do not function as a three-core shared-memory machine. Do not move individual neural decisions back and forth across SPI; migrate the whole coherent simulation subsystem if needed.

The existing M4-to-ESP32 programming/passthrough path is documented by Adafruit, but flashing must wait for an explicit implementation gate, with restoration instructions and backups in place.

### Optional observatory transport

Start with USB serial telemetry because it is simple and local. Add ESP32 Wi-Fi networking only when useful. Keep read-only monitoring distinct from state-changing commands. Later support a local authenticated endpoint for controls and snapshots; do not put an unauthenticated world-control service on the public internet.

## 9. Memory and performance targets

### Hardware facts

- ATSAMD51J19: 120 MHz, 192 KiB SRAM, 512 KiB internal flash.
- MatrixPortal M4 includes 2 MiB QSPI flash for assets/persistence.
- ESP32-WROOM-32 class module: typically dual-core Xtensa CPU up to 240 MHz and 520 KiB total chip SRAM; networking/RTOS consume part of it. Confirm the exact module on the physical board before optimizing against an assumed ESP32 variant.
- HUB75 output: 64×32 physical pixels, 2,048 RGB LEDs. Adafruit's examples indicate that display buffering can consume approximately 11–17 KiB depending on bit depth, with additional CPU/display refresh overhead.

### Illustrative expanded-world RAM budget

| Item | Planning estimate |
| --- | ---: |
| 128×64 terrain and ecology, 6 bytes/cell | 48 KiB |
| 160 active organisms, 128 bytes each | 20 KiB |
| 100 dormant genomes, 128 bytes each | 12.5 KiB |
| Plant genotype templates/catalog | 8 KiB |
| Matrix/display memory | ~11–17 KiB |
| Other stacks, caches, allocator and firmware overhead | To measure |

These are provisional engineering budgets, **not measured allocations**. At 256×128, six bytes per environmental cell alone would consume 192 KiB, leaving no working RAM for the M4's other responsibilities. Prioritize packed fields, fixed-capacity pools, spatial indexing, fixed-point math, and derived rather than redundantly stored state.

### Benchmark expectations

1. Measure SRAM usage and free headroom on both native and embedded builds.
2. Measure per-system tick cost: environment, creature sensing, neural evaluation, interactions, reproduction, and rendering.
3. Measure achieved simulation speed under 1×, 2×, 4×, and 8×.
4. Profile worst-case density, reproduction surges, large dormant-egg populations, and disasters.
5. Verify camera changes do not change simulation throughput materially.
6. Record dropped render frames separately from simulation tick throughput.
7. Use a migration decision gate rather than assuming the ESP32 solves everything. Initial guidance: investigate ESP32 offload when the desired world/population cannot meet display responsiveness or accelerated-time goals with safe RAM headroom on M4.

Do not commit to a sustained frame rate or eightfold time acceleration before hardware measurements. LED refresh and the simulation share CPU/memory bandwidth, and the two cores inside the ESP32 also share its RAM and system tasks.

## 10. Persistence, telemetry, and history

### World snapshots

Store versioned world snapshots containing world fields, living/dormant entities, genomes, lineage IDs, PRNG state, simulation tick, and relevant settings. Use checksums and rotating slots with a last-known-good recovery path. Test interrupted-write recovery. Avoid writing every simulation tick; choose a checkpoint cadence that balances expected data loss against flash wear.

### Historical event log

Keep a bounded onboard log and optionally export full history to the desktop. Useful records:

- Birth, death, predation, and cause-of-death counts
- Total and active/dormant population by lineage
- Extinction events and ancestral relationships
- Genome trait distributions, changes in means/variance, and genetic diversity
- Biomass, nutrient, moisture, and energy accounting
- Weather, disease, and biome disturbances
- Population capacity saturation and simulation exceptions
- Simulation tick, seed, version, and performance timings

Do not store every organism's entire historical state indefinitely on the board. Preserve meaningful aggregate statistics and a bounded ancestry/event history; export full datasets to desktop when connected.

### Desktop observatory (later)

Candidate Angular/Electron UI: live world map with camera selection, selected organism phenotype/genome, lineage tree, population and trait charts, event timeline, pause/speed controls, backup/restore, and reproducible experiment presets. The desktop is an **observer/controller**, never the only owner of the running world.

## 11. Verification and scientific acceptance criteria

The aim is observable selection, not merely colorful randomized motion.

### Core correctness

- Identical seeds and commands produce identical checkpoints at a fixed simulation tick on the same simulator build.
- Genome mutation respects bounds and documented distributions.
- Reproduction requires energy/materials; failed reproduction does not produce offspring.
- Resources, health, population counters, and array bounds cannot go negative or overflow.
- Carcasses and failed/dormant eggs follow documented resource-conversion rules.
- Permanent extinction and delayed hatching from viable eggs are distinguishable.
- Simulation behavior does not depend on camera position, zoom, or render FPS.
- Save/load round trips restore PRNG state and reproduce subsequent outcomes.

### Evolution experiments

Run multiple seeded headless experiments rather than selecting one favorable example:

1. **Scarcity selection:** Under lower food availability, compare survival and heritable metabolic traits with a resource-rich control.
2. **Diet divergence:** Give populations different available food types; compare inherited digestion efficiencies across generations.
3. **Predator/prey pressure:** Enable predation and compare defense, movement, and behavior trait changes against controls.
4. **Egg arms race:** After implementing egg consumption, compare predator egg-access traits and prey egg defenses against controls.
5. **Dormancy recovery:** Apply a severe drought; verify only lineages with surviving viable stages can return.
6. **Extinction permanence:** Destroy all viable reproductive material from a lineage and verify no spontaneous resurrection.
7. **Neutral control:** Run without selection pressure to distinguish drift from directional adaptation.

Success means consistent, measurable differences in **inherited trait distributions and reproductive success**, not that every run must reach a particular species or equilibrium. Real ecological collapse may occur; identify design errors separately from valid extinction scenarios.

### Endurance and performance

- Native simulator completes long-run stress experiments (initially hundreds, later thousands of generations) without crashes or corrupt state.
- Hardware operates continuously for a multi-hour soak test before being considered always-on.
- Requested fast-forward throughput is measured against achieved simulation ticks/second.
- Resource saturation, fragmented populations, and extinction events are reported rather than silently patched.
- Power-cycle recovery works from a valid snapshot and a deliberately interrupted save.

## 12. Development roadmap and gates

No calendar dates are imposed until the hardware/toolchain has been inspected. Milestones are sequenced by dependency and have exit criteria; later work can be reprioritized after measurement.

### P0 — Hardware preflight and preservation

**Tasks:** Verify board variant, physical matrix orientation and power, existing firmware type, available source files, USB bootloader/serial access, and development toolchain. Back up the old WIRE tracker wherever possible. Record any credentials separately from version control. Verify a minimal Protomatter test and basic buttons. Document how to restore the original firmware/asset backup.

**Exit gate:** Reliable firmware upload and LED test; previous application preserved to the extent technically possible; no destructive firmware changes made implicitly.

### P1 — Headless first-life simulation

**Tasks:** Create portable C++ core, deterministic RNG and fixed timestep, 64×32 environment, sunlight/moisture/nutrients, plant biomass growth, basic creatures, energy accounting, death, asexual reproduction, and a handful of heritable scalar genes. Build native CLI output, scenario tests, and lineage counters.

**Exit gate:** Sustained reproduction and death; mutations inherited; different resource pressures produce measurable differences in descendants; native simulator can repeat a seeded run.

### P2 — MatrixPortal proof of concept

**Tasks:** Compile the shared core for SAMD51, render the full 64×32 world via Protomatter, show moving creatures and plant growth, wire buttons for pause/time and simple view cycling, and log metrics over USB. Measure actual RAM and CPU usage.

**Exit gate:** Autonomous simulation visibly runs without a desktop application; no freezes over a multi-hour soak; measured performance and memory baseline recorded.

**First meaningful deliverable:** A living, evolving Petri environment on the actual matrix. This is the MVP. Do not require procedural 8× art, ESP32 custom firmware, or desktop UI for this milestone.

### P3 — Food web and emergent behavior

**Tasks:** Add plant/meat digestion tradeoffs, animal predation, feeding competition, decomposed animal biomass, fixed-topology inherited neural controllers, behavior telemetry, and headless selection experiments.

**Exit gate:** Herbivore-like, carnivore-like, and omnivore-like niches can emerge through traits/behavior without assigning permanent diet classes. Behavior evolution is distinguishable from random movement.

### P4 — Dormancy, lineages, and dynamic ecology

**Tasks:** Add eggs/spores and viable-stage accounting, dormant-stage consumption, defenses/egg survival costs, true-extinction rules, plant genotype divergence, provisional lineage clustering, climate cycles, weather events, and ecological recovery. Introduce limited alien environmental chemistry/biomes only after the basic food web is coherent.

**Exit gate:** Demonstrate dormant recovery, permanent extinction, egg consumption, and non-scripted ecological consequences under deterministic scenario tests.

### P5 — Expanded world and multiscale camera

**Tasks:** Expand toward 128×64, optimize compact world storage, implement scrolling viewport and 0.5×–8× zoom, generate higher-zoom sprites from genotype/phenotype, add automatic camera interests, and implement 1×/2×/4×/8× time controls. Add optional 16× inspector only if readable.

**Exit gate:** Offscreen entities continue evolving correctly; zoom does not change biology; full-world overview and detail views are readable; achieved simulation speeds and memory headroom documented.

### P6 — Persistence and desktop observatory

**Tasks:** Add rotating checksummed snapshots on available flash, recovery testing, bounded onboard event history, USB telemetry protocol, optional Angular/Electron observatory, lineage graphs, reproducible scenario exports, and camera control.

**Exit gate:** Restart resumes the same living world; desktop app can connect/disconnect without stopping it; experiments and evolutionary history are inspectable.

### P7 — Conditional ESP32 simulation offload

**Entry gate:** Benchmarks demonstrate a meaningful benefit that outweighs maintaining two firmware applications; the desired world size or fast-forward targets cannot be met comfortably on the SAMD51.

**Update, November 18, 2026:** The project now targets dual-processor operation directly, for a dedicated simulation core and a Wi-Fi phone app. [DUAL_PROCESSOR.md](DUAL_PROCESSOR.md) records the ownership split and the bring-up stages. Benchmarks are still recorded at every stage.

**Tasks:** Preserve/restorable stock NINA firmware, verify passthrough flashing process, develop custom ESP32 simulation firmware, define and implement versioned SPI messaging, move authoritative world ownership to ESP32, leave SAMD51 to render/input, revalidate networking and persistence, and rerun deterministic/endurance tests.

**Exit gate:** Same biology and saved state semantics as the single-processor engine; reliable startup/recovery if either processor resets; documented performance gain; no regression in visual responsiveness.

### Future experiments (explicitly not committed)

- Sexual reproduction, mate compatibility, speciation, and genetic isolation.
- Symbiosis, parasitism, social behavior, parental care, migration, and niche construction.
- Chemosynthetic biomes and multiple energy cycles.
- More expressive evolving morphology and neural topologies.
- Simulation chunking and a 256×128 or larger environment if evidence supports it.
- Multi-device shared biospheres or offline population exchange.

## 13. Risk register and mitigations

| Risk | Why it matters | Mitigation |
| --- | --- | --- |
| Ecology collapses too quickly | No interesting long-run evolution | Headless scenario suite, configurable life-cycle rates, viable dormant stages, statistical testing |
| Ecology becomes permanently saturated | No selection pressure or room for new organisms | Resource costs, local carrying capacity through ecology, overflow telemetry; no silent culling |
| Genetics appears random but has no selective effect | Violates the project's defining goal | Trait-outcome experiments and controls; log reproduction and fitness proxies |
| Neural controllers consume too much CPU | Fast-forward and responsiveness suffer | Fixed topology, quantized weights, bounded sensory fields, profile per decision |
| Large grid exhausts RAM | Crashes or hidden compromises | Packed arrays, small initial world, data-driven budgets, offload only after measurements |
| High zoom appears as enlarged blocks | Undermines visual curiosity | Generate level-of-detail morphology from traits; do not scale a 1-pixel asset |
| High simulation speed changes outcomes | Breaks scientific comparability | Fixed timesteps, deterministic RNG, no elapsed-time physics, exact replay tests |
| QSPI wear or interrupted saves | Long-lived worlds lost | Rotating validated checkpoints, bounded cadence, backup/export |
| ESP32 custom flash removes Wi-Fi compatibility | Potential development stall | Keep stock firmware initially; document passthrough/restore; gate migration |
| Display power/refresh instability | Visual artifacts or reset under bright scenes | Verify adequate 5 V panel power; profile refresh and brightness |
| Scope creep into alien features | Delays first proof of evolution | P0–P2 are mandatory MVP; advanced features enter only after exit gates |

## 14. Definition of done

### MVP (P0–P2)

Petri is an MVP when a MatrixPortal M4 can autonomously display a 64×32 ecosystem, with plants growing from environmental resources and creatures consuming, reproducing, mutating, inheriting traits, and dying; a native headless harness demonstrates selective adaptation under different conditions; and the board runs stably without a desktop app.

### Core biological experience (through P4)

Multiple trophic niches, heritable behaviors, vulnerable dormant stages, lineage tracking, permanent extinction, and ecological events produce inspectable unscripted outcomes. The simulation withstands longevity tests and offers defensible evidence for adaptation.

### Full intended product (through P6; P7 conditional)

The LED panel is an attractive autonomous window into a larger alien world, supporting smooth camera movement, multiple zoom levels, procedural morphology, fast-forward, power-safe persistence, and a useful optional desktop observatory. Dual-processor operation is an optimization path, **not** a prerequisite for the experience.

## 15. Immediate implementation handoff

For the first Codex implementation pass:

1. Inspect the actual repository, device accessibility, and existing firmware before changing anything. Do not assume the code structure or connected USB device.
2. Implement **P0 and P1 only** unless explicitly directed to proceed further. If the board is unavailable, produce the native simulation with a documented firmware adapter boundary.
3. Keep core rules independent of Arduino, Protomatter, and Wi-Fi. Design a stable RNG/tick interface and serialized world state from the start.
4. Begin with resource-aware plants and scalar-genome creatures. Do not implement full neural evolution, egg predators, desktop UI, or ESP32 reflashing in the first pass.
5. Provide test cases for heredity, mutation bounds, energy costs, deterministic replay, and selection under different food conditions.
6. End with a native benchmark and brief measured performance report, plus explicit next tasks for P2.

**North-star test:** Let two initially similar populations evolve under different resource pressures. If the differences in their inherited traits and reproductive success can be observed and reproduced—not merely drawn as different colors—Petri's foundation is working.

## 16. Hardware and reference documentation

Primary hardware references (verify against the physical board and current toolchain during P0):

- [Adafruit MatrixPortal M4 overview](https://learn.adafruit.com/adafruit-matrixportal-m4/overview)
- [MatrixPortal M4 pinouts and memory](https://learn.adafruit.com/adafruit-matrixportal-m4/pinouts)
- [Adafruit Protomatter for the MatrixPortal M4](https://learn.adafruit.com/adafruit-matrixportal-m4/protomatter-arduino-library)
- [RGB matrix display memory/performance considerations](https://learn.adafruit.com/rgb-led-matrices-matrix-panels-with-circuitpython/advanced-multiple-panels)
- [MatrixPortal M4 ESP32 firmware update](https://learn.adafruit.com/adafruit-matrixportal-m4/updating-esp32-firmware)
- [Adafruit AirLift passthrough firmware procedure](https://learn.adafruit.com/upgrading-esp32-firmware/upgrade-all-in-one-esp32-airlift-firmware)
- [Espressif ESP32 chip series comparison](https://docs.espressif.com/projects/esp-idf/en/v5.0/esp32/hw-reference/chip-series-comparison.html)

---

**Guiding constraint:** Preserve biological integrity and long-run persistence before increasing world size, visual detail, or processor count. A small ecosystem that truly evolves is more valuable than a large ecosystem that only looks alive.
