/**
 * @file a2dp_source_example.c
 * @brief Example usage of the A2DP source module
 * 
 * This file demonstrates how to use the a2dp_source module to stream
 * audio data over Bluetooth A2DP to wireless speakers/headphones.
 * 
 * Note: This is an example file and is not compiled into the main project.
 */

#include "a2dp_source.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "a2dp_example";

/**
 * @brief Callback function for A2DP control events
 */
static void a2dp_event_callback(a2dp_ctrl_event_t event, void *param)
{
    switch (event) {
    case A2DP_CTRL_EVENT_CONNECTED:
        ESP_LOGI(TAG, "A2DP connected to audio sink device");
        break;
        
    case A2DP_CTRL_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "A2DP disconnected from audio sink device");
        break;
        
    case A2DP_CTRL_EVENT_AUDIO_START:
        ESP_LOGI(TAG, "Audio streaming started");
        break;
        
    case A2DP_CTRL_EVENT_AUDIO_STOP:
        ESP_LOGI(TAG, "Audio streaming stopped");
        break;
        
    case A2DP_CTRL_EVENT_AUDIO_SUSPEND:
        ESP_LOGI(TAG, "Audio streaming suspended");
        break;
        
    default:
        ESP_LOGW(TAG, "Unknown A2DP event: %d", event);
        break;
    }
}

/**
 * @brief Example: Stream audio from I2S to A2DP
 * 
 * This function reads audio data from an I2S source and streams it
 * over Bluetooth A2DP. In a real application, you would replace this
 * with your actual audio source (e.g., audio_player.c).
 */
void example_stream_i2s_to_a2dp(void)
{
    esp_err_t ret;
    
    // Initialize A2DP source
    ESP_LOGI(TAG, "Initializing A2DP source...");
    ret = a2dp_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize A2DP source: %s", esp_err_to_name(ret));
        return;
    }
    
    // Register event callback
    ret = a2dp_source_register_ctrl_cb(a2dp_event_callback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register callback: %s", esp_err_to_name(ret));
        a2dp_source_deinit();
        return;
    }
    
    ESP_LOGI(TAG, "A2DP source initialized successfully");
    ESP_LOGI(TAG, "Waiting for connection to audio sink...");
    
    // Wait a bit for connection to establish
    vTaskDelay(pdMS_TO_TICKS(5000));
    
    // Example: Stream silence (in real app, read from I2S or audio file)
    uint8_t audio_buffer[2048];
    memset(audio_buffer, 0, sizeof(audio_buffer)); // Silence
    
    size_t bytes_written = 0;
    int frame_count = 0;
    
    ESP_LOGI(TAG, "Starting audio stream...");
    
    while (1) {
        // Write audio data to A2DP
        ret = a2dp_source_write(audio_buffer, sizeof(audio_buffer), 
                                &bytes_written, 1000);
        
        if (ret == ESP_OK) {
            frame_count++;
            if (frame_count % 100 == 0) {
                ESP_LOGI(TAG, "Streamed %d frames", frame_count);
            }
        } else if (ret == ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "Write timeout, retrying...");
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(1000));
        } else {
            ESP_LOGE(TAG, "Write error: %s", esp_err_to_name(ret));
            break;
        }
        
        // Small delay to prevent tight loop
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    // Cleanup
    ESP_LOGI(TAG, "Stopping A2DP stream...");
    a2dp_source_deinit();
}

/**
 * @brief Example: Integration with existing audio player
 * 
 * This shows how to integrate A2DP with the existing audio_player module.
 * You would call a2dp_source_write() instead of or in addition to
 * i2s_channel_write() in your audio player task.
 */
void example_audio_player_integration(void)
{
    /*
     * In your audio player task (e.g., in audio_player.c),
     * after writing to I2S, also write to A2DP:
     *
     * // Write to I2S
     * size_t i2s_bytes_written = 0;
     * i2s_channel_write(g_tx_handle, pcm_buffer, buffer_size, 
     *                   &i2s_bytes_written, portMAX_DELAY);
     *
     * // Also stream via Bluetooth A2DP
     * size_t a2dp_bytes_written = 0;
     * a2dp_source_write(pcm_buffer, buffer_size, 
     *                   &a2dp_bytes_written, 0); // Non-blocking
     */
    
    ESP_LOGI(TAG, "This is a placeholder for integration example");
    ESP_LOGI(TAG, "See comments in the source code for integration details");
}

/**
 * @brief Main application entry point (for standalone example)
 */
#ifdef A2DP_STANDALONE_EXAMPLE
void app_main(void)
{
    ESP_LOGI(TAG, "A2DP Source Example Starting...");
    
    // Run the example
    example_stream_i2s_to_a2dp();
    
    // Or show integration example
    // example_audio_player_integration();
}
#endif
