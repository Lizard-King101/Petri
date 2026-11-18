#pragma once

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
inline constexpr int capacity = 160;
inline constexpr int founder_count = 32;
inline constexpr int faction_count = 4;
inline constexpr int body_mass = 8;
inline constexpr int max_fruit = 4;
inline constexpr int climate_spacing = 16;
inline constexpr int climate_columns = width / climate_spacing + 1;
inline constexpr int climate_rows = height / climate_spacing + 1;
inline constexpr int climate_nodes = climate_columns * climate_rows;
inline constexpr int rainfall_limit = 80;

// Header, metadata, cells, creature slots, and a checksum.
using LegacyCheckpoint = std::array<std::uint8_t, 6 + 37 + width * height * 4 + capacity * 34 + 8>;
using ScavengingCheckpoint = std::array<std::uint8_t, 6 + 54 + width * height * 8 + capacity * 50 + 8>;
using PredationCheckpoint = std::array<std::uint8_t, 6 + 71 + width * height * 8 + capacity * 50 + 8>;
using FoodWebCheckpoint = std::array<std::uint8_t, 6 + 72 + width * height * 8 + capacity * 51 + 8>;
using ConnectedCheckpoint = std::array<std::uint8_t, 6 + 121 + width * height * 8 + capacity * 53 + 8>;
using IrregularCheckpoint = std::array<std::uint8_t, 6 + 122 + width * height * 8 + capacity * 53 + 8>;
using SexualCheckpoint = std::array<std::uint8_t, 6 + 123 + width * height * 8 + capacity * 64 + 8>;
using NestCheckpoint = std::array<std::uint8_t, 6 + 126 + 15 * 8 + width * height * 8 + capacity * 77 + 8>;
using AdaptiveCheckpoint = std::array<std::uint8_t, 6 + 128 + 15 * 8 + width * height * 8 + capacity * 95 + 8>;
using HuntTuningCheckpoint = std::array<std::uint8_t, 6 + 130 + 15 * 8 + width * height * 8 + capacity * 95 + 8>;
using KinAvoidanceCheckpoint = std::array<std::uint8_t, 6 + 131 + 15 * 8 + width * height * 8 + capacity * 96 + 8>;
using KinTuningCheckpoint = std::array<std::uint8_t, 6 + 134 + 15 * 8 + width * height * 8 + capacity * 96 + 8>;
using FactionCheckpoint = std::array<std::uint8_t, 6 + 136 + 15 * 8 + width * height * 8 + capacity * 97 + 8>;
using FloraCheckpoint = std::array<std::uint8_t, 6 + 146 + 15 * 8 + width * height * 9 + capacity * 98 + 8>;
using Checkpoint = std::array<std::uint8_t, 6 + 151 + climate_nodes + 15 * 8 + width * height * 9 + capacity * 98 + 8>;

enum class Biome : std::uint8_t { scrub, grassland, wetland };

constexpr Biome biome_at(unsigned moisture) {
    return moisture >= 128 ? Biome::wetland : moisture >= 56 ? Biome::grassland : Biome::scrub;
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

// One byte per cell: the occupying creature slot, or -1 when the cell is vacant.
class OccupancyGrid {
public:
    static constexpr std::uint8_t vacant = 0xff;

    class Ref {
    public:
        operator int() const { return byte_ == vacant ? -1 : byte_; }
        Ref& operator=(int slot) {
            byte_ = slot < 0 ? vacant : static_cast<std::uint8_t>(slot);
            return *this;
        }
        Ref& operator=(const Ref& other) { return *this = int(other); }

    private:
        friend class OccupancyGrid;
        explicit Ref(std::uint8_t& byte) : byte_(byte) {}
        std::uint8_t& byte_;
    };

    Ref operator[](std::size_t cell) { return Ref(slots_[cell]); }
    int operator[](std::size_t cell) const { return slots_[cell] == vacant ? -1 : slots_[cell]; }
    void fill(int slot) { slots_.fill(slot < 0 ? vacant : static_cast<std::uint8_t>(slot)); }

private:
    std::array<std::uint8_t, width * height> slots_{};
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
    bool fruiting_shrub(std::size_t cell) const { return shrubs_[cell] != 0; }
    int fruit_at(std::size_t cell) const { return fruiting_shrub(cell) ? shrubs_[cell] - 1 : 0; }
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
    std::uint8_t inherit_reluctance(const Creature& parent, std::uint64_t child,
                                    const Creature* mate = nullptr) const;

    std::array<Cell, width * height> cells_{};
    // Zero is bare of shrubs. Other values store one plus the ripe fruit count.
    ShrubGrid shrubs_{};
    std::array<std::int8_t, climate_nodes> rainfall_{};
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
    std::uint32_t climate_seed_ = 0;
};

} // namespace petri
