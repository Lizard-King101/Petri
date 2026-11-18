#include "link.hpp"
#include "messages.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

void check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

using namespace petri::link;

Frame test_frame(std::uint16_t sequence, std::uint16_t length) {
    Frame frame{};
    fill_pattern(payload(frame), length, sequence, master_salt);
    seal(frame, {Type::test, sequence, static_cast<std::uint16_t>(sequence - 1), length, 0x0102});
    return frame;
}

} // namespace

int main() {
    const char* digits = "123456789";
    check(crc32(reinterpret_cast<const std::uint8_t*>(digits), 9) == 0xCBF43926u, "CRC-32 check value differs");

    // Round trip at the size limits.
    for (const std::uint16_t length : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{payload_max}}) {
        const auto frame = test_frame(700, length);
        Header header;
        check(open(frame, header) == Decode::ok, "Valid frame was rejected");
        check(header.type == Type::test && header.sequence == 700 && header.acknowledged == 699 &&
                  header.length == length && header.flags == 0x0102,
              "Header fields changed in transit");
        check(check_pattern(payload(frame), length, 700, master_salt), "Pattern did not verify");
        check(length == 0 || !check_pattern(payload(frame), length, 701, master_salt),
              "Pattern verified against the wrong sequence");
    }

    // Every single-bit error in the header and payload is caught.
    const auto good = test_frame(42, payload_max);
    for (std::size_t byte = 0; byte < header_bytes + payload_max; ++byte) {
        for (int bit = 0; bit < 8; ++bit) {
            auto damaged = good;
            damaged[byte] ^= static_cast<std::uint8_t>(1 << bit);
            Header header;
            check(open(damaged, header) != Decode::ok, "Single-bit error was accepted");
        }
    }

    // Specific rejections.
    Header header;
    auto damaged = good;
    damaged[0] = 'X';
    check(open(damaged, header) == Decode::bad_magic, "Bad magic was not reported");
    damaged = good;
    damaged[2] = version + 1;
    check(open(damaged, header) == Decode::bad_version, "Bad version was not reported");
    damaged = good;
    damaged[8] = 0x01;
    damaged[9] = 0x10; // 4097 bytes
    check(open(damaged, header) == Decode::bad_length, "Oversized length was not reported");
    Frame zeros{};
    check(open(zeros, header) == Decode::bad_magic, "An all-zero frame was accepted");

    // Variable-length transfers cover both payloads in whole 32-bit words.
    check(transfer_bytes(0, 0) == 0 && transfer_bytes(1, 0) == 4 && transfer_bytes(5, 4096) == 4096 &&
              transfer_bytes(4095, 3) == 4096,
          "Transfer length is wrong");
    check(peek_length(good) == payload_max && peek_length(zeros) == 0, "Peeked length is wrong");

    // Messages round-trip.
    Telemetry sent;
    sent.tick = 123456789012ull;
    sent.fingerprint = 0xfedcba9876543210ull;
    sent.free_heap = 210000;
    sent.population = 77;
    sent.events_dropped = 3;
    std::uint8_t bytes[telemetry_bytes];
    check(write(sent, bytes) == telemetry_bytes, "Telemetry size changed");
    Telemetry back;
    check(read(bytes, sizeof(bytes), back) && back.tick == sent.tick && back.fingerprint == sent.fingerprint &&
              back.free_heap == sent.free_heap && back.population == 77 && back.events_dropped == 3,
          "Telemetry did not round-trip");
    check(!read(bytes, sizeof(bytes) - 1, back), "Short telemetry was accepted");
    CommandMessage speed{Command::restore_begin, 89738};
    std::uint8_t command[command_bytes];
    CommandMessage parsed;
    check(write(speed, command) == command_bytes && read(command, command_bytes, parsed) &&
              parsed.command == Command::restore_begin && parsed.value == 89738,
          "Command did not round-trip");
    command[0] = 99;
    check(!read(command, command_bytes, parsed), "An unknown command was accepted");

    // Chunk headers round-trip and bound their data.
    std::uint8_t chunk[chunk_header_bytes + 10];
    write(ChunkHeader{80000, 89738, 7}, chunk);
    ChunkHeader chunk_header;
    check(read(chunk, sizeof(chunk), chunk_header) && chunk_header.offset == 80000 && chunk_header.total == 89738 &&
              chunk_header.request == 7,
          "Chunk header did not round-trip");
    write(ChunkHeader{89735, 89738, 7}, chunk);
    check(!read(chunk, sizeof(chunk), chunk_header), "A chunk past the end was accepted");

    // A census message round-trips and fits a payload.
    CensusEntry census[census_slots_per_message];
    for (std::size_t i = 0; i < census_slots_per_message; ++i) {
        census[i] = {std::uint32_t(1000 + i), 7, std::uint32_t(i), 900000, 12, 150, 3, 9, 11, 4};
    }
    std::uint8_t census_bytes[payload_max];
    const auto census_size = write_census(census, 80, census_slots_per_message, census_bytes);
    check(census_size <= payload_max, "A census message does not fit a payload");
    std::size_t visited = 0;
    check(read_census(census_bytes, census_size, [&](std::size_t slot, const CensusEntry& e) {
              check(slot == 80 + visited && e.id == 1000 + visited && e.generation == visited &&
                        e.plants_eaten == 900000 && e.energy == 150 && e.founder == 3 && e.moisture_preference == 4,
                    "A census entry changed in transit");
              ++visited;
          }) && visited == census_slots_per_message,
          "Census did not round-trip");
    check(!read_census(census_bytes, census_size - 1, [](std::size_t, const CensusEntry&) {}),
          "A short census was accepted");

    // Padding past the payload is not covered, so stale bytes there are harmless.
    auto padded = test_frame(9, 100);
    std::memset(payload(padded) + 100, 0xAA, payload_max - 100);
    check(open(padded, header) == Decode::ok, "Padding affected the CRC");
}
