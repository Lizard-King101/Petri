// Standalone USB-to-ESP32 serial bridge for the MatrixPortal M4.
//
// The Petri firmware offers the same bridge when UP is held at boot. This
// smaller sketch stays available for recovery. See esp32_bridge.hpp for the
// reset wiring.
//
// Tap UP to restart the ESP32 normally and watch its boot log at 115200.
// Tap DOWN to return it to the bootloader.

#include "esp32_bridge.hpp"

namespace {

constexpr uint8_t upPin = 2;
constexpr uint8_t downPin = 3;
constexpr uint32_t debounceMs = 25;

struct Button {
    uint8_t pin;
    bool pressed = false;
    uint32_t changedAt = 0;

    explicit Button(uint8_t inputPin) : pin(inputPin) {}

    bool tapped(uint32_t now) {
        const bool reading = digitalRead(pin) == LOW;
        if (reading != pressed && now - changedAt >= debounceMs) {
            pressed = reading;
            changedAt = now;
            return pressed;
        }
        return false;
    }
};

Button up(upPin);
Button down(downPin);
esp32_bridge::Bridge bridge;

} // namespace

void setup() {
    pinMode(upPin, INPUT_PULLUP);
    pinMode(downPin, INPUT_PULLUP);
    Serial.begin(115200);
    bridge.begin();
}

void loop() {
    bridge.service();
    const uint32_t now = millis();
    if (up.tapped(now)) {
        bridge.restart(false);
    }
    if (down.tapped(now)) {
        bridge.restart(true);
    }
}
