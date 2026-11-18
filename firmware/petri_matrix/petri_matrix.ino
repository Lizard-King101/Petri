#include <Adafruit_Protomatter.h>
#include <Adafruit_TinyUSB.h>
#include <algorithm>
#include <cstdio>
#include <unistd.h>

#include "src/panel.hpp"
#ifdef PETRI_OBSERVATORY
#include "src/observation.hpp"
#endif
#ifdef PETRI_DEMO_CHECKPOINT
#include "src/demo_checkpoint.hpp"
#endif

namespace {

constexpr unsigned ticksPerSecondAt1x = 1;
constexpr uint32_t tickMicrosAt1x = 1000000 / ticksPerSecondAt1x;
constexpr uint32_t frameMicros = 50000;
constexpr uint32_t debounceMs = 25;
constexpr uint32_t holdMs = 600;
constexpr unsigned maxTicksPerLoop = 8;

uint8_t rgbPins[] = {7, 8, 9, 10, 11, 12};
uint8_t addressPins[] = {17, 18, 19, 20};
// Write the next frame offscreen and swap at a scan boundary.
Adafruit_Protomatter matrix(petri::panel_width, 4, 1, rgbPins, 4, addressPins, 14, 15, 16, true);
#ifdef PETRI_WIDE_WORLD
constexpr unsigned warmupTicks = 256;
#ifdef PETRI_CONNECTED_BIOMES
#ifdef PETRI_OBSERVATORY
petri::World world(14, petri::observatory_config());
#else
petri::World world(14, {24, true, 1, 0, true, true, true});
#endif
#else
petri::World world(4, {24, true, 1, 0, true, true});
#endif
#else
petri::World world(42);
#endif
petri::Panel panel(world);
#ifdef PETRI_OBSERVATORY
petri::PanelInput panelInput;
petri::Observation observation;
std::size_t observationSize = 0;
std::size_t observationSent = 0;
bool observationRequested = false;
#endif

enum class Gesture { none, tap, hold };

struct Button {
    const uint8_t pin;
    bool sampled = false;
    bool pressed = false;
    bool holdSent = false;
    uint32_t sampledAt = 0;
    uint32_t pressedAt = 0;

    explicit Button(uint8_t inputPin) : pin(inputPin) {}

    void begin() {
        pinMode(pin, INPUT_PULLUP);
        pressed = sampled = digitalRead(pin) == LOW;
        // A button held during power-on must be released before it can act.
        holdSent = pressed;
    }

    Gesture poll(uint32_t now) {
        const bool reading = digitalRead(pin) == LOW;
        if (sampled != reading) {
            sampled = reading;
            sampledAt = now;
        }
        if (sampled != pressed && now - sampledAt >= debounceMs) {
            pressed = sampled;
            if (pressed) {
                pressedAt = now;
                holdSent = false;
            } else if (!holdSent) {
                return Gesture::tap;
            }
        }
        if (pressed && sampled && !holdSent && now - pressedAt >= holdMs) {
            holdSent = true;
            return Gesture::hold;
        }
        return Gesture::none;
    }
};

Button up(2);
Button down(3);
bool ready = false;
bool paused = false;
unsigned speedIndex = 0;
uint32_t lastTick = 0;
uint32_t lastFrame = 0;
uint32_t lastReport = 0;
uint32_t framesSinceReport = 0;
uint32_t maxStepUs = 0;
uint32_t maxFrameGapUs = 0;
uint64_t ticksAtReport = 0;

} // namespace

void updateControls(uint32_t nowMs, uint32_t nowUs) {
    if (up.poll(nowMs) == Gesture::tap) {
        panel.cycle_view(nowMs);
    }

    switch (down.poll(nowMs)) {
    case Gesture::tap:
        speedIndex = (speedIndex + 1) % 4;
        lastTick = nowUs;
        break;
    case Gesture::hold:
        paused = !paused;
        lastTick = nowUs;
        break;
    case Gesture::none:
        break;
    }
}

void drawWorld(uint32_t nowMs) {
    panel.draw(nowMs, micros(), speedIndex);
    matrix.drawRGBBitmap(0, 0, panel.frame().data(), petri::panel_width, petri::panel_height);
    matrix.show();
#ifdef PETRI_OBSERVATORY
    if (observationRequested && observationSize == 0) {
        observationSize = petri::capture_observation(world, panel, panelInput.revision(), observation);
        observationSent = 0;
        observationRequested = false;
    }
#endif
}

unsigned long ramGap() {
    const auto heap = reinterpret_cast<std::uintptr_t>(sbrk(0));
    const auto stack = std::uintptr_t(__get_MSP());
    return stack > heap ? static_cast<unsigned long>(stack - heap) : 0;
}

