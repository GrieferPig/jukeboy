#include "led_animations.h"
#include "esp_log.h"

static const char *TAG = "led_anim";

// Prebuilt animation sequences matching the pseudocode

// Blink default color 3 times, 200ms on/off
static const led_anim_step_t ANIM_SD_INSERTED[] = {
    LED_LOOP(3),
    LED_SET_DEFAULT,
    LED_SLEEP(200),
    LED_SET_DEFAULT, // keep on for clarity (same as previous)
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
    LED_SET_DEFAULT,
    LED_SLEEP(500),
    LED_OFF_STEP,
    LED_SLEEP(500),
    LED_END_LOOP,
    LED_END,
};

// Default: show default color for 5 seconds, then off
static const led_anim_step_t ANIM_DEFAULT[] = {
    LED_SET_DEFAULT,
    LED_SLEEP(5000),
    LED_OFF_STEP,
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

esp_err_t led_animations_play_action(led_action_t action)
{
    switch (action)
    {
    case LED_ACT_SD_INSERTED:
        return led_mgr_play(ANIM_SD_INSERTED);
    case LED_ACT_SD_FAIL:
        return led_mgr_play(ANIM_SD_FAIL);
    case LED_ACT_PLAY_PAUSED:
        return led_mgr_play(ANIM_PLAY_PAUSED);
    case LED_ACT_PLAY_STARTED:
    case LED_ACT_NEXT_TRACK:
    case LED_ACT_PREV_TRACK:
    case LED_ACT_VOLUME_UP:
    case LED_ACT_VOLUME_DOWN:
    case LED_ACT_TOGGLE_SHUFFLE:
    case LED_ACT_DEFAULT:
    default:
        return led_mgr_play(ANIM_DEFAULT);
    }
}
