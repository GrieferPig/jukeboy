#pragma once
#include <Arduino.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define PANIC_DELAY 1000

#define panic(msg, ...)                                                  \
    {                                                                    \
        ESP_LOGE("PANIC", msg, ##__VA_ARGS__);                           \
        vTaskDelay(pdMS_TO_TICKS(PANIC_DELAY)); /* Allow log to flush */ \
        ESP.restart();                          /* Restart the ESP */    \
    }

#define panic_if(condition, msg, ...)  \
    {                                  \
        if (condition)                 \
        {                              \
            panic(msg, ##__VA_ARGS__); \
        }                              \
    }

#define unwrap_basetype(result, msg, ...)               \
    {                                                   \
        panic_if(result != pdPASS, msg, ##__VA_ARGS__); \
    }

#define todo(msg, ...)                        \
    {                                         \
        ESP_LOGW("TODO", msg, ##__VA_ARGS__); \
    }
