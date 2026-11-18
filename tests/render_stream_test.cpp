#include "observation.hpp"
#include "panel.hpp"
#include "render_stream.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace {

using namespace petri;

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

using Payload = std::vector<std::uint8_t>;

void collect(void* context, const std::uint8_t* data, std::size_t size) {
    static_cast<std::vector<Payload>*>(context)->emplace_back(data, data + size);
}

bool same_state(const RenderState& a, const RenderState& b) {
    return a.tick == b.tick && a.connected_biomes == b.connected_biomes && a.cells == b.cells &&
           a.creatures == b.creatures;
}

Config stream_config() {
    if constexpr (wide_world) {
        return observatory_config();
    }
    Config config;
    config.initial_nutrients = 48;
    config.carcass_decay = 1;
    config.predation = config.food_web = true;
    config.sexual_reproduction = config.nests = config.guarding = config.provisioning = true;
    config.adaptive_hunts = config.faction_markers = config.fruiting_plants = true;
    return config;
}

struct Fixture {
    std::unique_ptr<World> world = std::make_unique<World>(14, stream_config());
    std::unique_ptr<RenderState> expected = std::make_unique<RenderState>();
    std::unique_ptr<RenderState> mirror = std::make_unique<RenderState>();
    std::unique_ptr<stream::Encoder> encoder = std::make_unique<stream::Encoder>();
    stream::Decoder decoder;
    EventLog sent, received;

    // Steps the world, encodes one update, and decodes every payload except dropped ones.
    stream::EncodeStats update(int ticks, std::size_t max_payloads, int drop_every = 0, int* counter = nullptr) {
        sent.clear();
        for (int tick = 0; tick < ticks; ++tick) {
            world->step(sent.observer(*world));
        }
        std::vector<Payload> payloads;
        const auto stats = encoder->encode(*world, sent, max_payloads, collect, &payloads);
        for (const auto& payload : payloads) {
            if (drop_every && counter && ++*counter % drop_every == 0) {
                continue;
            }
            check(decoder.apply(payload.data(), payload.size(), *mirror, received), "A valid payload was rejected");
        }
        capture(*world, *expected);
        return stats;
    }
};

void lossless_round_trip() {
    for (const int ticks : {1, 4, 12}) {
        Fixture f;
        // The first update after priming may take several payloads.
        f.update(0, 16);
        std::size_t total = 0, worst = 0, cells = 0, worst_cells = 0, updates = 0;
        for (int i = 0; i < 300; ++i) {
            const auto stats = f.update(ticks, 16);
            check(stats.cells_pending == 0, "A generous budget left cells pending");
            check(f.decoder.complete(), "An update did not complete");
            check(same_state(*f.mirror, *f.expected), "The mirror differs from the world's render state");
            check(f.received.size() == f.sent.size() &&
                      std::equal(f.sent.begin(), f.sent.end(), f.received.begin()),
                  "Decoded events differ from the recorded events");
            total += stats.bytes;
            worst = std::max(worst, stats.bytes);
            cells += stats.cells_sent;
            worst_cells = std::max(worst_cells, stats.cells_sent);
            ++updates;
        }
        check(f.decoder.stats().checksum_mismatches == 0 && f.decoder.stats().malformed == 0,
              "A lossless stream reported damage");
        std::printf("STREAM world=%dx%d ticks_per_update=%d mean_bytes=%zu max_bytes=%zu mean_cells=%zu max_cells=%zu\n",
                    width, height, ticks, total / updates, worst, cells / updates, worst_cells);
    }
}

void panel_frames_match() {
    // A Panel drawing the decoded mirror matches a LocalPanel drawing the world.
    Fixture f;
    f.update(0, 16);
    LocalPanel local(*f.world);
    Panel remote(*f.mirror);
    f.decoder.on_update([](void* panel) { static_cast<Panel*>(panel)->begin_update(0); }, &remote);
    std::uint32_t now_ms = 1000;
    std::size_t frames = 0;
    for (int tick = 0; tick < 900; ++tick) {
        if (tick % 150 == 149) {
            local.cycle_view(now_ms);
            remote.cycle_view(now_ms);
        }
        // Mirror LocalPanel's hooks: snapshot, step, then apply the events.
        auto observer = local.before_step(now_ms);
        remote.begin_update(now_ms);
        f.sent.clear();
        StepObserver both;
        struct Pair {
            StepObserver a, b;
        } pair{observer, f.sent.observer(*f.world)};
        both.context = &pair;
        both.record_hunt = [](void* c, std::uint16_t h, std::uint16_t p) {
            auto& x = *static_cast<Pair*>(c);
            x.a.record_hunt(x.a.context, h, p);
            x.b.record_hunt(x.b.context, h, p);
        };
        both.record_meal = [](void* c, const Creature& creature, bool meat) {
            auto& x = *static_cast<Pair*>(c);
            x.a.record_meal(x.a.context, creature, meat);
            x.b.record_meal(x.b.context, creature, meat);
        };
        both.record_attack = [](void* c, const Creature& a, const Creature& t, bool landed) {
            auto& x = *static_cast<Pair*>(c);
            x.a.record_attack(x.a.context, a, t, landed);
            x.b.record_attack(x.b.context, a, t, landed);
        };
        both.record_death = [](void* c, const Creature& creature, DeathCause cause) {
            auto& x = *static_cast<Pair*>(c);
            x.a.record_death(x.a.context, creature, cause);
            x.b.record_death(x.b.context, creature, cause);
        };
        f.world->step(both);
        std::vector<Payload> payloads;
        f.encoder->encode(*f.world, f.sent, 16, collect, &payloads);
        // The hook already took the snapshot above with the right time; keep it out of this pass.
        f.decoder.on_update(nullptr, nullptr);
        for (const auto& payload : payloads) {
            check(f.decoder.apply(payload.data(), payload.size(), *f.mirror, f.received), "Payload was rejected");
        }
        const std::uint32_t interval = 1000000 >> (tick % 4);
        local.after_step(now_ms * 1000, interval);
        remote.apply(f.received);
        remote.end_update(now_ms * 1000, interval);
        for (int frame = 0; frame < 3; ++frame) {
            now_ms += 47;
            local.draw(now_ms, now_ms * 1000, tick % 4);
            remote.draw(now_ms, now_ms * 1000, tick % 4);
            check(local.frame() == remote.frame(), "A frame from the mirror differs from the local frame");
            ++frames;
        }
    }
    std::printf("STREAM frames_compared=%zu\n", frames);
}

