#include "hid_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/rtc_io.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_continuous.h"
#include "esp_adc/adc_filter.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs.h"
#include "pin_defs.h"
#include "power_mgmt_service.h"

ESP_EVENT_DEFINE_BASE(HID_SERVICE_EVENT);

#define HID_SVC_BUTTON_SAMPLE_PERIOD_MS 10
#define HID_SVC_BUTTON_DEBOUNCE_COUNT 3
#define HID_SVC_BUTTON_ADC_UNIT ADC_UNIT_1
#define HID_SVC_BUTTON_ADC_ATTEN ADC_ATTEN_DB_12
#define HID_SVC_BUTTON_ADC_BITWIDTH ADC_BITWIDTH_12
#define HID_SVC_MAIN_ADC_CHANNEL ADC_CHANNEL_0
#define HID_SVC_MISC_ADC_CHANNEL ADC_CHANNEL_3
#define HID_SVC_BUTTON_ADC_SAMPLE_FREQ_HZ 2000
#define HID_SVC_BUTTON_ADC_CONV_FRAME_SIZE 64
#define HID_SVC_BUTTON_ADC_READ_BUF_SIZE 128
#define HID_SVC_BUTTON_ADC_STORE_BUF_SIZE 512
#define HID_SVC_BUTTON_ADC_IIR_COEFF ADC_DIGI_IIR_FILTER_COEFF_8
#define HID_SVC_BUTTON_ADC_MAX_RAW 4095U
#define HID_SVC_SIDE_BUTTON_ACTIVE_LEVEL 0
#define HID_SVC_BUTTONS_PER_LADDER 3U
#define HID_SVC_LADDER_STATE_COUNT (1U << HID_SVC_BUTTONS_PER_LADDER)
#define HID_SVC_LADDER_LEVEL_COUNT HID_SERVICE_BUTTON_LADDER_STATE_COUNT
#define HID_SVC_BUTTON_CALIBRATION_NAMESPACE "hid"
#define HID_SVC_BUTTON_CALIBRATION_KEY "calib"
#define HID_SVC_BUTTON_CALIBRATION_VERSION 1U
#define HID_SVC_LED_RMT_RESOLUTION_HZ (10 * 1000 * 1000)
#define HID_SVC_LED_SETTLE_MS 5
#define HID_SVC_TASK_STACK_SIZE 3072
#define HID_SVC_TASK_PRIORITY 4
#define HID_SVC_SHUTDOWN_PRIORITY 250

#define HID_SVC_BUTTON_ADC_OUTPUT_FORMAT ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define HID_SVC_BUTTON_ADC_GET_CHANNEL(sample) ((sample)->type2.channel)
#define HID_SVC_BUTTON_ADC_GET_DATA(sample) ((sample)->type2.data)
#define HID_SVC_BUTTON_ADC_GET_UNIT(sample) ((sample)->type2.unit == 0 ? ADC_UNIT_1 : ADC_UNIT_2)

static const char *TAG = "hid_svc";

typedef struct
{
    uint16_t raw;
    uint8_t state;
} hid_service_ladder_level_t;

typedef struct
{
    adc_channel_t channel;
    adc_cali_handle_t cali_handle;
    adc_cali_scheme_ver_t cali_scheme;
    bool cali_enabled;
    bool decode_ready;
    uint8_t stable_state;
    uint8_t pending_state;
    uint8_t pending_count;
    uint16_t last_raw;
    uint16_t last_value;
    hid_service_ladder_level_t levels[HID_SVC_LADDER_LEVEL_COUNT];
    uint16_t calibration_value[HID_SVC_LADDER_STATE_COUNT];
    uint16_t threshold_value[HID_SVC_LADDER_LEVEL_COUNT - 1U];
} hid_service_ladder_runtime_t;

typedef struct
{
    uint8_t version;
    uint8_t valid;
    uint16_t reserved;
    uint16_t main_value[HID_SVC_LADDER_STATE_COUNT];
    uint16_t misc_value[HID_SVC_LADDER_STATE_COUNT];
} hid_service_button_calibration_record_t;

typedef struct
{
    bool stable_pressed;
    bool pending_pressed;
    uint8_t pending_count;
} hid_service_gpio_button_runtime_t;

