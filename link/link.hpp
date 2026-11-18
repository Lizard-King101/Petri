#pragma once

// SPI link between the SAMD51 (master) and the ESP32 (slave).
//
// Every SPI transaction exchanges one frame in each direction. The ESP32
// queues a frame of up to `frame_bytes` and drives READY low. The SAMD51 waits
// for READY low, selects the ESP32, and drives READY high again through the
// ESP32's chip-select interrupt. The link test clocks whole frames. The
// firmware clocks the 16-byte headers first, then only as many payload bytes as
// the longer of the two frames needs (`transfer_bytes`), without releasing
// chip select, so an idle poll costs microseconds.
//
// Frame layout, little-endian:
//   0  magic 'P' 'L'       2  version         3  type
//   4  sequence (u16)      6  acknowledged sequence from the peer (u16)
//   8  payload length (u16)                   10 flags (u16)
//   12 CRC-32 of bytes 0..11 and the payload (u32)
//   16 payload, then zero padding to frame_bytes
//
// Neither side may assume the other kept its state: a sequence that does not
// follow the previous one marks a lost frame or a restarted peer.

#include <array>
#include <cstddef>
#include <cstdint>

namespace petri::link {

inline constexpr std::uint8_t version = 1;
inline constexpr std::size_t header_bytes = 16;
inline constexpr std::size_t payload_max = 4096;
// One 64x32 RGB565 frame fits a single transaction. ESP32 DMA needs a multiple of 4.
inline constexpr std::size_t frame_bytes = header_bytes + payload_max;
static_assert(frame_bytes % 4 == 0);

enum class Type : std::uint8_t {
    idle = 0,     // No payload of interest; keeps sequences and acknowledgments moving.
    test = 1,     // Link test pattern.
    restart = 2,  // Master asks the ESP32 to restart (link test only).
    render = 3,   // ESP32 to SAMD51: one render-stream payload (render/render_stream.hpp).
    command = 4,  // SAMD51 to ESP32: link/messages.hpp.
    telemetry = 5, // ESP32 to SAMD51: link/messages.hpp.
    census = 6,    // ESP32 to SAMD51: creature details for the observatory, link/messages.hpp.
    checkpoint = 7, // ESP32 to SAMD51: a chunk of a world checkpoint, link/messages.hpp.
    restore = 8,   // SAMD51 to ESP32: a chunk of a checkpoint to load, link/messages.hpp.
};

// Header flag: the sender has more frames queued, so poll again at once.
inline constexpr std::uint16_t more_pending = 1;
// Header flag from the ESP32: it has no world yet and waits for start_fresh or a restore.
inline constexpr std::uint16_t awaiting_world = 2;

using Frame = std::array<std::uint8_t, frame_bytes>;

struct Header {
    Type type = Type::idle;
    std::uint16_t sequence = 0, acknowledged = 0, length = 0, flags = 0;
};

enum class Decode : std::uint8_t { ok, bad_magic, bad_version, bad_length, bad_crc };

std::uint32_t crc32(const std::uint8_t* data, std::size_t size, std::uint32_t crc = 0);

// Writes the header and CRC. The payload must already be at frame.data() + header_bytes.
void seal(Frame& frame, const Header& header);
Decode open(const Frame& frame, Header& header);

inline std::uint8_t* payload(Frame& frame) { return frame.data() + header_bytes; }

// Payload bytes to clock after the headers so both frames arrive whole. ESP32
// DMA receives whole 32-bit words, so the count is rounded up to a multiple of 4.
inline std::size_t transfer_bytes(std::size_t sent_length, std::size_t received_length) {
    const auto longer = sent_length > received_length ? sent_length : received_length;
    return (longer + 3) & ~std::size_t(3);
}
// Reads the payload length from a header without validating the frame.
inline std::size_t peek_length(const Frame& frame) {
    const std::size_t length = frame[8] | frame[9] << 8;
    return length <= payload_max ? length : payload_max;
}
inline const std::uint8_t* payload(const Frame& frame) { return frame.data() + header_bytes; }

// Deterministic test bytes. Each direction uses its own salt.
void fill_pattern(std::uint8_t* data, std::size_t size, std::uint16_t sequence, std::uint32_t salt);
bool check_pattern(const std::uint8_t* data, std::size_t size, std::uint16_t sequence, std::uint32_t salt);

inline constexpr std::uint32_t master_salt = 0x5341u, slave_salt = 0x4553u;

// Measured on the MatrixPortal M4, November 18, 2026: SPI mode 0 ran error-free
// at 8 MHz, and 12 MHz failed. Both sides must use these values from reset;
// the ESP32 cannot be relied on to remember a setting across a hardware reset.
inline constexpr std::uint8_t spi_mode = 0;
inline constexpr std::uint32_t spi_clock_hz = 8000000;

} // namespace petri::link
