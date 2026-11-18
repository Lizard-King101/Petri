// SAMD51 half of the dual-processor Petri firmware.
//
// The ESP32 owns the world. This sketch keeps a render mirror from the ESP32's
// stream, draws it on the 64x32 panel, sends speed and pause commands, and
// serves the desktop companion over USB. Holding UP at boot selects the ESP32
// bridge instead (see esp32_bridge.hpp).
//
// Buttons: tap UP to change view, tap DOWN to change speed (1, 2, 4, 8 ticks/s),
// hold DOWN to pause or resume. Over USB, '>' and '|' act like a DOWN tap and hold,
// 'W' shows or hides rain, 'S' saves a checkpoint now, 'C' toggles back-to-back saves, and 'X' corrupts
// the newest slot. 'C' and 'X' are test hooks for power-cut and recovery tests.
//
// The world is saved to QSPI every 10 minutes (firmware/checkpoint_store.hpp).
// Whenever the ESP32 starts without a world, this side restores the newest
// valid slot, or tells it to start fresh.

#include <Adafruit_Protomatter.h>
#include <Adafruit_TinyUSB.h>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include "src/checkpoint_store.hpp"
#include "src/esp32_bridge.hpp"
#include "src/link_master.hpp"
#include "src/messages.hpp"
#include "src/observation.hpp"
#include "src/panel.hpp"
#include "src/render_stream.hpp"

namespace {

// Measured with firmware/gfxbench: depth 5 refreshes at 233 Hz and pushes a
// frame in 4.3 ms, the same cost as depth 4 with twice the levels per channel.
#ifndef PETRI_COLOR_DEPTH
#define PETRI_COLOR_DEPTH 5
#endif
#ifndef PETRI_FRAME_RATE
#define PETRI_FRAME_RATE 30
#endif
constexpr uint32_t frameMicros = 1000000ul / PETRI_FRAME_RATE;
constexpr uint32_t pollMicros = 2000;
constexpr uint32_t debounceMs = 25;
constexpr uint32_t holdMs = 600;
constexpr uint32_t bridgeHoldMs = 300;
constexpr uint32_t staleMs = 1500;
constexpr std::uint16_t speeds[] = {1, 2, 4, 8};
constexpr uint32_t saveIntervalMs = 10ul * 60 * 1000;

uint8_t rgbPins[] = {7, 8, 9, 10, 11, 12};
uint8_t addressPins[] = {17, 18, 19, 20};
Adafruit_Protomatter matrix(petri::panel_width, PETRI_COLOR_DEPTH, 1, rgbPins, 4, addressPins, 14, 15, 16, true);

petri::RenderState mirror;
petri::Panel panel(mirror);
petri::stream::Decoder decoder;
petri::EventLog received;
petri::link::Master espLink;
petri::link::Telemetry telemetry;
std::array<petri::CreatureDetails, petri::capacity> details{};
petri::PanelInput panelInput;
petri::Observation observation;
std::size_t observationSize = 0, observationSent = 0;
bool observationRequested = false;

esp32_bridge::Bridge bridge;
petri::CheckpointStore store;

// Restore handshake, run whenever the ESP32 reports it has no world.
struct {
    enum class Step { idle, begin, chunks, end } step = Step::idle;
    int slot = -1;
    petri::CheckpointStore::Slot info;
    std::uint32_t offset = 0;
} restore;

struct {
    std::uint32_t request = 0, ok = 0, failed = 0, restored = 0, fresh = 0, lastSequence = 0, lastMs = 0;
    std::uint32_t startedMs = 0, lastSaveMs = 0;
    bool wanted = false, inFlight = false, continuous = false;
    int lastSlot = -1;
} saves;
std::uint8_t chunkBuffer[petri::link::payload_max];
bool bridgeMode = false;
bool ready = false;
bool haveTelemetry = false;
bool morePending = false;
bool commandPending = true;
bool paused = false;
unsigned speedIndex = 0;
uint32_t completedUpdates = 0;
uint32_t lastPoll = 0, lastFrame = 0, lastReport = 0, lastUpdateMs = 0;
uint32_t framesSinceReport = 0, updatesSinceReport = 0;
uint32_t maxDecodeUs = 0, maxDrawUs = 0;

enum class Gesture { none, tap, hold };

struct Button {
    const uint8_t pin;
    bool sampled = false, pressed = false, holdSent = false;
    uint32_t sampledAt = 0, pressedAt = 0;

