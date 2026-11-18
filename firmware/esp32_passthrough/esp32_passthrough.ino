// USB-to-ESP32 serial bridge for the MatrixPortal M4.
//
// At power-on, and each time a host program opens the USB port while the
// bridge is in bootloader mode, the ESP32 is reset with GPIO0 held low. That
// starts its ROM bootloader at 115200 baud. Run esptool with
// `--before no-reset --after no-reset` so the bridge does not need the other
// modem-control lines. The bridge follows the baud rate that the host sets
// on the USB port.
//
// NeoPixel: yellow = ESP32 bootloader, green = ESP32 running its firmware.
// Tap UP to restart the ESP32 normally and watch its boot log at 115200.
// Tap DOWN to return it to the bootloader.

#include <Adafruit_NeoPixel.h>
#include <Adafruit_TinyUSB.h>

namespace {

constexpr uint8_t upPin = 2;
constexpr uint8_t downPin = 3;
constexpr uint32_t debounceMs = 25;

Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
uint32_t baud = 115200;
bool bootloaderMode = true;
bool hostOpen = false;

void restartEsp32(bool bootloader) {
    pinMode(ESP32_GPIO0, OUTPUT);
    digitalWrite(ESP32_GPIO0, bootloader ? LOW : HIGH);
    digitalWrite(ESP32_RESETN, LOW);
    delay(100);
    digitalWrite(ESP32_RESETN, HIGH);
    delay(100);
    if (!bootloader) {
        // Release GPIO0 so the ESP32 firmware can use it.
        pinMode(ESP32_GPIO0, INPUT);
    }
    bootloaderMode = bootloader;
    pixel.setPixelColor(0, bootloader ? pixel.Color(24, 18, 0) : pixel.Color(0, 24, 0));
    pixel.show();
}

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

} // namespace

void setup() {
    pinMode(upPin, INPUT_PULLUP);
    pinMode(downPin, INPUT_PULLUP);
    pinMode(ESP32_RESETN, OUTPUT);
    pixel.begin();
    Serial.begin(baud);
    SerialESP32.begin(baud);
    restartEsp32(true);
}

void loop() {
    // A new host session in bootloader mode gets a fresh ROM bootloader,
    // even if an earlier session left a flasher stub at another baud rate.
    const bool open = Serial.dtr();
    if (open && !hostOpen && bootloaderMode) {
        restartEsp32(true);
    }
    hostOpen = open;
    uint8_t buffer[64];
    while (Serial.available()) {
        const auto count = Serial.read(buffer, min(Serial.available(), int(sizeof(buffer))));
        SerialESP32.write(buffer, count);
    }
    while (SerialESP32.available()) {
        const auto count = SerialESP32.readBytes(buffer, min(SerialESP32.available(), int(sizeof(buffer))));
        Serial.write(buffer, count);
    }
    if (Serial.baud() != baud) {
        baud = Serial.baud();
        SerialESP32.flush();
        SerialESP32.begin(baud);
    }
    const uint32_t now = millis();
    if (up.tapped(now)) {
        restartEsp32(false);
    }
    if (down.tapped(now)) {
        restartEsp32(true);
    }
}
