#pragma once

#include "panel.hpp"

namespace petri {

// PTR4 includes the panel framebuffer, creatures, and the actor-grid layout flag.
inline constexpr std::size_t observation_prefix_size = 8;
inline constexpr std::size_t observation_header_size = 57;
inline constexpr std::size_t observation_creature_size = 54;
using Observation = std::array<std::uint8_t, observation_prefix_size + observation_header_size +
    panel_width * panel_height * 2 + capacity * observation_creature_size + 4>;

constexpr Config observatory_config() {
    Config config{24, true, 1, 0, true, true, wide_world, wide_world};
    config.faction_markers = true;
    config.fruiting_plants = true;
    config.dynamic_environment = wide_world;
    config.storm_fronts = wide_world;
    config.morphology = true;
    return config;
}

std::size_t capture_observation(const World& world, const LocalPanel& panel, std::uint32_t revision,
                                Observation& output);

// The same observation from a render mirror, for a renderer without the World.
// `details` holds the per-slot facts the mirror leaves out; a slot whose id does
// not match the mirror's creature is reported with zero details.
// Fields are 32 bits, as in World and the census; the observation widens them.
struct CreatureDetails {
    std::uint32_t id = 0, parent = 0, founder = 0, plants_eaten = 0, animal_eaten = 0;
    std::uint32_t generation = 0;
    std::uint16_t energy = 0;
    std::uint8_t digestion = 0, thrift = 0, moisture_preference = 0;
};

struct ObservationCounts {
    std::uint64_t ticks = 0, births = 0, deaths = 0, kills = 0;
};

std::size_t capture_observation(const RenderState& state, const std::array<CreatureDetails, capacity>& details,
                                const ObservationCounts& counts, const Panel& panel, std::uint32_t revision,
                                Observation& output);

class PanelInput {
public:
    // '?' requests the next drawn frame. Commands are '!revision view zoom left top follow\n'.
    bool feed(char byte, Panel& panel, std::uint32_t now_ms);
    bool feed(char byte, LocalPanel& panel, std::uint32_t now_ms) { return feed(byte, panel.panel(), now_ms); }
    void reset() { reading_ = false; length_ = 0; }
    std::uint32_t revision() const { return revision_; }

private:
    std::array<char, 72> command_{};
    std::size_t length_ = 0;
    bool reading_ = false;
    std::uint32_t revision_ = 0;
};

} // namespace petri
