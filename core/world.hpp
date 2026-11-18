#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <tuple>

namespace petri {

#ifdef PETRI_WIDE_WORLD
inline constexpr int width = 128;
inline constexpr int height = 64;
inline constexpr bool wide_world = true;
#else
inline constexpr int width = 64;
inline constexpr int height = 32;
inline constexpr bool wide_world = false;
#endif
inline constexpr int capacity = 320;
// Checkpoint versions 1-16 stored 160 creature slots. Version 16 is reserved for
// storm fronts, which are developed on another branch; this build cannot load it.
inline constexpr int legacy_capacity = 160;
inline constexpr int founder_count = 32;
inline constexpr int faction_count = 4;
inline constexpr int body_mass = 8;
inline constexpr int max_fruit = 4;
inline constexpr int climate_spacing = 16;
inline constexpr int climate_columns = width / climate_spacing + 1;
inline constexpr int climate_rows = height / climate_spacing + 1;
inline constexpr int climate_nodes = climate_columns * climate_rows;
inline constexpr int rainfall_limit = 80;
inline constexpr int storm_slots = 4;

// Header, metadata, cells, creature slots, and a checksum.
using LegacyCheckpoint = std::array<std::uint8_t, 6 + 37 + width * height * 4 + legacy_capacity * 34 + 8>;
using ScavengingCheckpoint = std::array<std::uint8_t, 6 + 54 + width * height * 8 + legacy_capacity * 50 + 8>;
using PredationCheckpoint = std::array<std::uint8_t, 6 + 71 + width * height * 8 + legacy_capacity * 50 + 8>;
using FoodWebCheckpoint = std::array<std::uint8_t, 6 + 72 + width * height * 8 + legacy_capacity * 51 + 8>;
using ConnectedCheckpoint = std::array<std::uint8_t, 6 + 121 + width * height * 8 + legacy_capacity * 53 + 8>;
using IrregularCheckpoint = std::array<std::uint8_t, 6 + 122 + width * height * 8 + legacy_capacity * 53 + 8>;
using SexualCheckpoint = std::array<std::uint8_t, 6 + 123 + width * height * 8 + legacy_capacity * 64 + 8>;
using NestCheckpoint = std::array<std::uint8_t, 6 + 126 + 15 * 8 + width * height * 8 + legacy_capacity * 77 + 8>;
using AdaptiveCheckpoint = std::array<std::uint8_t, 6 + 128 + 15 * 8 + width * height * 8 + legacy_capacity * 95 + 8>;
using HuntTuningCheckpoint = std::array<std::uint8_t, 6 + 130 + 15 * 8 + width * height * 8 + legacy_capacity * 95 + 8>;
using KinAvoidanceCheckpoint = std::array<std::uint8_t, 6 + 131 + 15 * 8 + width * height * 8 + legacy_capacity * 96 + 8>;
using KinTuningCheckpoint = std::array<std::uint8_t, 6 + 134 + 15 * 8 + width * height * 8 + legacy_capacity * 96 + 8>;
using FactionCheckpoint = std::array<std::uint8_t, 6 + 136 + 15 * 8 + width * height * 8 + legacy_capacity * 97 + 8>;
using FloraCheckpoint = std::array<std::uint8_t, 6 + 146 + 15 * 8 + width * height * 9 + legacy_capacity * 98 + 8>;
using ClimateCheckpoint = std::array<std::uint8_t, 6 + 151 + climate_nodes + 15 * 8 + width * height * 9 + legacy_capacity * 98 + 8>;
// Version 16 adds the storm-front flag, the storms, and the soil water at each climate node.
inline constexpr std::size_t storm_bytes = 4 + 4 + 2 + 2 + 1 + 1;
inline constexpr std::size_t storm_state_bytes = 1 + storm_slots * storm_bytes + climate_nodes * 4;
using StormCheckpoint = std::array<std::uint8_t, std::tuple_size<ClimateCheckpoint>::value + storm_state_bytes>;
// Version 17: 320 slots with 32-bit IDs and lifetime counters.
using CapacityCheckpoint = std::array<std::uint8_t, 6 + 151 + climate_nodes + storm_state_bytes + 15 * 8 +
                                                    width * height * 9 + capacity * 59 + 8>;
// Version 18 adds the morphology settings and four morphology genes per slot.
inline constexpr std::size_t checkpoint_creature_bytes = 63;
using Checkpoint = std::array<std::uint8_t, 6 + 153 + climate_nodes + storm_state_bytes + 15 * 8 + width * height * 9 +
                                            capacity * checkpoint_creature_bytes + 8>;
// The SAMD51 keeps checkpoints in 96 KiB QSPI slots whose data starts at byte 256.
static_assert(std::tuple_size<Checkpoint>::value <= 96 * 1024 - 256);

enum class Biome : std::uint8_t { scrub, grassland, wetland };

// A storm front crossing the map. It rains only where the regional climate is
// wet, so wet regions get their water in passing storms with dry spells between.
struct Storm {
    std::int32_t x = 0, y = 0;   // Center, in 1/256 cell.
    std::int16_t vx = 0, vy = 0; // Motion per tick, in 1/256 cell.
    std::uint8_t radius = 0;     // Cells. 0 means the slot is empty.
    std::uint8_t strength = 0;   // Rain gain at the center.

