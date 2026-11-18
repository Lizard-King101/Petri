#pragma once

// Render-state stream from the simulation chip to the rendering chip.
//
// One update describes the world after one or more steps. It is a sequence
// of records split across payloads of at most `payload_bytes` each. A record
// never spans two payloads, so a lost payload loses only its own records.
//
// Record: type (u8), body length (u16), body. All integers are little-endian.
//   update     tick (u64), update number (u16), flags (u8: bit 0 connected biomes)
//   creatures  first slot (u16), slot count (u16), creature count (u8), then per living creature
//              in slots [first, first + slot count), in slot order: slot (u16), id (u32),
//              cell (u16), look (u32). Slots in the range that are not listed are empty.
//              Each update's creature records cover every slot once, in order, so the
//              list is complete per update. It may take several records and payloads; a
//              lost record leaves only its slot range as of an earlier update.
//   events     count (u8), then per event: kind (u8), flag (u8), two actors of id (u32), cell (u16), look (u32)
//   cells      first cell (u16), then 3 bytes per consecutive cell
//   climate    flags (u8: bit 0 active, bit 1 storm fronts), seasonal (i8), one i8 per
//              rainfall node, then per storm slot: x, y (i32), vx, vy (i16), radius, strength (u8)
//   end        update number (u16), FNV-1a of all render cells the encoder believes were sent (u32)
//
// The encoder keeps a shadow of the mirror it has sent. Cells that differ from
// the world are sent as runs. Each update also resends a rolling window of
// cells, which repairs a mirror that missed payloads. Cells that do not fit the
// payload budget stay pending until a later update.

#include "render_state.hpp"

#include <cstddef>
#include <cstdint>

namespace petri::stream {

inline constexpr std::size_t payload_bytes = 4096;
inline constexpr std::size_t refresh_cells = 128;

enum class Record : std::uint8_t { update = 1, creatures = 2, events = 3, cells = 4, end = 5, climate = 6 };

struct EncodeStats {
    std::size_t payloads = 0, bytes = 0, cells_sent = 0, cells_pending = 0, events_sent = 0;
};

class Encoder {
public:
    using Sink = void (*)(void* context, const std::uint8_t* payload, std::size_t size);

    // Encodes one update into at most `max_payloads` payloads, passing each to `sink`.
    EncodeStats encode(const World& world, const EventLog& events, std::size_t max_payloads, Sink sink,
                       void* context);
    // The same from a captured state, for a relay or a test.
    EncodeStats encode(const RenderState& state, const EventLog& events, std::size_t max_payloads, Sink sink,
                       void* context);
    // Forget what was sent, so the next updates resend the whole map.
    void reset();
    const RenderState& shadow() const { return shadow_; }

private:
    template<class Source>
    EncodeStats encode_from(const Source& source, const EventLog& events, std::size_t max_payloads, Sink sink,
                            void* context);

    RenderState shadow_{};
    std::uint16_t update_ = 0;
    std::size_t refresh_cursor_ = 0;
    bool primed_ = false;
};

struct DecodeStats {
    std::uint32_t updates = 0, complete = 0, malformed = 0, checksum_mismatches = 0;
    // Updates whose end record arrived without creature records for every slot.
    std::uint32_t creature_gaps = 0;
};

class Decoder {
public:
    using Hook = void (*)(void* context);

    // Called when an update record arrives, before the mirror changes. A renderer
    // takes its pre-update snapshot here.
    void on_update(Hook hook, void* context) {
        hook_ = hook;
        context_ = context;
    }
    // Applies one payload to the mirror. Events from the current update go to `events`.
    // Returns false and stops at the first malformed record.
    bool apply(const std::uint8_t* payload, std::size_t size, RenderState& mirror, EventLog& events);
    // True after an end record matched the update being decoded and every
    // creature slot was covered.
    bool complete() const { return complete_; }
    const DecodeStats& stats() const { return stats_; }

private:
    Hook hook_ = nullptr;
    void* context_ = nullptr;
    std::uint16_t update_ = 0;
    // Creature slots covered so far in the current update.
    std::size_t creature_slots_ = 0;
    bool in_update_ = false, complete_ = false;
    DecodeStats stats_{};
};

// FNV-1a over the render cells, as carried in the end record.
std::uint32_t cells_checksum(const RenderState& state);

} // namespace petri::stream
