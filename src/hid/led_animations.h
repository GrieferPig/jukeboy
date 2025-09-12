#pragma once
#include "esp_err.h"
#include "led_mgr.h"
#include <stdbool.h>

// High-level actions the rest of the app can trigger
typedef enum
{
    LED_ACT_SD_INSERTED,
    LED_ACT_SD_FAIL,
    LED_ACT_PLAY_PAUSED,
    LED_ACT_PLAY_STARTED,
    LED_ACT_NEXT_TRACK,
    LED_ACT_PREV_TRACK,
    LED_ACT_VOLUME_UP,
    LED_ACT_VOLUME_DOWN,
    LED_ACT_TOGGLE_SHUFFLE,
    LED_ACT_DEFAULT,
    LED_ACT_NO_SD_ATTENTION
} led_action_t;

// Set default color based on low battery flag
void led_animations_set_default_from_battery(bool low_battery);

// Play a predefined animation for an action. If no_skip is true, the animation will finish before others run.
esp_err_t led_animations_play_action(led_action_t action, bool no_skip);
