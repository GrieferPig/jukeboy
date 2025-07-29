#pragma once

#include "esp_err.h"

// LED color structure
typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// LED event types
typedef enum
{
    LED_EVENT_SET_COLOR,
    LED_EVENT_TURN_OFF,
    LED_EVENT_START_FLASH,
    LED_EVENT_STOP_FLASH
} led_event_type_t;

// LED event structure
typedef struct
{
    led_event_type_t type;
    led_color_t color;
} led_event_t;

// Common colors
#define LED_COLOR_RED {255, 0, 0}
#define LED_COLOR_GREEN {0, 255, 0}
#define LED_COLOR_BLUE {0, 0, 255}
#define LED_COLOR_YELLOW {255, 255, 0}
#define LED_COLOR_PURPLE {255, 0, 255}
#define LED_COLOR_CYAN {0, 255, 255}
#define LED_COLOR_WHITE {255, 255, 255}
#define LED_COLOR_OFF {0, 0, 0}

/**
 * @brief Initialize the LED manager
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_mgr_init(void);

/**
 * @brief Deinitialize the LED manager
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_mgr_deinit(void);

/**
 * @brief Set LED color (non-blocking)
 * @param color The color to set
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_mgr_set_color(led_color_t color);

/**
 * @brief Turn off LED (non-blocking)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_mgr_turn_off(void);

/**
 * @brief Start flashing LED with specified color
 * @param color The color to flash
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_mgr_start_flash(led_color_t color);

/**
 * @brief Stop flashing LED
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t led_mgr_stop_flash(void);