    bool active() const { return radius != 0; }
};

// Storm rain at a cell is its positive bias times cover / storm_unit. The value
// is calibrated so storms deliver the steady model's water on average: across
// eight seeds the mean ratio was 1.01 (0.68 to 1.41).
inline constexpr int storm_unit = 155;
// Summed coverage of a cell: falloff (0-256) times strength, for each storm over it.
int storm_cover_at(const std::array<Storm, storm_slots>& storms, std::size_t cell);

constexpr Biome biome_at(unsigned moisture) {
    return moisture >= 128 ? Biome::wetland : moisture >= 56 ? Biome::grassland : Biome::scrub;
}

// A cell's background on the pigment gene's scale: 0 for the driest ground, 15 for the wettest.
constexpr int background_tone(unsigned moisture) {
    return static_cast<int>(moisture * 15 / 255);
}

struct Cell {
    // Measured peaks stay below 8,200. Deposits past the limit are clipped and counted in Metrics.
    static constexpr std::uint16_t max_detritus = 0xffff;
    std::uint16_t detritus;
    std::uint8_t moisture, nutrients, plants, animal;
};
static_assert(sizeof(Cell) == 6);

// Creature IDs are 32 bits in memory and 64 bits in checkpoints and observations.
// Births stop, counted as capacity blocks, if the ID space is ever exhausted.
using CreatureId = std::uint32_t;
inline constexpr std::uint64_t max_creature_id = 0xffffffffu;

// Nine bits per cell: the occupying creature slot, or -1 when the cell is vacant.
// The low eight bits take a byte per cell and the ninth is packed eight cells
// to a byte, so 320 slots cost 1.125 bytes per cell rather than 2.
class OccupancyGrid {
public:
    static constexpr unsigned vacant = 0x1ff;

    class Ref {
    public:
        operator int() const { return grid_.get(cell_); }
        Ref& operator=(int slot) {
            grid_.set(cell_, slot);
            return *this;
        }
        Ref& operator=(const Ref& other) { return *this = int(other); }

    private:
        friend class OccupancyGrid;
        Ref(OccupancyGrid& grid, std::size_t cell) : grid_(grid), cell_(cell) {}
        OccupancyGrid& grid_;
        std::size_t cell_;
    };

