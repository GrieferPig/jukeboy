# Integrating A2DP Source with Audio Player

## Overview

This guide shows how to integrate the A2DP source module with the existing audio player to enable Bluetooth audio streaming alongside or instead of I2S output.

## Integration Approaches

### Approach 1: Dual Output (I2S + Bluetooth)

Stream audio to both I2S (wired speakers) and Bluetooth simultaneously.

```c
// In audio_player.c, modify the playback task

#include "audio/a2dp_source.h"

// In audio_player_init()
void audio_player_init() {
    // ... existing I2S initialization ...
    
    // Initialize A2DP source
    esp_err_t ret = a2dp_source_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "A2DP init failed, continuing with I2S only");
    } else {
        ESP_LOGI(TAG, "A2DP source initialized");
    }
    
    // ... rest of initialization ...
}

// In your audio playback loop
static void playback_task(void *pvParameters) {
    uint8_t pcm_buffer[2048];
    size_t bytes_read = 0;
    size_t i2s_written = 0;
    size_t a2dp_written = 0;
    
    while (1) {
        // Decode audio to PCM buffer
        bytes_read = decode_audio_chunk(pcm_buffer, sizeof(pcm_buffer));
        
        if (bytes_read > 0) {
            // Write to I2S (blocking)
            i2s_channel_write(g_tx_handle, pcm_buffer, bytes_read, 
                            &i2s_written, portMAX_DELAY);
            
            // Also write to A2DP (non-blocking to avoid delay)
            a2dp_source_write(pcm_buffer, bytes_read, &a2dp_written, 0);
        }
    }
}
```

### Approach 2: Switchable Output

Allow user to switch between I2S and Bluetooth output.

```c
typedef enum {
    AUDIO_OUTPUT_I2S,
    AUDIO_OUTPUT_A2DP,
    AUDIO_OUTPUT_BOTH
} audio_output_mode_t;

static audio_output_mode_t g_output_mode = AUDIO_OUTPUT_I2S;

void set_audio_output_mode(audio_output_mode_t mode) {
    g_output_mode = mode;
    ESP_LOGI(TAG, "Audio output mode set to %d", mode);
}

static void playback_task(void *pvParameters) {
    uint8_t pcm_buffer[2048];
    size_t bytes_read = 0;
    size_t written = 0;
    
    while (1) {
        bytes_read = decode_audio_chunk(pcm_buffer, sizeof(pcm_buffer));
        
        if (bytes_read > 0) {
            switch (g_output_mode) {
            case AUDIO_OUTPUT_I2S:
                i2s_channel_write(g_tx_handle, pcm_buffer, bytes_read, 
                                &written, portMAX_DELAY);
                break;
                
            case AUDIO_OUTPUT_A2DP:
                a2dp_source_write(pcm_buffer, bytes_read, &written, 
                                portMAX_DELAY);
                break;
                
            case AUDIO_OUTPUT_BOTH:
                i2s_channel_write(g_tx_handle, pcm_buffer, bytes_read, 
                                &written, portMAX_DELAY);
                a2dp_source_write(pcm_buffer, bytes_read, &written, 0);
                break;
            }
        }
    }
}
```

### Approach 3: Event-Driven Output Selection

Automatically switch to Bluetooth when connected, fall back to I2S when disconnected.

```c
static bool g_a2dp_connected = false;

void a2dp_connection_callback(a2dp_ctrl_event_t event, void *param) {
    switch (event) {
    case A2DP_CTRL_EVENT_CONNECTED:
        ESP_LOGI(TAG, "A2DP connected - switching to Bluetooth output");
        g_a2dp_connected = true;
        
        // Optional: Pause I2S to save power
        i2s_channel_disable(g_tx_handle);
        break;
        
    case A2DP_CTRL_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "A2DP disconnected - switching to I2S output");
        g_a2dp_connected = false;
        
        // Resume I2S
        i2s_channel_enable(g_tx_handle);
        break;
        
    default:
        break;
    }
}

void audio_player_init() {
    // ... I2S init ...
    
    // Initialize A2DP with callback
    a2dp_source_init();
    a2dp_source_register_ctrl_cb(a2dp_connection_callback);
}

static void playback_task(void *pvParameters) {
    uint8_t pcm_buffer[2048];
    size_t bytes_read = 0;
    size_t written = 0;
    
    while (1) {
        bytes_read = decode_audio_chunk(pcm_buffer, sizeof(pcm_buffer));
        
        if (bytes_read > 0) {
            if (g_a2dp_connected) {
                // Stream via Bluetooth
                a2dp_source_write(pcm_buffer, bytes_read, &written, 
                                portMAX_DELAY);
            } else {
                // Stream via I2S
                i2s_channel_write(g_tx_handle, pcm_buffer, bytes_read, 
                                &written, portMAX_DELAY);
            }
        }
    }
}
```

## Handling Audio Format

The A2DP module expects 16-bit stereo PCM at 44.1kHz. Ensure your audio matches this format:

```c
// Constants should match A2DP requirements
#define SAMPLE_RATE 44100
#define CHANNELS 2
#define BITS_PER_SAMPLE 16
#define BYTES_PER_FRAME (CHANNELS * BITS_PER_SAMPLE / 8)  // = 4

// Verify format before writing
void verify_audio_format(void) {
    assert(SAMPLE_RATE == 44100);
    assert(CHANNELS == 2);
    assert(BITS_PER_SAMPLE == 16);
}
```

