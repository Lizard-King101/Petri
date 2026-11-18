// SPI slave side of the SAMD51 link test (see link/link.hpp).
//
// The task on core 0 exchanges frames forever. Each outgoing payload starts
// with this side's counters, followed by a test pattern. A `restart` frame from
// the master restarts the ESP32 in the SPI mode named in its flags, which is
// kept across the restart in RTC memory. Core 1 steps the observatory world
// throughout, as the simulation will in the dual-processor firmware.

#include "linktest.hpp"

#include "link.hpp"
#include "observation.hpp"

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"

#include <cstdio>
#include <cstring>
#include <optional>

namespace {

using namespace petri::link;

// MatrixPortal M4 wiring, from Adafruit nina-fw boards/esp32/board.h.
constexpr gpio_num_t mosi_pin = GPIO_NUM_14, miso_pin = GPIO_NUM_23, sclk_pin = GPIO_NUM_18;
constexpr gpio_num_t cs_pin = GPIO_NUM_5, ready_pin = GPIO_NUM_33;
constexpr spi_host_device_t host = SPI3_HOST;
constexpr std::uint32_t mode_magic = 0x504d4f44; // "PMOD"

RTC_NOINIT_ATTR std::uint32_t saved_mode_magic;
RTC_NOINIT_ATTR std::uint32_t saved_mode;

// Ready is active low: the slave has a transaction loaded.
void IRAM_ATTR on_setup(spi_slave_transaction_t*) {
    gpio_ll_set_level(&GPIO, ready_pin, 0);
}

void IRAM_ATTR on_select(void*) {
    gpio_ll_set_level(&GPIO, ready_pin, 1);
}

struct Counters {
    std::uint32_t ok = 0, bad_crc = 0, bad_other = 0, bad_pattern = 0, gaps = 0, short_frames = 0;
    std::uint32_t uptime_ms = 0, mode = 0;
};

void put32(std::uint8_t* at, std::uint32_t value) {
    for (int byte = 0; byte < 4; ++byte) {
        at[byte] = static_cast<std::uint8_t>(value >> (8 * byte));
    }
}

std::optional<petri::World> world;
volatile std::uint32_t simulated_ticks = 0;

void simulation_task(void*) {
    world.emplace(14, petri::observatory_config());
    for (;;) {
        world->step();
        ++simulated_ticks;
        if (simulated_ticks % 64 == 0) {
            vTaskDelay(1);
        }
    }
}

constexpr std::size_t counter_bytes = sizeof(Counters);
static_assert(counter_bytes == 32);

void link_task(void*) {
    // SPI mode 0 measured error-free at 8 MHz; mode 1 failed above 6 MHz. A power-on or
    // hardware reset clears RTC memory, so the default must match the master's default.
    const std::uint32_t mode = saved_mode_magic == mode_magic && saved_mode <= 3 ? saved_mode : 0;
    saved_mode_magic = 0;

    gpio_config_t ready{};
    ready.pin_bit_mask = 1ull << ready_pin;
    ready.mode = GPIO_MODE_OUTPUT;
    gpio_config(&ready);
    gpio_set_level(ready_pin, 1);

    spi_bus_config_t bus{};
    bus.mosi_io_num = mosi_pin;
    bus.miso_io_num = miso_pin;
    bus.sclk_io_num = sclk_pin;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = frame_bytes;
    spi_slave_interface_config_t slave{};
    slave.mode = mode;
    slave.spics_io_num = cs_pin;
    slave.queue_size = 1;
    slave.post_setup_cb = on_setup;
    gpio_set_pull_mode(mosi_pin, GPIO_FLOATING);
    gpio_set_pull_mode(sclk_pin, GPIO_PULLDOWN_ONLY);
    gpio_set_pull_mode(cs_pin, GPIO_PULLUP_ONLY);
    ESP_ERROR_CHECK(spi_slave_initialize(host, &bus, &slave, SPI_DMA_CH_AUTO));
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_set_intr_type(cs_pin, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(cs_pin, on_select, nullptr);

    auto* tx = static_cast<Frame*>(heap_caps_malloc(sizeof(Frame), MALLOC_CAP_DMA));
    auto* rx = static_cast<Frame*>(heap_caps_malloc(sizeof(Frame), MALLOC_CAP_DMA));
    std::printf("LINKTEST slave ready mode=%u frame_bytes=%u\n", unsigned(mode), unsigned(frame_bytes));

    Counters counters;
    counters.mode = mode;
    std::uint16_t sequence = 0, acknowledged = 0, expected = 0;
    bool have_previous = false;
    std::int64_t last_report = esp_timer_get_time();
    std::uint32_t ticks_at_report = simulated_ticks;

    for (;;) {
        counters.uptime_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
        auto* out = payload(*tx);
        const std::uint32_t values[] = {counters.ok, counters.bad_crc, counters.bad_other, counters.bad_pattern,
                                        counters.gaps, counters.short_frames, counters.uptime_ms, counters.mode};
        for (std::size_t i = 0; i < 8; ++i) {
            put32(out + 4 * i, values[i]);
        }
        fill_pattern(out + counter_bytes, payload_max - counter_bytes, sequence, slave_salt);
        seal(*tx, {Type::test, sequence, acknowledged, static_cast<std::uint16_t>(payload_max), 0});
        std::memset(rx->data(), 0, header_bytes);

        spi_slave_transaction_t transaction{};
        transaction.length = frame_bytes * 8;
        transaction.tx_buffer = tx->data();
        transaction.rx_buffer = rx->data();
        if (spi_slave_transmit(host, &transaction, portMAX_DELAY) != ESP_OK) {
            continue;
        }
        ++sequence;

        if (transaction.trans_len != frame_bytes * 8) {
            ++counters.short_frames;
            continue;
        }
        Header header;
        const auto result = open(*rx, header);
        if (result == Decode::bad_crc) {
            ++counters.bad_crc;
            continue;
        }
        if (result != Decode::ok) {
            ++counters.bad_other;
            continue;
        }
        if (header.type == Type::restart) {
            saved_mode_magic = mode_magic;
            saved_mode = header.flags & 3;
            std::printf("LINKTEST restart requested mode=%u\n", unsigned(saved_mode));
            std::fflush(stdout);
            vTaskDelay(pdMS_TO_TICKS(20));
            esp_restart();
        }
        if (have_previous && header.sequence != expected) {
            ++counters.gaps;
        }
        have_previous = true;
        expected = static_cast<std::uint16_t>(header.sequence + 1);
        acknowledged = header.sequence;
        if (header.type == Type::test &&
            !check_pattern(payload(*rx), header.length, header.sequence, master_salt)) {
            ++counters.bad_pattern;
            continue;
        }
        ++counters.ok;

        const auto now = esp_timer_get_time();
        if (now - last_report >= 5000000) {
            const auto ticks = simulated_ticks;
            const auto sim_tps = (ticks - ticks_at_report) * 1000000ull / std::uint64_t(now - last_report);
            ticks_at_report = ticks;
            last_report = now;
            std::printf("LINKTEST slave sim_tps=%lu ok=%lu bad_crc=%lu bad_other=%lu bad_pattern=%lu gaps=%lu short=%lu\n",
                        static_cast<unsigned long>(sim_tps),
                        static_cast<unsigned long>(counters.ok), static_cast<unsigned long>(counters.bad_crc),
                        static_cast<unsigned long>(counters.bad_other),
                        static_cast<unsigned long>(counters.bad_pattern), static_cast<unsigned long>(counters.gaps),
                        static_cast<unsigned long>(counters.short_frames));
        }
    }
}

} // namespace

void start_linktest() {
    std::printf("LINKTEST boot reset_reason=%d\n", int(esp_reset_reason()));
    // Core 0 owns the link in the dual-processor plan.
    xTaskCreatePinnedToCore(link_task, "link", 8192, nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(simulation_task, "petri", 8192, nullptr, 5, nullptr, 1);
}
