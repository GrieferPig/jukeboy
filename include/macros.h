#pragma once
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

#define PANIC_DELAY 1000

//  Use this to set the reboot flag as panic
#define hardfault()             \
    {                           \
        volatile int *p = NULL; \
        *p = 42069;             \
    }

#define todo(msg, ...)                                           \
    {                                                            \
        esp_log_write(ESP_LOG_WARN, "TODO", msg, ##__VA_ARGS__); \
    }

#define panic(msg, ...)                                                  \
    {                                                                    \
        esp_log_write(ESP_LOG_ERROR, "PANIC", msg, ##__VA_ARGS__);       \
        todo("remove this panic() call in production code");             \
        vTaskDelay(pdMS_TO_TICKS(PANIC_DELAY)); /* Allow log to flush */ \
        hardfault();                                                     \
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

#define jam(condition, msg, ...)                                                       \
    {                                                                                  \
        if (!(condition))                                                              \
        {                                                                              \
            ESP_LOGE("JAM", msg, ##__VA_ARGS__);                                       \
            todo("remove this jam() call in production code");                         \
            vTaskDelay(pdMS_TO_TICKS(PANIC_DELAY)); /* Allow log to flush */           \
            for (;;)                                                                   \
            {                                                                          \
                vTaskDelay(pdMS_TO_TICKS(1000)); /* Infinite loop to halt execution */ \
            }                                                                          \
        }                                                                              \
    }

// Return an error to the upper level caller
// Function must be defined to return esp_err_t
#define throw_esp_err(result, msg, ...)            \
    {                                              \
        if (result != ESP_OK)                      \
        {                                          \
            ESP_LOGE("THROW", msg, ##__VA_ARGS__); \
            return result;                         \
        }                                          \
    }
