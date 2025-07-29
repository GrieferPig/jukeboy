#pragma once
#include "esp_err.h"

// Bits from other tasks that indicate whether the pm can sleep
// Only if all bits are cleared, the power manager can sleep
extern EventGroupHandle_t power_mgr_tired_event_group;

typedef enum
{
    AUDIO_PLAYER_TIRED_BIT = BIT0, // Audio player
    HID_TIRED_BIT = BIT1,          // HID task
    MAIN_TIRED_BIT = BIT2,         // Main task (prevent deepsleep until everything is initialized)
} power_mgr_tired_bits_t;

esp_err_t power_mgr_init(void);
esp_err_t power_mgr_deinit(void);
void power_mgr_notify_main_initialized(void); // Notify the power manager that the main task is exiting
void power_mgr_deep_sleep(void);              // Remove this after test
