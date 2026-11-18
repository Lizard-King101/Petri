#include "render_stream.hpp"

#include <algorithm>
#include <array>

namespace petri::stream {
namespace {

constexpr std::size_t record_header = 3;
constexpr std::size_t update_record = record_header + 11;
constexpr std::size_t creature_prefix = 2 + 2 + 1;
constexpr std::size_t creature_bytes = 2 + 4 + 2 + 4;
constexpr std::size_t creatures_per_record = 255;
constexpr std::size_t actor_bytes = 4 + 2 + 4;
constexpr std::size_t event_bytes = 2 + 2 * actor_bytes;
constexpr std::size_t cell_bytes = 3;
constexpr std::size_t cell_count = width * height;
constexpr std::size_t end_record = record_header + 2 + 4;

// The most bytes a full creature list can take.
constexpr std::size_t max_creature_list = (capacity + creatures_per_record - 1) / creatures_per_record *
    (record_header + creature_prefix) + capacity * creature_bytes;
// With every slot alive the list still fits the first payload beside the update
// and end records, so even a one-payload budget sends it whole.
static_assert(update_record + max_creature_list + end_record <= payload_bytes);

struct Writer {
    std::array<std::uint8_t, payload_bytes> buffer{};
    std::size_t size = 0;
    std::size_t record = 0;

    std::size_t room() const { return buffer.size() - size; }
    void put8(unsigned value) { buffer[size++] = static_cast<std::uint8_t>(value); }
    void put16(unsigned value) {
        put8(value);
        put8(value >> 8);
    }
    void put32(std::uint32_t value) {
        put16(value);
        put16(value >> 16);
    }
    void put64(std::uint64_t value) {
        put32(static_cast<std::uint32_t>(value));
        put32(static_cast<std::uint32_t>(value >> 32));
    }
    void begin(Record type) {
        record = size;
        put8(static_cast<unsigned>(type));
        put16(0);
    }
    void finish() {
        patch16(record + 1, static_cast<unsigned>(size - record - record_header));
    }
    void patch16(std::size_t at, unsigned value) {
        buffer[at] = static_cast<std::uint8_t>(value);
        buffer[at + 1] = static_cast<std::uint8_t>(value >> 8);
    }
    void actor(const RenderActor& actor) {
        put32(actor.id);
        put16(actor.cell);
        put32(actor.look.bits);
    }
    void cell(const RenderCell& cell) {
        put8(cell.terrain);
        put8(cell.plants);
        put8(cell.detail);
    }
};

struct Reader {
    const std::uint8_t* data;
    std::size_t size, at = 0;

    bool has(std::size_t count) const { return size - at >= count; }
    unsigned get8() { return data[at++]; }
    unsigned get16() {
        const unsigned low = get8();
        return low | get8() << 8;
    }
    std::uint32_t get32() {
        const std::uint32_t low = get16();
        return low | std::uint32_t(get16()) << 16;
    }
    std::uint64_t get64() {
        const std::uint64_t low = get32();
        return low | std::uint64_t(get32()) << 32;
    }
    RenderActor actor() {
        RenderActor actor;
        actor.id = get32();
        actor.cell = static_cast<std::uint16_t>(get16());
        actor.look.bits = get32();
        return actor;
    }
};

class Output {
public:
    Output(std::size_t max_payloads, Encoder::Sink sink, void* context, EncodeStats& stats)
        : max_(max_payloads), sink_(sink), context_(context), stats_(stats) {}