static SemaphoreHandle_t s_service_lock;
static TaskHandle_t s_task_handle;
static adc_continuous_handle_t s_button_adc_handle;
static adc_iir_filter_handle_t s_main_button_adc_filter;
static adc_iir_filter_handle_t s_misc_button_adc_filter;
static led_strip_handle_t s_led_strip;
static uint8_t s_led_red;
static uint8_t s_led_green;
static uint8_t s_led_blue;
static uint8_t s_led_brightness = 100;
static uint32_t s_button_state;
static uint32_t s_button_generation;
static bool s_initialized;
static bool s_led_rail_requested;
static bool s_button_adc_started;
static bool s_button_decode_enabled;
static bool s_button_calibration_active;
static hid_service_ladder_runtime_t s_main_ladder = {
    .channel = HID_SVC_MAIN_ADC_CHANNEL,
    .last_raw = HID_SVC_BUTTON_ADC_MAX_RAW,
    .last_value = HID_SVC_BUTTON_ADC_MAX_RAW,
};
static hid_service_ladder_runtime_t s_misc_ladder = {
    .channel = HID_SVC_MISC_ADC_CHANNEL,
    .last_raw = HID_SVC_BUTTON_ADC_MAX_RAW,
    .last_value = HID_SVC_BUTTON_ADC_MAX_RAW,
};
static hid_service_gpio_button_runtime_t s_side_button;

static inline uint32_t hid_service_button_mask(hid_button_t button)
{
    return 1UL << (uint32_t)button;
}

static TickType_t hid_service_button_sample_ticks(void)
{
    TickType_t sample_ticks = pdMS_TO_TICKS(HID_SVC_BUTTON_SAMPLE_PERIOD_MS);
    return sample_ticks == 0 ? 1 : sample_ticks;
}

static uint32_t hid_service_compose_button_state(uint8_t main_state, uint8_t misc_state, bool side_pressed)
{
    uint32_t button_state = ((uint32_t)main_state & 0x7U) |
                            (((uint32_t)misc_state & 0x7U) << HID_SVC_BUTTONS_PER_LADDER);

    if (side_pressed)
    {
        button_state |= hid_service_button_mask(HID_BUTTON_SIDE);
    }

    return button_state;
}

static void hid_service_set_button_state_locked(uint32_t next_state)
{
    if (s_button_state != next_state)
    {
        s_button_generation++;
    }

    s_button_state = next_state;
}

static uint32_t hid_service_build_button_state_locked(void)
{
    uint8_t main_state = 0;
    uint8_t misc_state = 0;

    if (s_button_decode_enabled && !s_button_calibration_active)
    {
        main_state = s_main_ladder.stable_state;
        misc_state = s_misc_ladder.stable_state;
    }

    return hid_service_compose_button_state(main_state, misc_state, s_side_button.stable_pressed);
}

static void hid_service_reset_ladder_state_locked(hid_service_ladder_runtime_t *ladder)
{
    if (ladder == NULL)
    {
        return;
    }

    ladder->stable_state = 0;
    ladder->pending_state = 0;
    ladder->pending_count = 0;
}

static void hid_service_reset_gpio_button_state_locked(hid_service_gpio_button_runtime_t *button)
{
    if (button == NULL)
    {
        return;
    }

    button->stable_pressed = false;
    button->pending_pressed = false;
    button->pending_count = 0;
}

static void hid_service_reset_button_state_locked(void)
{
    hid_service_reset_ladder_state_locked(&s_main_ladder);
    hid_service_reset_ladder_state_locked(&s_misc_ladder);
    hid_service_set_button_state_locked(hid_service_build_button_state_locked());
}

