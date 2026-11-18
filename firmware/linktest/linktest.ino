// SPI master side of the SAMD51-ESP32 link test (see link/link.hpp).
//
// Exchanges frames with the ESP32 link test app as fast as the handshake
// allows and reports once a second over USB:
//   LINK hz=... mode=... tps=... payload_kBps=... ok=... timeouts=... ...
// ESP32 console lines are forwarded as `ESP ...`.
//
// USB commands, one per line:
//   clock <hz>   set the SPI clock (the SAMD51 rounds to 48 MHz / 2n)
//   mode <0-3>   restart the ESP32 in that SPI mode and follow it
//   reset        pulse the ESP32 reset line and time the recovery
//   restart      ask the ESP32 to restart itself and time the recovery
//   float        release the ESP32 reset line for 500 ms
//   clear        zero the counters

#include <Adafruit_TinyUSB.h>
#include <SPI.h>

#include "src/link.hpp"

namespace {

using namespace petri::link;

constexpr uint8_t csPin = SPIWIFI_SS;
constexpr uint8_t readyPin = SPIWIFI_ACK;
constexpr uint32_t readyTimeoutUs = 200000;
constexpr std::size_t counterBytes = 32;

Frame tx{}, rx{};
uint32_t clockHz = spi_clock_hz;
uint8_t spiMode = spi_mode;

struct Stats {
    uint32_t ok = 0, timeouts = 0, badCrc = 0, badOther = 0, badPattern = 0, gaps = 0, restarts = 0;
    uint64_t busyUs = 0;
    uint32_t maxUs = 0;
} stats;

uint32_t slave[8]{};
uint16_t sequence = 0, acknowledged = 0, expected = 0;
bool havePrevious = false;

struct {
    const char* kind = nullptr;
    uint32_t startedMs = 0;
    int wantMode = -1;
} recovery;

uint32_t actualHz(uint32_t requested) {
    uint32_t divider = 48000000ul / (2 * requested);
    if (divider > 0) {
        --divider;
    }
    return 48000000ul / (2 * (divider + 1));
}

uint8_t modeSetting(uint8_t mode) {
    static constexpr uint8_t modes[] = {SPI_MODE0, SPI_MODE1, SPI_MODE2, SPI_MODE3};
    return modes[mode & 3];
}

bool waitReady() {
    const uint32_t started = micros();
    while (digitalRead(readyPin) == HIGH) {
        if (micros() - started > readyTimeoutUs) {
            return false;
        }
    }
    return true;
}

uint32_t get32(const uint8_t* at) {
    return at[0] | at[1] << 8 | at[2] << 16 | uint32_t(at[3]) << 24;
}

// Returns true when the received frame was valid.
bool exchange(Type type, uint16_t flags) {
    fill_pattern(payload(tx), payload_max, sequence, master_salt);
    seal(tx, {type, sequence, acknowledged, static_cast<uint16_t>(payload_max), flags});
    const uint32_t started = micros();
    if (!waitReady()) {
        ++stats.timeouts;
        return false;
    }
    SPIWIFI.beginTransaction(SPISettings(clockHz, MSBFIRST, modeSetting(spiMode)));
    digitalWrite(csPin, LOW);
    SPIWIFI.transfer(tx.data(), rx.data(), frame_bytes, true);
    digitalWrite(csPin, HIGH);
    SPIWIFI.endTransaction();
    ++sequence;
    const uint32_t elapsed = micros() - started;

    Header header;
    const auto result = open(rx, header);
    if (result == Decode::bad_crc) {
        ++stats.badCrc;
        return false;
    }
    if (result != Decode::ok) {
        ++stats.badOther;
        return false;
    }
    if (havePrevious && header.sequence != expected) {
        // A slave that restarted begins again at sequence 0.
        ++(header.sequence == 0 ? stats.restarts : stats.gaps);
    }
    havePrevious = true;
    expected = static_cast<uint16_t>(header.sequence + 1);
    acknowledged = header.sequence;
    const auto* in = payload(rx);
    if (!check_pattern(in + counterBytes, header.length - counterBytes, header.sequence, slave_salt)) {
        ++stats.badPattern;
        return false;
    }
    for (int i = 0; i < 8; ++i) {
        slave[i] = get32(in + 4 * i);
    }
    ++stats.ok;
    stats.busyUs += elapsed;
    stats.maxUs = max(stats.maxUs, elapsed);
    return true;
}

void forwardEsp32() {
    static char line[160];
    static std::size_t length = 0;
    while (SerialESP32.available()) {
        const char c = static_cast<char>(SerialESP32.read());
        if (c == '\n' || length + 1 == sizeof(line)) {
            line[length] = 0;
            Serial.print("ESP ");
            Serial.println(line);
            length = 0;
        } else if (c != '\r') {
            line[length++] = c;
        }
    }
}

void resetEsp32() {
    pinMode(ESP32_GPIO0, INPUT);
    digitalWrite(ESP32_RESETN, LOW);
    delay(20);
    digitalWrite(ESP32_RESETN, HIGH);
}

void startRecovery(const char* kind, int wantMode = -1) {
    recovery.kind = kind;
    recovery.startedMs = millis();
    recovery.wantMode = wantMode;
}

void handleCommand(char* line) {
    if (!strncmp(line, "clock ", 6)) {
        clockHz = strtoul(line + 6, nullptr, 10);
        Serial.print("OK clock requested=");
        Serial.print(clockHz);
        Serial.print(" actual=");
        Serial.println(actualHz(clockHz));
    } else if (!strncmp(line, "mode ", 5)) {
        const uint8_t mode = atoi(line + 5) & 3;
        exchange(Type::restart, mode);
        spiMode = mode;
        startRecovery("mode", mode);
    } else if (!strcmp(line, "reset")) {
        // A hardware reset returns the ESP32 to the default mode.
        resetEsp32();
        spiMode = spi_mode;
        startRecovery("reset");
    } else if (!strcmp(line, "restart")) {
        exchange(Type::restart, spiMode);
        startRecovery("restart");
    } else if (!strcmp(line, "float")) {
        // Release the reset line for 500 ms to see whether the board pulls it low.
        pinMode(ESP32_RESETN, INPUT);
        delay(500);
        Serial.print("FLOAT reset_line_read=");
        Serial.println(digitalRead(ESP32_RESETN));
        pinMode(ESP32_RESETN, OUTPUT);
        digitalWrite(ESP32_RESETN, HIGH);
        startRecovery("float");
    } else if (!strcmp(line, "clear")) {
        stats = {};
        Serial.println("OK clear");
    }
}

void readCommands() {
    static char line[40];
    static std::size_t length = 0;
    while (Serial.available()) {
        const char c = static_cast<char>(Serial.read());
        if (c == '\n' || c == '\r') {
            if (length) {
                line[length] = 0;
                handleCommand(line);
                length = 0;
            }
        } else if (length + 1 < sizeof(line)) {
            line[length++] = c;
        }
    }
}

void report() {
    static uint32_t last = 0, lastOk = 0;
    const uint32_t now = millis();
    if (now - last < 1000) {
        return;
    }
    const uint32_t elapsed = now - last;
    const uint32_t frames = stats.ok - lastOk;
    last = now;
    lastOk = stats.ok;
    char text[400];
    snprintf(text, sizeof(text),
             "LINK hz=%lu mode=%u tps=%lu payload_kBps=%lu ok=%lu timeouts=%lu bad_crc=%lu bad_other=%lu "
             "bad_pattern=%lu gaps=%lu restarts=%lu mean_us=%lu max_us=%lu slave_ok=%lu slave_bad_crc=%lu "
             "slave_bad_other=%lu slave_bad_pattern=%lu slave_gaps=%lu slave_short=%lu slave_mode=%lu",
             actualHz(clockHz), spiMode, frames * 1000 / elapsed, frames * payload_max / elapsed, stats.ok,
             stats.timeouts, stats.badCrc, stats.badOther, stats.badPattern, stats.gaps, stats.restarts,
             stats.ok ? static_cast<unsigned long>(stats.busyUs / stats.ok) : 0ul, stats.maxUs, slave[0], slave[1],
             slave[2], slave[3], slave[4], slave[5], slave[7]);
    Serial.println(text);
}

} // namespace

