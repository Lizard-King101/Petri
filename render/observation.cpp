#include "observation.hpp"

#include <limits>

namespace petri {
namespace {

struct Writer {
    std::uint8_t* next;

    template<class Integer>
    void put(Integer value) {
        for (std::size_t i = 0; i < sizeof(value); ++i) {
            *next++ = static_cast<std::uint8_t>(std::uint64_t(value) >> (8 * i));
        }
    }
};

} // namespace

std::size_t capture_observation(const World& world, const LocalPanel& panel, std::uint32_t revision,
                                Observation& output) {
    const auto& metrics = world.metrics();
    const auto body_size = observation_header_size + panel.frame().size() * 2 +
        metrics.population * observation_creature_size;

    Writer writer{output.data()};
    for (const char byte : {'P', 'T', 'R', '4'}) {
        writer.put(std::uint8_t(byte));
    }
    writer.put(std::uint32_t(body_size));
    writer.put(std::uint16_t(width));
    writer.put(std::uint16_t(height));
    writer.put(std::uint16_t(metrics.population));
    writer.put(metrics.ticks);
    writer.put(metrics.births);
    writer.put(metrics.deaths);
    writer.put(metrics.kills);

    const auto& camera = panel.camera();
    writer.put(static_cast<std::uint8_t>(camera.view));
    writer.put(camera.zoom);
    writer.put(camera.left);
    writer.put(camera.top);
    writer.put(camera.follow);
    writer.put(revision);
    writer.put(std::uint8_t(0)); // Regular actor grid with an offset terrain drawing layer.

    for (const auto color : panel.frame()) {
        writer.put(color);
    }
    for (std::size_t slot = 0; slot < world.creatures().size(); ++slot) {
        const auto& creature = world.creatures()[slot];
        if (creature.alive()) {
            // PTR4 keeps 64-bit IDs and lifetime counters.
            writer.put(std::uint64_t(creature.id));
            writer.put(std::uint64_t(creature.parent));
            writer.put(std::uint64_t(creature.founder));
            writer.put(creature.generation);
            writer.put(creature.cell);
            writer.put(creature.energy);
            writer.put(creature.genes.digestion);
            writer.put(creature.genes.thrift);
            writer.put(creature.genes.moisture_preference);
            writer.put(std::uint64_t(creature.plants_eaten));
            writer.put(std::uint64_t(creature.animal_eaten));
            const auto point = panel.location(slot);
            writer.put(point.x);
            writer.put(point.y);
            writer.put(creature.genes.faction);
        }
    }

    std::uint32_t checksum = 2166136261u;
    for (const auto* byte = output.data() + observation_prefix_size; byte < writer.next; ++byte) {
        checksum = (checksum ^ *byte) * 16777619u;
    }
    writer.put(checksum);
    return writer.next - output.data();
}

std::size_t capture_observation(const RenderState& state, const std::array<CreatureDetails, capacity>& details,
                                const ObservationCounts& counts, const Panel& panel, std::uint32_t revision,
                                Observation& output) {
    unsigned population = 0;
    for (const auto& creature : state.creatures) {
        population += creature.alive();
    }
    const auto body_size = observation_header_size + panel.frame().size() * 2 + population * observation_creature_size;

    Writer writer{output.data()};
    for (const char byte : {'P', 'T', 'R', '4'}) {
        writer.put(std::uint8_t(byte));
    }
    writer.put(std::uint32_t(body_size));
    writer.put(std::uint16_t(width));
    writer.put(std::uint16_t(height));
    writer.put(std::uint16_t(population));
    writer.put(counts.ticks);
    writer.put(counts.births);
    writer.put(counts.deaths);
    writer.put(counts.kills);

    const auto& camera = panel.camera();
    writer.put(static_cast<std::uint8_t>(camera.view));
    writer.put(camera.zoom);
    writer.put(camera.left);
    writer.put(camera.top);
    writer.put(camera.follow);
    writer.put(revision);
    writer.put(std::uint8_t(0));

    for (const auto color : panel.frame()) {
        writer.put(color);
    }
    for (std::size_t slot = 0; slot < state.creatures.size(); ++slot) {
        const auto& creature = state.creatures[slot];
        if (!creature.alive()) {
            continue;
        }
        const auto& facts = details[slot].id == creature.id ? details[slot] : CreatureDetails{};
        writer.put(std::uint64_t(creature.id));
        writer.put(facts.parent);
        writer.put(facts.founder);
        writer.put(facts.generation);
        writer.put(creature.cell);
        writer.put(facts.energy);
        writer.put(facts.digestion);
        writer.put(facts.thrift);
        writer.put(facts.moisture_preference);
        writer.put(facts.plants_eaten);
        writer.put(facts.animal_eaten);
        const auto point = panel.location(slot);
        writer.put(point.x);
        writer.put(point.y);
        writer.put(creature.look.faction());
    }

    std::uint32_t checksum = 2166136261u;
    for (const auto* byte = output.data() + observation_prefix_size; byte < writer.next; ++byte) {
        checksum = (checksum ^ *byte) * 16777619u;
    }
    writer.put(checksum);
    return writer.next - output.data();
}

bool PanelInput::feed(char byte, Panel& panel, std::uint32_t now_ms) {
    if (byte == '!') {
        reading_ = true;
        length_ = 0;
        return false;
    }
    if (!reading_) {
        return byte == '?';
    }
    if (byte != '\n') {
        if (length_ == command_.size()) {
            reset();
        } else {
            command_[length_++] = byte;
        }
        return false;
    }

    std::array<std::uint64_t, 6> fields{};
    std::size_t field = 0;
    bool digits = false;
    const auto length = length_;
    reset();
    for (std::size_t i = 0; i < length; ++i) {
        const char ch = command_[i];
        if (ch == ' ' && digits && field + 1 < fields.size()) {
            ++field;
            digits = false;
        } else if (ch >= '0' && ch <= '9' &&
                   fields[field] <= (std::numeric_limits<std::uint64_t>::max() - (ch - '0')) / 10) {
            fields[field] = fields[field] * 10 + (ch - '0');
            digits = true;
        } else {
            return false;
        }
    }
    const auto [revision, view, zoom, left, top, follow] = fields;
    if (digits && field == 5 && revision <= UINT32_MAX && view <= static_cast<unsigned>(PanelView::habitat) &&
        zoom <= detail_zoom && left <= UINT16_MAX && top <= UINT16_MAX &&
        ((view == static_cast<unsigned>(PanelView::close)) == (zoom >= sprite_zoom)) &&
        (view != static_cast<unsigned>(PanelView::hunt) || (zoom == 0 && follow == 0))) {
        panel.set_camera({static_cast<PanelView>(view), static_cast<std::uint8_t>(zoom),
                          static_cast<std::uint16_t>(left), static_cast<std::uint16_t>(top), follow}, now_ms);
        revision_ = static_cast<std::uint32_t>(revision);
    }
    return false;
}

} // namespace petri
