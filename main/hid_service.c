#include "hid_service.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/rtc_io.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "pin_defs.h"
#include "power_mgmt_service.h"
#include "ulp.h"
#include "ulp_hid.h"

#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"

ESP_EVENT_DEFINE_BASE(HID_SERVICE_EVENT);

#define HID_SVC_BUTTON_SAMPLE_PERIOD_US 10000
#define HID_SVC_BUTTON_DEBOUNCE_COUNT 3
#define HID_SVC_BUTTON_ADC_UNIT ADC_UNIT_1
#define HID_SVC_BUTTON_ADC_ATTEN ADC_ATTEN_DB_12
#define HID_SVC_BUTTON_ADC_BITWIDTH ADC_BITWIDTH_12
#define HID_SVC_MAIN_ADC_CHANNEL ADC_CHANNEL_7
#define HID_SVC_MISC_ADC_CHANNEL ADC_CHANNEL_6
#define HID_SVC_LED_RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define HID_SVC_LED_SETTLE_MS 5
#define HID_SVC_TASK_STACK_SIZE 3072
#define HID_SVC_TASK_PRIORITY 4
#define HID_SVC_SHUTDOWN_PRIORITY 250

static const char *TAG = "hid_svc";

extern const uint8_t ulp_hid_bin_start[] asm("_binary_ulp_hid_bin_start");
extern const uint8_t ulp_hid_bin_end[] asm("_binary_ulp_hid_bin_end");

static SemaphoreHandle_t s_service_lock;
static TaskHandle_t s_task_handle;
static adc_oneshot_unit_handle_t s_button_adc_handle;
static led_strip_handle_t s_led_strip;
static uint8_t s_led_red;
static uint8_t s_led_green;
static uint8_t s_led_blue;
static uint8_t s_led_brightness = 100;
static uint32_t s_button_state;
static uint32_t s_button_generation;
static bool s_initialized;
static bool s_led_rail_requested;
static bool s_ulp_isr_registered;

static inline uint32_t hid_service_button_mask(hid_button_t button)
{
    return 1UL << (uint32_t)button;
}

static esp_err_t hid_service_post_button_event(hid_button_t button, hid_service_event_id_t event_id)
{
    return esp_event_post(HID_SERVICE_EVENT,
                          event_id,
                          &button,
                          sizeof(button),
                          pdMS_TO_TICKS(1000));
}

static esp_err_t hid_service_configure_button_adc_channel(adc_oneshot_unit_handle_t handle,
                                                          adc_channel_t channel)
{
    adc_oneshot_chan_cfg_t channel_cfg = {
        .bitwidth = HID_SVC_BUTTON_ADC_BITWIDTH,
        .atten = HID_SVC_BUTTON_ADC_ATTEN,
    };

    return adc_oneshot_config_channel(handle, channel, &channel_cfg);
}

static esp_err_t hid_service_apply_led_locked(void)
{
    ESP_RETURN_ON_FALSE(s_led_strip != NULL, ESP_ERR_INVALID_STATE, TAG, "LED strip is not initialized");

    bool should_enable = s_led_brightness > 0 && (s_led_red != 0 || s_led_green != 0 || s_led_blue != 0);
    uint8_t scaled_red = (uint8_t)(((uint16_t)s_led_red * s_led_brightness) / 100U);
    uint8_t scaled_green = (uint8_t)(((uint16_t)s_led_green * s_led_brightness) / 100U);
    uint8_t scaled_blue = (uint8_t)(((uint16_t)s_led_blue * s_led_brightness) / 100U);
    esp_err_t err;

    if (should_enable && !s_led_rail_requested)
    {
        err = power_mgmt_service_rail_request(POWER_MGMT_RAIL_LED);
        if (err != ESP_OK)
        {
            return err;
        }

        s_led_rail_requested = true;
        vTaskDelay(pdMS_TO_TICKS(HID_SVC_LED_SETTLE_MS));
    }

    if (should_enable)
    {
        err = led_strip_set_pixel(s_led_strip, 0, scaled_red, scaled_green, scaled_blue);
        if (err != ESP_OK)
        {
            return err;
        }

        err = led_strip_refresh(s_led_strip);
        if (err != ESP_OK)
        {
            return err;
        }
    }
    else
    {
        err = led_strip_clear(s_led_strip);
        if (err != ESP_OK)
        {
            return err;
        }

        if (s_led_rail_requested)
        {
            err = power_mgmt_service_rail_release(POWER_MGMT_RAIL_LED);
            if (err != ESP_OK)
            {
                return err;
            }
            s_led_rail_requested = false;
        }
    }

    return ESP_OK;
}

