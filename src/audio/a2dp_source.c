#include "a2dp_source.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"

static const char *TAG = "a2dp_source";

// Module state
typedef enum {
    A2DP_STATE_UNINITIALIZED,
    A2DP_STATE_INITIALIZED,
    A2DP_STATE_DISCOVERING,
    A2DP_STATE_CONNECTING,
    A2DP_STATE_CONNECTED,
    A2DP_STATE_STREAMING,
} a2dp_state_t;

static a2dp_state_t s_a2dp_state = A2DP_STATE_UNINITIALIZED;
static a2dp_ctrl_cb_t s_ctrl_callback = NULL;

// Audio data queue
#define AUDIO_QUEUE_SIZE 10
#define AUDIO_BUFFER_SIZE 2048

typedef struct {
    uint8_t data[AUDIO_BUFFER_SIZE];
    size_t size;
} audio_buffer_t;

static QueueHandle_t s_audio_queue = NULL;
static SemaphoreHandle_t s_write_semaphore = NULL;
static TaskHandle_t s_a2dp_task_handle = NULL;

// Discovered device
static esp_bd_addr_t s_peer_bda = {0};
static bool s_peer_found = false;

// Forward declarations
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);
static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len);
static void a2dp_task(void *pvParameters);

/**
 * @brief Helper function to notify control callback
 */
static void notify_ctrl_event(a2dp_ctrl_event_t event, void *param)
{
    if (s_ctrl_callback) {
        s_ctrl_callback(event, param);
    }
}

/**
 * @brief GAP callback function for device discovery
 */
static void bt_app_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        ESP_LOGI(TAG, "Device found: %02x:%02x:%02x:%02x:%02x:%02x",
                 param->disc_res.bda[0], param->disc_res.bda[1],
                 param->disc_res.bda[2], param->disc_res.bda[3],
                 param->disc_res.bda[4], param->disc_res.bda[5]);
        
        // Check if device supports audio sink by examining COD (Class of Device)
        // Major service class: Audio (bit 21)
        // Major device class: Audio/Video (0x04)
        uint32_t cod = param->disc_res.cod;
        bool is_audio_device = false;
        
        // Check if it's an audio device
        if (((cod >> 8) & 0x1F) == 0x04) {  // Major device class is Audio/Video
            ESP_LOGI(TAG, "Found audio device");
            is_audio_device = true;
        }
        
        // Also check for audio service in EIR if available
        if (param->disc_res.num_uuids > 0 && param->disc_res.eid_uuid.uuid.len == ESP_UUID_LEN_16) {
            if (param->disc_res.eid_uuid.uuid.uuid16 == 0x110B) {  // Audio Sink
                ESP_LOGI(TAG, "Found audio sink in EIR");
                is_audio_device = true;
            }
        }
        
        if (is_audio_device && !s_peer_found) {
            ESP_LOGI(TAG, "Selecting this device for A2DP connection");
            memcpy(s_peer_bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
            s_peer_found = true;
            // Stop discovery
            esp_bt_gap_cancel_discovery();
        }
        break;
        
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Discovery stopped");
            if (s_peer_found) {
                // Connect to the found device
                ESP_LOGI(TAG, "Connecting to peer device...");
                s_a2dp_state = A2DP_STATE_CONNECTING;
                esp_a2d_source_connect(s_peer_bda);
            } else {
                ESP_LOGW(TAG, "No audio sink device found, retrying...");
                // Retry discovery after a delay
                vTaskDelay(pdMS_TO_TICKS(2000));
                s_a2dp_state = A2DP_STATE_DISCOVERING;
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        } else if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED) {
            ESP_LOGI(TAG, "Discovery started");
        }
        break;
        
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Authentication success: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGE(TAG, "Authentication failed, status: %d", param->auth_cmpl.stat);
        }
        break;
        
    default:
        ESP_LOGD(TAG, "GAP event: %d", event);
        break;
    }
}

/**
 * @brief A2DP callback function
 */
