# Meat-specialist experiments

The goal is coexistence of a reproducing meat-specialist lineage and living
plant feeders across multiple matched seeds and separated late observations.
A specialist needs inherited meat digestion of at least 10/15 and at least
80% animal food by lifetime biomass, with a minimum of 32 units consumed.
A qualifying birth has a parent meeting those criteria and a child inheriting
at least 10/15 meat digestion. Founder IDs connect births and living specialists
across intervals. The native observer records births before any same-tick
mortality.

## Baseline and exploratory results

Six matched seeds (1, 2, 3, 42, 101, 2026) ran to tick 100,000 at nutrient
budgets of 7 and 48 with no seeded carrion. Each condition has a predation-off
control. The baseline reported no living specialists and no qualifying births
in 240 samples at intervals of 10,000 ticks.

The exploratory patch adds three mechanisms. Dissolved nutrients move along
neighboring cell edges with a bias toward wet ground. Creatures can move one
cell toward prey they sense two cells away. A successful hunt transfers part
of the prey's existing energy reserve to the hunter, scaled by inherited meat
digestion. No new animal material enters the world. Only the nutrient
transport operates in the predator-off controls.

| Nutrients | Predation | Seeds with kills in ticks 70,001–100,000 | Late kills per seed | Peak population | Extinctions |
| ---: | :--- | ---: | :--- | ---: | ---: |
| 7 | Off | 0 of 6 | 0 | 146 | 0 |
| 7 | On | 6 of 6 | 4,210–10,272 | 72 | 0 |
| 48 | Off | 0 of 6 | 0 | 152 | 0 |
| 48 | On | 5 of 6 | 0–20,083 | 120 | 1 |

None of the 24 prototype runs had blocked births, a sampled live specialist,
or an observed qualifying birth. The prototype sustained hunting in 11 of
12 predator-on runs, so scarce encounters alone cannot account for the missing
niche. That prototype remains a separate scratch build. The native CLI retains
the original ecology as its default.

## Replay the baseline and earlier prototype

From the repository root, run the standard build or compile the prototype in
a scratch directory using the saved patch.

```sh
work=$(mktemp -d)
mkdir "$work/core"
cp core/world.cpp core/world.hpp core/checkpoint.cpp core/nests.cpp core/hunting.cpp "$work/core/"
cp apps/main.cpp "$work/"
patch -s -d "$work" -p1 < docs/experiments/nutrient-transport-and-pursuit.patch
c++ -std=c++17 -O3 -I"$work/core" "$work/main.cpp" \
  "$work/core/world.cpp" "$work/core/checkpoint.cpp" "$work/core/nests.cpp" \
  "$work/core/hunting.cpp" -o "$work/petri"

for exe in ./build/petri "$work/petri"; do
  for seed in 1 2 3 42 101 2026; do
    for nutrients in 7 48; do
      "$exe" --seed "$seed" --nutrients "$nutrients" --ticks 100000 --lineage-every 10000
      "$exe" --seed "$seed" --nutrients "$nutrients" --ticks 100000 --lineage-every 10000 --predation
    done
  done
done
```

## Reproducing meat-specialist lineages

The fixed `--food-web` benchmark uses 80 nutrients per cell, a decay rate of
1, and zero seeded carrion. Predation-off worlds use the same food-web rules
with hunting disabled.

- **Reproduction uses nutrients carried by the parent.** Creatures retain a
  bounded fraction of what they eat and can reproduce after moving away
  from the meal. Death returns unused nutrients to the environment.
  Dissolved nutrients flow along cell edges, with a bias toward wet ground.
- **Digestion affects foraging and hunting.** A single inherited allocation
  splits digestion between plant and animal food. Plant fiber costs rise
  as allocation shifts toward meat. Hungry creatures weigh the expected
  return on nearby prey against other available food. They can pursue prey
  within five cells. Capture odds rise with the meat allocation and fall
  with the prey's energy and surrounding vegetation. A capture transfers
  up to three quarters of the prey's existing energy, along with a bite
  from its carcass.
