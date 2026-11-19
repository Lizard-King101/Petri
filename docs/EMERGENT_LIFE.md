# Emergent life: roadmap and Phase 1–2 design

**Status:** November 18, 2026. Phase 1 runs natively behind `--brains`
(`core/brains_world.cpp`) and Phase 2 behind `--trophic`, with founders bred
by `apps/nursery.cpp`. Worlds last on all eight seeds, with thousands of kills,
but nothing lives on meat and one founding lineage remains: see Results and
Phase 2 results. Brains worlds save and resume (checkpoint version 19). The
board has not run either: see On the board.

## Where the world stands

These are matched runs of the observatory world: 8 seeds (1, 2, 3, 4, 14, 42,
101, 2026), 50,000 ticks each, with storms, morphology, and 320 slots.

| Measure | Result |
| --- | --- |
| Meat-eaters alive at the end | 0 of 8 seeds |
| Kills per run | 1–8 |
| Plants eaten vs meat eaten (seed 14) | 6.9 million vs 735 |
| Founding lineages left | 1 of 32, on every seed |
| Population | about 50 on 8,192 cells, one creature per 160 cells |
| Pigment | drifts at random; nothing hunts by sight |

Neither of the obvious levers rescues carnivores (medians over the 8 seeds):

| Treatment | Kills | Meat-eaters alive |
| --- | ---: | ---: |
| Current rules | 4 | 0/8 |
| `--adaptive-hunts` | 93 | 0/8 |
| 64 instead of 24 soil nutrients | 3 | 0/8 |
| Both | 78 | 0/8 |

Two-parent breeding with nests, guarding, and provisioning is stable in the
same world (0 extinctions; 89% of young fledged on seed 14). It changes none of
the above.

## Why: structural gaps

1. **Behavior is fixed formulas.** Each tick `World::update_creature` scores
   the four neighboring cells for food, crowding, habitat match, and the pull
   of a mate or nest. It then forages, hunts, or moves by fixed rules, and the
   genes only tune the weights. Nothing outside the formulas can evolve: no
   herding, ambush, flight strategy, or territory.
2. **Senses are minimal.** Most decisions see one cell away. Prey is found by
   bumping into it, which at one creature per 160 cells is rare.
3. **Meat does not pay.** A grazer with digestion 15 gains about 7 energy per
   bite from plants that are rarely scarce. A kill returns about 100 energy,
   but one diet gene trades plant digestion directly against meat digestion,
   so a capable hunter is a poor grazer and starves between kills.
4. **Fights have no reason.** Attacks exist only as predation. There is no
   competition for mates, food patches, or space, so nothing selects for
   weapons or armor.
5. **There is one medium.** The biomes are moisture levels on the same
   ground, and moving costs the same everywhere. Nothing separates water,
   land, and air.
6. **Body plans are thin.** Size, pigment, pattern, and limbs, with no body
   budget across organs.
7. **There are no species.** Any two creatures can breed.
8. **There is no life history.** No aging, juvenile stage, or eggs.

## Roadmap

| Phase | Change | What should emerge |
| --- | --- | --- |
| 1. Brain and senses | An evolved integer network replaces the decision formulas. Creatures sense food, creatures, and signals in four directions. | Chasing, fleeing, grouping, ambush |
| 2. Trophic economics | Prey energy moves up the food chain, plant and meat digestion become separate abilities with upkeep, and carrying capacity rises. | A lasting food chain of grazers, hunters, and scavengers |
| 3. Media and body plans | Deep water and an air layer become real media. A body budget covers legs, fins, wings, eyes, teeth, and armor. | Aquatic, land, flying, and amphibious kinds; arms races |
| 4. Society and species | Two-parent breeding and nests on, mate choice that reads looks and signals, compatibility-limited mating, and species detection. | Herds, displays, fights over mates, real species |
| 5. Life history | Aging, juvenile stages, and eggs that predators can eat. | Fast and slow life strategies |

Phases 1 and 2 land together. A brain in today's economy would only learn to
graze better. The visual work (births, nests, fights, herds, swimmers, fliers,
a camera that finds the action) runs alongside each phase.

Every phase follows the existing discipline: matched seeds with a neutral
control, a new checkpoint version, native baselines, and ESP32 fingerprints
that match native.

## Phase 1: brain and senses

### Principles

- Only physics is fixed: what a creature can perceive, what actions exist,
  and what each action costs. Choosing among actions is the brain's job.
- Moving into an occupied cell is an attack, so fighting needs no special
  output. Whether it is worth it is decided by energy.
- Integer arithmetic only. The network uses int8 weights and int16 or int32
  accumulators, so native, ESP32, and SAMD51 stay bit-identical.