## Buffer Management

Consider using a shared buffer architecture:

```c
#define AUDIO_BUFFER_COUNT 4
#define AUDIO_BUFFER_SIZE 2048

typedef struct {
    uint8_t data[AUDIO_BUFFER_SIZE];
    size_t size;
    bool in_use;
} audio_buffer_t;

static audio_buffer_t g_audio_buffers[AUDIO_BUFFER_COUNT];
static QueueHandle_t g_free_buffers;
static QueueHandle_t g_full_buffers;

void init_buffer_pool(void) {
    g_free_buffers = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_buffer_t*));
    g_full_buffers = xQueueCreate(AUDIO_BUFFER_COUNT, sizeof(audio_buffer_t*));
    
    for (int i = 0; i < AUDIO_BUFFER_COUNT; i++) {
        audio_buffer_t *buf = &g_audio_buffers[i];
        buf->in_use = false;
        xQueueSend(g_free_buffers, &buf, 0);
    }
}

// Decoder task fills buffers
void decoder_task(void *pvParameters) {
    audio_buffer_t *buf;
    while (1) {
        // Get free buffer
        if (xQueueReceive(g_free_buffers, &buf, portMAX_DELAY)) {
            // Decode into buffer
            buf->size = decode_audio(buf->data, AUDIO_BUFFER_SIZE);
            // Send to output queues
            xQueueSend(g_full_buffers, &buf, portMAX_DELAY);
        }
    }
}

// Output tasks consume buffers
void output_task(void *pvParameters) {
    audio_buffer_t *buf;
    size_t written;
    
    while (1) {
        if (xQueueReceive(g_full_buffers, &buf, portMAX_DELAY)) {
            // Write to I2S
            i2s_channel_write(g_tx_handle, buf->data, buf->size, 
                            &written, portMAX_DELAY);
            
            // Write to A2DP
            a2dp_source_write(buf->data, buf->size, &written, 0);
            
            // Return buffer to pool
            xQueueSend(g_free_buffers, &buf, 0);
        }
    }
}
```

## Error Handling

Implement robust error handling for A2DP operations:

```c
esp_err_t write_audio_with_retry(const uint8_t *data, size_t size) {
    size_t written = 0;
    int retry_count = 0;
    const int MAX_RETRIES = 3;
    
    while (retry_count < MAX_RETRIES) {
        esp_err_t ret = a2dp_source_write(data, size, &written, 1000);
        
        if (ret == ESP_OK) {
            return ESP_OK;
        } else if (ret == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "A2DP not connected, data dropped");
            return ret;
        } else if (ret == ESP_ERR_TIMEOUT) {
            retry_count++;
            ESP_LOGW(TAG, "A2DP write timeout, retry %d/%d", 
                     retry_count, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            ESP_LOGE(TAG, "A2DP write error: %d", ret);
            return ret;
        }
    }
    
    return ESP_ERR_TIMEOUT;
}
```

## Performance Considerations

### Memory Usage

- A2DP adds ~80KB heap usage for Bluetooth stack
- Each queued audio buffer uses 2KB + overhead
- Consider reducing buffer count if memory is tight

### CPU Usage

- A2DP encoding (SBC) adds CPU overhead
- Run A2DP task on separate core if possible:
  ```c
  xTaskCreatePinnedToCore(a2dp_task, "a2dp", 4096, NULL, 5, 
                          &task_handle, 1); // Core 1
  ```

### Latency

- I2S has lower latency (~20ms)
- Bluetooth A2DP has higher latency (~100-200ms)
- For synchronized output, delay I2S by A2DP latency:
  ```c
  vTaskDelay(pdMS_TO_TICKS(150)); // Delay I2S
  ```

## Testing Integration

1. **Test I2S Only**:
   ```c
   // Disable A2DP
   // set_audio_output_mode(AUDIO_OUTPUT_I2S);
   ```

2. **Test A2DP Only**:
   ```c
   set_audio_output_mode(AUDIO_OUTPUT_A2DP);
   ```

3. **Test Switching**:
   - Play audio via I2S
   - Connect Bluetooth device
   - Verify automatic switch to A2DP
   - Disconnect Bluetooth
   - Verify switch back to I2S

4. **Test Concurrent Output**:
   - Connect both wired and Bluetooth speakers
   - Verify audio plays on both
   - Check for synchronization issues

## Troubleshooting

**Audio Stuttering on Bluetooth**:
- Increase A2DP task priority
- Increase audio buffer size
- Reduce other task priorities

**No Audio on Bluetooth**:
- Verify A2DP connection callback fires
- Check `a2dp_source_write` return value
- Ensure audio format is correct (44.1kHz, stereo, 16-bit)

**High CPU Usage**:
- Profile using `uxTaskGetSystemState()`
- Consider reducing sample rate (if supported)
- Optimize audio decoding

## Example: Complete Integration

See `src/audio/a2dp_source_example.c` for a complete working example.

## Next Steps

1. Implement your chosen integration approach
2. Test thoroughly with various Bluetooth devices
3. Profile performance and optimize if needed
4. Add user controls for output selection
5. Implement proper error recovery
