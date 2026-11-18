# Petri

Petri is an artificial-life simulation intended for a MatrixPortal M4 and a
64×32 RGB matrix. This repository starts with a headless Linux prototype.
See the [design spec and roadmap](docs/SPEC_AND_ROADMAP.md) for the broader project.

## Build and run

Use CMake 3.16 or later and a C++17 compiler. The native build has no third-party
dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./build/petri --seed 42 --ticks 1000
```

The CLI reports births and deaths, current and peak end-of-tick population,
blocked birth attempts, standing plant biomass, surviving founder lineages,
mean traits among living creatures, and a fingerprint of the final state. Use
the same seed, initial nutrients, and tick count to replay a run. The
fingerprint serves as a replay diagnostic.

```sh
./build/petri --seed 42 --ticks 1000 --nutrients 0
./build/petri --seed 42 --ticks 100000 --benchmark
```

## Save and resume a native run

```sh
./build/petri --seed 42 --ticks 901 --save build/world.petri
./build/petri --load build/world.petri --ticks 499 --save build/world.petri
./build/petri --seed 42 --ticks 1400 --save build/continuous.petri
cmp build/world.petri build/continuous.petri
```

On resume, `--ticks` counts additional ticks and peak population covers the
current invocation. Checkpoints store little-endian
fields. The magic and version precede the RNG state, ecology settings, counters,
row-major cells, and 320 creature slots in update order. Version-1 files
resume the plant-only ecology and start collecting diet counters at load.
Version-2 files resume scavenging with predation disabled. Version-3 files
resume the original predation rules. Version-4 files resume the food web
with connected biomes disabled. Version-5 files retain the original connected
terrain. Version-6 files retain irregular terrain and resume with asexual
reproduction. Version 7 stores two-parent reproduction and resumes with
mobile newborns. Version 8 stores nest residency, carried food, care traits,
and the counters for offspring and parent behavior. Version 9 adds hunting
and evasion traits, active chase targets, and individual kill counts. Version
10 stores the hunt threshold and per-cell chase cost. Version 11 stores kin
avoidance and each creature's inherited reluctance. Version 12 also stores
separate reluctance strengths and the experimental prey protection flag.
Version 13 stores faction markers and the cross-faction hunting setting.
Version 14 stores staggered environment updates, rooted fruiting shrubs,
ripe fruit, carried fruit, and the fruit consumption counter.
Version 15 also stores the climate seed and regional rainfall state.
Version 16 stores the storm-front flag, up to four storms, and the soil water
at each climate node. Version-15 files resume with steady rainfall.
Version 17 holds 320 creature slots instead of 160 and stores IDs and lifetime
counters at their 32-bit widths. Versions 1–16 load into the first 160 slots,
and the rest of the pool starts empty. Version 18 adds the morphology settings
and four morphology genes per slot (94,457 bytes at 128×64, inside a
96 KiB QSPI slot). Version 17 files resume with morphology off.
Checkpoint size depends on world dimensions.
The file ends with a 64-bit FNV-1a
checksum. Load validates the checksum and state, rebuilds occupancy and
derived counters, and preserves the live world on rejection. The CLI stages
a replacement beside the destination and renames it after a successful write. Onboard flash rotation and power-loss recovery belong to the
persistence milestone.

## Starter model

- Four wet patches support plant growth and hold the starting creatures. Dry
  ground separates the patches. Daylight drives growth, and decomposition
  returns detritus to the nutrient pool. Each patch contains 61 cells.
- Creatures graze, move toward richer adjacent cells, and pay energy to stay
  alive and move. A foraging creature can place offspring in the cell it just
  vacated. Offspring inherit digestion and metabolic thrift with bounded
  mutations. Thrift reduces upkeep and limits bite size.
- Each living creature carries its own ID, parent ID, founder ID, and generation.
  The fixed pool holds 320 creatures. Full-pool birth attempts increment a counter
  and leave parental energy intact.
- `core/` owns simulation state and advances it one tick at a time.
  `apps/` owns argument parsing, wall-clock timing, and console output. A future
  SAMD51 adapter can schedule ticks and render the read-only world view.

## Memory layout

`World` occupies 39,104 bytes in the 64×32 build and 86,104 bytes in the
128×64 build, including the storm state. With 160 slots it took 26,552 and 72,664 bytes, down from
41,392 and 109,008.

- Each cell uses 6 bytes. Detritus is 16 bits. The highest measured value was
  8,031, in a 64×32 run at 255 nutrients with every ecology feature enabled.
  A million-tick run at 24 nutrients held a single-cell peak of about 5,700.
  A deposit that would exceed 65,535 is clipped. The CLI then reports
  `detritus_clipped`. That counter is not stored in checkpoints.
- Cell occupancy uses nine bits per cell: a byte plus one bit packed eight
  cells to a byte. Fruiting shrubs use half a byte.
- Each creature slot uses 68 bytes, 4 of them for the morphology genes. IDs
  and lifetime counters are 32 bits.
  If all 2³²−1 IDs are used, further births count as capacity blocks.
- Fingerprints and `PTR4` observations keep their earlier 64-bit fields.
  Checkpoints did until version 17. Loading rejects IDs above 2³²−1 and
  detritus above 65,535.

### Environment early-outs

`World::recycle` returns at once for a cell with no carcass and no
detritus, and `World::grow_plants` returns for a cell with no soil
nutrients or full plant cover. Both skip only work that cannot change
anything. The 68 matched runs described below, each with `--save`,
produced byte-identical CLI output and checkpoints before and after.
Native ticks/s, best of three 20,000-tick runs at seed 14:

| World | Before | After | Gain |
| --- | ---: | ---: | ---: |
| 128×64 observatory | 25,907 | 27,887 | 8% |
| 128×64 default | 97,556 | 187,726 | 92% |
| 128×64 food web | 52,724 | 68,216 | 29% |
| 64×32 default | 205,620 | 248,865 | 21% |
| 64×32 food web | 55,171 | 55,686 | 1% |

The observatory gains least because its creatures, not its cells, take
most of each step.

### 320 creature slots

The pool grew from 160 to 320 slots on November 18, 2026. Three of the four
regional-weather runs in [the experiments](docs/experiments/README.md) had
reached the 160 limit. Below the old limit
the simulation is unchanged: across 68 matched runs (both world sizes, eight
or ten flag sets, seeds 1, 4, 14, and 42, 20,000 ticks), every CLI field
except the fingerprint matched the 160-slot build, and none of those runs
reached 160 creatures. The fingerprint changes because it hashes every slot.
All 68 version-15 checkpoints from those runs loaded and resumed for 3,000
ticks with the same results as the 160-slot build. `tests/` builds a
version-15 checkpoint from a version-17 one and checks that it resumes
exactly.

| 128×64 build | 160 slots | 320 slots |
| --- | ---: | ---: |
| `World` | 72,664 | 84,568 |
| Render state, and the encoder that holds one | 25,872 | 28,432 |
| Event log | 2,584 | 3,608 |
| `Panel` | 7,568 | 10,640 |
| `PTR4` observation buffer | 12,805 | 21,445 |
| Checkpoint | 89,738 | 92,938 |

The figures are from before the morphology genes, which add 4 bytes per
slot: the 128×64 `World` is now 85,856 bytes and its checkpoint 94,220. With
the storm state merged in they are 86,104 and 94,457.
The render state grew because `Look` is now 32 bits. Native speed did not
change measurably: 26,616 ticks/s for the seed 14 observatory world, against
27,887 before the change and 25,907 before the environment early-outs.

Across 180 matched runs covering both world sizes and every ecology flag,
final fingerprints, CLI output, and saved checkpoints matched the previous
layout byte for byte. Native speed fell about 12% for the seed 14 wide world
with fruiting shrubs and regional weather (30,400 to 26,800 ticks/s) and about
1% for the 64×32 food web. The observatory firmware (`--observatory`) now
uses 104,216 bytes of static RAM: 448 initialized and 103,768 zero-filled.
That is 36,344 bytes less than the previous build. On the board, births,
deaths, and population at ticks 265 and 275 matched the native build.
Runtime RAM headroom and tick rates above 1× have not been measured yet.

## Two-parent reproduction experiment

Add `--sexual` to a `--food-web` run. Adjacent, well-fed adults can reproduce
when both have enough energy and body nutrients. Each contributes 25 energy
and 4 units of body material. The offspring inherits one parent's allele for
each trait, with the usual bounded mutation. Two heritable traits affect how
strongly an adult approaches a potential mate and the energy it needs to
be receptive. Young mature after 24 ticks, and adults recover for 42 ticks
after a birth. The headless model tracks both parents. The CLI's existing
lineage count follows the first parent's founder ID.

```sh
./build-wide/petri --seed 14 --nutrients 24 --food-web --predation \
  --connected-biomes --irregular-terrain --sexual --ticks 20000
