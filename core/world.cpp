#include "world.hpp"

#include <algorithm>
#include <cstdlib>

namespace petri {
namespace {

constexpr int habitat_width = width / 2;
constexpr int habitat_height = height / 2;
constexpr int wet_radius = 5;
constexpr int reproduction_threshold = 100;
constexpr int offspring_energy = 40;
constexpr int reproduction_cost = offspring_energy + 10;
constexpr int attack_cost = 2;
constexpr int pursuit_cost = 2;
constexpr int pursuit_range = 5;
constexpr int hunter_appetite = 45;
constexpr int food_web_reproduction_threshold = 115;
constexpr int minimum_meal_gain = 2;
constexpr int fasting_upkeep = 2;
constexpr int mate_range = 3;
constexpr int parental_energy = reproduction_cost / 2;
constexpr int parental_material = body_mass / 2;

// Separate wetlands keep multiple groups visible on the overview.
constexpr int wetland_columns = 4;
constexpr int wetland_rows = 3;
constexpr int wetland_width = width / wetland_columns;
constexpr int wetland_height = height / wetland_rows;
constexpr int crowding_cost = 36;

int wetland_profile(int index) {
    const int x = index % width;
    const int y = index / width;
    const int region = y / wetland_height * wetland_columns + x / wetland_width;
    const int dx = x % wetland_width - wetland_width / 2 + region % 5 - 2;
    const int dy = y % wetland_height - wetland_height / 2 + region % 3 - 1;
    const int rx = 4 + region % 3;
    const int ry = 3 + region / 3 % 2;
    const int boundary = rx * rx * ry * ry;
    const int distance = dx * dx * ry * ry + dy * dy * rx * rx;
    return distance > boundary ? 0 : 124 + 36 * (boundary - distance) / boundary;
}

int water_supply(int index) {
    if (const int wetland = wetland_profile(index); wetland > 0) {
        return wetland;
    }

    const int x = index % width;
    const int y = index / width;
    const int x_wave = x / 6 % 4;
    const int y_wave = y / 5 % 4;
    const int row_center = wetland_height / 2 + std::min(x_wave, 4 - x_wave) - 1;
    const int col_center = wetland_width / 2 + std::min(y_wave, 4 - y_wave) - 1;
    const int dy = y % wetland_height - row_center;
    const int dx = x % wetland_width - col_center;
    const bool horizontal = x >= 13 && x <= width - 13 && y < wetland_rows * wetland_height;
    const int link = (y - wetland_height / 2) / wetland_height;
    const bool vertical = y >= wetland_height / 2 && y <= height - wetland_height / 2 &&
        (x / wetland_width + link) % 2 == 0;
    const int distance = std::min(horizontal ? std::max(dy, -dy) : 10,
                                  vertical ? std::max(dx, -dx) : 10);
    if (distance <= 1) {
        return 83 - distance * 9;
    }
    if (distance <= 3) {
        return 77 - distance * 10;
    }
    return 24;
}

constexpr int terrain_noise(int x, int y, unsigned salt) {
    constexpr int period = 13;
    const int gx = x / period;
    const int gy = y / period;
    const int ax = x % period;
    const int ay = y % period;
    const auto sample = [salt](int i, int j) {
        std::uint32_t value = std::uint32_t(i) * 0x27d4eb2du ^
            std::uint32_t(j) * 0x85ebca6bu ^ salt;
        value = (value ^ (value >> 15)) * 0xc2b2ae35u;
        return int((value ^ (value >> 16)) % 19) - 9;
    };
    const int top = sample(gx, gy) * (period - ax) + sample(gx + 1, gy) * ax;
    const int bottom = sample(gx, gy + 1) * (period - ax) + sample(gx + 1, gy + 1) * ax;
    return (top * (period - ay) + bottom * ay) / (period * period);
}

struct Basin {
    int x, y, radius_x, radius_y;
};

constexpr Basin basins[] = {
    {9, 8, 5, 4}, {44, 15, 7, 4}, {79, 7, 5, 5}, {116, 13, 6, 4},
    {20, 30, 6, 5}, {52, 25, 8, 4}, {74, 37, 5, 6}, {108, 33, 7, 4},
    {10, 55, 6, 4}, {47, 48, 5, 5}, {83, 56, 7, 4}, {117, 49, 5, 5}
};

constexpr std::array<int, 2> wetland_links[] = {
    {0, 1}, {0, 4}, {1, 5}, {1, 2}, {2, 5}, {2, 3}, {3, 7}, {4, 5},
    {4, 8}, {5, 9}, {5, 6}, {6, 9}, {6, 7}, {6, 10}, {7, 11}, {9, 10}, {10, 11}
};

constexpr int segment_distance_squared(int x, int y, int ax, int ay, int bx, int by) {
    constexpr int unit = 8;
    const int dx = bx - ax;
    const int dy = by - ay;
    const int length = dx * dx + dy * dy;
    const int position = std::clamp(((x - ax) * dx + (y - ay) * dy) * unit / length, 0, unit);
    const int ex = (x - ax) * unit - dx * position;
    const int ey = (y - ay) * unit - dy * position;
    return (ex * ex + ey * ey) / (unit * unit);
}

constexpr std::uint8_t irregular_supply(int x, int y) {
    const int wx = x + terrain_noise(x, y, 0x891ab53u) / 2;
    const int wy = y + terrain_noise(x, y, 0x42bb35fu) / 2;
    const int texture = terrain_noise(x, y, 0x77ea91bu) / 2;
    int supply = 24;
    for (const auto& basin : basins) {
        const int dx = wx - basin.x;
        const int dy = wy - basin.y;
        const int rx = basin.radius_x * basin.radius_x;
        const int ry = basin.radius_y * basin.radius_y;
        const int boundary = rx * ry;
        const int distance = dx * dx * ry + dy * dy * rx;
        if (distance <= boundary) {
            supply = std::max(supply, 126 + 38 * (boundary - distance) / boundary);
        }
    }
    if (supply < 124) {
        for (unsigned i = 0; i < sizeof(wetland_links) / sizeof(*wetland_links); ++i) {
            const auto& a = basins[wetland_links[i][0]];
            const auto& b = basins[wetland_links[i][1]];
            const int bend_x = (a.x + b.x) / 2 + (int(i % 3) - 1) * 5;
            const int bend_y = (a.y + b.y) / 2 + (int(i % 5) - 2) * 3;
            // Avoid evaluating distant links for every cell.
            if (wx < std::min(a.x, b.x) - 9 || wx > std::max(a.x, b.x) + 9 ||
                wy < std::min(a.y, b.y) - 10 || wy > std::max(a.y, b.y) + 10) {
                continue;
            }
            const int distance = std::min(segment_distance_squared(wx, wy, a.x, a.y, bend_x, bend_y),
                                          segment_distance_squared(wx, wy, bend_x, bend_y, b.x, b.y));
            if (distance <= 16) {
                supply = std::max(supply, 87 - distance * 3);
            }
        }
    }
    return static_cast<std::uint8_t>(supply + texture);
}

template<int first_row>
constexpr auto watershed_half() {
    std::array<std::uint8_t, width * height / 2> terrain{};
    if constexpr (wide_world) {
        for (int y = 0; y < height / 2; ++y) {
            for (int x = 0; x < width; ++x) {
                terrain[y * width + x] = irregular_supply(x, first_row + y);
            }
        }
    }
    return terrain;
}

// Separate evaluations keep the flash tables within the MCU compiler's constexpr limit.
constexpr auto north_watershed = watershed_half<0>();
constexpr auto south_watershed = watershed_half<height / 2>();

int water_supply(int index, bool irregular) {
    if (!irregular) {
        return water_supply(index);
    }
    constexpr int half = width * height / 2;
    return index < half ? north_watershed[index] : south_watershed[index - half];
}

int grid_distance(int ax, int ay, int bx, int by) {
    return std::max(ax - bx, bx - ax) + std::max(ay - by, by - ay);
}

int capture_chance(const Creature& prey) {
    return std::clamp(60 - prey.energy / 3, 15, 55);
}

constexpr std::uint32_t kin_trait_salt = 0xb94485a9u;
constexpr std::uint32_t founder_faction_salt = 0xa2cf4c35u;
constexpr std::uint32_t inherited_faction_salt = 0xd28fa43bu;
constexpr std::uint32_t shrub_salt = 0x6d2b79f5u;
// One salt per morphology gene, in the order body size, pigment, pattern, limbs.
constexpr std::uint32_t morphology_salts[] = {0x1b873593u, 0xcc9e2d51u, 0xe6546b64u, 0x5bd1e995u};
constexpr std::uint8_t Genome::*morphology_genes[] = {&Genome::body_size, &Genome::pigment, &Genome::pattern,
                                                      &Genome::limbs};
constexpr unsigned faction_mutation_denominator = 128;
constexpr int shrub_min_foliage = 4;
constexpr unsigned weather_duration = 4096;
constexpr unsigned season_duration = 32768;
constexpr unsigned environment_warmup = 1024;
constexpr std::uint32_t storm_salt = 0x3c6ef372u, wind_salt = 0xa54ff53au;
// Storms recharge a soil-water store at each climate node, in proportion to the
// node's positive bias times cover / storm_unit. The store drains with a time
// constant of soil_drain ticks, so its long-run mean tracks the steady bias.
constexpr int soil_drain = 1024;

bool shrub_habitat(const Cell& cell) {
    return cell.moisture >= 56 && cell.moisture <= 192;
}

constexpr bool plant_growth_tick(unsigned tick) {
    return tick % 64 < 48 && (!wide_world || tick % 8 == 0);
}

std::uint32_t trait_roll(std::uint32_t seed, std::uint32_t id, std::uint32_t salt) {
    auto mixed = seed + id * 0x9e3779b9u + salt;
    mixed = (mixed ^ (mixed >> 13)) * 0xc2b2ae35u;
    return mixed ^ (mixed >> 16);
}

std::uint8_t founder_trait(std::uint32_t seed, std::uint32_t id, std::uint32_t salt) {
    return 5 + trait_roll(seed, id, salt) % 7;
}

std::uint8_t mutate_trait(std::uint8_t trait, unsigned roll) {
    const int change = int(roll == 0) - int(roll == 1);
    return std::clamp(trait + change, Genome::minimum, Genome::maximum);
}

} // namespace

std::array<int, 4> World::neighbors(int cell) {
    return {
        cell % width > 0 ? cell - 1 : -1,
        cell % width < width - 1 ? cell + 1 : -1,
        cell >= width ? cell - width : -1,
        cell < width * (height - 1) ? cell + width : -1
    };
}

World::World(std::uint32_t seed, Config config)
    : rng_(seed), trait_effects_(config.trait_effects), carcass_decay_(config.carcass_decay),
      predation_(config.predation && config.carcass_decay != 0),
      food_web_(config.food_web && config.carcass_decay != 0),
      connected_biomes_(wide_world && config.connected_biomes && config.food_web && config.carcass_decay != 0),
      irregular_terrain_(connected_biomes_ && config.irregular_terrain),
      sexual_reproduction_(food_web_ && config.sexual_reproduction),
      nests_(sexual_reproduction_ && config.nests), guarding_(nests_ && config.guarding),
      provisioning_(nests_ && config.provisioning),
      adaptive_hunts_(food_web_ && predation_ && config.adaptive_hunts),
      passive_prey_(adaptive_hunts_ && config.passive_prey),
      hunt_tuning_(adaptive_hunts_ ? HuntTuning{std::min(config.hunt_tuning.threshold, HuntTuning::max_threshold),
          std::min(config.hunt_tuning.chase_cost, HuntTuning::max_chase_cost)} : HuntTuning{}),
      kin_avoidance_(adaptive_hunts_ && config.kin_avoidance),
      kin_tuning_(kin_avoidance_ ? KinTuning{std::min(config.kin_tuning.kin_strength, KinTuning::max_strength),
          std::min(config.kin_tuning.resemblance_strength, KinTuning::max_strength),
          config.kin_tuning.protect_similar_prey} : KinTuning{}),
      faction_markers_(config.faction_markers || (adaptive_hunts_ && config.cross_faction_hunts)),
      cross_faction_hunts_(adaptive_hunts_ && config.cross_faction_hunts),
      staggered_growth_(config.staggered_growth || config.fruiting_plants || (connected_biomes_ && config.dynamic_environment)),
      fruiting_plants_(config.fruiting_plants),
      dynamic_environment_(connected_biomes_ && config.dynamic_environment),
      storm_fronts_(dynamic_environment_ && config.storm_fronts),
      morphology_(food_web_ && config.morphology),
      neutral_morphology_(morphology_ && config.neutral_morphology),
      climate_seed_(dynamic_environment_ ? seed : 0),
      brains_(food_web_ && predation_ && config.brains && !sexual_reproduction_ && !adaptive_hunts_),
      neutral_brains_(brains_ && config.neutral_brains),
      shuffled_brains_(brains_ && config.shuffled_brains) {
    occupants_.fill(-1);
    for (int index = 0; index < width * height; ++index) {
        auto& cell = cells_[index];
        // Four wet habitats with dry ground between them.
        const int x = index % width % habitat_width - habitat_width / 2;
        const int y = index / width % habitat_height - habitat_height / 2;
        const int supply = connected_biomes_ ? water_supply(index, irregular_terrain_) : 0;
        const bool wet = irregular_terrain_ ? supply >= 120 : wide_world ? wetland_profile(index) > 0 :
            std::max(x, -x) + std::max(y, -y) <= wet_radius;
        const auto variation = random();
        cell.moisture = connected_biomes_
            ? std::clamp(supply + int(variation % 15) - 7, 0, 255)
            : (wet ? 128 + variation % 128 : variation % 64);
        if (wet || (connected_biomes_ && cell.moisture >= 36)) {
            if (carcass_decay_) {
                cell.animal = config.initial_nutrients *
                    std::min<unsigned>(100, config.initial_carrion_percent) / 100;
            }
            const unsigned starting_cover = connected_biomes_ ? (wet ? 22 : 7) : 32;
            cell.plants = std::min<unsigned>(config.initial_nutrients - cell.animal, random() % starting_cover);
        }
        cell.nutrients = config.initial_nutrients - cell.plants - cell.animal;
        if (fruiting_plants_ && cell.plants && shrub_habitat(cell) && trait_roll(seed, index, shrub_salt) % 8 == 0) {
            shrubs_[index] = 1;
        }
    }
    for (int slot = 0; slot < founder_count; ++slot) {
        int cell;
        do {
            if constexpr (wide_world) {
                const int region = slot % (wetland_columns * wetland_rows);
                const int x = (region % wetland_columns) * wetland_width + random() % wetland_width;
                const int y = (region / wetland_columns) * wetland_height + random() % wetland_height;
                cell = y * width + x;
            } else {
                cell = random() % cells_.size();
            }
        } while (occupants_[cell] >= 0 ||
                 (connected_biomes_ ? cells_[cell].moisture < 42 : cells_[cell].moisture < 64));
        const auto id = next_id_++;
        const auto digestion = static_cast<std::uint8_t>(food_web_ ? 5 + random() % 7 : 8);
        const auto preference = static_cast<std::uint8_t>(connected_biomes_ ? 1 + random() % 15 : 0);
        const std::uint8_t mating_trait = sexual_reproduction_ ? 8 : 0;
        const std::uint8_t care_trait = nests_ ? 8 : 0;
        creatures_[slot] = {CreatureId(id), 0, CreatureId(id), 0, static_cast<std::uint16_t>(cell), 65,
                            {digestion, 8, preference, mating_trait, mating_trait, care_trait, care_trait}, 0, 0};
        if (adaptive_hunts_) {
            // Keep the ecological seed stream aligned with the control at initialization.
            creatures_[slot].genes.hunting = founder_trait(seed, id, 0x85ebca6bu);
            creatures_[slot].genes.evasion = founder_trait(seed, id, 0x21f0aaadu);
        }
        if (kin_avoidance_) {
            creatures_[slot].genes.kin_reluctance = founder_trait(seed, id, kin_trait_salt);
        }
        if (morphology_) {
            // Standing variation on separate draws, like the hunting traits. Pigment spans
            // every background so that no habitat starts without a match.
            auto& genes = creatures_[slot].genes;
            genes.body_size = founder_trait(seed, id, morphology_salts[0]);
            genes.pigment = static_cast<std::uint8_t>(1 + trait_roll(seed, id, morphology_salts[1]) % 15);
            genes.pattern = founder_trait(seed, id, morphology_salts[2]);
            genes.limbs = founder_trait(seed, id, morphology_salts[3]);
        }
        occupants_[cell] = slot;
        ++metrics_.population;
    }
    if (faction_markers_) {
        // Shuffle equal groups independently of the draws for placement and physiology.
        static_assert(founder_count % faction_count == 0);
        for (int slot = 0; slot < founder_count; ++slot) {
            creatures_[slot].genes.faction = 1 + slot % faction_count;
        }
        for (int slot = founder_count - 1; slot > 0; --slot) {
            const auto other = trait_roll(seed, slot, founder_faction_salt) % (slot + 1);
            std::swap(creatures_[slot].genes.faction, creatures_[other].genes.faction);
        }
    }
    if (brains_) {
        found_brains(seed, config.random_founder_brains);
    }
}

std::uint32_t World::random() {
    // Keep all 32-bit seeds valid, including zero.
    std::uint32_t value = (rng_ += 0x9e3779b9u);
    value = (value ^ (value >> 16)) * 0x85ebca6bu;
    value = (value ^ (value >> 13)) * 0xc2b2ae35u;
    return value ^ (value >> 16);
}

void World::step(StepObserver observer) {
    ++metrics_.ticks;
    if (dynamic_environment_ && metrics_.ticks >= environment_warmup) {
        update_climate();
    }
    if (staggered_growth_) {
        update_staggered_environment();
    } else if (metrics_.ticks % 2 == 0) {
        update_plants();
    }
    if (food_web_ && (staggered_growth_ || metrics_.ticks % 4 == 0)) {
        transport_nutrients();
    }
    if (connected_biomes_ && !staggered_growth_ && metrics_.ticks % 16 == 0) {
        for (int index = 0; index < width * height; ++index) {
            update_moisture(index);
        }
    }
    if (sexual_reproduction_) {
        for (auto& creature : creatures_) {
            creature.breeding_cooldown -= creature.breeding_cooldown > 0;
        }
    }
    if (nests_) {
        prepare_nests();
    }
    if (adaptive_hunts_) {
        prepare_evasion(observer);
    }
    if (brains_) {
        step_brains(observer);
        return;
    }
    const auto first_newborn_id = next_id_;
    for (int slot = 0; slot < capacity; ++slot) {
        const auto& creature = creatures_[slot];
        if (creature.alive() && creature.id < first_newborn_id && !evaded_[slot]) {
            update_creature(slot, observer);
        }
    }
}

void World::update_plants() {
    const auto tick = static_cast<unsigned>(metrics_.ticks);
    const bool growth_tick = plant_growth_tick(tick);
    for (auto& cell : cells_) {
        recycle(cell);
        if (growth_tick) {
            grow_plants(cell, tick);
        }
    }
}

void World::update_staggered_environment() {
    // Odd strides interleave the cells' 64-tick clocks spatially. Each cell is
    // visited on alternate ticks, with the same growth and recycling cadence.
    const auto tick = static_cast<unsigned>(metrics_.ticks);
    for (int y = 0; y < height; ++y) {
        const unsigned row_tick = tick + 17u * y;
        for (int x = row_tick % 2; x < width; x += 2) {
            const unsigned local_tick = row_tick + 29u * x;
            const int index = y * width + x;
            auto& cell = cells_[index];
            recycle(cell);
            if (plant_growth_tick(local_tick)) {
                grow_plants(cell, local_tick);
            }
            if (fruiting_plants_ && fruiting_shrub(index)) {
                update_fruit(index, local_tick);
            }
            if (connected_biomes_ && local_tick % 16 == 0) {
                update_moisture(index);
            }
        }
    }
}

void World::add_detritus(Cell& cell, unsigned amount) {
    const unsigned room = Cell::max_detritus - cell.detritus;
    const unsigned stored = std::min(amount, room);
    cell.detritus += stored;
    metrics_.detritus_clipped += amount - stored;
}

void World::recycle(Cell& cell) {
    // Most cells hold neither carcass nor detritus. Both steps below are then
    // no-ops, so skipping them leaves every result unchanged.
    if (!cell.animal && !cell.detritus) {
        return;
    }
    const int decay = std::min(cell.animal, carcass_decay_);
    cell.animal -= decay;
    add_detritus(cell, decay);
    const auto recycled = std::min<std::uint32_t>({2, cell.detritus, 255u - cell.nutrients});
    cell.detritus -= recycled;
    cell.nutrients += recycled;
}

void World::grow_plants(Cell& cell, unsigned tick) {
    // Growth is capped by soil nutrients and by the plant ceiling, so a cell
    // with either exhausted cannot change.
    if (!cell.nutrients || cell.plants == 255) {
        return;
    }
    // Wetter regions can expand under the weather field. Slower regrowth keeps
    // those larger feeding grounds from multiplying the world's food supply.
    if (dynamic_environment_ && metrics_.ticks >= environment_warmup && tick % 32 == 8) {
        return;
    }
    int growth_rate = cell.moisture / 64;
    if (connected_biomes_) {
        switch (biome_at(cell.moisture)) {
        case Biome::wetland:
            growth_rate = irregular_terrain_ ? 1 : 2;
            break;
        case Biome::grassland:
            growth_rate = tick % (irregular_terrain_ ? 32 : 16) == 0;
            break;
        case Biome::scrub:
            growth_rate = cell.moisture >= 36 && tick % 32 == 0;
            break;
        }
    }
    const int growth = std::min({growth_rate, int(cell.nutrients), 255 - cell.plants});
    cell.nutrients -= growth;
    cell.plants += growth;
}

void World::update_fruit(int index, unsigned tick) {
    auto& cell = cells_[index];
    auto stock = shrubs_[index];
    const auto phase = tick % 64;
    if (phase == 32) {
        if (dynamic_environment_ && fruit_at(index)) {
            const auto roll = trait_roll(climate_seed_, index, static_cast<unsigned>(metrics_.ticks / 64) ^ shrub_salt);
            const int target = neighbors(index)[roll % 4];
            if ((roll / 4) % 64 == 0 && target >= 0 && !fruiting_shrub(target) && shrub_habitat(cells_[target]) &&
                cells_[target].plants >= shrub_min_foliage && cells_[target].plants < 255) {
                // One uneaten fruit seeds an existing patch and becomes seedling tissue.
                --stock;
                ++cells_[target].plants;
                shrubs_[target] = 1;
            }
        }
        add_detritus(cell, fruit_at(index));
        stock = 1;
    } else if (phase == 0 && cell.plants > shrub_min_foliage && shrub_habitat(cell)) {
        const int ripe = std::min(max_fruit - fruit_at(index), cell.plants - shrub_min_foliage);
        cell.plants -= ripe;
        stock += ripe;
    }
}

void World::transport_nutrients() {
    // Four alternating sets of disjoint horizontal and vertical edges.
    const auto tick = static_cast<unsigned>(metrics_.ticks);
    const unsigned pass = ((tick + (staggered_growth_ ? 3 : 0)) / 4) % 4;
    // When staggered, split each edge set over its four ticks.
    const int lane = staggered_growth_ ? (tick - 1) % 4 : 0;
    const int lanes = staggered_growth_ ? 4 : 1;
    const bool horizontal = pass < 2;
    const int stride = horizontal ? 1 : width;
    for (int y = horizontal ? lane : pass % 2; y < height - !horizontal; y += horizontal ? lanes : 2) {
        for (int x = horizontal ? pass % 2 : lane; x < width - horizontal; x += horizontal ? 2 : lanes) {
            auto& from = cells_[y * width + x];
            auto& to = cells_[y * width + x + stride];
            // Wet ground attracts dissolved nutrients.
            const int bias = 16 * ((to.moisture >= 64) - (from.moisture >= 64));
            const int amount = std::clamp((from.nutrients - to.nutrients + bias) / 3,
                -std::min(int(to.nutrients), 255 - from.nutrients),
                std::min(int(from.nutrients), 255 - to.nutrients));
            from.nutrients -= amount;
            to.nutrients += amount;
        }
    }
}

void World::update_moisture(int index) {
    auto& cell = cells_[index];
    const auto before = biome_at(cell.moisture);
    // Vegetation holds water. Foot traffic and crowding dry the surface.
    const int retained = std::min(18, cell.plants * 2);
    const int pressure = (occupants_[index] >= 0 ? 9 : 0) + 3 * occupied_neighbors(index);
    const bool weather_active = dynamic_environment_ && metrics_.ticks >= environment_warmup;
    const int rainfall = weather_active ? rainfall_at(index) : 0;
    const int supply = water_supply(index, irregular_terrain_);
    const int spring = weather_active && supply >= 144 ? 128 : 0;
    const int target = std::clamp(supply + rainfall + retained - pressure, spring, 255);
    cell.moisture += std::clamp(target - cell.moisture, -2, 2);
    metrics_.biome_shifts += before != biome_at(cell.moisture);
    if (weather_active && cell.moisture < 56) {
        if (cell.plants) {
            --cell.plants;
            add_detritus(cell, 1);
        }
        if (!cell.plants && fruiting_shrub(index)) {
            add_detritus(cell, fruit_at(index));
            shrubs_[index] = 0;
        }
    }
}

void World::update_climate() {
    // One coarse node advances per tick. Its wet/dry target changes on a slower
    // clock, and neighboring cells sample the same smoothly blended field.
    const auto index = static_cast<std::size_t>(metrics_.ticks % rainfall_.size());
    const auto epoch = static_cast<std::uint32_t>((metrics_.ticks - environment_warmup) / weather_duration);
    const int target = int(trait_roll(climate_seed_, index, epoch ^ 0x53b5d731u) % (2 * rainfall_limit + 1)) - rainfall_limit;
    rainfall_[index] += std::clamp(target - rainfall_[index], -2, 2);
    if (storm_fronts_) {
        update_storms();
        update_soil_water();
    }
}

void World::update_storms() {
    constexpr std::int32_t unit = 256;
    for (auto& storm : storms_) {
        if (!storm.active()) {
            continue;
        }
        storm.x += storm.vx;
        storm.y += storm.vy;
        const std::int32_t margin = (storm.radius + 2) * unit;
        if (storm.x < -margin || storm.x > width * unit + margin || storm.y < -margin || storm.y > height * unit + margin) {
            storm = {};
        }
    }
    const auto free = std::find_if(storms_.begin(), storms_.end(), [](const Storm& storm) { return !storm.active(); });
    if (free == storms_.end()) {
        return;
    }
    // The wet season breeds storms about thirteen times as often as the dry one.
    const auto tick = static_cast<std::uint32_t>(metrics_.ticks);
    const auto roll = trait_roll(climate_seed_, tick, storm_salt);
    const int wetness = std::clamp(seasonal_bias() + 16, 0, 32);
    if (roll % 4096 >= static_cast<unsigned>(8 + 3 * wetness)) {
        return;
    }
    // One prevailing wind per weather epoch, from one of eight directions.
    static constexpr std::int8_t directions[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
    const auto epoch = static_cast<std::uint32_t>((metrics_.ticks - environment_warmup) / weather_duration);
    const auto wind = trait_roll(climate_seed_, epoch, wind_salt);
    const auto& direction = directions[wind % 8];
    const auto shape = trait_roll(climate_seed_, tick, storm_salt ^ wind_salt);
    // 0.19 to 0.43 cells a tick; diagonal winds keep about the same speed.
    const int speed = 48 + static_cast<int>((wind >> 8) % 64) + static_cast<int>(shape % 9) - 4;
    const int diagonal = direction[0] && direction[1] ? 181 : 256;
    Storm storm;
    storm.vx = static_cast<std::int16_t>(direction[0] * speed * diagonal / 256);
    storm.vy = static_cast<std::int16_t>(direction[1] * speed * diagonal / 256);
    storm.radius = static_cast<std::uint8_t>(8 + (shape >> 8) % 14);
    storm.strength = static_cast<std::uint8_t>(6 + (shape >> 16) % 6);
    // Start just off the upwind edge, at a random point along it.
    const std::int32_t edge = storm.radius * unit;
    const auto along = shape >> 20;
    if (std::abs(storm.vx) >= std::abs(storm.vy)) {
        storm.x = storm.vx > 0 ? -edge : width * unit + edge;
        storm.y = static_cast<std::int32_t>(along % (height * unit));
    } else {
        storm.y = storm.vy > 0 ? -edge : height * unit + edge;
        storm.x = static_cast<std::int32_t>(along % (width * unit));
    }
    *free = storm;
}

void World::update_soil_water() {
    // Each node's store is in 1/256 bias units.
    const int seasonal = seasonal_bias();
    for (int node = 0; node < climate_nodes; ++node) {
        const int col = node % climate_columns, row = node / climate_columns;
        const int x = std::min(col * climate_spacing, width - 1), y = std::min(row * climate_spacing, height - 1);
        const int bias = rainfall_[node] + seasonal;
        const std::int32_t recharge = bias > 0 ? bias * storm_cover(std::size_t(y) * width + x) / storm_unit : 0;
        auto& store = soil_water_[node];
        store += (recharge * 256 - store) / soil_drain;
    }
}

int World::storm_cover(std::size_t cell) const {
    return storm_cover_at(storms_, cell);
}

int storm_cover_at(const std::array<Storm, storm_slots>& storms, std::size_t cell) {
    // Distances in 1/16 cell keep the squares within 32 bits.
    const std::int32_t cx = static_cast<std::int32_t>(cell % width) * 16 + 8;
    const std::int32_t cy = static_cast<std::int32_t>(cell / width) * 16 + 8;
    int cover = 0;
    for (const auto& storm : storms) {
        if (!storm.active()) {
            continue;
        }
        const std::int32_t dx = cx - storm.x / 16, dy = cy - storm.y / 16;
        const std::int32_t reach = storm.radius * 16;
        const std::int32_t distance = dx * dx + dy * dy, limit = reach * reach;
        if (distance < limit) {
            cover += static_cast<int>((limit - distance) * 256 / limit) * storm.strength;
        }
    }
    return cover;
}

int World::rainfall_at(std::size_t cell) const {
    const int bias = weather_bias(static_cast<int>(cell));
    if (!storm_fronts_ || bias <= 0) {
        return bias;
    }
    // Wet ground reads the soil water the storms left, blended like the bias.
    const int x = static_cast<int>(cell % width), y = static_cast<int>(cell / width);
    const int col = x / climate_spacing, row = y / climate_spacing;
    const int dx = x % climate_spacing, dy = y % climate_spacing;
    const int node = row * climate_columns + col;
    const auto& w = soil_water_;
    const std::int64_t north = std::int64_t(w[node]) * (climate_spacing - dx) + std::int64_t(w[node + 1]) * dx;
    const std::int64_t south = std::int64_t(w[node + climate_columns]) * (climate_spacing - dx) +
                               std::int64_t(w[node + climate_columns + 1]) * dx;
    return static_cast<int>((north * (climate_spacing - dy) + south * dy) / (climate_spacing * climate_spacing * 256));
}

int World::storm_rain_at(std::size_t cell) const {
    const int bias = weather_at(cell);
    return storm_fronts_ && bias > 0 ? bias * storm_cover(cell) / storm_unit : 0;
}

int World::weather_bias(int index) const {
    const int x = index % width, y = index / width;
    const int col = x / climate_spacing, row = y / climate_spacing;
    const int dx = x % climate_spacing, dy = y % climate_spacing;
    const int node = row * climate_columns + col;
    const int north = rainfall_[node] * (climate_spacing - dx) + rainfall_[node + 1] * dx;
    const int south = rainfall_[node + climate_columns] * (climate_spacing - dx) +
        rainfall_[node + climate_columns + 1] * dx;
    const int regional = (north * (climate_spacing - dy) + south * dy) / (climate_spacing * climate_spacing);
    return regional + seasonal_bias();
}

int World::seasonal_bias() const {
    const auto phase = static_cast<unsigned>((metrics_.ticks - environment_warmup + season_duration / 4) % season_duration);
    const int ramp = phase < season_duration / 2 ? phase : season_duration - phase;
    return ramp * 32 / (season_duration / 2) - 16;
}

bool World::weather_active() const {
    return dynamic_environment_ && metrics_.ticks >= environment_warmup;
}

int World::occupied_neighbors(int cell) const {
    int count = 0;
    for (const int neighbor : neighbors(cell)) {
        if (neighbor >= 0 && occupants_[neighbor] >= 0) {
            ++count;
        }
    }
    return count;
}

void World::update_creature(int slot, StepObserver observer) {
    auto& creature = creatures_[slot];
    if (creature.nest_ticks) {
        update_young(creature, observer);
        return;
    }
    auto* young = nests_ ? dependent(slot) : nullptr;
    if (!young && creature.provisions()) {
        add_detritus(cells_[creature.cell], creature.provisions());
        creature.packed_plants = creature.packed_animal = creature.packed_fruit = 0;
    }
    const int nest_pull = young ? care_pull(creature, *young) : 0;
    // The neutral control inherits and mutates genes with founder physiology.
    const Genome effective = trait_effects_ ? creature.genes : Genome{8, 8, 8, 8, 8, 8, 8, 8, 8};
    const auto habitat_distance = [&](const Cell& cell) {
        const int difference = effective.preferred_moisture() - cell.moisture;
        return std::max(difference, -difference);
    };
    const int bite_limit = this->bite_limit(creature, effective);
    const int fiber_cost = food_web_ ? std::max(0, 8 - int(effective.digestion)) * 3 / 2 : 0;
    const auto plant_energy = [&](const Cell& cell) {
        // Digestion biased toward meat pays more to process plant fiber.
        return std::min(int(cell.plants), bite_limit) * effective.digestion / 10 - fiber_cost;
    };
    const auto meat_energy = [&](const Cell& cell) {
        return std::min(int(cell.animal), bite_limit) * 2 * effective.meat_digestion() / 10;
    };
    const int fruit_value = effective.digestion + 8;
    const auto fruit_energy = [&](int index) {
        return fruit_at(index) * fruit_value / 10;
    };
    const auto meal = [&](int index) {
        const auto& cell = cells_[index];
        return std::max({plant_energy(cell), meat_energy(cell), fruit_energy(index)});
    };
    const int mate = sexual_reproduction_ && can_mate(creature) ? locate_mate(creature) : -1;
    const int mate_drive = trait_effects_ ? creature.genes.mate_drive : 8;
    const auto forage_value = [&](int index) {
        const auto& cell = cells_[index];
        const int base_food = carcass_decay_
            ? std::max(cell.plants * effective.digestion, cell.animal * 2 * effective.meat_digestion())
            : int(cell.plants);
        const int food = std::max(base_food, fruit_at(index) * fruit_value);
        const int mate_cost = mate < 0 ? 0 :
            mate_drive * 18 * grid_distance(index % width, index / width, mate % width, mate / width);
        const int nest_cost = nest_pull ? nest_pull * cell_distance(index, young->cell) : 0;
        if constexpr (wide_world) {
            // The moving creature would vacate its old cell.
            const int self = index != creature.cell;
            const int mismatch = connected_biomes_ ? habitat_distance(cell) : 0;
            return food - crowding_cost * (occupied_neighbors(index) - self) - mismatch - mate_cost - nest_cost;
        }
        return food - mate_cost - nest_cost;
    };

    const auto adjacent = neighbors(creature.cell);
    int destination = -1;
    int prey_slot = -1;
    const int meat_digestion = effective.meat_digestion();
    const auto recovered_energy = [&](int energy) {
        return hunt_energy(energy, meat_digestion, adaptive_hunts_);
    };
    const auto chance = [&](const Creature& prey) {
        if (!food_web_) {
            return capture_chance(prey);
        }
        const int vegetation_cover = std::min(30, cells_[prey.cell].plants * 30 / 12);
        const int open_nest = prey.nest_ticks ? 36 : 0;
        const int protection = prey.nest_ticks && guarded(prey) ? 52 : 0;
        return std::clamp(48 - prey.energy / 4 + (meat_digestion - 8) * 18 - vegetation_cover +
                          open_nest - protection + body_odds(creature, prey), 5, 92);
    };
    const auto expected = [&](const Creature& prey) {
        if (prey.nest_ticks && prey.has_parent(creature.id)) {
            return 0;
        }
        const int bite_gain = std::min(body_mass, bite_limit) * meat_digestion * 2 / 10;
        return chance(prey) * (bite_gain + (food_web_ ? recovered_energy(prey.energy) : 0));
    };
    const auto offset = random() % adjacent.size();
    for (unsigned i = 0; i < adjacent.size(); ++i) {
        const int candidate = adjacent[(i + offset) % adjacent.size()];
        if (candidate < 0) {
            continue;
        }
        const int occupant = occupants_[candidate];
        if (occupant < 0) {
            if (destination < 0 || forage_value(candidate) > forage_value(destination)) {
                destination = candidate;
            }
        } else if (predation_ && !adaptive_hunts_ &&
                   !(creatures_[occupant].nest_ticks && creatures_[occupant].has_parent(creature.id))) {
            const auto& prey = creatures_[occupant];
            if (prey_slot < 0 || (food_web_
                    ? expected(prey) > expected(creatures_[prey_slot])
                    : prey.energy < creatures_[prey_slot].energy)) {
                prey_slot = occupant;
            }
        }
    }

    bool seeking_food = false;
    if (dynamic_environment_ && destination >= 0 && creature.energy < 85 &&
        std::max(meal(creature.cell), meal(destination)) <= minimum_meal_gain) {
        // Long limbs widen the search for food; short ones narrow it.
        const int range = 4 + limb_reach(creature);
        const int x = creature.cell % width, y = creature.cell / width;
        int best = minimum_meal_gain;
        for (int yy = std::max(0, y - range); yy <= std::min(height - 1, y + range); ++yy) {
            const int span = range - std::abs(yy - y);
            for (int xx = std::max(0, x - span); xx <= std::min(width - 1, x + span); ++xx) {
                const int target = yy * width + xx;
                const int distance = cell_distance(creature.cell, target);
                if (distance < 2 || occupants_[target] >= 0) {
                    continue;
                }
                const int gain = meal(target) - distance;
                if (gain > best) {
                    const int step = step_toward(creature.cell, target, offset);
                    if (step >= 0) {
                        destination = step;
                        best = gain;
                        seeking_food = true;
                    }
                }
            }
        }
    }

    // Thrift lowers upkeep but reduces the amount an organism can eat per tick.
    const int investment = adaptive_hunts_ && effective.hunting >= 11 ? 1 : 0;
    // A larger body and longer limbs cost upkeep, and smaller ones save it. Living always costs something.
    const int body_cost = body_upkeep(creature);
    int cost = std::max(1, 5 - effective.thrift / 4 + investment + body_cost);
    const int origin = creature.cell;
    int hunting_chance = 0;
    int captured_energy = 0;
    int hunt_step = -1;
    int strike_cost = attack_cost;
    int move_cost = pursuit_cost;
    auto decision = HuntDecision::forage_or_rest;
    if (adaptive_hunts_) {
        int forage = meal(origin);
        if (destination >= 0) {
            forage = std::max(forage, meal(destination) - 1);
        }
        const auto plan = plan_hunt(creature, effective, offset, forage, cost);
        decision = plan.decision;
        prey_slot = plan.prey;
        hunt_step = plan.step;
        hunting_chance = plan.chance;
        strike_cost = plan.strike_cost;
        move_cost = plan.chase_cost;
        creature.hunt_target = prey_slot >= 0 ? creatures_[prey_slot].id : 0;
    } else {
        const bool willing_to_hunt = !food_web_ || creature.energy < hunter_appetite;
        if (prey_slot >= 0 && willing_to_hunt && creature.energy > cost + attack_cost) {
            int forage_gain = meal(origin);
            if (destination >= 0) {
                forage_gain = std::max(forage_gain, meal(destination) - 1);
            }
            const int chance_to_capture = chance(creatures_[prey_slot]);
            if (expected(creatures_[prey_slot]) > 100 * (attack_cost + forage_gain)) {
                hunting_chance = chance_to_capture;
            }
        }

        if (food_web_ && predation_ && willing_to_hunt && hunting_chance == 0 && destination >= 0) {
            const int x = origin % width;
            const int y = origin / width;
            int best = 100 * (attack_cost + pursuit_cost + std::max(meal(origin), meal(destination) - 1));
            const int range = pursuit_range + limb_reach(creature);
            for (int dy = -range; dy <= range; ++dy) {
                const int yy = y + dy;
                if (yy < 0 || yy >= height) {
                    continue;
                }
                const int row_range = range - std::max(dy, -dy);
                for (int dx = -row_range; dx <= row_range; ++dx) {
                    const int xx = x + dx;
                    const int distance = grid_distance(x, y, xx, yy);
                    if (xx < 0 || xx >= width || distance < 2) {
                        continue;
                    }
                    const int prey = occupants_[yy * width + xx];
                    // Prey that matches its background must be closer to be seen.
                    if (prey < 0 || distance > range - concealment(creatures_[prey])) {
                        continue;
                    }
                    const int score = expected(creatures_[prey]) - distance * 240;
                    if (score <= best) {
                        continue;
                    }
                    for (unsigned i = 0; i < adjacent.size(); ++i) {
                        const int step = adjacent[(i + offset) % adjacent.size()];
                        if (step >= 0 && occupants_[step] < 0 &&
                            grid_distance(step % width, step / width, xx, yy) < distance) {
                            best = score;
                            hunt_step = step;
                            break;
                        }
                    }
                }
            }
        }
        if (observer.record_hunt_decision && predation_) {
            if (hunting_chance > 0) {
                decision = HuntDecision::strike;
            } else if (hunt_step >= 0) {
                decision = HuntDecision::pursue;
            } else if (!willing_to_hunt) {
                decision = HuntDecision::sated;
            } else if (creature.energy <= cost + attack_cost) {
                decision = HuntDecision::low_energy;
            } else {
                decision = skipped_hunt(creature, food_web_ ? pursuit_range : 1, offset);
            }
        }
    }
    if (observer.record_hunt_decision && predation_) {
        observer.record_hunt_decision(observer.context, decision);
    }

    bool captured = false;
    int chase_paid = 0, strike_paid = 0;
    // Limb length sets foraging speed on a fixed schedule: short limbs sometimes
    // miss a foraging step, and long limbs sometimes take a second.
    const int stride = limb_stride(creature);
    bool foraging_move = false;
    if (hunting_chance > 0) {
        if (adaptive_hunts_ && hunt_step >= 0) {
            creature.cell = hunt_step;
            chase_paid = move_cost;
        }
        const auto strike_from = creature.cell;
        ++metrics_.attacks;
        const auto& target = creatures_[prey_slot];
        if (target.nest_ticks) {
            ++metrics_.nests.attacks;
            metrics_.nests.guarded_attacks += guarded(target);
        }
        strike_paid = strike_cost;
        cost += strike_paid + chase_paid;
        const bool landed = int(random() % 100) < hunting_chance;
        if (observer.record_attack) {
            observer.record_attack(observer.context, creature, target, landed);
        }
        if (landed) {
            auto& prey = creatures_[prey_slot];
            const auto prey_cell = prey.cell;
            captured_energy = food_web_ ? recovered_energy(prey.energy) : 0;
            metrics_.nests.killed += prey.nest_ticks > 0;
            if (observer.record_kill) {
                observer.record_kill(observer.context, creature, prey);
            }
            kill(prey, observer, DeathCause::predation);
            creature.cell = prey_cell;
            ++metrics_.kills;
            creature.prey_killed += creature.prey_killed < 0xffffffffu;
            creature.hunt_target = 0;
            if (observer.record_hunt) {
                observer.record_hunt(observer.context, strike_from, prey_cell);
            }
            captured = true;
        }
    } else if (hunt_step >= 0) {
        creature.cell = hunt_step;
        chase_paid = move_cost;
        cost += chase_paid;
    } else if (destination >= 0 &&
               (seeking_food || forage_value(destination) > forage_value(origin) ||
                (connected_biomes_ && creature.energy < 85 && cells_[destination].moisture >= 32 &&
                 meal(origin) <= minimum_meal_gain &&
                 forage_value(destination) + 48 >= forage_value(origin)))) {
        foraging_move = true;
        if (stride >= 0) {
            creature.cell = destination;
            ++cost;
        }
    }
    bool crossed = move_creature(slot, origin, captured);
    if (creature.cell != origin) {
        destination = origin;
    }
    if (foraging_move && stride > 0) {
        // A second step, to a better neighbor of the new cell, at the usual cost.
        const auto ahead = neighbors(creature.cell);
        int next = -1;
        for (unsigned i = 0; i < ahead.size(); ++i) {
            const int candidate = ahead[(i + offset) % ahead.size()];
            if (candidate >= 0 && occupants_[candidate] < 0 &&
                (next < 0 || forage_value(candidate) > forage_value(next))) {
                next = candidate;
            }
        }
        if (next >= 0 && forage_value(next) > forage_value(creature.cell)) {
            const int from = creature.cell;
            creature.cell = static_cast<std::uint16_t>(next);
            ++cost;
            crossed |= move_creature(slot, from, false);
            destination = from;
        }
    }
    const auto destination_biome = biome_at(cells_[creature.cell].moisture);
    const auto preferred_biome = biome_at(effective.preferred_moisture());

    const auto& local = cells_[creature.cell];
    const bool eating = !food_web_ || meal(creature.cell) > minimum_meal_gain;
    if (hunting_chance == 0 && creature.cell == origin && !eating) {
        cost = std::max(1, fasting_upkeep + investment + body_cost);
    }
    if (connected_biomes_) {
        cost += habitat_distance(local) / 50;
    }
    int energy = creature.energy - cost + captured_energy;
    int meal_energy = 0;
    if ((hunting_chance == 0 && eating && !(adaptive_hunts_ && hunt_step >= 0)) || captured) {
        auto& cell = cells_[creature.cell];
        const int fruit_gain = fruit_energy(creature.cell);
        const int foliage_gain = plant_energy(cell);
        const bool eat_meat = captured || meat_energy(cell) > std::max(foliage_gain, fruit_gain);
        const auto food = eat_meat ? Food::meat : fruit_gain > foliage_gain ? Food::fruit : Food::foliage;
        const int gathered = gather_food(creature.cell, food, bite_limit);
        const bool feeding_young = young && young->nest_ticks && young->has_parent(creature.id);
        const int bite = gathered - (provisioning_ && feeding_young ? pack_food(creature, food, gathered) : 0);
        const int efficiency = eat_meat ? meat_digestion : effective.digestion;
        const int retained = food_web_
            ? std::min({bite, body_mass - creature.nutrients_carried, (bite * efficiency + 10) / 20})
            : 0;
        creature.nutrients_carried += retained;
        add_detritus(cell, bite - retained);
        if (bite > 0 && crossed) {
            ++metrics_.cross_biome_meals;
        }
        if (bite > 0 && destination_biome != preferred_biome &&
            creature.excursion != Excursion::none) {
            if (captured) {
                creature.excursion = Excursion::hunted;
            } else if (creature.excursion == Excursion::away) {
                creature.excursion = Excursion::fed;
            }
        }
        record_food(creature, food, bite, observer);
        meal_energy = eat_meat ? bite * 2 * meat_digestion / 10
            : food == Food::fruit ? bite * fruit_value / 10 : bite * effective.digestion / 10 - fiber_cost;
        energy += meal_energy;
    }
    if (observer.record_hunt_energy && (hunting_chance > 0 || hunt_step >= 0)) {
        const auto units = [](int value) { return static_cast<std::uint64_t>(value); };
        const int cap_loss = std::max(0, energy - max_energy(creature));
        observer.record_hunt_energy(observer.context,
            {units(chase_paid), units(strike_paid), units(cost - chase_paid - strike_paid),
             units(captured_energy), captured ? units(meal_energy) : 0,
             captured ? units(cap_loss) : 0, captured ? 0 : meal_energy - cap_loss});
    }
    if (energy <= 0) {
        kill(creature, observer, DeathCause::depleted_energy);
        return;
    }
    creature.energy = std::min(energy, max_energy(creature));
    if (provisioning_ && young && young->nest_ticks && young->has_parent(creature.id) &&
        cell_distance(creature.cell, young->cell) == 1) {
        deliver_food(creature, *young, observer);
    }
    if (sexual_reproduction_) {
        if (can_mate(creature)) {
            reproduce_sexually(creature, observer);
        }
    } else if (destination >= 0 &&
               creature.energy >= body_share(food_web_ ? food_web_reproduction_threshold : reproduction_threshold,
                                             creature)) {
        reproduce(creature, destination, observer);
    }
}

int World::gather_food(int index, Food food, int bite_limit) {
    if (food == Food::fruit) {
        const int bite = std::min(fruit_at(index), bite_limit);
        shrubs_[index] -= bite;
        return bite;
    }
    auto& stock = food == Food::meat ? cells_[index].animal : cells_[index].plants;
    const int bite = std::min(int(stock), bite_limit);
    stock -= bite;
    return bite;
}

void World::record_food(Creature& creature, Food food, int amount, StepObserver observer) {
    const bool meat = food == Food::meat;
    auto& lifetime = meat ? creature.animal_eaten : creature.plants_eaten;
    lifetime = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::uint64_t(lifetime) + amount, 0xffffffffu));
    (meat ? metrics_.animal_eaten : metrics_.plants_eaten) += amount;
    if (food == Food::fruit) {
        metrics_.fruit_eaten += amount;
    }
    if (amount > 0 && observer.record_meal) {
        observer.record_meal(observer.context, creature, meat);
    }
}


