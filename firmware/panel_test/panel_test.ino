#include <Adafruit_Protomatter.h>

constexpr uint8_t width = 64;
constexpr uint8_t rowAddressBits = 4;
constexpr uint8_t upPin = 2;
constexpr uint8_t downPin = 3;
constexpr uint32_t debounceMs = 25;

// MatrixPortal M4 HUB75 pin assignments.
uint8_t rgbPins[] = {7, 8, 9, 10, 11, 12};
uint8_t addressPins[] = {17, 18, 19, 20};
Adafruit_Protomatter matrix(width, 4, 1, rgbPins, rowAddressBits, addressPins,
                           14, 15, 16, false);

struct Button {
    uint8_t pin;
    bool sample = false;
    bool pressed = false;
    uint32_t lastChange = 0;

    explicit Button(uint8_t inputPin) : pin(inputPin) {}

    void begin() { pinMode(pin, INPUT_PULLUP); }

    bool update(uint32_t now) {
        const bool reading = digitalRead(pin) == LOW;
        if (sample != reading) {
            sample = reading;
            lastChange = now;
        }
        if (sample == pressed || now - lastChange < debounceMs) {
            return false;
        }
        pressed = sample;
        return true;
    }
};

Button up(upPin);
Button down(downPin);
bool matrixReady = false;

void drawTest() {
    const auto white = matrix.color565(96, 96, 96);
    const auto yellow = matrix.color565(96, 96, 0);
    const auto idle = matrix.color565(32, 32, 32);
    const auto cyan = matrix.color565(0, 128, 128);
    const auto magenta = matrix.color565(128, 0, 128);

    matrix.fillScreen(0);
    matrix.drawFastHLine(0, 0, 9, white);
    matrix.drawFastVLine(0, 0, 5, white);
    matrix.drawFastHLine(60, 31, 4, yellow);
    matrix.drawFastVLine(63, 26, 6, yellow);

    // Each bar crosses the split between the upper and lower HUB75 RGB lanes.
    matrix.fillRect(3, 8, 18, 16, matrix.color565(96, 0, 0));
    matrix.fillRect(23, 8, 18, 16, matrix.color565(0, 96, 0));
    matrix.fillRect(43, 8, 18, 16, matrix.color565(0, 0, 96));

    matrix.fillTriangle(26, 6, 38, 6, 32, 1, up.pressed ? cyan : idle);
    matrix.fillTriangle(26, 25, 38, 25, 32, 30, down.pressed ? magenta : idle);
    matrix.show();
}

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    up.begin();
    down.begin();
    matrixReady = matrix.begin() == PROTOMATTER_OK;
    if (matrixReady) {
        drawTest();
    }
}

void loop() {
    const auto now = millis();
    if (!matrixReady) {
        // Slow blink on the board LED signals display-driver initialization failure.
        digitalWrite(LED_BUILTIN, (now / 500) % 2 ? HIGH : LOW);
        return;
    }
    const bool upChanged = up.update(now);
    const bool downChanged = down.update(now);
    if (upChanged || downChanged) {
        drawTest();
    }
}