- **The seed provides modest standing variation.** Founder meat allocation
  is sampled from 5–11 out of 15. Mutations can raise it above that range.
  Every creature uses the same rules and can eat either food. There's no
  replenished carrion source.

The development set used seeds 1, 2, 3, 42, 101, and 2026. After fixing the
settings, the validation set used seeds 4 through 15. A run qualified if
the same founder lineage had living specialists and qualifying births at
every listed late observation, with plant feeders present. At least one
of those observations also had to show a specialist above the founder
digestion range. Every sample reports births during the preceding 10,000
ticks. The result describes lineage persistence across sampled intervals.

| Seed set | Observations | Predation on | Predation off |
| --- | --- | ---: | ---: |
| Development | 80,000, 90,000, 100,000 | 4 of 6 | 0 of 6 |
| Validation | Every 10,000 from 160,000 to 200,000 | 2 of 12 | 0 of 12 |

| Validation seed | Founder lineage | Qualifying births across five sample windows | Late plant feeders | Highest observed specialist meat allocation |
| ---: | ---: | ---: | :--- | ---: |
| 4 | 24 | 1,080 | 89–107 | 14/15 |
| 14 | 18 | 987 | 98–113 | 15/15 |

At the final observation, qualifying specialists in those lineages had
reached generations 1,902 and 1,934 respectively. Across all 36 paired
treatment and control runs, the highest peak was 151 out of 160 and zero
births were blocked. The niche replicated at the calibrated setting and
was sensitive to the seed. The screening experiments also produced
extinction or pool saturation with some other settings.