static esp_err_t hid_service_create_button_adc_cali(hid_service_ladder_runtime_t *ladder)
{
    adc_cali_scheme_ver_t scheme_mask = 0;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(ladder != NULL, ESP_ERR_INVALID_ARG, TAG, "ladder state is null");

    ladder->cali_handle = NULL;
    ladder->cali_scheme = 0;
    ladder->cali_enabled = false;

    err = adc_cali_check_scheme(&scheme_mask);
    if (err == ESP_ERR_NOT_SUPPORTED)
    {
        ESP_LOGW(TAG, "ADC calibration is not supported for channel %u", (unsigned)ladder->channel);
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if ((scheme_mask & ADC_CALI_SCHEME_VER_CURVE_FITTING) != 0)
    {
        adc_cali_curve_fitting_config_t cali_cfg = {
            .unit_id = HID_SVC_BUTTON_ADC_UNIT,
            .chan = ladder->channel,
            .atten = HID_SVC_BUTTON_ADC_ATTEN,
            .bitwidth = HID_SVC_BUTTON_ADC_BITWIDTH,
        };
        err = adc_cali_create_scheme_curve_fitting(&cali_cfg, &ladder->cali_handle);
        if (err == ESP_OK)
        {
            ladder->cali_scheme = ADC_CALI_SCHEME_VER_CURVE_FITTING;
            ladder->cali_enabled = true;
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_SUPPORTED)
        {
            return err;
        }
    }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    if ((scheme_mask & ADC_CALI_SCHEME_VER_LINE_FITTING) != 0)
    {
        adc_cali_line_fitting_config_t cali_cfg = {
            .unit_id = HID_SVC_BUTTON_ADC_UNIT,
            .atten = HID_SVC_BUTTON_ADC_ATTEN,
            .bitwidth = HID_SVC_BUTTON_ADC_BITWIDTH,
#if CONFIG_IDF_TARGET_ESP32
            .default_vref = 0,
#endif
        };
        err = adc_cali_create_scheme_line_fitting(&cali_cfg, &ladder->cali_handle);
        if (err == ESP_OK)
        {
            ladder->cali_scheme = ADC_CALI_SCHEME_VER_LINE_FITTING;
            ladder->cali_enabled = true;
            return ESP_OK;
        }
        if (err != ESP_ERR_NOT_SUPPORTED)
        {
            return err;
        }
    }
#endif

    ESP_LOGW(TAG, "ADC calibration scheme creation failed for channel %u; using raw values", (unsigned)ladder->channel);
    return ESP_OK;
}

static void hid_service_delete_button_adc_cali(hid_service_ladder_runtime_t *ladder)
{
    if (ladder == NULL || ladder->cali_handle == NULL)
    {
        return;
    }

    switch (ladder->cali_scheme)
    {
    case ADC_CALI_SCHEME_VER_CURVE_FITTING:
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        (void)adc_cali_delete_scheme_curve_fitting(ladder->cali_handle);
#endif
        break;
    case ADC_CALI_SCHEME_VER_LINE_FITTING:
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        (void)adc_cali_delete_scheme_line_fitting(ladder->cali_handle);
#endif
        break;
    default:
        break;
    }

    ladder->cali_handle = NULL;
    ladder->cali_scheme = 0;
    ladder->cali_enabled = false;
}

static esp_err_t hid_service_sample_value_from_raw(const hid_service_ladder_runtime_t *ladder,
                                                   uint16_t raw,
                                                   uint16_t *value_out)
{
    ESP_RETURN_ON_FALSE(ladder != NULL, ESP_ERR_INVALID_ARG, TAG, "ladder state is null");
    ESP_RETURN_ON_FALSE(value_out != NULL, ESP_ERR_INVALID_ARG, TAG, "button ADC value output is null");

    if (!ladder->cali_enabled || ladder->cali_handle == NULL)
    {
        *value_out = raw;
        return ESP_OK;
    }

    int voltage = 0;
    esp_err_t err = adc_cali_raw_to_voltage(ladder->cali_handle, raw, &voltage);
    if (err != ESP_OK)
    {
        return err;
    }
    if (voltage <= 0)
    {
        *value_out = 0;
        return ESP_OK;
    }
    if (voltage >= UINT16_MAX)
    {
        *value_out = UINT16_MAX;
        return ESP_OK;
    }

    *value_out = (uint16_t)voltage;
    return ESP_OK;
}

static esp_err_t hid_service_load_button_calibration_record(hid_service_button_calibration_record_t *record_out)
{
    nvs_handle_t handle;
    size_t record_size = sizeof(*record_out);
    esp_err_t err;

    ESP_RETURN_ON_FALSE(record_out != NULL, ESP_ERR_INVALID_ARG, TAG, "button calibration record output is null");

    memset(record_out, 0, sizeof(*record_out));
    err = nvs_open(HID_SVC_BUTTON_CALIBRATION_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_get_blob(handle, HID_SVC_BUTTON_CALIBRATION_KEY, record_out, &record_size);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        memset(record_out, 0, sizeof(*record_out));
        return err;
    }

    if (record_size != sizeof(*record_out) ||
        record_out->version != HID_SVC_BUTTON_CALIBRATION_VERSION ||
        !record_out->valid)
    {
        memset(record_out, 0, sizeof(*record_out));
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static esp_err_t hid_service_store_button_calibration_record(const hid_service_button_calibration_record_t *record)
{
    nvs_handle_t handle;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(record != NULL, ESP_ERR_INVALID_ARG, TAG, "button calibration record is null");

    err = nvs_open(HID_SVC_BUTTON_CALIBRATION_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_blob(handle, HID_SVC_BUTTON_CALIBRATION_KEY, record, sizeof(*record));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void hid_service_sort_ladder_levels(hid_service_ladder_level_t *levels, size_t level_count)
{
    for (size_t index = 1; index < level_count; index++)
    {
        hid_service_ladder_level_t level = levels[index];
        size_t insert_index = index;

        while (insert_index > 0 && level.raw < levels[insert_index - 1U].raw)
        {
            levels[insert_index] = levels[insert_index - 1U];
            insert_index--;
        }

        levels[insert_index] = level;
    }
}

static esp_err_t hid_service_prepare_ladder_calibration(const uint16_t *values,
                                                        hid_service_ladder_level_t *levels_out,
                                                        uint16_t *threshold_out)
{
    ESP_RETURN_ON_FALSE(values != NULL, ESP_ERR_INVALID_ARG, TAG, "ladder calibration values are null");
    ESP_RETURN_ON_FALSE(levels_out != NULL, ESP_ERR_INVALID_ARG, TAG, "ladder calibration levels are null");
    ESP_RETURN_ON_FALSE(threshold_out != NULL, ESP_ERR_INVALID_ARG, TAG, "ladder calibration thresholds are null");

    for (uint8_t state = 0; state < HID_SVC_LADDER_STATE_COUNT; state++)
    {
        levels_out[state].state = state;
        levels_out[state].raw = values[state];
    }

    hid_service_sort_ladder_levels(levels_out, HID_SVC_LADDER_LEVEL_COUNT);

    for (uint8_t index = 0; index < HID_SVC_LADDER_LEVEL_COUNT - 1U; index++)
    {
        if (levels_out[index].raw >= levels_out[index + 1U].raw)
        {
            return ESP_ERR_INVALID_STATE;
        }

        threshold_out[index] =
            (uint16_t)(((uint32_t)levels_out[index].raw + (uint32_t)levels_out[index + 1U].raw) / 2U);
    }

    return ESP_OK;
}

static esp_err_t hid_service_apply_ladder_calibration(hid_service_ladder_runtime_t *ladder,
                                                      const uint16_t *values)
{
    hid_service_ladder_level_t levels[HID_SVC_LADDER_LEVEL_COUNT];
    uint16_t thresholds[HID_SVC_LADDER_LEVEL_COUNT - 1U];
    esp_err_t err;

    ESP_RETURN_ON_FALSE(ladder != NULL, ESP_ERR_INVALID_ARG, TAG, "ladder state is null");

    err = hid_service_prepare_ladder_calibration(values, levels, thresholds);
    if (err != ESP_OK)
    {
        ladder->decode_ready = false;
        ESP_LOGE(TAG, "invalid ladder calibration for channel %u", (unsigned)ladder->channel);
        return err;
    }

    memcpy(ladder->calibration_value, values, sizeof(ladder->calibration_value));
    memcpy(ladder->levels, levels, sizeof(ladder->levels));
    memcpy(ladder->threshold_value, thresholds, sizeof(ladder->threshold_value));
    ladder->decode_ready = true;

    ESP_LOGI(TAG,
             "button ladder channel %u values 0=%u 1=%u 2=%u 3=%u 4=%u 5=%u 6=%u 7=%u",
             (unsigned)ladder->channel,
             (unsigned)ladder->calibration_value[0],
             (unsigned)ladder->calibration_value[1],
             (unsigned)ladder->calibration_value[2],
             (unsigned)ladder->calibration_value[3],
             (unsigned)ladder->calibration_value[4],
             (unsigned)ladder->calibration_value[5],
             (unsigned)ladder->calibration_value[6],
             (unsigned)ladder->calibration_value[7]);

    return ESP_OK;
}

static esp_err_t hid_service_apply_button_calibration(const hid_service_button_calibration_record_t *record)
{
    esp_err_t err;

    ESP_RETURN_ON_FALSE(record != NULL, ESP_ERR_INVALID_ARG, TAG, "button calibration record is null");

    err = hid_service_apply_ladder_calibration(&s_main_ladder, record->main_value);
    if (err != ESP_OK)
    {
        s_button_decode_enabled = false;
        s_misc_ladder.decode_ready = false;
        hid_service_reset_button_state_locked();
        return err;
    }

    err = hid_service_apply_ladder_calibration(&s_misc_ladder, record->misc_value);
    if (err != ESP_OK)
    {
        s_button_decode_enabled = false;
        s_main_ladder.decode_ready = false;
        s_misc_ladder.decode_ready = false;
        hid_service_reset_button_state_locked();
        return err;
    }

    s_button_decode_enabled = true;
    hid_service_reset_button_state_locked();
    return ESP_OK;
}

static esp_err_t hid_service_load_button_calibration(void)
{
    hid_service_button_calibration_record_t record;
    esp_err_t err = hid_service_load_button_calibration_record(&record);
    if (err != ESP_OK)
    {
        s_button_decode_enabled = false;
        s_main_ladder.decode_ready = false;
        s_misc_ladder.decode_ready = false;
        hid_service_reset_button_state_locked();
        return err;
    }

    return hid_service_apply_button_calibration(&record);
}

static uint8_t hid_service_decode_ladder_state(const hid_service_ladder_runtime_t *ladder, uint16_t value)
{
    ESP_RETURN_ON_FALSE(ladder != NULL, 0, TAG, "ladder state is null");

    for (uint8_t index = 0; index < HID_SVC_LADDER_LEVEL_COUNT - 1U; index++)
    {
        if (value < ladder->threshold_value[index])
        {
            return ladder->levels[index].state;
        }
    }

    return ladder->levels[HID_SVC_LADDER_LEVEL_COUNT - 1U].state;
}

static esp_err_t hid_service_update_button_adc_raws(uint16_t *main_raw_out, uint16_t *misc_raw_out)
{
    uint16_t main_raw = s_main_ladder.last_raw;
    uint16_t misc_raw = s_misc_ladder.last_raw;
    uint8_t result[HID_SVC_BUTTON_ADC_READ_BUF_SIZE];

    ESP_RETURN_ON_FALSE(main_raw_out != NULL, ESP_ERR_INVALID_ARG, TAG, "main button ADC output is null");
    ESP_RETURN_ON_FALSE(misc_raw_out != NULL, ESP_ERR_INVALID_ARG, TAG, "misc button ADC output is null");
    ESP_RETURN_ON_FALSE(s_button_adc_handle != NULL, ESP_ERR_INVALID_STATE, TAG, "button ADC handle is not initialized");

    while (true)
    {
        uint32_t out_length = 0;
        esp_err_t err = adc_continuous_read(s_button_adc_handle,
                                            result,
                                            sizeof(result),
                                            &out_length,
                                            0);
        if (err == ESP_ERR_TIMEOUT)
        {
            break;
        }
        if (err != ESP_OK)
        {
            return err;
        }

        for (uint32_t index = 0; index + SOC_ADC_DIGI_RESULT_BYTES <= out_length; index += SOC_ADC_DIGI_RESULT_BYTES)
        {
            adc_digi_output_data_t *sample = (adc_digi_output_data_t *)&result[index];

            if (HID_SVC_BUTTON_ADC_GET_UNIT(sample) != HID_SVC_BUTTON_ADC_UNIT)
            {
                continue;
            }

            adc_channel_t channel = (adc_channel_t)HID_SVC_BUTTON_ADC_GET_CHANNEL(sample);
            uint16_t raw = (uint16_t)HID_SVC_BUTTON_ADC_GET_DATA(sample);
            if (channel == HID_SVC_MAIN_ADC_CHANNEL)
            {
                main_raw = raw;
            }
            else if (channel == HID_SVC_MISC_ADC_CHANNEL)
            {
                misc_raw = raw;
            }
        }
    }

    *main_raw_out = main_raw;
    *misc_raw_out = misc_raw;
    return ESP_OK;
}

static void hid_service_update_ladder_sample_locked(hid_service_ladder_runtime_t *ladder,
                                                    uint16_t raw,
                                                    uint16_t value)
{
    if (ladder == NULL)
    {
        return;
    }

    ladder->last_raw = raw;
    ladder->last_value = value;
}

static bool hid_service_update_ladder_locked(hid_service_ladder_runtime_t *ladder, uint16_t value)
{
    uint8_t decoded_state;

    ESP_RETURN_ON_FALSE(ladder != NULL, false, TAG, "ladder state is null");
    if (!ladder->decode_ready)
    {
        return false;
    }

    decoded_state = hid_service_decode_ladder_state(ladder, value);

    if (decoded_state == ladder->stable_state)
    {
        ladder->pending_state = decoded_state;
        ladder->pending_count = 0;
        return false;
    }

    if (decoded_state != ladder->pending_state)
    {
        ladder->pending_state = decoded_state;
        ladder->pending_count = 1;
    }
    else if (ladder->pending_count < UINT8_MAX)
    {
        ladder->pending_count++;
    }

    if (ladder->pending_count < HID_SVC_BUTTON_DEBOUNCE_COUNT)
    {
        return false;
    }

    ladder->stable_state = decoded_state;
    ladder->pending_state = decoded_state;
    ladder->pending_count = 0;
    return true;
}

static bool hid_service_update_gpio_button_locked(hid_service_gpio_button_runtime_t *button, bool pressed)
{
    ESP_RETURN_ON_FALSE(button != NULL, false, TAG, "GPIO button state is null");

    if (pressed == button->stable_pressed)
    {
        button->pending_pressed = pressed;
        button->pending_count = 0;
        return false;
    }

    if (pressed != button->pending_pressed)
    {
        button->pending_pressed = pressed;
        button->pending_count = 1;
    }
    else if (button->pending_count < UINT8_MAX)
    {
        button->pending_count++;
    }

    if (button->pending_count < HID_SVC_BUTTON_DEBOUNCE_COUNT)
    {
        return false;
    }

    button->stable_pressed = pressed;
    button->pending_pressed = pressed;
    button->pending_count = 0;
    return true;
}

static bool hid_service_read_side_button_pressed(void)
{
    return gpio_get_level(HAL_SIDE_BTN_PIN) == HID_SVC_SIDE_BUTTON_ACTIVE_LEVEL;
}

static esp_err_t hid_service_post_button_event(hid_button_t button, hid_service_event_id_t event_id)
{
    return esp_event_post(HID_SERVICE_EVENT,
                          event_id,
                          &button,
                          sizeof(button),
                          pdMS_TO_TICKS(1000));
}

static void hid_service_deinit_button_adc(void)
{
    s_button_decode_enabled = false;
    s_button_calibration_active = false;
    s_main_ladder.decode_ready = false;
    s_misc_ladder.decode_ready = false;

    hid_service_delete_button_adc_cali(&s_main_ladder);
    hid_service_delete_button_adc_cali(&s_misc_ladder);

    if (s_button_adc_handle != NULL)
    {
        if (s_button_adc_started)
        {
            (void)adc_continuous_stop(s_button_adc_handle);
            s_button_adc_started = false;
        }

        if (s_main_button_adc_filter != NULL)
        {
            (void)adc_continuous_iir_filter_disable(s_main_button_adc_filter);
            (void)adc_del_continuous_iir_filter(s_main_button_adc_filter);
            s_main_button_adc_filter = NULL;
        }

        if (s_misc_button_adc_filter != NULL)
        {
            (void)adc_continuous_iir_filter_disable(s_misc_button_adc_filter);
            (void)adc_del_continuous_iir_filter(s_misc_button_adc_filter);
            s_misc_button_adc_filter = NULL;
        }

        (void)adc_continuous_deinit(s_button_adc_handle);
        s_button_adc_handle = NULL;
    }
}

static esp_err_t hid_service_configure_button_adc(void)
{
    if (s_button_adc_handle != NULL)
    {
        return ESP_OK;
    }

    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = HID_SVC_BUTTON_ADC_STORE_BUF_SIZE,
        .conv_frame_size = HID_SVC_BUTTON_ADC_CONV_FRAME_SIZE,
        .flags = {
            .flush_pool = 1,
        },
    };
    esp_err_t err = adc_continuous_new_handle(&handle_cfg, &s_button_adc_handle);
    if (err != ESP_OK)
    {
        return err;
    }

    adc_digi_pattern_config_t adc_pattern[2] = {
        {
            .atten = HID_SVC_BUTTON_ADC_ATTEN,
            .channel = HID_SVC_MAIN_ADC_CHANNEL,
            .unit = HID_SVC_BUTTON_ADC_UNIT,
            .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
        },
        {
            .atten = HID_SVC_BUTTON_ADC_ATTEN,
            .channel = HID_SVC_MISC_ADC_CHANNEL,
            .unit = HID_SVC_BUTTON_ADC_UNIT,
            .bit_width = SOC_ADC_DIGI_MAX_BITWIDTH,
        },
    };
    adc_continuous_config_t dig_cfg = {
        .pattern_num = sizeof(adc_pattern) / sizeof(adc_pattern[0]),
        .adc_pattern = adc_pattern,
        .sample_freq_hz = HID_SVC_BUTTON_ADC_SAMPLE_FREQ_HZ,
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = HID_SVC_BUTTON_ADC_OUTPUT_FORMAT,
    };

    err = adc_continuous_config(s_button_adc_handle, &dig_cfg);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    adc_continuous_iir_filter_config_t main_filter_cfg = {
        .unit = HID_SVC_BUTTON_ADC_UNIT,
        .channel = HID_SVC_MAIN_ADC_CHANNEL,
        .coeff = HID_SVC_BUTTON_ADC_IIR_COEFF,
    };
    err = adc_new_continuous_iir_filter(s_button_adc_handle, &main_filter_cfg, &s_main_button_adc_filter);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    err = adc_continuous_iir_filter_enable(s_main_button_adc_filter);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    adc_continuous_iir_filter_config_t misc_filter_cfg = {
        .unit = HID_SVC_BUTTON_ADC_UNIT,
        .channel = HID_SVC_MISC_ADC_CHANNEL,
        .coeff = HID_SVC_BUTTON_ADC_IIR_COEFF,
    };
    err = adc_new_continuous_iir_filter(s_button_adc_handle, &misc_filter_cfg, &s_misc_button_adc_filter);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    err = adc_continuous_iir_filter_enable(s_misc_button_adc_filter);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    err = adc_continuous_start(s_button_adc_handle);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    s_button_adc_started = true;

    return ESP_OK;
}

static esp_err_t hid_service_configure_side_button_gpio(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << (uint32_t)HAL_SIDE_BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&cfg);
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

static void hid_service_poll_buttons_once(void)
{
    hid_button_t changed_buttons[HID_BUTTON_COUNT];
    hid_service_event_id_t changed_events[HID_BUTTON_COUNT];
    size_t changed_count = 0;
    bool adc_sample_ready = false;
    bool side_pressed = hid_service_read_side_button_pressed();
    uint16_t main_raw = 0;
    uint16_t misc_raw = 0;
    uint16_t main_value = 0;
    uint16_t misc_value = 0;
    uint32_t next_state;
    uint32_t changed_mask;
    esp_err_t err;

    err = hid_service_update_button_adc_raws(&main_raw, &misc_raw);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to read button ladders: %s", esp_err_to_name(err));
    }

    if (err == ESP_OK)
    {
        err = hid_service_sample_value_from_raw(&s_main_ladder, main_raw, &main_value);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to calibrate main button ladder: %s", esp_err_to_name(err));
        }
    }

    if (err == ESP_OK)
    {
        err = hid_service_sample_value_from_raw(&s_misc_ladder, misc_raw, &misc_value);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to calibrate misc button ladder: %s", esp_err_to_name(err));
        }
        else
        {
            adc_sample_ready = true;
        }
    }

    xSemaphoreTake(s_service_lock, portMAX_DELAY);

    if (adc_sample_ready)
    {
        hid_service_update_ladder_sample_locked(&s_main_ladder, main_raw, main_value);
        hid_service_update_ladder_sample_locked(&s_misc_ladder, misc_raw, misc_value);
    }

    if (adc_sample_ready && s_button_decode_enabled && !s_button_calibration_active)
    {
        (void)hid_service_update_ladder_locked(&s_main_ladder, main_value);
        (void)hid_service_update_ladder_locked(&s_misc_ladder, misc_value);
    }

    (void)hid_service_update_gpio_button_locked(&s_side_button, side_pressed);

    next_state = hid_service_build_button_state_locked();
    changed_mask = s_button_state ^ next_state;
    if (changed_mask != 0)
    {
        hid_service_set_button_state_locked(next_state);
    }

    xSemaphoreGive(s_service_lock);

    if (changed_mask == 0)
    {
        return;
    }

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
    TickType_t sample_ticks = hid_service_button_sample_ticks();
    TickType_t wake_ticks = xTaskGetTickCount();

    (void)param;

    for (;;)
    {
        hid_service_poll_buttons_once();
        vTaskDelayUntil(&wake_ticks, sample_ticks);
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
    ESP_RETURN_ON_FALSE(GPIO_IS_VALID_GPIO(HAL_SIDE_BTN_PIN), ESP_ERR_INVALID_ARG, TAG, "side button is not a valid GPIO");
    ESP_RETURN_ON_FALSE(HAL_MAIN_BTN_PIN == GPIO_NUM_1, ESP_ERR_INVALID_STATE, TAG, "main button ladder expects GPIO1");
    ESP_RETURN_ON_FALSE(HAL_MISC_BTN_PIN == GPIO_NUM_4, ESP_ERR_INVALID_STATE, TAG, "misc button ladder expects GPIO4");
    ESP_RETURN_ON_FALSE(HAL_SIDE_BTN_PIN == GPIO_NUM_5, ESP_ERR_INVALID_STATE, TAG, "side button expects GPIO5");

    esp_err_t err;

    err = hid_service_configure_side_button_gpio();
    ESP_RETURN_ON_ERROR(err, TAG, "failed to configure side button GPIO");

    err = hid_service_configure_button_adc();
    ESP_RETURN_ON_ERROR(err, TAG, "failed to configure button ADC channels");

    err = hid_service_create_button_adc_cali(&s_main_ladder);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    err = hid_service_create_button_adc_cali(&s_misc_ladder);
    if (err != ESP_OK)
    {
        hid_service_deinit_button_adc();
        return err;
    }

    s_main_ladder.last_raw = HID_SVC_BUTTON_ADC_MAX_RAW;
    s_main_ladder.last_value = HID_SVC_BUTTON_ADC_MAX_RAW;
    s_main_ladder.decode_ready = false;
    s_misc_ladder.last_raw = HID_SVC_BUTTON_ADC_MAX_RAW;
    s_misc_ladder.last_value = HID_SVC_BUTTON_ADC_MAX_RAW;
    s_misc_ladder.decode_ready = false;
    hid_service_reset_ladder_state_locked(&s_main_ladder);
    hid_service_reset_ladder_state_locked(&s_misc_ladder);
    hid_service_reset_gpio_button_state_locked(&s_side_button);
    s_button_state = 0;
    s_button_generation = 0;
    s_button_decode_enabled = false;
    s_button_calibration_active = false;

    err = hid_service_load_button_calibration();
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "no valid HID calibration found in NVS; button state changes will be ignored until `hid calib` is run");
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "failed to load HID calibration (%s); button state changes will be ignored until `hid calib` is run",
                 esp_err_to_name(err));
        return ESP_OK;
    }

    return ESP_OK;
}