```

At 24 nutrients per cell, all eight seeds (1, 2, 3, 4, 14, 42, 101, and 2026)
remained populated through tick 20,000. Two-parent runs logged 4,174–4,981 births
and peaked at 124 out of 160 slots, with zero blocked births. Matched asexual
runs logged 12,616–13,460 births and peaked at 119. Both treatments used the
same starting terrain, founder placement, and food web.
An additional four seeds at each of 12 and 36 nutrients per cell also
completed 20,000 ticks with zero blocked births. At 36 nutrients seed 101
peaked at 157 out of 160 slots.

Unscavenged carcass biomass decays into detritus, which becomes soil nutrients.
Plants use those nutrients where growing conditions permit. With the food web
enabled, dissolved nutrients also move between cells. In a test with no
initial soil nutrients, founder carcasses supported 256 units of new plant
biomass. The total amount of material stayed constant.

## Nest care experiment

`--nests` adds a 12-tick vulnerable stage to the two-parent mode. Young stay
in the birth cell, eat small amounts of food within reach, and can starve
or be hunted. `--guarding` lets adults spend energy guarding from an adjacent
cell, making attacks against the young less likely to succeed. `--provisioning`
lets adults reserve part of a meal as a portable food supply and deliver it
from next to the nest. Each action competes with the adult's need to forage.
Inherited care traits affect how strongly adults approach the nest, how much
food they carry, and how much energy they reserve for themselves. Prior mates
are tracked for measurement. Each adult can choose a new mate as before.

```sh
for seed in 1 2 3 4 14 42 101 2026; do
  for treatment in none guarding provisioning both; do
    case "$treatment" in
      none) set -- ;;
      guarding) set -- --guarding ;;
      provisioning) set -- --provisioning ;;
      both) set -- --guarding --provisioning ;;
    esac
    ./build-wide/petri --seed "$seed" --nutrients 24 --food-web --predation \
      --connected-biomes --irregular-terrain --sexual --nests --ticks 20000 "$@"
  done
done
```

Each row averages the eight seed-level percentages. The fledging rate uses
young whose nest phase resolved by tick 20,000. Both-near measures the fraction
of ticks with both living parents within two cells of an occupied nest. After
the nest phase, the proximity measure uses the first twelve ticks following
the scheduled end of the nest phase and counts living parents within two
cells of one another. Repeat mating is measured for adults whose previous
mate was still alive at the time of their next successful mating.

| Care available | Fledged | Both near nest | Near each other after nest | Same mate again |
| --- | ---: | ---: | ---: | ---: |
| None | 50.2% | 9.1% | 5.5% | 29.5% |
| Guarding | 54.0% | 85.1% | 9.5% | 27.5% |
| Provisioning | 93.9% | 48.4% | 8.5% | 24.5% |
| Both | 78.9% | 82.2% | 8.8% | 25.4% |

Provisioning improved fledging in every seed. Provisioning alone delivered
13.4 food units per birth, compared with 6.9 when guarding was also available.
Starvation accounted for 20,446 nest deaths in the control and predators
accounted for 63. With guarding alone, predators killed 15 young. The limited
predation in these runs leaves the benefit under heavier pressure open.
Parents gathered around nests when care was available. Repeat mating was
less common with care in this setup. All 32 runs completed with zero blocked
births and a highest observed population of 119 out of 160 slots.

## Adaptive hunting and evasion

Use `--adaptive-hunts` with `--food-web --predation`. `--hunt-every` logs
hunt decisions, outcomes, and energy costs and returns in both hunting modes.
`--lineage-every` reports feeding strategies and births in the same windows.

```sh
./build-wide/petri --seed 8 --nutrients 24 --food-web --predation \
  --connected-biomes --irregular-terrain --sexual --nests --guarding \
  --provisioning --adaptive-hunts --ticks 100000 --hunt-every 10000 --lineage-every 10000
```

See the [matched hunting experiment](docs/experiments/README.md#adaptive-hunting-and-evasion)
for the decision rules, passive-prey control, and results.
The [hunt energy experiment](docs/experiments/README.md#hunt-energy-and-tuning)
compares `--hunt-threshold` and `--chase-cost` one at a time.
Use `--kin-avoidance` to enable inherited hunting reluctance. Set
`--kin-strength` and `--resemblance-strength` independently from 0 to 20.
Both default to 7. `--protect-similar-prey` excludes genetically similar prey
from hunts. [Matched reluctance sweep](docs/experiments/README.md#kin-and-resemblance-sweep)

Use `--faction-markers` to give the founders four inherited markers drawn
independently of diet. `--cross-faction-hunts` enables the markers and makes
creatures with the same marker ineligible as targets in `--adaptive-hunts`.
`--lineage-every` reports faction sizes, and `--hunt-every` reports kills
within and across factions. [Matched faction experiment](docs/experiments/README.md#heritable-faction-markers)

## Fruiting shrubs and staggered growth

`--fruiting-plants` adds rooted shrubs to some initially vegetated cells in
moderate-to-wet habitat. A mature shrub converts up to four units of foliage
into ripe fruit every 64 ticks while retaining at least four units of foliage.
Its fruiting phase depends on location, so different shrubs ripen on different
ticks. Unpicked fruit returns to detritus after 32 ticks. Animals compare the
energy from fruit, leaves, and meat when choosing a meal. Fruit has less fiber
cost and can also be carried to a nest. Fruit biomass and consumption are
reported separately. Plant-food intake includes fruit.

```sh
./build-wide/petri --seed 14 --nutrients 24 --food-web --predation \
  --connected-biomes --irregular-terrain --fruiting-plants --ticks 20000
