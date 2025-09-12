#include "led_animations.h"
#include "esp_log.h"

static const char *TAG = "led_anim";

// Prebuilt animation sequences matching the pseudocode

// Blink default color 3 times, 200ms on/off
static const led_anim_step_t ANIM_SD_INSERTED[] = {
    LED_LOOP(3),
    LED_SET_DEFAULT,
    LED_SLEEP(200),
    LED_OFF_STEP,
    LED_SLEEP(200),
    LED_END_LOOP,
    LED_END,
};

// Blink red forever, 200ms on/off
static const led_anim_step_t ANIM_SD_FAIL[] = {
    LED_LOOP_FOREVER,
    LED_SET((led_color_t)LED_COLOR_RED),
    LED_SLEEP(200),
    LED_OFF_STEP,
    LED_SLEEP(200),
    LED_END_LOOP,
    LED_END,
};

// Play paused: blink default color 10 times, 500ms on/off
static const led_anim_step_t ANIM_PLAY_PAUSED[] = {
    LED_LOOP(10),
    LED_SET_DEFAULT_SMOOTH,
    LED_SLEEP(500),
    LED_OFF_STEP_SMOOTH,
    LED_SLEEP(500),
    LED_END_LOOP,
    LED_END,
};

// Default: show default color for 5 seconds, then off
static const led_anim_step_t ANIM_DEFAULT[] = {
    LED_SET_DEFAULT_SMOOTH,
    LED_SLEEP(5000),
    LED_OFF_STEP_SMOOTH,
    LED_END,
};

// No SD attention: 10x slow smooth orange on/off with 1000ms sleeps
static const led_anim_step_t ANIM_NO_SD_ATTENTION[] = {
    LED_LOOP(10),
    LED_SET_PAL_SMOOTH_SLOW(LED_PAL_ORANGE),
    LED_SLEEP(1000),
    LED_OFF_STEP_SMOOTH_SLOW,
    LED_SLEEP(1000),
    LED_END_LOOP,
    LED_END,
};

// For other actions not explicitly specified, reuse ANIM_DEFAULT

void led_animations_set_default_from_battery(bool low_battery)
{
    // if (low_battery_status) default = green; else default = orange
    if (low_battery)
    {
        led_mgr_set_default_color((led_color_t)LED_COLOR_GREEN);
    }
    else
    {
        led_mgr_set_default_color((led_color_t){255, 165, 0}); // orange
    }
}

esp_err_t led_animations_play_action(led_action_t action, bool no_skip)
{
    switch (action)
    {
    case LED_ACT_SD_INSERTED:
        return led_mgr_play_ex(ANIM_SD_INSERTED, no_skip);
    case LED_ACT_SD_FAIL:
        return led_mgr_play_ex(ANIM_SD_FAIL, no_skip);
    case LED_ACT_PLAY_PAUSED:
        return led_mgr_play_ex(ANIM_PLAY_PAUSED, no_skip);
    case LED_ACT_PLAY_STARTED:
    case LED_ACT_NEXT_TRACK:
    case LED_ACT_PREV_TRACK:
    case LED_ACT_VOLUME_UP:
    case LED_ACT_VOLUME_DOWN:
    case LED_ACT_TOGGLE_SHUFFLE:
    case LED_ACT_DEFAULT:
    case LED_ACT_NO_SD_ATTENTION:
    default:
        if (action == LED_ACT_NO_SD_ATTENTION)
            return led_mgr_play_ex(ANIM_NO_SD_ATTENTION, no_skip);
        return led_mgr_play_ex(ANIM_DEFAULT, no_skip);
    }
}
