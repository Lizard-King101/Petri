// The ESP32 half of the dual-processor firmware.
//
// Core 1 owns the world. It steps at the speed the SAMD51 requests and, between
// steps, encodes render-stream updates into a pool of payload buffers. Core 0
// runs the SPI slave link: each transaction sends the oldest queued payload, or
// an idle frame, and receives a command, restore chunk, or idle frame.
//
// At boot the ESP32 has no world. It flags `awaiting_world` until the SAMD51
// either sends start_fresh or restores its newest checkpoint from QSPI. Saves
// are streamed to the SAMD51 at a tick boundary when it asks for one.

#include "world_app.hpp"

#include "link.hpp"
#include "messages.hpp"
#include "observation.hpp"
#include "render_stream.hpp"

#include "driver/gpio.h"
#include "driver/spi_slave.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

namespace {

using namespace petri;

// MatrixPortal M4 wiring, from Adafruit nina-fw boards/esp32/board.h.
constexpr gpio_num_t mosi_pin = GPIO_NUM_14, miso_pin = GPIO_NUM_23, sclk_pin = GPIO_NUM_18;
constexpr gpio_num_t cs_pin = GPIO_NUM_5, ready_pin = GPIO_NUM_33;
constexpr spi_host_device_t host = SPI3_HOST;

constexpr std::uint32_t seed = 14;
constexpr unsigned warmup_ticks = 256;
constexpr std::size_t pool_size = 12;
constexpr std::size_t payloads_per_update = 6;
constexpr std::int64_t min_update_us = 30000, paused_update_us = 1000000, telemetry_us = 1000000;
// How long to wait for the boot handshake before starting fresh, unless a restore is under way.
constexpr std::int64_t boot_wait_us = 5000000, boot_wait_limit_us = 30000000;
constexpr std::size_t checkpoint_size = std::tuple_size<Checkpoint>::value;
constexpr std::size_t census_messages = (capacity + link::census_slots_per_message - 1) / link::census_slots_per_message;
static_assert(payloads_per_update + 1 + census_messages <= pool_size);
static_assert(link::census_header_bytes + link::census_slots_per_message * link::census_entry_bytes <= link::payload_max);

enum Boot : int { waiting, fresh, restore };
std::atomic<int> boot_decision{waiting};
std::atomic<bool> awaiting{true}, restoring{false};
std::atomic<std::uint32_t> save_request{0};
std::uint8_t* restore_buffer = nullptr;
std::uint32_t restore_received = 0;

struct Outgoing {
    link::Type type = link::Type::idle;
    std::uint16_t size = 0;
    std::uint8_t data[link::payload_max];
};

QueueHandle_t free_queue, send_queue;
std::atomic<std::uint16_t> requested_speed{1};

// The world, the event log and the encoder's shadow of the mirror live on the
// heap, about 100 KB together. Static DRAM then keeps room for the Bluetooth
// controller, which reserves about 56 KB of that segment.
World* world = nullptr;
EventLog* events = nullptr;
stream::Encoder* encoder = nullptr;

// Allocates and constructs one T, or stops the app: nothing runs without these.
template<class T, class... Args>
T* make(const char* name, Args&&... args) {
    void* storage = heap_caps_malloc(sizeof(T), MALLOC_CAP_8BIT);
    if (!storage) {
        std::printf("WORLD out of memory: %s needs %u bytes, largest block %u\n", name, unsigned(sizeof(T)),
                    unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        std::abort();
    }
    return new (storage) T(std::forward<Args>(args)...);
}

void rebuild_world() {
    world->~World();
    new (world) World(seed, observatory_config());
}

void IRAM_ATTR on_setup(spi_slave_transaction_t*) {
    gpio_ll_set_level(&GPIO, ready_pin, 0);
}

void IRAM_ATTR on_select(void*) {
    gpio_ll_set_level(&GPIO, ready_pin, 1);
}

Outgoing* take_free() {
    Outgoing* buffer = nullptr;
    xQueueReceive(free_queue, &buffer, 0);
    return buffer;
}

void send_payload(void*, const std::uint8_t* data, std::size_t size) {
    // encode() only runs when the pool can hold a whole update.
    auto* out = take_free();
    if (!out) {
        return;
    }
    out->type = link::Type::render;
    out->size = static_cast<std::uint16_t>(size);
    std::memcpy(out->data, data, size);
    xQueueSend(send_queue, &out, portMAX_DELAY);
}

struct SimStats {
    std::uint32_t updates = 0, stream_bytes = 0, max_step_us = 0;
    std::uint64_t encode_us_total = 0;
    std::uint32_t encode_us_max = 0;
};

void send_telemetry(const SimStats& stats, std::uint32_t ticks_per_second_measured) {
    auto* out = take_free();
    if (!out) {
        return;
    }
    const auto& metrics = world->metrics();
    link::Telemetry t;
    t.tick = metrics.ticks;
    t.births = metrics.births;
    t.deaths = metrics.deaths;
    t.kills = metrics.kills;
    t.fingerprint = world->fingerprint();
    t.uptime_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    t.sim_ticks_per_second = ticks_per_second_measured;
    t.max_step_us = stats.max_step_us;
    t.free_heap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    t.min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    t.updates = stats.updates;
    t.stream_bytes = stats.stream_bytes;
    t.encode_us_mean = stats.updates ? static_cast<std::uint32_t>(stats.encode_us_total / stats.updates) : 0;
    t.encode_us_max = stats.encode_us_max;
    t.population = static_cast<std::uint16_t>(metrics.population);
    t.ticks_per_second = requested_speed.load();
    t.capacity_blocks = static_cast<std::uint16_t>(metrics.capacity_blocks);
    t.events_dropped = static_cast<std::uint16_t>(events->dropped());
    out->type = link::Type::telemetry;
    out->size = static_cast<std::uint16_t>(link::write(t, out->data));
    xQueueSend(send_queue, &out, portMAX_DELAY);
}

void send_census() {
    // Three messages cover all 320 slots; the observatory shows these details.
    static link::CensusEntry entries[link::census_slots_per_message];
    for (std::size_t first = 0; first < capacity; first += link::census_slots_per_message) {
        auto* out = take_free();
        if (!out) {
            return;
        }
        const auto count = std::min<std::size_t>(link::census_slots_per_message, capacity - first);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& c = world->creatures()[first + i];
            auto& e = entries[i];
            e = {};
            if (c.alive()) {
                e = {c.id, c.parent, c.generation, c.plants_eaten, c.animal_eaten, c.energy,
                     static_cast<std::uint8_t>(c.founder), c.genes.digestion, c.genes.thrift,
                     c.genes.moisture_preference};
            }
        }
        out->type = link::Type::census;
        out->size = static_cast<std::uint16_t>(
            link::write_census(entries, static_cast<std::uint16_t>(first), static_cast<std::uint8_t>(count), out->data));
        xQueueSend(send_queue, &out, portMAX_DELAY);
    }
}

struct SaveStream {
    std::uint32_t request = 0, offset = 0;
    Outgoing* current = nullptr;
    bool failed = false;
};

void flush_save(SaveStream& save) {
    if (!save.current) {
        return;
    }
    const auto data = save.current->size;
    link::write(link::ChunkHeader{save.offset, static_cast<std::uint32_t>(checkpoint_size), save.request},
                save.current->data);
    save.current->type = link::Type::checkpoint;
    save.current->size = static_cast<std::uint16_t>(link::chunk_header_bytes + data);
    xQueueSend(send_queue, &save.current, portMAX_DELAY);
    save.offset += data;
    save.current = nullptr;
}

void save_world(std::uint32_t request) {
    // Streams the checkpoint at this tick boundary. Waiting for buffers pauses the
    // world briefly, which keeps the snapshot consistent.
    SaveStream save;
    save.request = request;
    const auto started = esp_timer_get_time();
    world->save_checkpoint([](void* context, const std::uint8_t* data, std::size_t size) {
        auto& save = *static_cast<SaveStream*>(context);
        while (size && !save.failed) {
            if (!save.current) {
                if (xQueueReceive(free_queue, &save.current, pdMS_TO_TICKS(2000)) != pdTRUE) {
                    save.failed = true;
                    return;
                }
                save.current->size = 0;
            }
            const auto room = link::chunk_data_max - save.current->size;
            const auto take = std::min<std::size_t>(room, size);
            std::memcpy(save.current->data + link::chunk_header_bytes + save.current->size, data, take);
            save.current->size = static_cast<std::uint16_t>(save.current->size + take);
            data += take;
            size -= take;
            if (save.current->size == link::chunk_data_max) {
                flush_save(save);
            }
        }
    }, &save);
    if (!save.failed) {
        flush_save(save);
    } else if (save.current) {
        xQueueSend(free_queue, &save.current, portMAX_DELAY);
    }
    std::printf("SAVE request=%lu tick=%llu bytes=%lu failed=%d ms=%lld\n", static_cast<unsigned long>(request),
                (unsigned long long)world->metrics().ticks, static_cast<unsigned long>(save.offset), save.failed ? 1 : 0,
                (long long)((esp_timer_get_time() - started) / 1000));
}

void start_world() {
    // Boot handshake: the SAMD51 restores its newest checkpoint or starts us fresh.
    const auto waiting_since = esp_timer_get_time();
    for (;;) {
        const auto waited = esp_timer_get_time() - waiting_since;
        if (boot_decision != waiting || waited >= boot_wait_limit_us || (!restoring && waited >= boot_wait_us)) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    int expected = waiting;
    const bool timed_out = boot_decision.compare_exchange_strong(expected, fresh);
    bool restored = false;
    if (boot_decision == restore) {
        restored = world->load_checkpoint_in_place(restore_buffer, checkpoint_size);
        if (!restored) {
            // A failed in-place decode leaves the world unspecified.
            rebuild_world();
        }
    }
    if (!restored) {
        for (unsigned tick = 0; tick < warmup_ticks; ++tick) {
            world->step();
        }
    }
    // A timed-out restore may still be writing the buffer, so only free it after a decision.
    if (!timed_out) {
        heap_caps_free(restore_buffer);
        restore_buffer = nullptr;
    }
    // The encoder and the payload pool take the space the restore buffer used.
    events = make<EventLog>("event log");
    encoder = make<stream::Encoder>("encoder");
    for (std::size_t i = 0; i < pool_size; ++i) {
        auto* buffer = static_cast<Outgoing*>(heap_caps_malloc(sizeof(Outgoing), MALLOC_CAP_8BIT));
        xQueueSend(free_queue, &buffer, 0);
    }
    awaiting = false;
    std::printf("WORLD ready restored=%d timed_out=%d tick=%llu fingerprint=%016llx\n", restored ? 1 : 0,
                timed_out ? 1 : 0, (unsigned long long)world->metrics().ticks,
                (unsigned long long)world->fingerprint());
}

void sim_task(void*) {
    start_world();

    SimStats stats;
    std::int64_t next_tick = esp_timer_get_time();
    std::int64_t last_update = 0, last_telemetry = esp_timer_get_time();
    std::uint64_t ticks_at_telemetry = world->metrics().ticks;
    bool stepped = false;
    for (;;) {
        const auto now = esp_timer_get_time();
        const auto speed = requested_speed.load();
        if (speed && now >= next_tick) {
            const auto started = esp_timer_get_time();
            world->step(events->observer(*world));
            const auto elapsed = static_cast<std::uint32_t>(esp_timer_get_time() - started);
            stats.max_step_us = elapsed > stats.max_step_us ? elapsed : stats.max_step_us;
            stepped = true;
            next_tick += 1000000 / speed;
            if (now - next_tick > 1000000) {
                next_tick = now;  // Fell behind by more than a second: do not race to catch up.
            }
        } else if (!speed) {
            next_tick = now;
        }
        // Saves wait for a tick boundary and never take the place of a due step.
        if (const auto request = save_request.exchange(0)) {
            save_world(request);
        }

        const bool due = stepped ? now - last_update >= min_update_us : now - last_update >= paused_update_us;
        // Keep buffers for telemetry and the census.
        if (due && uxQueueMessagesWaiting(free_queue) >= payloads_per_update + 1 + census_messages) {
            const auto started = esp_timer_get_time();
            const auto sent = encoder->encode(*world, *events, payloads_per_update, send_payload, nullptr);
            const auto elapsed = static_cast<std::uint32_t>(esp_timer_get_time() - started);
            events->clear();
            stepped = false;
            last_update = now;
            ++stats.updates;
            stats.stream_bytes += sent.bytes;
            stats.encode_us_total += elapsed;
            stats.encode_us_max = elapsed > stats.encode_us_max ? elapsed : stats.encode_us_max;
        }

        if (now - last_telemetry >= telemetry_us) {
            const auto ticks = world->metrics().ticks;
            const auto measured = static_cast<std::uint32_t>((ticks - ticks_at_telemetry) * 1000000 /
                                                             std::uint64_t(now - last_telemetry));
            send_telemetry(stats, measured);
            send_census();
            stats = {};
            ticks_at_telemetry = ticks;
            last_telemetry = now;
        }
        // Nothing to do until the next tick or update: let the idle task run.
        vTaskDelay(1);
    }
}

void link_task(void*) {
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
    bus.max_transfer_sz = link::frame_bytes;
    spi_slave_interface_config_t slave{};
    slave.mode = link::spi_mode;
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

    auto* tx = static_cast<link::Frame*>(heap_caps_malloc(sizeof(link::Frame), MALLOC_CAP_DMA));
    auto* rx = static_cast<link::Frame*>(heap_caps_malloc(sizeof(link::Frame), MALLOC_CAP_DMA));
    std::uint16_t sequence = 0, acknowledged = 0;
    std::uint32_t bad = 0;

    for (;;) {
        Outgoing* out = nullptr;
        xQueueReceive(send_queue, &out, pdMS_TO_TICKS(5));
        link::Header header;
        header.type = out ? out->type : link::Type::idle;
        header.length = out ? out->size : 0;
        header.sequence = sequence;
        header.acknowledged = acknowledged;
        header.flags = (uxQueueMessagesWaiting(send_queue) ? link::more_pending : 0) |
                       (awaiting ? link::awaiting_world : 0);
        if (out) {
            std::memcpy(link::payload(*tx), out->data, out->size);
        }
        link::seal(*tx, header);
        std::memset(rx->data(), 0, link::header_bytes);

        spi_slave_transaction_t transaction{};
        transaction.length = link::frame_bytes * 8;
        transaction.tx_buffer = tx->data();
        transaction.rx_buffer = rx->data();
        const bool done = spi_slave_transmit(host, &transaction, portMAX_DELAY) == ESP_OK;
        ++sequence;
        // A transaction too short to have carried our frame means the master did not read it.
        const bool delivered = done && transaction.trans_len >= (link::header_bytes + header.length) * 8;
        if (out) {
            if (delivered) {
                xQueueSend(free_queue, &out, portMAX_DELAY);
            } else {
                // Send it again first. The queue has room: this buffer came from it.
                xQueueSendToFront(send_queue, &out, portMAX_DELAY);
            }
        }
        if (!done) {
            continue;
        }

        link::Header in;
        if (transaction.trans_len < link::header_bytes * 8 || link::open(*rx, in) != link::Decode::ok ||
            transaction.trans_len < (link::header_bytes + in.length) * 8) {
            ++bad;
            continue;
        }
        acknowledged = in.sequence;
        const auto* body = link::payload(*rx);
        const bool boot_open = boot_decision == waiting;
        if (in.type == link::Type::command) {
            link::CommandMessage command;
            if (!link::read(body, in.length, command)) {
                continue;
            }
            switch (command.command) {
            case link::Command::set_speed:
                requested_speed = static_cast<std::uint16_t>(command.value);
                break;
            case link::Command::save_checkpoint:
                if (!awaiting && command.value) {
                    save_request = command.value;
                }
                break;
            case link::Command::start_fresh:
                if (boot_open) {
                    boot_decision = fresh;
                }
                break;
            case link::Command::restore_begin:
                if (boot_open && restore_buffer && command.value == checkpoint_size) {
                    restore_received = 0;
                    restoring = true;
                } else if (boot_open) {
                    boot_decision = fresh;
                }
                break;
            case link::Command::restore_end:
                if (boot_open && restoring) {
                    const bool whole = restore_received == checkpoint_size &&
                                       link::crc32(restore_buffer, checkpoint_size) == command.value;
                    restoring = false;
                    boot_decision = whole ? restore : fresh;
                }
                break;
            }
        } else if (in.type == link::Type::restore && boot_open && restoring) {
            link::ChunkHeader chunk;
            const auto data = in.length - link::chunk_header_bytes;
            if (link::read(body, in.length, chunk) && chunk.offset == restore_received &&
                chunk.total == checkpoint_size) {
                std::memcpy(restore_buffer + chunk.offset, body + link::chunk_header_bytes, data);
                restore_received += data;
            } else {
                restoring = false;
                boot_decision = fresh;
            }
        }
    }
}

} // namespace

void start_world_app() {
    free_queue = xQueueCreate(pool_size, sizeof(Outgoing*));
    send_queue = xQueueCreate(pool_size, sizeof(Outgoing*));
    // The two large blocks are allocated first, while the heap is unfragmented:
    // the restore buffer, then the world it decodes into. start_world() frees the
    // restore buffer and then allocates the encoder and the payload pool.
    const auto largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    restore_buffer = static_cast<std::uint8_t*>(heap_caps_malloc(checkpoint_size, MALLOC_CAP_8BIT));
    // Constructed now so a fresh start needs no second allocation. A restore replaces it in place.
    world = make<World>("world", seed, observatory_config());
    std::printf("WORLD boot restore_buffer=%d largest_block=%u world_bytes=%u checkpoint_bytes=%u free=%u\n",
                restore_buffer ? 1 : 0, unsigned(largest), unsigned(sizeof(World)), unsigned(checkpoint_size),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    xTaskCreatePinnedToCore(link_task, "link", 6144, nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(sim_task, "petri", 8192, nullptr, 5, nullptr, 1);
}
