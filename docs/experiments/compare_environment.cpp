#include "observation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv) {
    std::uint32_t seed = 0;
    if (argc != 4) {
        std::cerr << "Usage: compare_environment SEED control|dynamic OUTPUT_PREFIX\n";
        return 2;
    }
    const std::string_view input = argv[1], mode = argv[2];
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), seed);
    if (parsed.ec != std::errc{} || parsed.ptr != input.data() + input.size() ||
        (mode != "control" && mode != "dynamic") || !petri::wide_world) {
        return 2;
    }
    auto config = petri::observatory_config();
    config.dynamic_environment = mode == "dynamic";
    petri::World world(seed, config);
    constexpr std::array samples{256u, 4096u, 16384u, 65536u, 131072u, 262144u, 524288u};
    std::array<petri::Biome, petri::width * petri::height> first{}, previous{};
    std::array<bool, petri::width * petri::height> first_shrubs{};
    unsigned peak = world.metrics().population;
    unsigned low = peak;
    std::cout << "seed,treatment,tick,population,min_population,peak_population,births,capacity_blocks,"
                 "scrub,grassland,wetland,changed_since_start,changed_since_previous,shrubs,changed_shrubs,"
                 "fruit_eaten,material\n";
    for (const auto until : samples) {
        while (world.metrics().ticks < until) {
            world.step();
            peak = std::max(peak, world.metrics().population);
            low = std::min(low, world.metrics().population);
        }
        std::array<unsigned, 3> biomes{};
        unsigned changed = 0, since_previous = 0, shrubs = 0, changed_shrubs = 0;
        std::uint64_t material = world.metrics().population * petri::body_mass;
        for (std::size_t index = 0; index < first.size(); ++index) {
            const auto& cell = world.cells()[index];
            const auto biome = petri::biome_at(cell.moisture);
            const bool shrub = world.fruiting_shrub(index);
            if (until == samples.front()) {
                first[index] = previous[index] = biome;
                first_shrubs[index] = shrub;
            }
            ++biomes[static_cast<unsigned>(biome)];
            changed += biome != first[index];
            since_previous += biome != previous[index];
            previous[index] = biome;
            shrubs += shrub;
            changed_shrubs += shrub != first_shrubs[index];
            material += cell.nutrients + cell.plants + cell.animal + cell.detritus + world.fruit_at(index);
        }
        for (const auto& creature : world.creatures()) {
            material += creature.nutrients_carried + creature.provisions();
        }
        if (material != 24 * first.size() + petri::founder_count * petri::body_mass) {
            std::cerr << "Material budget changed at tick " << until << '\n';
            return 1;
        }
        const auto& metrics = world.metrics();
        std::cout << seed << ',' << mode << ',' << until << ',' << metrics.population << ',' << low << ','
                  << peak << ',' << metrics.births << ',' << metrics.capacity_blocks << ',' << biomes[0] << ','
                  << biomes[1] << ',' << biomes[2] << ',' << changed << ',' << since_previous << ',' << shrubs << ','
                  << changed_shrubs << ',' << metrics.fruit_eaten << ',' << material << '\n' << std::flush;

        petri::Frame frame;
        petri::render_life(world, frame, false);
        std::ofstream image(std::string(argv[3]) + "-" + std::to_string(until) + ".ppm", std::ios::binary);
        image << "P6\n64 32\n255\n";
        for (const auto color : frame) {
            const std::array<char, 3> rgb{char((color >> 12) * 17), char(((color >> 7) & 15) * 17),
                                          char(((color >> 1) & 15) * 17)};
            image.write(rgb.data(), rgb.size());
        }
        if (!image) {
            return 1;
        }
    }
}
