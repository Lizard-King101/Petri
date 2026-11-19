// The ESP32 half of the dual-processor firmware.
//
// Core 1 owns the world. It steps at the speed the SAMD51 requests and, between
// steps, encodes render-stream updates into a pool of payload buffers. Core 0
// runs the SPI slave link: each transaction sends the oldest queued payload, or
// an idle frame, and receives a command, restore chunk, or idle frame.
//
// At boot the ESP32 has no world. It flags `awaiting_world` until the SAMD51
// either sends start_fresh or restores its newest checkpoint from QSPI. A
// restore is decoded as its chunks arrive, with no buffer for the whole
// checkpoint: a world with brains saves up to 177 KB. Saves are streamed to
// the SAMD51 at a tick boundary when it asks for one.
//
// The world is the social brains world (docs/EMERGENT_LIFE.md): trophic
// economics with two-parent breeding and nests. Its brains'
// weights go in instruction RAM, which allows only 32-bit access and is
// otherwise unused.

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
#include "freertos/stream_buffer.h"
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
// Versions 15 to 19 restore. Version 19's size varies with the population.
constexpr bool restorable_size(std::uint32_t size) {
    return size >= std::tuple_size<ClimateCheckpoint>::value && size <= max_checkpoint_bytes;
}
// The link task hands restore chunks to the simulation task through this many bytes.
constexpr std::size_t restore_stream_bytes = 2 * link::payload_max;

Config world_config() {
    auto config = observatory_config();
    config.brains = config.trophic = true;
    config.plant_growth = 3;
    // Two parents, nests, guarding, and provisioning: the social world.
    config.sexual_reproduction = config.nests = config.guarding = config.provisioning = true;
    return config;
}
constexpr std::size_t census_messages = (capacity + link::census_slots_per_message - 1) / link::census_slots_per_message;
static_assert(payloads_per_update + 1 + census_messages <= pool_size);
static_assert(link::census_header_bytes + link::census_slots_per_message * link::census_entry_bytes <= link::payload_max);

enum Boot : int { waiting, fresh, restore };
std::atomic<int> boot_decision{waiting};
std::atomic<bool> awaiting{true}, restoring{false};
std::atomic<std::uint32_t> save_request{0};
// A restore in progress. The link task receives, checks the CRC of the whole,
// and sets `whole` or `failed`; the simulation task decodes and may set `failed`.
enum Restore : int { none, receiving, whole, failed };
std::atomic<int> restore_state{none};
StreamBufferHandle_t restore_stream = nullptr;
std::uint32_t restore_received = 0, restore_size = 0, restore_crc = 0;

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
    new (world) World(seed, world_config());
}

// One block of brain weights. It goes in the instruction RAM that allows only
// 32-bit access while a block fits there, and otherwise in the heap. A request
// for executable memory can also be met from the byte-addressable heap under
// its instruction address, which would spend the heap this is meant to spare:
// the instruction RAM proper ends where that region begins.
int weight_blocks_in_iram = 0, weight_blocks_in_heap = 0;

