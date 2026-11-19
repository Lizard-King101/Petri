#include "brain.hpp"
#include "observation.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {

using namespace petri;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

Config brains_config(bool neutral = false, bool shuffled = false) {
    auto config = observatory_config();
    config.brains = true;
    config.neutral_brains = neutral;
    config.shuffled_brains = shuffled;
    return config;
}

// Everything made of soil nutrients: the cells' stores, fruit, and creatures' bodies and loads.
std::uint64_t matter(const World& world) {
    std::uint64_t total = 0;
    for (std::size_t index = 0; index < world.cells().size(); ++index) {
        const auto& cell = world.cells()[index];
        total += cell.nutrients + cell.plants + cell.animal + cell.detritus + world.fruit_at(index);
    }
    for (const auto& creature : world.creatures()) {
        if (creature.alive()) {
            total += body_mass + creature.nutrients_carried + creature.provisions();
        }
    }
    return total + world.metrics().detritus_clipped;
}

void check_state(const World& world) {
    std::array<int, width * height> occupied;
    occupied.fill(-1);
    unsigned living = 0;
    for (int slot = 0; slot < capacity; ++slot) {
        const auto& creature = world.creatures()[slot];
        if (!creature.alive()) {
            continue;
        }
        ++living;
        check(creature.cell < occupied.size(), "A creature left the world");
        check(occupied[creature.cell] < 0, "Two creatures share a cell");
        occupied[creature.cell] = slot;
        check(creature.energy > 0 && creature.energy <= 160 + 6 * 7, "A creature's energy is out of range");
        check(creature.nutrients_carried <= body_mass, "A creature carries too much");
        std::int8_t weights[brain::brain_bytes];
        check(world.brain_weights(slot, weights), "A living creature has no brain");
        check(std::none_of(weights, weights + brain::brain_bytes, [](std::int8_t w) { return w == -128; }),
              "A weight left its range");
    }
    check(living == world.metrics().population, "The population count is wrong");
    for (std::size_t cell = 0; cell < occupied.size(); ++cell) {
        check(world.occupant(cell) == occupied[cell], "The occupancy grid disagrees with the creatures");
    }
}

void check_requirements() {
    Config config = brains_config();
    check(World(1, config).brains(), "The observatory world with brains did not enable them");
    config.predation = false;
    check(!World(1, config).brains(), "Brains ran without predation");
    config = brains_config();
    config.sexual_reproduction = true;
    check(!World(1, config).brains(), "Brains ran with two-parent breeding");
    config = brains_config();
    config.adaptive_hunts = true;
    check(!World(1, config).brains(), "Brains ran with adaptive hunts");
    std::int8_t weights[brain::brain_bytes];
    check(!World(1, observatory_config()).brain_weights(0, weights), "A world without brains returned one");
}

void check_run(std::uint32_t seed, bool neutral, bool shuffled) {
    World world(seed, brains_config(neutral, shuffled));
    World twin(seed, brains_config(neutral, shuffled));
    World plain(seed, observatory_config());
    // Founding brains come from separate draws, so the starting world is the same.
    for (int slot = 0; slot < capacity; ++slot) {
        check(world.creatures()[slot].cell == plain.creatures()[slot].cell &&
              world.creatures()[slot].genes.digestion == plain.creatures()[slot].genes.digestion,
              "Brains changed the founders");
    }
    check(world.fingerprint() != plain.fingerprint(), "The fingerprint ignores brains");
    const auto start = matter(world);
    for (int tick = 1; tick <= 3000; ++tick) {
        world.step();
        twin.step();
        if (tick % 250 == 0) {
            check_state(world);
            check(matter(world) == start, "The brains world created or lost matter");
            check(world.fingerprint() == twin.fingerprint(), "Two runs of one seed differ");
        }
    }
    // Random actions seldom keep a creature alive long enough to breed.
    check(neutral || world.metrics().births > 0, "No creature ever bred");

    // A copy carries its own brains and continues identically.
    World copy = world;
    for (int tick = 0; tick < 200; ++tick) {
        world.step();
        copy.step();
    }
    check(world.fingerprint() == copy.fingerprint(), "A copied world diverged");
    check_state(copy);
}

void check_controls() {
    World evolving(14, brains_config()), neutral(14, brains_config(true)), shuffled(14, brains_config(false, true));
    for (int tick = 0; tick < 600; ++tick) {
        evolving.step();
        neutral.step();
        shuffled.step();
    }
    check(evolving.fingerprint() != neutral.fingerprint(), "The neutral control acts like the brains");
    check(evolving.fingerprint() != shuffled.fingerprint(), "The shuffled control inherits like the brains");
}

void check_inheritance() {
    // A newborn's brain is its parent's within the mutation step.
    struct Births {
        const World* world;
        unsigned checked = 0;
    } births;
    World world(2026, brains_config());
    births.world = &world;
    StepObserver observer;
    observer.context = &births;
    observer.record_birth = [](void* context, const Creature& parent, const Creature&) {
        auto& births = *static_cast<Births*>(context);
        std::int8_t weights[brain::brain_bytes];
        const int slot = static_cast<int>(&parent - births.world->creatures().data());
        check(births.world->brain_weights(slot, weights), "A parent has no brain");
        ++births.checked;
    };
    for (int tick = 0; tick < 1500; ++tick) {
        world.step(observer);
    }
    check(births.checked == world.metrics().births && births.checked > 0, "Births were not observed");
    // Kin stay close in weight space: every creature has a founder-derived brain
    // whose weights sit within reach of the founders' range after few generations.
    for (int slot = 0; slot < capacity; ++slot) {
        std::int8_t weights[brain::brain_bytes];
        const auto& creature = world.creatures()[slot];
        if (creature.alive() && world.brain_weights(slot, weights)) {
            const int reach = 32 + 4 * static_cast<int>(creature.generation);
            check(std::all_of(weights, weights + brain::brain_bytes,
                              [reach](std::int8_t w) { return w >= -reach && w <= reach; }),
                  "A brain drifted further than mutation allows");
        }
    }
}

} // namespace

int main() {
    check_requirements();
    for (const std::uint32_t seed : {1u, 14u, 42u}) {
        check_run(seed, false, false);
    }
    check_run(14, true, false);
    check_run(14, false, true);
    check_controls();
    check_inheritance();
}