```

Fruiting plants enable staggered environment updates. `--staggered-growth`
also enables those updates on its own. Each cell keeps the earlier growth
and recycling cadence on a location-dependent clock. Moisture and nutrient
transport are divided across ticks. The observatory demo and current UF2
enable shrubs and the staggered schedule.

Native seed 14 profiling measured 4,096 ticks after the 256-tick warmup, with
five timing repetitions per mode. The largest tick grew foliage in 3,523
cells under the earlier schedule and 184 with shrubs and staggered growth.
The latter showed growth on all 4,096 sampled ticks. Median 99th-percentile
tick durations were 90.0 µs and 37.6 µs, respectively, on the development
machine. Board telemetry reports `max_step_us` and `max_frame_gap_us` to
check pauses on the physical display.

Seeds 4, 14, 42, and 101 each completed 20,000 ticks in the wide world with
shrubs, zero blocked births, and peak end-of-tick populations of 107–140.
They consumed 5,608–7,117 units of fruit per run.

## Regional weather and habitat succession

`--dynamic-environment` adds changing regional moisture to connected terrain.
After 1,024 ticks of establishment, a coarse weather field begins shifting
the local water supply. Regional targets change every 4,096 ticks and blend
gradually between neighboring areas. A 32,768-tick wet/dry cycle adds a
smaller seasonal variation. Spring-fed wetland cores remain available
through droughts.

Vegetation on drying ground returns to detritus, and new wet ground can
support regrowth. Uneaten fruit occasionally seeds an adjacent vegetated
patch. Shrubs die back when dry ground loses its foliage. Wetland regrowth
uses four growth steps per 64 ticks to balance the expanded feeding areas.
Hungry creatures can search within four cells for a worthwhile meal when
their current and adjacent cells offer too little food.

```sh
./build-wide/petri --seed 14 --nutrients 24 --food-web --predation \
  --connected-biomes --irregular-terrain --fruiting-plants --dynamic-environment \
  --ticks 524288
```

The native observatory and current firmware enable regional weather.

`--storms` (with `--dynamic-environment`) delivers wet weather in storm
fronts instead of a steady bias. Storms cross the map on the epoch's
prevailing wind, rain only where the regional climate is wet, and recharge a
soil-water store that drains over about 1,000 ticks. Between storms the map is
dry. The observatory world and current firmware enable storms. See the
[storm results](docs/GRAPHICS_AND_EVOLUTION.md#progress).
The [long environment comparison](docs/experiments/README.md#regional-weather-and-succession)
records changes in habitat, shrubs, population, and the conserved material
budget on matched seeds.

## Morphology genes

`--morphology` (with `--food-web`) gives each creature four more heritable
genes: body size, pigment, pattern, and limbs. Each one changes what the
creature can do and has a cost:

- **Body size** raises the bite and the energy store and helps both to
  catch prey and to resist capture. It costs upkeep, and larger parents
  must store more before breeding, because young are provisioned in
  proportion to body size.
- **Pigment** is causal camouflage. Prey whose pigment matches the ground
  under it is harder to catch and is seen only at closer range. A strong
  mismatch makes it easier to catch. No pigment matches every habitat.
- **Pattern** hides prey in dense vegetation and shows it on bare ground,
  where a plain coat hides better.
- **Limbs** set foraging speed: long limbs sometimes take a second paid
  step, and short ones sometimes miss a step. They also help in chases and
  escapes. They cost upkeep.

`--neutral-morphology` is the matched control: the genes are inherited and
mutate but do nothing, and the ecology is then identical to a run without
`--morphology`. In matched 200,000-tick runs, camouflage and plain coats
were selected only where hunting was common. Body size rose under
predation in the rich 64×32 food web and fell, with a much narrower spread
than the control's, in the lean observatory world. Limbs lengthened in the
64×32 world. The [morphology experiment](docs/experiments/README.md#morphology-genes)
has the rules, numbers, and commands.

```sh
./build/petri --seed 5 --nutrients 80 --food-web --predation --adaptive-hunts --morphology --ticks 200000
```

The genes appear in each creature's `Look` (bits 9–20, documented in
`render/render_state.hpp`) so the renderer can draw them. The panel does
not draw them yet, and the observatory configuration leaves morphology off.

## Earlier plant-only experiments

Use `--carcass-decay 0` to replay this baseline.

Six matched seeds (1, 2, 3, 42, 101, 2026) started with 32 creatures, each
with digestion and thrift of 8 on a 1–15 scale. Each pair shared its habitat
moisture and founder placement. The two conditions used nutrient budgets of 7
and 48 per cell. The budget includes standing plants, detritus, and free
nutrients. The table averages each endpoint across the six seeds.

| Tick | Nutrient budget | Plant biomass | Births to date | Mean digestion | Mean thrift |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 2,000 | 7 | 939 | 185 | 10.05 | 8.43 |
| 2,000 | 48 | 2,515 | 686 | 11.89 | 9.31 |
| 20,000 | 7 | 280 | 6,683 | 14.98 | 12.26 |
| 20,000 | 48 | 1,694 | 8,071 | 14.84 | 12.33 |

At tick 2,000, the richer treatment had more births and higher mean digestion
in every seed pair. Digestion directly increases the energy gained from grazing.
With more births in the richer treatment, the comparison cannot isolate a
difference in selection strength from a difference in the pace of reproduction.
By tick 20,000, digestion was near its ceiling in both treatments, and paired
thrift differences went in both directions. The experiment found an early
difference in the rate of trait change, with no consistent late separation.

All 12 comparison runs had zero blocked births and peaked at 148 creatures or
fewer. A separate 30,000-tick sweep of seeds 4–15 at nutrient budgets 7, 48,
and 255 logged zero blocked births across 36 runs and peaked at 151.
The default seed 42 benchmark ran for 100,000 ticks and peaked at 115 with
zero blocked births. On this Linux host it ran at about 260,000 ticks/s, with
an 18,744-byte `World` object.

```sh
for ticks in 2000 20000; do
  for seed in 1 2 3 42 101 2026; do
    for nutrients in 7 48; do
      ./build/petri --seed "$seed" --ticks "$ticks" --nutrients "$nutrients" --carcass-decay 0
    done
  done
