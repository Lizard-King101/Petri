// Runs the portable Petri self-test (esp32/selftest) on the SAMD51 so its
// fingerprints and timings can be compared with the ESP32 and native builds.
// Open the USB serial port to start the run.

#include <Adafruit_TinyUSB.h>

#include "src/selftest.hpp"

namespace {

std::uint64_t now_us() {
    // Extend the 32-bit micros() counter across wraps.
    static std::uint32_t last = 0;
    static std::uint64_t high = 0;
    const std::uint32_t now = micros();
    if (now < last) {
        high += std::uint64_t(1) << 32;
    }
    last = now;
    return high | now;
}

} // namespace

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }
    delay(500);
    petri::selftest::run({
        now_us,
        [](const char* line) {
            Serial.println(line);
            Serial.flush();
        },
        [] { yield(); },
    });
    Serial.println("DONE core=samd51");
}

void loop() {}