    Ref operator[](std::size_t cell) { return Ref(*this, cell); }
    int operator[](std::size_t cell) const { return get(cell); }
    void fill(int slot) {
        const unsigned value = slot < 0 ? vacant : unsigned(slot);
        low_.fill(static_cast<std::uint8_t>(value));
        high_.fill(value & 0x100 ? 0xff : 0);
    }

private:
    int get(std::size_t cell) const {
        const unsigned value = low_[cell] | ((high_[cell / 8] >> (cell % 8)) & 1u) << 8;
        return value == vacant ? -1 : int(value);
    }
    void set(std::size_t cell, int slot) {
        const unsigned value = slot < 0 ? vacant : unsigned(slot);
        low_[cell] = static_cast<std::uint8_t>(value);
        auto& bits = high_[cell / 8];
        const unsigned bit = 1u << (cell % 8);
        bits = static_cast<std::uint8_t>(value & 0x100 ? bits | bit : bits & ~bit);
    }

    std::array<std::uint8_t, width * height> low_{};
    std::array<std::uint8_t, width * height / 8> high_{};
};
static_assert(capacity < OccupancyGrid::vacant);

// Half a byte per cell: zero for no shrub, otherwise one plus the ripe fruit count.
class ShrubGrid {
public:
    class Ref {
    public:
        operator std::uint8_t() const { return (byte_ >> shift_) & 15; }
        Ref& operator=(unsigned stock) {
            byte_ = static_cast<std::uint8_t>((byte_ & ~(15u << shift_)) | ((stock & 15u) << shift_));
            return *this;
        }
        Ref& operator=(const Ref& other) { return *this = unsigned(std::uint8_t(other)); }
        Ref& operator+=(unsigned amount) { return *this = unsigned(std::uint8_t(*this)) + amount; }
        Ref& operator-=(unsigned amount) { return *this = unsigned(std::uint8_t(*this)) - amount; }
        Ref& operator--() { return *this -= 1; }

    private:
        friend class ShrubGrid;
        Ref(std::uint8_t& byte, unsigned shift) : byte_(byte), shift_(shift) {}
        std::uint8_t& byte_;
        unsigned shift_;
    };

    Ref operator[](std::size_t cell) { return Ref(packed_[cell / 2], cell % 2 * 4); }
    std::uint8_t operator[](std::size_t cell) const { return (packed_[cell / 2] >> (cell % 2 * 4)) & 15; }
    static constexpr std::size_t size() { return width * height; }

private:
    std::array<std::uint8_t, (width * height + 1) / 2> packed_{};
};

struct Genome {
    static constexpr int minimum = 1;
    static constexpr int maximum = 15;
    static constexpr int max_prey_resemblance = 12;
    std::uint8_t digestion, thrift;
    std::uint8_t moisture_preference = 0;
    std::uint8_t mate_drive = 0, receptivity = 0;
    std::uint8_t guard_drive = 0, provision_drive = 0;
    std::uint8_t hunting = 0, evasion = 0;
    std::uint8_t kin_reluctance = 0;
    std::uint8_t faction = 0;
    // Morphology, present with Config::morphology. Founders' standard is 8, except
    // pigment, which matches background_tone().
    std::uint8_t body_size = 0, pigment = 0, pattern = 0, limbs = 0;

    int preferred_moisture() const { return 12 + moisture_preference * 12; }

    int meat_digestion() const { return minimum + maximum - digestion; }
    int prey_resemblance(const Genome& other) const;
};

enum class Excursion : std::uint8_t { none, away, fed, hunted };

struct Creature {
    static constexpr std::uint8_t maturity_ticks = 24, recovery_ticks = 42;
    static constexpr std::uint8_t nest_duration = 12, max_provisions = 8;
    CreatureId id, parent, founder;
    std::uint32_t generation;
    std::uint16_t cell, energy;
    Genome genes;
    // Lifetime intake and kills saturate rather than wrap.
    std::uint32_t plants_eaten, animal_eaten;
    std::uint8_t nutrients_carried = 0;
    Excursion excursion = Excursion::none;
    std::uint8_t breeding_cooldown = 0;
    CreatureId other_parent = 0;
    CreatureId last_mate = 0;
    std::uint8_t nest_ticks = 0, packed_plants = 0, packed_animal = 0;
    std::uint8_t packed_fruit = 0;
    CreatureId hunt_target = 0;
    std::uint32_t prey_killed = 0;

