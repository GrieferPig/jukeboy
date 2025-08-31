#pragma once

#include <stdint.h>
#include "esp_err.h"

/**
 * @brief Initialize the HID manager and start the button monitoring task
 *
 * This function sets up GPIO interrupts for buttons BTN1-BTN6 and starts
 * a task to handle button press events. The buttons are mapped as follows:
 * - BTN1: Previous track
 * - BTN2: Pause/Unpausef
 * - BTN3: Next track
 * - BTN4: Toggle shuffle
 * - BTN5: Volume down
 * - BTN6: Volume up
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