static void bt_app_a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGI(TAG, "A2DP connected");
            s_a2dp_state = A2DP_STATE_CONNECTED;
            notify_ctrl_event(A2DP_CTRL_EVENT_CONNECTED, NULL);
        } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_LOGI(TAG, "A2DP disconnected");
            s_a2dp_state = A2DP_STATE_INITIALIZED;
            s_peer_found = false;
            notify_ctrl_event(A2DP_CTRL_EVENT_DISCONNECTED, NULL);
            // Try to reconnect
            vTaskDelay(pdMS_TO_TICKS(1000));
            s_a2dp_state = A2DP_STATE_DISCOVERING;
            esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
        }
        break;
        
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_LOGI(TAG, "Audio started");
            s_a2dp_state = A2DP_STATE_STREAMING;
            notify_ctrl_event(A2DP_CTRL_EVENT_AUDIO_START, NULL);
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND) {
            ESP_LOGI(TAG, "Audio suspended");
            notify_ctrl_event(A2DP_CTRL_EVENT_AUDIO_SUSPEND, NULL);
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED) {
            ESP_LOGI(TAG, "Audio stopped");
            notify_ctrl_event(A2DP_CTRL_EVENT_AUDIO_STOP, NULL);
        }
        break;
        
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "A2DP audio config, codec type: %d", param->audio_cfg.mcc.type);
        // For now, assume SBC codec
        break;
        
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        ESP_LOGD(TAG, "Media control ACK: cmd=%d, status=%d", 
                 param->media_ctrl_stat.cmd, param->media_ctrl_stat.status);
        break;
        
    default:
        ESP_LOGD(TAG, "A2DP event: %d", event);
        break;
    }
}

/**
 * @brief A2DP data callback - this is where we send audio data
 */
static void bt_app_a2d_data_cb(const uint8_t *data, uint32_t len)
{
    // This callback is used by A2DP source to request more data
    // We don't need to implement it as we're using a2dp_source_write instead
}

/**
 * @brief Task to handle A2DP operations
 * 
 * This task receives PCM audio data from the queue and manages A2DP streaming.
 * 
 * TODO: For production use, implement SBC encoding here:
 * 1. Receive PCM audio from queue (buffer.data, buffer.size)
 * 2. Encode PCM to SBC format using an SBC encoder library
 * 3. Send encoded SBC data using esp_a2d_media_ctrl() or appropriate API
 * 
 * Example SBC encoding flow:
 *   sbc_encoder_encode(buffer.data, buffer.size, sbc_buffer, &sbc_size);
 *   esp_a2d_media_data_send(sbc_buffer, sbc_size);
 * 
 * The module provides the complete framework (init, discovery, connection, queuing)
 * and this is the integration point for the audio codec.
 */
static void a2dp_task(void *pvParameters)
{
    audio_buffer_t buffer;
    
    while (1) {
        // Wait for audio data
        if (xQueueReceive(s_audio_queue, &buffer, portMAX_DELAY) == pdTRUE) {
            // Only send if we're in streaming state
            if (s_a2dp_state == A2DP_STATE_STREAMING) {
                // TODO: Implement SBC encoding and transmission here
                // For now, we acknowledge the write operation
                ESP_LOGD(TAG, "Received %d bytes of PCM audio data for transmission", buffer.size);
                
                // In production, you would:
                // 1. Encode buffer.data (PCM) to SBC format
                // 2. Call ESP-IDF API to send SBC data to A2DP sink
                // 3. Then signal write completion
                
                // Signal that write is complete
                xSemaphoreGive(s_write_semaphore);
            } else if (s_a2dp_state == A2DP_STATE_CONNECTED) {
                // Start audio streaming
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
                // Put data back in queue
                xQueueSendToFront(s_audio_queue, &buffer, 0);
            } else {
                // Not connected, signal write complete but data is dropped
                ESP_LOGW(TAG, "Dropping audio data - not connected");
                xSemaphoreGive(s_write_semaphore);
            }
        }
    }
}

esp_err_t a2dp_source_init(void)
{
    if (s_a2dp_state != A2DP_STATE_UNINITIALIZED) {
        ESP_LOGW(TAG, "A2DP source already initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Initializing A2DP source");
    
    // Create audio queue
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_SIZE, sizeof(audio_buffer_t));
    if (s_audio_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create audio queue");
        return ESP_ERR_NO_MEM;
    }
    
    // Create write semaphore
    s_write_semaphore = xSemaphoreCreateBinary();
    if (s_write_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create write semaphore");
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize Bluetooth controller
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    
    ret = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        esp_bt_controller_deinit();
        goto fail;
    }
    
    // Initialize Bluedroid stack
    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        goto fail;
    }
    
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        goto fail;
    }
    
    // Set device name
    esp_bt_dev_set_device_name("JukeBoy-A2DP");
    
    // Initialize A2DP source
    ret = esp_a2d_register_callback(bt_app_a2d_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register A2DP callback: %s", esp_err_to_name(ret));
        goto fail_bluedroid;
    }
    
    ret = esp_a2d_source_register_data_callback(bt_app_a2d_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register A2DP data callback: %s", esp_err_to_name(ret));
        goto fail_bluedroid;
    }
    
    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(ret));
        goto fail_bluedroid;
    }
    
    // Set discoverable and connectable
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    
    // Register GAP callback
    ret = esp_bt_gap_register_callback(bt_app_gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register GAP callback: %s", esp_err_to_name(ret));
        goto fail_a2dp;
    }
    
    // Create A2DP task
    if (xTaskCreate(a2dp_task, "a2dp_task", 4096, NULL, 5, &s_a2dp_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create A2DP task");
        ret = ESP_ERR_NO_MEM;
        goto fail_a2dp;
    }
    
    s_a2dp_state = A2DP_STATE_INITIALIZED;
    
    // Start device discovery
    ESP_LOGI(TAG, "Starting device discovery");
    s_a2dp_state = A2DP_STATE_DISCOVERING;
    ret = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Start discovery failed: %s", esp_err_to_name(ret));
        // Continue anyway, we can retry later
    }
    
    ESP_LOGI(TAG, "A2DP source initialized successfully");
    return ESP_OK;
    