static void hid_service_handle_button_update(void)
{
    hid_button_t changed_buttons[HID_BUTTON_COUNT];
    hid_service_event_id_t changed_events[HID_BUTTON_COUNT];
    size_t changed_count = 0;
    uint32_t next_generation = ulp_button_generation & UINT16_MAX;
    uint32_t next_state = ulp_button_state & UINT16_MAX;

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    if (next_generation == s_button_generation)
    {
        xSemaphoreGive(s_service_lock);
        return;
    }

    uint32_t changed_mask = s_button_state ^ next_state;
    s_button_state = next_state;
    s_button_generation = next_generation;
    xSemaphoreGive(s_service_lock);

    for (uint32_t button = 0; button < HID_BUTTON_COUNT; button++)
    {
        uint32_t button_mask = hid_service_button_mask((hid_button_t)button);
        if ((changed_mask & button_mask) == 0)
        {
            continue;
        }

        changed_buttons[changed_count] = (hid_button_t)button;
        changed_events[changed_count] = (next_state & button_mask) != 0 ? HID_EVENT_BUTTON_DOWN : HID_EVENT_BUTTON_UP;
        changed_count++;
    }

    for (size_t index = 0; index < changed_count; index++)
    {
        esp_err_t err = hid_service_post_button_event(changed_buttons[index], changed_events[index]);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG,
                     "failed to post button event %u for button %u: %s",
                     (unsigned)changed_events[index],
                     (unsigned)changed_buttons[index],
                     esp_err_to_name(err));
        }
    }
}

static void hid_service_task(void *param)
{
    (void)param;

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        hid_service_handle_button_update();
    }
}