bool World::move_creature(int slot, int origin, bool captured) {
    auto& creature = creatures_[slot];
    const auto origin_biome = biome_at(cells_[origin].moisture);
    const auto destination_biome = biome_at(cells_[creature.cell].moisture);
    const int moisture = trait_effects_ ? creature.genes.preferred_moisture() : Genome{8, 8, 8}.preferred_moisture();
    const auto preferred_biome = biome_at(moisture);
    const bool crossed = connected_biomes_ && origin_biome != destination_biome;
    if (crossed) {
        ++metrics_.biome_crossings;
        metrics_.cross_biome_hunts += captured;
        if (destination_biome == preferred_biome) {
            if (creature.excursion == Excursion::fed || creature.excursion == Excursion::hunted) {
                ++metrics_.feeding_round_trips;
                metrics_.hunting_round_trips += creature.excursion == Excursion::hunted;
            }
            creature.excursion = Excursion::none;
        } else if (origin_biome == preferred_biome) {
            creature.excursion = Excursion::away;
        }
    }
    if (creature.cell != origin) {
        occupants_[origin] = -1;
        occupants_[creature.cell] = slot;
    }
    return crossed;
}

void World::kill(Creature& creature, StepObserver observer, DeathCause cause) {
    if (observer.record_death) {
        observer.record_death(observer.context, creature, cause);
    }
    if (carcass_decay_) {
        auto& cell = cells_[creature.cell];
        const int carcass = std::min(body_mass, 255 - cell.animal);
        cell.animal += carcass;
        add_detritus(cell, body_mass - carcass + creature.nutrients_carried);
        add_detritus(cell, creature.provisions());
    }
    occupants_[creature.cell] = -1;
    creature = {};
    ++metrics_.deaths;
    --metrics_.population;
}

