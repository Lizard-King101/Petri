// The brains world (docs/EMERGENT_LIFE.md, Phase 1): an evolved network
// chooses each creature's action, and every creature decides from the state at
// the start of the tick before any of them acts.
//
// Only the choice changes. Upkeep, eating, the capture roll, and budding use
// the food web's existing arithmetic, so a brains run and a run under the fixed
// rules differ in who decides and nothing else.

#include "brain.hpp"
#include "founder_brains.hpp"
#include "founder_brains_social.hpp"
#include "founder_brains_trophic.hpp"
#include "world.hpp"

#include <algorithm>
#include <cstring>
#include <new>

namespace petri {

namespace {

// Each block holds the brains of this many consecutive slots.
constexpr int block_slots = capacity / BrainStore::weight_blocks;
static_assert(block_slots * BrainStore::weight_blocks == capacity);
constexpr std::size_t block_words = std::size_t(block_slots) * brain::brain_words;
BrainStore::Allocate allocate_weights = nullptr;
BrainStore::Release release_weights = nullptr;

} // namespace

void BrainStore::set_weight_memory(Allocate allocate, Release release) {
    allocate_weights = allocate;
    release_weights = release;
}

struct BrainState {
    // Weights, four to a 32-bit word. Byte order within a word is the target's,
    // and every target is little-endian. The block may sit in memory that
    // allows only whole-word access, so it is touched through `volatile`: the
    // compiler then cannot turn a copy loop into a byte-wise memcpy.
    std::array<volatile std::uint32_t*, BrainStore::weight_blocks> blocks{};

    BrainState() {
        for (auto& block : blocks) {
            block = allocate();
            for (std::size_t i = 0; i < block_words; ++i) {
                block[i] = 0;
            }
        }
    }
    BrainState(const BrainState& other)
        : inner(other.inner), born(other.born), flash(other.flash), decisions(other.decisions),
          deciders(other.deciders), courting(other.courting), sight(other.sight) {
        for (std::size_t n = 0; n < blocks.size(); ++n) {
            blocks[n] = allocate();
            for (std::size_t i = 0; i < block_words; ++i) {
                blocks[n][i] = other.blocks[n][i];
            }
        }
    }
    BrainState& operator=(const BrainState&) = delete;
    ~BrainState() {
        for (auto* block : blocks) {
            if (release_weights) {
                release_weights(const_cast<std::uint32_t*>(block));
            } else {
                ::operator delete(const_cast<std::uint32_t*>(block));
            }
        }
    }
    static volatile std::uint32_t* allocate() {
        constexpr std::size_t bytes = block_words * sizeof(std::uint32_t);
        return static_cast<std::uint32_t*>(allocate_weights ? allocate_weights(bytes) : ::operator new(bytes));
    }
    // The words of one slot's brain.
    volatile std::uint32_t* words(int slot) const {
        return blocks[slot / block_slots] + std::size_t(slot % block_slots) * brain::brain_words;
    }

