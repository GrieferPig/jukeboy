#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// LED color structure
typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} led_color_t;

// Common colors
#define LED_COLOR_RED {255, 0, 0}
#define LED_COLOR_GREEN {0, 255, 0}
#define LED_COLOR_BLUE {0, 0, 255}
#define LED_COLOR_YELLOW {255, 255, 0}
#define LED_COLOR_PURPLE {255, 0, 255}
#define LED_COLOR_CYAN {0, 255, 255}
#define LED_COLOR_WHITE {255, 255, 255}
#define LED_COLOR_OFF {0, 0, 0}

// Animation opcodes
typedef enum
{
    LED_ANIM_OP_SET_COLOR,  // Set to a color (or default)
    LED_ANIM_OP_TURN_OFF,   // Turn off (alias for color off)
    LED_ANIM_OP_SLEEP_MS,   // Delay
    LED_ANIM_OP_LOOP_START, // Start loop; count=0 means infinite
    LED_ANIM_OP_LOOP_END,   // End loop
    LED_ANIM_OP_END         // End of sequence
} led_anim_op_t;

typedef struct
{
    led_anim_op_t op;
    union
    {
        struct
        {
            led_color_t color;
            bool use_default; // if true, ignore color and use default
        } set;
        struct
        {
            uint32_t duration_ms;
        } sleep;
        struct
        {
            uint16_t count; // 0 = infinite
        } loop;
    } data;
} led_anim_step_t;

// Helper macros for building animations
#define LED_SET(c)                                                       \
    {                                                                    \
        .op = LED_ANIM_OP_SET_COLOR, .data.set = {.color = (c),          \
                                                  .use_default = false } \
    }
#define LED_SET_DEFAULT                                                 \
    {                                                                   \
        .op = LED_ANIM_OP_SET_COLOR, .data.set = {.color = {0, 0, 0},   \
                                                  .use_default = true } \
    }
#define LED_OFF_STEP {.op = LED_ANIM_OP_TURN_OFF}
#define LED_SLEEP(ms)                                                    \
    {                                                                    \
        .op = LED_ANIM_OP_SLEEP_MS, .data.sleep = {.duration_ms = (ms) } \
    }
#define LED_LOOP(n)                                                \
    {                                                              \
        .op = LED_ANIM_OP_LOOP_START, .data.loop = {.count = (n) } \
    }
#define LED_LOOP_FOREVER {.op = LED_ANIM_OP_LOOP_START, .data.loop = {.count = 0}}
#define LED_END_LOOP {.op = LED_ANIM_OP_LOOP_END}
#define LED_END {.op = LED_ANIM_OP_END}

/**
 * @brief Initialize the LED manager
 */
esp_err_t led_mgr_init(void);

/**
 * @brief Deinitialize the LED manager
 */
esp_err_t led_mgr_deinit(void);

/**
 * @brief Play an animation (non-blocking). Replaces any running animation.
 * The passed array must remain valid for the duration of the animation (typically static const).
 */
esp_err_t led_mgr_play(const led_anim_step_t *steps);

/**
 * @brief Stop any running animation and turn the LED off.
 */
esp_err_t led_mgr_stop(void);

/**
 * @brief Set the default color used when a step requests default.
 */
void led_mgr_set_default_color(led_color_t color);