    explicit Button(uint8_t inputPin) : pin(inputPin) {}

    void begin() {
        pinMode(pin, INPUT_PULLUP);
        pressed = sampled = digitalRead(pin) == LOW;
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

std::uint16_t requestedSpeed() {
    return paused ? 0 : speeds[speedIndex];
}

uint32_t tickInterval() {
    return 1000000ul / speeds[speedIndex];
}

bool bridgeRequested() {
    const uint32_t started = millis();
    while (digitalRead(2) == LOW) {
        if (millis() - started >= bridgeHoldMs) {
            return true;
        }
    }
    return false;
}

void drawBridge() {
    if (!ready) {
        return;
    }
    const bool bootloader = bridge.bootloader();
    matrix.fillScreen(0);
    matrix.setTextWrap(false);
    matrix.setTextColor(matrix.color565(96, 96, 96));
    matrix.setCursor(2, 4);
    matrix.print("ESP32 LINK");
    matrix.setTextColor(bootloader ? matrix.color565(160, 120, 0) : matrix.color565(0, 160, 0));
    matrix.setCursor(2, 18);
    matrix.print(bootloader ? "BOOTLOADER" : "RUNNING");
    matrix.show();
}

void serviceBridge(uint32_t nowMs) {
    const bool before = bridge.bootloader();
    bridge.service();
    if (up.poll(nowMs) == Gesture::tap) {
        bridge.restart(false);
    }
    if (down.poll(nowMs) == Gesture::tap) {
        bridge.restart(true);
    }
    if (bridge.bootloader() != before) {
        drawBridge();
    }
}

void speedGesture(Gesture gesture) {
    switch (gesture) {
    case Gesture::tap:
        speedIndex = (speedIndex + 1) % 4;
        commandPending = true;
        break;
    case Gesture::hold:
        paused = !paused;
        commandPending = true;
        break;
    case Gesture::none:
        break;
    }
}

void updateControls(uint32_t nowMs) {
    if (up.poll(nowMs) == Gesture::tap) {
        panel.cycle_view(nowMs);
    }
    speedGesture(down.poll(nowMs));
}

void handleFrame(const petri::link::Master::Result& result) {
    using petri::link::Type;
    const auto* data = espLink.received_payload();
    const auto length = result.header.length;
    switch (result.header.type) {
    case Type::render: {
        const uint32_t started = micros();
        decoder.apply(data, length, mirror, received);
        if (decoder.stats().complete != completedUpdates) {
            completedUpdates = decoder.stats().complete;
            panel.apply(received);
            panel.end_update(micros(), tickInterval());
            lastUpdateMs = millis();
            ++updatesSinceReport;
        }
        maxDecodeUs = max(maxDecodeUs, micros() - started);
        break;
    }
    case Type::telemetry:
        if (petri::link::read(data, length, telemetry)) {
            haveTelemetry = true;
            // Resend the speed if the ESP32 restarted or missed the command.
            commandPending |= telemetry.ticks_per_second != requestedSpeed();
        }
        break;
    case Type::census:
        petri::link::read_census(data, length, [](std::size_t slot, const petri::link::CensusEntry& e) {
            if (slot < details.size()) {
                details[slot] = {e.id, e.parent, e.founder, e.plants_eaten, e.animal_eaten, e.generation,
                                 e.energy, e.digestion, e.thrift, e.moisture_preference};
            }
        });
        break;
    default:
        break;
    }
}

petri::link::Master::Result sendCommand(petri::link::Command kind, std::uint32_t value) {
    std::uint8_t body[petri::link::command_bytes];
    petri::link::write(petri::link::CommandMessage{kind, value}, body);
    return espLink.exchange(petri::link::Type::command, body, sizeof(body));
}

// Builds and sends the next restore frame.
petri::link::Master::Result sendRestoreStep() {
    using Step = decltype(restore.step);
    switch (restore.step) {
    case Step::begin:
        restore.step = Step::chunks;
        restore.offset = 0;
        return sendCommand(petri::link::Command::restore_begin, restore.info.size);
    case Step::chunks: {
        const auto take = std::min<std::uint32_t>(petri::link::chunk_data_max, restore.info.size - restore.offset);
        petri::link::write(petri::link::ChunkHeader{restore.offset, restore.info.size, 0}, chunkBuffer);
        store.read(restore.slot, restore.offset, chunkBuffer + petri::link::chunk_header_bytes, take);
        const auto result = espLink.exchange(petri::link::Type::restore, chunkBuffer,
                                             petri::link::chunk_header_bytes + take);
        restore.offset += take;
        if (restore.offset >= restore.info.size) {
            restore.step = Step::end;
        }
        return result;
    }
    case Step::end:
        restore.step = Step::idle;
        ++saves.restored;
        return sendCommand(petri::link::Command::restore_end, restore.info.crc);
    case Step::idle:
        break;
    }
    return {};
}

void handleCheckpointChunk(const std::uint8_t* data, std::size_t length) {
    petri::link::ChunkHeader chunk;
    if (!petri::link::read(data, length, chunk) || chunk.request != saves.request || !saves.inFlight) {
        return;
    }
    const auto* bytes = data + petri::link::chunk_header_bytes;
    const auto size = static_cast<std::uint32_t>(length - petri::link::chunk_header_bytes);
    if ((chunk.offset == 0 && !store.begin_write(chunk.total)) || !store.write(chunk.offset, bytes, size)) {
        saves.inFlight = false;
        ++saves.failed;
        return;
    }
    if (store.complete()) {
        const int slot = store.finish(telemetry.tick);
        saves.inFlight = false;
        saves.lastMs = millis() - saves.startedMs;
        if (slot >= 0) {
            ++saves.ok;
            saves.lastSlot = slot;
            saves.lastSequence = store.sequence();
        } else {
            ++saves.failed;
        }
    }
}

void serviceLink(uint32_t nowUs) {
    static bool handshakeSent = false, freshPending = false;
    const bool restoring = restore.step != decltype(restore.step)::idle;
    const bool saveDue = saves.wanted && !saves.inFlight && haveTelemetry;
    if (!espLink.ready() ||
        (!restoring && !freshPending && !commandPending && !saveDue && !morePending &&
         nowUs - lastPoll < pollMicros)) {
        return;
    }
    lastPoll = nowUs;
    petri::link::Master::Result result;
    if (restoring) {
        result = sendRestoreStep();
    } else if (freshPending) {
        freshPending = false;
        result = sendCommand(petri::link::Command::start_fresh, 0);
    } else if (commandPending && haveTelemetry) {
        result = sendCommand(petri::link::Command::set_speed, requestedSpeed());
        commandPending = false;
    } else if (saveDue) {
        saves.wanted = false;
        saves.inFlight = true;
        saves.startedMs = millis();
        result = sendCommand(petri::link::Command::save_checkpoint, ++saves.request);
    } else {
        result = espLink.exchange(petri::link::Type::idle, nullptr, 0);
    }
    morePending = result.valid && (result.header.flags & petri::link::more_pending);
    if (!result.valid) {
        return;
    }
    if (result.header.flags & petri::link::awaiting_world) {
        // The ESP32 booted, or restarted on its own. Offer the newest checkpoint once.
        haveTelemetry = false;
        saves.inFlight = false;
        if (!handshakeSent) {
            handshakeSent = true;
            restore.slot = store.ready() ? store.newest(restore.info) : -1;
            if (restore.slot >= 0) {
                restore.step = decltype(restore.step)::begin;
            } else {
                ++saves.fresh;
                // Sent on the next poll: the ESP32 has not loaded its next transaction yet.
                freshPending = true;
            }
            // The ESP32 starts at 1 tick/s; send the chosen speed once its world runs.
            commandPending = true;
        }
        return;
    }
    handshakeSent = false;
    if (result.header.type == petri::link::Type::checkpoint) {
        handleCheckpointChunk(espLink.received_payload(), result.header.length);
    } else {
        handleFrame(result);
    }
}

void serviceObservatory(uint32_t nowMs) {
    if (!Serial.dtr()) {
        panelInput.reset();
        observationRequested = false;
        observationSize = 0;
        return;
    }
    for (int i = 0; i < 64 && Serial.available() > 0; ++i) {
        const char byte = static_cast<char>(Serial.read());
        if (byte == '>' || byte == '|') {
            speedGesture(byte == '>' ? Gesture::tap : Gesture::hold);
            continue;
        }
        if (byte == 'W') {
            panel.set_rain(!panel.rain());
            continue;
        }
        if (byte == 'S') {
            saves.wanted = true;
            continue;
        }
        if (byte == 'C') {
            saves.continuous = !saves.continuous;
            continue;
        }
        if (byte == 'X') {
            petri::CheckpointStore::Slot newest;
            const int slot = store.newest(newest);
            if (slot >= 0) {
                store.corrupt(slot);
            }
            continue;
        }
        observationRequested |= panelInput.feed(byte, panel, nowMs);
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

void drawWorld(uint32_t nowMs) {
    const uint32_t started = micros();
    panel.draw(nowMs, micros(), speedIndex);
    std::memcpy(matrix.getBuffer(), panel.frame().data(), sizeof(petri::Frame));
    matrix.show();
    maxDrawUs = max(maxDrawUs, micros() - started);
    if (observationRequested && observationSize == 0) {
        observationSize = petri::capture_observation(
            mirror, details, {telemetry.tick, telemetry.births, telemetry.deaths, telemetry.kills}, panel,
            panelInput.revision(), observation);
        observationSent = 0;
        observationRequested = false;
    }
}

unsigned long ramGap() {
    const auto heap = reinterpret_cast<std::uintptr_t>(sbrk(0));
    const auto stack = std::uintptr_t(__get_MSP());
    return stack > heap ? static_cast<unsigned long>(stack - heap) : 0;
}

void report(uint32_t nowUs) {
    const uint32_t elapsed = nowUs - lastReport;
    if (elapsed < 1000000) {
        return;
    }
    const auto fps = (uint64_t(framesSinceReport) * 1000000 + elapsed / 2) / elapsed;
    const auto ups = (uint64_t(updatesSinceReport) * 1000000 + elapsed / 2) / elapsed;
    framesSinceReport = updatesSinceReport = 0;
    lastReport = nowUs;
    if (observationSize != 0 || !Serial) {
        return;
    }
    // One short line per second, in turn: the USB transmit buffer holds 256 bytes.
    static unsigned which = 0;
    which = (which + 1) % 3;
    const auto send = [](const char* text, int length) {
        if (length > 0 && Serial.availableForWrite() >= length) {
            Serial.write(reinterpret_cast<const uint8_t*>(text), length);
        }
    };
    char line[200];
    const auto& stream = decoder.stats();
    int length = std::snprintf(line, sizeof(line),
        "DISPLAY tick=%lu pop=%u esp_tps=%lu target_tps=%u fps=%lu ups=%lu fingerprint=%08lx%08lx "
        "max_decode_us=%lu max_draw_us=%lu ram_gap=%lu\r\n",
        static_cast<unsigned long>(telemetry.tick), telemetry.population,
        static_cast<unsigned long>(telemetry.sim_ticks_per_second), requestedSpeed(),
        static_cast<unsigned long>(fps), static_cast<unsigned long>(ups),
        static_cast<unsigned long>(telemetry.fingerprint >> 32), static_cast<unsigned long>(telemetry.fingerprint),
        static_cast<unsigned long>(maxDecodeUs), static_cast<unsigned long>(maxDrawUs), ramGap());
    if (which == 0) {
        send(line, length);
    }
    length = std::snprintf(line, sizeof(line),
        "STREAM updates=%lu complete=%lu malformed=%lu mismatches=%lu link_invalid=%lu esp_heap=%lu "
        "esp_min_heap=%lu esp_encode_us=%lu/%lu esp_max_step_us=%lu\r\n",
        static_cast<unsigned long>(stream.updates), static_cast<unsigned long>(stream.complete),
        static_cast<unsigned long>(stream.malformed), static_cast<unsigned long>(stream.checksum_mismatches),
        static_cast<unsigned long>(espLink.stats().invalid), static_cast<unsigned long>(telemetry.free_heap),
        static_cast<unsigned long>(telemetry.min_free_heap), static_cast<unsigned long>(telemetry.encode_us_mean),
        static_cast<unsigned long>(telemetry.encode_us_max), static_cast<unsigned long>(telemetry.max_step_us));
    if (which == 1) {
        send(line, length);
    }
    length = std::snprintf(line, sizeof(line),
        "STORE ready=%d continuous=%d saves_ok=%lu saves_failed=%lu last_slot=%d last_sequence=%lu "
        "last_save_ms=%lu restores=%lu fresh_starts=%lu\r\n",
        store.ready() ? 1 : 0, saves.continuous ? 1 : 0, static_cast<unsigned long>(saves.ok), static_cast<unsigned long>(saves.failed),
        saves.lastSlot, static_cast<unsigned long>(saves.lastSequence), static_cast<unsigned long>(saves.lastMs),
        static_cast<unsigned long>(saves.restored), static_cast<unsigned long>(saves.fresh));
    if (which == 2) {
        send(line, length);
        maxDecodeUs = maxDrawUs = 0;
    }
}

} // namespace

void setup() {
    // The board holds the ESP32 in reset until this pin drives high.
    pinMode(ESP32_RESETN, OUTPUT);
    digitalWrite(ESP32_RESETN, HIGH);
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, LOW);
    pinMode(2, INPUT_PULLUP);
    bridgeMode = bridgeRequested();
    up.begin();
    down.begin();
    Serial.begin(115200);
    if (bridgeMode) {
        bridge.begin();
        ready = matrix.begin() == PROTOMATTER_OK;
        drawBridge();
        return;
    }
    store.begin();
    espLink.begin();
    decoder.on_update([](void*) { panel.begin_update(millis()); }, nullptr);
    ready = matrix.begin() == PROTOMATTER_OK;
    lastFrame = lastReport = lastPoll = micros();
}

void loop() {
    const uint32_t nowMs = millis();
    if (bridgeMode) {
        serviceBridge(nowMs);
        return;
    }
    if (!ready) {
        digitalWrite(LED_BUILTIN, (nowMs / 500) % 2 ? HIGH : LOW);
        return;
    }
    const uint32_t nowUs = micros();
    updateControls(nowMs);
    serviceLink(nowUs);
    serviceObservatory(nowMs);
    // The red D13 LED lights while no complete update has arrived for 1.5 s.
    digitalWrite(LED_BUILTIN, nowMs - lastUpdateMs > staleMs ? HIGH : LOW);
    if (nowUs - lastFrame >= frameMicros) {
        lastFrame = nowUs;
        drawWorld(nowMs);
        ++framesSinceReport;
    }
    if (saves.continuous && !saves.inFlight) {
        saves.wanted = true;
    }
    if (nowMs - saves.lastSaveMs >= saveIntervalMs) {
        saves.lastSaveMs = nowMs;
        saves.wanted = true;
    }
    report(nowUs);
}