done
```

### Birth-matched and neutral controls

The follow-up used 12 paired seeds (1, 2, 3, 42, 101, 2026, 4–9) with the
same two nutrient budgets. In the neutral control, mutations and inheritance
continue while both genes use the founders' values of 8 for physiology.

| Nutrients per cell | Mean tick at 250 births | Mean digestion at 250 births | Mean digestion at tick 2,000 | Neutral digestion at tick 2,000 |
| ---: | ---: | ---: | ---: | ---: |
| 7 | 2,429 | 10.61 | 10.11 | 7.84 |
| 48 | 809 | 9.88 | 11.68 | 8.07 |

At the first completed tick with at least 250 births (250–252 actual births),
the lower-food world had higher mean digestion in all 12 seed pairs. The rich
world reached that milestone sooner in every pair. Mean thrift at the birth
milestone was close (8.64 versus 8.51). The neutral endpoint for 48 nutrients
averages 11 surviving worlds because one went extinct before tick 2,000.
Birth matching controls total births approximately. It permits different
elapsed time, lineage turnover, and survival exposure. These runs support
selection on digestion under scarcity and a faster early response in the
richer environment in elapsed ticks.

```sh
for seed in 1 2 3 42 101 2026 4 5 6 7 8 9; do
  for nutrients in 7 48; do
    ./build/petri --seed "$seed" --nutrients "$nutrients" --carcass-decay 0 --ticks 20000 --until-births 250
    ./build/petri --seed "$seed" --nutrients "$nutrients" --carcass-decay 0 --ticks 2000
    ./build/petri --seed "$seed" --nutrients "$nutrients" --carcass-decay 0 --ticks 2000 --neutral
  done
done
```

## Animal biomass experiments

The headless CLI now enables carrion by default. In this ecology, founders
start with eight units of body material each. Birth takes eight units from
the parent's local detritus, and death deposits that material as edible
remains. The total material in plants, free nutrients, detritus, carcasses,
and living bodies is conserved. Feeding returns consumed material to
detritus and yields energy. Carrion decays into detritus every two ticks.
The default decay removes one unit per cell per update.

A single inherited digestion allocation splits 16 points between plants
and meat. Forage movement favors local reserves weighted by digestion.
Creatures pick the available bite that yields more energy, with plant food
winning ties. Meat has twice the gross energy density per unit in this model.
Neutral runs inherit and mutate the allocation while using 8/8 for feeding.
`--initial-carrion` seeds a share of the same starting nutrient budget in wet
cells as carrion. It does not replenish that supply. There is no predation
in this experiment.

The table uses six matched seeds (1, 2, 3, 42, 101, 2026). Animal share is the
mean per-run fraction of all biomass eaten. Individual diets use lifetime
consumption among living creatures with at least 32 units eaten. Mixed
diets get 20% to under 80% of biomass from animals, and animal-dominant diets
get at least 80%.

| Condition | Ticks | Mean animal share | Mixed diets | Animal-dominant diets |
| --- | ---: | ---: | ---: | ---: |
| Natural carcasses, 7 nutrients | 20,000 | 0.04% | 0 / 190 | 0 / 190 |
| Natural carcasses, 48 nutrients | 20,000 | 0.56% | 0 / 139 | 0 / 139 |
| Faster decay, 48 nutrients | 20,000 | 0.27% | 0 / 122 | 0 / 122 |
| Natural carcasses, 48 nutrients | 100 | 0.42% | 0 / 294 | 0 / 294 |
| 75% initial carrion, 48 nutrients | 100 | 29.42% | 46 / 94 | 0 / 94 |
| 75% initial carrion, neutral control | 100 | 29.09% | 45 / 102 | 0 / 102 |
| 75% initial carrion, 48 nutrients | 1,000 | 4.19% | 0 / 327 | 0 / 327 |

No births were blocked by the pool across these runs. The largest peak was
143 of 160. In the long natural-carcass runs, mean meat digestion ended at
5.91 at 7 nutrients and 1.03 at 48 nutrients, with 8 in the founding
population. The seeded carrion pulse produced mixed diets in every seed,
including the neutral control. The diets were transient. These results show
opportunistic scavenging and a response to temporary food availability, with
no evidence of a stable meat-specialist niche.

```sh
for seed in 1 2 3 42 101 2026; do
  ./build/petri --seed "$seed" --nutrients 7 --ticks 20000
  ./build/petri --seed "$seed" --nutrients 48 --ticks 20000
  ./build/petri --seed "$seed" --nutrients 48 --ticks 20000 --carcass-decay 8
  ./build/petri --seed "$seed" --nutrients 48 --ticks 100
  ./build/petri --seed "$seed" --nutrients 48 --ticks 100 --initial-carrion 75
  ./build/petri --seed "$seed" --nutrients 48 --ticks 100 --initial-carrion 75 --neutral
  ./build/petri --seed "$seed" --nutrients 48 --ticks 1000 --initial-carrion 75
done
```

## Predation experiment

`--predation` enables hunting in the headless simulation. An animal can attack
the lowest-energy adjacent creature when the expected energy from a successful
bite, minus the attack cost, exceeds its forage alternative. Each attempt costs
two energy on top of upkeep. Capture chance falls as prey energy rises, from
at most 55% to at least 15%. Successful hunters enter the prey cell and take
one bite from the carcass. A failed attack uses the feeding opportunity for
that tick. Remaining carcass material follows the same recycling rules as
natural deaths. The CLI reports attempts and successful kills separately.

The experiment used six seeds (1, 2, 3, 42, 101, 2026) at each of two nutrient
budgets. Each pair shared the same starting state, with and without predation,
with a carcass decay rate of 1 and zero seeded carrion. We inspected ticks 500,
2,000, 10,000, 20,000, and 100,000. A live meat specialist was defined as a
creature with at least 32 units of lifetime consumption, at least 80% from
animals, and inherited meat digestion of at least 10 out of 15.

| Nutrients per cell | Predation | Mean animal food share at tick 100,000 | Total kills | Kills between ticks 20,000 and 100,000 |
| ---: | :--- | ---: | ---: | ---: |
| 7 | Off | 0.010% | 0 | 0 |
| 7 | On | 0.017% | 1,666 | 118 |
| 48 | Off | 0.274% | 0 | 0 |
| 48 | On | 0.415% | 9,353 | 48 |

The treatment produced one mixed-diet survivor at tick 500 in the richer
condition. No sampled population contained an animal-dominant feeder or a
meat specialist. By tick 100,000 all worlds still had living creatures, with
zero blocked births and a peak of 98 out of 160. Seven of the 12 predation
runs recorded zero kills in the final 80,000 ticks. This adjacent-hunt model
supports opportunistic predation. The experiment did not find a stable
meat-specialist niche.

```sh
for seed in 1 2 3 42 101 2026; do
  for nutrients in 7 48; do
    for ticks in 500 2000 10000 20000 100000; do
      ./build/petri --seed "$seed" --nutrients "$nutrients" --ticks "$ticks"
      ./build/petri --seed "$seed" --nutrients "$nutrients" --ticks "$ticks" --predation
    done
  done
