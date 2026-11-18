#pragma once

// What the renderer knows about the world.
//
// The renderer never reads World directly. It draws a RenderState: one
// RenderCell per cell, one RenderCreature per slot, and the RenderEvents
// reported since the last update. On a single chip the state is captured from
// a World each step (see LocalPanel). In the dual-processor firmware the ESP32
// captures it and streams it to the SAMD51, which keeps a mirror.
//
// Creatures carry phenotype (Look), not genotype, so biology can change
// without changing the renderer.

#include "world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace petri {

enum class DietShape : std::uint8_t { meat, mixed, plant };

// How a creature looks, derived from its genome. Bit layout (32 bits):
//   0-1    diet shape (DietShape)
//   2-3    body color: the biome of the preferred moisture (Biome)
//   4-5    back height in the 16x16 detail sprite, minus 4
//   6-8    faction marker, 0 for none
//   9      1 when the genome carries morphology genes; bits 10-20 are zero otherwise
//   10-12  body size class 0-7: (body_size - 1) / 2, so 8 (the founders' standard) is class 3
//   13-16  pigment 1-15, on the background_tone() scale: 1 matches the driest ground,
//          15 the wettest. Biome edges fall at tone 3 (scrub to grassland) and 7
//          (grassland to wetland)
//   17-18  pattern class 0-3: (pattern - 1) / 4. 0 is a plain coat, 3 a bold
//          disruptive pattern
//   19-20  limb class 0-3: (limbs - 1) / 4. 0 is stubby, 3 long
//   21-31  zero
// Each morphology trait has an ecological effect (see Genome in world.hpp). A
// renderer must draw every look, including bits it does not yet use.
struct Look {
    std::uint32_t bits = 0;

    DietShape shape() const { return static_cast<DietShape>(bits & 3); }
    // Biome of the preferred moisture, used for body color in connected worlds.
    Biome body() const { return static_cast<Biome>((bits >> 2) & 3); }
    // Height of the back line in the 16x16 detail sprite, 4 to 7.
    int back() const { return 4 + ((bits >> 4) & 3); }
    std::uint8_t faction() const { return static_cast<std::uint8_t>((bits >> 6) & 7); }
    bool morphology() const { return (bits >> 9) & 1; }
    int size_class() const { return (bits >> 10) & 7; }
    int pigment() const { return (bits >> 13) & 15; }
    int pattern() const { return (bits >> 17) & 3; }
    int limbs() const { return (bits >> 19) & 3; }

    friend bool operator==(Look a, Look b) { return a.bits == b.bits; }
};

Look look_of(const Genome& genes);

// Three bytes per cell. The biome is exact; moisture is kept in 16 levels,
// which removes most per-tick churn from the stream.
struct RenderCell {
    // Bits 0-1: biome. Bits 2-5: moisture / 16. Bit 6: carcass present.
    // Bit 7: the carcass is fresh, at least half a body (fresh_carcass_units).
    std::uint8_t terrain = 0;
    std::uint8_t plants = 0;
    // Bits 0-4: detritus capped at 16, as every drawing use caps it.
    // Bits 5-7: 0 without a shrub, else 1 plus the ripe fruit count.
    std::uint8_t detail = 0;

    Biome biome() const { return static_cast<Biome>(terrain & 3); }
    // The middle of the stored moisture level.
    unsigned moisture() const { return ((terrain >> 2) & 15) * 16u + 8u; }
    bool carcass() const { return (terrain >> 6) & 1; }
    bool fresh_carcass() const { return (terrain >> 7) & 1; }
    unsigned detritus() const { return detail & 31; }
    bool shrub() const { return (detail >> 5) != 0; }
    int fruit() const { return shrub() ? (detail >> 5) - 1 : 0; }

    friend bool operator==(const RenderCell& a, const RenderCell& b) {
        return a.terrain == b.terrain && a.plants == b.plants && a.detail == b.detail;
    }
    friend bool operator!=(const RenderCell& a, const RenderCell& b) { return !(a == b); }
};
static_assert(sizeof(RenderCell) == 3);