    bool alive() const { return id != 0; }
    int provisions() const { return packed_plants + packed_animal + packed_fruit; }
    bool has_parent(std::uint64_t adult) const { return parent == adult || other_parent == adult; }
    bool close_kin(const Creature& other) const {
        return alive() && other.alive() && id != other.id && (has_parent(other.id) || other.has_parent(id) ||
            (parent && other.has_parent(parent)) || (other_parent && other.has_parent(other_parent)));
    }
};

enum class HuntDecision : std::uint8_t {
    sated, low_energy, no_prey, blocked, forage_or_rest, strike, pursue, flee, cover,
    kin_reluctance, faction_protected, count,
};

struct HuntEnergy {
    std::uint64_t chase = 0, strike = 0, upkeep = 0;
    std::uint64_t prey_reserve = 0, kill_meal = 0, kill_cap_loss = 0;
    std::int64_t other_meal = 0;
};

enum class DeathCause : std::uint8_t { depleted_energy, predation };

struct StepObserver {
    void* context = nullptr;
    void (*record_birth)(void*, const Creature&, const Creature&) = nullptr;
    void (*record_hunt)(void*, std::uint16_t hunter_cell, std::uint16_t prey_cell) = nullptr;
    void (*record_meal)(void*, const Creature&, bool meat) = nullptr;
    void (*record_hunt_decision)(void*, HuntDecision) = nullptr;
    void (*record_hunt_energy)(void*, const HuntEnergy&) = nullptr;
    void (*record_kill)(void*, const Creature& hunter, const Creature& prey) = nullptr;
    void (*record_attack)(void*, const Creature& attacker, const Creature& target, bool landed) = nullptr;
    void (*record_death)(void*, const Creature&, DeathCause) = nullptr;
};

struct NestMetrics {
    std::uint64_t fledged = 0, starved = 0, killed = 0;
    std::uint64_t attacks = 0, guarded_attacks = 0, guard_ticks = 0;
    std::uint64_t deliveries = 0, food_delivered = 0;
    std::uint64_t both_near = 0, both_alive = 0;
    std::uint64_t after_nest_near = 0, after_nest_alive = 0;
    std::uint64_t repeated_mates = 0, previous_mate_alive = 0, subsequent_matings = 0;

    template<class Self>
    static auto fields(Self& self) {
        return std::tie(self.fledged, self.starved, self.killed, self.attacks, self.guarded_attacks,
            self.guard_ticks, self.deliveries, self.food_delivered, self.both_near, self.both_alive,
            self.after_nest_near, self.after_nest_alive,
            self.repeated_mates, self.previous_mate_alive, self.subsequent_matings);
    }
};

struct Metrics {
    std::uint64_t ticks = 0, births = 0, deaths = 0, capacity_blocks = 0;
    unsigned population = 0;
    std::uint64_t plants_eaten = 0, animal_eaten = 0;
    std::uint64_t attacks = 0, kills = 0;
    std::uint64_t biome_crossings = 0, cross_biome_meals = 0, cross_biome_hunts = 0;
    std::uint64_t biome_shifts = 0;
    std::uint64_t feeding_round_trips = 0, hunting_round_trips = 0;
    NestMetrics nests;
    std::uint64_t fruit_eaten = 0;
    // Detritus that could not be stored in a full cell. Not saved in checkpoints.
    std::uint64_t detritus_clipped = 0;
};

struct HuntTuning {
    static constexpr std::uint8_t default_threshold = 42, default_chase_cost = 2;
    static constexpr std::uint8_t max_threshold = 160, max_chase_cost = 160;

    std::uint8_t threshold = default_threshold;
    std::uint8_t chase_cost = default_chase_cost;

