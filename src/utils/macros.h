#pragma once
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

#define PANIC_DELAY 1000

#define todo(msg, ...)                                           \
    {                                                            \
        esp_log_write(ESP_LOG_WARN, "TODO", msg, ##__VA_ARGS__); \
    }

#define panic(msg, ...)                                                  \
    {                                                                    \
        esp_log_write(ESP_LOG_ERROR, "PANIC", msg, ##__VA_ARGS__);       \
        todo("remove this panic() call in production code");             \
        vTaskDelay(pdMS_TO_TICKS(PANIC_DELAY)); /* Allow log to flush */ \
        esp_restart();                          /* Restart the ESP */    \
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

#define unwrap_esp_err(result, msg, ...)                \
    {                                                   \
        panic_if(result != ESP_OK, msg, ##__VA_ARGS__); \
    }