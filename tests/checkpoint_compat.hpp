#pragma once

// Builds older checkpoint layouts from the current one, so tests can check
// that older versions still load.

#include "world.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace petri::compat {

// Field widths of one creature slot, in order, in version 16 and in version 15.
inline constexpr std::uint8_t version16_fields[] = {4, 4, 1, 4, 2, 2, 1, 1, 1, 4, 4, 1, 1, 4, 1,
                                                    1, 1, 4, 1, 1, 1, 1, 1, 1, 1, 4, 4, 1, 1, 1};
inline constexpr std::uint8_t version15_fields[] = {8, 8, 8, 4, 2, 2, 1, 1, 1, 8, 8, 1, 1, 8, 1,
                                                    1, 1, 8, 1, 1, 1, 1, 1, 1, 1, 8, 8, 1, 1, 1};

template<class Bytes>
void seal(Bytes& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t index = 0; index + 8 < bytes.size(); ++index) {
        hash = (hash ^ bytes[index]) * 1099511628211ull;
    }
    for (int byte = 0; byte < 8; ++byte) {
        bytes[bytes.size() - 8 + byte] = static_cast<std::uint8_t>(hash >> (8 * byte));
    }
}

// Rewrites a version 16 checkpoint as version 15: 160 slots with 64-bit IDs and
// counters. Returns null if a creature lives in a slot that version 15 lacks.
inline std::unique_ptr<WeatherCheckpoint> to_version15(const Checkpoint& current) {
    constexpr std::size_t slots_at = std::tuple_size<Checkpoint>::value - 8 - capacity * checkpoint_creature_bytes;
    auto old = std::make_unique<WeatherCheckpoint>();
    std::copy(current.begin(), current.begin() + slots_at, old->begin());
    (*old)[5] = 15;
    const auto* in = current.data() + slots_at;
    auto* out = old->data() + slots_at;
    for (int slot = 0; slot < capacity; ++slot) {
        for (const auto width : version16_fields) {
            if (slot >= legacy_capacity && std::any_of(in, in + width, [](std::uint8_t byte) { return byte; })) {
                return nullptr;
            }
            in += width;
        }
    }
    in = current.data() + slots_at;
    for (int slot = 0; slot < legacy_capacity; ++slot) {
        for (std::size_t field = 0; field < sizeof(version16_fields); ++field) {
            // Little-endian values widen with zero bytes.
            std::copy(in, in + version16_fields[field], out);
            std::fill(out + version16_fields[field], out + version15_fields[field], std::uint8_t{0});
            in += version16_fields[field];
            out += version15_fields[field];
        }
    }
    seal(*old);
    return old;
}

} // namespace petri::compat