// Carcass units at which remains still read as a fresh body; a death leaves body_mass.
inline constexpr unsigned fresh_carcass_units = body_mass / 2 + 1;

RenderCell render_cell(const World& world, std::size_t index);

struct RenderCreature {
    CreatureId id = 0;
    std::uint16_t cell = 0;
    Look look{};

    bool alive() const { return id != 0; }
    friend bool operator==(const RenderCreature& a, const RenderCreature& b) {
        return a.id == b.id && a.cell == b.cell && a.look == b.look;
    }
};

RenderCreature render_creature(const Creature& creature);

// The world's climate: coarse rainfall nodes and the season. The renderer draws
// rain and season from it, so what the panel shows is the weather the biology feels.
struct RenderClimate {
    bool active = false, storm_fronts = false;
    std::int8_t seasonal = 0;
    std::array<std::int8_t, climate_nodes> rainfall{};
    std::array<Storm, storm_slots> storms{};

    friend bool operator==(const RenderClimate& a, const RenderClimate& b) {
        const auto same_storm = [](const Storm& x, const Storm& y) {
            return x.x == y.x && x.y == y.y && x.vx == y.vx && x.vy == y.vy && x.radius == y.radius &&
                   x.strength == y.strength;
        };
        return a.active == b.active && a.storm_fronts == b.storm_fronts && a.seasonal == b.seasonal &&
               a.rainfall == b.rainfall && std::equal(a.storms.begin(), a.storms.end(), b.storms.begin(), same_storm);
    }
};

RenderClimate render_climate(const World& world);

// Regional rainfall plus season at a cell, as World::weather_at computes it.
int climate_bias(const RenderClimate& climate, std::size_t cell);
// Rain falling on a cell now: storm rain with storm fronts (World::storm_rain_at),
// otherwise the positive climate bias.
int rain_at(const RenderClimate& climate, std::size_t cell);

struct RenderState {
    std::uint64_t tick = 0;
    bool connected_biomes = false;
    RenderClimate climate{};
    std::array<RenderCell, width * height> cells{};
    std::array<RenderCreature, capacity> creatures{};
};

// Copies everything the renderer needs from the world.
void capture(const World& world, RenderState& state);

struct RenderActor {
    CreatureId id = 0;
    std::uint16_t cell = 0;
    Look look{};

    friend bool operator==(const RenderActor& a, const RenderActor& b) {
        return a.id == b.id && a.cell == b.cell && a.look == b.look;
    }
};

struct RenderEvent {
    enum class Kind : std::uint8_t { hunt, meal, attack, death };
    Kind kind = Kind::hunt;
    // hunt: prey cell was wet; meal: meat; attack: landed; death: predation.
    bool flag = false;
    // hunt: the prey cell is first.cell.
    RenderActor first{}, second{};

    friend bool operator==(const RenderEvent& a, const RenderEvent& b) {
        return a.kind == b.kind && a.flag == b.flag && a.first == b.first && a.second == b.second;
    }
};

// Events reported during one or more steps, in the order they happened.
class EventLog {
public:
    static constexpr std::size_t limit = 128;

    // Records the next World::step into this log. The world is read for event details.
    StepObserver observer(const World& world);
    void add(const RenderEvent& event);
    void clear() { size_ = 0; }

    const RenderEvent* begin() const { return events_.data(); }
    const RenderEvent* end() const { return events_.data() + size_; }
    std::size_t size() const { return size_; }
    // Events lost because the log was full. Drawing loses detail, not correctness.
    std::uint32_t dropped() const { return dropped_; }

private:
    std::array<RenderEvent, limit> events_{};
    std::size_t size_ = 0;
    std::uint32_t dropped_ = 0;
    const World* world_ = nullptr;
};

} // namespace petri
