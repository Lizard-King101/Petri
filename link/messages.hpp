#pragma once

// Small messages carried in link frames of type `command` and `telemetry`.
// All integers are little-endian.

#include <cstddef>
#include <cstdint>

namespace petri::link {

enum class Command : std::uint8_t {
    set_speed = 1,       // value: ticks per second; 0 pauses the world.
    save_checkpoint = 2, // value: request number, echoed in the checkpoint chunks.
    start_fresh = 3,     // Boot handshake: no checkpoint, start the world from its seed.
    restore_begin = 4,   // value: checkpoint size in bytes; restore chunks follow.
    restore_end = 5,     // value: CRC-32 of the whole checkpoint.
};

struct CommandMessage {
    Command command = Command::set_speed;
    std::uint32_t value = 0;
};

inline constexpr std::size_t command_bytes = 5;

// Chunk header for `checkpoint` and `restore` frames: offset (u32), total size (u32),
// request number (u32), then data.
inline constexpr std::size_t chunk_header_bytes = 12;
inline constexpr std::size_t chunk_data_max = 4096 - chunk_header_bytes;

struct ChunkHeader {
    std::uint32_t offset = 0, total = 0, request = 0;
};

void write(const ChunkHeader& header, std::uint8_t* out);
bool read(const std::uint8_t* data, std::size_t size, ChunkHeader& header);

// Status of the world, sent by the ESP32 about once a second.
struct Telemetry {
    std::uint64_t tick = 0, births = 0, deaths = 0, kills = 0, fingerprint = 0;
    std::uint32_t uptime_ms = 0, sim_ticks_per_second = 0, max_step_us = 0;
    std::uint32_t free_heap = 0, min_free_heap = 0;
    std::uint32_t updates = 0, stream_bytes = 0, encode_us_mean = 0, encode_us_max = 0;
    std::uint16_t population = 0, ticks_per_second = 0, capacity_blocks = 0;
    std::uint16_t events_dropped = 0;
};

inline constexpr std::size_t telemetry_bytes = 5 * 8 + 9 * 4 + 4 * 2;

// Creature details the renderer does not need but the observatory shows.
// A census message covers consecutive slots; empty slots have id 0.
// Layout: first slot (u16), count (u8), then the entries.
struct CensusEntry {
    std::uint32_t id = 0, parent = 0, generation = 0, plants_eaten = 0, animal_eaten = 0;
    std::uint16_t energy = 0;
    std::uint8_t founder = 0, digestion = 0, thrift = 0, moisture_preference = 0;
};

inline constexpr std::size_t census_entry_bytes = 5 * 4 + 2 + 4;
inline constexpr std::size_t census_header_bytes = 3;
inline constexpr std::size_t census_slots_per_message = 128;

std::size_t write_census(const CensusEntry* entries, std::uint16_t first_slot, std::uint8_t count, std::uint8_t* out);
// Calls `entry(slot, entry)` for each slot in the message.
template<class Visit>
bool read_census(const std::uint8_t* data, std::size_t size, Visit&& entry);
CensusEntry read_census_entry(const std::uint8_t* at);

std::size_t write(const CommandMessage& message, std::uint8_t* out);
bool read(const std::uint8_t* data, std::size_t size, CommandMessage& message);
std::size_t write(const Telemetry& telemetry, std::uint8_t* out);
bool read(const std::uint8_t* data, std::size_t size, Telemetry& telemetry);

template<class Visit>
bool read_census(const std::uint8_t* data, std::size_t size, Visit&& entry) {
    if (size < census_header_bytes) {
        return false;
    }
    const std::size_t first = data[0] | std::size_t(data[1]) << 8, count = data[2];
    if (size != census_header_bytes + count * census_entry_bytes) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        entry(first + i, read_census_entry(data + census_header_bytes + i * census_entry_bytes));
    }
    return true;
}

} // namespace petri::link
