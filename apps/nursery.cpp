// Breeds founder brains for the brains world (docs/EMERGENT_LIFE.md) and
// writes them to core/founder_brains.hpp.
//
// Random brains die within a few hundred ticks. The nursery selects only for
// what a founder needs: starting a population that lasts, from any of the 32
// founding bodies and places, in the observatory world's own physics. A brain
// is scored by short runs in which all 32 founders carry it. The best are kept
// and mutated until worlds founded from them sustain themselves. What the
// creatures then do with their brains is left to evolution in the world.
//
// Several families are bred independently. Each is proven over long runs, on
// the nursery's worlds and on others it never met, and the steadiest becomes
// the pool. Families are not mixed: before the next-cell food sense existed,
// two viable families that each sustained 6 of 8 worlds sustained 2 of 8 together.
//
// The pool is not taken from the survivors of long runs. Those brains fit the
// bodies and the grazed-down world they evolved with, and fail to found a new
// one: in trials, the survivors of 30,000-tick runs went extinct within a few
// dozen births in every fresh world.
//
// The nursery's seeds are never the experiment seeds (1, 2, 3, 4, 14, 42, 101,
// 2026), so the gates test founders on worlds they were not bred in.

#include "brain.hpp"
#include "observation.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace petri;
using Brain = std::array<std::int8_t, brain::brain_bytes>;

constexpr std::array<std::uint32_t, 8> nursery_seeds{7001, 7002, 7003, 7004, 7005, 7006, 7007, 7008};
// A family is proven on these as well, which it never met while it was bred.
constexpr std::array<std::uint32_t, 24> proving_seeds{7101, 7102, 7103, 7104, 7105, 7106, 7107, 7108,
                                                      7109, 7110, 7111, 7112, 7113, 7114, 7115, 7116,
                                                      7117, 7118, 7119, 7120, 7121, 7122, 7123, 7124};
constexpr int families_bred = 4, families_used = 1, family_size = 32, kept = 8, screened_brains = 1024;
constexpr int trial_ticks = 600, trials_per_round = 2, viable_ticks = 4000, proving_ticks = 20000;
constexpr int round_limit = 40;
// A world counts as sustained with this many creatures at the end of a run.
constexpr unsigned sustained_population = 20;

std::uint32_t rng = 0x2f6e2b1u;

std::uint32_t random() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 8;
}

// Set by --trophic: breed for the Phase 2 physics, at the plant growth that world runs with.
// Set by --social: the trophic world with two-parent breeding, nests, guarding, and provisioning.
bool trophic = false, social = false;

Config nursery_config() {
    auto config = observatory_config();
    config.brains = true;
    config.random_founder_brains = true;
    config.trophic = trophic;
    config.plant_growth = trophic ? 3 : 1;
    config.sexual_reproduction = config.nests = config.guarding = config.provisioning = social;
    return config;
}

struct Tally {
    const World* world = nullptr;
    std::uint64_t births = 0, founder_ticks = 0, eaten = 0;
};

// Runs one world whose founder in slot f has pool[(f + offset) % size].
World run(const std::vector<Brain>& pool, std::uint32_t seed, unsigned offset, int ticks, Tally* tally = nullptr) {
    World world(seed, nursery_config());
    for (int slot = 0; slot < founder_count; ++slot) {
        world.set_brain(slot, pool[(slot + offset) % pool.size()].data());
    }
    StepObserver observer;
    if (tally) {
        tally->world = &world;
        observer.context = tally;
        observer.record_birth = [](void* context, const Creature&, const Creature&) {
            ++static_cast<Tally*>(context)->births;
        };
        observer.record_death = [](void* context, const Creature& creature, DeathCause) {
            auto& tally = *static_cast<Tally*>(context);
            tally.eaten += creature.plants_eaten + creature.animal_eaten;
            if (creature.generation == 0) {
                tally.founder_ticks += tally.world->metrics().ticks;
            }
        };
    }
    for (int tick = 0; tick < ticks && world.metrics().population; ++tick) {
        world.step(observer);
    }
    if (tally) {
        tally->world = nullptr;
        for (const auto& creature : world.creatures()) {
            tally->founder_ticks += creature.alive() && creature.generation == 0 ? ticks : 0;
            tally->eaten += creature.alive() ? creature.plants_eaten + creature.animal_eaten : 0;
        }
    }
    return world;
}

// One brain alone in a world, in all 32 founders, so it is tried in every
// founding body and place. The creatures left at the end count most, then
// births. Founders' lifetimes and what was eaten separate brains that cannot yet live.
std::uint64_t trial(const Brain& brain, std::uint32_t seed) {
    Tally tally;
    const auto world = run({brain}, seed, 0, trial_ticks, &tally);
    return 2000ull * world.metrics().population + 600ull * tally.births + tally.founder_ticks + tally.eaten;
}

struct Candidate {
    Brain brain;
    std::uint64_t total = 0;
    unsigned trials = 0;

    std::uint64_t mean() const { return trials ? total / trials : 0; }
};

Brain mutated(const Brain& parent, int passes) {
    Brain child = parent;
    for (int pass = 0; pass < passes; ++pass) {
        Brain next;
        std::uint32_t state = random();
        brain::inherit(next.data(), child.data(), state);
        child = next;
    }
    return child;
}

// How many nursery worlds a mixed pool founds and sustains.
int sustained(const std::vector<Brain>& pool, int ticks) {
    int count = 0;
    for (const auto seed : nursery_seeds) {
        count += run(pool, seed, seed, ticks).metrics().population >= sustained_population;
    }
    if (ticks == proving_ticks) {
        for (const auto seed : proving_seeds) {
            count += run(pool, seed, seed, ticks).metrics().population >= sustained_population;
        }
    }
    return count;
}

