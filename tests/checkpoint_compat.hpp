#pragma once

// Builds older checkpoint layouts from the current one, so tests can check
// that older versions still load.

#include "world.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace petri::compat {

// Field widths of one creature slot, in order, in version 17 and in version 15.
// Version 18 appends four one-byte morphology genes to the version 17 slot.
inline constexpr std::uint8_t version17_fields[] = {4, 4, 1, 4, 2, 2, 1, 1, 1, 4, 4, 1, 1, 4, 1,
                                                    1, 1, 4, 1, 1, 1, 1, 1, 1, 1, 4, 4, 1, 1, 1};
inline constexpr std::uint8_t version15_fields[] = {8, 8, 8, 4, 2, 2, 1, 1, 1, 8, 8, 1, 1, 8, 1,
                                                    1, 1, 8, 1, 1, 1, 1, 1, 1, 1, 8, 8, 1, 1, 1};
inline constexpr std::size_t version17_slot = 59;
// Version 16's storm state follows the rainfall nodes, and version 18's two
// morphology settings follow the storm state.
inline constexpr std::size_t storm_state_at = 6 + 4 + 23 + 4 + climate_nodes;
inline constexpr std::size_t morphology_settings_at = storm_state_at + storm_state_bytes;

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

// Rewrites a version 18 checkpoint as version 17, or returns null if it uses morphology.
inline std::unique_ptr<CapacityCheckpoint> to_version17(const Checkpoint& current) {
    constexpr std::size_t slots_at = std::tuple_size<Checkpoint>::value - 8 - capacity * checkpoint_creature_bytes;
    const auto* settings = current.data() + morphology_settings_at;
    if (settings[0] || settings[1]) {
        return nullptr;
    }
    auto old = std::make_unique<CapacityCheckpoint>();
    auto* out = std::copy(current.begin(), current.begin() + morphology_settings_at, old->begin());
    out = std::copy(current.begin() + morphology_settings_at + 2, current.begin() + slots_at, out);
    (*old)[5] = 17;
    for (int slot = 0; slot < capacity; ++slot) {
        const auto* in = current.data() + slots_at + slot * checkpoint_creature_bytes;
        out = std::copy(in, in + version17_slot, out);
    }
    seal(*old);
    return old;
}

// Rewrites a version 17 checkpoint as version 16 or 15: 160 slots with 64-bit
// IDs and counters. Version 15 also drops the storm state. Returns null if a
// creature lives in a slot the old version lacks, or if version 15 is asked for
// a world with storm fronts.
template<class Old>
std::unique_ptr<Old> to_legacy(const CapacityCheckpoint& version17, std::uint8_t version) {
    constexpr std::size_t slots_at = std::tuple_size<CapacityCheckpoint>::value - 8 - capacity * version17_slot;
    const bool keep_storms = version >= 16;
    const auto* in = version17.data() + slots_at;
    if ((!keep_storms && version17[storm_state_at]) ||
        std::any_of(in + legacy_capacity * version17_slot, in + capacity * version17_slot,
                    [](std::uint8_t byte) { return byte; })) {
        return nullptr;
    }
    auto old = std::make_unique<Old>();
    auto* out = std::copy(version17.begin(), version17.begin() + storm_state_at, old->begin());
    const std::size_t resume = keep_storms ? storm_state_at : storm_state_at + storm_state_bytes;
    out = std::copy(version17.begin() + resume, version17.begin() + slots_at, out);
    (*old)[5] = version;
    for (int slot = 0; slot < legacy_capacity; ++slot) {
        for (std::size_t field = 0; field < sizeof(version17_fields); ++field) {
            // Little-endian values widen with zero bytes.
            std::copy(in, in + version17_fields[field], out);
            std::fill(out + version17_fields[field], out + version15_fields[field], std::uint8_t{0});
            in += version17_fields[field];
            out += version15_fields[field];
        }
    }
    seal(*old);
    return old;
}

inline std::unique_ptr<StormCheckpoint> to_version16(const CapacityCheckpoint& version17) {
    return to_legacy<StormCheckpoint>(version17, 16);
}

inline std::unique_ptr<ClimateCheckpoint> to_version15(const CapacityCheckpoint& version17) {
    return to_legacy<ClimateCheckpoint>(version17, 15);
}

} // namespace petri::compat
