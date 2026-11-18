#include "link.hpp"

namespace petri::link {
namespace {

constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t index = 0; index < 256; ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = value & 1 ? 0xEDB88320u ^ (value >> 1) : value >> 1;
        }
        table[index] = value;
    }
    return table;
}

constexpr auto crc_table = make_table();

void put16(std::uint8_t* at, std::uint16_t value) {
    at[0] = static_cast<std::uint8_t>(value);
    at[1] = static_cast<std::uint8_t>(value >> 8);
}

std::uint16_t get16(const std::uint8_t* at) {
    return static_cast<std::uint16_t>(at[0] | at[1] << 8);
}

std::uint32_t frame_crc(const Frame& frame, std::uint16_t length) {
    const auto header = crc32(frame.data(), 12);
    return crc32(frame.data() + header_bytes, length, header);
}

std::uint32_t pattern_state(std::uint16_t sequence, std::uint32_t salt) {
    const std::uint32_t state = (sequence + 1u) * 2654435761u ^ salt;
    return state ? state : 1;
}

} // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc) {
    crc = ~crc;
    for (std::size_t index = 0; index < size; ++index) {
        crc = crc_table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

void seal(Frame& frame, const Header& header) {
    auto* bytes = frame.data();
    bytes[0] = 'P';
    bytes[1] = 'L';
    bytes[2] = version;
    bytes[3] = static_cast<std::uint8_t>(header.type);
    put16(bytes + 4, header.sequence);
    put16(bytes + 6, header.acknowledged);
    put16(bytes + 8, header.length);
    put16(bytes + 10, header.flags);
    const auto crc = frame_crc(frame, header.length);
    for (int byte = 0; byte < 4; ++byte) {
        bytes[12 + byte] = static_cast<std::uint8_t>(crc >> (8 * byte));
    }
}

Decode open(const Frame& frame, Header& header) {
    const auto* bytes = frame.data();
    if (bytes[0] != 'P' || bytes[1] != 'L') {
        return Decode::bad_magic;
    }
    if (bytes[2] != version) {
        return Decode::bad_version;
    }
    const auto length = get16(bytes + 8);
    if (length > payload_max) {
        return Decode::bad_length;
    }
    const std::uint32_t stored = bytes[12] | bytes[13] << 8 | bytes[14] << 16 | std::uint32_t(bytes[15]) << 24;
    if (stored != frame_crc(frame, length)) {
        return Decode::bad_crc;
    }
    header.type = static_cast<Type>(bytes[3]);
    header.sequence = get16(bytes + 4);
    header.acknowledged = get16(bytes + 6);
    header.length = length;
    header.flags = get16(bytes + 10);
    return Decode::ok;
}

void fill_pattern(std::uint8_t* data, std::size_t size, std::uint16_t sequence, std::uint32_t salt) {
    auto state = pattern_state(sequence, salt);
    for (std::size_t index = 0; index < size; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        data[index] = static_cast<std::uint8_t>(state);
    }
}

bool check_pattern(const std::uint8_t* data, std::size_t size, std::uint16_t sequence, std::uint32_t salt) {
    auto state = pattern_state(sequence, salt);
    for (std::size_t index = 0; index < size; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        if (data[index] != static_cast<std::uint8_t>(state)) {
            return false;
        }
    }
    return true;
}

} // namespace petri::link
