#include "link.hpp"

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

    // Padding past the payload is not covered, so stale bytes there are harmless.
    auto padded = test_frame(9, 100);
    std::memset(payload(padded) + 100, 0xAA, payload_max - 100);
    check(open(padded, header) == Decode::ok, "Padding affected the CRC");
}
