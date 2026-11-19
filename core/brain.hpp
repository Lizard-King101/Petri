#pragma once

// The Phase 1 creature brain (docs/EMERGENT_LIFE.md): what a creature senses,
// and an integer network that turns the senses into an action. Nothing here
// changes a World. Results are bit-identical on every target.

#include "world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace petri::brain {

inline constexpr int radius = 6, directions = 4;
// Directions run clockwise: 0 north (up, y falling), 1 east, 2 south, 3 west.
inline constexpr std::array<std::int8_t, directions> step_x{0, 1, 0, -1}, step_y{-1, 0, 1, 0};

// What a creature senses toward one direction. Each value is scaled to an int8.
enum DirectionInput : std::uint8_t {
    in_plants,      // foliage and fruit, valued by the creature's plant digestion
    in_meat,        // carcass units
    in_near,        // closeness of the nearest creature, 0 if none
    in_size,        // that creature's body size minus mine
    in_kin,         // how that creature is related to me: see kinship()
    in_crowd,       // creatures, closeness-weighted
    in_signal,      // signal flashes, closeness-weighted
    in_habitat,     // how much better the ground there suits my moisture preference
    in_wall,        // closeness of the world's edge
    in_step,        // plant food on the next cell that way, as it is sensed underfoot
};
inline constexpr int direction_inputs = in_step + 1;

enum SelfInput : std::uint8_t {
    self_energy,
    self_nutrients,
    self_youth,   // ticks until maturity
    self_food,    // plant food underfoot
    self_carcass, // carcass underfoot
    self_mismatch,
    self_neighbors,
    self_damage, // taken last tick
    self_memory0,
    self_memory1,
};
inline constexpr int self_inputs = self_memory1 + 1;

inline constexpr int scorer_hidden = 6, head_hidden = 4;
enum HeadOutput : std::uint8_t { out_stay, out_sprint, out_breed, out_signal, out_memory0, out_memory1 };
inline constexpr int head_outputs = out_memory1 + 1;

// Weight layout, one int8 each. A weight of 64 is unity.
//   Direction scorer, shared by the four directions:
//     per hidden unit: direction weights, self weights, bias
//     then the move score: one weight per hidden unit, and a bias
//   Global head:
//     per hidden unit: self weights, weights for the direction inputs averaged
//     over the four directions, bias
//     per output: one weight per hidden unit, and a bias
inline constexpr int scorer_unit_bytes = direction_inputs + self_inputs + 1;
inline constexpr int scorer_bytes = scorer_hidden * scorer_unit_bytes + scorer_hidden + 1;
inline constexpr int head_unit_bytes = self_inputs + direction_inputs + 1;
inline constexpr int head_bytes = head_hidden * head_unit_bytes + head_outputs * (head_hidden + 1);
inline constexpr int brain_bytes = scorer_bytes + head_bytes;
// Brains are stored four weights to a 32-bit word (ESP32 instruction RAM allows
// only 32-bit access) and copied out before use.
inline constexpr int brain_words = (brain_bytes + 3) / 4;

// The quadrant a relative cell belongs to. Each quadrant owns one of its two
// bounding diagonals, so a quarter turn maps the quadrants onto each other and
// each holds the same number of cells. The origin belongs to none.
constexpr int quadrant(int dx, int dy) {
    return dy < 0 && dy < dx && dx <= -dy ? 0 : dx > 0 && -dx < dy && dy <= dx ? 1 : dy > 0 && -dy <= dx && dx < dy ? 2 : 3;
}

// How one creature perceives its relation to another, as a sense value. A
// parent or child reads strongest, then a sibling or the last mate, then a
// stranger wearing the same heritable marker. This is perception only: what a
// creature does about its kin is left to its brain.
inline constexpr int kin_parent_or_child = 127, kin_sibling_or_mate = 96, kin_same_marker = 48;
int kinship(const Creature& me, const Creature& them);

// One bit per cell for the scan: is a creature there, and is it flashing a
// signal. Built once per tick, before any creature decides.
struct Sight {
    std::array<std::uint32_t, width * height / 32> occupied{}, flashing{};

    Sight() = default;
    explicit Sight(const World& world);
    static bool test(const std::array<std::uint32_t, width * height / 32>& bits, int cell) {
        return bits[cell >> 5] >> (cell & 31) & 1u;
    }
    void flash(int cell) { flashing[cell >> 5] |= 1u << (cell & 31); }
};

// Per-creature state the world does not hold yet.
struct Inner {
    std::int8_t youth = 0, damage = 0;
    std::array<std::int8_t, 2> memory{};
};

struct Senses {
    std::int8_t direction[directions][direction_inputs];
    std::int8_t self[self_inputs];
};

struct Decision {
    std::uint8_t action = 0; // 0 stay, otherwise 1 + direction
    bool sprint = false, breed = false, signal = false;
    std::array<std::int8_t, 2> memory{};
};

Senses sense(const World& world, const Sight& sight, int slot, const Inner& inner = {});

// `turn` (0-3) is the direction examined first. Equal move scores keep the
// earlier direction, so the caller varies `turn` to avoid a built-in heading.
Decision think(const Senses& senses, const std::int8_t* weights, unsigned turn);

// A per-creature, per-tick value for `turn` that leaves the world's random stream alone.
unsigned turn_for(CreatureId id, std::uint64_t tick);

// Copies a parent's weights to a child with mutation: each weight has one
// chance in 128 of a step of 1 to 4 either way, about two changes per brain.
// `state` is the caller's random state and is advanced. Returns the number of changes.
int inherit(std::int8_t* child, const std::int8_t* parent, std::uint32_t& state);

// Builds a child's brain from two parents, before mutation. Each hidden unit
// comes whole from one parent, with its incoming weights, its bias, and its
// outgoing weights, so a unit stays a working part. Each output bias comes from
// either parent.
void combine(std::int8_t* child, const std::int8_t* first, const std::int8_t* second, std::uint32_t& state);

} // namespace petri::brain
