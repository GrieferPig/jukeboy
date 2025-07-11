#ifdef BUILD_FACTORY_APP

#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "part_mgr.h"
#include "macros.h"
#include "ble_serv.h"
#include "pindef.h"
#include "driver/gpio.h"

const char *TAG = "factory_app";

// panic counter in NVS
#define PANIC_MAX_RETRIES 3
#define NVS_NAMESPACE "factory_app"
#define NVS_KEY_PANIC_COUNTER "panic_counter"

void recovery_mode(void);

#define enter_rec_when(cond, msg, ...)         \
    {                                          \
        if (cond)                              \
        {                                      \
            ESP_LOGE(TAG, msg, ##__VA_ARGS__); \
            recovery_mode();                   \
        }                                      \
    }

// Button used to enter recovery mode, ext pulled up
#define REC_BUTTON_GPIO BTN2_GPIO // GPIO 0 for recovery button

void factory_app_main(void)
{
    // Check whether rec buttons are pressed
    gpio_set_direction(REC_BUTTON_GPIO, GPIO_MODE_INPUT);
    if (gpio_get_level(REC_BUTTON_GPIO) == 0)
    {
        // need to be held for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        if (gpio_get_level(REC_BUTTON_GPIO) == 0)
        {
            ESP_LOGI(TAG, "Recovery button pressed, entering recovery mode.");
            recovery_mode();
        }
        else
        {
            ESP_LOGI(TAG, "Recovery button released too early, not entering recovery mode.");
        }
    }
    else
    {
        ESP_LOGI(TAG, "Recovery button not pressed, continuing normal boot.");
    }

    // Initialize NVS
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        // NVS partition was truncated and needs to be erased
        // Retry nvs_flash_init
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    int32_t panic_counter = 0;
    nvs_handle_t nvs_handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
    }
    else
    {
        // Read the panic counter
        err = nvs_get_i32(nvs_handle, NVS_KEY_PANIC_COUNTER, &panic_counter);
        if (err == ESP_ERR_NVS_NOT_FOUND)
        {
            ESP_LOGI(TAG, "Panic counter not found in NVS, initializing to 0.");
            panic_counter = 0;
        }
        else
        {
            jam(err == ESP_OK, "Failed to read panic counter from NVS: %s", esp_err_to_name(err));
        }
    }

    // get reboot reason
    esp_reset_reason_t reboot_reason = esp_reset_reason();
    ESP_LOGI(TAG, "Reboot reason: %d", reboot_reason);

    switch (reboot_reason)
    {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    {
        ESP_LOGW(TAG, "Detected a watchdog reset or panic. Incrementing panic counter.");
        // Increment the panic counter in NVS
        panic_counter++;
        nvs_set_i32(nvs_handle, NVS_KEY_PANIC_COUNTER, panic_counter);
        nvs_commit(nvs_handle);
        ESP_LOGW(TAG, "Panic counter: %d", panic_counter);
        if (panic_counter >= PANIC_MAX_RETRIES)
        {
            ESP_LOGE(TAG, "Panic counter exceeded %d.", PANIC_MAX_RETRIES);
            // Reset the panic counter
            panic_counter = 0;
            nvs_set_i32(nvs_handle, NVS_KEY_PANIC_COUNTER, panic_counter);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            recovery_mode();
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "Normal (re)boot, resetting panic counter.");
        panic_counter = 0;
        nvs_set_i32(nvs_handle, NVS_KEY_PANIC_COUNTER, panic_counter);
        nvs_commit(nvs_handle);
        break;
    }

    nvs_close(nvs_handle);

    // Find the next available OTA partition (which is ota_0 in our case)
    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    enter_rec_when(ota_partition == NULL, "No OTA partition found.");

    // Set the boot partition to ota_0. This marks it as "unverified".
    err = esp_ota_set_boot_partition(ota_partition);
    enter_rec_when(err != ESP_OK, "Failed to set boot partition: %s.", esp_err_to_name(err));

    ESP_LOGI(TAG, "Booting into main app (ota_0) for a single run...");
    esp_restart();
}

void recovery_mode(void)
{
    ESP_LOGI(TAG, "Entering recovery mode...");

    // Initialize BLE service as the last step in recovery mode
    ble_serv_init();

    ESP_LOGI(TAG, "BLE service initialized. Recovery mode ready for OTA updates.");

    // Keep the device in recovery mode - don't restart
    // The BLE service will handle OTA updates
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // esp_restart();
}
#endif // BUILD_FACTORY_APP