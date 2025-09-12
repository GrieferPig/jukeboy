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
#include "hid/led_mgr.h"
#include "hid/led_animations.h"

static const char *TAG = "factory_app";
#define bootloader_ver "1.0.0"

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
#define REC_BUTTON_GPIO BTN2_GPIO      // BTN 2 for recovery button
#define REC_EXIT_BUTTON_GPIO BTN3_GPIO // BTN 3 for exit recovery button

void factory_app_main(void)
{
    ESP_LOGI(TAG, "Starting third stage bootloader: version %s", bootloader_ver);
    // Initialize LED Manager first
    esp_err_t led_ret = led_mgr_init();
    if (led_ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LED Manager: %s", esp_err_to_name(led_ret));
        // Continue without LED functionality
    }

    // initialize i2s pins to output 0 to avoid noise
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << I2S_BCK_PIN) | (1ULL << I2S_WS_PIN) | (1ULL << I2S_DOUT_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE};
    gpio_config(&io_conf);
    gpio_set_level(I2S_BCK_PIN, 0);
    gpio_set_level(I2S_WS_PIN, 0);
    gpio_set_level(I2S_DOUT_PIN, 0);

    // Check whether rec buttons are pressed
    gpio_set_direction(REC_BUTTON_GPIO, GPIO_MODE_INPUT);
    if (gpio_get_level(REC_BUTTON_GPIO) == 0)
    {
        ESP_LOGI(TAG, "Recovery button detected pressed, showing default animation");
        led_animations_play_action(LED_ACT_DEFAULT, false);

        int held = 1;
        for (int i = 0; i < 30; ++i) // 30 * 100ms = 3 seconds
        {
            vTaskDelay(pdMS_TO_TICKS(100));
            if (gpio_get_level(REC_BUTTON_GPIO) != 0)
            {
                held = 0;
                break;
            }
        }
        if (held)
        {
            ESP_LOGI(TAG, "Recovery button pressed for 3 seconds, entering recovery mode.");
            recovery_mode();
        }
        else
        {
            ESP_LOGI(TAG, "Recovery button released too early, not entering recovery mode.");
            // Stop LED animation since we're not entering recovery mode
            led_mgr_stop();
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

            // Stop LED before entering recovery mode
            led_mgr_stop();

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

    // Stop LED before continuing to OTA app
    led_mgr_stop();

    // Find the next available OTA partition (which is ota_0 in our case)
    const esp_partition_t *ota_partition = esp_ota_get_next_update_partition(NULL);
    enter_rec_when(ota_partition == NULL, "No OTA partition found.");

    // Set the boot partition to ota_0. This marks it as "unverified".
    err = esp_ota_set_boot_partition(ota_partition);
    enter_rec_when(err != ESP_OK, "Failed to set boot partition: %s.", esp_err_to_name(err));

    ESP_LOGI(TAG, "Booting into main app (ota_0) for a single run...");

    // Turn off LED before restart
    led_mgr_stop();
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to ensure LED turns off

    esp_restart();
}

void recovery_mode(void)
{
    ESP_LOGI(TAG, "Entering recovery mode...");

    // Start flashing red LED in recovery mode to notify
    led_animations_play_action(LED_ACT_SD_FAIL, false);

    // Initialize BLE service as the last step in recovery mode
    ble_serv_init();

    ESP_LOGI(TAG, "BLE service initialized. Recovery mode ready for OTA updates.");

    // Poll if the recovery exit button is pressed
    while (gpio_get_level(REC_EXIT_BUTTON_GPIO) == 1)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Button pressed, exit recovery mode

    // Turn off LED before restart
    led_mgr_stop();
    vTaskDelay(pdMS_TO_TICKS(100)); // Small delay to ensure LED turns off

    esp_restart();
}
#endif // BUILD_FACTORY_APP