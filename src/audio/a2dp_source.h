#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A2DP control event types
 */
typedef enum {
    A2DP_CTRL_EVENT_CONNECTED,          /**< A2DP connection established */
    A2DP_CTRL_EVENT_DISCONNECTED,       /**< A2DP connection lost */
    A2DP_CTRL_EVENT_AUDIO_START,        /**< Audio streaming started */
    A2DP_CTRL_EVENT_AUDIO_STOP,         /**< Audio streaming stopped */
    A2DP_CTRL_EVENT_AUDIO_SUSPEND,      /**< Audio streaming suspended */
} a2dp_ctrl_event_t;

/**
 * @brief A2DP control event callback function type
 * 
 * @param event The control event that occurred
 * @param param Optional event parameter (reserved for future use)
 */
typedef void (*a2dp_ctrl_cb_t)(a2dp_ctrl_event_t event, void *param);

/**
 * @brief Initialize the A2DP source module
 * 
 * This function initializes the Bluetooth stack, sets up A2DP source profile,
 * starts device discovery, and attempts to auto-connect to audio sink devices.
 * 
 * @return 
 *     - ESP_OK: Success
 *     - ESP_FAIL: Initialization failed
 *     - ESP_ERR_INVALID_STATE: Already initialized
 *     - ESP_ERR_NO_MEM: Memory allocation failed
 */
esp_err_t a2dp_source_init(void);

/**
 * @brief Write PCM audio data to the A2DP source
 * 
 * This function writes PCM audio data to be transmitted over A2DP.
 * The function blocks until the data is written or timeout occurs.
 * 
 * @param src Pointer to the PCM audio data buffer (16-bit stereo, 44.1kHz)
 * @param size Size of the data to write in bytes
 * @param bytes_written Pointer to store the number of bytes actually written
 * @param timeout_ms Timeout in milliseconds (0 = no wait, -1 = wait forever)
 * 
 * @return 
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_INVALID_STATE: A2DP not initialized or not connected
 *     - ESP_ERR_TIMEOUT: Operation timed out
 */
esp_err_t a2dp_source_write(const uint8_t *src, size_t size, size_t *bytes_written, uint32_t timeout_ms);

/**
 * @brief Register callback for A2DP sink control events
 * 
 * Registers a callback function that will be called when control events
 * are received from the A2DP sink (e.g., connection/disconnection, play/pause).
 * 
 * @param callback Callback function pointer (NULL to unregister)
 * 
 * @return 
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_STATE: A2DP not initialized
 */
esp_err_t a2dp_source_register_ctrl_cb(a2dp_ctrl_cb_t callback);

/**
 * @brief Deinitialize the A2DP source module
 * 
 * Stops audio streaming, disconnects from sink, and releases all resources.
 * 
 * @return 
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_STATE: Not initialized
 */
esp_err_t a2dp_source_deinit(void);

#ifdef __cplusplus
}
#endif