#ifdef PETRI_OBSERVATORY
void serviceObservatory(uint32_t nowMs) {
    if (!Serial.dtr()) {
        panelInput.reset();
        observationRequested = false;
        observationSize = 0;
        return;
    }
    for (int i = 0; i < 64 && Serial.available() > 0; ++i) {
        observationRequested |= panelInput.feed(static_cast<char>(Serial.read()), panel, nowMs);
    }
    if (observationSize == 0) {
        return;
    }

    const int capacity = Serial.availableForWrite();
    if (capacity > 0) {
        const auto count = std::min<std::size_t>(capacity, observationSize - observationSent);
        observationSent += Serial.write(observation.data() + observationSent, count);
        if (observationSent >= observationSize) {
            observationSize = 0;
        }
    }
}
#endif

void reportMetrics(uint32_t nowUs) {
    const uint32_t elapsed = nowUs - lastReport;
    if (elapsed < 1000000) {
        return;
    }
    const auto& metrics = world.metrics();
    const auto ticksPerSecond = ((metrics.ticks - ticksAtReport) * 1000000 + elapsed / 2) / elapsed;
    const auto framesPerSecond = (uint64_t(framesSinceReport) * 1000000 + elapsed / 2) / elapsed;
    ticksAtReport = metrics.ticks;
    framesSinceReport = 0;
    lastReport = nowUs;

#ifdef PETRI_OBSERVATORY
    if (observationSize != 0) {
        return;
    }
#endif
    if (Serial) {
        char line[320];
        int length = std::snprintf(line, sizeof(line),
            "tick=%lu pop=%u births=%lu deaths=%lu blocked=%lu kills=%lu target_tps=%u tps=%lu fps=%lu ram_gap=%lu"
            " max_step_us=%lu max_frame_gap_us=%lu",
            static_cast<unsigned long>(metrics.ticks), metrics.population,
            static_cast<unsigned long>(metrics.births), static_cast<unsigned long>(metrics.deaths),
            static_cast<unsigned long>(metrics.capacity_blocks), static_cast<unsigned long>(metrics.kills),
            paused ? 0u : ticksPerSecondAt1x << speedIndex,
            static_cast<unsigned long>(ticksPerSecond), static_cast<unsigned long>(framesPerSecond), ramGap(),
            static_cast<unsigned long>(maxStepUs), static_cast<unsigned long>(maxFrameGapUs));
        if (length > 0 && length < int(sizeof(line))) {
            const auto available = sizeof(line) - length;
            const int appended = world.connected_biomes()
                ? std::snprintf(line + length, available,
                    " cross=%lu cross_hunts=%lu returns=%lu hunt_returns=%lu shifts=%lu\r\n",
                    static_cast<unsigned long>(metrics.biome_crossings),
                    static_cast<unsigned long>(metrics.cross_biome_hunts),
                    static_cast<unsigned long>(metrics.feeding_round_trips),
                    static_cast<unsigned long>(metrics.hunting_round_trips),
                    static_cast<unsigned long>(metrics.biome_shifts))
                : std::snprintf(line + length, available, "\r\n");
            if (appended > 0 && std::size_t(appended) < available) {
                length += appended;
                if (Serial.availableForWrite() >= length) {
                    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
                    maxStepUs = maxFrameGapUs = 0;
                }
            }
        }
    }
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    up.begin();
    down.begin();
    Serial.begin(115200);

#ifdef PETRI_WIDE_WORLD
    // Start with established groups without decoding a large checkpoint on the stack.
    for (unsigned tick = 0; tick < warmupTicks; ++tick) {
        world.step();
    }
#endif

#ifdef PETRI_DEMO_CHECKPOINT
    // Decode before the display buffers claim heap space.
    const bool demoLoaded = world.load_checkpoint(demoCheckpoint);
#endif
    ready = matrix.begin() == PROTOMATTER_OK;
#ifdef PETRI_DEMO_CHECKPOINT
    ready = ready && demoLoaded;
#endif
    if (ready) {
        drawWorld(millis());
        ticksAtReport = world.metrics().ticks;
        lastTick = lastFrame = lastReport = micros();
    }
}

void loop() {
    const uint32_t nowMs = millis();
    if (!ready) {
        digitalWrite(LED_BUILTIN, (nowMs / 500) % 2 ? HIGH : LOW);
        return;
    }

    const uint32_t nowUs = micros();
    updateControls(nowMs, nowUs);
#ifdef PETRI_OBSERVATORY
    serviceObservatory(nowMs);
#endif
    const uint32_t interval = tickMicrosAt1x >> speedIndex;
    // Limit each pass to keep inputs and rendering responsive while catching up.
    // The tick clock advances only when the world actually steps.
    for (unsigned stepped = 0;
         !paused && stepped < maxTicksPerLoop && nowUs - lastTick >= interval;
         ++stepped) {
        const uint32_t started = micros();
        world.step(panel.before_step(nowMs));
        const uint32_t finished = micros();
        maxStepUs = std::max(maxStepUs, finished - started);
        panel.after_step(finished, interval);
        lastTick += interval;
    }

    if (nowUs - lastFrame >= frameMicros) {
        maxFrameGapUs = std::max(maxFrameGapUs, nowUs - lastFrame);
        lastFrame = nowUs;
        drawWorld(nowMs);
        ++framesSinceReport;
    }
    reportMetrics(nowUs);
}
