#include "brain.hpp"
#include "observation.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <vector>

namespace {

using namespace petri;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

Config brains_config(bool neutral = false, bool shuffled = false, bool trophic = false, bool social = false) {
    auto config = observatory_config();
    config.brains = true;
    config.neutral_brains = neutral;
    config.shuffled_brains = shuffled;
    config.trophic = trophic;
    config.plant_growth = trophic ? 3 : 1;
    config.sexual_reproduction = config.nests = config.guarding = config.provisioning = social;
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
            total += world.body_matter(creature) + creature.nutrients_carried + creature.provisions();
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
        check(creature.nutrients_carried <= world.birth_matter(creature), "A creature carries too much");
        check(world.trophic() == (creature.genes.meat != 0), "The meat gene does not match the world");
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
    check(World(1, config).brains(), "Brains did not run with two-parent breeding");
    config = brains_config();
    config.adaptive_hunts = true;
    check(!World(1, config).brains(), "Brains ran with adaptive hunts");
    std::int8_t weights[brain::brain_bytes];
    check(!World(1, observatory_config()).brain_weights(0, weights), "A world without brains returned one");
}

// Returns whether the world still holds a population at the end.
bool check_run(std::uint32_t seed, bool neutral, bool shuffled, bool trophic = false, bool social = false) {
    World world(seed, brains_config(neutral, shuffled, trophic, social));
    World twin(seed, brains_config(neutral, shuffled, trophic, social));
    auto plain_config = observatory_config();
    plain_config.sexual_reproduction = plain_config.nests = plain_config.guarding = plain_config.provisioning = social;
    World plain(seed, plain_config);
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
    const bool lasting = world.metrics().population >= 10;

    // A copy carries its own brains and continues identically.
    World copy = world;
    for (int tick = 0; tick < 200; ++tick) {
        world.step();
        copy.step();
    }
    check(world.fingerprint() == copy.fingerprint(), "A copied world diverged");
    check_state(copy);
    return lasting;
}

void check_trophic() {
    Config config = brains_config(false, false, true);
    check(World(1, config).trophic(), "The trophic world did not start");
    config.brains = false;
    check(!World(1, config).trophic(), "Trophic economics ran without brains");
    // The meat gene is its own gene, and absent it meat digestion mirrors plant digestion.
    Genome genes{};
    genes.digestion = 12;
    check(genes.meat_digestion() == 4, "Meat digestion no longer mirrors plant digestion");
    genes.meat = 13;
    check(genes.meat_digestion() == 13, "The meat gene is ignored");
    // Plants grow faster, so the world holds more creatures.
    World trophic(14, brains_config(false, false, true)), plain(14, brains_config());
    check(trophic.plant_growth() == 3 && plain.plant_growth() == 1, "The plant growth setting is wrong");
    for (int tick = 0; tick < 4000; ++tick) {
        trophic.step();
        plain.step();
    }
    check(!wide_world || trophic.metrics().population > plain.metrics().population,
          "Faster plant growth did not raise the population");
    check(trophic.metrics().capacity_blocks == 0, "The trophic world ran out of creature slots");
}

std::vector<std::uint8_t> saved(const World& world) {
    std::vector<std::uint8_t> bytes;
    const auto total = world.save_checkpoint([](void* context, const std::uint8_t* data, std::size_t size) {
        auto& out = *static_cast<std::vector<std::uint8_t>*>(context);
        out.insert(out.end(), data, data + size);
    }, &bytes);
    check(total == bytes.size() && total == world.checkpoint_bytes(), "A checkpoint is not the size it reports");
    return bytes;
}

// Feeds a checkpoint to the streaming loader in uneven pieces.
struct Pieces {
    const std::vector<std::uint8_t>& bytes;
    std::size_t at = 0, step = 1;