bool World::guarded(const Creature& young) const {
    if (!guarding_) {
        return false;
    }
    const int nest_slot = &young - creatures_.data();
    for (const int cell : neighbors(young.cell)) {
        if (cell < 0) {
            continue;
        }
        const int slot = occupants_[cell];
        if (slot >= 0 && active_guards_[slot] && dependents_[slot] == nest_slot &&
            young.has_parent(creatures_[slot].id)) {
            return true;
        }
    }
    return false;
}

void World::reproduce(Creature& parent, int destination, StepObserver observer) {
    if constexpr (wide_world) {
        // Leave a buffer around a nursery. Hunting can still bring creatures together.
        if (occupied_neighbors(destination) > 1) {
            return;
        }
    }
    auto& detritus = cells_[parent.cell].detritus;
    const auto available = food_web_ ? parent.nutrients_carried : detritus;
    if (carcass_decay_ && available < body_mass) {
        return;
    }
    auto* slot = birth_slot();
    if (!slot) {
        return;
    }
    if (food_web_) {
        parent.nutrients_carried -= body_mass;
    } else if (carcass_decay_) {
        detritus -= body_mass;
    }
    *slot = {CreatureId(next_id_++), parent.id, parent.founder, parent.generation + 1,
             static_cast<std::uint16_t>(destination), offspring_energy,
             {mutate(parent.genes.digestion), mutate(parent.genes.thrift),
              connected_biomes_ ? mutate(parent.genes.moisture_preference) : std::uint8_t{0}}, 0, 0};
    if (adaptive_hunts_) {
        slot->genes.hunting = mutate(parent.genes.hunting);
        slot->genes.evasion = mutate(parent.genes.evasion);
    }
    if (kin_avoidance_) {
        slot->genes.kin_reluctance = inherit_reluctance(parent, slot->id);
    }
    if (morphology_) {
        inherit_morphology(*slot, parent, nullptr);
    }
    // Young are provisioned in proportion to their body size.
    slot->energy = static_cast<std::uint16_t>(body_share(offspring_energy, *slot));
    parent.energy -= slot->energy + (reproduction_cost - offspring_energy);
    record_birth(*slot, parent, observer);
}