    bool is_default() const { return threshold == default_threshold && chase_cost == default_chase_cost; }
};

struct KinTuning {
    static constexpr std::uint8_t default_strength = 7, max_strength = 20;

    std::uint8_t kin_strength = default_strength;
    std::uint8_t resemblance_strength = default_strength;
    bool protect_similar_prey = false;

    bool is_default() const {
        return kin_strength == default_strength && resemblance_strength == default_strength && !protect_similar_prey;
    }
};

struct Config {
    std::uint8_t initial_nutrients = 48;
    bool trait_effects = true;
    std::uint8_t carcass_decay = 0;
    std::uint8_t initial_carrion_percent = 0;
    bool predation = false;
    bool food_web = false;
    bool connected_biomes = false;
    bool irregular_terrain = false;
    bool sexual_reproduction = false;
    bool nests = false, guarding = false, provisioning = false;
    bool adaptive_hunts = false;
    bool passive_prey = false;
    HuntTuning hunt_tuning;
    bool kin_avoidance = false;
    KinTuning kin_tuning;
    bool faction_markers = false;
    bool cross_faction_hunts = false;
    bool staggered_growth = false;
    bool fruiting_plants = false;
    bool dynamic_environment = false;
    // Requires dynamic_environment. Positive regional rainfall arrives in storms.
    bool storm_fronts = false;
    // Heritable body size, pigment, pattern, and limbs. Requires the food web.
    bool morphology = false;
    // The control: morphology genes are inherited and mutate but have no effect.
    bool neutral_morphology = false;
};

class World {
public:
    explicit World(std::uint32_t seed, Config config = {});
    void step(StepObserver observer = {});