### Replay the fixed benchmark

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
python3 docs/experiments/verify_food_web.py build/petri
```

To inspect one matched pair directly:

```sh
./build/petri --food-web --predation --seed 4 --nutrients 80 --ticks 200000 --lineage-every 10000
./build/petri --food-web             --seed 4 --nutrients 80 --ticks 200000 --lineage-every 10000
```

## Adaptive hunting and evasion

In `--adaptive-hunts`, inherited hunting affects sensory range, capture odds,
and whether an adult can sprint or lunge through an open cell. High hunting
investment raises upkeep. Digestion shapes appetite and how much energy the
adult can recover from a kill. A pursuit costs energy and takes the place of the
foraging action. A prey animal can notice a pursuit or missed strike from
the previous tick and spend its next action retreating or entering denser
vegetation. Its inherited evasion trait affects how far it can notice the
threat and how strongly it values cover. Nest-bound young stay in place and
continue to benefit from adjacent guards. Founders have independent standing
variation in the new traits. The seed gives the treatments the same starting
terrain, food, placements, and digestion genes.

`--hunt-every` counts adult decisions by reporting interval. `sated` and
`low_energy` end the decision before the search. `no_prey` and `blocked` follow
a search with no eligible target or with no open step toward a target.
`forage_or_rest` counts turns when a reachable hunt failed to beat its
alternative. The same output counts pursuit steps, strikes, kills, retreats,
and cover moves. Small young use their nest action instead of the adult hunt
decision. The `--passive-prey` treatment uses adaptive hunting with active
retreats and cover moves disabled.

A hunter has landed at least one kill and eaten a majority animal diet. A
grazer has eaten more than 80% plants. Both need a lifetime sample of at least
32 food units. A birth counts toward either strategy when one or both parents
qualify. A mixed pair can contribute to both counts. Coexistence requires a
living representative and a qualifying birth for both strategies in every
one of the five final 10,000-tick windows, with zero blocked births.

Eight seeds, 5–12, ran for 100,000 ticks under each treatment. The 128×64 runs
used 24 nutrients per cell, connected terrain, sexual reproduction, nests,
guarding, and provisioning. The 64×32 runs used the older 80-nutrient food-web
setup with asexual reproduction. The comparisons are matched within each
ecology.

| Ecology | Existing hunts | Adaptive hunts, passive prey | Adaptive hunts and evasion |
| --- | ---: | ---: | ---: |
| 128×64 with nests | 0/8 | 0/8 | 0/8 |
| 64×32 benchmark | 2/8 | 0/8 | 0/8 |

In the wide world, adaptive hunting and evasion produced 1,015 kills, 4,886
retreats, and 868 cover moves. Adaptive hunting with passive prey produced
1,641 kills. Both adaptive treatments had zero qualifying hunter births in
the final five windows in either ecology. In the wide active-evasion runs,
92.0% of recorded adult actions in those late windows ended at the satiety
gate. Of turns that reached the target search, 55.8% rejected a reachable hunt
in favor of foraging or resting and 44.2% found no eligible prey. All 48
populations survived with zero blocked births.

```sh
python3 docs/experiments/compare_hunting.py build-wide/petri --output build-wide/hunting-matched-runs.csv
python3 docs/experiments/compare_hunting.py build/petri --world compact --output build/hunting-matched-runs.csv
```

## Hunt energy and tuning

`--hunt-every` reports the hunter's energy during each window. `hunt_cost`
adds up movement and strike charges, including failed strikes and chases
that ended without a kill. `hunt_return` adds energy drawn from prey and
energy from meat actually eaten on the kill turn, then deducts any amount
lost at the hunter's 160-energy cap. The log breaks out routine upkeep on
those turns and energy credited from any other meals. The CSV gives net
energy after direct costs and net energy after the full set of charges for
those turns. It measures the hunter's energy balance during hunting turns.

`--hunt-threshold` sets the base energy below which an adult can consider
hunting. The simulation adds two units per point of meat digestion above
eight and subtracts two per point below eight. `--chase-cost` charges per
cell of pursuit movement, including the approach cell of a lunge. A strike
still costs three units. Both flags apply to `--adaptive-hunts` and are
saved in the checkpoint.

The sweep used the same two ecologies, seeds 5–12, active evasion, and
100,000-tick horizon as the previous experiment. Each treatment changed one
parameter from the reference of 42 for threshold and 2 for chase cost.
Zero chase cost tests the limit of reducing movement costs. Net values in
the table divide pooled energy by pooled kills, so all pursuit and strike
attempts contribute to the result.

| Threshold | Chase cost | Wide direct cost per kill | Wide return per kill | Wide net per kill after upkeep | Compact net per kill after upkeep | Compact hunter-parent births |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 42 | 2 | 29.9 | 27.3 | −44.6 | +3.7 | 4 |
| 30 | 2 | 44.4 | 29.7 | −78.9 | +2.6 | 1 |
| 60 | 2 | 29.7 | 30.8 | −41.9 | +5.5 | 53 |
| 80 | 2 | 25.5 | 32.3 | −31.7 | +7.7 | 98 |
| 42 | 0 | 6.0 | 27.3 | −28.3 | +1.8 | 12 |
| 42 | 1 | 17.9 | 28.0 | −30.3 | +36.8 | 1,403 |
| 42 | 4 | 55.8 | 30.4 | −69.2 | +3.0 | 5 |

Every treatment scored 0/8 on late coexistence in both ecologies. All 112
populations survived and recorded zero blocked births. Raising the threshold
to 80 increased wide-world kills from 1,015 to 2,306. Compact seed 10 recorded
1,396 of the 1,403 hunter-parent births at chase cost 1, all by tick 40,000.
Living hunters and grazers both produced young in the windows ending at
20,000 and 30,000 in that seed. There were zero qualifying hunter-parent
births in the final five windows of every treatment.

```sh
python3 docs/experiments/compare_hunting.py build-wide/petri --energy-sweep --output build-wide/hunt-energy-sweep.csv
python3 docs/experiments/compare_hunting.py build/petri --world compact --energy-sweep --output build/hunt-energy-sweep.csv
```

## Heritable kin avoidance

`--kin-avoidance` adds an inherited reluctance to `--adaptive-hunts`. By
default, parents, offspring, and siblings, including half siblings, receive
the strongest discount to the hunter's expected payoff. Other prey receive a
smaller discount based on how similar they are in digestion, thrift, hunting,
and evasion. An adult can still hunt a relative when the discounted payoff
is high enough.

Founders start with reluctance alleles from 5 to 11 on the usual 1–15 scale.
Offspring inherit the trait with the usual bounded mutation. The allele
uses a separate deterministic draw, preserving the control's ecological
random sequence until the first change in a decision. The `kin_reluctance`
counter records turns where the discount rules out the hunt and the creature
returns to foraging or rest. `--hunt-every` also reports kills of close kin,
parent–offspring kills, kills within a founder lineage, and kills of
genetically similar prey in both modes.

Both compact runs used seed 10, 80 nutrients per cell, chase cost 1, active
evasion, and the default hunt threshold. Each ran for 100,000 ticks. The
control reproduced the earlier seed 10 experiment in every reporting window.

| Metric | Control | Kin avoidance |
| --- | ---: | ---: |
| Total kills | 14,359 | 804 |
| Close-kin kills | 2,415 (16.8%) | 23 (2.9%) |
| Parent–offspring kills | 2,340 | 15 |
| Births with a qualifying hunter parent | 1,396 | 20 |
| Hunter-parent births in the final five windows | 0 | 0 |
| Grazer-parent births in the final five windows | 9,674 | 19,736 |

In the enabled run, all 20 hunter-parent births occurred before the 10,000-tick
sample. The control still had hunter-parent births in the window ending at
tick 40,000. Both populations survived without blocked births. This pair
shows the combined effect of kin and resemblance discounts for one seed.

```sh
python3 docs/experiments/compare_hunting.py build/petri --world compact --kin-test \
  --output build/compact-seed10-kin-avoidance.csv