bool World::can_mate(const Creature& creature) const {
    const int receptivity = trait_effects_ ? creature.genes.receptivity : 8;
    return creature.breeding_cooldown == 0 && creature.nutrients_carried >= parental_material &&
        creature.energy >= body_share(124 - receptivity * 4, creature);
}

int World::locate_mate(const Creature& creature) const {
    const int x = creature.cell % width;
    const int y = creature.cell / width;
    int best = -1;
    int distance = mate_range + 1;
    for (int yy = std::max(0, y - mate_range); yy <= std::min(height - 1, y + mate_range); ++yy) {
        for (int xx = std::max(0, x - mate_range); xx <= std::min(width - 1, x + mate_range); ++xx) {
            const int gap = grid_distance(x, y, xx, yy);
            if (gap == 0 || gap >= distance) {
                continue;
            }
            const int cell = yy * width + xx;
            const int slot = occupants_[cell];
            if (slot >= 0 && can_mate(creatures_[slot])) {
                best = cell;
                distance = gap;
            }
        }
    }
    return best;
}

void World::reproduce_sexually(Creature& parent, StepObserver observer) {
    Creature* mate = nullptr;
    int nursery = -1;
    for (const int cell : neighbors(parent.cell)) {
        if (cell < 0) {
            continue;
        }
        const int occupant = occupants_[cell];
        if (occupant >= 0) {
            auto& candidate = creatures_[occupant];
            if (can_mate(candidate) && (!mate || candidate.energy > mate->energy)) {
                mate = &candidate;
            }
        } else if ((!wide_world || occupied_neighbors(cell) <= 1) &&
                   (nursery < 0 || cells_[cell].plants > cells_[nursery].plants)) {
            nursery = cell;
        }
    }
    if (!mate || nursery < 0) {
        return;
    }
    auto* newborn = birth_slot();
    if (!newborn) {
        return;
    }

    const auto inherit = [&](std::uint8_t Genome::*gene) {
        return mutate((random() & 1 ? parent.genes : mate->genes).*gene);
    };
    const Genome genes{inherit(&Genome::digestion), inherit(&Genome::thrift),
                       connected_biomes_ ? inherit(&Genome::moisture_preference) : std::uint8_t{0},
                       inherit(&Genome::mate_drive), inherit(&Genome::receptivity),
                       nests_ ? inherit(&Genome::guard_drive) : std::uint8_t{0},
                       nests_ ? inherit(&Genome::provision_drive) : std::uint8_t{0},
                       adaptive_hunts_ ? inherit(&Genome::hunting) : std::uint8_t{0},
                       adaptive_hunts_ ? inherit(&Genome::evasion) : std::uint8_t{0}};
    *newborn = {CreatureId(next_id_++), parent.id, parent.founder, std::max(parent.generation, mate->generation) + 1,
                static_cast<std::uint16_t>(nursery), offspring_energy, genes, 0, 0,
                0, Excursion::none, Creature::maturity_ticks, mate->id};
    if (kin_avoidance_) {
        newborn->genes.kin_reluctance = inherit_reluctance(parent, newborn->id, mate);
    }
    if (morphology_) {
        inherit_morphology(*newborn, parent, mate);
    }
    // Young are provisioned in proportion to their body size, half by each parent.
    newborn->energy = static_cast<std::uint16_t>(body_share(offspring_energy, *newborn));
    for (auto* contributor : {&parent, mate}) {
        contributor->energy -= body_share(parental_energy, *newborn);
        contributor->nutrients_carried -= parental_material;
        contributor->breeding_cooldown = Creature::recovery_ticks;
    }
    if (nests_) {
        newborn->nest_ticks = Creature::nest_duration;
        const auto track_mate = [&](Creature& adult, std::uint64_t mate_id) {
            if (adult.last_mate) {
                ++metrics_.nests.subsequent_matings;
                const bool repeated = adult.last_mate == mate_id;
                metrics_.nests.repeated_mates += repeated;
                metrics_.nests.previous_mate_alive += repeated ||
                    std::any_of(creatures_.begin(), creatures_.end(), [&](const Creature& candidate) {
                        return candidate.id == adult.last_mate;
                    });
            }
            adult.last_mate = mate_id;
        };
        track_mate(parent, mate->id);
        track_mate(*mate, parent.id);
    }
    record_birth(*newborn, parent, observer, mate);
}