void setup() {
    Serial.begin(115200);
    SerialESP32.begin(115200);
    pinMode(csPin, OUTPUT);
    digitalWrite(csPin, HIGH);
    pinMode(readyPin, INPUT);
    pinMode(ESP32_RESETN, OUTPUT);
    SPIWIFI.begin();
    // Join a running ESP32 instead of resetting it: in the dual-processor
    // firmware the ESP32 owns the world, which must survive a SAMD51 restart.
    digitalWrite(ESP32_RESETN, HIGH);
    startRecovery("boot");
}

void loop() {
    static bool joined = false, resetAtBoot = false;
    readCommands();
    forwardEsp32();
    const bool valid = exchange(Type::test, 0);
    joined |= valid;
    if (!joined && !resetAtBoot && millis() > 1000) {
        // Nothing answered: start the ESP32 from reset.
        resetAtBoot = true;
        resetEsp32();
        Serial.println("BOOT no valid frame after 1 s; reset the ESP32");
    }
    if (valid && recovery.kind && (recovery.wantMode < 0 || slave[7] == uint32_t(recovery.wantMode))) {
        Serial.print("RECOVERY kind=");
        Serial.print(recovery.kind);
        Serial.print(" ms=");
        Serial.print(millis() - recovery.startedMs);
        Serial.print(" slave_uptime_ms=");
        Serial.println(slave[6]);
        recovery.kind = nullptr;
    }
    report();
}
