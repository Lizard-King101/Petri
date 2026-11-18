#include "world.hpp"

#include <algorithm>
#include <cstdlib>

namespace petri {
namespace {

int cover(const Cell& cell) {
    return std::min(36, cell.plants * 2);
}

int hunt_range(int hunting) {
    return 2 + hunting / 3;
}

int kin_discount(const Creature& hunter, const Creature& prey, bool express_traits, KinTuning tuning) {
    const int resemblance = express_traits ? hunter.genes.prey_resemblance(prey.genes) : Genome::max_prey_resemblance;
    if (tuning.protect_similar_prey && resemblance > 0) {
        return 100;
    }
    const int reluctance = express_traits ? hunter.genes.kin_reluctance : 8;
    const int kin = hunter.close_kin(prey) ? reluctance * tuning.kin_strength : 0;
    const int resemblance_discount = reluctance * tuning.resemblance_strength * resemblance / 16;
    return std::min(95, std::max(kin, resemblance_discount));
}

} // namespace

int Genome::prey_resemblance(const Genome& other) const {
    const int distance = 2 * std::abs(digestion - other.digestion) + std::abs(thrift - other.thrift) +
        std::abs(hunting - other.hunting) + std::abs(evasion - other.evasion);
    return std::max(0, max_prey_resemblance - distance);
}

int World::hunt_energy(int prey_energy, int digestion, bool adaptive) {
    return std::min(prey_energy * 3 / 4, (prey_energy + (adaptive ? 28 : 40)) * digestion * digestion /
        (adaptive ? 125 : 140));
}

int World::step_toward(int from, int target, unsigned offset) const {
    const int distance = cell_distance(from, target);
    const auto adjacent = neighbors(from);
    for (unsigned i = 0; i < adjacent.size(); ++i) {
        const int step = adjacent[(i + offset) % adjacent.size()];
        if (step >= 0 && occupants_[step] < 0 && cell_distance(step, target) < distance) {
            return step;
        }
    }
    return -1;
}

HuntDecision World::skipped_hunt(const Creature& hunter, int range, int offset) const {
    bool saw_prey = false;
    const int x = hunter.cell % width, y = hunter.cell / width;
    for (int yy = std::max(0, y - range); yy <= std::min(height - 1, y + range); ++yy) {
        const int span = range - std::max(y - yy, yy - y);
        for (int xx = std::max(0, x - span); xx <= std::min(width - 1, x + span); ++xx) {
            const int cell = yy * width + xx;
            const int slot = occupants_[cell];
            if (cell == hunter.cell || slot < 0 ||
                (creatures_[slot].nest_ticks && creatures_[slot].has_parent(hunter.id))) {
                continue;
            }
            saw_prey = true;
            if (cell_distance(cell, hunter.cell) == 1 || step_toward(hunter.cell, cell, offset) >= 0) {
                return HuntDecision::forage_or_rest;
            }
        }
    }
    return saw_prey ? HuntDecision::blocked : HuntDecision::no_prey;
}

World::HuntPlan World::plan_hunt(const Creature& hunter, const Genome& genes, int offset,
                               int forage, int upkeep) const {
    constexpr int strike_cost = 3;
    const int chase_cost = hunt_tuning_.chase_cost;
    const int skill = trait_effects_ ? genes.hunting : 8;
    const int digestion = genes.meat_digestion();
    // Hunting remains a response to hunger. Skill prices the chase below.
    const int threshold = std::clamp(hunt_tuning_.threshold + 2 * (digestion - 8), 0, 160);
    if (hunter.energy >= threshold) {
        return {HuntDecision::sated};
    }
    if (hunter.energy <= upkeep + strike_cost) {
        return {HuntDecision::low_energy};
    }

    const int range = hunt_range(skill);
    const int x = hunter.cell % width, y = hunter.cell / width;
    const int bite = std::min(body_mass, 9 - genes.thrift / 4);
    HuntPlan plan{HuntDecision::no_prey};
    bool saw_prey = false, has_route = false, can_pay = false;
    bool viable_without_reluctance = false, viable_same_faction = false;
    const int forage_score = 100 * forage;
    int best = forage_score;
    for (int yy = std::max(0, y - range); yy <= std::min(height - 1, y + range); ++yy) {
        const int span = range - std::max(y - yy, yy - y);
        for (int xx = std::max(0, x - span); xx <= std::min(width - 1, x + span); ++xx) {
            const int cell = yy * width + xx;
            const int slot = occupants_[cell];
            if (cell == hunter.cell || slot < 0) {
                continue;
            }
            const auto& prey = creatures_[slot];
            if (prey.nest_ticks && prey.has_parent(hunter.id)) {
                continue;
            }
            saw_prey = true;
            const int distance = cell_distance(hunter.cell, cell);
            const bool adjacent = distance == 1;
            int step = adjacent ? -1 : step_toward(hunter.cell, cell, offset);
            if (!adjacent && step < 0) {
                continue;
            }
            has_route = true;
            if (!adjacent && hunter.energy <= upkeep + chase_cost) {
                continue;
            }
            can_pay = true;
            const bool lunge = skill >= 10 && distance == 2 && cells_[step].plants < 14 &&
                hunter.energy > upkeep + strike_cost + chase_cost;
            const bool striking = adjacent || lunge;
            int movement_cost = chase_cost;
            const bool sprint = skill >= 11 && distance >= 3 && hunter.energy > upkeep + 2 * chase_cost &&
                                cells_[step].plants < 16;
            if (sprint) {
                const int extra = step_toward(step, cell, offset);
                if (extra >= 0) {
                    step = extra;
                    movement_cost += chase_cost;
                }
            }

            const int shield = prey.nest_ticks && guarded(prey) ? 52 : 0;
            const int chance = std::clamp(58 + (skill - 8) * 7 - prey.energy / 10 - cover(cells_[cell]) +
                (prey.nest_ticks ? 24 : 0) - shield - (lunge ? 12 : 0), 5, 95);
            const int reserve = hunt_energy(prey.energy, digestion, true);
            const int reward = std::min(160 - int(hunter.energy), reserve + bite * 2 * digestion / 10);
            const int steps_left = striking ? 0 : cell_distance(step, cell);
            // A chase consumes an action, and the strike may still be several turns away.
            const int turns = 1 + steps_left;
            const int chase_paid = striking ? (lunge ? chase_cost : 0) : movement_cost;
            const int cost = (strike_cost + chase_paid) * 100;
            const int payoff = chance * reward;
            int value = payoff;
            if (kin_avoidance_) {
                const int discount = kin_discount(hunter, prey, trait_effects_, kin_tuning_);
                value = value * (100 - discount) / 100;
            }
            const int score = (value - cost) / turns;
            if (cross_faction_hunts_ && hunter.genes.faction == prey.genes.faction) {
                viable_same_faction |= score > forage_score;
                continue;
            }
            viable_without_reluctance |= score <= forage_score && value < payoff &&
                (payoff - cost) / turns > forage_score;
            if (score > best) {
                best = score;
                plan = {striking ? HuntDecision::strike : HuntDecision::pursue, slot, step,
                        striking ? chance : 0, chase_paid, striking ? strike_cost : 0};
            }
        }
    }
    if (plan.prey < 0) {
        plan.decision = !saw_prey ? HuntDecision::no_prey
            : !has_route ? HuntDecision::blocked
            : !can_pay ? HuntDecision::low_energy
            : viable_same_faction ? HuntDecision::faction_protected
            : viable_without_reluctance ? HuntDecision::kin_reluctance : HuntDecision::forage_or_rest;
    }
    return plan;
}

void World::prepare_evasion(StepObserver observer) {
    evaded_.fill(false);
    if (passive_prey_) {
        return;
    }
    std::array<int, capacity> threats;
    threats.fill(-1);
    // Use the previous tick's commitments so escape precedes adult actions in every slot.
    for (const auto& hunter : creatures_) {
        if (!hunter.hunt_target) {
            continue;
        }
        const auto target = std::find_if(creatures_.begin(), creatures_.end(), [&](const Creature& candidate) {
            return candidate.id == hunter.hunt_target;
        });
        if (target != creatures_.end() && !target->nest_ticks) {
            auto& threat = threats[target - creatures_.begin()];
            if (threat < 0 || cell_distance(hunter.cell, target->cell) < cell_distance(threat, target->cell)) {
                threat = hunter.cell;
            }
        }
    }

    for (int slot = 0; slot < capacity; ++slot) {
        const int threat = threats[slot];
        if (threat < 0) {
            continue;
        }
        auto& prey = creatures_[slot];
        const int origin = prey.cell;
        const int skill = trait_effects_ ? prey.genes.evasion : 8;
        const int distance = cell_distance(origin, threat);
        const int range = 1 + skill / 4;
        if (distance > range || (range - distance + 2) * skill < 18 + std::min(12, int(cells_[origin].plants))) {
            continue;
        }
        constexpr int distance_value = 30;
        const int cover_value = 1 + skill / 5;
        const auto safety = [&](int cell) {
            return cell_distance(cell, threat) * distance_value + cover(cells_[cell]) * cover_value;
        };
        int destination = origin;
        for (const int cell : neighbors(origin)) {
            if (cell >= 0 && occupants_[cell] < 0 && safety(cell) > safety(destination)) {
                destination = cell;
            }
        }
        if (destination == origin) {
            continue;
        }
        const int cover_gain = cover(cells_[destination]) - cover(cells_[origin]);
        const int retreat = std::max(0, cell_distance(destination, threat) - distance);
        const bool sheltered = cover_gain * cover_value > retreat * distance_value;
        const int cost = sheltered ? 4 : 5;
        if (prey.energy <= cost) {
            continue;
        }
        prey.energy -= cost;
        prey.cell = destination;
        prey.hunt_target = 0;
        move_creature(slot, origin, false);
        evaded_[slot] = true;
        if (observer.record_hunt_decision) {
            observer.record_hunt_decision(observer.context, sheltered ? HuntDecision::cover : HuntDecision::flee);
        }
    }
}

} // namespace petri
