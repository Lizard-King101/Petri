#include "world.hpp"

#include <algorithm>

namespace petri {
namespace {

int guard_reserve(int drive) {
    return 100 - drive * 5;
}

int provision_reserve(int drive) {
    return 42 + (15 - drive) * 2;
}

} // namespace

int World::cell_distance(int from, int to) {
    const int dx = from % width - to % width;
    const int dy = from / width - to / width;
    return std::max(dx, -dx) + std::max(dy, -dy);
}

void World::prepare_nests() {
    dependents_.fill(-1);
    active_guards_.fill(false);
    // Sample the twelve ticks after the nest phase would have ended, even
    // if the young died. Both adults are still in their breeding cooldown.
    for (const auto& adult : creatures_) {
        if (adult.last_mate <= adult.id || adult.breeding_cooldown >= Creature::recovery_ticks - Creature::nest_duration ||
            adult.breeding_cooldown < Creature::recovery_ticks - 2 * Creature::nest_duration) {
            continue;
        }
        const auto mate = std::find_if(creatures_.begin(), creatures_.end(), [&](const Creature& candidate) {
            return candidate.id == adult.last_mate;
        });
        if (mate != creatures_.end()) {
            ++metrics_.nests.after_nest_alive;
            metrics_.nests.after_nest_near += cell_distance(adult.cell, mate->cell) <= 2;
        }
    }
    for (std::size_t slot = 0; slot < creatures_.size(); ++slot) {
        const auto& young = creatures_[slot];
        if (!young.nest_ticks) {
            continue;
        }
        int alive = 0;
        int near = 0;
        for (std::size_t i = 0; i < creatures_.size(); ++i) {
            auto& adult = creatures_[i];
            if (!adult.alive() || !young.has_parent(adult.id)) {
                continue;
            }
            dependents_[i] = slot;
            const auto distance = cell_distance(adult.cell, young.cell);
            ++alive;
            near += distance <= 2;
            const int drive = trait_effects_ ? adult.genes.guard_drive : 8;
            if (guarding_ && distance == 1 && adult.energy >= guard_reserve(drive)) {
                adult.energy -= 2;
                active_guards_[i] = true;
                ++metrics_.nests.guard_ticks;
            }
            if (alive == 2) {
                break;
            }
        }
        metrics_.nests.both_alive += alive == 2;
        metrics_.nests.both_near += near == 2;
    }
}

Creature* World::dependent(int adult_slot) {
    const int nest = dependents_[adult_slot];
    if (nest >= 0 && creatures_[nest].nest_ticks && creatures_[nest].has_parent(creatures_[adult_slot].id)) {
        return &creatures_[nest];
    }
    return nullptr;
}

int World::care_pull(const Creature& adult, const Creature& young) const {
    // Food stays valuable to the adult. A hungry parent can abandon the nest.
    const int guard = trait_effects_ ? adult.genes.guard_drive : 8;
    const int provision = trait_effects_ ? adult.genes.provision_drive : 8;
    const int guarding = guarding_ && adult.energy >= guard_reserve(guard) ? guard * 15 : 0;
    const int returning = provisioning_ && young.energy < 64 && adult.provisions() &&
        adult.energy >= provision_reserve(provision) ? provision * 22 : 0;
    return std::max(guarding, returning);
}

int World::pack_food(Creature& adult, Food food, int bite) {
    const int drive = trait_effects_ ? adult.genes.provision_drive : 8;
    if (adult.energy < provision_reserve(drive)) {
        return 0;
    }
    const int amount = std::min({bite / 2, 1 + drive / 5,
        Creature::max_provisions - adult.provisions()});
    auto& stock = food == Food::meat ? adult.packed_animal
        : food == Food::fruit ? adult.packed_fruit : adult.packed_plants;
    stock += amount;
    return amount;
}

void World::deliver_food(Creature& adult, Creature& young, StepObserver observer) {
    const int drive = trait_effects_ ? adult.genes.provision_drive : 8;
    const int amount = adult.provisions();
    if (!amount || adult.energy < provision_reserve(drive) || young.energy >= 64) {
        return;
    }
    --adult.energy;
    if (adult.packed_plants) {
        feed_young(young, Food::foliage, adult.packed_plants, observer);
    }
    if (adult.packed_fruit) {
        feed_young(young, Food::fruit, adult.packed_fruit, observer);
    }
    if (adult.packed_animal) {
        feed_young(young, Food::meat, adult.packed_animal, observer);
    }
    adult.packed_plants = adult.packed_animal = adult.packed_fruit = 0;
    ++metrics_.nests.deliveries;
    metrics_.nests.food_delivered += amount;
}

void World::feed_young(Creature& young, Food food, int amount, StepObserver observer) {
    // Young can eat small amounts as long as the food reaches the nest.
    young.energy = static_cast<std::uint16_t>(
        std::min(max_energy(young), young.energy + amount * (food == Food::foliage ? 2 : 3)));
    const int retained = std::min(amount / 2, birth_matter(young) - young.nutrients_carried);
    young.nutrients_carried += retained;
    add_detritus(cells_[young.cell], amount - retained);
    record_food(young, food, amount, observer);
}

void World::update_young(Creature& young, StepObserver observer) {
    if (young.energy < 56) {
        const auto food = cells_[young.cell].animal ? Food::meat : fruit_at(young.cell) ? Food::fruit : Food::foliage;
        if (const int bite = gather_food(young.cell, food, 2)) {
            feed_young(young, food, bite, observer);
        }
    }
    constexpr int upkeep = 4;
    if (young.energy <= upkeep) {
        ++metrics_.nests.starved;
        kill(young, observer, DeathCause::depleted_energy);
        return;
    }
    young.energy -= upkeep;
    if (--young.nest_ticks == 0) {
        ++metrics_.nests.fledged;
    }
}

} // namespace petri