static void IRAM_ATTR hid_service_ulp_isr(void *arg)
{
    (void)arg;

    BaseType_t task_woken = pdFALSE;
    if (s_task_handle != NULL)
    {
        vTaskNotifyGiveFromISR(s_task_handle, &task_woken);
    }
    if (task_woken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t hid_service_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return hid_service_led_off();
}

static esp_err_t hid_service_init_led_strip(void)
{
    led_strip_config_t strip_cfg = {
        .strip_gpio_num = HAL_LED_DATA_PIN,
        .max_leds = 1,
        .led_model = LED_MODEL_SK6812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = HID_SVC_LED_RMT_RESOLUTION_HZ,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip);
    if (err != ESP_OK)
    {
        return err;
    }

    return led_strip_clear(s_led_strip);
}

static esp_err_t hid_service_init_buttons(void)
{
    ESP_RETURN_ON_FALSE(rtc_gpio_is_valid_gpio(HAL_MAIN_BTN_PIN), ESP_ERR_INVALID_ARG, TAG, "main button is not RTC capable");
    ESP_RETURN_ON_FALSE(rtc_gpio_is_valid_gpio(HAL_MISC_BTN_PIN), ESP_ERR_INVALID_ARG, TAG, "misc button is not RTC capable");
    ESP_RETURN_ON_FALSE(HAL_MAIN_BTN_PIN == GPIO_NUM_35, ESP_ERR_INVALID_STATE, TAG, "main button ladder expects GPIO35");
    ESP_RETURN_ON_FALSE(HAL_MISC_BTN_PIN == GPIO_NUM_34, ESP_ERR_INVALID_STATE, TAG, "misc button ladder expects GPIO34");

    esp_err_t err;

    err = ulp_load_binary(0,
                          ulp_hid_bin_start,
                          (size_t)(ulp_hid_bin_end - ulp_hid_bin_start) / sizeof(uint32_t));
    ESP_RETURN_ON_ERROR(err, TAG, "failed to load ULP program");

    adc_oneshot_unit_init_cfg_t adc_init_cfg = {
        .unit_id = HID_SVC_BUTTON_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_FSM,
    };
    err = adc_oneshot_new_unit(&adc_init_cfg, &s_button_adc_handle);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to initialize button ADC unit");

    err = hid_service_configure_button_adc_channel(s_button_adc_handle, HID_SVC_MAIN_ADC_CHANNEL);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to configure main button ADC channel");
    err = hid_service_configure_button_adc_channel(s_button_adc_handle, HID_SVC_MISC_ADC_CHANNEL);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to configure misc button ADC channel");

    ulp_main_counter = 0;
    ulp_misc_counter = 0;
    ulp_main_stable = 0;
    ulp_misc_stable = 0;
    ulp_main_last_raw = 0;
    ulp_misc_last_raw = 0;
    ulp_button_state = 0;
    ulp_button_generation = 0;
    ulp_debounce_threshold = HID_SVC_BUTTON_DEBOUNCE_COUNT;

    err = ulp_set_wakeup_period(0, HID_SVC_BUTTON_SAMPLE_PERIOD_US);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to set ULP wake period");

    err = ulp_isr_register(hid_service_ulp_isr, NULL);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to register ULP ISR");
    s_ulp_isr_registered = true;

    err = ulp_run(&ulp_entry - RTC_SLOW_MEM);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to start ULP program");

    return ESP_OK;
}

esp_err_t hid_service_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_service_lock = xSemaphoreCreateMutex();
    if (s_service_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(hid_service_task,
                                "hid_svc",
                                HID_SVC_TASK_STACK_SIZE,
                                NULL,
                                HID_SVC_TASK_PRIORITY,
                                &s_task_handle,
                                0) != pdPASS)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = hid_service_init_led_strip();
    if (err != ESP_OK)
    {
        return err;
    }

    err = hid_service_init_buttons();
    if (err != ESP_OK)
    {
        return err;
    }

    err = power_mgmt_service_register_shutdown_callback(hid_service_shutdown_callback,
                                                        NULL,
                                                        HID_SVC_SHUTDOWN_PRIORITY);
    if (err != ESP_OK)
    {
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "HID service started (LED=%d MAIN=%d MISC=%d)",
             HAL_LED_DATA_PIN,
             HAL_MAIN_BTN_PIN,
             HAL_MISC_BTN_PIN);
    return ESP_OK;
}

esp_err_t hid_service_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    s_led_red = red;
    s_led_green = green;
    s_led_blue = blue;
    esp_err_t err = hid_service_apply_led_locked();
    xSemaphoreGive(s_service_lock);
    return err;
}

esp_err_t hid_service_led_set_brightness(uint8_t percent)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");
    ESP_RETURN_ON_FALSE(percent <= 100, ESP_ERR_INVALID_ARG, TAG, "brightness must be 0-100");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    s_led_brightness = percent;
    esp_err_t err = hid_service_apply_led_locked();
    xSemaphoreGive(s_service_lock);
    return err;
}

esp_err_t hid_service_led_off(void)
{
    return hid_service_led_set_rgb(0, 0, 0);
}

esp_err_t hid_service_get_button_state(uint32_t *button_state_out)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");
    ESP_RETURN_ON_FALSE(button_state_out != NULL, ESP_ERR_INVALID_ARG, TAG, "button state output is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    *button_state_out = s_button_state;
    xSemaphoreGive(s_service_lock);
    return ESP_OK;
}

esp_err_t hid_service_get_adc_status(hid_service_adc_status_t *status_out)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");
    ESP_RETURN_ON_FALSE(status_out != NULL, ESP_ERR_INVALID_ARG, TAG, "ADC status output is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    status_out->main_raw = (uint16_t)(ulp_main_last_raw & UINT16_MAX);
    status_out->misc_raw = (uint16_t)(ulp_misc_last_raw & UINT16_MAX);
    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}