Creature* World::birth_slot() {
    const auto slot = std::find_if(creatures_.begin(), creatures_.end(),
                                  [](const Creature& creature) { return !creature.alive(); });
    if (slot == creatures_.end() || next_id_ > max_creature_id) {
        ++metrics_.capacity_blocks;
        return nullptr;
    }
    return &*slot;
}

void World::record_birth(Creature& newborn, const Creature& parent, StepObserver observer, const Creature* mate) {
    if (faction_markers_) {
        // Keep the marker on its own draw so it does not shift the ecological RNG.
        const auto roll = trait_roll(rng_ ^ std::uint32_t(std::uint64_t(newborn.id) >> 32), std::uint32_t(newborn.id),
                                     inherited_faction_salt);
        const auto& source = mate && (roll & 1) ? *mate : parent;
        auto& faction = newborn.genes.faction;
        faction = source.genes.faction;
        const auto mutation_roll = roll >> 1;
        if (mutation_roll % faction_mutation_denominator == 0) {
            const auto offset = 1 + (mutation_roll / faction_mutation_denominator) % (faction_count - 1);
            faction = 1 + (faction - 1 + offset) % faction_count;
        }
    }
    occupants_[newborn.cell] = &newborn - creatures_.data();
    ++metrics_.births;
    ++metrics_.population;
    if (observer.record_birth) {
        observer.record_birth(observer.context, parent, newborn);
    }
}

