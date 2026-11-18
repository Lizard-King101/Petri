#pragma once

// SAMD51 side of the SPI link (link/link.hpp) for the MatrixPortal M4.
// Each exchange clocks both 16-byte headers, then only as many payload bytes as
// the longer frame needs, without releasing chip select.

#include <Arduino.h>
#include <SPI.h>

#include <cstring>

#include "link.hpp"

namespace petri::link {

class Master {
public:
    struct Result {
        bool exchanged = false, valid = false;
        Header header{};
    };
    struct Stats {
        std::uint32_t exchanges = 0, invalid = 0, payload_bytes = 0;
    };

    void begin() {
        pinMode(SPIWIFI_SS, OUTPUT);
        digitalWrite(SPIWIFI_SS, HIGH);
        pinMode(SPIWIFI_ACK, INPUT);
        SPIWIFI.begin();
    }

    // The ESP32 has a transaction loaded.
    bool ready() const { return digitalRead(SPIWIFI_ACK) == LOW; }

    // Sends one frame and receives one. Call only when ready().
    Result exchange(Type type, const std::uint8_t* data, std::size_t size) {
        static_assert(spi_mode == 0, "The master below is set for SPI mode 0");
        Header out{type, sequence_, acknowledged_, static_cast<std::uint16_t>(size), 0};
        if (size) {
            std::memcpy(payload(tx_), data, size);
        }
        seal(tx_, out);
        SPIWIFI.beginTransaction(SPISettings(spi_clock_hz, MSBFIRST, SPI_MODE0));
        digitalWrite(SPIWIFI_SS, LOW);
        SPIWIFI.transfer(tx_.data(), rx_.data(), header_bytes, true);
        const auto rest = transfer_bytes(size, peek_length(rx_));
        if (rest) {
            SPIWIFI.transfer(tx_.data() + header_bytes, rx_.data() + header_bytes, rest, true);
        }
        digitalWrite(SPIWIFI_SS, HIGH);
        SPIWIFI.endTransaction();
        ++sequence_;
        ++stats_.exchanges;
        stats_.payload_bytes += rest;

        Result result;
        result.exchanged = true;
        result.valid = open(rx_, result.header) == Decode::ok;
        if (result.valid) {
            acknowledged_ = result.header.sequence;
        } else {
            ++stats_.invalid;
        }
        return result;
    }

    const std::uint8_t* received_payload() const { return payload(rx_); }
    const Stats& stats() const { return stats_; }

private:
    Frame tx_{}, rx_{};
    std::uint16_t sequence_ = 0, acknowledged_ = 0;
    Stats stats_{};
};

} // namespace petri::link
