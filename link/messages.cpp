#include "messages.hpp"

namespace petri::link {
namespace {

struct Out {
    std::uint8_t* at;
    template<class T>
    void put(T value) {
        for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
            *at++ = static_cast<std::uint8_t>(static_cast<std::uint64_t>(value) >> (8 * byte));
        }
    }
};

struct In {
    const std::uint8_t* at;
    template<class T>
    void get(T& value) {
        std::uint64_t result = 0;
        for (std::size_t byte = 0; byte < sizeof(T); ++byte) {
            result |= std::uint64_t(*at++) << (8 * byte);
        }
        value = static_cast<T>(result);
    }
};

template<class Stream, class T>
void fields(Stream& s, T& t) {
    s(t.tick), s(t.births), s(t.deaths), s(t.kills), s(t.fingerprint);
    s(t.uptime_ms), s(t.sim_ticks_per_second), s(t.max_step_us), s(t.free_heap), s(t.min_free_heap);
    s(t.updates), s(t.stream_bytes), s(t.encode_us_mean), s(t.encode_us_max);
    s(t.population), s(t.ticks_per_second), s(t.capacity_blocks), s(t.events_dropped);
}

} // namespace

std::size_t write_census(const CensusEntry* entries, std::uint16_t first_slot, std::uint8_t count, std::uint8_t* out) {
    Out o{out};
    o.put(first_slot);
    o.put(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& e = entries[i];
        o.put(e.id), o.put(e.parent), o.put(e.generation), o.put(e.plants_eaten), o.put(e.animal_eaten);
        o.put(e.energy), o.put(e.founder), o.put(e.digestion), o.put(e.thrift), o.put(e.moisture_preference);
    }
    return census_header_bytes + count * census_entry_bytes;
}

CensusEntry read_census_entry(const std::uint8_t* at) {
    In in{at};
    CensusEntry e;
    in.get(e.id), in.get(e.parent), in.get(e.generation), in.get(e.plants_eaten), in.get(e.animal_eaten);
    in.get(e.energy), in.get(e.founder), in.get(e.digestion), in.get(e.thrift), in.get(e.moisture_preference);
    return e;
}

std::size_t write(const CommandMessage& message, std::uint8_t* out) {
    Out o{out};
    o.put(static_cast<std::uint8_t>(message.command));
    o.put(message.value);
    return command_bytes;
}

bool read(const std::uint8_t* data, std::size_t size, CommandMessage& message) {
    if (size < command_bytes || data[0] < 1 || data[0] > static_cast<std::uint8_t>(Command::restore_end)) {
        return false;
    }
    In in{data + 1};
    message.command = static_cast<Command>(data[0]);
    in.get(message.value);
    return true;
}

void write(const ChunkHeader& header, std::uint8_t* out) {
    Out o{out};
    o.put(header.offset);
    o.put(header.total);
    o.put(header.request);
}

bool read(const std::uint8_t* data, std::size_t size, ChunkHeader& header) {
    if (size < chunk_header_bytes) {
        return false;
    }
    In in{data};
    in.get(header.offset);
    in.get(header.total);
    in.get(header.request);
    return header.offset <= header.total && size - chunk_header_bytes <= header.total - header.offset;
}

std::size_t write(const Telemetry& telemetry, std::uint8_t* out) {
    Out o{out};
    auto copy = telemetry;
    auto put = [&](auto value) { o.put(value); };
    fields(put, copy);
    return telemetry_bytes;
}

bool read(const std::uint8_t* data, std::size_t size, Telemetry& telemetry) {
    if (size < telemetry_bytes) {
        return false;
    }
    In in{data};
    auto get = [&](auto& value) { in.get(value); };
    fields(get, telemetry);
    return true;
}

} // namespace petri::link
