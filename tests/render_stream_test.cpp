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
        check(f.decoder.stats().checksum_mismatches == 0 && f.decoder.stats().malformed == 0 &&
                  f.decoder.stats().creature_gaps == 0,
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

// Re-cuts payloads into one payload per record, so a test can lose a single record.
std::vector<Payload> split_records(const std::vector<Payload>& payloads) {
    std::vector<Payload> records;
    for (const auto& payload : payloads) {
        for (std::size_t at = 0; at + 3 <= payload.size();) {
            const std::size_t length = payload[at + 1] | payload[at + 2] << 8;
            records.emplace_back(payload.begin() + at, payload.begin() + at + 3 + length);
            at += 3 + length;
        }
    }
    return records;
}

std::size_t creature_records(const std::vector<Payload>& records) {
    return std::count_if(records.begin(), records.end(), [](const Payload& record) {
        return record[0] == static_cast<std::uint8_t>(stream::Record::creatures);
    });
}

void full_capacity_list() {
    // Every slot alive, with looks that use all 32 bits.
    auto world = std::make_unique<World>(14, stream_config());
    auto state = std::make_unique<RenderState>();
    capture(*world, *state);
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        state->creatures[slot] = {CreatureId(1000 + slot), static_cast<std::uint16_t>(slot * 7 % (width * height)),
                                  Look{0x80000001u ^ std::uint32_t(slot) * 0x9e3779b9u}};
    }
    auto encoder = std::make_unique<stream::Encoder>();
    auto mirror = std::make_unique<RenderState>();
    stream::Decoder decoder;
    EventLog none, received;

    // Even a one-payload budget carries the whole list.
    std::vector<Payload> payloads;
    encoder->encode(*state, none, 1, collect, &payloads);
    check(payloads.size() == 1, "A full creature list did not fit one payload");
    const auto full_bytes = payloads[0].size();
    check(creature_records(split_records(payloads)) == 2, "A full list should take two creature records");
    check(decoder.apply(payloads[0].data(), payloads[0].size(), *mirror, received) && decoder.complete(),
          "A full creature list was not decoded");
    check(mirror->creatures == state->creatures, "A full creature list changed in transit");

    // Losing the second creature record leaves only its slots as they were.
    const auto before = mirror->creatures;
    for (auto& creature : state->creatures) {
        creature.cell = static_cast<std::uint16_t>((creature.cell + 1) % (width * height));
    }
    payloads.clear();
    encoder->encode(*state, none, 16, collect, &payloads);
    const auto records = split_records(payloads);
    std::size_t seen = 0, second_first = 0;
    for (const auto& record : records) {
        if (record[0] == static_cast<std::uint8_t>(stream::Record::creatures) && ++seen == 2) {
            second_first = record[3] | record[4] << 8;
            continue;
        }
        check(decoder.apply(record.data(), record.size(), *mirror, received), "A valid record was rejected");
    }
    check(second_first == 255, "The second creature record should start after 255 creatures");
    check(!decoder.complete() && decoder.stats().creature_gaps == 1, "A lost creature record was not reported");
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        check(mirror->creatures[slot] == (slot < second_first ? state->creatures[slot] : before[slot]),
              "A lost creature record changed slots outside its range");
    }

    // The next update repairs the list.
    payloads.clear();
    encoder->encode(*state, none, 16, collect, &payloads);
    for (const auto& payload : payloads) {
        check(decoder.apply(payload.data(), payload.size(), *mirror, received), "A valid payload was rejected");
    }
    check(decoder.complete() && same_state(*mirror, *state), "The next update did not repair the creature list");

    // Records that break the slot rules are rejected, and the mirror keeps its creatures.
    const auto creature_record = [](std::uint16_t first, std::uint16_t span, std::vector<std::uint16_t> slots) {
        Payload record{static_cast<std::uint8_t>(stream::Record::creatures), 0, 0,
                       static_cast<std::uint8_t>(first), static_cast<std::uint8_t>(first >> 8),
                       static_cast<std::uint8_t>(span), static_cast<std::uint8_t>(span >> 8),
                       static_cast<std::uint8_t>(slots.size())};
        for (const auto slot : slots) {
            const std::uint8_t entry[] = {static_cast<std::uint8_t>(slot), static_cast<std::uint8_t>(slot >> 8),
                                          9, 0, 0, 0, 3, 0, 1, 2, 3, 4};
            record.insert(record.end(), std::begin(entry), std::end(entry));
        }
        const auto length = record.size() - 3;
        record[1] = static_cast<std::uint8_t>(length);
        record[2] = static_cast<std::uint8_t>(length >> 8);
        return record;
    };
    const auto rejected = [&](const Payload& record) {
        const auto creatures = mirror->creatures;
        return !decoder.apply(record.data(), record.size(), *mirror, received) && mirror->creatures == creatures;
    };
    check(rejected(creature_record(10, 5, {12, 11})), "Out-of-order creature slots were accepted");
    check(rejected(creature_record(10, 5, {15})), "A creature outside its record's range was accepted");
    check(rejected(creature_record(capacity - 2, 3, {})), "A slot range past capacity was accepted");
    check(rejected(creature_record(10, 1, {10, 10})), "More creatures than slots were accepted");
    check(!rejected(creature_record(10, 5, {10, 14})), "A valid creature record was rejected");
    check(mirror->creatures[10].id == 9 && !mirror->creatures[11].alive() && mirror->creatures[14].look.bits == 0x04030201u,
          "A valid creature record was not applied to its range");
    std::printf("STREAM full_capacity creatures=%d one_payload_bytes=%zu\n", capacity, full_bytes);
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
    full_capacity_list();
}
