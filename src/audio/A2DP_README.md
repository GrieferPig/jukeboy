# A2DP Source Module

This module provides a Bluetooth A2DP (Advanced Audio Distribution Profile) source implementation for ESP32, allowing the device to stream audio to Bluetooth speakers and headphones.

## Features

- Automatic device discovery and connection to A2DP audio sinks
- PCM audio streaming (16-bit stereo, 44.1kHz)
- Event callbacks for connection and playback control events
- Task-based architecture using FreeRTOS
- API similar to ESP-IDF I2S interface for easy integration

## API Functions

### `a2dp_source_init()`
Initializes the Bluetooth stack, A2DP source profile, and starts device discovery. Automatically connects to the first audio sink device found.

**Returns:**
- `ESP_OK`: Success
- `ESP_FAIL`: Initialization failed
- `ESP_ERR_INVALID_STATE`: Already initialized
- `ESP_ERR_NO_MEM`: Memory allocation failed

### `a2dp_source_write(src, size, bytes_written, timeout_ms)`
Writes PCM audio data to be transmitted over A2DP. Similar to `i2s_channel_write()`.

**Parameters:**
- `src`: Pointer to PCM audio data buffer (16-bit stereo, 44.1kHz)
- `size`: Size of data to write in bytes
- `bytes_written`: Pointer to store number of bytes actually written
- `timeout_ms`: Timeout in milliseconds (0 = no wait, -1 = wait forever)

**Returns:**
- `ESP_OK`: Success
- `ESP_ERR_INVALID_ARG`: Invalid arguments
- `ESP_ERR_INVALID_STATE`: Not initialized or not connected
- `ESP_ERR_TIMEOUT`: Operation timed out

### `a2dp_source_register_ctrl_cb(callback)`
Registers a callback function for A2DP control events.

**Parameters:**
- `callback`: Callback function pointer (NULL to unregister)

**Returns:**
- `ESP_OK`: Success
- `ESP_ERR_INVALID_STATE`: Not initialized

**Control Events:**
- `A2DP_CTRL_EVENT_CONNECTED`: Connection established
- `A2DP_CTRL_EVENT_DISCONNECTED`: Connection lost
- `A2DP_CTRL_EVENT_AUDIO_START`: Streaming started
- `A2DP_CTRL_EVENT_AUDIO_STOP`: Streaming stopped
- `A2DP_CTRL_EVENT_AUDIO_SUSPEND`: Streaming suspended

## Configuration Requirements

To use this module, the following configuration options must be enabled in `sdkconfig`:

```
CONFIG_BT_ENABLED=y
CONFIG_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
CONFIG_BT_SPP_ENABLED=n  # Optional, not needed for A2DP
```

You can enable these options using `idf.py menuconfig`:

1. Navigate to: Component config → Bluetooth → Bluedroid Enable
2. Enable: [*] Bluetooth → [*] Bluedroid Bluetooth stack enabled
3. Enable: [*] Classic Bluetooth
4. Enable: [*] A2DP

## Usage Example

```c
#include "audio/a2dp_source.h"

// Callback function for A2DP events
void my_a2dp_callback(a2dp_ctrl_event_t event, void *param) {
    switch (event) {
        case A2DP_CTRL_EVENT_CONNECTED:
            ESP_LOGI("APP", "A2DP Connected");
            break;
        case A2DP_CTRL_EVENT_DISCONNECTED:
            ESP_LOGI("APP", "A2DP Disconnected");
            break;
        case A2DP_CTRL_EVENT_AUDIO_START:
            ESP_LOGI("APP", "Audio streaming started");
            break;
        default:
            break;
    }
}

void app_main(void) {
    // Initialize A2DP source
    esp_err_t ret = a2dp_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE("APP", "A2DP init failed");
        return;
    }
    
    // Register callback
    a2dp_source_register_ctrl_cb(my_a2dp_callback);
    
    // Write audio data (example: silence)
    uint8_t audio_buffer[2048] = {0};
    size_t bytes_written = 0;
    
    while (1) {
        ret = a2dp_source_write(audio_buffer, sizeof(audio_buffer), 
                                &bytes_written, 1000);
        if (ret == ESP_OK) {
            // Successfully sent audio data
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
```

## Implementation Notes

- The module uses FreeRTOS queues for audio data buffering
- Device discovery runs automatically and retries if no devices are found
- Auto-reconnection is implemented when connection is lost
- The module currently supports SBC codec (default for A2DP)
- Audio data is queued and sent asynchronously via a dedicated task

## Limitations and Future Work

- Currently implements basic device discovery (connects to first audio sink found)
- No device filtering or selection mechanism
- **SBC encoding needs to be implemented for actual audio transmission** - The module currently provides the framework and queues audio data, but the actual SBC encoding and transmission to the A2DP sink needs to be added for production use
- Maximum audio buffer size is 2048 bytes per write
- The module provides all the infrastructure (initialization, discovery, connection, queuing, callbacks) but requires SBC codec integration for actual audio playback

## Future Enhancements

- Implement proper SBC audio encoding
- Add device filtering and manual device selection
- Support for AAC codec
- Better error recovery and retry logic
- Connection management (disconnect, reconnect to specific device)