    // Bytes a record may use in the current payload. The last allowed payload
    // keeps space for the end record.
    std::size_t room() const {
        const std::size_t keep = stats_.payloads + 1 >= max_ ? end_record : 0;
        return writer_.room() > keep ? writer_.room() - keep : 0;
    }
    // Makes room for a record body of `bytes`, starting a new payload if needed.
    bool reserve(std::size_t bytes) {
        if (room() >= record_header + bytes) {
            return true;
        }
        if (stats_.payloads + 1 >= max_) {
            return false;
        }
        flush();
        return room() >= record_header + bytes;
    }
    // The end record always fits: room() kept it free in the last payload.
    void reserve_end() {
        if (writer_.room() < end_record) {
            flush();
        }
    }
    bool flush() {
        if (writer_.size == 0) {
            return true;
        }
        sink_(context_, writer_.buffer.data(), writer_.size);
        ++stats_.payloads;
        stats_.bytes += writer_.size;
        writer_.size = 0;
        return true;
    }
    Writer& writer() { return writer_; }

private:
    Writer writer_;
    std::size_t max_;
    Encoder::Sink sink_;
    void* context_;
    EncodeStats& stats_;
};

} // namespace

std::uint32_t cells_checksum(const RenderState& state) {
    std::uint32_t hash = 2166136261u;
    for (const auto& cell : state.cells) {
        for (const auto byte : {cell.terrain, cell.plants, cell.detail}) {
            hash = (hash ^ byte) * 16777619u;
        }
    }
    return hash;
}

void Encoder::reset() {
    primed_ = false;
    refresh_cursor_ = 0;
}

namespace {

// What the encoder reads, from a World or from a captured RenderState.
struct WorldSource {
    const World& world;
    std::uint64_t tick() const { return world.metrics().ticks; }
    bool connected_biomes() const { return world.connected_biomes(); }
    RenderCell cell(std::size_t index) const { return render_cell(world, index); }
    RenderCreature creature(std::size_t slot) const { return render_creature(world.creatures()[slot]); }
};

struct StateSource {
    const RenderState& state;
    std::uint64_t tick() const { return state.tick; }
    bool connected_biomes() const { return state.connected_biomes; }
    RenderCell cell(std::size_t index) const { return state.cells[index]; }
    RenderCreature creature(std::size_t slot) const { return state.creatures[slot]; }
};

} // namespace

EncodeStats Encoder::encode(const World& world, const EventLog& events, std::size_t max_payloads, Sink sink,
                            void* context) {
    return encode_from(WorldSource{world}, events, max_payloads, sink, context);
}

EncodeStats Encoder::encode(const RenderState& state, const EventLog& events, std::size_t max_payloads, Sink sink,
                            void* context) {
    return encode_from(StateSource{state}, events, max_payloads, sink, context);
}

template<class Source>
EncodeStats Encoder::encode_from(const Source& source, const EventLog& events, std::size_t max_payloads, Sink sink,
                                 void* context) {
    EncodeStats stats;
    if (max_payloads == 0) {
        return stats;
    }
    if (!primed_) {
        // Nothing is known about the receiver: mark every cell as different.
        for (std::size_t index = 0; index < cell_count; ++index) {
            auto cell = source.cell(index);
            cell.plants = static_cast<std::uint8_t>(~cell.plants);
            shadow_.cells[index] = cell;
        }
        primed_ = true;
    }
    Output out(max_payloads, sink, context, stats);
    auto& w = out.writer();
    ++update_;

    w.begin(Record::update);
    w.put64(source.tick());
    w.put16(update_);
    w.put8(source.connected_biomes() ? 1 : 0);
    w.finish();
    shadow_.tick = source.tick();
    shadow_.connected_biomes = source.connected_biomes();

    // The complete creature list, every update, as records that each cover a
    // range of slots. A record ends at 255 creatures or at the payload's room.
    for (std::size_t slot = 0; slot < capacity;) {
        if (!out.reserve(creature_prefix + creature_bytes)) {
            // Unreachable: the static_assert above keeps room for the whole list.
            break;
        }
        const std::size_t fit = std::min(creatures_per_record,
                                         (out.room() - record_header - creature_prefix) / creature_bytes);
        const std::size_t first = slot;
        w.begin(Record::creatures);
        w.put16(static_cast<unsigned>(first));
        const std::size_t span_at = w.size;
        w.put16(0);
        const std::size_t count_at = w.size;
        w.put8(0);
        std::size_t count = 0;
        for (; slot < capacity; ++slot) {
            const auto creature = source.creature(slot);
            if (creature.alive()) {
                if (count == fit) {
                    break;
                }
                w.put16(static_cast<unsigned>(slot));
                w.put32(creature.id);
                w.put16(creature.cell);
                w.put32(creature.look.bits);
                ++count;
            }
            shadow_.creatures[slot] = creature;
        }
        w.patch16(span_at, static_cast<unsigned>(slot - first));
        w.buffer[count_at] = static_cast<std::uint8_t>(count);
        w.finish();
    }

    // Events in order, as many records as needed.
    const auto* next_event = events.begin();
    while (next_event != events.end()) {
        if (!out.reserve(1 + event_bytes)) {
            break;
        }
        const std::size_t fit = std::min<std::size_t>({255, (out.room() - record_header - 1) / event_bytes,
                                                       std::size_t(events.end() - next_event)});
        w.begin(Record::events);
        w.put8(static_cast<unsigned>(fit));
        for (std::size_t i = 0; i < fit; ++i, ++next_event) {
            w.put8(static_cast<unsigned>(next_event->kind));
            w.put8(next_event->flag ? 1 : 0);
            w.actor(next_event->first);
            w.actor(next_event->second);
        }
        w.finish();
        stats.events_sent += fit;
    }

    // Cell runs: changed cells, merging single unchanged cells into a run.
    const auto send_run = [&](std::size_t first, std::size_t last) {
        // Sends cells [first, last), splitting at payload boundaries. Returns the end sent.
        while (first < last) {
            if (!out.reserve(2 + cell_bytes)) {
                return first;
            }
            const std::size_t fit = std::min((out.room() - record_header - 2) / cell_bytes, last - first);
            w.begin(Record::cells);
            w.put16(static_cast<unsigned>(first));
            for (std::size_t index = first; index < first + fit; ++index) {
                const auto cell = source.cell(index);
                w.cell(cell);
                shadow_.cells[index] = cell;
            }
            w.finish();
            stats.cells_sent += fit;
            first += fit;
        }
        return last;
    };
    bool budget_left = true;
    for (std::size_t index = 0; index < cell_count && budget_left;) {
        if (source.cell(index) == shadow_.cells[index]) {
            ++index;
            continue;
        }
        std::size_t end = index + 1;
        while (end < cell_count) {
            if (source.cell(end) != shadow_.cells[end]) {
                ++end;
            } else if (end + 1 < cell_count && source.cell(end + 1) != shadow_.cells[end + 1]) {
                end += 2;
            } else {
                break;
            }
        }
        budget_left = send_run(index, end) == end;
        index = end;
    }

    // Rolling refresh, if the budget allows.
    if (budget_left) {
        const std::size_t first = refresh_cursor_;
        const std::size_t last = std::min(cell_count, first + refresh_cells);
        if (send_run(first, last) == last) {
            refresh_cursor_ = last == cell_count ? 0 : last;
        }
    }

    if (!budget_left) {
        for (std::size_t index = 0; index < cell_count; ++index) {
            stats.cells_pending += source.cell(index) != shadow_.cells[index];
        }
    }

    out.reserve_end();
    w.begin(Record::end);
    w.put16(update_);
    w.put32(cells_checksum(shadow_));
    w.finish();
    out.flush();
    return stats;
}

bool Decoder::apply(const std::uint8_t* payload, std::size_t size, RenderState& mirror, EventLog& events) {
    Reader r{payload, size};
    while (r.at < size) {
        if (!r.has(record_header)) {
            ++stats_.malformed;
            return false;
        }
        const auto type = static_cast<Record>(r.get8());
        const std::size_t length = r.get16();
        if (!r.has(length)) {
            ++stats_.malformed;
            return false;
        }
        Reader body{payload + r.at, length};
        r.at += length;
        bool valid = true;
        switch (type) {
        case Record::update:
            valid = length == 11;
            if (valid) {
                if (hook_) {
                    hook_(context_);
                }
                mirror.tick = body.get64();
                update_ = static_cast<std::uint16_t>(body.get16());
                mirror.connected_biomes = body.get8() & 1;
                events.clear();
                creature_slots_ = 0;
                in_update_ = true;
                complete_ = false;
                ++stats_.updates;
            }
            break;
        case Record::creatures: {
            valid = length >= creature_prefix;
            if (!valid) {
                break;
            }
            const std::size_t first = body.get16();
            const std::size_t span = body.get16();
            const std::size_t count = body.get8();
            valid = length == creature_prefix + count * creature_bytes && first + span <= capacity &&
                    count <= span;
            // Check every entry before the mirror changes.
            const std::size_t entries = body.at;
            for (std::size_t i = 0, previous = first; i < count && valid; ++i) {
                const std::size_t slot = body.get16();
                const auto id = body.get32();
                const auto cell = body.get16();
                body.get32();
                valid = slot >= previous && slot < first + span && cell < cell_count && id != 0;
                previous = slot + 1;
            }
            if (!valid) {
                break;
            }
            body.at = entries;
            std::fill(mirror.creatures.begin() + first, mirror.creatures.begin() + first + span, RenderCreature{});
            for (std::size_t i = 0; i < count; ++i) {
                const auto slot = body.get16();
                auto& creature = mirror.creatures[slot];
                creature.id = body.get32();
                creature.cell = static_cast<std::uint16_t>(body.get16());
                creature.look.bits = body.get32();
            }
            // The encoder never repeats a slot within an update.
            creature_slots_ += span;
            break;
        }
        case Record::events: {
            const std::size_t count = length ? body.get8() : 0;
            valid = length == 1 + count * event_bytes;
            for (std::size_t i = 0; i < count && valid; ++i) {
                RenderEvent event;
                const auto kind = body.get8();
                valid = kind <= static_cast<unsigned>(RenderEvent::Kind::death);
                event.kind = static_cast<RenderEvent::Kind>(kind);
                event.flag = body.get8() & 1;
                event.first = body.actor();
                event.second = body.actor();
                valid = valid && event.first.cell < cell_count && event.second.cell < cell_count;
                if (valid) {
                    events.add(event);
                }
            }
            break;
        }
        case Record::cells: {
            valid = length >= 2 && (length - 2) % cell_bytes == 0;
            if (valid) {
                const std::size_t first = body.get16();
                const std::size_t count = (length - 2) / cell_bytes;
                valid = first + count <= cell_count;
                for (std::size_t i = 0; i < count && valid; ++i) {
                    auto& cell = mirror.cells[first + i];
                    cell.terrain = static_cast<std::uint8_t>(body.get8());
                    cell.plants = static_cast<std::uint8_t>(body.get8());
                    cell.detail = static_cast<std::uint8_t>(body.get8());
                }
            }
            break;
        }
        case Record::end:
            valid = length == 6;
            if (valid) {
                const auto number = body.get16();
                const auto checksum = body.get32();
                if (in_update_ && number == update_ && creature_slots_ != capacity) {
                    ++stats_.creature_gaps;
                } else if (in_update_ && number == update_) {
                    complete_ = true;
                    ++stats_.complete;
                    if (checksum != cells_checksum(mirror)) {
                        ++stats_.checksum_mismatches;
                    }
                }
                in_update_ = false;
            }
            break;
        default:
            valid = false;
        }
        if (!valid) {
            ++stats_.malformed;
            return false;
        }
    }
    return true;
}

} // namespace petri::stream
