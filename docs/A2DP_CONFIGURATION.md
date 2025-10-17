# A2DP Source Module - Configuration Guide

## Overview

The A2DP source module requires Bluetooth Classic to be enabled in the ESP-IDF configuration. This document explains how to configure the project to use the A2DP module.

## Required Configuration Changes

### Method 1: Using menuconfig (Recommended)

1. Run the configuration tool:
   ```bash
   idf.py menuconfig
   ```
   or
   ```bash
   pio run -t menuconfig
   ```

2. Navigate through the menu:
   ```
   Component config → Bluetooth → [*] Bluetooth
                                  → Bluedroid Options
                                      → [*] Classic Bluetooth
                                      → [*] A2DP
   ```

3. Save and exit

### Method 2: Manual sdkconfig Edit

Add or modify the following lines in your `sdkconfig` file:

```ini
# Enable Bluetooth
CONFIG_BT_ENABLED=y

# Enable Bluedroid stack
CONFIG_BLUEDROID_ENABLED=y

# Enable Classic Bluetooth
CONFIG_BT_CLASSIC_ENABLED=y

# Enable A2DP profile
CONFIG_BT_A2DP_ENABLE=y

# Optional: Configure Bluetooth device name
CONFIG_BT_BLUEDROID_PINNED_TO_CORE_0=y
CONFIG_BT_BLUEDROID_PINNED_TO_CORE=0

# Optional: Adjust task priorities if needed
CONFIG_BT_TASK_PRIORITY=19
```

### Method 3: Using sdkconfig.defaults

Create or modify `sdkconfig.defaults` file in the project root:

```ini
# Bluetooth Configuration
CONFIG_BT_ENABLED=y
CONFIG_BLUEDROID_ENABLED=y
CONFIG_BT_CLASSIC_ENABLED=y
CONFIG_BT_A2DP_ENABLE=y
```

## Memory Considerations

Enabling Bluetooth requires significant memory. Ensure your partition table allocates enough space:

- **Minimum Flash Size**: 4MB recommended
- **Minimum Free Heap**: ~80KB for Bluetooth stack
- **Stack Sizes**: Default Bluetooth stack sizes are adequate for A2DP

## Build System Integration

The A2DP source module is automatically included in the build when:

1. The source files are in `src/audio/`
2. The `CMakeLists.txt` uses `GLOB_RECURSE` to include all `.c` files
3. Bluetooth is enabled in sdkconfig

### Verification

To verify Bluetooth is enabled, check your build output:

```bash
idf.py build 2>&1 | grep -i bluetooth
```

You should see Bluetooth components being compiled.

## Common Issues

### Issue 1: Bluetooth Controller Init Failed

**Error**: `Bluetooth controller init failed`

**Solution**: 
- Ensure ESP32 chip variant supports Bluetooth Classic (ESP32, not ESP32-C3/S2/S3)
- Check that `CONFIG_BT_ENABLED=y` is set
- Verify sufficient memory is available

### Issue 2: A2DP Init Failed

**Error**: `A2DP source init failed`

**Solution**:
- Ensure `CONFIG_BT_A2DP_ENABLE=y` is set
- Check that Classic Bluetooth is enabled (`CONFIG_BT_CLASSIC_ENABLED=y`)
- Verify Bluedroid stack is initialized properly

### Issue 3: No Audio Sink Found

**Error**: `No audio sink device found, retrying...`

**Solution**:
- Ensure Bluetooth speaker/headphone is in pairing mode
- Check that device is not already paired with another device
- Verify ESP32 Bluetooth is working by using `esp_bt_gap_start_discovery()`

### Issue 4: Compilation Errors

**Error**: `esp_a2dp_api.h: No such file or directory`

**Solution**:
- Ensure Bluetooth components are enabled in menuconfig
- Clean and rebuild: `idf.py fullclean && idf.py build`
- Verify ESP-IDF version supports A2DP (v4.0+)

## Platform Compatibility

- ✅ **ESP32**: Full support (Bluetooth Classic + A2DP)
- ❌ **ESP32-S2**: No Bluetooth support
- ❌ **ESP32-S3**: Bluetooth LE only, no Classic BT/A2DP
- ❌ **ESP32-C3**: Bluetooth LE only, no Classic BT/A2DP
- ❌ **ESP32-C6**: Bluetooth LE only, no Classic BT/A2DP

**Note**: A2DP requires Bluetooth Classic, which is only available on the original ESP32 chip.

## Testing the Module

### Basic Test

```c
#include "audio/a2dp_source.h"

void app_main(void) {
    // Initialize
    esp_err_t ret = a2dp_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE("TEST", "Init failed: %d", ret);
        return;
    }
    
    // Wait for connection
    vTaskDelay(pdMS_TO_TICKS(10000));
    
    // Send test audio (silence)
    uint8_t audio[2048] = {0};
    size_t written = 0;
    ret = a2dp_source_write(audio, sizeof(audio), &written, 1000);
    ESP_LOGI("TEST", "Write result: %d, written: %d", ret, written);
}
```

### Monitor Output

Expected log output when successful:

```
I (xxx) a2dp_source: Initializing A2DP source
I (xxx) a2dp_source: Starting device discovery
I (xxx) a2dp_source: Discovery started
I (xxx) a2dp_source: Device found: xx:xx:xx:xx:xx:xx
I (xxx) a2dp_source: Found audio device
I (xxx) a2dp_source: Discovery stopped
I (xxx) a2dp_source: Connecting to peer device...
I (xxx) a2dp_source: A2DP connected
I (xxx) a2dp_source: Audio started
```

## Performance Optimization

### Reduce Latency

```ini
CONFIG_BT_ACL_CONNECTIONS=1
CONFIG_BT_A2DP_MAX_SRC_NUM=1
```

### Reduce Memory Usage

```ini
CONFIG_BT_BLE_ENABLED=n  # Disable BLE if not needed
CONFIG_BTDM_CTRL_BR_EDR_SCO_DATA_PATH_EFF=0
```

## Further Reading

- [ESP-IDF Bluetooth Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/index.html)
- [A2DP Profile Specification](https://www.bluetooth.com/specifications/specs/a2dp-1-3-2/)
- [ESP32 A2DP Example](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/bluedroid/classic_bt/a2dp_source)