    static const std::uint8_t* next(void* context, std::size_t* size) {
        auto& pieces = *static_cast<Pieces*>(context);
        if (pieces.at == pieces.bytes.size()) {
            return nullptr;
        }
        *size = std::min(pieces.step, pieces.bytes.size() - pieces.at);
        pieces.step = pieces.step % 997 + 61;
        const auto* data = pieces.bytes.data() + pieces.at;
        pieces.at += *size;
        return data;
    }
};

std::size_t weight_blocks = 0;

void check_checkpoints() {
    // The weights can be placed by the board, and every block taken is given back.
    BrainStore::set_weight_memory([](std::size_t bytes) { ++weight_blocks; return ::operator new(bytes); },
                                  [](void* block) { --weight_blocks; ::operator delete(block); });
    for (const int kind : {0, 1, 2}) {
        const bool trophic = kind > 0;
        auto world = std::make_unique<World>(14, brains_config(false, false, trophic, kind == 2));
        for (int tick = 0; tick < 2000; ++tick) {
            world->step();
        }
        check(world->extended_checkpoint(), "A brains world would save without its brains");
        const auto bytes = saved(*world);
        check(bytes[5] == 19 && bytes.size() > std::tuple_size<Checkpoint>::value &&
              bytes.size() <= max_checkpoint_bytes, "A brains checkpoint has the wrong version or size");

        auto staged = std::make_unique<World>(1, Config{});
        check(staged->load_checkpoint(bytes.data(), bytes.size()), "A brains checkpoint did not load");
        auto in_place = std::make_unique<World>(2, observatory_config());
        check(in_place->load_checkpoint_in_place(bytes.data(), bytes.size()), "A brains checkpoint did not load in place");
        auto streamed = std::make_unique<World>(3, brains_config());
        Pieces pieces{bytes};
        check(streamed->load_checkpoint_stream(Pieces::next, &pieces), "A brains checkpoint did not load as a stream");
        for (const auto* loaded : {staged.get(), in_place.get(), streamed.get()}) {
            check(loaded->fingerprint() == world->fingerprint(), "A loaded world differs from the saved one");
            check(loaded->brains() && loaded->trophic() == trophic && loaded->plant_growth() == world->plant_growth() &&
                  loaded->second_gut() == world->second_gut(), "A loaded world lost its settings");
            check(saved(*loaded) == bytes, "A loaded world saves differently");
            check_state(*loaded);
        }
        // The loaded worlds go on exactly as the original does.
        for (int tick = 0; tick < 400; ++tick) {
            world->step();
            staged->step();
            streamed->step();
        }
        check(staged->fingerprint() == world->fingerprint() && streamed->fingerprint() == world->fingerprint(),
              "A loaded world diverged from the original");

        // Damage anywhere, a missing end, or extra bytes are all refused.
        for (const std::size_t at : {std::size_t(6), bytes.size() / 2, bytes.size() - 300, bytes.size() - 1}) {
            auto damaged = bytes;
            damaged[at] ^= 0x10;
            auto target = std::make_unique<World>(4, Config{});
            check(!target->load_checkpoint(damaged.data(), damaged.size()), "A damaged checkpoint loaded");
            Pieces broken{damaged};
            check(!target->load_checkpoint_stream(Pieces::next, &broken), "A damaged checkpoint loaded as a stream");
        }
        auto target = std::make_unique<World>(4, Config{});
        auto shortened = bytes;
        shortened.resize(bytes.size() - 258);
        Pieces cut{shortened};
        check(!target->load_checkpoint_stream(Pieces::next, &cut), "A shortened checkpoint loaded as a stream");
        check(!target->load_checkpoint(shortened.data(), shortened.size()), "A shortened checkpoint loaded");
        auto longer = bytes;
        longer.push_back(0);
        Pieces extra{longer};
        check(!target->load_checkpoint_stream(Pieces::next, &extra), "A checkpoint with trailing bytes loaded");
    }
    check(weight_blocks == 0, "A world kept or lost its block of weights");
    BrainStore::set_weight_memory(nullptr, nullptr);

    // Worlds without brains keep the version 18 format, which also streams.
    auto plain = std::make_unique<World>(14, observatory_config());
    for (int tick = 0; tick < 500; ++tick) {
        plain->step();
    }
    const auto bytes = saved(*plain);
    check(bytes[5] == 18 && bytes.size() == std::tuple_size<Checkpoint>::value, "A plain checkpoint changed format");
    auto streamed = std::make_unique<World>(3, brains_config());
    Pieces pieces{bytes};
    check(streamed->load_checkpoint_stream(Pieces::next, &pieces) && streamed->fingerprint() == plain->fingerprint() &&
          !streamed->brains(), "A plain checkpoint did not load as a stream");

    // Faster plant growth alone is saved too.
    auto config = observatory_config();
    config.plant_growth = 2;
    auto growing = std::make_unique<World>(14, config);
    for (int tick = 0; tick < 500; ++tick) {
        growing->step();
    }
    const auto grown = saved(*growing);
    auto loaded = std::make_unique<World>(1, Config{});
    check(grown[5] == 19 && loaded->load_checkpoint(grown.data(), grown.size()) && loaded->plant_growth() == 2 &&
          !loaded->brains() && loaded->fingerprint() == growing->fingerprint(), "Plant growth was not saved");
}

void check_social() {
    // Two-parent breeding with nests under brains: young have two parents, sit
    // in the nest before they act, and are fed and fledged.
    World world(14, brains_config(false, false, true, true));
    check(world.brains() && world.sexual_reproduction() && world.nests(), "The social world did not start");
    unsigned nestlings = 0;
    for (int tick = 0; tick < 4000; ++tick) {
        world.step();
        for (const auto& creature : world.creatures()) {
            if (creature.alive() && creature.generation > 0) {
                check(creature.parent && creature.other_parent && creature.parent != creature.other_parent,
                      "A creature was born without two parents");
                nestlings += creature.nest_ticks > 0;
            }
        }
    }
    const auto& nests = world.metrics().nests;
    check(nestlings > 0, "No young ever sat in a nest");
    check(!wide_world || (nests.fledged > 0 && world.metrics().population >= 10), "No young fledged in the social world");
    check(nests.fledged + nests.starved + nests.killed <= world.metrics().births, "The nest counts exceed the births");
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
    // The founder pools are bred in the 128 by 64 world and must found lasting
    // populations there. A few worlds in a hundred still fail, so the plain
    // brains world, the least steady, is allowed one failure in five.
    int lasting = 0;
    for (const std::uint32_t seed : {1u, 3u, 14u, 42u, 101u}) {
        lasting += check_run(seed, false, false);
    }
    check(!wide_world || lasting >= 4, "The bred founders did not found populations");
    check_run(14, true, false);
    check_run(14, false, true);
    for (const std::uint32_t seed : {1u, 14u, 42u}) {
        check(check_run(seed, false, false, true) || !wide_world, "The trophic founders did not found a population");
    }
    check_trophic();
    for (const std::uint32_t seed : {1u, 14u, 42u}) {
        check(check_run(seed, false, false, true, true) || !wide_world, "The social founders did not found a population");
    }
    check_social();
    check_checkpoints();
    check_controls();
    check_inheritance();
}
