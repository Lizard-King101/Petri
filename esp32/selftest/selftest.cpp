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

// Prototype of the planned Phase 1 brain (docs/EMERGENT_LIFE.md), timed on a
// real world: the radius-6 sensing scan plus a shared direction scorer and a
// global head with int8 weights packed four to a 32-bit word. It reads the world
// and never changes it, so FP lines are unaffected.
namespace brain {

constexpr int radius = 6, directions = 4;
constexpr int direction_inputs = 7, self_inputs = 11, scorer_hidden = 6, head_hidden = 4, head_outputs = 6;
constexpr int scorer_bytes = (direction_inputs + self_inputs) * scorer_hidden + scorer_hidden + scorer_hidden + 1;
constexpr int head_inputs = self_inputs + direction_inputs;
constexpr int head_bytes = head_inputs * head_hidden + head_hidden + head_hidden * head_outputs + head_outputs;
constexpr int brain_words = (scorer_bytes + head_bytes + 3) / 4;

// The scan's 84 cells, in a fixed order, with their direction and closeness.
struct Offset {
    std::int8_t dx, dy;
    std::uint8_t direction, close;
};

constexpr std::array<Offset, 84> make_offsets() {
    std::array<Offset, 84> table{};
    int n = 0;
    for (int dy = -radius; dy <= radius; ++dy) {
        const int span = radius - (dy < 0 ? -dy : dy);
        for (int dx = -span; dx <= span; ++dx) {
            if (!dx && !dy) {
                continue;
            }
            const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
            table[n++] = {static_cast<std::int8_t>(dx), static_cast<std::int8_t>(dy),
                          static_cast<std::uint8_t>(ax >= ay ? (dx > 0 ? 1 : 3) : (dy > 0 ? 2 : 0)),
                          static_cast<std::uint8_t>(radius + 1 - (ax + ay))};
        }
    }
    return table;
}

constexpr auto offsets = make_offsets();

int clip(int value) {
    value >>= 6;
    return value < -127 ? -127 : value > 127 ? 127 : value;
}

// One layer: `Outputs` units over `Inputs` values, each followed by its bias.
template<int Inputs, int Outputs>
const std::int8_t* layer(const std::int8_t* w, const int* in, int* out) {
    for (int o = 0; o < Outputs; ++o) {
        int sum = 0;
        for (int i = 0; i < Inputs; ++i) {
            sum += w[i] * in[i];
        }
        w += Inputs;
        out[o] = clip(sum + *w++ * 64);
    }
    return w;
}

// Senses and decides for one creature; returns the chosen action (0 stay, 1-4 a direction) plus head outputs.
// mode: 0 full, 1 scan only, 2 network only.
int decide(const World& world, const std::int16_t* occupants, const std::uint32_t* words, int slot, int mode) {
    const auto& creature = world.creatures()[slot];
    const int x = creature.cell % width, y = creature.cell / width;
    int sense[directions][direction_inputs] = {};
    int nearest[directions] = {};
    const bool interior = x >= radius && x < width - radius && y >= radius && y < height - radius;
    for (std::size_t n = 0; n < (mode == 2 ? 0 : offsets.size()); ++n) {
        const auto& at = offsets[n];
        const int xx = x + at.dx, yy = y + at.dy;
        if (!interior && (xx < 0 || xx >= width || yy < 0 || yy >= height)) {
            continue;
        }
        const int d = at.direction, close = at.close;
        const int index = yy * width + xx;
        const auto& cell = world.cells()[index];
        sense[d][0] += cell.plants * close;
        sense[d][1] += cell.animal * close;
        const int other = occupants[index];
        if (other >= 0) {
            const auto& them = world.creatures()[other];
            sense[d][5] += close;
            if (close > nearest[d]) {
                nearest[d] = close;
                sense[d][2] = close * 16;
                sense[d][3] = (int(them.genes.body_size) - int(creature.genes.body_size)) * 8;
                sense[d][4] = creature.genes.prey_resemblance(them.genes) * 8;
            }
            sense[d][6] += (them.id & 1) * close;
        }
    }
    if (mode == 1) {
        return sense[0][0] + sense[1][1] + sense[2][2] + sense[3][5];
    }
    const auto& here = world.cells()[creature.cell];
    const int self[self_inputs] = {creature.energy, creature.nutrients_carried * 8, creature.breeding_cooldown,
                                   here.plants * 4, here.animal * 4, int(here.moisture) - 128, nearest[0],
                                   nearest[1], nearest[2], 0, 0};
    // Copy the brain out with 32-bit loads, as instruction RAM requires.
    std::uint32_t local[brain_words];
    for (int i = 0; i < brain_words; ++i) {
        local[i] = words[i];
    }
    const auto* weights = reinterpret_cast<const std::int8_t*>(local);
    int best = -1 << 30, action = 0;
    int summed[direction_inputs] = {};
    int in[direction_inputs + self_inputs];
    for (int i = 0; i < self_inputs; ++i) {
        in[direction_inputs + i] = self[i];
    }
    for (int d = 0; d < directions; ++d) {
        for (int i = 0; i < direction_inputs; ++i) {
            in[i] = sense[d][i] > 255 ? 255 : sense[d][i];
            summed[i] += in[i];
        }
        int hidden[scorer_hidden], score;
        const auto* w = layer<direction_inputs + self_inputs, scorer_hidden>(weights, in, hidden);
        layer<scorer_hidden, 1>(w, hidden, &score);
        if (score > best) {
            best = score;
            action = d + 1;
        }
    }
    int head_in[head_inputs], hidden[head_hidden], out[head_outputs];
    for (int i = 0; i < self_inputs; ++i) {
        head_in[i] = self[i];
    }
    for (int i = 0; i < direction_inputs; ++i) {
        head_in[self_inputs + i] = summed[i] / directions;
    }
    const auto* w = layer<head_inputs, head_hidden>(weights + scorer_bytes, head_in, hidden);
    layer<head_hidden, head_outputs>(w, hidden, out);
    return (out[0] > best ? 0 : action) + (out[1] > 0 ? 8 : 0) + (out[2] > 0 ? 16 : 0);
}

} // namespace brain

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
    auto* occupants = new std::int16_t[width * height];
    std::uint64_t sense_us = 0, decide_us = 0, decisions = 0;
    std::uint32_t checksum = 0;
    constexpr int ticks = 100;
    for (int tick = 0; tick < ticks; ++tick) {
        auto started = host.now_us();
        for (int i = 0; i < width * height; ++i) {
            occupants[i] = -1;
        }
        for (int slot = 0; slot < capacity; ++slot) {
            if (world.creatures()[slot].alive()) {
                occupants[world.creatures()[slot].cell] = static_cast<std::int16_t>(slot);
            }
        }
        sense_us += host.now_us() - started;
        started = host.now_us();
        for (int slot = 0; slot < capacity; ++slot) {
            if (world.creatures()[slot].alive()) {
                checksum = checksum * 31 + static_cast<std::uint32_t>(
                    brain::decide(world, occupants, words + std::size_t(slot) * brain::brain_words, slot, mode));
                ++decisions;
            }
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
                  "us_per_tick_at_320=%s occupancy_us_per_tick=%s checksum=%08x",
                  modes[mode], region, brain::brain_words * 4, unsigned(bytes), ticks, decimal(decisions).digits,
                  decimal(per_decision_ns).digits, decimal(per_decision_ns * capacity / 1000).digits,
                  decimal(sense_us / ticks).digits, unsigned(checksum));
    host.print(line);
    delete[] occupants;
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