esp_err_t hid_service_init(void)
{
    esp_err_t err;

    if (s_initialized)
    {
        return ESP_OK;
    }

    s_service_lock = xSemaphoreCreateMutex();
    if (s_service_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    err = hid_service_init_led_strip();
    if (err != ESP_OK)
    {
        return err;
    }

    err = hid_service_init_buttons();
    if (err != ESP_OK)
    {
        return err;
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

    err = power_mgmt_service_register_shutdown_callback(hid_service_shutdown_callback,
                                                        NULL,
                                                        HID_SVC_SHUTDOWN_PRIORITY);
    if (err != ESP_OK)
    {
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "HID service started (LED=%d MAIN=%d MISC=%d SIDE=%d)",
             HAL_LED_DATA_PIN,
             HAL_MAIN_BTN_PIN,
             HAL_MISC_BTN_PIN,
             HAL_SIDE_BTN_PIN);
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
    status_out->main_raw = s_main_ladder.last_raw;
    status_out->misc_raw = s_misc_ladder.last_raw;
    status_out->main_value = s_main_ladder.last_value;
    status_out->misc_value = s_misc_ladder.last_value;
    status_out->calibration_loaded = s_button_decode_enabled;
    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}

esp_err_t hid_service_capture_adc_average(uint32_t sample_count,
                                          uint32_t sample_period_ms,
                                          hid_service_adc_status_t *status_out)
{
    uint64_t main_raw_total = 0;
    uint64_t misc_raw_total = 0;
    uint64_t main_value_total = 0;
    uint64_t misc_value_total = 0;
    bool calibration_loaded = false;
    TickType_t sample_delay_ticks = 0;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");
    ESP_RETURN_ON_FALSE(status_out != NULL, ESP_ERR_INVALID_ARG, TAG, "ADC average output is null");
    ESP_RETURN_ON_FALSE(sample_count > 0, ESP_ERR_INVALID_ARG, TAG, "ADC average sample count must be non-zero");

    if (sample_period_ms > 0)
    {
        sample_delay_ticks = pdMS_TO_TICKS(sample_period_ms);
        if (sample_delay_ticks == 0)
        {
            sample_delay_ticks = 1;
        }
    }

    memset(status_out, 0, sizeof(*status_out));
    for (uint32_t index = 0; index < sample_count; index++)
    {
        hid_service_adc_status_t sample = {0};
        esp_err_t err = hid_service_get_adc_status(&sample);
        if (err != ESP_OK)
        {
            return err;
        }

        main_raw_total += sample.main_raw;
        misc_raw_total += sample.misc_raw;
        main_value_total += sample.main_value;
        misc_value_total += sample.misc_value;
        calibration_loaded = sample.calibration_loaded;

        if (sample_delay_ticks > 0 && (index + 1U) < sample_count)
        {
            vTaskDelay(sample_delay_ticks);
        }
    }

    status_out->main_raw = (uint16_t)(main_raw_total / sample_count);
    status_out->misc_raw = (uint16_t)(misc_raw_total / sample_count);
    status_out->main_value = (uint16_t)(main_value_total / sample_count);
    status_out->misc_value = (uint16_t)(misc_value_total / sample_count);
    status_out->calibration_loaded = calibration_loaded;
    return ESP_OK;
}

esp_err_t hid_service_set_calibration_active(bool active)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    s_button_calibration_active = active;
    hid_service_reset_button_state_locked();
    xSemaphoreGive(s_service_lock);
    return ESP_OK;
}

esp_err_t hid_service_store_calibration(const uint16_t *main_values,
                                        const uint16_t *misc_values)
{
    hid_service_button_calibration_record_t record = {
        .version = HID_SVC_BUTTON_CALIBRATION_VERSION,
        .valid = 1,
    };
    hid_service_ladder_level_t validation_levels[HID_SVC_LADDER_LEVEL_COUNT];
    uint16_t validation_thresholds[HID_SVC_LADDER_LEVEL_COUNT - 1U];
    esp_err_t err;

    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "HID service is not initialized");
    ESP_RETURN_ON_FALSE(main_values != NULL, ESP_ERR_INVALID_ARG, TAG, "main calibration values are null");
    ESP_RETURN_ON_FALSE(misc_values != NULL, ESP_ERR_INVALID_ARG, TAG, "misc calibration values are null");

    err = hid_service_prepare_ladder_calibration(main_values, validation_levels, validation_thresholds);
    if (err != ESP_OK)
    {
        return err;
    }

    err = hid_service_prepare_ladder_calibration(misc_values, validation_levels, validation_thresholds);
    if (err != ESP_OK)
    {
        return err;
    }

    memcpy(record.main_value, main_values, sizeof(record.main_value));
    memcpy(record.misc_value, misc_values, sizeof(record.misc_value));

    err = hid_service_store_button_calibration_record(&record);
    if (err != ESP_OK)
    {
        return err;
    }

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    err = hid_service_apply_button_calibration(&record);
    xSemaphoreGive(s_service_lock);
    return err;
}