void* allocate_weights(std::size_t bytes) {
    constexpr std::uintptr_t shared_region = 0x400A0000;
    void* block = heap_caps_malloc(bytes, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
    if (block && reinterpret_cast<std::uintptr_t>(block) < shared_region) {
        ++weight_blocks_in_iram;
        return block;
    }
    heap_caps_free(block);
    block = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    if (!block) {
        std::printf("WORLD out of memory: a block of brain weights needs %u bytes, largest heap block %u\n",
                    unsigned(bytes), unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        std::abort();
    }
    ++weight_blocks_in_heap;
    return block;
}

// The next bytes of the checkpoint being restored, or null at its end.
const std::uint8_t* next_restore_bytes(void*, std::size_t* size) {
    static std::uint8_t chunk[1024];
    const auto started = esp_timer_get_time();
    for (;;) {
        const auto received = xStreamBufferReceive(restore_stream, chunk, sizeof(chunk), pdMS_TO_TICKS(20));
        if (received) {
            *size = received;
            return chunk;
        }
        const int state = restore_state;
        if (state == failed || (state == whole && xStreamBufferIsEmpty(restore_stream))) {
            return nullptr;
        }
        if (esp_timer_get_time() - started > boot_wait_limit_us) {
            restore_state = failed;
            return nullptr;
        }
    }
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
    std::uint32_t request = 0, offset = 0, total = 0;
    Outgoing* current = nullptr;
    bool failed = false;
};

void flush_save(SaveStream& save) {
    if (!save.current) {
        return;
    }
    const auto data = save.current->size;
    link::write(link::ChunkHeader{save.offset, save.total, save.request}, save.current->data);
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
    // The size follows the population, which cannot change before the save ends.
    save.total = static_cast<std::uint32_t>(world->checkpoint_bytes());
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
        if (boot_decision != waiting || restore_state != none || waited >= boot_wait_us) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool restored = false, timed_out = false;
    if (restore_state != none) {
        // Decode the checkpoint as it arrives. The checksum and the link's CRC
        // are only known at the end, and a failure leaves the world unspecified.
        restored = world->load_checkpoint_stream(next_restore_bytes, nullptr) && restore_state == whole;
        if (!restored) {
            restore_state = failed;
            rebuild_world();
        }
        boot_decision = restored ? restore : fresh;
    } else {
        int expected = waiting;
        timed_out = boot_decision.compare_exchange_strong(expected, fresh);
        // A restore that began at this very moment is dropped, not left waiting for a reader.
        restore_state = failed;
    }
    restoring = false;
    if (!restored) {
        for (unsigned tick = 0; tick < warmup_ticks; ++tick) {
            world->step();
        }
    }
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
                if (boot_open && restore_state == none && restorable_size(command.value)) {
                    restore_size = command.value;
                    restore_received = 0;
                    restore_crc = 0;
                    restoring = true;
                    restore_state = receiving;
                } else if (boot_open && restore_state == none) {
                    boot_decision = fresh;
                }
                break;
            case link::Command::restore_end:
                if (restore_state == receiving) {
                    restore_state = restore_received == restore_size && restore_crc == command.value ? whole : failed;
                }
                break;
            }
        } else if (in.type == link::Type::restore && restore_state == receiving) {
            link::ChunkHeader chunk;
            const std::size_t data = in.length - link::chunk_header_bytes;
            if (in.length >= link::chunk_header_bytes && link::read(body, in.length, chunk) &&
                chunk.offset == restore_received && chunk.total == restore_size &&
                data <= restore_size - chunk.offset) {
                const auto* bytes = body + link::chunk_header_bytes;
                restore_crc = link::crc32(bytes, data, restore_crc);
                restore_received += data;
                // The simulation task decodes as fast as chunks arrive. While this
                // waits for room, READY stays high and the SAMD51 holds the next chunk.
                for (std::size_t sent = 0; sent < data && restore_state == receiving;) {
                    sent += xStreamBufferSend(restore_stream, bytes + sent, data - sent, pdMS_TO_TICKS(50));
                }
            } else {
                restore_state = failed;
            }
        }
    }
}

} // namespace

void start_world_app() {
    free_queue = xQueueCreate(pool_size, sizeof(Outgoing*));
    send_queue = xQueueCreate(pool_size, sizeof(Outgoing*));
    restore_stream = xStreamBufferCreate(restore_stream_bytes, 1);
    // The world is allocated first, while the heap is unfragmented, and
    // constructed now so a fresh start needs no second allocation. A restore
    // replaces it in place. Its brains' weights take the instruction RAM.
    const auto largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    BrainStore::set_weight_memory(allocate_weights, [](void* block) { heap_caps_free(block); });
    world = make<World>("world", seed, world_config());
    std::printf("WORLD boot largest_block=%u world_bytes=%u checkpoint_bytes=%u max_checkpoint_bytes=%u free=%u "
                "free_32bit_only=%u weight_blocks_iram=%d weight_blocks_heap=%d\n",
                unsigned(largest), unsigned(sizeof(World)), unsigned(world->checkpoint_bytes()),
                unsigned(max_checkpoint_bytes), unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                weight_blocks_in_iram, weight_blocks_in_heap);
    xTaskCreatePinnedToCore(link_task, "link", 6144, nullptr, 10, nullptr, 0);
    xTaskCreatePinnedToCore(sim_task, "petri", 12288, nullptr, 5, nullptr, 1);
}