void tight_budget_catches_up() {
    Fixture f;
    for (int i = 0; i < 100; ++i) {
        f.update(12, 1);
    }
    int updates = 0;
    while (!same_state(*f.mirror, *f.expected) && updates < 20) {
        f.update(0, 1);
        ++updates;
    }
    check(same_state(*f.mirror, *f.expected), "A one-payload budget did not catch up");
    std::printf("STREAM one_payload_catch_up_updates=%d\n", updates);
}

void dropped_payloads_repair() {
    Fixture f;
    f.update(0, 16);
    int counter = 0;
    for (int i = 0; i < 200; ++i) {
        f.update(4, 16, 3, &counter);
    }
    check(f.decoder.stats().checksum_mismatches > 0, "Lost payloads were not detected");
    // With the world paused, one full refresh cycle repairs the mirror.
    const int cycle = int((width * height + stream::refresh_cells - 1) / stream::refresh_cells);
    for (int i = 0; i < cycle + 1; ++i) {
        f.update(0, 16);
    }
    check(same_state(*f.mirror, *f.expected), "The rolling refresh did not repair the mirror");
    std::printf("STREAM dropped_every_3rd_payload mismatches=%u repaired_within=%d_updates\n",
                f.decoder.stats().checksum_mismatches, cycle + 1);
}

void malformed_payloads_rejected() {
    Fixture f;
    f.update(1, 16);
    std::vector<Payload> payloads;
    f.encoder->encode(*f.world, f.sent, 16, collect, &payloads);
    auto bad = payloads.front();
    bad.resize(bad.size() / 2);
    check(!f.decoder.apply(bad.data(), bad.size(), *f.mirror, f.received), "A truncated payload was accepted");
    const std::uint8_t unknown[] = {0x77, 0, 0};
    check(!f.decoder.apply(unknown, sizeof(unknown), *f.mirror, f.received), "An unknown record was accepted");
    const std::uint8_t outside[] = {4, 5, 0, 0xff, 0xff, 1, 2, 3};
    check(!f.decoder.apply(outside, sizeof(outside), *f.mirror, f.received), "Cells past the map were accepted");
    // Random bytes never crash the decoder.
    std::uint32_t state = 12345;
    for (int trial = 0; trial < 20000; ++trial) {
        Payload noise(1 + trial % 300);
        for (auto& byte : noise) {
            state = state * 1664525u + 1013904223u;
            byte = static_cast<std::uint8_t>(state >> 24);
        }
        f.decoder.apply(noise.data(), noise.size(), *f.mirror, f.received);
    }
}

void observation_from_mirror_matches() {
    // With full details, an observation built from the mirror equals the World-based one.
    auto world = std::make_unique<World>(14, stream_config());
    auto panel = std::make_unique<LocalPanel>(*world);
    for (int tick = 0; tick < 300; ++tick) {
        world->step(panel->before_step(tick * 1000));
        panel->after_step(tick * 1000000u, 1000000);
    }
    panel->draw(300000, 300000000u, 0);
    std::array<CreatureDetails, capacity> details{};
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        const auto& c = world->creatures()[slot];
        if (c.alive()) {
            details[slot] = {c.id, c.parent, c.founder, c.plants_eaten, c.animal_eaten, c.generation, c.energy,
                             c.genes.digestion, c.genes.thrift, c.genes.moisture_preference};
        }
    }
    const auto& m = world->metrics();
    auto direct = std::make_unique<Observation>();
    auto mirrored = std::make_unique<Observation>();
    const auto direct_size = capture_observation(*world, *panel, 7, *direct);
    const auto mirrored_size = capture_observation(panel->state(), details, {m.ticks, m.births, m.deaths, m.kills},
                                                   panel->panel(), 7, *mirrored);
    check(direct_size == mirrored_size && std::equal(direct->begin(), direct->begin() + direct_size, mirrored->begin()),
          "The mirror observation differs from the World observation");
}

} // namespace

int main() {
    observation_from_mirror_matches();
    lossless_round_trip();
    panel_frames_match();
    tight_budget_catches_up();
    dropped_payloads_repair();
    malformed_payloads_rejected();
}
