#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "power_mgr.h"
#include "pindef.h"
#include "macros.h"
#include "ulp_main.h"
#include <esp32/ulp.h>
#include <esp_sleep.h>
#include "hid_mgr.h"

static const char *TAG = "POWER_MGR";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static TaskHandle_t power_mgr_task_handle = NULL;

// ADC mutex
static SemaphoreHandle_t adc_mutex = NULL;

EventGroupHandle_t power_mgr_tired_event_group = NULL;

extern const uint8_t ulp_main_bin_start[] asm("_binary_ulp_main_bin_start");
extern const uint8_t ulp_main_bin_end[] asm("_binary_ulp_main_bin_end");

void power_mgr_deep_sleep()
{
    // Kill hid_mgr task
    hid_mgr_deinit();
    // Acquire adc mutex to prevent conflict
    xSemaphoreTake(adc_mutex, portMAX_DELAY);
    unwrap_esp_err(ulp_load_binary(0, ulp_main_bin_start, (ulp_main_bin_end - ulp_main_bin_start) / sizeof(uint32_t)), "Failed to load ULP binary");
    ESP_LOGI(TAG, "ULP binary loaded successfully");
    // initialize all input GPIOs
    rtc_gpio_init(BTN1_GPIO);
    rtc_gpio_init(BTN2_GPIO);
    rtc_gpio_init(BTN3_GPIO);
    rtc_gpio_init(BTN4_GPIO);
    rtc_gpio_init(BTN5_GPIO);
    rtc_gpio_init(BTN6_GPIO);
    rtc_gpio_init(CART_PRESENCE_GPIO);

    rtc_gpio_set_direction(BTN1_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(BTN2_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(BTN3_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(BTN4_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(BTN5_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(BTN6_GPIO, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_set_direction(CART_PRESENCE_GPIO, RTC_GPIO_MODE_INPUT_ONLY);

    // reconfigure ADC to hand control to ULP
    adc_oneshot_del_unit(adc_handle);
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_FSM, // Enable ULP mode
    };
    unwrap_esp_err(adc_oneshot_new_unit(&init_cfg, &adc_handle), "Failed to create ADC one-shot handle for ULP");
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH,
        .atten = ADC_ATTEN,
    };
    unwrap_esp_err(adc_oneshot_config_channel(adc_handle, BAT_ADC_CHANNEL, &chan_cfg), "Failed to configure ADC channel");
    unwrap_esp_err(ulp_set_wakeup_period(0, 100 * 1000), "Failed to set ULP wakeup period");
    unwrap_esp_err(esp_sleep_enable_ulp_wakeup(), "Failed to enable ULP wakeup");
    unwrap_esp_err(ulp_run(&ulp_entry - RTC_SLOW_MEM), "Failed to run ULP program");

    ESP_LOGW(TAG, "Entering deep sleep");
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow time for log to flush

    // for (;;)
    // {
    //     ulp_adc_value &= 0xFFFF; // Ensure we only read the lower 16 bits
    //     ESP_LOGI(TAG, "ADC value: %d", ulp_adc_value);
    //     ulp_prog_status &= 0xFFFF;
    //     ESP_LOGI(TAG, "Program status: %04x", ulp_prog_status);
    //     vTaskDelay(pdMS_TO_TICKS(100)); // Poll every second
    // }
    esp_deep_sleep_start();
}

/**
 * @brief Task to read ADC values and control LDO_EN pin
 */
static void power_mgr_task(void *pvParameters)
{
    int adc_raw = 0;

    ESP_LOGI(TAG, "Power manager ADC polling task started");

    for (;;)
    {
        if (xSemaphoreTake(adc_mutex, portMAX_DELAY) != pdTRUE)
        {
            ESP_LOGE(TAG, "Failed to acquire ADC mutex");
            vTaskDelay(pdMS_TO_TICKS(ADC_POLL_INTERVAL_MS));
            continue; // Retry if mutex acquisition fails
        }
        esp_err_t ret = adc_oneshot_read(adc_handle, BAT_ADC_CHANNEL, &adc_raw);
        if (ret == ESP_OK)
        {
            ESP_LOGI(TAG, "ADC Channel 0 Value: %d", adc_raw);

            if (adc_raw < ADC_THRESHOLD)
            {
                ESP_LOGW(TAG, "ADC below threshold (%d), forcing deep sleep", ADC_THRESHOLD);
                todo("Notify user about low battery");
                power_mgr_deep_sleep();
            }
        }
        else
        {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
        }
        xSemaphoreGive(adc_mutex);

        // Check whether the power manager can sleep
        if (xEventGroupGetBits(power_mgr_tired_event_group) == 0)
        {
            ESP_LOGI(TAG, "All tasks are tired, entering deep sleep");
            power_mgr_deep_sleep();
        }
        else
        {
            ESP_LOGI(TAG, "Tasks are still active, bits: %x", xEventGroupGetBits(power_mgr_tired_event_group));
        }
        vTaskDelay(pdMS_TO_TICKS(ADC_POLL_INTERVAL_MS));
    }
}

esp_err_t power_mgr_init(void)
{
    ESP_LOGI(TAG, "Initializing power manager...");

    // Keep LDO enabled
    rtc_gpio_init(LDO_EN_GPIO);
    rtc_gpio_set_direction(LDO_EN_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    rtc_gpio_set_level(LDO_EN_GPIO, 1);
    rtc_gpio_hold_en(LDO_EN_GPIO);

    // Kill ULP processor
    ulp_timer_stop();

    // Initialize the event group for power manager tired events
    power_mgr_tired_event_group = xEventGroupCreate();

    // Pre-set main task tired bit
    xEventGroupSetBits(power_mgr_tired_event_group, MAIN_TIRED_BIT);

    // Initialize ADC mutex
    adc_mutex = xSemaphoreCreateMutex();
    if (adc_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create ADC mutex");
        return ESP_ERR_NO_MEM;
    }

    // --- 1. ADC One-Shot Mode Initialization ---
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_cfg, &adc_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create ADC one-shot handle: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "ADC one-shot handle created.");

    // --- 2. ADC Channel Configuration ---
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH,
        .atten = ADC_ATTEN,
    };
    ret = adc_oneshot_config_channel(adc_handle, BAT_ADC_CHANNEL, &chan_cfg);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
        adc_oneshot_del_unit(adc_handle);
        return ret;
    }
    ESP_LOGI(TAG, "ADC channel configured.");

    // --- 4. Create the power manager task ---
    BaseType_t task_ret = xTaskCreate(power_mgr_task, "power_mgr_task", 4096, NULL, 5, &power_mgr_task_handle);
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create power manager task");
        adc_oneshot_del_unit(adc_handle);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Power manager initialized successfully");
    return ESP_OK;
}

void power_mgr_notify_main_initialized(void)
{
    ESP_LOGI(TAG, "Everything is initialized, notifying power manager...");
    xEventGroupClearBits(power_mgr_tired_event_group, MAIN_TIRED_BIT);
}

esp_err_t power_mgr_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing power manager...");

    // Delete the task if it exists
    if (power_mgr_task_handle != NULL)
    {
        vTaskDelete(power_mgr_task_handle);
        power_mgr_task_handle = NULL;
    }

    // Delete ADC handle
    if (adc_handle != NULL)
    {
        adc_oneshot_del_unit(adc_handle);
        adc_handle = NULL;
    }

    ESP_LOGI(TAG, "Power manager deinitialized");
    return ESP_OK;
}