#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#define HID_SERVICE_BUTTON_LADDER_STATE_COUNT 8U

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(HID_SERVICE_EVENT);

    typedef enum
    {
        /* Button IDs map to the ladder tap resistors:
         * *_1 = 4.7k tap, *_2 = 2.2k tap, *_3 = 1k tap.
         * SIDE is a discrete active-low GPIO button.
         */
        HID_BUTTON_MAIN_1 = 0,
        HID_BUTTON_MAIN_2,
        HID_BUTTON_MAIN_3,
        HID_BUTTON_MISC_1,
        HID_BUTTON_MISC_2,
        HID_BUTTON_MISC_3,
        HID_BUTTON_SIDE,
        HID_BUTTON_COUNT,
    } hid_button_t;

    typedef enum
    {
        HID_EVENT_BUTTON_DOWN = 0,
        HID_EVENT_BUTTON_UP,
    } hid_service_event_id_t;

    typedef struct
    {
        uint16_t main_raw;
        uint16_t misc_raw;
        uint16_t main_value;
        uint16_t misc_value;
        bool calibration_loaded;
    } hid_service_adc_status_t;

    esp_err_t hid_service_init(void);
    esp_err_t hid_service_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
    esp_err_t hid_service_led_set_brightness(uint8_t percent);
    esp_err_t hid_service_led_off(void);
    esp_err_t hid_service_get_button_state(uint32_t *button_state_out);
    esp_err_t hid_service_get_adc_status(hid_service_adc_status_t *status_out);
    esp_err_t hid_service_capture_adc_average(uint32_t sample_count,
                                              uint32_t sample_period_ms,
                                              hid_service_adc_status_t *status_out);
    esp_err_t hid_service_set_calibration_active(bool active);
    esp_err_t hid_service_store_calibration(const uint16_t *main_values,
                                            const uint16_t *misc_values);

#ifdef __cplusplus
}
#endif