std::uint8_t World::mutate(std::uint8_t trait) {
    return mutate_trait(trait, random() % 8);
}

std::uint8_t World::inherit_reluctance(const Creature& parent, std::uint64_t child, const Creature* mate) const {
    // Draw the new locus without advancing the ecological RNG for the matched control.
    const auto roll = trait_roll(rng_ ^ std::uint32_t(child >> 32), std::uint32_t(child), kin_trait_salt);
    const auto& source = mate && (roll & 1) ? *mate : parent;
    return mutate_trait(source.genes.kin_reluctance, (roll >> 1) % 8);
}

int World::bite_limit(const Creature& creature, const Genome& effective) const {
    // Thrift limits the bite. Each two steps of body size from the founders'
    // 8 add or remove one unit.
    const int size = morphology_effects() ? creature.genes.body_size : 8;
    return std::max(1, 9 - effective.thrift / 4 + (size - 8) / 2);
}

int World::max_energy(const Creature& creature) const {
    // A larger body stores more energy for lean times, and a smaller one less.
    return morphology_effects() ? 160 + 6 * (creature.genes.body_size - 8) : 160;
}

int World::body_upkeep(const Creature& creature) const {
    if (!morphology_effects()) {
        return 0;
    }
    // Each step of body size or limb length above 8 costs 1/32 of an energy
    // unit per tick, and each step below saves as much. A shared 32-tick phase
    // spreads the fractions evenly: the mean is exactly steps / 32.
    const int steps = creature.genes.body_size + creature.genes.limbs - 16;
    const int total = steps + static_cast<int>(metrics_.ticks % 32);
    return total >= 0 ? total / 32 : -((31 - total) / 32);
}

