#include "selftest.hpp"

#include "observation.hpp"
#include "panel.hpp"
#include "world.hpp"

#include <array>
#include <cstdio>
#include <optional>

namespace petri::selftest {
namespace {

struct Case {
    const char* name;
    std::uint32_t seed;
    Config config;
};

Config food_web_config() {
    Config config;
    config.initial_nutrients = 80;
    config.carcass_decay = 1;
    config.predation = config.food_web = true;
    return config;
}

Config full_config() {
    Config config;
    config.initial_nutrients = 24;
    config.carcass_decay = 1;
    config.predation = config.food_web = true;
    config.connected_biomes = config.irregular_terrain = wide_world;
    config.sexual_reproduction = config.nests = config.guarding = config.provisioning = true;
    config.adaptive_hunts = config.kin_avoidance = true;
    config.faction_markers = config.cross_faction_hunts = true;
    config.fruiting_plants = true;
    config.dynamic_environment = wide_world;
    return config;
}

constexpr std::array<std::uint64_t, 4> checkpoints{1, 256, 2000, 10000};
// Steady-state timing covers the steps between the last two checkpoints.
constexpr std::uint64_t timing_from = 2000;
constexpr std::uint32_t bucket_us = 25;
constexpr std::size_t bucket_count = 400;

// Static and constructed in place: a World is larger than an ESP32 task stack,
// so even a temporary copy would overflow it.
std::optional<World> world_storage;
std::optional<Panel> panel_storage;
std::array<std::uint32_t, bucket_count> histogram{};

// Some embedded printf builds (newlib-nano on the SAMD51) cannot format
// 64-bit integers, so these helpers produce the text themselves.
struct Text {
    char digits[21];
};

Text decimal(std::uint64_t value) {
    Text text{};
    char reversed[21];
    int count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    for (int i = 0; i < count; ++i) {
        text.digits[i] = reversed[count - 1 - i];
    }
    return text;
}

Text hex16(std::uint64_t value) {
    Text text{};
    for (int i = 15; i >= 0; --i, value >>= 4) {
        text.digits[i] = "0123456789abcdef"[value & 15];
    }
    return text;
}

void print_fp(const Host& host, const char* name, const World& world) {
    const auto& metrics = world.metrics();
    char line[200];
    std::snprintf(line, sizeof(line), "FP case=%s tick=%s fingerprint=%s population=%u births=%s deaths=%s kills=%s",
                  name, decimal(metrics.ticks).digits, hex16(world.fingerprint()).digits, metrics.population,
                  decimal(metrics.births).digits, decimal(metrics.deaths).digits, decimal(metrics.kills).digits);
    host.print(line);
}

std::uint32_t percentile_us(std::uint64_t total, unsigned percent) {
    const std::uint64_t target = (total * percent + 99) / 100;
    std::uint64_t seen = 0;
    for (std::size_t bucket = 0; bucket < bucket_count; ++bucket) {
        seen += histogram[bucket];
        if (seen >= target) {
            return static_cast<std::uint32_t>((bucket + 1) * bucket_us);
        }
    }
    return bucket_count * bucket_us;
}

void run_case(const Host& host, const Case& test) {
    auto& world = world_storage.emplace(test.seed, test.config);
    histogram.fill(0);
    std::uint64_t busy_us = 0, timed_steps = 0, max_us = 0;
    std::uint64_t last_yield = host.now_us();
    for (const auto checkpoint : checkpoints) {
        while (world.metrics().ticks < checkpoint) {
            const auto started = host.now_us();
            world.step();
            const auto elapsed = host.now_us() - started;
            if (world.metrics().ticks > timing_from) {
                busy_us += elapsed;
                ++timed_steps;
                max_us = elapsed > max_us ? elapsed : max_us;
                ++histogram[elapsed / bucket_us < bucket_count ? elapsed / bucket_us : bucket_count - 1];
            }
            if (started - last_yield >= 20000) {
                host.yield();
                last_yield = host.now_us();
            }
        }
        print_fp(host, test.name, world);
    }
    char line[200];
    std::snprintf(line, sizeof(line), "TIMING case=%s steps=%s ticks_per_second=%s mean_us=%s p99_us<=%s max_us=%s",
                  test.name, decimal(timed_steps).digits,
                  decimal(busy_us ? timed_steps * 1000000 / busy_us : 0).digits,
                  decimal(timed_steps ? busy_us / timed_steps : 0).digits,
                  decimal(percentile_us(timed_steps, 99)).digits, decimal(max_us).digits);
    host.print(line);
}

void measure_render(const Host& host) {
    // Frames from the observatory world, which the current firmware renders.
    auto& world = world_storage.emplace(14, observatory_config());
    for (int tick = 0; tick < 256; ++tick) {
        world.step();
    }
    auto& panel = panel_storage.emplace(world);
    static constexpr const char* names[] = {"close", "hunt", "life", "habitat"};
    std::uint32_t now_ms = 0;
    for (const auto* name : names) {
        std::uint64_t total = 0, max_us = 0;
        constexpr int frames = 200;
        for (int frame = 0; frame < frames; ++frame) {
            now_ms += 50;
            if (frame % 20 == 0) {
                world.step(panel.before_step(now_ms));
                panel.after_step(now_ms * 1000, 1000000);
            }
            const auto started = host.now_us();
            panel.draw(now_ms, now_ms * 1000, 0);
            const auto elapsed = host.now_us() - started;
            total += elapsed;
            max_us = elapsed > max_us ? elapsed : max_us;
        }
        char line[160];
        std::snprintf(line, sizeof(line), "RENDER view=%s frames=%d mean_us=%s max_us=%s", name, frames,
                      decimal(total / frames).digits, decimal(max_us).digits);
        host.print(line);
        panel.cycle_view(now_ms);
        host.yield();
    }
}

} // namespace

void run(const Host& host) {
    char line[160];
    std::snprintf(line, sizeof(line), "SELFTEST world=%dx%d world_bytes=%u panel_bytes=%u", width, height,
                  unsigned(sizeof(World)), unsigned(sizeof(Panel)));
    host.print(line);
    const Case cases[] = {
        {"default", 42, Config{48, true, 1}},
        {"food_web", 4, food_web_config()},
        {"full", 14, full_config()},
        {"observatory", 14, observatory_config()},
    };
    for (const auto& test : cases) {
        run_case(host, test);
    }
    measure_render(host);
    host.print("SELFTEST done");
}

} // namespace petri::selftest
