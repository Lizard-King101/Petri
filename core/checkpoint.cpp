#include "world.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>

namespace petri {
namespace {

constexpr std::array<std::uint8_t, 6> header{'P', 'E', 'T', 'R', 'I', 15};

struct Writer {
    std::uint8_t* cursor;

    template<class Integer>
    void put(Integer value) {
        for (std::size_t byte = 0; byte < sizeof(value); ++byte) {
            *cursor++ = static_cast<std::uint8_t>(std::uint64_t(value) >> (8 * byte));
        }
    }
};

struct Reader {
    const std::uint8_t* cursor;

    template<class Integer>
    Integer get() {
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < sizeof(Integer); ++byte) {
            value |= std::uint64_t(*cursor++) << (8 * byte);
        }
        return static_cast<Integer>(value);
    }
};

std::uint64_t checksum(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (auto byte = data; byte != data + size - sizeof(hash); ++byte) {
        hash ^= *byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool valid_trait(std::uint8_t trait) {
    return trait >= Genome::minimum && trait <= Genome::maximum;
}

} // namespace

void World::save_checkpoint(Checkpoint& checkpoint) const {
    Writer writer{std::copy(header.begin(), header.end(), checkpoint.begin())};
    writer.put(rng_);
    writer.put(std::uint8_t(trait_effects_));
    writer.put(carcass_decay_);
    writer.put(std::uint8_t(predation_));
    writer.put(std::uint8_t(food_web_));
    writer.put(std::uint8_t(connected_biomes_));
    writer.put(std::uint8_t(irregular_terrain_));
    writer.put(std::uint8_t(sexual_reproduction_));
    writer.put(std::uint8_t(nests_));
    writer.put(std::uint8_t(guarding_));
    writer.put(std::uint8_t(provisioning_));
    writer.put(std::uint8_t(adaptive_hunts_));
    writer.put(std::uint8_t(passive_prey_));
    writer.put(hunt_tuning_.threshold);
    writer.put(hunt_tuning_.chase_cost);
    writer.put(std::uint8_t(kin_avoidance_));
    writer.put(kin_tuning_.kin_strength);
    writer.put(kin_tuning_.resemblance_strength);
    writer.put(std::uint8_t(kin_tuning_.protect_similar_prey));
    writer.put(std::uint8_t(faction_markers_));
    writer.put(std::uint8_t(cross_faction_hunts_));
    writer.put(std::uint8_t(staggered_growth_));
    writer.put(std::uint8_t(fruiting_plants_));
    writer.put(std::uint8_t(dynamic_environment_));
    writer.put(climate_seed_);
    for (const auto rain : rainfall_) {
        writer.put(rain);
    }
    writer.put(metrics_.ticks);
    writer.put(metrics_.births);
    writer.put(metrics_.deaths);
    writer.put(metrics_.capacity_blocks);
    writer.put(metrics_.plants_eaten);
    writer.put(metrics_.animal_eaten);
    writer.put(metrics_.attacks);
    writer.put(metrics_.kills);
    writer.put(metrics_.biome_crossings);
    writer.put(metrics_.cross_biome_meals);
    writer.put(metrics_.cross_biome_hunts);
    writer.put(metrics_.biome_shifts);
    writer.put(metrics_.feeding_round_trips);
    writer.put(metrics_.hunting_round_trips);
    writer.put(metrics_.fruit_eaten);
    std::apply([&](const auto&... values) { (writer.put(values), ...); }, NestMetrics::fields(metrics_.nests));
    for (std::size_t index = 0; index < cells_.size(); ++index) {
        const auto& cell = cells_[index];
        writer.put(cell.moisture);
        writer.put(cell.nutrients);
        writer.put(cell.plants);
        writer.put(std::uint32_t(cell.detritus));
        writer.put(cell.animal);
        writer.put(shrubs_[index]);
    }
    for (const auto& creature : creatures_) {
        // Checkpoints keep the 64-bit field widths of format version 15.
        writer.put(std::uint64_t(creature.id));
        writer.put(std::uint64_t(creature.parent));
        writer.put(std::uint64_t(creature.founder));
        writer.put(creature.generation);
        writer.put(creature.cell);
        writer.put(creature.energy);
        writer.put(creature.genes.digestion);
        writer.put(creature.genes.thrift);
        writer.put(creature.genes.moisture_preference);
        writer.put(std::uint64_t(creature.plants_eaten));
        writer.put(std::uint64_t(creature.animal_eaten));
        writer.put(creature.nutrients_carried);
        writer.put(std::uint8_t(creature.excursion));
        writer.put(std::uint64_t(creature.other_parent));
        writer.put(creature.breeding_cooldown);
        writer.put(creature.genes.mate_drive);
        writer.put(creature.genes.receptivity);
        writer.put(std::uint64_t(creature.last_mate));
        writer.put(creature.nest_ticks);
        writer.put(creature.packed_plants);
        writer.put(creature.packed_animal);
        writer.put(creature.genes.guard_drive);
        writer.put(creature.genes.provision_drive);
        writer.put(creature.genes.hunting);
        writer.put(creature.genes.evasion);
        writer.put(std::uint64_t(creature.hunt_target));
        writer.put(std::uint64_t(creature.prey_killed));
        writer.put(creature.genes.kin_reluctance);
        writer.put(creature.genes.faction);
        writer.put(creature.packed_fruit);
    }
    writer.put(checksum(checkpoint.data(), checkpoint.size()));
}

bool World::load_checkpoint(const Checkpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), header.back());
}