    const auto& cells() const { return cells_; }
    const auto& creatures() const { return creatures_; }
    const Metrics& metrics() const { return metrics_; }
    std::uint8_t carcass_decay() const { return carcass_decay_; }
    bool predation() const { return predation_; }
    bool food_web() const { return food_web_; }
    bool connected_biomes() const { return connected_biomes_; }
    bool irregular_terrain() const { return irregular_terrain_; }
    bool sexual_reproduction() const { return sexual_reproduction_; }
    bool nests() const { return nests_; }
    bool guarding() const { return guarding_; }
    bool provisioning() const { return provisioning_; }
    bool adaptive_hunts() const { return adaptive_hunts_; }
    bool passive_prey() const { return passive_prey_; }
    bool kin_avoidance() const { return kin_avoidance_; }
    HuntTuning hunt_tuning() const { return hunt_tuning_; }
    KinTuning kin_tuning() const { return kin_tuning_; }
    bool faction_markers() const { return faction_markers_; }
    bool cross_faction_hunts() const { return cross_faction_hunts_; }
    bool staggered_growth() const { return staggered_growth_; }
    bool fruiting_plants() const { return fruiting_plants_; }
    bool dynamic_environment() const { return dynamic_environment_; }
    bool morphology() const { return morphology_; }
    bool neutral_morphology() const { return neutral_morphology_; }
    bool fruiting_shrub(std::size_t cell) const { return shrubs_[cell] != 0; }
    int fruit_at(std::size_t cell) const { return fruiting_shrub(cell) ? shrubs_[cell] - 1 : 0; }
    // Climate, read-only for observers such as the renderer.
    const std::array<std::int8_t, climate_nodes>& rainfall() const { return rainfall_; }
    bool weather_active() const;
    // The seasonal part of the weather bias.
    int seasonal_bias() const;
    // Regional rainfall plus season at a cell; 0 while the weather is inactive.
    int weather_at(std::size_t cell) const { return weather_active() ? weather_bias(static_cast<int>(cell)) : 0; }
    bool storm_fronts() const { return storm_fronts_; }
    const std::array<Storm, storm_slots>& storms() const { return storms_; }
    // Summed storm coverage at a cell: falloff (0-256) times strength.
    int storm_cover(std::size_t cell) const;
    // The water the weather adds to a cell's moisture target. Drought (a negative
    // bias) dries steadily. With storm fronts, wet ground reads the soil water that
    // passing storms recharge, instead of the steady bias.
    int rainfall_at(std::size_t cell) const;
    // Rain falling on a cell right now; what the renderer draws.
    int storm_rain_at(std::size_t cell) const;
    const std::array<std::int32_t, climate_nodes>& soil_water() const { return soil_water_; }
    std::uint64_t fingerprint() const;
    void save_checkpoint(Checkpoint& checkpoint) const;
    // Streams the same bytes in chunks of up to 256 bytes. Returns the total size.
    using ChunkSink = void (*)(void* context, const std::uint8_t* data, std::size_t size);
    std::size_t save_checkpoint(ChunkSink sink, void* context) const;
    // Decodes a current-version checkpoint directly into this world, without a
    // staging copy. On failure the world is left in an unspecified state and must
    // be replaced. Use it only when no running world needs protecting, such as at boot.
    bool load_checkpoint_in_place(const std::uint8_t* data, std::size_t size);
    bool load_checkpoint(const Checkpoint& checkpoint);
    bool load_checkpoint(const LegacyCheckpoint& checkpoint);
    bool load_checkpoint(const ScavengingCheckpoint& checkpoint);
    bool load_checkpoint(const PredationCheckpoint& checkpoint);
    bool load_checkpoint(const FoodWebCheckpoint& checkpoint);
    bool load_checkpoint(const ConnectedCheckpoint& checkpoint);
    bool load_checkpoint(const IrregularCheckpoint& checkpoint);
    bool load_checkpoint(const SexualCheckpoint& checkpoint);
    bool load_checkpoint(const NestCheckpoint& checkpoint);
    bool load_checkpoint(const AdaptiveCheckpoint& checkpoint);
    bool load_checkpoint(const HuntTuningCheckpoint& checkpoint);
    bool load_checkpoint(const KinAvoidanceCheckpoint& checkpoint);
    bool load_checkpoint(const KinTuningCheckpoint& checkpoint);
    bool load_checkpoint(const FactionCheckpoint& checkpoint);
    bool load_checkpoint(const FloraCheckpoint& checkpoint);
    bool load_checkpoint(const ClimateCheckpoint& checkpoint);
    bool load_checkpoint(const StormCheckpoint& checkpoint);
    bool load_checkpoint(const CapacityCheckpoint& checkpoint);

private:
    World() = default;
    bool decode_checkpoint(const std::uint8_t* data, std::size_t size, std::uint8_t version);
    static bool decode_into(World& decoded, const std::uint8_t* data, std::size_t size, std::uint8_t version);
    std::uint32_t random();
    void update_plants();
    void update_staggered_environment();
    void recycle(Cell& cell);
    void add_detritus(Cell& cell, unsigned amount);
    void grow_plants(Cell& cell, unsigned tick);
    void update_fruit(int index, unsigned tick);
    void transport_nutrients();
    void update_moisture(int index);
    void update_climate();
    void update_storms();
    void update_soil_water();
    int weather_bias(int index) const;
    int occupied_neighbors(int cell) const;
    void update_creature(int slot, StepObserver observer);
    struct HuntPlan {
        HuntDecision decision;
        int prey = -1, step = -1, chance = 0, chase_cost = 0, strike_cost = 0;
    };
    HuntPlan plan_hunt(const Creature& hunter, const Genome& genes, int offset, int forage, int upkeep) const;
    HuntDecision skipped_hunt(const Creature& hunter, int range, int offset) const;
    static int hunt_energy(int prey_energy, int digestion, bool adaptive);
    void prepare_evasion(StepObserver observer);
    int step_toward(int from, int target, unsigned offset) const;
    bool move_creature(int slot, int origin, bool captured);
    static std::array<int, 4> neighbors(int cell);
    static int cell_distance(int from, int to);
    void prepare_nests();
    Creature* dependent(int adult_slot);
    bool guarded(const Creature& young) const;
    int care_pull(const Creature& adult, const Creature& young) const;
    enum class Food { foliage, fruit, meat };
    int gather_food(int index, Food food, int bite_limit);
    void record_food(Creature& creature, Food food, int amount, StepObserver observer);
    int pack_food(Creature& adult, Food food, int bite);
    void deliver_food(Creature& adult, Creature& young, StepObserver observer);
    void feed_young(Creature& young, Food food, int amount, StepObserver observer);
    void update_young(Creature& young, StepObserver observer);
    void reproduce(Creature& parent, int destination, StepObserver observer);
    bool can_mate(const Creature& creature) const;
    int locate_mate(const Creature& creature) const;
    void reproduce_sexually(Creature& parent, StepObserver observer);
    Creature* birth_slot();
    void record_birth(Creature& newborn, const Creature& parent, StepObserver observer, const Creature* mate = nullptr);
    void kill(Creature& creature, StepObserver observer, DeathCause cause);
    std::uint8_t mutate(std::uint8_t trait);
    // Morphology effects. Each is zero, or the founders' standard, unless the
    // genes are expressed (see morphology_effects()).
    bool morphology_effects() const { return morphology_ && trait_effects_ && !neutral_morphology_; }
    int bite_limit(const Creature& creature, const Genome& effective) const;
    int max_energy(const Creature& creature) const;
    // An energy amount scaled by the creature's store: max_energy / 160 times `amount`.
    int body_share(int amount, const Creature& creature) const { return amount * max_energy(creature) / 160; }
    int body_upkeep(const Creature& creature) const;
    int limb_reach(const Creature& creature) const;
    int limb_stride(const Creature& creature) const;
    int camouflage(const Creature& prey) const;
    int concealment(const Creature& prey) const { return std::max(0, camouflage(prey)); }
    int pattern_cover(const Creature& prey) const;
    int body_odds(const Creature& hunter, const Creature& prey) const;
    void inherit_morphology(Creature& child, const Creature& parent, const Creature* mate) const;
    std::uint8_t inherit_reluctance(const Creature& parent, std::uint64_t child,
                                    const Creature* mate = nullptr) const;