    std::array<brain::Inner, capacity> inner{};
    std::array<std::uint32_t, capacity> born{};
    // Set by a creature's last action and seen by others on the next tick.
    std::array<bool, capacity> flash{};
    // The decide phase's results. `deciders` holds the ID each decision was made
    // for, so a slot that changes hands before its turn is skipped.
    std::array<brain::Decision, capacity> decisions{};
    std::array<CreatureId, capacity> deciders{};
    // Who chose to breed this tick. Two-parent breeding needs both to have chosen it.
    std::array<bool, capacity> courting{};
    brain::Sight sight;
};

BrainStore::BrainStore(const BrainStore& other) : state_(other.state_ ? new BrainState(*other.state_) : nullptr) {}

BrainStore& BrainStore::operator=(const BrainStore& other) {
    if (this != &other) {
        auto* copy = other.state_ ? new BrainState(*other.state_) : nullptr;
        delete state_;
        state_ = copy;
    }
    return *this;
}

BrainStore::~BrainStore() {
    delete state_;
}

void BrainStore::create() {
    delete state_;
    state_ = new BrainState;
}

namespace {

// The food web's constants, as in world.cpp.
constexpr int attack_cost = 2;
constexpr int minimum_meal_gain = 2;
constexpr int fasting_upkeep = 2;
// Budding costs the young's energy plus 10. The young's share grows with its
// body, to 50 at most, so a parent with this much can always pay.
constexpr int breeding_energy = 70;
constexpr int move_cost = 1, sprint_cost = 2;
// With Config::trophic an attack costs this plus a quarter of the target's
// body size, and takes at least `bite_minimum` energy from it.
constexpr int bite_base_cost = 1, bite_minimum = 2;

constexpr std::uint32_t founder_brain_salt = 0x2545f491u, inherit_salt = 0x9e3779b1u, shuffle_salt = 0x68e31da4u;
constexpr std::uint32_t neutral_salt = 0x1f83d9abu, order_salt = 0x5be0cd19u;
// Founders start with small random weights: enough to act, with no behavior written in.
constexpr int founder_weight_limit = 32;

std::uint32_t mix(std::uint32_t a, std::uint32_t b, std::uint32_t salt) {
    auto mixed = a + b * 0x9e3779b9u + salt;
    mixed = (mixed ^ (mixed >> 16)) * 0x85ebca6bu;
    mixed = (mixed ^ (mixed >> 13)) * 0xc2b2ae35u;
    return mixed ^ (mixed >> 16);
}

void read_brain(const BrainState& state, int slot, std::int8_t* weights) {
    std::uint32_t local[brain::brain_words];
    const auto* stored = state.words(slot);
    for (int i = 0; i < brain::brain_words; ++i) {
        local[i] = stored[i];
    }
    std::memcpy(weights, local, brain::brain_bytes);
}

void write_brain(BrainState& state, int slot, const std::int8_t* weights) {
    std::uint32_t padded[brain::brain_words] = {};
    std::memcpy(padded, weights, brain::brain_bytes);
    auto* stored = state.words(slot);
    for (int i = 0; i < brain::brain_words; ++i) {
        stored[i] = padded[i];
    }
}

} // namespace

bool World::brain_weights(int slot, std::int8_t* weights) const {
    if (!brains_ || slot < 0 || slot >= capacity || !creatures_[slot].alive()) {
        return false;
    }
    read_brain(*brain_store_.get(), slot, weights);
    return true;
}

bool World::set_brain(int slot, const std::int8_t* weights) {
    if (!brains_ || slot < 0 || slot >= capacity || !creatures_[slot].alive()) {
        return false;
    }
    write_brain(*brain_store_.get(), slot, weights);
    return true;
}

void World::found_brains(std::uint32_t seed, bool random_weights) {
    brain_store_.create();
    auto& state = *brain_store_.get();
    for (int slot = 0; slot < founder_count; ++slot) {
        // Separate draws keep the ecological random stream the same as without brains.
        // Founders are bred for the physics they will live in.
        const bool social = trophic_ && sexual_reproduction_;
        const int pool_size = social ? brain::founder_pool_social_size
            : trophic_ ? brain::founder_pool_trophic_size : brain::founder_pool_size;
        if (!random_weights && pool_size > 0) {
            // Each world starts from a different rotation of the bred pool.
            const auto index = (mix(seed, 0, founder_brain_salt) + std::uint32_t(slot)) % std::uint32_t(pool_size);
            write_brain(state, slot, social ? brain::founder_pool_social[index]
                : trophic_ ? brain::founder_pool_trophic[index] : brain::founder_pool[index]);
            continue;
        }
        std::int8_t weights[brain::brain_bytes];
        for (int i = 0; i < brain::brain_bytes; ++i) {
            const auto roll = mix(seed, creatures_[slot].id * 251u + std::uint32_t(i), founder_brain_salt);
            weights[i] = static_cast<std::int8_t>(int(roll % (2 * founder_weight_limit + 1)) - founder_weight_limit);
        }
        write_brain(state, slot, weights);
    }
}

bool World::brain_courting(int slot) const {
    return brain_store_.get()->courting[slot];
}

void World::inherit_brain(int child, int parent, int mate) {
    auto& state = *brain_store_.get();
    int source = parent;
    if (shuffled_brains_) {
        // Any other living creature, chosen without touching the ecological stream.
        const auto id = creatures_[child].id;
        int pick = static_cast<int>(mix(rng_, id, shuffle_salt) % (metrics_.population - 1));
        for (int slot = 0; slot < capacity; ++slot) {
            if (slot != child && creatures_[slot].alive() && pick-- == 0) {
                source = slot;
                break;
            }
        }
    }
    std::int8_t inherited[brain::brain_bytes], weights[brain::brain_bytes];
    read_brain(state, source, inherited);
    auto mutation = mix(rng_, creatures_[child].id, inherit_salt);
    if (mate >= 0 && !shuffled_brains_) {
        std::int8_t first[brain::brain_bytes], second[brain::brain_bytes];
        read_brain(state, parent, first);
        read_brain(state, mate, second);
        brain::combine(inherited, first, second, mutation);
    }
    brain::inherit(weights, inherited, mutation);
    write_brain(state, child, weights);
    state.inner[child] = {};
    state.flash[child] = false;
    state.born[child] = static_cast<std::uint32_t>(metrics_.ticks);
}

void World::decide_brains(int first, int last) {
    // Reads the world and writes only this range's decisions, so ranges can run
    // on separate cores.
    auto& state = *brain_store_.get();
    const auto tick = metrics_.ticks;
    for (int slot = first; slot < last; ++slot) {
        const auto& creature = creatures_[slot];
        state.deciders[slot] = creature.id;
        state.decisions[slot] = {};
        // Young in the nest do not act.
        if (!creature.alive() || creature.nest_ticks) {
            continue;
        }
        auto inner = state.inner[slot];
        const auto age = static_cast<std::uint32_t>(tick) - state.born[slot];
        inner.youth = static_cast<std::int8_t>(age < Creature::maturity_ticks ? (Creature::maturity_ticks - age) * 5 : 0);
        const auto senses = brain::sense(*this, state.sight, slot, inner);
        auto& decision = state.decisions[slot];
        if (neutral_brains_) {
            const auto roll = mix(creature.id, static_cast<std::uint32_t>(tick), neutral_salt);
            decision = {};
            decision.action = static_cast<std::uint8_t>(roll % (brain::directions + 1));
            decision.sprint = roll >> 8 & 1u;
            decision.breed = roll >> 9 & 1u;
            decision.signal = roll >> 10 & 1u;
        } else {
            std::int8_t weights[brain::brain_bytes];
            read_brain(state, slot, weights);
            decision = brain::think(senses, weights, brain::turn_for(creature.id, tick));
        }
    }
}

void World::step_brains(StepObserver observer) {
    auto& state = *brain_store_.get();
    state.sight = brain::Sight(*this);
    for (int slot = 0; slot < capacity; ++slot) {
        if (creatures_[slot].alive() && state.flash[slot]) {
            state.sight.flash(creatures_[slot].cell);
        }
    }
    decide_brains(0, capacity);
    for (int slot = 0; slot < capacity; ++slot) {
        state.courting[slot] = creatures_[slot].alive() && state.decisions[slot].breed;
    }
    // Damage was sensed by the decisions above. Attacks in this tick's act phase
    // set it again for the next.
    for (auto& inner : state.inner) {
        inner.damage = 0;
    }
    // Creatures act in slot order from a start that moves every tick, so no
    // slot always reaches a contested cell first.
    const int start = static_cast<int>(mix(rng_, static_cast<std::uint32_t>(metrics_.ticks), order_salt) % capacity);
    for (int n = 0; n < capacity; ++n) {
        const int slot = (start + n) % capacity;
        const auto& creature = creatures_[slot];
        if (creature.alive() && creature.id == state.deciders[slot]) {
            act_brain(slot, observer);
        }
    }
}

void World::act_brain(int slot, StepObserver observer) {
    auto& state = *brain_store_.get();
    auto& creature = creatures_[slot];
    if (creature.nest_ticks) {
        update_young(creature, observer);
        return;
    }
    // A parent whose young has left the nest or died drops what it was carrying for it.
    auto* young = nests_ ? dependent(slot) : nullptr;
    if (!young && creature.provisions()) {
        add_detritus(cells_[creature.cell], creature.provisions());
        creature.packed_plants = creature.packed_animal = creature.packed_fruit = 0;
    }
    const auto decision = state.decisions[slot];
    // The neutral control inherits and mutates genes with founder physiology.
    const Genome effective = trait_effects_ ? creature.genes : Genome{8, 8, 8, 8, 8, 8, 8, 8, 8};
    const int meat_digestion = effective.meat_digestion();
    const int body_cost = body_upkeep(creature) + gut_upkeep(effective);
    int cost = std::max(1, 5 - effective.thrift / 4 + body_cost);
    const int origin = creature.cell;
    int vacated = -1, captured_energy = 0;
    bool captured = false, struck = false, crossed = false;

    if (decision.action) {
        const int d = decision.action - 1;
        const auto ahead = [d](int cell) {
            const int x = cell % width + brain::step_x[d], y = cell / width + brain::step_y[d];
            return x >= 0 && x < width && y >= 0 && y < height ? y * width + x : -1;
        };
        const int target = ahead(origin);
        const int occupant = target < 0 ? -1 : int(occupants_[target]);
        if (target < 0) {
            // Walking into the world's edge costs the step and goes nowhere.
            cost += move_cost;
        } else if (occupant < 0) {
            creature.cell = static_cast<std::uint16_t>(target);
            cost += move_cost;
            crossed |= move_creature(slot, origin, false);
            vacated = origin;
            const int beyond = decision.sprint ? ahead(target) : -1;
            if (beyond >= 0 && occupants_[beyond] < 0) {
                creature.cell = static_cast<std::uint16_t>(beyond);
                cost += sprint_cost;
                crossed |= move_creature(slot, target, false);
                vacated = target;
            }
        } else if (trophic_) {
            // Moving into an occupied cell is an attack: a bite that takes energy
            // out of the target. A larger attacker bites deeper, and a larger
            // target is harder to bite and costs more to attack. The target lives
            // while it has energy left, and can flee.
            auto& prey = creatures_[occupant];
            struck = true;
            ++metrics_.attacks;
            const int size = morphology_effects() ? creature.genes.body_size : 8;
            const int prey_size = morphology_effects() ? prey.genes.body_size : 8;
            cost += bite_base_cost + prey_size / 4;
            int depth = std::max(bite_minimum, 6 + 2 * size - prey_size);
            if (prey.nest_ticks) {
                // A parent beside the nest takes most of a bite meant for its young.
                const bool defended = guarded(prey);
                ++metrics_.nests.attacks;
                metrics_.nests.guarded_attacks += defended;
                depth = defended ? std::max(1, depth / 4) : depth;
            }
            const int bite = std::min<int>(prey.energy, depth);
            // Meat digestion sets how much of the bite the attacker keeps: 5% to 75%.
            captured_energy = bite * meat_digestion / 20;
            const bool landed = bite == prey.energy;
            if (observer.record_attack) {
                observer.record_attack(observer.context, creature, prey, landed);
            }
            // Flesh counts toward the attacker's diet in carcass units of about four energy.
            const std::uint32_t flesh = static_cast<std::uint32_t>(std::max(1, bite / 4));
            creature.animal_eaten += std::min<std::uint32_t>(flesh, 0xffffffffu - creature.animal_eaten);
            metrics_.animal_eaten += flesh;
            if (landed) {
                metrics_.nests.killed += prey.nest_ticks > 0;
                if (observer.record_kill) {
                    observer.record_kill(observer.context, creature, prey);
                }
                kill(prey, observer, DeathCause::predation);
                creature.cell = static_cast<std::uint16_t>(target);
                ++metrics_.kills;
                creature.prey_killed += creature.prey_killed < 0xffffffffu;
                if (observer.record_hunt) {
                    observer.record_hunt(observer.context, static_cast<std::uint16_t>(origin),
                                         static_cast<std::uint16_t>(target));
                }
                captured = true;
                crossed |= move_creature(slot, origin, true);
                vacated = origin;
            } else {
                prey.energy = static_cast<std::uint16_t>(prey.energy - bite);
                state.inner[occupant].damage = static_cast<std::int8_t>(std::min(127, bite * 4));
            }
        } else {
            // Moving into an occupied cell is an attack, resolved by the food web's capture roll.
            auto& prey = creatures_[occupant];
            struck = true;
            cost += attack_cost;
            ++metrics_.attacks;
            const int vegetation_cover = std::min(30, cells_[prey.cell].plants * 30 / 12);
            // Young in an open nest are easy to take, unless a parent stands guard.
            const bool defended = prey.nest_ticks && guarded(prey);
            if (prey.nest_ticks) {
                ++metrics_.nests.attacks;
                metrics_.nests.guarded_attacks += defended;
            }
            const int chance = std::clamp(48 - prey.energy / 4 + (meat_digestion - 8) * 18 - vegetation_cover +
                                          (prey.nest_ticks ? 36 : 0) - (defended ? 52 : 0) +
                                          body_odds(creature, prey), 5, 92);
            const bool landed = int(random() % 100) < chance;
            if (observer.record_attack) {
                observer.record_attack(observer.context, creature, prey, landed);
            }
            if (landed) {
                metrics_.nests.killed += prey.nest_ticks > 0;
                captured_energy = hunt_energy(prey.energy, meat_digestion, false);
                if (observer.record_kill) {
                    observer.record_kill(observer.context, creature, prey);
                }
                kill(prey, observer, DeathCause::predation);
                creature.cell = static_cast<std::uint16_t>(target);
                ++metrics_.kills;
                creature.prey_killed += creature.prey_killed < 0xffffffffu;
                if (observer.record_hunt) {
                    observer.record_hunt(observer.context, static_cast<std::uint16_t>(origin),
                                         static_cast<std::uint16_t>(target));
                }
                captured = true;
                crossed |= move_creature(slot, origin, true);
                vacated = origin;
            } else {
                state.inner[occupant].damage = 127;
            }
        }
    }

    // Eating is not a choice: a creature eats what its digestion values most
    // where it ends the tick, as under the fixed rules.
    auto& cell = cells_[creature.cell];
    const int bite_limit = this->bite_limit(creature, effective);
    const int fiber_cost = std::max(0, 8 - int(effective.digestion)) * 3 / 2;
    const int fruit_value = effective.digestion + 8;
    const int foliage_gain = std::min(int(cell.plants), bite_limit) * effective.digestion / 10 - fiber_cost;
    const int meat_gain = std::min(int(cell.animal), bite_limit) * 2 * meat_digestion / 10;
    const int fruit_gain = fruit_at(creature.cell) * fruit_value / 10;
    const bool eating = std::max({foliage_gain, meat_gain, fruit_gain}) > minimum_meal_gain;
    if (!decision.action && !eating) {
        cost = std::max(1, fasting_upkeep + body_cost);
    }
    if (connected_biomes_) {
        const int difference = effective.preferred_moisture() - cell.moisture;
        cost += std::max(difference, -difference) / 50;
    }
    int energy = creature.energy - cost + captured_energy;
    if ((eating && !struck) || captured) {
        const bool eat_meat = captured || meat_gain > std::max(foliage_gain, fruit_gain);
        const auto food = eat_meat ? Food::meat : fruit_gain > foliage_gain ? Food::fruit : Food::foliage;
        const int gathered = gather_food(creature.cell, food, bite_limit);
        // A parent with young in the nest sets part of each meal aside for it.
        const bool feeding_young = young && young->nest_ticks && young->has_parent(creature.id);
        const int bite = gathered - (provisioning_ && feeding_young ? pack_food(creature, food, gathered) : 0);
        const int efficiency = eat_meat ? meat_digestion : effective.digestion;
        const int retained = std::min({bite, birth_matter(creature) - creature.nutrients_carried,
                                       (bite * efficiency + 10) / 20});
        creature.nutrients_carried += retained;
        add_detritus(cell, bite - retained);
        record_food(creature, food, bite, observer);
        // The biome counters follow the fixed rules: a meal after crossing, and
        // a meal away from the preferred biome that a return trip then counts.
        if (bite > 0 && crossed) {
            ++metrics_.cross_biome_meals;
        }
        if (bite > 0 && creature.excursion != Excursion::none &&
            biome_at(cell.moisture) != biome_at(effective.preferred_moisture())) {
            if (captured) {
                creature.excursion = Excursion::hunted;
            } else if (creature.excursion == Excursion::away) {
                creature.excursion = Excursion::fed;
            }
        }
        energy += eat_meat ? bite * 2 * meat_digestion / 10
            : food == Food::fruit ? bite * fruit_value / 10 : bite * effective.digestion / 10 - fiber_cost;
    }
    if (energy <= 0) {
        kill(creature, observer, DeathCause::depleted_energy);
        return;
    }
    creature.energy = static_cast<std::uint16_t>(std::min(energy, max_energy(creature)));
    state.inner[slot].memory = decision.memory;
    state.flash[slot] = decision.signal;
    // Food carried for the young reaches it when the parent stands beside the nest.
    if (provisioning_ && young && young->nest_ticks && young->has_parent(creature.id) &&
        cell_distance(creature.cell, young->cell) == 1) {
        deliver_food(creature, *young, observer);
    }

    if (sexual_reproduction_) {
        // Two adjacent creatures that both chose to breed, and are able to, mate.
        if (decision.breed && can_mate(creature)) {
            const auto newest = next_id_;
            reproduce_sexually(creature, observer);
            if (next_id_ != newest) {
                const auto born = std::find_if(creatures_.begin(), creatures_.end(), [&](const Creature& candidate) {
                    return candidate.id == newest;
                });
                const auto other = std::find_if(creatures_.begin(), creatures_.end(), [&](const Creature& candidate) {
                    return candidate.id == born->other_parent;
                });
                inherit_brain(static_cast<int>(born - creatures_.begin()), slot,
                              static_cast<int>(other - creatures_.begin()));
            }
        }
    } else if (decision.breed && creature.energy >= breeding_energy &&
               creature.nutrients_carried >= birth_matter(creature)) {
        // The young buds into the cell just left, or else the first free neighbor.
        int nursery = vacated >= 0 && occupants_[vacated] < 0 ? vacated : -1;
        const auto adjacent = neighbors(creature.cell);
        const unsigned turn = brain::turn_for(creature.id, metrics_.ticks);
        for (unsigned i = 0; nursery < 0 && i < adjacent.size(); ++i) {
            const int candidate = adjacent[(i + turn) % adjacent.size()];
            if (candidate >= 0 && occupants_[candidate] < 0) {
                nursery = candidate;
            }
        }
        const auto births = metrics_.births;
        if (nursery >= 0) {
            reproduce(creature, nursery, observer);
        }
        if (metrics_.births != births) {
            inherit_brain(occupants_[nursery], slot);
        }
    }
}

int World::gut_upkeep(const Genome& effective) const {
    if (!trophic_) {
        return 0;
    }
    // Each gut costs upkeep with the square of its gene, in 1/32 of an energy
    // unit per tick, less the founders' standard pair of 8 and 8. The smaller
    // gut is charged Config::second_gut percent of its cost. At the default 25,
    // one full gut (15 and 1) costs 0.56 a tick and two full guts 0.78. The
    // shared 32-tick phase spreads the fractions evenly, as in body_upkeep().
    const int plant = effective.digestion, meat = effective.meat_digestion();
    const int larger = std::max(plant, meat), smaller = std::min(plant, meat);
    const int steps = (larger * larger - 64 + (smaller * smaller - 64) * second_gut_ / 100) / 8;
    const int total = steps + static_cast<int>(metrics_.ticks % 32);
    return total >= 0 ? total / 32 : -((31 - total) / 32);
}

// A record is the brain's words, least significant byte first, then the two
// memory values, the damage sensed, the signal flash, and the tick of birth.
static_assert(brain_record_bytes == brain::brain_words * 4 + 4 + 4);

void World::export_brain(int slot, std::uint8_t* record) const {
    const auto& state = *brain_store_.get();
    const auto put32 = [&record](std::uint32_t value) {
        for (int byte = 0; byte < 4; ++byte) {
            *record++ = static_cast<std::uint8_t>(value >> (8 * byte));
        }
    };
    for (int word = 0; word < brain::brain_words; ++word) {
        put32(state.words(slot)[word]);
    }
    const auto& inner = state.inner[slot];
    *record++ = static_cast<std::uint8_t>(inner.memory[0]);
    *record++ = static_cast<std::uint8_t>(inner.memory[1]);
    *record++ = static_cast<std::uint8_t>(inner.damage);
    *record++ = state.flash[slot];
    put32(state.born[slot]);
}

bool World::import_brain(int slot, const std::uint8_t* record) {
    auto& state = *brain_store_.get();
    const auto get32 = [&record] {
        std::uint32_t value = 0;
        for (int byte = 0; byte < 4; ++byte) {
            value |= std::uint32_t(*record++) << (8 * byte);
        }
        return value;
    };
    // No weight is ever -128, and the padding past the last weight is zero.
    for (int i = 0; i < brain::brain_words * 4; ++i) {
        if (i < brain::brain_bytes ? record[i] == 0x80 : record[i] != 0) {
            return false;
        }
    }
    for (int word = 0; word < brain::brain_words; ++word) {
        state.words(slot)[word] = get32();
    }
    auto& inner = state.inner[slot];
    inner.memory[0] = static_cast<std::int8_t>(*record++);
    inner.memory[1] = static_cast<std::int8_t>(*record++);
    inner.damage = static_cast<std::int8_t>(*record++);
    const auto flash = *record++;
    const auto born = get32();
    if (flash > 1 || born > static_cast<std::uint32_t>(metrics_.ticks)) {
        return false;
    }
    state.flash[slot] = flash != 0;
    state.born[slot] = born;
    return true;
}

std::uint64_t World::brain_hash() const {
    const auto& state = *brain_store_.get();
    std::uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            hash ^= (value >> (8 * byte)) & 255;
            hash *= 1099511628211ull;
        }
    };
    for (int slot = 0; slot < capacity; ++slot) {
        if (!creatures_[slot].alive()) {
            continue;
        }
        for (int word = 0; word < brain::brain_words; ++word) {
            add(state.words(slot)[word]);
        }
        const auto& inner = state.inner[slot];
        add(static_cast<std::uint8_t>(inner.memory[0]) | static_cast<std::uint8_t>(inner.memory[1]) << 8 |
            static_cast<std::uint8_t>(inner.damage) << 16 | std::uint32_t(state.flash[slot]) << 24);
        add(state.born[slot]);
    }
    return hash;
}

} // namespace petri