- Directions are treated symmetrically. One shared scorer rates each of the
  four directions from that direction's features. This builds in symmetry,
  not behavior, and cuts the weights by a factor of four.

### Senses

A creature scans a diamond of radius 6 (85 cells). In Phase 3 the radius
becomes the eye gene. For each direction it sums what lies in that quadrant,
weighting each cell by closeness (7 − distance). Each quadrant owns one of its
two bounding diagonals, so all four hold 21 cells and a quarter turn maps one
onto the next. The timing prototype gave both diagonals to east and west (24
cells against 18), which would have built a heading into the shared scorer.

| Per direction (×4) | Meaning |
| --- | --- |
| plants | foliage and fruit, weighted by the creature's plant digestion |
| meat | carcass units |
| nearest creature: closeness | 0 if none |
| nearest creature: relative size | its body size minus mine |
| nearest creature: kinship | 127 for my parent or child, 96 for a sibling or my last mate, 48 for a stranger with my heritable marker, else 0 |
| crowd | count of creatures, closeness-weighted |
| signal | signal flashes seen, closeness-weighted |
| habitat | how much better the ground there matches the creature's moisture preference than the ground underfoot |
| wall | closeness of the world's edge, which otherwise looks like empty ground |
| step | plant food on the next cell that way, scaled like food underfoot |

The step sense was added after the first results. The quadrant sums blur the
one cell a step reaches: it carries 6 of a quadrant's 56 closeness weights. The
fixed rules compare the four neighboring cells directly, and brains without
that sight grazed too poorly to found a world reliably (see Results).

Every input is scaled to an int8. The scales come from the observatory world
(seeds 1, 14, 42, 2026 to tick 30,000): the plant sum has a median of 63 and a
99th percentile of 341 before scaling, and is halved; meat and crowd are zero
in 97% and 71% of quadrants.

Camouflage acts on perception, not on a hunt formula. A creature whose pigment
and pattern match its ground is detected only within a shorter range. This
makes pigment matter because brains look, rather than because a rule
penalizes a chance.

Self inputs (10): energy, carried nutrients, age (until maturity), food
underfoot, carcass underfoot, habitat mismatch, crowding of the four neighbors,
damage taken last tick, and two memory values fed back from the previous tick.
`World` does not track age, damage, or memory yet, so they read zero.

### Network

| Part | Inputs | Hidden | Outputs | Bytes (int8 weights + biases) |
| --- | ---: | ---: | ---: | ---: |
| Direction scorer, shared across 4 directions | 10 direction + 10 self = 20 | 6 | 1 move score | 20×6 + 6 + 6 + 1 = 133 |
| Global head | 10 self + 10 averaged = 20 | 4 | stay, sprint, breed, signal, memory ×2 = 6 | 20×4 + 4 + 4×6 + 6 = 114 |
| **Total** | | | | **247** |

- The creature picks the highest-scoring of {stay, four directions}. Equal
  move scores keep the direction examined first, and that direction rotates by
  a hash of the creature's ID and the tick, so ties favor no heading. Staying
  wins a tie with the best move.
- The self inputs' share of each scorer unit is the same for all four
  directions and is summed once. That removes 180 of 608 multiply-adds with
  identical results.
- Sprint takes a second step at double cost.
- It eats at the cell where it ends the tick, choosing the food its digestion
  values most, as today.
- Breed buds asexually when energy allows. With Phase 4's sexual mode, it
  courts instead: two adjacent courting creatures that are compatible mate.
- Signal sets a flash that others sense next tick.
- Hidden units use a clipped linear activation in integer arithmetic.

Cost per creature per tick is about 430 multiply-adds plus the 84-cell
sensing scan. See the measured figures below.

### Measured cost

The ESP32 self-test has a `BRAIN` case that times a prototype: the full scan
and network, with random int8 weights, for every creature of the observatory
world from tick 2,000. The prototype is the 228-byte layout above, with the
scan cells in a precomputed offset table and each brain copied out of memory
with 32-bit loads once per decision. Its decisions are bit-identical between
native and the ESP32, by checksum, and the world fingerprints are unchanged.

| Per creature decision | Native | ESP32 |
| --- | ---: | ---: |
| Sensing scan | 0.16 µs | 19.4 µs |
| Network, brains in normal heap | 0.19 µs | 38.8 µs |
| Network, brains in 32-bit-only instruction RAM | — | 40.5 µs |
| Scan and network | 0.34 µs | 56.9 µs |
| **At 320 creatures, per tick** | 0.11 ms | **18.2 ms** |

The table above is the earlier prototype (7 direction and 11 self inputs, 228
bytes, a 16-bit occupancy array). `core/brain.cpp` has not been timed on the
ESP32. Natively, per decision:

| | Prototype | `core/brain.cpp` |
| --- | ---: | ---: |
| Sensing scan | 0.16 µs | 0.29 µs |
| Network | 0.19 µs | 0.14 µs |
| Scan and network | 0.34 µs | 0.43 µs |

The prototype column is the figure recorded above, from another machine. The
network is faster with three more inputs per direction. The scan is slower: it
adds the fruit, habitat, and signal senses and reads the packed 9-bit
occupancy grid behind a 1 KB bitmap built once per tick, where the prototype
used a 16 KB array the board has no room for. The `BRAIN` checksums changed
with the senses, so the ESP32 must be re-run against the new native lines
before any figure here is trusted for the board.

- **Rate on one core:** with today's step of about 6.5 ms at 100 creatures, a
  full world of 320 brains would run at roughly 40 ticks/s. That is 5× the
  display's 8 ticks/s, but it leaves little room for fast-forward.
- **Not the bottleneck:** running the kernel from instruction RAM changed
  nothing, so flash-cache misses are ruled out. The ESP32 needs about 16
  cycles per multiply-add in this C code, against about 70× native speed for
  the rest of the simulation.
- **32-bit-only memory works:** the 72,960-byte brain array fits in it and
  costs 4% in speed.

Levers, in the order they will be tried:

1. **Decide, then act.** Every creature decides from the state at the start
   of the tick, and the moves then resolve in slot order. This is
   deterministic, and it lets the decide phase split across both ESP32 cores
   (core 0 mostly waits on the link), roughly halving brain time.
2. **Cheaper senses.** Summed-area tables do not fit these senses. A
   closeness-weighted quadrant sum needs one table lookup per distance, six
   rectangles of four reads against 21 cells, and the tables cover all 8,192
   cells every tick while the scans cover 84 cells per creature: fewer cells
   below about 200 creatures. Cheaper options, in order: refresh the food and
   habitat sums every few ticks, since plants grow on a slow clock; drop a
   sense that the gates show brains ignore; shrink the radius.
3. **A faster network.** Sixteen cycles per multiply-add is high for a
   240 MHz core at `-O2`; read the generated code first. 16-bit weights avoid
   sign extension, but only in the copy made for each decision: stored that
   way the array would double and fit nowhere. If it is still too slow, the
   hidden layers shrink.

When moves resolve in slot order, the lowest slot wins every contested cell on
every tick. The resolve order should start from a slot that rotates by a hash
of the tick.

### Inheritance and founders

- Each weight has one chance in 128 of a ±1 to ±4 step, clipped to ±127:
  about 1.9 changes per brain (`brain::inherit`). The scalar genes' rate of
  two in eight would change about 59 weights at every birth. In the sexual mode, each neuron's weights are
  inherited from one parent, so neurons stay working units.
- Founders need brains that can survive day one, but hand-writing them would
  hard-code behavior. Instead a native **nursery** evolves founders from
  random weights in the same physics, which native code runs at thousands of
  ticks per second. A fixed pool of evolved founder brains is compiled into
  the firmware. From then on evolution is free. The nursery must not use the
  gate seeds, or the gates would test what the founders were bred on.
- Founder brains depend on the exact inputs and their scales. A change to
  either means a new nursery run and a new checkpoint version. Inputs added
  later with zero weights leave old brains' behavior unchanged.
- **Neutral control:** brains that inherit and mutate but whose outputs are
  replaced by a seeded random choice. This separates the brain's effect from
  that of the rest of the genome.
- **Shuffled control:** each newborn takes the brain of a random living
  creature instead of its parent's. Random actions will probably starve, and a
  smaller population confounds the pigment and lineage gates. Shuffling keeps
  the population competent and removes only the heritability of behavior.

### Memory on the board

- The ESP32 has about 157 KB of normal heap at boot, but the largest single
  block is 110 KB and the `World` is already 86 KB.
- It also has 81 KB of instruction RAM that allows only 32-bit access and is
  currently unused.
- 247 bytes pad to 62 words; × 320 slots = 79,360 bytes, so brains live in
  their own array in that 32-bit-only memory. Weights are packed four to a
  word and read with 32-bit loads, and writes (mutation, restore) use word
  read-modify-write.
- **That memory is too small with Bluetooth on.** The [lab notebook](LAB_NOTEBOOK.md)'s heap table
  records 81,212 bytes of it free without a radio and 34,152 with NimBLE
  advertising. Even without a radio the array leaves under 2 KB of it. With Bluetooth the array must shrink or move. The options,
  cheapest first: tune NimBLE's buffers and measure again; split the array
  between this memory and the normal heap; lower the slot count (256 slots
  need 63,488 bytes); shrink the hidden layers (34 KB allows 106 bytes per
  brain at 320 slots).