    std::array<Cell, width * height> cells_{};
    // Zero is bare of shrubs. Other values store one plus the ripe fruit count.
    ShrubGrid shrubs_{};
    std::array<std::int8_t, climate_nodes> rainfall_{};
    std::array<Storm, storm_slots> storms_{};
    std::array<std::int32_t, climate_nodes> soil_water_{};
    std::array<Creature, capacity> creatures_{};
    OccupancyGrid occupants_{};
    std::array<std::int16_t, capacity> dependents_{};
    std::array<bool, capacity> active_guards_{};
    std::array<bool, capacity> evaded_{};
    Metrics metrics_;
    std::uint64_t next_id_ = 1;
    std::uint32_t rng_ = 0;
    bool trait_effects_ = true;
    std::uint8_t carcass_decay_ = 0;
    bool predation_ = false;
    bool food_web_ = false;
    bool connected_biomes_ = false;
    bool irregular_terrain_ = false;
    bool sexual_reproduction_ = false;
    bool nests_ = false, guarding_ = false, provisioning_ = false;
    bool adaptive_hunts_ = false, passive_prey_ = false;
    HuntTuning hunt_tuning_;
    bool kin_avoidance_ = false;
    KinTuning kin_tuning_;
    bool faction_markers_ = false;
    bool cross_faction_hunts_ = false;
    bool staggered_growth_ = false;
    bool fruiting_plants_ = false;
    bool dynamic_environment_ = false;
    bool storm_fronts_ = false;
    bool morphology_ = false, neutral_morphology_ = false;
    std::uint32_t climate_seed_ = 0;
};

} // namespace petri