```

## Kin and resemblance sweep

`--kin-strength` and `--resemblance-strength` scale how much a creature's
inherited reluctance reduces its expected hunt payoff. Each strength ranges
from 0 to 20 and defaults to 7. A prey item can qualify for either discount
or both. The hunter applies the larger discount and caps it at 95%. The
resemblance measure weights digestion twice and counts thrift, hunting,
and evasion once. A weighted difference below 12 identifies similar prey.
This is a pairwise comparison of traits. The world has no species boundaries.

`--protect-similar-prey` tests a strict cutoff using the same measure. Prey
below that difference are ineligible for hunts. With `--neutral`, all prey
have the same expressed traits and the cutoff excludes them all. The strengths
and cutoff require `--kin-avoidance` and are stored in the checkpoint.

The compact sweep crossed strengths 0, 2, 7, and 14, with the 0/0 run using
the original control. Two additional treatments applied the strict cutoff
with kin strength 0 or 14. Each treatment ran on seeds 5–12 for 100,000
ticks with 80 initial nutrients per cell, chase cost 1, hunt threshold 42,
and active evasion. The counts below pool all eight seeds. A qualifying
hunter had made a kill, eaten at least 32 food units, and obtained a majority
of its lifetime food from meat.

| Kin strength | Resemblance strength | Total kills | Close-kin kills | Dissimilar prey kills | Hunter-parent births |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 26,433 | 3,702 | 10,895 | 1,403 |
| 0 | 2 | 17,107 | 2,074 | 5,490 | 666 |
| 7 | 7 | 26,241 | 464 | 2,001 | 26 |
| 14 | 0 | 19,936 | 11 | 3,850 | 27 |
| 0 | 14 | 64,587 | 4,060 | 6,265 | 34 |
| 0 or 14 | Strict cutoff | 423 | 0 | 423 | 0 |

Grazer parents reproduced in every final window ending at ticks
60,000–100,000. There were zero hunter-parent births in those windows across
all 144 runs. Every population survived without blocked births, and the
highest observed population was 151 out of 160.

Kin strength 14 alone reduced close-kin kills from 14.0% to 0.06% of kills.
Resemblance strength 14 alone raised the similar-prey share from 58.8% to
90.3% while reducing the total number of dissimilar prey killed. The kill
composition reflects which prey were present as well as the hunter's choices.
Both strict treatments produced the same outcome, with all kills occurring
in the first 20,000 ticks. In seed 10, the strict cutoff yielded 35 kills and
zero hunter-parent births, down from 14,359 kills and 1,396 hunter-parent
births in the control.

```sh
python3 docs/experiments/compare_hunting.py build/petri --world compact --kin-sweep \
  --output build/compact-kin-strength-sweep.csv