done
```

### Tracking reproduction over time

Use `--lineage-every 10000` to follow an uninterrupted run. Each sample
reports the living plant feeders and specialist counts by founder ID.
It also reports births in the preceding interval whose parent qualified
as a meat specialist and whose offspring inherited at least 10/15 meat
digestion. The birth observer records births as they happen, including
offspring that are killed later in the same tick. For a resumed run, both
the interval and the birth observer start at the loaded tick. See the
[meat-specialist experiments](docs/experiments/README.md) for matched-run
results and the nutrient transport and pursuit prototype.

```sh
./build/petri --seed 42 --nutrients 48 --ticks 100000 --lineage-every 10000
./build/petri --seed 42 --nutrients 48 --ticks 100000 --lineage-every 10000 --predation
```

### Food-web benchmark

The optional `--food-web` ecology allows feeding to accumulate nutrients
inside each creature for reproduction and includes nutrient transport,
plant cover, and longer-range hunting. With predation enabled, this
produced reproducing meat-specialist lineages alongside plant feeders in
multiple matched seeds. See the [benchmark results and replay commands](docs/experiments/README.md).

```sh
./build/petri --food-web --predation --seed 4 --nutrients 80 --ticks 200000 --lineage-every 10000
python3 docs/experiments/verify_food_web.py build/petri
```

## MatrixPortal handoff

The connected board identified itself as a MatrixPortal M4 running
CircuitPython 10.0.3. Its CIRCUITPY volume was copied and verified. The
ignored `backups/` directory holds the private volume copy, matching
recovery UF2, bootloader readback, and restoration steps. The display script uses the USB CDC
data port for input, so the original host-side sender is also needed to test
the tracker end to end.

1. MATRIXBOOT enumerated with UF2 Bootloader 3.16.0 and board ID
   `SAMD51J19A-MatrixPortal-v0`. The public recovery image matched every
   payload block it covers in the board's `CURRENT.UF2` readback. The
   setup workspace sees the bootloader mount as read-only. The first UF2
   was copied manually.
   [Adafruit bootloader guide](https://learn.adafruit.com/adafruit-matrixportal-m4/install-circuitpython)
2. The panel and button test booted on the board. The RGB bars and both
   buttons worked.
   [Adafruit Arduino setup](https://learn.adafruit.com/adafruit-matrixportal-m4/arduino-ide-setup)
   [Adafruit Protomatter guide](https://learn.adafruit.com/adafruit-matrixportal-m4/protomatter-arduino-library)
3. Compile `core/world.cpp` and `render/life.cpp` with C++17 support. Give the
   board adapter a statically allocated `World` and `Frame`. The adapter
   schedules whole calls to `World::step()` and draws independently with
   `render_life(world, frame)`. The 4,096-byte frame is row-major RGB565,
   ready to pass to Protomatter's `drawPixel` and `show`.
   [Adafruit RGB565 drawing API](https://learn.adafruit.com/adafruit-matrixportal-m4/protomatter-arduino-library)
4. Measure achieved rates and RAM headroom on the board, including
   Protomatter’s runtime allocations, then check stability in a multi-hour soak.

### First Petri firmware

Double-tap RESET and copy `build/petri-matrixportal-m4.uf2` to MATRIXBOOT. The
world starts autonomously from seed 42 on each boot. Expect green vegetation
and moving amber creatures. The matrix refreshes independently from the
simulation.

| Input | Action |
| --- | --- |
| Tap UP | Switch between Life and Habitat views |
| Tap DOWN | Cycle 1×, 2×, 4×, 8×, then back to 1× |
| Hold DOWN for about 0.6 seconds | Pause or resume |

The requested rates are 10, 20, 40, and 80 ticks/s, with up to 20 new frames/s.
Optional USB CDC telemetry at 115200 reports the achieved rates, requested tick
rate, population, births, deaths, and blocked births. The build uses about
33 KiB of static RAM, excluding runtime display allocations and stack.

The sketch is in [firmware/petri_matrix](firmware/petri_matrix/petri_matrix.ino).
The build script stages the shared core and renderer with Arduino CLI and
packages the result. This build used Adafruit SAMD 1.7.17, Protomatter 1.7.1,
and Adafruit TinyUSB 3.7.7.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py
```

### Food-web demo UF2

Copy `build/petri-food-web-seed4.uf2` to MATRIXBOOT. It starts at tick
160,000 of seed 4 with 112 creatures, including two meat specialists.
Each restart reloads that embedded checkpoint. Amber pixels mark living
creatures. Magenta marks exposed carcasses. Successful hunts flash white
and then pink in Life view for about 0.3 seconds. Habitat view keeps the
terrain and carcasses visible.

Tap UP to switch views, tap DOWN to cycle speed, and hold DOWN to pause
or resume. Optional USB CDC at 115200 reports `tps`, `fps`,
cumulative `kills`, and `ram_gap`. The RAM value is the sampled space in
bytes between the heap end and current stack pointer, after display
allocation. It measures the current gap and does not measure the worst
stack depth. The boot checkpoint is stored in flash.

On the MatrixPortal M4, the seed 4 demo held 10 ticks/s at 1× for 51
seconds and 80 ticks/s at 8× for 30 seconds. Both settings delivered 20
new rendered frames/s. The sampled heap-to-stack gap was 136,032 bytes
(132.8 KiB) throughout. Peak sampled population was 140, with no blocked
births or telemetry gaps. At 8×, 524 hunts landed in about 30 seconds.

Build from the repository root with the native CLI, Arduino toolchain,
and UF2 converter installed.

```sh
./build/petri --food-web --predation --seed 4 --nutrients 80 --ticks 160000 --save build/petri-seed4-demo.petri
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-food-web-seed4.uf2 build/petri-seed4-demo.petri
```

### Hunt view UF2

Copy `build/petri-hunt-view-seed4.uf2` to MATRIXBOOT. This variant uses
the same tick-160,000 checkpoint. Tap UP twice from the boot view to
cycle from Life through Habitat to Hunt. Life shows the ecosystem without
event flashes. Habitat shows terrain and carcasses. Hunt dims the terrain
and holds one scene for 1.2 seconds, with a cyan marker showing the hunter's
approach and a pink box framing the prey. A short quiet interval follows
each event. The simulation keeps running during the hold, and other kills
during the hold are counted by telemetry.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-hunt-view-seed4.uf2 build/petri-seed4-demo.petri
```

### Close-up and hunt replay UF2

Copy `build/petri-close-up-seed4.uf2` to MATRIXBOOT. The seed 4 demo
opens in Close view at 1×. The camera follows the cyan creature on an
8×3-cell part of the world. The bar tracks its energy, and `GRAZE` or
`EAT MEAT` appears when the tracked creature takes a bite. Nearby
creatures are amber. Plants and carcasses have separate marks.

Tap UP to cycle Close → Hunt → Life → Habitat. Hunt plays a 2.35-second
stylized replay of one actual successful capture. The cyan hunter
approaches the amber prey, takes a bite, and eats the pink remains.
The simulation runs at the selected speed throughout the replay.
Tap DOWN to change speed and hold DOWN to pause or resume the simulation.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-close-up-seed4.uf2 build/petri-seed4-demo.petri
```