int World::limb_stride(const Creature& creature) const {
    if (!morphology_effects()) {
        return 0;
    }
    // Over 16 ticks a creature with limbs L takes L - 8 extra steps when L > 8,
    // or misses 8 - L steps when L < 8. The phase differs between creatures.
    const int phase = static_cast<int>((metrics_.ticks + creature.id) % 16);
    const int limbs = creature.genes.limbs;
    return limbs > 8 && phase < limbs - 8 ? 1 : limbs < 8 && phase < 8 - limbs ? -1 : 0;
}

int World::limb_reach(const Creature& creature) const {
    return morphology_effects() ? (creature.genes.limbs - 8) / 4 : 0;
}

int World::camouflage(const Creature& prey) const {
    if (!morphology_effects()) {
        return 0;
    }
    // Close matches to the ground hide the body; strong mismatches make it stand out.
    const int mismatch = std::abs(int(prey.genes.pigment) - background_tone(cells_[prey.cell].moisture));
    return mismatch <= 4 ? 4 - mismatch : -std::min(2, std::max(0, mismatch - 8));
}

int World::pattern_cover(const Creature& prey) const {
    if (!morphology_effects()) {
        return 0;
    }
    // A bold pattern breaks up the outline in dense vegetation and shows on bare
    // ground. A plain coat does the reverse.
    const int vegetation = std::min<int>(cells_[prey.cell].plants, 16) - 8;
    return (prey.genes.pattern - 8) * vegetation / 4;
}

