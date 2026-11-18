// Runs the portable Petri self-test on ESP32 core 1 and reports memory.
#include "selftest.hpp"

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include <cstdio>
#include <cstring>

#if CONFIG_PETRI_BLE_ADVERTISE
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#endif

namespace {

void print_heap(const char* stage) {
    std::printf("HEAP stage=%s free_8bit=%u largest_8bit=%u min_free_8bit=%u free_32bit_only=%u\n", stage,
                unsigned(heap_caps_get_free_size(MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT)),
                unsigned(heap_caps_get_free_size(MALLOC_CAP_32BIT) - heap_caps_get_free_size(MALLOC_CAP_8BIT)));
}

#if CONFIG_PETRI_BLE_ADVERTISE
volatile bool advertising = false;

int on_gap_event(ble_gap_event*, void*) {
    return 0;
}

void start_advertising() {
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char* name = ble_svc_gap_device_name();
    fields.name = reinterpret_cast<const std::uint8_t*>(name);
    fields.name_len = std::strlen(name);
    fields.name_is_complete = 1;
    ble_gap_adv_set_fields(&fields);
    std::uint8_t address_type = 0;
    ble_hs_id_infer_auto(0, &address_type);
    ble_gap_adv_params params{};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    advertising = ble_gap_adv_start(address_type, nullptr, BLE_HS_FOREVER, &params, on_gap_event, nullptr) == 0;
}

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void start_ble() {
    if (nvs_flash_init() != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    nimble_port_init();
    ble_hs_cfg.sync_cb = start_advertising;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("Petri");
    nimble_port_freertos_init(host_task);
    for (int wait = 0; wait < 200 && !advertising; ++wait) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    std::printf("BLE nimble advertising=%d\n", advertising ? 1 : 0);
}
#endif

void selftest_task(void*) {
    petri::selftest::run({
        [] { return std::uint64_t(esp_timer_get_time()); },
        [](const char* line) {
            std::puts(line);
            std::fflush(stdout);
        },
        [] { vTaskDelay(1); },
    });
    print_heap("after_selftest");
    std::printf("DONE core=%d\n", xPortGetCoreID());
    vTaskDelete(nullptr);
}

} // namespace

extern "C" void app_main() {
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    std::printf("BOOT idf=%s cores=%d revision=%d cpu_mhz=%d\n", esp_get_idf_version(), chip.cores,
                chip.revision, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    print_heap("boot");
#if CONFIG_PETRI_BLE_ADVERTISE
    start_ble();
    print_heap("ble_advertising");
#endif
    // Core 1 runs only the simulation, as planned for the dual-processor firmware.
    xTaskCreatePinnedToCore(selftest_task, "petri", 16384, nullptr, 5, nullptr, 1);
}
