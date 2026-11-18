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

std::uint16_t biome_tint(unsigned moisture, unsigned plants) {
    const unsigned cover = std::min(48u, plants * 4);
    // Three red steps and one green step give the matrix a warm brown base.
    switch (biome_at(moisture)) {
    case Biome::scrub:
        return rgb565(48 + cover / 3, 16 + cover / 6, 0);
    case Biome::grassland:
        return rgb565(8 + cover / 4, 32 + cover / 2, 0);
    case Biome::wetland:
        return rgb565(0, 32 + cover / 2, 32 + cover / 3);
    }
    return 0;
}

std::uint16_t creature_tint(const World& world) {
    if (world.connected_biomes()) {
        return rgb565(224, 128, 32);
    }
    return wide_world ? rgb565(184, 118, 32) : rgb565(255, 192, 48);
}

void render_life(const World& world, Frame& frame, bool show_creatures) {
    constexpr int scale = width / panel_width;
    static_assert(width == panel_width * scale && height == panel_height * scale);
    const auto& cells = world.cells();
    for (int y = 0; y < panel_height; ++y) {
        for (int x = 0; x < panel_width; ++x) {
            unsigned detritus = 0;
            unsigned plants = 0;
            unsigned moisture = 0;
            bool carcass = false;
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    const auto& cell = cells[(y * scale + dy) * width + x * scale + dx];
                    detritus += std::min<std::uint32_t>(16, cell.detritus);
                    plants += cell.plants;
                    moisture += cell.moisture;
                    carcass |= cell.animal > 0;
                }
            }
            constexpr unsigned area = scale * scale;
            const unsigned red = 8 + detritus * 3 / area;
            const unsigned green = 8 + std::min(184u, plants * 8 / area);
            const unsigned blue = 8 + moisture / (8 * area);
            // Magenta stands out against vegetation and amber creatures.
            const auto terrain = world.connected_biomes()
                ? biome_tint(moisture / area, plants / area) : rgb565(red, green, blue);
            const auto carrion = world.connected_biomes() ? rgb565(160, 20, 120) : rgb565(160, 0, 155);
            frame[y * panel_width + x] = carcass ? carrion : terrain;
        }
    }
    if (!show_creatures) {
        return;
    }
    const auto color = creature_tint(world);
    for (const auto& creature : world.creatures()) {
        if (creature.alive()) {
            const int x = (creature.cell % width) / scale;
            const int y = (creature.cell / width) / scale;
            frame[y * panel_width + x] = color;
        }
    }
}

} // namespace petri
