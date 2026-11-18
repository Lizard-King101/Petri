#pragma once

#include "world.hpp"

#include <algorithm>

namespace petri {

constexpr std::uint32_t terrain_edge_hash(int col, int row) {
    auto value = std::uint32_t(col) * 0x9e3779b9u ^ std::uint32_t(row) * 0x85ebca6bu ^ 0xc2b2ae35u;
    value = (value ^ (value >> 16)) * 0x7feb352du;
    return value ^ (value >> 15);
}

// Sample centers of a 16-pixel mask at the 4-, 8-, and 16-pixel zooms. The
// warp is anchored at cell centers. Neighbors share their edge noise knots.
constexpr std::array<int, 2> terrain_sample(int col, int row, int x, int y, int tile) {
    const auto origin = terrain_edge_hash(col, row);
    const auto below = terrain_edge_hash(col, row + 1);
    const auto right = terrain_edge_hash(col + 1, row);
    const int last = tile - 1;
    const auto noise = [](std::uint32_t value, unsigned shift) {
        return int((value >> shift) & 15) - 8;
    };
    const int horizontal = (noise(origin, 0) * (last - y) + noise(below, 0) * y) / last;
    const int vertical = (noise(origin, 8) * (last - x) + noise(right, 8) * x) / last;
    const int dx = horizontal * 3 * x * (last - x) / (last * last);
    const int dy = vertical * 3 * y * (last - y) / (last * last);
    // Arc and straight cases meet at the same mask pixel centers on shared edges.
    return {std::clamp((2 * x + 1) * 16 / tile + dx, 1, 31) | 1,
            std::clamp((2 * y + 1) * 16 / tile + dy, 1, 31) | 1};
}

// A display tile joins four cell centers, half a tile off the world grid.
// Bits are top-left, top-right, bottom-left, bottom-right. Diagonal corners
// stay separate so a one-cell gap does not become a visual land bridge.
constexpr bool dual_coverage_at(unsigned mask, int x, int y, int tile) {
    const auto corner = [&](bool right, bool bottom) {
        const int dx = right ? 2 * tile - x : x;
        const int dy = bottom ? 2 * tile - y : y;
        return dx * dx + dy * dy <= tile * tile;
    };
    switch (mask) {
    case 0: return false;
    case 1: return corner(false, false);
    case 2: return corner(true, false);
    case 3: return y < tile;
    case 4: return corner(false, true);
    case 5: return x < tile;
    case 6: return corner(true, false) || corner(false, true);
    case 7: return !corner(true, true);
    case 8: return corner(true, true);
    case 9: return corner(false, false) || corner(true, true);
    case 10: return x >= tile;
    case 11: return !corner(false, true);
    case 12: return y >= tile;
    case 13: return !corner(true, false);
    case 14: return !corner(false, false);
    case 15: return true;
    }
    return false;
}

constexpr bool dual_coverage(unsigned mask, int x, int y, int tile) {
    return dual_coverage_at(mask, 2 * x + 1, 2 * y + 1, tile);
}

constexpr Biome dual_terrain(const std::array<Biome, 4>& corners, int x, int y, int tile, int col, int row) {
    if (corners[0] == corners[1] && corners[0] == corners[2] && corners[0] == corners[3]) {
        return corners[0];
    }
    unsigned grass = 0, wetland = 0;
    for (unsigned i = 0; i < corners.size(); ++i) {
        grass |= unsigned(corners[i] != Biome::scrub) << i;
        wetland |= unsigned(corners[i] == Biome::wetland) << i;
    }
    const auto sample = terrain_sample(col, row, x, y, tile);
    return dual_coverage_at(wetland, sample[0], sample[1], 16) ? Biome::wetland
        : dual_coverage_at(grass, sample[0], sample[1], 16) ? Biome::grassland : Biome::scrub;
}

} // namespace petri