// Breeds one family and returns its best brains.
std::vector<Brain> breed(int family) {
    std::vector<Candidate> pool;
    for (int n = 0; n < screened_brains; ++n) {
        Candidate candidate;
        for (auto& weight : candidate.brain) {
            weight = static_cast<std::int8_t>(int(random() % 65) - 32);
        }
        candidate.total = trial(candidate.brain, nursery_seeds[n % nursery_seeds.size()]);
        candidate.trials = 1;
        pool.push_back(candidate);
    }
    const auto better = [](const Candidate& a, const Candidate& b) { return a.mean() > b.mean(); };
    std::stable_sort(pool.begin(), pool.end(), better);
    pool.resize(family_size);

    std::vector<Brain> best;
    for (int round = 0; round < round_limit; ++round) {
        // Every brain meets more seeds each round. The kept brains carry their
        // earlier trials, so one lucky world does not keep a brain for long.
        for (auto& candidate : pool) {
            for (int n = 0; n < trials_per_round; ++n) {
                candidate.total += trial(candidate.brain,
                                         nursery_seeds[(round * trials_per_round + n) % nursery_seeds.size()]);
                ++candidate.trials;
            }
        }
        std::stable_sort(pool.begin(), pool.end(), better);
        best.clear();
        for (int i = 0; i < kept; ++i) {
            best.push_back(pool[i].brain);
        }
        const int worlds = round % 5 == 4 ? sustained(best, viable_ticks) : -1;
        if (worlds >= 0) {
            std::printf("family=%d round=%d best=%llu kept=%llu median=%llu sustained=%d/%zu\n", family, round + 1,
                        (unsigned long long)pool[0].mean(), (unsigned long long)pool[kept - 1].mean(),
                        (unsigned long long)pool[family_size / 2].mean(), worlds, nursery_seeds.size());
            std::fflush(stdout);
        }
        if (worlds * 4 >= int(nursery_seeds.size()) * 3) {
            return best;
        }
        pool.resize(kept);
        while (int(pool.size()) < family_size) {
            pool.push_back({mutated(pool[random() % kept].brain, 1 + random() % 4)});
        }
    }
    std::printf("family=%d stopped at the round limit\n", family);
    return {};
}

void write(const std::vector<Brain>& pool, const char* path) {
    std::ofstream out(path, std::ios::binary);
    const char* name = social ? "founder_pool_social" : trophic ? "founder_pool_trophic" : "founder_pool";
    out << "#pragma once\n\n// Founder brains for the " << (social ? "social " : trophic ? "trophic " : "")
        << "brains world, bred by apps/nursery.cpp. Generated:\n"
           "// do not edit. Rebuild the nursery and run it again after any change to the\n"
           "// senses, the network, or the brains world's physics.\n\n#include \"brain.hpp\"\n\n"
           "namespace petri::brain {\n\ninline constexpr int " << name << "_size = "
        << pool.size() << ";\ninline constexpr std::int8_t " << name << "[" << pool.size() << "][brain_bytes] = {\n";
    for (const auto& brain : pool) {
        out << "    {";
        for (std::size_t i = 0; i < brain.size(); ++i) {
            out << (i ? "," : "") << int(brain[i]);
        }
        out << "},\n";
    }
    out << "};\n\n} // namespace petri::brain\n";
}

} // namespace

int main(int argc, char** argv) {
    social = argc == 3 && std::string_view(argv[1]) == "--social";
    trophic = social || (argc == 3 && std::string_view(argv[1]) == "--trophic");
    if (!wide_world || argc != (trophic ? 3 : 2)) {
        std::fprintf(stderr, "Usage: petri_nursery [--trophic | --social] OUTPUT.hpp (from the 128 by 64 build)\n"
                             "Writes core/founder_brains.hpp, core/founder_brains_trophic.hpp with --trophic,\n"
                             "or core/founder_brains_social.hpp with --social.\n");
        return 2;
    }
    const char* output = argv[argc - 1];
    // Breed more families than are used, prove each over a long run, and keep the steadiest.
    std::vector<std::pair<int, std::vector<Brain>>> bred;
    for (int family = 0; family < families_bred; ++family) {
        auto brains = breed(family);
        if (!brains.empty()) {
            const int worlds = sustained(brains, proving_ticks);
            std::printf("family=%d sustains %d of %zu worlds for %d ticks\n", family, worlds, nursery_seeds.size() + proving_seeds.size(),
                        proving_ticks);
            std::fflush(stdout);
            bred.emplace_back(worlds, std::move(brains));
        }
    }
    std::stable_sort(bred.begin(), bred.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (int(bred.size()) > families_used) {
        bred.resize(families_used);
    }
    // Interleave the families so any rotation of the pool mixes them.
    std::vector<Brain> pool;
    for (int i = 0; i < kept; ++i) {
        for (const auto& family : bred) {
            pool.push_back(family.second[i]);
        }
    }
    const int worlds = pool.empty() ? 0 : sustained(pool, proving_ticks);
    std::printf("pool of %zu founds and sustains %d of %zu nursery worlds for %d ticks\n", pool.size(), worlds,
                nursery_seeds.size() + proving_seeds.size(), proving_ticks);
    if (worlds * 2 < int(nursery_seeds.size() + proving_seeds.size())) {
        std::fprintf(stderr, "The pool does not found self-sustaining worlds\n");
        return 1;
    }
    write(pool, output);
    std::printf("wrote %zu founder brains to %s\n", pool.size(), output);
}