### Slow-paced Close view UF2

Copy `build/petri-slow-pace-seed4.uf2` to MATRIXBOOT for the seed 4 demo
at the slower viewing pace. At 1× the world advances once per second.
DOWN cycles through 2, 4, and 8 ticks/s, then returns to 1 tick/s.
The display still targets 20 rendered frames/s, and hunt replays use
the same wall-clock timing at every speed. Close view holds meal labels
for about 0.9 seconds so individual bites are easier to see.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-slow-pace-seed4.uf2 build/petri-seed4-demo.petri
```

### Text-free panel UF2

Copy `build/petri-clean-panel-seed4.uf2` to MATRIXBOOT. The panel
uses the full height for the live 8×4-cell close-up and larger hunt
sprites. A green or pink mouth pixel still marks a plant or meat bite
on the tracked creature. Numerical diagnostics remain available over USB
for monitoring and future companion-app presentation.

The demo starts at 1 tick/s. UP cycles Close → Hunt → Life → Habitat.
Tap DOWN to change speed and hold DOWN to pause or resume. The board LED
blinks on initialization failure.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-clean-panel-seed4.uf2 build/petri-seed4-demo.petri
```

### 128×64 world UF2

Copy `build/petri-wide-seed4.uf2` to MATRIXBOOT. The panel is still 64×32.
Life view fits the whole world by mapping each 2×2 block to a pixel.
Close view follows a creature across the world and interpolates its
display position between simulation updates. The camera also pans
gradually near the display edges. UP still cycles Close → Hunt → Life →
Habitat, and 1× still advances one simulation tick per second.

This build starts a fresh seed 4 world at 24 nutrients per cell and
advances 256 ticks before showing it. It has twelve staggered wetlands.
Plants regrow more slowly, ordinary moves favor less crowded cells, and
births leave space around the new creature. The previous late-run demo
is a separate world.

The wide seed 4 start has 44 creatures, of which 43 occupy distinct
overview pixels and two have an orthogonal neighbor. The previous demo
started with 112 creatures and 86 had an orthogonal neighbor. These are
boot-state measurements from two different demos. After 20,000 ticks,
the four checked wide seeds had 51–56 living creatures and zero blocked
births. The build uses about 107 KiB of static RAM. Firmware frame rates
and runtime RAM headroom still need measurement on the panel.

```sh
cmake -S . -B build-wide -DCMAKE_BUILD_TYPE=Release -DPETRI_WIDE_WORLD=ON
cmake --build build-wide
ctest --test-dir build-wide --output-on-failure
./build-wide/petri --seed 4 --nutrients 24 --food-web --predation --ticks 256
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-wide-seed4.uf2 --wide
```

### Connected biomes UF2

Copy `build/petri-connected-biomes.uf2` to MATRIXBOOT. This optional
128×64 world links twelve wetlands with winding grass corridors and scrub
fringes. It starts from seed 14 after 256 ticks, with 52 living creatures.
The text-free views and pacing use the same buttons as the wide build.

- In Life and Habitat views, the ground is ochre for scrub, green for
  grassland, and teal for wetland. Magenta still marks exposed carcasses.
- In Close view, non-focal creatures are copper, gold, or mint according
  to their inherited preference for drier, intermediate, or wetter
  ground. The cyan focal creature has the same ecology as its neighbors.
- Outside a creature's preferred moisture range it spends more energy.
  Hunger and prey can draw it across the boundary. Plants hold moisture,
  while local traffic and crowding dry the ground. Biome borders move
  when the moisture crosses a threshold.

Seven seeds ran for 20,000 ticks without a blocked birth. Wetland
occupants had higher average inherited moisture preferences than
grassland occupants in all seven. Four seeds recorded completed
hunting round trips. In seed 14, animals fed outside their preferred
biome and returned 47,033 times, including two trips with a kill. A
round trip can cross a nearby boundary and return to any cell of the
preferred biome. The same seed also showed 1,520 individual movements
between distinct wetlands in a position trace. The cumulative biome
shift count can include repeated changes to the same cell.

USB telemetry adds crossing, cross-boundary hunt, return-trip, and
biome-shift counts. The UF2 and host rendering are verified. Visual
readability and throughput still need an on-panel check.

```sh
./build-wide/petri --seed 14 --nutrients 24 --food-web --predation --connected-biomes --ticks 20000
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-connected-biomes.uf2 --connected
```

### Desktop companion and Panel Mirror UF2

The Linux companion needs Python 3, GTK 4, PyGObject with Cairo support, and
PyCairo. From the repository root, start the demo with the wide native build.

```sh
cmake -S . -B build-wide -DCMAKE_BUILD_TYPE=Release -DPETRI_WIDE_WORLD=ON
cmake --build build-wide
python3 companion/observatory.py
```

The companion shows the panel's 64×32 frame. From the 8× **Sprites** view,
scroll in once more or click **+** for the 16× detail view. Drag to pan when
zoomed in, or select a creature and click **Follow** to track it on both
screens. **Fit** returns to the world view. In the detail view, inherited
digestion shapes the head, thrift changes the body outline, and faction
markers appear as coat patterns. The inspector shows the inherited diet
tendency and lifetime food intake.

At 8×, creatures use the same body colors and smaller coat marks. At 4×,
the 2×2 creature sprite leaves room for a bottom-center plant accent and a
two-pixel carcass on the bottom left. At 2×, the carcass sits left of the live creature.
At 16×, scrub shrubs, grass tufts, and wetland reeds vary with local moisture
and plant biomass. Fruiting shrubs carry orange berries at 4×, 8×, and 16×.
At 4× and above, dual-grid terrain tiles sit half a cell across and down
from the world grid. Each reads the four surrounding cells, with 16 corner
patterns reused for grass and wetland layers over scrub. Rounded corners
and sparse soil, grass, and wetland texture soften the terrain boundaries.
Small, fixed edge distortions break up long straight borders. Adjacent
tiles share their boundary noise values, and cell centers stay anchored.
The texture and edge noise stay fixed to world coordinates while panning.
Creatures, plants, carcasses, and attack scenes use the regular world grid.

At 8× and 16×, attempted strikes show a lunge, contact flash, and recoil.
The camera frames both participants. Energy depletion closes the creature's
eyes and lowers its silhouette into a carcass. A lethal strike shows the
hit before the collapse. These sequences use display time, with a roughly
two-second hold when the followed creature dies. After predation, follow
passes to the surviving attacker. The standalone camera chooses a nearby
survivor after an energy death. Target changes ease across the world, and
the companion inspector follows an automatic handoff.