```

## Heritable faction markers

`--faction-markers` gives the 32 founders four markers, with eight founders
per marker. A seeded shuffle assigns markers independently of placement and
diet. An offspring copies its sole parent's marker or draws equally from
its two parents. The marker mutates to one of the other three values on
roughly one birth in 128. Marker assignment and inheritance use a separate
deterministic draw from the ecological random stream.

`--cross-faction-hunts` enables the markers and limits adaptive pursuits
and strikes to prey with a different marker. Creatures still choose mates
and forage by the usual rules. `faction_population` gives each marker's
living count. The hunt log counts kills within and across factions and
reports `faction_protected` when the hunter has an otherwise worthwhile
same-faction target and finds no worthwhile eligible hunt.

Seeds 5–12 ran for 100,000 ticks per treatment with chase cost 1, hunt
threshold 42, and active evasion. The 64×32 world used 80 initial nutrients
per cell and asexual reproduction. The 128×64 world used 24 initial
nutrients, connected irregular terrain, sexual reproduction, nests,
guarding, and provisioning. Both treatments began with identical markers
and used the same kin and genetic resemblance settings, with both
discounts disabled.

| World | Hunt rule | Same-faction kills | Cross-faction kills | Hunter-parent births |
| --- | --- | ---: | ---: | ---: |
| 64×32 | All factions eligible | 16,670 | 9,763 | 1,403 |
| 64×32 | Cross-faction only | 0 | 952 | 4 |
| 128×64 | All factions eligible | 920 | 360 | 0 |
| 128×64 | Cross-faction only | 0 | 352 | 0 |

All 32 populations survived with zero blocked births. Grazers bred in every
late window, and no hunter parent reproduced in any of the five windows
ending at ticks 60,000–100,000. All four hunter-parent births under
protection occurred in the compact world's first 10,000 ticks. Compact seed
10 went from 1,396 hunter-parent births in the control to zero with
protection. Every protected run still had at least two markers at tick
100,000. The last five windows recorded 39 cross-faction kills in the
protected compact runs and zero in the protected wide runs.

```sh
python3 docs/experiments/compare_hunting.py build/petri --world compact --faction-test \
  --output build/compact-faction-hunt-runs.csv
python3 docs/experiments/compare_hunting.py build-wide/petri --world wide --faction-test \
  --output build-wide/faction-hunt-runs.csv
