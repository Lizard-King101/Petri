// Measures what the 64x32 panel costs the SAMD51 at each Protomatter color
// depth: refresh rate, CPU left for drawing, frame push time, and RAM.
// Open the USB serial port to start; results print as `GFX ...` lines.

#include <Adafruit_Protomatter.h>
#include <Adafruit_TinyUSB.h>
#include <cstdint>
#include <cstring>
#include <unistd.h>

namespace {

uint8_t rgbPins[] = {7, 8, 9, 10, 11, 12};
uint8_t addressPins[] = {17, 18, 19, 20};
uint16_t frame[64 * 32];

unsigned long ramGap() {
    const auto heap = reinterpret_cast<std::uintptr_t>(sbrk(0));
    const auto stack = std::uintptr_t(__get_MSP());
    return stack > heap ? static_cast<unsigned long>(stack - heap) : 0;
}

// A fixed integer workload; its duration shows how much CPU the refresh interrupt takes.
volatile uint32_t sink;
uint32_t workload_us() {
    const uint32_t started = micros();
    uint32_t state = 1;
    for (uint32_t i = 0; i < 400000; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
    }
    sink = state;
    return micros() - started;
}

void fill_test_frame(uint32_t seed) {
    for (int i = 0; i < 64 * 32; ++i) {
        seed = seed * 1664525u + 1013904223u;
        frame[i] = static_cast<uint16_t>(seed >> 16);
    }
}

} // namespace

void setup() {
    Serial.begin(115200);
    while (!Serial) {
        delay(10);
    }
    delay(300);
    const auto baseline = workload_us();
    char line[200];
    snprintf(line, sizeof(line), "GFX baseline workload_us=%lu ram_gap=%lu", static_cast<unsigned long>(baseline),
             ramGap());
    Serial.println(line);

    for (uint8_t depth = 4; depth <= 6; ++depth) {
        const auto gapBefore = ramGap();
        auto* matrix = new Adafruit_Protomatter(64, depth, 1, rgbPins, 4, addressPins, 14, 15, 16, true);
        if (matrix->begin() != PROTOMATTER_OK) {
            Serial.println("GFX begin failed");
            continue;
        }
        const auto gapAfter = ramGap();
        delay(200);
        matrix->getFrameCount();
        delay(1000);
        const auto refresh = matrix->getFrameCount();
        const auto busy = workload_us();

        // Current firmware path: GFX drawRGBBitmap, then show().
        uint32_t gfx = 0, direct = 0, showOnly = 0;
        constexpr int frames = 60;
        for (int f = 0; f < frames; ++f) {
            fill_test_frame(f);
            uint32_t t = micros();
            matrix->drawRGBBitmap(0, 0, frame, 64, 32);
            matrix->show();
            gfx += micros() - t;
            t = micros();
            std::memcpy(matrix->getBuffer(), frame, sizeof(frame));
            matrix->show();
            direct += micros() - t;
            t = micros();
            matrix->show();
            showOnly += micros() - t;
        }
        snprintf(line, sizeof(line),
                 "GFX depth=%u refresh_hz=%lu cpu_left_pct=%lu gfx_push_us=%lu direct_push_us=%lu show_us=%lu "
                 "matrix_ram=%lu",
                 depth, static_cast<unsigned long>(refresh),
                 static_cast<unsigned long>(uint64_t(baseline) * 100 / busy), static_cast<unsigned long>(gfx / frames),
                 static_cast<unsigned long>(direct / frames), static_cast<unsigned long>(showOnly / frames),
                 gapBefore > gapAfter ? gapBefore - gapAfter : 0ul);
        Serial.println(line);
        delete matrix;
        delay(200);
    }
    Serial.println("GFX done");
}

void loop() {}
