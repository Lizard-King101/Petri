#pragma once

// USB-to-ESP32 serial bridge for the MatrixPortal M4.
//
// The host's DTR and RTS lines drive the ESP32 like the two-transistor
// auto-reset circuit on ESP32 development boards:
//   RTS on, DTR off  -> ESP32 held in reset
//   DTR on, RTS off  -> GPIO0 held low, which selects the ROM bootloader
//   both on or off   -> both released
// esptool's default reset and `idf.py flash monitor` therefore work. Opening
// the port with both lines on, as most terminals do, leaves the ESP32 alone.
// Hosts often change DTR and RTS in separate steps. Like the capacitor on a
// development board's EN pin, the bridge keeps reset asserted for a short
// time after the lines release it. GPIO0 has settled by the end of it.
//
// The ESP32 starts in its ROM bootloader when the bridge begins. The bridge
// follows the baud rate the host sets on the USB port. The NeoPixel is yellow
// after a restart into the bootloader and green after a normal restart.

#include <Adafruit_NeoPixel.h>
#include <Adafruit_TinyUSB.h>

namespace esp32_bridge {

class Bridge {
public:
    void begin() {
        pinMode(ESP32_RESETN, OUTPUT);
        digitalWrite(ESP32_RESETN, HIGH);
        pixel_.begin();
        SerialESP32.begin(baud_);
        restart(true);
    }

    // Restart the ESP32, either into its ROM bootloader or into its firmware.
    void restart(bool bootloader) {
        holdGpio0(bootloader);
        digitalWrite(ESP32_RESETN, LOW);
        delay(100);
        digitalWrite(ESP32_RESETN, HIGH);
        delay(100);
        holdGpio0(false);
        show(bootloader);
    }

    bool bootloader() const { return bootloader_; }

    // Call from every pass of loop().
    void service() {
        const uint8_t lines = tud_cdc_n_get_line_state(0);
        if (lines != lines_) {
            applyLines(lines);
        }
        if (releasePending_ && millis() - releaseRequestedAt_ >= resetReleaseDelayMs) {
            releasePending_ = false;
            digitalWrite(ESP32_RESETN, HIGH);
            // The ESP32 samples GPIO0 as it leaves reset.
            show(gpio0Low_);
        }

        uint8_t buffer[64];
        while (Serial.available()) {
            const auto count = Serial.read(buffer, min(Serial.available(), int(sizeof(buffer))));
            SerialESP32.write(buffer, count);
        }
        while (SerialESP32.available()) {
            const auto count = SerialESP32.readBytes(buffer, min(SerialESP32.available(), int(sizeof(buffer))));
            sendToHost(buffer, count);
        }
        if (Serial.baud() != baud_) {
            baud_ = Serial.baud();
            SerialESP32.flush();
            SerialESP32.begin(baud_);
        }
    }

private:
    static constexpr uint8_t dtrBit = 1, rtsBit = 2;
    static constexpr uint32_t resetReleaseDelayMs = 10;

    void applyLines(uint8_t lines) {
        const bool dtr = lines & dtrBit, rts = lines & rtsBit;
        const bool reset = rts && !dtr;
        lines_ = lines;
        gpio0Low_ = dtr && !rts;
        holdGpio0(gpio0Low_);
        if (reset) {
            digitalWrite(ESP32_RESETN, LOW);
            inReset_ = true;
            releasePending_ = false;
        } else if (inReset_) {
            inReset_ = false;
            releasePending_ = true;
            releaseRequestedAt_ = millis();
        }
    }

    // Serial.write() discards data while DTR is off, but esptool turns DTR off
    // before it syncs. Queue bytes for the host directly. A host that stops
    // reading for 50 ms loses the rest of the chunk instead of stalling the bridge.
    static void sendToHost(const uint8_t* data, std::size_t size) {
        for (const uint32_t started = millis(); size && millis() - started < 50;) {
            const auto written = tud_cdc_n_write(0, data, size);
            data += written;
            size -= written;
            tud_cdc_n_write_flush(0);
            if (size) {
                yield();
            }
        }
    }

    static void holdGpio0(bool low) {
        if (low) {
            pinMode(ESP32_GPIO0, OUTPUT);
            digitalWrite(ESP32_GPIO0, LOW);
        } else {
            // Release GPIO0 so its pull-up selects a normal boot and the firmware can use the pin.
            pinMode(ESP32_GPIO0, INPUT);
        }
    }

    void show(bool bootloader) {
        bootloader_ = bootloader;
        pixel_.setPixelColor(0, bootloader ? pixel_.Color(24, 18, 0) : pixel_.Color(0, 24, 0));
        pixel_.show();
    }

    Adafruit_NeoPixel pixel_{1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800};
    uint32_t baud_ = 115200;
    uint8_t lines_ = 0;
    uint32_t releaseRequestedAt_ = 0;
    bool gpio0Low_ = false, inReset_ = false, releasePending_ = false;
    bool bootloader_ = true;
};

} // namespace esp32_bridge