bool World::load_checkpoint(const LegacyCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 1);
}

bool World::load_checkpoint(const ScavengingCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 2);
}

bool World::load_checkpoint(const PredationCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 3);
}

bool World::load_checkpoint(const FoodWebCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 4);
}

bool World::load_checkpoint(const ConnectedCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 5);
}

bool World::load_checkpoint(const IrregularCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 6);
}

bool World::load_checkpoint(const SexualCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 7);
}

bool World::load_checkpoint(const NestCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 8);
}

bool World::load_checkpoint(const AdaptiveCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 9);
}

bool World::load_checkpoint(const HuntTuningCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 10);
}

bool World::load_checkpoint(const KinAvoidanceCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 11);
}

bool World::load_checkpoint(const KinTuningCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 12);
}

bool World::load_checkpoint(const FactionCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 13);
}

bool World::load_checkpoint(const FloraCheckpoint& checkpoint) {
    return decode_checkpoint(checkpoint.data(), checkpoint.size(), 14);
}

bool World::decode_checkpoint(const std::uint8_t* data, std::size_t size, std::uint8_t version) {
    const bool legacy = version == 1;
    Reader trailer{data + size - sizeof(std::uint64_t)};
    if (!std::equal(header.begin(), header.end() - 1, data) ||
        data[header.size() - 1] != version ||
        trailer.get<std::uint64_t>() != checksum(data, size)) {
        return false;
    }

    // Stage the decoded state so a rejected checkpoint leaves the running world intact.
    World decoded;
    Reader reader{data + header.size()};
    decoded.rng_ = reader.get<std::uint32_t>();
    const auto trait_effects = reader.get<std::uint8_t>();
    if (trait_effects > 1) {
        return false;
    }
    decoded.trait_effects_ = trait_effects != 0;
    if (!legacy) {
        decoded.carcass_decay_ = reader.get<std::uint8_t>();
    }
    if (version >= 3) {
        const auto enabled = reader.get<std::uint8_t>();
        if (enabled > 1 || (enabled && !decoded.carcass_decay_)) {
            return false;
        }
        decoded.predation_ = enabled != 0;
    }
    if (version >= 4) {
        const auto enabled = reader.get<std::uint8_t>();
        if (enabled > 1 || (enabled && !decoded.carcass_decay_)) {
            return false;
        }
        decoded.food_web_ = enabled != 0;
    }
    if (version >= 5) {
        const auto enabled = reader.get<std::uint8_t>();
        if (enabled > 1 || (enabled && (!wide_world || !decoded.food_web_))) {
            return false;
        }
        decoded.connected_biomes_ = enabled != 0;
    }
    if (version >= 6) {
        const auto enabled = reader.get<std::uint8_t>();
        if (enabled > 1 || (enabled && !decoded.connected_biomes_)) {
            return false;
        }
        decoded.irregular_terrain_ = enabled != 0;
    }
    if (version >= 7) {
        const auto enabled = reader.get<std::uint8_t>();
        if (enabled > 1 || (enabled && !decoded.food_web_)) {
            return false;
        }
        decoded.sexual_reproduction_ = enabled != 0;
    }
    if (version >= 8) {
        const auto nests = reader.get<std::uint8_t>();
        const auto guarding = reader.get<std::uint8_t>();
        const auto provisioning = reader.get<std::uint8_t>();
        if (nests > 1 || guarding > 1 || provisioning > 1 || (nests && !decoded.sexual_reproduction_) ||
            (!nests && (guarding || provisioning))) {
            return false;
        }
        decoded.nests_ = nests;
        decoded.guarding_ = guarding;
        decoded.provisioning_ = provisioning;
    }
    if (version >= 9) {
        const auto adaptive = reader.get<std::uint8_t>();
        const auto passive = reader.get<std::uint8_t>();
        if (adaptive > 1 || passive > 1 || (adaptive && (!decoded.food_web_ || !decoded.predation_)) ||
            (passive && !adaptive)) {
            return false;
        }
        decoded.adaptive_hunts_ = adaptive;
        decoded.passive_prey_ = passive;
    }
    if (version >= 10) {
        decoded.hunt_tuning_ = {reader.get<std::uint8_t>(), reader.get<std::uint8_t>()};
        const auto tuning = decoded.hunt_tuning_;
        if (tuning.threshold > HuntTuning::max_threshold || tuning.chase_cost > HuntTuning::max_chase_cost ||
            (!decoded.adaptive_hunts_ && !tuning.is_default())) {
            return false;
        }
    }
    if (version >= 11) {
        const auto enabled = reader.get<std::uint8_t>();
        if (enabled > 1 || (enabled && !decoded.adaptive_hunts_)) {
            return false;
        }
        decoded.kin_avoidance_ = enabled != 0;
    }
    if (version >= 12) {
        const auto kin_strength = reader.get<std::uint8_t>();
        const auto resemblance_strength = reader.get<std::uint8_t>();
        const auto protection = reader.get<std::uint8_t>();
        const KinTuning tuning{kin_strength, resemblance_strength, protection != 0};
        if (tuning.kin_strength > KinTuning::max_strength || tuning.resemblance_strength > KinTuning::max_strength ||
            protection > 1 || (!decoded.kin_avoidance_ && !tuning.is_default())) {
            return false;
        }
        decoded.kin_tuning_ = tuning;
    }
    if (version >= 13) {
        const auto markers = reader.get<std::uint8_t>();
        const auto cross_faction = reader.get<std::uint8_t>();
        if (markers > 1 || cross_faction > 1 || (cross_faction && (!markers || !decoded.adaptive_hunts_))) {
            return false;
        }
        decoded.faction_markers_ = markers != 0;
        decoded.cross_faction_hunts_ = cross_faction != 0;
    }
    if (version >= 14) {
        const auto staggered = reader.get<std::uint8_t>();
        const auto fruiting = reader.get<std::uint8_t>();
        if (staggered > 1 || fruiting > 1 || (fruiting && !staggered)) {
            return false;
        }
        decoded.staggered_growth_ = staggered != 0;
        decoded.fruiting_plants_ = fruiting != 0;
    }
    if (version >= 15) {
        const auto dynamic = reader.get<std::uint8_t>();
        if (dynamic > 1 || (dynamic && (!decoded.connected_biomes_ || !decoded.staggered_growth_))) {
            return false;
        }
        decoded.dynamic_environment_ = dynamic != 0;
        decoded.climate_seed_ = reader.get<std::uint32_t>();
        if (!dynamic && decoded.climate_seed_) {
            return false;
        }
        for (auto& rain : decoded.rainfall_) {
            rain = reader.get<std::int8_t>();
            if (rain < -rainfall_limit || rain > rainfall_limit || (!dynamic && rain)) {
                return false;
            }
        }
    }
    auto& metrics = decoded.metrics_;
    metrics.ticks = reader.get<std::uint64_t>();
    metrics.births = reader.get<std::uint64_t>();
    metrics.deaths = reader.get<std::uint64_t>();
    metrics.capacity_blocks = reader.get<std::uint64_t>();
    if (!legacy) {
        metrics.plants_eaten = reader.get<std::uint64_t>();
        metrics.animal_eaten = reader.get<std::uint64_t>();
    }
    if (version >= 3) {
        metrics.attacks = reader.get<std::uint64_t>();
        metrics.kills = reader.get<std::uint64_t>();
        if (metrics.kills > metrics.attacks || metrics.kills > metrics.deaths ||
            (!decoded.predation_ && metrics.attacks)) {
            return false;
        }
    }
    if (version >= 5) {
        metrics.biome_crossings = reader.get<std::uint64_t>();
        metrics.cross_biome_meals = reader.get<std::uint64_t>();
        metrics.cross_biome_hunts = reader.get<std::uint64_t>();
        metrics.biome_shifts = reader.get<std::uint64_t>();
        metrics.feeding_round_trips = reader.get<std::uint64_t>();
        metrics.hunting_round_trips = reader.get<std::uint64_t>();
        if (metrics.cross_biome_meals > metrics.biome_crossings ||
            metrics.cross_biome_hunts > metrics.cross_biome_meals ||
            metrics.cross_biome_hunts > metrics.kills ||
            metrics.feeding_round_trips > metrics.biome_crossings ||
            metrics.hunting_round_trips > metrics.feeding_round_trips ||
            metrics.hunting_round_trips > metrics.kills ||
            (!decoded.connected_biomes_ && (metrics.biome_crossings || metrics.biome_shifts))) {
            return false;
        }
    }
    if (version >= 14) {
        metrics.fruit_eaten = reader.get<std::uint64_t>();
        if (metrics.fruit_eaten > metrics.plants_eaten || (metrics.fruit_eaten && !decoded.fruiting_plants_)) {
            return false;
        }
    }
    auto& nests = metrics.nests;
    if (version >= 8) {
        std::apply([&](auto&... values) { ((values = reader.get<std::uint64_t>()), ...); }, NestMetrics::fields(nests));
        const bool has_metrics = std::apply([](auto... values) { return (... || values); }, NestMetrics::fields(nests));
        if ((!decoded.nests_ && has_metrics) ||
            nests.fledged > metrics.births || nests.starved > metrics.births - nests.fledged ||
            nests.killed > metrics.births - nests.fledged - nests.starved || nests.killed > metrics.kills ||
            nests.attacks > metrics.attacks || nests.killed > nests.attacks || nests.guarded_attacks > nests.attacks ||
            (!decoded.guarding_ && (nests.guarded_attacks || nests.guard_ticks)) ||
            nests.deliveries > nests.food_delivered || (!decoded.provisioning_ && (nests.deliveries || nests.food_delivered)) ||
            nests.both_near > nests.both_alive || nests.after_nest_near > nests.after_nest_alive ||
            nests.repeated_mates > nests.previous_mate_alive ||
            nests.previous_mate_alive > nests.subsequent_matings) {
            return false;
        }
    }
    if (metrics.births >= std::numeric_limits<std::uint64_t>::max() - founder_count) {
        return false;
    }
    decoded.next_id_ = founder_count + metrics.births + 1;
    if (decoded.next_id_ - 1 > max_creature_id) {
        return false;
    }
    const auto total_creatures = decoded.next_id_ - 1;
    if (metrics.deaths > total_creatures || total_creatures - metrics.deaths > capacity) {
        return false;
    }

    std::uint64_t total_material = 0;
    std::uint64_t local_budget = 0;
    for (std::size_t index = 0; index < decoded.cells_.size(); ++index) {
        auto& cell = decoded.cells_[index];
        cell.moisture = reader.get<std::uint8_t>();
        cell.nutrients = reader.get<std::uint8_t>();
        cell.plants = reader.get<std::uint8_t>();
        const std::uint32_t detritus = legacy ? reader.get<std::uint8_t>() : reader.get<std::uint32_t>();
        if (detritus > Cell::max_detritus) {
            return false;
        }
        cell.detritus = static_cast<std::uint16_t>(detritus);
        cell.animal = legacy ? 0 : reader.get<std::uint8_t>();
        if (version >= 14) {
            const auto stock = reader.get<std::uint8_t>();
            if (stock > max_fruit + 1 || (stock && !decoded.fruiting_plants_)) {
                return false;
            }
            decoded.shrubs_[index] = stock;
        }
        const auto material = std::uint64_t(cell.detritus) + cell.nutrients + cell.plants + cell.animal + decoded.fruit_at(index);
        if (index == 0) {
            local_budget = material;
        }
        if (!decoded.carcass_decay_ && (cell.animal || material > 255 || material != local_budget)) {
            return false;
        }
        total_material += material;
    }
    const auto living = total_creatures - metrics.deaths;
    unsigned active_nests = 0;
    std::uint64_t recorded_kills = 0;
    decoded.occupants_.fill(-1);
    for (int slot = 0; slot < capacity; ++slot) {
        auto& creature = decoded.creatures_[slot];
        // IDs are bounded by next_id_, which fits a CreatureId, so they narrow without loss.
        // Lifetime counters wider than 32 bits saturate.
        bool id_overflow = false;
        const auto id = [&] {
            const auto value = reader.get<std::uint64_t>();
            id_overflow |= value > max_creature_id;
            return static_cast<CreatureId>(value);
        };
        const auto lifetime = [&] {
            return static_cast<std::uint32_t>(std::min<std::uint64_t>(reader.get<std::uint64_t>(), 0xffffffffu));
        };
        std::uint64_t wide_ids[3];
        for (auto& value : wide_ids) {
            value = reader.get<std::uint64_t>();
        }
        if (std::any_of(std::begin(wide_ids), std::end(wide_ids), [](std::uint64_t value) { return value > max_creature_id; })) {
            return false;
        }
        creature = {};
        creature.id = static_cast<CreatureId>(wide_ids[0]);
        creature.parent = static_cast<CreatureId>(wide_ids[1]);
        creature.founder = static_cast<CreatureId>(wide_ids[2]);
        creature.generation = reader.get<std::uint32_t>();
        creature.cell = reader.get<std::uint16_t>();
        creature.energy = reader.get<std::uint16_t>();
        creature.genes.digestion = reader.get<std::uint8_t>();
        creature.genes.thrift = reader.get<std::uint8_t>();
        creature.genes.moisture_preference = version >= 5 ? reader.get<std::uint8_t>() : std::uint8_t{0};
        creature.plants_eaten = legacy ? 0 : lifetime();
        creature.animal_eaten = legacy ? 0 : lifetime();
        creature.nutrients_carried = version >= 4 ? reader.get<std::uint8_t>() : std::uint8_t{0};
        creature.excursion = version >= 5 ? static_cast<Excursion>(reader.get<std::uint8_t>()) : Excursion::none;
        if (version >= 7) {
            creature.other_parent = id();
            creature.breeding_cooldown = reader.get<std::uint8_t>();
            creature.genes.mate_drive = reader.get<std::uint8_t>();
            creature.genes.receptivity = reader.get<std::uint8_t>();
        }
        if (version >= 8) {
            creature.last_mate = id();
            creature.nest_ticks = reader.get<std::uint8_t>();
            creature.packed_plants = reader.get<std::uint8_t>();
            creature.packed_animal = reader.get<std::uint8_t>();
            creature.genes.guard_drive = reader.get<std::uint8_t>();
            creature.genes.provision_drive = reader.get<std::uint8_t>();
        }
        if (version >= 9) {
            creature.genes.hunting = reader.get<std::uint8_t>();
            creature.genes.evasion = reader.get<std::uint8_t>();
            creature.hunt_target = id();
            creature.prey_killed = lifetime();
        }
        if (version >= 11) {
            creature.genes.kin_reluctance = reader.get<std::uint8_t>();
        }
        if (version >= 13) {
            creature.genes.faction = reader.get<std::uint8_t>();
        }
        if (version >= 14) {
            creature.packed_fruit = reader.get<std::uint8_t>();
        }
        if (id_overflow) {
            return false;
        }
        const auto faction = creature.genes.faction;
        if (!creature.alive()) {
            if (creature.parent || creature.founder || creature.generation || creature.cell ||
                creature.energy || creature.genes.digestion || creature.genes.thrift ||
                creature.genes.moisture_preference ||
                creature.plants_eaten || creature.animal_eaten || creature.nutrients_carried ||
                creature.excursion != Excursion::none || creature.other_parent ||
                creature.breeding_cooldown || creature.genes.mate_drive || creature.genes.receptivity ||
                creature.last_mate || creature.nest_ticks || creature.provisions() ||
                creature.genes.guard_drive || creature.genes.provision_drive || creature.genes.hunting ||
                creature.genes.evasion || creature.hunt_target || creature.prey_killed ||
                creature.genes.kin_reluctance || faction) {
                return false;
            }
            continue;
        }
        if (creature.id >= decoded.next_id_ || creature.cell >= decoded.cells_.size() ||
            decoded.occupants_[creature.cell] >= 0 || creature.energy == 0 || creature.energy > 160 ||
            !valid_trait(creature.genes.digestion) || !valid_trait(creature.genes.thrift) ||
            creature.plants_eaten > metrics.plants_eaten || creature.animal_eaten > metrics.animal_eaten ||
            (decoded.connected_biomes_ ? !valid_trait(creature.genes.moisture_preference)
                                      : creature.genes.moisture_preference != 0) ||
            creature.excursion > Excursion::hunted ||
            (!decoded.connected_biomes_ && creature.excursion != Excursion::none) ||
            creature.nutrients_carried > body_mass ||
            (!decoded.food_web_ && creature.nutrients_carried) ||
            creature.founder == 0 || creature.founder > founder_count) {
            return false;
        }
        if (decoded.sexual_reproduction_) {
            if (!valid_trait(creature.genes.mate_drive) || !valid_trait(creature.genes.receptivity) ||
                creature.breeding_cooldown > std::max(Creature::maturity_ticks, Creature::recovery_ticks) ||
                (creature.id <= founder_count ? creature.other_parent != 0 :
                    !creature.other_parent || creature.other_parent >= creature.id ||
                    creature.other_parent == creature.parent)) {
                return false;
            }
        } else if (creature.other_parent || creature.breeding_cooldown ||
                   creature.genes.mate_drive || creature.genes.receptivity) {
            return false;
        }
        const int packed = creature.provisions();
        if (creature.packed_fruit && !decoded.fruiting_plants_) {
            return false;
        }
        if (decoded.nests_) {
            if (!valid_trait(creature.genes.guard_drive) || !valid_trait(creature.genes.provision_drive) ||
                creature.last_mate == creature.id || creature.last_mate >= decoded.next_id_ ||
                creature.nest_ticks > Creature::nest_duration || packed > Creature::max_provisions ||
                (packed && !decoded.provisioning_) ||
                (creature.nest_ticks && (!creature.other_parent || creature.last_mate || packed ||
                    creature.breeding_cooldown != creature.nest_ticks + Creature::maturity_ticks - Creature::nest_duration))) {
                return false;
            }
        } else if (creature.last_mate || creature.nest_ticks || packed ||
                   creature.genes.guard_drive || creature.genes.provision_drive) {
            return false;
        }
        if (creature.prey_killed > metrics.kills - recorded_kills ||
            (creature.nest_ticks && (creature.hunt_target || creature.prey_killed))) {
            return false;
        }
        recorded_kills += creature.prey_killed;
        if (decoded.adaptive_hunts_) {
            if (!valid_trait(creature.genes.hunting) || !valid_trait(creature.genes.evasion) ||
                creature.hunt_target == creature.id || creature.hunt_target >= decoded.next_id_) {
                return false;
            }
        } else if (creature.genes.hunting || creature.genes.evasion || creature.hunt_target) {
            return false;
        }
        if (decoded.kin_avoidance_ ? !valid_trait(creature.genes.kin_reluctance) : creature.genes.kin_reluctance != 0) {
            return false;
        }
        if (decoded.faction_markers_ ? faction == 0 || faction > faction_count : faction != 0) {
            return false;
        }
        if (creature.id <= founder_count) {
            if (creature.founder != creature.id || creature.parent != 0 || creature.generation != 0) {
                return false;
            }
        } else if (creature.parent < creature.founder || creature.parent >= creature.id ||
                   creature.generation == 0 || creature.generation > metrics.ticks) {
            return false;
        }
        const auto first = decoded.creatures_.begin();
        if (std::any_of(first, first + slot,
                        [&](const Creature& previous) { return previous.id == creature.id; })) {
            return false;
        }
        total_material += creature.nutrients_carried + packed;
        active_nests += creature.nest_ticks > 0;
        decoded.occupants_[creature.cell] = slot;
        ++metrics.population;
    }
    if (decoded.carcass_decay_) {
        total_material += living * body_mass;
        const auto founder_material = founder_count * body_mass;
        if (total_material < founder_material ||
            total_material - founder_material > decoded.cells_.size() * 255 ||
            (total_material - founder_material) % decoded.cells_.size() != 0) {
            return false;
        }
    }
    if (metrics.population != living) {
        return false;
    }
    if (decoded.nests_ && active_nests != metrics.births - nests.fledged - nests.starved - nests.killed) {
        return false;
    }

    *this = decoded;
    return true;
}

} // namespace petri
