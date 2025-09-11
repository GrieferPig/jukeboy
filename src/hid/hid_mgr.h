#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "hid_event_system.h"

// Button configuration
#define HID_LONG_PRESS_DURATION_MS 1000
#define HID_DOUBLE_PRESS_WINDOW_MS 500
#define HID_RESTART_COMBO_DURATION_MS 3000

/**
 * @brief Initialize the HID manager
 *
 * Sets up GPIO interrupts for configured buttons and starts the HID task.
 * Uses the HID event system for decoupled event handling.
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t hid_mgr_init(void);

/**
 * @brief Stop the HID manager and clean up resources
 *
 * @return ESP_OK on success, error code on failure
 */
esp_err_t hid_mgr_deinit(void);
