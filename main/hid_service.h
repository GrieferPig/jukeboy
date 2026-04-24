#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(HID_SERVICE_EVENT);

    typedef enum
    {
        /* Buttons are ordered from highest sensed ladder voltage to lowest:
         * *_1 = 4.7k tap, *_2 = 2.2k tap, *_3 = 1k tap.
         */
        HID_BUTTON_MAIN_1 = 0,
        HID_BUTTON_MAIN_2,
        HID_BUTTON_MAIN_3,
        HID_BUTTON_MISC_1,
        HID_BUTTON_MISC_2,
        HID_BUTTON_MISC_3,
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
    } hid_service_adc_status_t;

    esp_err_t hid_service_init(void);
    esp_err_t hid_service_led_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
    esp_err_t hid_service_led_set_brightness(uint8_t percent);
    esp_err_t hid_service_led_off(void);
    esp_err_t hid_service_get_button_state(uint32_t *button_state_out);
    esp_err_t hid_service_get_adc_status(hid_service_adc_status_t *status_out);

#ifdef __cplusplus
}
#endif