- The boot peak has the same problem. The 94 KB restore buffer and the 86 KB
  world do not fit the roughly 122 KB of heap measured with Bluetooth up, so
  streaming restore is needed for Bluetooth with or without brains.
- The checkpoint grows by the same amount, to about 174 KB. Version 18
  checkpoints hold no brains, so they resume only with brains off. The 96 KiB QSPI
  slots become four 256 KiB slots: 1 MiB of the 2 MiB flash.
- Restore must stream into place instead of through a full-size buffer,
  which no longer fits.
- The SAMD51 never holds brains. The render stream gains only per-creature
  state (Phase 1 visuals: signal flash, sprint, damage) of about 1 byte.
- Bluetooth claims part of the heap. Its share is measured before the brain
  array is sized; if it does not fit, the hidden layer shrinks first.

## The brains world as built

`--brains` (`Config::brains`) needs the food web and predation, without
two-parent breeding or adaptive hunts. Only the choice of action changes:
upkeep, eating, the capture roll, and budding use the food web's arithmetic, so
a brains run and a run under the fixed rules differ in who decides.

Each tick, after the environment updates:

1. **Decide.** Every living creature senses the world as it stands and its
   network picks an action. Nothing is changed, so this phase can be split
   across cores (`World::decide_brains` takes a slot range).
2. **Act.** Creatures act in slot order from a start that moves every tick by
   a hash, so no slot always reaches a contested cell first. A move into a free
   cell costs 1, and a sprint's second step 2 more. A move into the edge costs
   1 and goes nowhere. A move into a creature is an attack: it costs 2, the
   food web's capture roll decides it, and a kill moves the attacker in to
   feed. A missed target senses the attack next tick. The creature then eats
   where it stands, as under the fixed rules.
3. **Breed.** A creature whose network asks to breed buds when it has 70
   energy and a full nutrient load, into the cell it just left or a free
   neighbor. The young takes the parent's brain with mutation.

A slot that changes hands between deciding and acting is skipped. Brains and
their state live in one heap block (about 80 KB) that exists only with
`--brains`, so other worlds and the SAMD51 pay one pointer.

Controls: `--neutral-brains` replaces each decision with a seeded random
choice; `--shuffled-brains` gives each newborn the brain of a random living
creature. `--random-founder-brains` skips the bred pool.

### The nursery

Founders with random weights went extinct within 300 ticks on all eight
seeds. `petri_nursery` (run from the 128×64 build, about two minutes) breeds
a pool into `core/founder_brains.hpp`:

- A brain is scored by 600-tick runs in which all 32 founders carry it, so it
  meets every founding body and place: creatures left at the end, births,
  founders' lifetimes, and food eaten. The best 8 of 32 are kept and mutated
  until worlds founded from the kept 8 sustain themselves.
- It selects only for founding a lasting population. Its seeds (7001–7008)
  are not the experiment seeds.
- Four families are bred. Each is proven for 20,000 ticks on the nursery
  seeds and on eight more (7101–7108, since raised to 24) it never met, and the steadiest becomes
  the pool of 8 brains. With the step sense all four families reached
  viability within 40 rounds; three sustained 16 of 16 worlds and one 15.
- Survivors of long runs make poor founders. Brains taken from the living
  after 30,000 ticks went extinct within a few dozen births in every fresh
  world. They fit the bodies and the grazed-down world they evolved with.
  (Measured before the step sense.)
- An earlier score that counted a founder's own feeding bred foragers that
  never chose to breed, with 150 energy and a full nutrient load.
- Without the step sense, 2 of 12 families reached viability. Each alone
  sustained 6 of 8 nursery worlds, and the two mixed sustained 2 of 8.

The pool must be bred again after any change to the senses, the network, or
the brains world's physics.

### Results

Observatory world, seeds 1, 2, 3, 4, 14, 42, 101, 2026, 50,000 ticks (the
gates ask for 100,000):

| | Fixed rules | Brains | Shuffled control | Neutral control |
| --- | ---: | ---: | ---: | ---: |
| Seeds with a population at the end | 8 | 8 | 8 | 0 |
| Population at the end | 40–74 | 35–88 | 34–75 | 0 |
| Births per run | 18,739–20,796 | 12,977–28,537 | 8,804–26,406 | 0–2 |
| Kills per run | 1–8 | 23–1,808 | 2–2,072 | — |
| Attacks per run | 4–37 | 325–36,069 | 41–41,758 | — |
| Meat eaten per run | 465–1,054 | 195–10,359 | 32–11,862 | — |
| Plants eaten per run | 6.6–7.4 million | 6.6–7.4 million | 6.4–7.5 million | — |
| Creatures eating mostly meat | 0 | 0 | 0 | — |
| Founding lineages left | 1 | 1 | 1 | — |
| Native speed, ticks/s | 25,300–26,800 | 17,300–19,200 | 17,800–21,200 | — |

