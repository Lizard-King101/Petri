# Emergent life: roadmap and Phase 1–2 design

**Status:** November 18, 2026. Phase 1 runs natively behind `--brains`
(`core/brains_world.cpp`), with founders bred by `apps/nursery.cpp`. It does
worlds last on all eight seeds, but it does not pass the gates: see Results.
Phase 2 is not started, the board has not run it, and checkpoints do not hold
brains.
This document supersedes simulation steps 3 and 4 of
[GRAPHICS_AND_EVOLUTION.md](GRAPHICS_AND_EVOLUTION.md).

The goal is the most advanced evolutionary ecosystem the board can run, with
as little behavior hard-coded as possible. Creatures get senses, a brain, and
a body. Physics and energy decide what works. Words such as "hunter",
"herd", "aquatic", and "flying" describe what we observe in the results, never
flags in the code.

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
| nearest creature: resemblance | the genome similarity used by kin avoidance |
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
- **That memory is too small with Bluetooth on.** The README's heap table
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
  seeds and on eight more (7101–7108) it never met, and the steadiest becomes
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

- **Energy moves up the chain.** A kill transfers a share of the prey's
  stored energy, set by meat digestion, plus a carcass proportional to body
  size, instead of the fixed 8-unit `body_mass`. A birth must then take the
  same size-scaled nutrients from the parent that a death returns, or soil
  nutrients are created or lost.
- **Two digestion abilities.** Plant and meat digestion become independent
  genes, and gut upkeep grows with the square of each. A generalist can
  exist, but specialists are more efficient.
- **Injury.** An attack costs both sides energy in proportion to the other's
  size (and, in Phase 3, weapons and armor). A target can survive and flee,
  so fights are real exchanges rather than a single capture roll.
- **Carrying capacity.** Steady population barely responds to soil nutrients
  (about 50 at both 24 and 64). The limit is in upkeep, crowding, and usable
  habitat. Phase 2 measures which one binds and raises it so the grazer
  population supports a predator level, aiming for 150–250 creatures in 320
  slots.

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
8. Phase 2 economics, so that kills can feed a meat-eater. Then run the
   gates at 100,000 ticks, with pigment match and clustering measured against
   the shuffled control.
9. Put brains in the checkpoint (a new version, streamed restore) and choose
   where the array lives on the ESP32.
