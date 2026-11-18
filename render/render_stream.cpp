#include "render_stream.hpp"

#include <algorithm>
#include <array>

namespace petri::stream {
namespace {

constexpr std::size_t record_header = 3;
constexpr std::size_t creature_bytes = 9;
constexpr std::size_t event_bytes = 2 + 2 * 8;
constexpr std::size_t cell_bytes = 3;
constexpr std::size_t cell_count = width * height;
constexpr std::size_t end_record = record_header + 2 + 4;

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
        const auto length = size - record - record_header;
        buffer[record + 1] = static_cast<std::uint8_t>(length);
        buffer[record + 2] = static_cast<std::uint8_t>(length >> 8);
    }
    void actor(const RenderActor& actor) {
        put32(actor.id);
        put16(actor.cell);
        put16(actor.look.bits);
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
        actor.look.bits = static_cast<std::uint16_t>(get16());
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

EncodeStats Encoder::encode(const World& world, const EventLog& events, std::size_t max_payloads, Sink sink,
                            void* context) {
    EncodeStats stats;
    if (max_payloads == 0) {
        return stats;
    }
    if (!primed_) {
        // Nothing is known about the receiver: mark every cell as different.
        for (std::size_t index = 0; index < cell_count; ++index) {
            auto cell = render_cell(world, index);
            cell.plants = static_cast<std::uint8_t>(~cell.plants);
            shadow_.cells[index] = cell;
        }
        primed_ = true;
    }
    Output out(max_payloads, sink, context, stats);
    auto& w = out.writer();
    ++update_;

    w.begin(Record::update);
    w.put64(world.metrics().ticks);
    w.put16(update_);
    w.put8(world.connected_biomes() ? 1 : 0);
    w.finish();
    shadow_.tick = world.metrics().ticks;
    shadow_.connected_biomes = world.connected_biomes();

    // A complete creature list fits one record and repairs itself every update.
    std::size_t living = 0;
    for (const auto& creature : world.creatures()) {
        living += creature.alive();
    }
    out.reserve(1 + living * creature_bytes);
    w.begin(Record::creatures);
    w.put8(static_cast<unsigned>(living));
    for (std::size_t slot = 0; slot < capacity; ++slot) {
        const auto creature = render_creature(world.creatures()[slot]);
        shadow_.creatures[slot] = creature;
        if (creature.alive()) {
            w.put8(static_cast<unsigned>(slot));
            w.put32(creature.id);
            w.put16(creature.cell);
            w.put16(creature.look.bits);
        }
    }
    w.finish();

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
                const auto cell = render_cell(world, index);
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
        if (render_cell(world, index) == shadow_.cells[index]) {
            ++index;
            continue;
        }
        std::size_t end = index + 1;
        while (end < cell_count) {
            if (render_cell(world, end) != shadow_.cells[end]) {
                ++end;
            } else if (end + 1 < cell_count && render_cell(world, end + 1) != shadow_.cells[end + 1]) {
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
            stats.cells_pending += render_cell(world, index) != shadow_.cells[index];
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
                in_update_ = true;
                complete_ = false;
                ++stats_.updates;
            }
            break;
        case Record::creatures: {
            const std::size_t count = length ? body.get8() : 0;
            valid = length == 1 + count * creature_bytes;
            if (valid) {
                mirror.creatures.fill({});
                for (std::size_t i = 0; i < count && valid; ++i) {
                    const auto slot = body.get8();
                    RenderCreature creature;
                    creature.id = body.get32();
                    creature.cell = static_cast<std::uint16_t>(body.get16());
                    creature.look.bits = static_cast<std::uint16_t>(body.get16());
                    valid = slot < capacity && creature.cell < cell_count && creature.id != 0;
                    if (valid) {
                        mirror.creatures[slot] = creature;
                    }
                }
            }
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
                if (in_update_ && number == update_) {
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