- **Founding is reliable.** Every world lasts. Before the step sense, four of
  the eight went extinct, three of them within 38 births: in each world 20 to
  28 of the 32 founders starved in the first 50 ticks, and the 2 to 8 left
  either recovered or did not.
- **Hunting varies by world.** Median kills are about 120 against 4 under the
  fixed rules, and two worlds pass 1,500. About one attack in twenty lands.
  Meat stays under 0.2% of intake, and plant digestion sits at its maximum of
  15 in every world. Nothing lives on meat: that is Phase 2's economics.
- **Inheritance of behavior is not shown.** The shuffled control lasts as
  well, with kills in the same range. Brains have more births on five of the
  eight seeds. Over 50,000 ticks the founders' brains explain the result.
- **Random action cannot live.** The neutral control is extinct within two
  births on every seed, so it cannot be a baseline for the pigment or lineage
  gates. The shuffled control can.
- The carrying capacity did not move: about 50 creatures either way.

Against the gates: extinctions pass (0 of 8). Meat-eaters (0 of 8 seeds) and
founding lineages (1) fail, as under the fixed rules. Pigment and clustering
are not measured yet. The ESP32 gates wait for the board.

## Phase 2: trophic economics

`--trophic` (`Config::trophic`) adds these to the brains world. It has its own
founder pool, `core/founder_brains_trophic.hpp` (`petri_nursery --trophic`).

- **Carrying capacity.** The plant growth clock is what binds. With the
  nursery's seeds 7001–7003 at 20,000 ticks, under the fixed rules and with
  brains alike:

  | Plant growth (`--plant-growth`) | Population | Births blocked for want of a slot |
  | ---: | ---: | --- |
  | 1 | 43–71 | none |
  | 2 | 99–166 | none |
  | 4 | 188–298 | up to 980,000 |
  | 8 | 297–320 | millions |

  Soil nutrients at 24 or 64 made no difference at any setting. `--trophic`
  runs at 3, which holds 90–166 creatures with no blocked births.
