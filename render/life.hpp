#pragma once

#include "world.hpp"

namespace petri {

inline constexpr int panel_width = 64;
inline constexpr int panel_height = 32;

// Row-major RGB565. Board drivers own orientation, brightness, and frame timing.
using Frame = std::array<std::uint16_t, panel_width * panel_height>;

// Actors use the world grid. Only the terrain drawing tiles are offset.
constexpr std::array<int, 2> cell_origin(int cell, int tile) {
    return {cell % width * tile, cell / width * tile};
}

constexpr int scene_width(int tile) {
    return width * tile;
}

constexpr std::uint16_t rgb565(unsigned red, unsigned green, unsigned blue) {
    return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
}

int interpolate_axis(int from, int to, std::uint32_t elapsed, std::uint32_t duration);
std::uint16_t biome_tint(unsigned moisture, unsigned plants);
std::uint16_t creature_tint(const World& world);
void render_life(const World& world, Frame& frame, bool show_creatures = true);

} // namespace petri
