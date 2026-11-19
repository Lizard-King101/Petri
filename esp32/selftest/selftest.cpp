#include "selftest.hpp"

#include "brain.hpp"
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

// Heap-allocated once and constructed in place: a World is larger than an ESP32
// task stack, and static storage would crowd the ESP32's static DRAM segment,
// which Bluetooth shrinks by 56 KB.
std::optional<World>* world_storage = new std::optional<World>;
std::optional<LocalPanel>* panel_storage = new std::optional<LocalPanel>;
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
    auto& world = world_storage->emplace(test.seed, test.config);
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
    auto& world = world_storage->emplace(14, observatory_config());
    for (int tick = 0; tick < 256; ++tick) {
        world.step();
    }
    auto& panel = panel_storage->emplace(world);
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

// Times the Phase 1 brain (core/brain.cpp, docs/EMERGENT_LIFE.md) on a real
// world: the per-tick sight bitmaps, the radius-6 sensing scan, and the network,
// with random int8 weights packed four to a 32-bit word. It reads the world and
// never changes it, so FP lines are unaffected.
// mode: 0 full, 1 scan only, 2 network only.
void measure_brains(const Host& host, bool words_memory, int mode) {
    auto& world = world_storage->emplace(14, observatory_config());
    for (int tick = 0; tick < 2000; ++tick) {
        world.step();
    }
    const char* region = "heap";
    constexpr std::size_t bytes = std::size_t(capacity) * brain::brain_words * 4;
    const bool special = words_memory && host.alloc_words;
    auto* words = static_cast<std::uint32_t*>(special ? host.alloc_words(bytes, &region) : ::operator new(bytes));
    if (!words) {
        host.print("BRAIN alloc_failed");
        return;
    }
    // Deterministic random weights, written as whole words.
    std::uint32_t state = 0x9e3779b9u;
    for (std::size_t i = 0; i < bytes / 4; ++i) {
        state = state * 1664525u + 1013904223u;
        words[i] = state;
    }
    auto* sight = new brain::Sight;
    std::uint64_t sight_us = 0, decide_us = 0, decisions = 0;
    std::uint32_t checksum = 0;
    constexpr int ticks = 100;
    for (int tick = 0; tick < ticks; ++tick) {
        auto started = host.now_us();
        *sight = brain::Sight(world);
        sight_us += host.now_us() - started;
        started = host.now_us();
        brain::Senses senses{};
        for (int slot = 0; slot < capacity; ++slot) {
            const auto& creature = world.creatures()[slot];
            if (!creature.alive()) {
                continue;
            }
            if (mode != 2) {
                senses = brain::sense(world, *sight, slot);
            }
            if (mode == 1) {
                for (const auto& direction : senses.direction) {
                    for (const auto value : direction) {
                        checksum = checksum * 31 + static_cast<std::uint8_t>(value);
                    }
                }
            } else {
                // Copy the brain out with 32-bit loads, as instruction RAM requires.
                std::uint32_t local[brain::brain_words];
                const auto* stored = words + std::size_t(slot) * brain::brain_words;
                for (int i = 0; i < brain::brain_words; ++i) {
                    local[i] = stored[i];
                }
                const auto decision = brain::think(senses, reinterpret_cast<const std::int8_t*>(local),
                                                   brain::turn_for(creature.id, world.metrics().ticks));
                checksum = checksum * 31 + decision.action + decision.sprint * 8u + decision.breed * 16u +
                           decision.signal * 32u + static_cast<std::uint8_t>(decision.memory[0]) * 64u;
            }
            ++decisions;
        }
        decide_us += host.now_us() - started;
        world.step();
        host.yield();
    }
    static constexpr const char* modes[] = {"full", "scan", "network"};
    const auto per_decision_ns = decisions ? decide_us * 1000 / decisions : 0;
    char line[320];
    std::snprintf(line, sizeof(line),
                  "BRAIN mode=%s region=%s brain_bytes=%d array_bytes=%u ticks=%d decisions=%s ns_per_decision=%s "
                  "us_per_tick_at_320=%s sight_us_per_tick=%s checksum=%08x",
                  modes[mode], region, brain::brain_words * 4, unsigned(bytes), ticks, decimal(decisions).digits,
                  decimal(per_decision_ns).digits, decimal(per_decision_ns * capacity / 1000).digits,
                  decimal(sight_us / ticks).digits, unsigned(checksum));
    host.print(line);
    delete sight;
    if (!special) {
        ::operator delete(words);
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
    measure_brains(host, false, 1);
    measure_brains(host, false, 2);
    measure_brains(host, false, 0);
    measure_brains(host, true, 2);
    host.print("SELFTEST done");
}

} // namespace petri::selftest