- **Two digestion abilities.** Meat digestion is its own gene, founded at 5 to
  11 like plant digestion. Each gut costs upkeep with the square of its gene,
  and the smaller gut is charged 25% of its cost (`--second-gut`; see The
  second gut's cost): a plant specialist (15 and 1) pays 0.56 energy a tick,
  the founders' 8 and 8 nothing, and two full guts 0.78.
- **Bites instead of a capture roll.** An attack takes energy out of the
  target: 6 plus twice the attacker's body size less the target's, at least
  2. The attacker keeps meat digestion / 20 of it (5% to 75%) and pays 1 plus
  a quarter of the target's size. The target dies when its energy is gone,
  and otherwise senses the damage and can flee.
- **Bodies hold matter by size.** A body holds its body size in soil
  nutrients. A birth takes the young's size from the parent, and a death
  returns it as carcass, so nutrients are conserved (tested).

### Phase 2 results

Observatory world, the eight experiment seeds, 50,000 ticks, with the second
gut at 100% of its cost, as first built. "Brains at
growth 3" is Phase 1 with the same plant growth, to separate the physics from
the larger population.

| | Brains at growth 3 | Trophic | Trophic, shuffled control |
| --- | ---: | ---: | ---: |
| Seeds with a population at the end | 8 | 8 | 8 |
| Population at the end | 89–206 | 90–166 | 70–159 |
| Births per run | 23,553–47,143 | 43,355–53,467 | 20,726–46,816 |
| Kills per run | 177–4,312 | 13,262–19,361 | 8,696–13,896 |
| Attacks per run | 3,825–85,189 | 108,193–145,097 | 77,858–118,583 |
| Mean meat gene at the end | — | 1.8–3.1 | 1.6–3.3 |
| Mean plant digestion at the end | 15.0 | 14.9–15.0 | 15.0 |
| Creatures eating mostly meat | 0 | 0 | 0 |
| Founding lineages left | 1 | 1 | 1 |
| Blocked births | 0 | 0 | 0 |
| Native speed, ticks/s | 8,300–9,900 | 9,200–11,200 | 9,100–11,100 |

- **Fights now have a reason, and it is not food.** About a third of all
  deaths are kills, yet the meat gene falls from the founders' 5–11 to about
  2, where an attacker keeps a tenth of what it bites out. Creatures attack
  neighbors to clear competitors off the plants.
- **Bodies shrink.** On seed 14 mean body size fell to 3.9 and limbs to 1.7,
  the cheap end of both.
- **Inheritance of behavior starts to show.** Trophic worlds have more births
  than their shuffled controls on 7 of 8 seeds (mean 48,500 against 32,400)
  and more kills on all 8. It is still one comparison at 50,000 ticks.
- **Nothing lives on meat, and the arithmetic says why.** Bites move about 32
  energy a tick through the whole world (400,000 flesh units of 4 energy in
  50,000 ticks). A lineage with full meat digestion would keep 24 of that.
  A creature spends about 4 a tick, so the world's entire flow of flesh feeds
  about six full-time meat-eaters. Six is not a population that lasts.
  Grazing meanwhile pays up to 10 energy a tick at full plant digestion.

**What this means for the gates.** The meat-eater gate (over half of intake
from meat, persisting on 6 of 8 seeds) is probably out of reach with 320
slots. A predator level of 30 creatures needs five times the flesh flow, and
so about five times the grazers. The choices:

1. Count omnivores. Lower the cost of a second gut so creatures that graze
   and bite can pay for both. Seed 2 already held nine with both genes at 9
   or more.
2. Make prey worth more: larger energy stores, or a carcass that holds the
   energy a kill leaves.
3. Make grazing pay less than 10 a tick, so flesh competes.
4. Raise the slot count, which the ESP32's memory does not allow today.

### The second gut's cost

`--second-gut` (`Config::second_gut`) charges the smaller of a creature's two
guts a percentage of its upkeep. Same eight seeds, 50,000 ticks, with the
founders bred at 100%:

| Second gut costs | 100% | 25% | 0% |
| --- | ---: | ---: | ---: |
| Mean meat gene at the end | 1.8–3.1 | 3.0–12.0 | 13.2–14.2 |
| Worlds where it ends above 10 | 0 | 3 | 8 |
| Kills per run | 13,262–19,361 | 15,478–29,149 | 23,636–43,326 |
| Flesh bitten per run, units | 307,000–471,000 | 418,000–809,000 | 634,000–1,285,000 |
| Share of energy from flesh, about | under 1% | 1–9% | 7–15% |
| Creatures with over half their energy from flesh | 0–2 | 0–3 | 0–3 |
| Population at the end | 90–166 | 72–153 | 77–164 |

- The prediction held: at full price a meat gut costs four times what it
  earns and the gene collapses; free, it rises to its ceiling in every world.
  At 25%, near break-even, worlds go either way.
- A working meat gut makes biting pay, so attacks, kills, and flesh eaten all
  roughly double. Flesh becomes a real tenth of the energy budget.
- It makes omnivores, not predators. Plant digestion stays at 15 everywhere
  and at most three creatures in a world get half their energy from flesh.
  Each world still holds one kind of creature.

25% is now the default, and the trophic founders were bred again for it. The
nursery proves each family on 32 worlds (its 8 seeds and 24 it never met);
all four families sustained 32 of 32. With those founders, the eight
experiment seeds at 50,000 ticks:

| | Trophic at 25% | Shuffled control |
| --- | ---: | ---: |
| Seeds with a population at the end | 7 | 8 |
| Population at the end, where alive | 75–146 | 71–146 |
| Births per run, where alive | 32,067–82,530 | 12,127–16,546 |
| Kills per run, where alive | 1,472–23,033 | 7,144–11,403 |
| Mean meat gene at the end | 4.5–12.8 | 10.4–13.1 |
| Worlds where it ends above 9 | 5 | 8 |
| Founding lineages left | 1 | 1 |

- **Seed 4 went extinct at founding**: 83 births, 67 of them killed. Over 200
  fresh seeds (8001–8200) to 5,000 ticks, 3 trophic worlds failed this way
  (1.5%) and 1 Phase 1 brains world (0.5%), each within 83 births. The
  extinction gate asks for none, and proving a family on 32 worlds does not
  screen out a failure this rare.
- **Worlds take two courses.** Five end with a working meat gut and 13,675 to
  23,033 kills. Seeds 1 and 3 keep the gene near 4.5, with about 1,500 kills
  and the most births.
- **Inheritance of behavior shows clearly.** Every surviving trophic world has
  at least twice its shuffled control's births. The controls all end with a
  high meat gene and a narrow range of kills: with behavior shuffled, the
  worlds do not diverge.

## Kinship and the social world

Before this, a third of all kills in the trophic world were between parent
and child (6,001 of 17,461 on seed 14 at 20,000 ticks; one birth in five ended
that way). Young bud beside the parent with 40 energy, the nearest and weakest
target, and brains could not tell kin from strangers: the resemblance sense
compared genes that are the same in every creature, and read 17,441 of the
17,461 victims as alike.

- **Kinship sense.** The resemblance input is now kinship (see Senses). It
  is perception only. Nothing forbids an attack on kin.
- **Two-parent breeding under brains.** With `--sexual`, two adjacent
  creatures that both choose to breed, and are able to, mate. Each of the
  young's hidden units comes whole from one parent (`brain::combine`), then
  the brain mutates. `--nests --guarding --provisioning` work as under the
  fixed rules: the young sits in the nest for 12 ticks, a parent standing
  beside it guards it and takes three quarters of a bite meant for it, and a
  parent with young sets part of each meal aside and hands it over when
  beside the nest. Returning to the nest is the brain's choice.
- **The social world** is the trophic world with all four. It has its own
  founders, `core/founder_brains_social.hpp` (`petri_nursery --social`); all
  four families sustained 32 of 32 nursery worlds.

Eight experiment seeds, 50,000 ticks, all founders bred again with the
kinship sense:

| | Trophic with kinship | Social | Social, shuffled control |
| --- | ---: | ---: | ---: |
| Seeds with a population at the end | 8 | 8 | 8 |
| Population at the end | 66–143 | 75–131 | 69–130 |
| Births per run | 41,340–88,654 | 10,535–14,054 | 9,062–13,527 |
| Kills per run | 1,427–33,575 | 3,258–4,491 | 2,199–4,549 |
| Kills between parent and child, share of kills | 2–23%, median 5% | 10–20%, median 12% | 8–18% |
| Kills between parent and child, per 100 births | 0.04–5 | 3–7 | 3–6 |
| Mean meat gene at the end | 2.6–13.7 | 8.9–12.5 | 8.2–14.0 |
| Creatures with over half their energy from flesh | 0–4 | 0–1 | 0 |
| Young fledged / starved in the nest / killed in the nest | — | 81% / 12% / 7% | 82% / 11% / 7% |
| Founding lineages left | 1 | 1–2, median 1.5 | 1–3, median 2 |

- **Parents mostly stopped killing their young** in the budding world: from
  a third of kills to a median of 5%, and from one birth in five to under one
  in twenty. Seed 4, which died of exactly this at founding, now lasts.
- **Killing between strangers did not stop.** Four budding worlds still see
  28,000 to 34,000 kills, nearly all of creatures wearing the same marker.
- **The social world is calmer and steadier.** Kills fall to 3,000–4,500 in
  every world, with a fifth as many births. Four fifths of young fledge.
- **More than one founding lineage survives** for the first time: 2 in five
  of the eight social worlds. A creature's lineage is its first parent's, so
  this counts less strictly than under budding.
- **Inheritance of behavior does not show in the social world.** The shuffled
  control matches it on every measure.
- **Still nothing lives on meat.** The meat gut is kept (gene 9–12), but the
  flow of flesh is the same small share of energy as before.

## On the board

What the core now provides, tested natively:

- **Checkpoint version 19.** A world with brains, or with plant growth other
  than 1, writes version 19: a version 18 checkpoint with four settings bytes
  after the header, then a meat gene per slot and a 258-byte record for each
  living creature's brain and state. Its size follows the population:
  119,551 bytes at tick 3,000 of trophic seed 14, and 177,343 at most. Other
  worlds still write version 18 byte for byte. A trophic run saved at tick
  3,000 and resumed for 3,000 more ends on the fingerprint of an unbroken
  6,000-tick run.
- **Streaming load.** `World::load_checkpoint_stream` decodes versions 15 to
  19 as the bytes arrive, straight into the world, checking the checksum at
  the end. The ESP32 then needs no restore buffer, which at 177 KB it could
  not hold.
- **Placing the weights.** `BrainStore::set_weight_memory` lets the board put
  the 79,360-byte block of weights in memory that allows only 32-bit access.
  The block is read and written a word at a time. The rest of a brains
  world's state is about 8 KB of ordinary heap beside the 86,120-byte `World`.

The firmware uses them, and the trophic world runs on the MatrixPortal M4
(November 18, 2026):

- **ESP32 world app** (`esp32/main/world_app.cpp`): ran the trophic world at
  plant growth 3 for the measurements below, and now runs the social world; restores by feeding link chunks to the streaming loader
  through an 8 KB stream buffer, with no restore buffer; reports each
  checkpoint's real size; accepts versions 15 to 19.
- **SAMD51 display** (`firmware/checkpoint_store.hpp`): four QSPI slots of
  256 KiB instead of 96 KiB. Only the first old slot sits where a new one
  does, and a board saved by earlier firmware restored that old world once,
  through the streaming loader. A fresh start needs that slot corrupted
  (`X` over USB) and a restart.
- **Weights in eight blocks.** One 79,360-byte block did not fit the world
  app's instruction RAM (76,424 bytes free; the self-test app has 81,212) and
  fell into the heap, leaving 5 KB of it. The weights are now eight blocks of
  40 slots. Seven sit in instruction RAM and one in the heap.

Measured on the board, trophic seed 14:

| | Result |
| --- | --- |
| Self-test fingerprints, 4 worlds at 4 ticks each | all 16 match native |
| Self-test brain checksums (scan, network, full) | match native |
| Trophic world fingerprint at ticks 256, 385, 457, 834, 870 | match native |
| Save to QSPI | 1.7–1.9 s, 2 of 2 succeeded |
| Restart and restore | resumed; fingerprint at tick 850 matches an unbroken native run |
| Longest step, 60 to 160 creatures | 8.9–9.1 ms, so about 110 ticks/s against the gate of 40 |
| Display | 30 fps at 8 ticks/s, the display's top speed |
| ESP32 heap free while running | 72,628 bytes, steady |
| Brain decision on the ESP32 | 56.1 µs: 33.2 scan, 24.7 network (26.5 from instruction RAM) |
| Fixed-rules observatory world on the ESP32 | 200 ticks/s |
| Social world (seed 14) fingerprint at ticks 256, 267, 891, 915 | match native |
| Social world: save, restart, and restore | resumed; fingerprint at tick 886 matches native |
| Social world: ESP32 heap free / longest step at 19 creatures | 72,116 bytes / 5.3 ms |

The social world starts small on seed 14: 15 creatures at tick 267, 19 at
tick 900, 49 at tick 5,000, and 99 at tick 20,000.

- **Building and flashing on Windows.** Adafruit's ARM compiler for Windows
  is a 32-bit program and ran out of memory evaluating the 128×64 terrain
  tables at compile time. ARM builds now read them from the generated
  `core/watershed_table.hpp` (`tools/gen/watershed_table.cpp`), and native
  builds fail to compile if the table and the formula disagree.
  `tools/board/flash_uf2.py` is a portable twin of `flash_uf2.sh`. The
  capture script's RTS pulse does not reach the bridge through Windows:
  restart the ESP32 with esptool and capture with `--no-reset`.

Not done:

1. A soak test, and saves under a full population (up to 177 KB).
2. **Bluetooth** does not fit beside brains as things stand (see Memory on
   the board). The board runs without the phone controller.
3. The renderer draws the brains world with what it already knows. Bites
   that do not kill, signals, and sprints are not shown.

## Experiments and gates

Matched seeds 1, 2, 3, 4, 14, 42, 101, 2026 at 100,000 ticks, compared with
the current rules and with the neutral-brain control:

| Gate | Threshold |
| --- | --- |
| Meat-eaters (over half of intake from meat) persist through the final 10,000 ticks | ≥ 6 of 8 seeds |
| Founding lineages left | ≥ 2 median |
| Extinctions / blocked births | 0 / rare |
| Pigment matched to background | above the neutral control |
| Clustering (mean nearest-neighbor distance vs random placement) | reported, not gated |
| ESP32 speed for the observatory world | ≥ 40 ticks/s |
| Fingerprints, native vs ESP32 | identical |

A phase ships when the gates pass. The experiments log records each run's
command and result, as it does today.

## Next steps

1. ~~Benchmark the network and the sensing scan natively and on the ESP32,
   including 32-bit-only memory access.~~ Done; see Measured cost.
2. ~~Fix the senses and the network in one place with tests.~~ Done:
   `core/brain.cpp` and `tests/brain_test.cpp`.
3. On the board: re-run the self-test for the new `BRAIN` lines, and measure
   the heap and 32-bit-only memory with Bluetooth up in the world app. Then
   choose where the brain array lives (see Memory on the board).
4. Measure the step with no creatures, to split today's 4.1 ms into
   environment and creature cost before trusting the 40 ticks/s gate at
   150–250 creatures.
5. ~~Build the Phase 1 world behind a `--brains` flag, with decide-then-act,
   keeping the current rules intact for comparison.~~ Done.
6. ~~Build the nursery and evolve founder brains natively.~~ Done; see Results.
7. ~~Make founding reliable.~~ Done with the step sense: 8 of 8 worlds last.
8. ~~Phase 2 economics.~~ Built behind `--trophic`; see Phase 2 results.
   Nothing lives on meat. Decide which of the four choices there to take
   before running the gates at 100,000 ticks.
9. ~~Put brains in the checkpoint.~~ Done in the core: version 19, a
   streaming loader, and a hook that places the weights. The firmware does not
   use them yet: see On the board.
