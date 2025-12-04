#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_psram.h>

#include "application.h"
#include "system_info.h"

extern "C" {
#include "mbedtls/platform.h"     // Cho mbedtls_platform_set_calloc_free
}

#define TAG "main"

// ==== WRAPPER ĐỂ MbedTLS dùng PSRAM ====
extern "C" void* psram_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
}

extern "C" void app_main(void)
{
    // Cấp phát mbedTLS vào PSRAM để tránh “esp-aes: Failed to allocate memory”
    if (esp_psram_is_initialized()) {
        mbedtls_platform_set_calloc_free(psram_calloc, heap_caps_free);
        ESP_LOGI(TAG, "Redirected mbedTLS allocation to PSRAM via psram_calloc()");
    } else {
        ESP_LOGW(TAG, "PSRAM not initialized — TLS may run out of internal RAM!");
    }

    // Khởi tạo event loop mặc định
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Khởi tạo NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Log RAM trước khi chạy ứng dụng
    ESP_LOGI(TAG, "Free heap: %d | Free PSRAM: %d | Min heap: %d",
             esp_get_free_heap_size(),
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             esp_get_minimum_free_heap_size());

    // Chạy ứng dụng chính
    auto& app = Application::GetInstance();
    app.Start();
}