For textured terrain, death and attack scenes, regional weather, and live mirroring,
copy `build/petri-noisy-terrain.uf2`
to MATRIXBOOT and run the current companion. The companion also supports
earlier mirror firmware and uses its advertised layout and zoom limits.
The companion supports both the regular actor grid and the previous
staggered-row firmware. The terrain layer renders into the existing
64×32 framebuffer without a second map or terrain buffer.
The noisy terrain build uses 95,872 bytes of sketch storage, with 448 bytes
of initialized data and 140,112 bytes of BSS. It adds 256 bytes of program
storage and no static RAM over the preceding dual-grid build.
Native render timing was about 0.049 ms per frame across 4×, 8×, and 16×;
on-board tick rates, frame rates, and runtime RAM gap still need measurement
after flashing this build.
After the board restarts, choose **Connect USB** using the board's serial port
(usually `/dev/ttyACM0`). Close other serial monitors first. If you see an
access error and your system uses device ACLs, grant your login access to the
selected port for this connection.

```sh
sudo setfacl -m "u:$(id -un):rw" /dev/ttyACM0
python3 companion/observatory.py --device /dev/ttyACM0
```

Both the demo and the Panel Mirror UF2 start seed 14 at tick 256. The wide
world has uneven wetland basins, meandering grass corridors, and scrub
fringes that move with local habitat pressure. Four seeds (4, 14, 42, and
101) ran for 20,000 ticks with zero blocked births and peak populations
between 114 and 117 out of 160 slots. The companion requests up to 12 frames
per second over USB. UP cycles through 8× Sprites, 16× detail, Hunt, Life,
and Habitat, with each change reflected in the companion. DOWN controls
speed and pause. The mirror
uses the same four bits per color channel as the panel scanout.
Bare scrub uses three red steps and one green step for a warm brown. The
Panel Mirror UF2 uses the established asexual simulation.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-noisy-terrain.uf2 --observatory
```

### USB baseline and display streak test

The original single-buffer build was measured on the MatrixPortal M4 over USB.
Across a 155-second capture, steady readings were 10, 20, 40, and 80 ticks/s
at the four speed settings and 20 rendered frames/s throughout. The 8×
setting sustained 80 ticks/s for 130 seconds. The highest sampled population
was 97, with zero blocked births and no observed reset or telemetry gap.

The double-buffered build uses a separate Protomatter scan buffer and swaps
at the frame boundary to address potential tearing. The built
`build/petri-matrixportal-m4-double-buffered.uf2` was verified against the
compiled binary. A follow-up USB capture at 8× recorded 4,800 ticks in
59.95 seconds. All 61 reports showed 80 ticks/s and 20 rendered frames/s,
with no observed reset or telemetry gap, a peak sampled population of 97,
and zero blocked births. The operator reported that the streaking was gone
after flashing this build.

### ESP32 bridge

The ESP32 has no USB port of its own. The SAMD51 bridges its USB serial
port to the ESP32 UART in two ways:

- **Petri firmware:** hold UP while the board starts (press RESET while
  holding UP, and keep holding for about a second). The panel shows
  `ESP32 LINK` and the ESP32's state. Press RESET to return to Petri.
- **Standalone:** `build/petri-esp32-passthrough.uf2` starts the bridge at
  every boot. It is smaller and useful for recovery.

The host's DTR and RTS lines drive the ESP32's reset and GPIO0 lines like the
auto-reset circuit on ESP32 development boards. esptool's default reset and
hard reset both work. Reads matched the backup at 921600 baud through the
standalone bridge and at 460800 through Petri's bridge mode. In Petri's
bridge mode, a 921600 read once lost a byte, likely because the panel's
refresh interrupt delays the UART receiver. Tap UP to
restart the ESP32 into its own firmware and DOWN to restart it into its
bootloader. The NeoPixel and the bridge screen show yellow for the bootloader
and green for the firmware.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-observatory-bridge.uf2 --observatory
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4:usbstack=tinyusb \
  --output-dir build/esp32-passthrough firmware/esp32_passthrough
python3 /path/to/uf2conv.py -c -b 0x4000 -f SAMD51 \
  -o build/petri-esp32-passthrough.uf2 build/esp32-passthrough/esp32_passthrough.ino.bin
esptool --port /dev/ttyACM0 --baud 460800 flash-id
```

The board's ESP32 is an ESP32-D0WD-V3 with 4 MB of flash, running Adafruit
nina-fw 1.2.2. A verified full image and its restore steps are kept in the
private `backups/` directory. See [the dual-processor plan](docs/DUAL_PROCESSOR.md).

[`tools/board/`](tools/board/README.md) has scripts that flash both chips,
drive the display over USB, record the ESP32 self-test, and run soak tests.

On Linux, the board's serial port is reassigned each time the board
re-enumerates. To give the logged-in user access to Adafruit devices,
including after each re-enumeration, install a udev rule once:

```sh
echo 'SUBSYSTEMS=="usb", ATTRS{idVendor}=="239a", TAG+="uaccess"' | sudo tee /etc/udev/rules.d/70-adafruit-uaccess.rules
sudo udevadm control --reload && sudo udevadm trigger
```

### ESP32 core self-test

`esp32/` is an ESP-IDF 5.5 project that runs the portable self-test in
`esp32/selftest` on ESP32 core 1. The self-test runs four 128×64
configurations to tick 10,000 and prints a fingerprint line at ticks 1, 256,
2,000, and 10,000. Every fingerprint must match the native build. It also
times steps from tick 2,000 to 10,000 and renders each panel view. The same
source runs natively and on the SAMD51 (`--selftest` in the UF2 build script).

Flash it through either bridge. This replaces the ESP32's nina-fw. The
restore steps are in `backups/`.

```sh
. ~/esp/esp-idf-v5.5.6/export.sh
cd esp32
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyACM0 -b 460800 flash
```

For the Bluetooth LE measurement, layer the NimBLE defaults into a
separate build directory:

```sh
idf.py -B build-ble -DSDKCONFIG=build-ble/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.ble" build
```

The system Python here is 3.14, which ESP-IDF 5.5 may not support. This
machine runs `install.sh` and `export.sh` with a `uv`-managed Python 3.12
first on `PATH`.

On November 18, 2026, all 16 fingerprints matched the native build on the
ESP32, both with and without NimBLE, and on the SAMD51. Step timings cover
ticks 2,000 to 10,000 on one core:

| Case | Native (x86) | ESP32 core 1, 240 MHz | SAMD51, 120 MHz |
| --- | ---: | ---: | ---: |
| Plant-only world, seed 42 | 94,334 ticks/s | 788 ticks/s | 283 ticks/s |
| Food web, seed 4 | 50,156 | 460 | 172 |
| Every ecology feature, seed 14 | 28,274 | 251–276 | 118 |
| Observatory world, seed 14 | 24,428 | 242–247 | 103 |
| Close view frame | 57 µs | 4.5 ms | 10.5 ms |
| Life view frame | 8 µs | 1.2 ms | 2.4 ms |

