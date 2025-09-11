#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/gpio.h"

// Maximum number of event listeners per button
#define MAX_LISTENERS_PER_BUTTON 8
#define MAX_BUTTONS 8

// Event types (Android SDK style)
typedef enum
{
    HID_EVENT_PRESS,
    HID_EVENT_RELEASE,
    HID_EVENT_LONG_PRESS,
    HID_EVENT_DOUBLE_PRESS,
    HID_EVENT_COMBO_START,
    HID_EVENT_COMBO_END
} hid_event_type_t;

// Button event data
typedef struct
{
    gpio_num_t gpio_num;
    hid_event_type_t event_type;
    uint32_t duration_ms; // For long press events
    uint32_t combo_mask;  // For combo events (bit mask of buttons)
} hid_event_data_t;

// Event listener callback function type
// Returns true if event was consumed (stops propagation to lower priority listeners)
typedef bool (*hid_event_listener_t)(const hid_event_data_t *event, void *user_data);

// Listener registration structure
typedef struct
{
    hid_event_listener_t callback;
    void *user_data;
    int priority; // Higher values = higher priority
    bool active;  // Can be used to temporarily disable listeners
} hid_listener_registration_t;

/**
 * @brief Initialize the HID event system
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t hid_event_system_init(void);

/**
 * @brief Deinitialize the HID event system
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t hid_event_system_deinit(void);

/**
 * @brief Register an event listener for a specific button
 * @param gpio_num GPIO number of the button
 * @param callback Callback function to handle events
 * @param user_data User data to pass to callback
 * @param priority Priority (higher = called first)
 * @return ESP_OK on success, ESP_FAIL if no space available
 */
esp_err_t hid_event_register_listener(gpio_num_t gpio_num,
                                      hid_event_listener_t callback,
                                      void *user_data,
                                      int priority);

/**
 * @brief Unregister an event listener
 * @param gpio_num GPIO number of the button
 * @param callback Callback function to remove
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t hid_event_unregister_listener(gpio_num_t gpio_num,
                                        hid_event_listener_t callback);

/**
 * @brief Enable/disable a specific listener
 * @param gpio_num GPIO number of the button
 * @param callback Callback function to enable/disable
 * @param enabled True to enable, false to disable
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not found
 */
esp_err_t hid_event_set_listener_enabled(gpio_num_t gpio_num,
                                         hid_event_listener_t callback,
                                         bool enabled);

/**
 * @brief Dispatch an event to all registered listeners
 * @param event Event data to dispatch
 * @return True if event was consumed, false otherwise
 */
bool hid_event_dispatch(const hid_event_data_t *event);