int World::body_odds(const Creature& hunter, const Creature& prey) const {
    if (!morphology_effects()) {
        return 0;
    }
    // Percentage points of capture chance: larger bodies overpower and longer
    // limbs outrun, while camouflage and pattern hide the prey.
    return 2 * (hunter.genes.body_size - prey.genes.body_size) + 2 * (hunter.genes.limbs - prey.genes.limbs) -
           5 * camouflage(prey) - pattern_cover(prey);
}

void World::inherit_morphology(Creature& child, const Creature& parent, const Creature* mate) const {
    // Separate draws keep the ecological random stream identical to a run without
    // morphology, so the neutral control matches it exactly.
    for (unsigned gene = 0; gene < 4; ++gene) {
        const auto roll = trait_roll(rng_, child.id, morphology_salts[gene]);
        const auto& source = mate && (roll & 1) ? *mate : parent;
        child.genes.*morphology_genes[gene] = mutate_trait(source.genes.*morphology_genes[gene], (roll >> 1) % 8);
    }
}

std::uint64_t World::fingerprint() const {
    std::uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](auto value) {
        for (unsigned byte = 0; byte < sizeof(value); ++byte) {
            hash ^= (std::uint64_t(value) >> (8 * byte)) & 255;
            hash *= 1099511628211ull;
        }
    };
    add(rng_);
    add(std::uint8_t(trait_effects_));
    add(carcass_decay_);
    add(std::uint8_t(predation_));
    add(next_id_);
    add(metrics_.ticks);
    add(metrics_.births);
    add(metrics_.deaths);
    add(metrics_.capacity_blocks);
    add(std::uint32_t(metrics_.population));
    add(metrics_.plants_eaten);
    add(metrics_.animal_eaten);
    add(metrics_.attacks);
    add(metrics_.kills);
    for (const auto& cell : cells_) {
        add(cell.moisture);
        add(cell.nutrients);
        add(cell.plants);
        add(std::uint32_t(cell.detritus));
        add(cell.animal);
    }
    // The hash keeps the 64-bit widths of earlier builds so fingerprints stay comparable.
    for (const auto& creature : creatures_) {
        add(std::uint64_t(creature.id));
        add(std::uint64_t(creature.parent));
        add(std::uint64_t(creature.founder));
        add(creature.generation);
        add(creature.cell);
        add(creature.energy);
        add(creature.genes.digestion);
        add(creature.genes.thrift);
        add(std::uint64_t(creature.plants_eaten));
        add(std::uint64_t(creature.animal_eaten));
    }
    if (food_web_) {
        add(std::uint8_t(food_web_));
        for (const auto& creature : creatures_) {
            add(creature.nutrients_carried);
        }
    }
    if (connected_biomes_) {
        add(std::uint8_t(connected_biomes_));
        if (irregular_terrain_) {
            add(std::uint8_t(irregular_terrain_));
        }
        add(metrics_.biome_crossings);
        add(metrics_.cross_biome_meals);
        add(metrics_.cross_biome_hunts);
        add(metrics_.biome_shifts);
        add(metrics_.feeding_round_trips);
        add(metrics_.hunting_round_trips);
        for (const auto& creature : creatures_) {
            add(creature.genes.moisture_preference);
            add(std::uint8_t(creature.excursion));
        }
    }
    if (sexual_reproduction_) {
        add(std::uint8_t(sexual_reproduction_));
        for (const auto& creature : creatures_) {
            add(std::uint64_t(creature.other_parent));
            add(creature.breeding_cooldown);
            add(creature.genes.mate_drive);
            add(creature.genes.receptivity);
        }
    }
    if (nests_) {
        add(std::uint8_t(nests_));
        add(std::uint8_t(guarding_));
        add(std::uint8_t(provisioning_));
        std::apply([&](const auto&... values) { (add(values), ...); }, NestMetrics::fields(metrics_.nests));
        for (const auto& creature : creatures_) {
            add(std::uint64_t(creature.last_mate));
            add(creature.nest_ticks);
            add(creature.packed_plants);
            add(creature.packed_animal);
            add(creature.genes.guard_drive);
            add(creature.genes.provision_drive);
        }
    }
    if (adaptive_hunts_) {
        add(std::uint8_t(adaptive_hunts_));
        add(std::uint8_t(passive_prey_));
        if (!hunt_tuning_.is_default()) {
            add(hunt_tuning_.threshold);
            add(hunt_tuning_.chase_cost);
        }
        for (const auto& creature : creatures_) {
            add(creature.genes.hunting);
            add(creature.genes.evasion);
            add(std::uint64_t(creature.hunt_target));
            add(std::uint64_t(creature.prey_killed));
        }
    }
    if (kin_avoidance_) {
        add(std::uint8_t(kin_avoidance_));
        if (!kin_tuning_.is_default()) {
            add(kin_tuning_.kin_strength);
            add(kin_tuning_.resemblance_strength);
            add(std::uint8_t(kin_tuning_.protect_similar_prey));
        }
        for (const auto& creature : creatures_) {
            add(creature.genes.kin_reluctance);
        }
    }
    if (faction_markers_) {
        add(std::uint8_t(faction_markers_));
        add(std::uint8_t(cross_faction_hunts_));
        for (const auto& creature : creatures_) {
            add(creature.genes.faction);
        }
    }
    if (staggered_growth_) {
        add(std::uint8_t(staggered_growth_));
        add(std::uint8_t(fruiting_plants_));
    }
    if (fruiting_plants_) {
        add(metrics_.fruit_eaten);
        for (std::size_t index = 0; index < shrubs_.size(); ++index) {
            add(shrubs_[index]);
        }
        for (const auto& creature : creatures_) {
            add(creature.packed_fruit);
        }
    }
    if (dynamic_environment_) {
        add(std::uint8_t(dynamic_environment_));
        add(climate_seed_);
        for (const auto rain : rainfall_) {
            add(rain);
        }
    }
    if (storm_fronts_) {
        add(std::uint8_t(storm_fronts_));
        for (const auto& storm : storms_) {
            add(storm.x), add(storm.y), add(storm.vx), add(storm.vy), add(storm.radius), add(storm.strength);
        }
        for (const auto store : soil_water_) {
            add(store);
        }
    }
    if (brains_) {
        add(std::uint8_t(brains_));
        add(std::uint8_t(neutral_brains_));
        add(std::uint8_t(shuffled_brains_));
        add(brain_hash());
    }
    if (morphology_) {
        add(std::uint8_t(morphology_));
        add(std::uint8_t(neutral_morphology_));
        for (const auto& creature : creatures_) {
            for (const auto gene : morphology_genes) {
                add(creature.genes.*gene);
            }
        }
    }
    return hash;
}

} // namespace petri
