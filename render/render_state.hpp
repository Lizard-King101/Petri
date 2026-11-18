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

#include <array>
#include <cstddef>
#include <cstdint>

namespace petri {

enum class DietShape : std::uint8_t { meat, mixed, plant };

// How a creature looks, derived from its genome. Bit layout (32 bits):
//   0-1   diet shape (DietShape)
//   2-3   body color: the biome of the preferred moisture (Biome)
//   4-5   back height in the 16x16 detail sprite, minus 4
//   6-8   faction marker, 0 for none
//   9-31  zero; reserved for morphology
// A renderer must draw every look, including bits it does not yet use.
struct Look {
    std::uint32_t bits = 0;

    DietShape shape() const { return static_cast<DietShape>(bits & 3); }
    // Biome of the preferred moisture, used for body color in connected worlds.
    Biome body() const { return static_cast<Biome>((bits >> 2) & 3); }
    // Height of the back line in the 16x16 detail sprite, 4 to 7.
    int back() const { return 4 + ((bits >> 4) & 3); }
    std::uint8_t faction() const { return static_cast<std::uint8_t>((bits >> 6) & 7); }

    friend bool operator==(Look a, Look b) { return a.bits == b.bits; }
};

Look look_of(const Genome& genes);

// Three bytes per cell. The biome is exact; moisture is kept in 16 levels,
// which removes most per-tick churn from the stream.
struct RenderCell {
    // Bits 0-1: biome. Bits 2-5: moisture / 16. Bit 6: carcass present.
    std::uint8_t terrain = 0;
    std::uint8_t plants = 0;
    // Bits 0-4: detritus capped at 16, as every drawing use caps it.
    // Bits 5-7: 0 without a shrub, else 1 plus the ripe fruit count.
    std::uint8_t detail = 0;

    Biome biome() const { return static_cast<Biome>(terrain & 3); }
    // The middle of the stored moisture level.
    unsigned moisture() const { return ((terrain >> 2) & 15) * 16u + 8u; }
    bool carcass() const { return (terrain >> 6) & 1; }
    unsigned detritus() const { return detail & 31; }
    bool shrub() const { return (detail >> 5) != 0; }
    int fruit() const { return shrub() ? (detail >> 5) - 1 : 0; }

    friend bool operator==(const RenderCell& a, const RenderCell& b) {
        return a.terrain == b.terrain && a.plants == b.plants && a.detail == b.detail;
    }
    friend bool operator!=(const RenderCell& a, const RenderCell& b) { return !(a == b); }
};
static_assert(sizeof(RenderCell) == 3);

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

struct RenderState {
    std::uint64_t tick = 0;
    bool connected_biomes = false;
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
