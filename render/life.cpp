#include "life.hpp"

#include <algorithm>

namespace petri {

int interpolate_axis(int from, int to, std::uint32_t elapsed, std::uint32_t duration) {
    if (duration == 0 || elapsed >= duration) {
        return to;
    }
    const auto delta = std::int64_t(to) - from;
    return from + static_cast<int>(delta * elapsed / duration);
}

std::uint16_t biome_tint(Biome biome, unsigned plants) {
    const unsigned cover = std::min(48u, plants * 4);
    // Three red steps and one green step give the matrix a warm brown base.
    switch (biome) {
    case Biome::scrub:
        return rgb565(48 + cover / 3, 16 + cover / 6, 0);
    case Biome::grassland:
        return rgb565(8 + cover / 4, 32 + cover / 2, 0);
    case Biome::wetland:
        return rgb565(0, 32 + cover / 2, 32 + cover / 3);
    }
    return 0;
}

std::uint16_t creature_tint(bool connected_biomes) {
    if (connected_biomes) {
        return rgb565(224, 128, 32);
    }
    return wide_world ? rgb565(184, 118, 32) : rgb565(255, 192, 48);
}

namespace {

// One overview pixel of the connected world. It averages the tints of the cells
// it covers, so biome edges blend, shades open water by depth, and adds a fixed
// grain so broad areas read as ground rather than flat color.
std::uint16_t overview_ground(const RenderState& state, int x, int y) {
    constexpr int scale = width / panel_width;
    unsigned red = 0, green = 0, blue = 0;
    for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
            const auto& cell = state.cells[(y * scale + dy) * width + x * scale + dx];
            const unsigned cover = std::min(48u, unsigned(cell.plants) * 4);
            switch (cell.biome()) {
            case Biome::scrub:
                red += 48 + cover / 3;
                green += 16 + cover / 6;
                break;
            case Biome::grassland:
                red += 8 + cover / 4;
                green += 32 + cover / 2;
                break;
            case Biome::wetland: {
                // Deeper water is bluer and darker.
                const unsigned depth = (cell.moisture() - 128) / 8;
                green += 32 + cover / 2 - std::min(12u, depth);
                blue += 32 + cover / 3 + depth;
                break;
            }
            }
        }
    }
    constexpr unsigned area = scale * scale;
    auto grain = static_cast<std::uint32_t>(x) * 0x9e3779b1u ^ static_cast<std::uint32_t>(y) * 0x85ebca6bu;
    grain ^= grain >> 15;
    grain *= 0x2c1b3c6du;
    const int step = static_cast<int>((grain >> 13) % 3) - 1;
    const auto channel = [&](unsigned sum, int bump) {
        return static_cast<unsigned>(std::clamp(int(sum / area) + bump, 0, 255));
    };
    return rgb565(channel(red, red > green ? 4 * step : 0), channel(green, 4 * step), channel(blue, blue ? 4 * step : 0));
}

} // namespace

void render_life(const RenderState& state, Frame& frame, bool show_creatures) {
    constexpr int scale = width / panel_width;
    static_assert(width == panel_width * scale && height == panel_height * scale);
    const auto& cells = state.cells;
    for (int y = 0; y < panel_height; ++y) {
        for (int x = 0; x < panel_width; ++x) {
            unsigned detritus = 0;
            unsigned plants = 0;
            unsigned moisture = 0;
            bool carcass = false;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    const auto& cell = cells[(y * scale + dy) * width + x * scale + dx];
                    detritus += cell.detritus();
                    plants += cell.plants;
                    moisture += cell.moisture();
                    carcass |= cell.carcass();
                }
            }
            constexpr unsigned area = scale * scale;
            if (state.connected_biomes && !carcass) {
                frame[y * panel_width + x] = overview_ground(state, x, y);
                continue;
            }
            const unsigned red = 8 + detritus * 3 / area;
            const unsigned green = 8 + std::min(184u, plants * 8 / area);
            const unsigned blue = 8 + moisture / (8 * area);
            // Magenta stands out against vegetation and amber creatures.
            const auto terrain = state.connected_biomes
                ? biome_tint(biome_at(moisture / area), plants / area) : rgb565(red, green, blue);
            const auto carrion = state.connected_biomes ? rgb565(160, 20, 120) : rgb565(160, 0, 155);
            frame[y * panel_width + x] = carcass ? carrion : terrain;
        }
    }
    if (!show_creatures) {
        return;
    }
    const auto color = creature_tint(state.connected_biomes);
    for (const auto& creature : state.creatures) {
        if (creature.alive()) {
            const int x = (creature.cell % width) / scale;
            const int y = (creature.cell / width) / scale;
            frame[y * panel_width + x] = color;
        }
    }
}

void render_life(const World& world, Frame& frame, bool show_creatures) {
    static RenderState state;
    capture(world, state);
    render_life(state, frame, show_creatures);
}

} // namespace petri