```

## Regional weather and succession

Seeds 4, 14, 42, and 101 ran to tick 524,288 with and without
`--dynamic-environment`. Each run used the observatory's 128×64 world,
24 nutrients per cell, irregular connected biomes, fruiting shrubs,
asexual reproduction, and the established hunting rules. Observations at
ticks 256, 4,096, 16,384, 65,536, 131,072, 262,144, and 524,288 recorded
habitat type and shrub occupancy. The last observation corresponds to
about 18.2 hours of operation at eight ticks per second.

Weather adds gradual regional wet/dry changes and a slower seasonal cycle.
Drought returns plant material to soil, and uneaten fruit can establish
neighboring shrubs. Spring-fed cores supply drought refuges. Hungry animals
search within four cells when the adjacent food supply is too small.
The weather treatment also reduces wetland growth from six to four growth
steps per 64 ticks.

| Seed | Control cells with a different habitat | Weather cells with a different habitat | Final weather population | Weather peak population | Blocked births |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4 | 3.1% | 23.2% | 42 | 155 | 0 |
| 14 | 2.9% | 16.9% | 50 | 160 | 199 |
| 42 | 2.7% | 22.5% | 75 | 160 | 247 |
| 101 | 3.0% | 24.5% | 59 | 160 | 60 |

The habitat percentages compare the last observation with tick 256.
Comparing the final two observations showed 22.1–40.1% habitat turnover
under weather and 1.9–2.2% in controls. Between 872 and 936 cells changed
shrub occupancy under weather. Shrub occupancy stayed fixed in controls.
Every population survived. All 56 observations retained exactly 196,864
units of material. Three weather runs reached the 160-creature limit,
with 506 blocked births across 838,126 births in the weather runs.

Build the comparison runner after the wide native libraries. It writes CSV
to stdout and renders the terrain at each sample to PPM files with the
given output prefix.

```sh
c++ -std=c++17 -O3 -DNDEBUG -DPETRI_WIDE_WORLD=1 -I core -I render \
  docs/experiments/compare_environment.cpp build-wide/libpetri_render.a \
  build-wide/libpetri_core.a -o build-wide/compare_environment