fail_a2dp:
    esp_a2d_source_deinit();
fail_bluedroid:
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
fail:
    if (s_write_semaphore) {
        vSemaphoreDelete(s_write_semaphore);
        s_write_semaphore = NULL;
    }
    if (s_audio_queue) {
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }
    return ret;
}

esp_err_t a2dp_source_write(const uint8_t *src, size_t size, size_t *bytes_written, uint32_t timeout_ms)
{
    if (s_a2dp_state == A2DP_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "A2DP source not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    if (src == NULL || bytes_written == NULL) {
        ESP_LOGE(TAG, "Invalid arguments");
        return ESP_ERR_INVALID_ARG;
    }
    
    if (size > AUDIO_BUFFER_SIZE) {
        ESP_LOGW(TAG, "Data size %d exceeds buffer size %d, will be truncated", size, AUDIO_BUFFER_SIZE);
        size = AUDIO_BUFFER_SIZE;
    }
    
    // Prepare audio buffer
    audio_buffer_t buffer;
    memcpy(buffer.data, src, size);
    buffer.size = size;
    
    // Send to queue
    TickType_t ticks_to_wait = (timeout_ms == (uint32_t)-1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    
    if (xQueueSend(s_audio_queue, &buffer, ticks_to_wait) != pdTRUE) {
        ESP_LOGW(TAG, "Audio queue full, data dropped");
        *bytes_written = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    // Wait for write to complete
    if (xSemaphoreTake(s_write_semaphore, ticks_to_wait) != pdTRUE) {
        ESP_LOGW(TAG, "Write timeout");
        *bytes_written = 0;
        return ESP_ERR_TIMEOUT;
    }
    
    *bytes_written = size;
    return ESP_OK;
}

esp_err_t a2dp_source_register_ctrl_cb(a2dp_ctrl_cb_t callback)
{
    if (s_a2dp_state == A2DP_STATE_UNINITIALIZED) {
        ESP_LOGE(TAG, "A2DP source not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    
    s_ctrl_callback = callback;
    ESP_LOGI(TAG, "Control callback %s", callback ? "registered" : "unregistered");
    return ESP_OK;
}

esp_err_t a2dp_source_deinit(void)
{
    if (s_a2dp_state == A2DP_STATE_UNINITIALIZED) {
        return ESP_ERR_INVALID_STATE;
    }
    
    ESP_LOGI(TAG, "Deinitializing A2DP source");
    
    // Stop audio if streaming
    if (s_a2dp_state == A2DP_STATE_STREAMING) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_STOP);
    }
    
    // Disconnect if connected
    if (s_a2dp_state >= A2DP_STATE_CONNECTED) {
        esp_a2d_source_disconnect(s_peer_bda);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    // Delete task
    if (s_a2dp_task_handle) {
        vTaskDelete(s_a2dp_task_handle);
        s_a2dp_task_handle = NULL;
    }
    
    // Cleanup A2DP
    esp_a2d_source_deinit();
    
    // Cleanup Bluetooth stack
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    
    // Cleanup queues and semaphores
    if (s_write_semaphore) {
        vSemaphoreDelete(s_write_semaphore);
        s_write_semaphore = NULL;
    }
    if (s_audio_queue) {
        vQueueDelete(s_audio_queue);
        s_audio_queue = NULL;
    }
    
    s_ctrl_callback = NULL;
    s_peer_found = false;
    s_a2dp_state = A2DP_STATE_UNINITIALIZED;
    
    ESP_LOGI(TAG, "A2DP source deinitialized");
    return ESP_OK;
}