On the ESP32, the observatory world's mean step was about 4.1 ms, with a
99th percentile of 5.7 ms. Running the core from IRAM instead of the flash
cache changed the rate by under 2%. A native profile splits the cost about
evenly between per-cell environment updates and creature updates.
`World::recycle` alone runs on about 4,096 cells each tick.

| ESP32 heap, bytes | No radio | NimBLE advertising |
| --- | ---: | ---: |
| Byte-addressable free at boot | 217,824 | 160,456 |
| Byte-addressable free with BLE up | — | 121,788 |
| Lowest byte-addressable free | 201,080 | 105,024 |
| Largest byte-addressable block | 110,592 | 94,208 after the run |
| 32-bit-only (IRAM) heap free | 81,212 | 34,152 |
| Static DRAM used of segment | 94,180 of 180,736 | 107,368 of 124,580 |

Static DRAM includes the 72,664-byte world and the 7,760-byte panel
renderer. Bluetooth reserves about 56 KB of the static DRAM segment for its
controller. NimBLE advertising used about 96 KB of byte-addressable heap and
47 KB of IRAM heap. Simulation timing did not change, because NimBLE runs on
core 0.

### Render state and stream

The renderer draws a `RenderState`, never `World` directly. It holds 3-byte
render cells, creature phenotypes, and step events. `LocalPanel` captures that
state from a `World` on the same chip. The ESP32 will stream it to the
SAMD51 with `stream::Encoder` and `stream::Decoder`. See the
[render-state section](docs/DUAL_PROCESSOR.md#render-state) of the plan
for the formats, frame comparisons, and stream sizes. In the 128×64 world an
update is about 3.6 KB at one tick per update and 12 KB at twelve.
`tests/render_stream_test.cpp` prints those figures.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-observatory-mirror.uf2 --observatory
```

### Dual-processor firmware

The ESP32 runs the world, and the SAMD51 draws it from the render stream.
Flash the ESP32 world app through a bridge, then the display firmware:

```sh
cd esp32
idf.py -B build-world -DSDKCONFIG=build-world/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.world" build
idf.py -B build-world -DSDKCONFIG=build-world/sdkconfig -p /dev/ttyACM0 -b 460800 flash
cd ..
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-display.uf2 --display
```

Tap UP to change view. Tap DOWN to cycle 1, 2, 4, and 8 ticks/s, and hold
DOWN to pause. Over USB, `>` and `|` do the same as a DOWN tap and hold. The
companion connects as before. Its inspector shows the ESP32's census, which
can be up to one second old. Once a second the display prints a `DISPLAY` and
a `STREAM` line with the tick, world fingerprint, frame and update rates, and
stream health.

The world is saved to QSPI every 10 minutes in four rotating slots. A SAMD51
reset or power cut also restarts the ESP32. On every boot the SAMD51 restores
the newest valid slot, or starts seed 14 fresh if none is valid. A save that
was interrupted leaves its slot invalid, and the previous slot is used. USB
commands: `S` saves now. The test hooks are `C`, which toggles back-to-back
saves, and `X`, which corrupts the newest slot. These checkpoints replace the
CIRCUITPY drive that was on the QSPI flash. Its files are in `backups/`.

### Graphics review without the board

`petri_snapshot` renders the observatory world into a PNG contact sheet.
Rows are the close, detail, zoom-3, life, and habitat views, and columns are
consecutive frames, with each LED drawn as a block.

```sh
./build-wide/petri_snapshot sheet.png --ticks 3000 --frames 4 --interval-ms 60
```

The display firmware draws at Protomatter depth 5 and 30 fps. To try other
values, set `PETRI_EXTRA_FLAGS` when building, for example
`PETRI_EXTRA_FLAGS=-DPETRI_COLOR_DEPTH=6 firmware/build_petri_uf2.sh ... --display`. Rain,
the season grade, and wetland glints come from the world's real rainfall
field and season. See the [graphics plan](docs/GRAPHICS_AND_EVOLUTION.md).

### SPI link test

`link/link.hpp` defines the fixed 4,112-byte frame the SAMD51 and ESP32
exchange over SPI. `firmware/linktest` is the SAMD51 master, and the
`esp32` project built with `sdkconfig.defaults.link` is the ESP32 slave. The
slave also steps the observatory world on core 1. The master reports once a
second over USB and accepts `clock <hz>`, `mode <0-3>`, `reset`, `restart`,
`float`, and `clear`.

```sh
firmware/build_petri_uf2.sh /path/to/uf2conv.py build/petri-linktest-samd51.uf2 --linktest
cd esp32
idf.py -B build-link -DSDKCONFIG=build-link/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.link" build
idf.py -B build-link -DSDKCONFIG=build-link/sdkconfig -p /dev/ttyACM0 -b 460800 flash
```

Results on November 18, 2026, with 15 seconds per setting:

| SPI clock | Mode 1 | Mode 0 |
| ---: | --- | --- |
| 2.4–4.8 MHz | No errors, 65–118 frames/s | No errors, 65–118 frames/s |
| 6 MHz | No errors, 141 frames/s | No errors, 141 frames/s |
| 8 MHz | SAMD51 decoded no frames | No errors, 176 frames/s (721 KB/s each way) |
| 12 MHz and 24 MHz | SAMD51 decoded no frames | SAMD51 decoded no frames |

In every failing setting the ESP32 still received valid frames. Only
ESP32-to-SAMD51 data failed. A 3-minute soak at 8 MHz in mode 0 moved
31,639 frames with no CRC, pattern, sequence, or timeout errors in either
direction. Five hardware resets and five software restarts of the ESP32 each
recovered, in 291 ms and about 309 ms. The board holds the ESP32 in reset
whenever the SAMD51 releases that pin, so a SAMD51 reset always restarts the
ESP32.

### Panel and button test

Copy `build/petri-panel-test.uf2` to MATRIXBOOT. After the board restarts, the
64×32 panel should show red, green, and blue bars from left to right, with a
white corner at top left and a yellow corner at bottom right. Hold UP to turn
the top arrow cyan and DOWN to turn the bottom arrow magenta. Both arrows
return to gray on release. A slow blink of the onboard red LED indicates
that the matrix driver failed to initialize.

The sketch is in [firmware/panel_test](firmware/panel_test/panel_test.ino).
The tested build used Adafruit SAMD 1.7.17 and Protomatter 1.7.1. With those
installed, rebuild from the repository root and convert the Arduino binary
with [Microsoft's UF2 converter](https://github.com/microsoft/uf2/tree/master/utils).

```sh
arduino-cli compile --fqbn adafruit:samd:adafruit_matrixportal_m4 \
  --output-dir build/panel-test firmware/panel_test
python3 /path/to/uf2conv.py -c -b 0x4000 -f SAMD51 \
  -o build/petri-panel-test.uf2 build/panel-test/panel_test.ino.bin
```

Native tests cover heredity, mutation bounds, birth energy costs, carrying
capacity, selection scenarios, and byte-for-byte checkpoint replay. The
render test verifies creature placement and identical simulation state at
the same tick with and without rendering.