./build-wide/compare_environment 14 control build-wide/control14 > build-wide/control14.csv
./build-wide/compare_environment 14 dynamic build-wide/weather14 > build-wide/weather14.csv
```

## Morphology genes

`--morphology` (with `--food-web`) adds four heritable genes, each from 1 to
15. Founders draw body size, pattern, and limbs from 5–11 and pigment from
1–15 on separate seeded draws. Offspring copy each gene from one parent and
mutate it by ±1 with probability 1/8 each way, clamped to 1–15, on draws
that leave the ecological random stream untouched. Every effect is relative
to 8, the founders' standard:

| Gene | Benefit | Cost |
| --- | --- | --- |
| Body size | One bite unit per two steps above 8. Energy store 160 + 6 per step. Two capture points per step of size advantage, as hunter or as prey | 1/32 energy per tick per step above 8. Young are provisioned in proportion to the parent's store, and breeding thresholds scale with it |
| Pigment | Prey within 4 tones of the ground under it is harder to catch (5 points per tone of match, up to 20) and is seen only closer than the hunter's range | Prey 9 or more tones off its ground is easier to catch, by up to 10 points. One pigment cannot match every habitat |
| Pattern | A bold pattern hides prey in dense vegetation: up to 14 points at 16+ plants | The same pattern shows on bare ground, up to 14 points, where a plain coat hides instead |
| Limbs | Over 16 ticks, L − 8 extra foraging steps (each paid for). Two capture points per step of limb advantage. Longer evasion reaction range, lunge and sprint at lower hunting skill, wider food search | 1/32 energy per tick per step above 8. Short limbs miss 8 − L foraging steps in 16 |

The ground's tone is `moisture × 15 / 255`, so 0 is the driest scrub and
15 the wettest marsh. Upkeep fractions are spread over a shared 32-tick
cycle, so the mean cost is exact and the run stays integer-only. Upkeep
never drops below 1.

`--neutral-morphology` is the control. The genes are inherited and mutate
in the same way but have no effect. Its ecology matches a run without
`--morphology` exactly, which `tests/world_test.cpp` checks slot by slot.
Without either flag, the 68 matched runs from the capacity change give
byte-identical CLI output.

Seeds 5–12 ran 200,000 ticks under each treatment in three ecologies. The
64×32 food web used 80 nutrients, asexual reproduction, and adaptive
hunting with evasion. Its no-predation variant dropped `--predation` and
`--adaptive-hunts`. The 128×64 world was the observatory: 24 nutrients,
irregular connected biomes, fruiting shrubs, factions, and weather. Values
are means across seeds of the living population's means, ± the standard
deviation across seeds. The count is how many of the 8 seeds moved the
same way as the mean difference.

| Ecology | Trait | Morphology | Neutral control | Seeds |
| --- | --- | ---: | ---: | ---: |
| 64×32 with predation | Body size | 9.9 ± 1.6 | 8.3 ± 1.4 | 6/8 larger |
| | Limbs | 10.0 ± 0.8 | 7.4 ± 2.0 | 7/8 longer |
| | Pattern | 4.9 ± 1.2 | 8.8 ± 1.6 | 8/8 plainer |
| | Pigment mismatch | 3.4 ± 1.4 | 5.7 ± 1.4 | 7/8 closer |
| | Kills | 1,273 ± 514 | 1,725 ± 524 | 5/8 fewer |
| 64×32 without predation | Body size | 7.4 ± 1.5 | 7.5 ± 1.5 | 5/8 smaller |
| | Limbs | 8.8 ± 0.9 | 7.4 ± 1.5 | 6/8 longer |
| | Pattern | 8.5 ± 1.4 | 7.6 ± 2.0 | 6/8 bolder |
| | Pigment mismatch | 4.6 ± 1.6 | 5.2 ± 1.4 | 5/8 closer |
| 128×64 observatory | Body size | 6.2 ± 1.2 | 9.1 ± 4.2 | 6/8 smaller |
| | Limbs | 8.0 ± 0.6 | 9.1 ± 2.5 | 5/8 shorter |
| | Pattern | 6.7 ± 3.8 | 9.2 ± 3.8 | 5/8 plainer |
| | Pigment mismatch | 4.5 ± 1.5 | 3.3 ± 0.7 | 6/8 farther |
| | Kills | 4.5 ± 2.3 | 3.5 ± 1.1 | 4/8 more |

Every population survived, and no run blocked a birth. Final 64×32
populations were within 4% of the controls on average (101.5 against 105.6
creatures with predation). Observatory populations ended 9% larger (63.0
against 57.6), and one observatory run peaked at 264 creatures, past the
old 160-slot limit.

- **Camouflage is selected where hunting happens.** With predation, pigment
  moved toward the ground the animals stand on in 7 of 8 seeds, and the
  share within two tones of it rose by 22.6 creatures per run. Without
  predation the shift was small and inconsistent (5 of 8 seeds). The
  observatory world, with about four kills per 200,000 ticks, showed no
  selection on pigment: its mismatch drifted, here away from the ground.
  Kills fell by a quarter with camouflage, but only in 5 of 8 seeds.
- **Pattern follows the ground under predation.** The animals stand mostly
  on grazed cells, where a plain coat hides best, and every predation seed
  went plainer. Without predation, pattern drifted the other way.
- **Body size depends on the ecology.** It rose in the predation world,
  where size helps both to catch and to resist capture, and was neutral
  without predation. In the lean observatory world it fell to about 6, and
  its spread across seeds was a third of the control's (1.2 against 4.2).
  That points to stabilizing selection around a smaller body, not
  only to drift.
- **Limbs pay for foraging speed in the 64×32 world**, with and without
  hunting, and settle near 8 in the observatory world, again with a much
  smaller spread than the control (0.6 against 2.5).

Earlier tunings were rejected. With upkeep of 1/16 per step and one bite
unit per three size steps, body size fell to 1.4 in the 64×32 world and 2.3
in the observatory world within 200,000 ticks, 64×32 limbs fell to 1.3, and
the 64×32 populations more than doubled on the cheaper upkeep (239 against
106). At 1/8 per step the collapse came sooner. Upkeep of 1/32 per step with one bite unit per two
size steps is the setting recorded above.

```sh
python3 docs/experiments/compare_morphology.py build/petri --world compact --ticks 200000 \
  --output build/morphology-compact.csv
python3 docs/experiments/compare_morphology.py build/petri --world compact-no-predation --ticks 200000 \
  --output build/morphology-no-predation.csv
python3 docs/experiments/compare_morphology.py build-wide/petri --world wide --ticks 200000 \
  --output build-wide/morphology-observatory.csv
```
