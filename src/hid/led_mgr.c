// Animation-driven LED manager
#include "led_mgr.h"
#include "pindef.h"
#include "power_mgr.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "driver/rmt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "led_mgr";

// WS2812 LED configuration
#define WS2812_RMT_CHANNEL RMT_CHANNEL_0
#define WS2812_T0H_NS 400
#define WS2812_T0L_NS 850
#define WS2812_T1H_NS 800
#define WS2812_T1L_NS 450
#define WS2812_RESET_US 50

// LED manager configuration
#define LED_TASK_STACK_SIZE 3072
#define LED_TASK_PRIORITY 3
#define LED_UPDATE_PERIOD_MS 10
#define LED_BRIGHTNESS_SCALE 0.2f

// LED manager context
typedef struct
{
    TaskHandle_t task_handle;

    // Animation interpreter state
    const led_anim_step_t *steps;
    const led_anim_step_t *pc; // program counter
    bool current_no_skip;

    // Loop stack (single level is enough for our use-case, but keep small stack)
    struct
    {
        const led_anim_step_t *loop_start;
        uint16_t remaining; // 0 = infinite
    } loop_stack[3];
    int loop_sp;

    // Timing for sleep
    uint64_t sleep_until_ms; // 0 means not sleeping

    // LED state
    led_color_t current_color;
    led_color_t default_color;
    float current_brightness;

    rmt_item32_t ws2812_bit0;
    rmt_item32_t ws2812_bit1;

    // Smooth transition state
    bool transition_active;
    led_color_t trans_from_color;
    led_color_t trans_to_color;
    float trans_from_brightness;
    float trans_to_brightness;
    uint64_t trans_start_ms;
    uint32_t trans_duration_ms;
} led_mgr_ctx_t;

static led_mgr_ctx_t led_ctx = {0};

// Forward declarations
static void led_task(void *pvParameters);
static void ws2812_init_rmt_items(void);
static void ws2812_set_color_with_brightness(led_color_t color, float brightness);
static void led_set_color_now(led_color_t color);
static uint64_t millis(void);
static void sample_current_output(led_color_t *out_color, float *out_brightness, uint64_t now_ms);
static led_color_t palette_to_color(led_palette_t p);

esp_err_t led_mgr_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing LED Manager...");

    // Initialize context
    memset(&led_ctx, 0, sizeof(led_ctx));
    led_ctx.steps = NULL;
    led_ctx.pc = NULL;
    led_ctx.current_no_skip = false;
    led_ctx.loop_sp = -1;
    led_ctx.sleep_until_ms = 0;
    led_ctx.default_color = (led_color_t)LED_COLOR_GREEN;
    led_ctx.current_color = (led_color_t)LED_COLOR_OFF;
    led_ctx.current_brightness = 0.0f;
    led_ctx.transition_active = false;
    led_ctx.trans_duration_ms = 0;

    // Initialize WS2812 LED
    rmt_config_t config = {
        .rmt_mode = RMT_MODE_TX,
        .channel = WS2812_RMT_CHANNEL,
        .gpio_num = WS2812_LED_GPIO,
        .clk_div = 2,
        .mem_block_num = 1,
        .tx_config = {
            .loop_en = false,
            .carrier_en = false,
            .idle_level = RMT_IDLE_LEVEL_LOW,
            .idle_output_en = true,
        }};

    ret = rmt_config(&config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure RMT");
        goto cleanup;
    }

    ret = rmt_driver_install(WS2812_RMT_CHANNEL, 0, 0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install RMT driver");
        goto cleanup;
    }

    ws2812_init_rmt_items();

    // Turn off LED initially and clear tired bit
    ws2812_set_color_with_brightness((led_color_t)LED_COLOR_OFF, 0.0f);
    if (power_mgr_tired_event_group)
        xEventGroupClearBits(power_mgr_tired_event_group, LED_TIRED_BIT);

    // Create LED task
    BaseType_t task_ret = xTaskCreate(
        led_task,
        "led_task",
        LED_TASK_STACK_SIZE,
        NULL,
        LED_TASK_PRIORITY,
        &led_ctx.task_handle);
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create LED task");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "LED Manager initialized successfully");
    return ESP_OK;

cleanup:
    led_mgr_deinit();
    return ret;
}

esp_err_t led_mgr_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing LED Manager...");

    // Delete task
    if (led_ctx.task_handle != NULL)
    {
        vTaskDelete(led_ctx.task_handle);
        led_ctx.task_handle = NULL;
    }

    // Clean up RMT
    rmt_driver_uninstall(WS2812_RMT_CHANNEL);

    ESP_LOGI(TAG, "LED Manager deinitialized");
    return ESP_OK;
}

// Simple ring buffer for pending animation requests
typedef struct
{
    const led_anim_step_t *steps;
    bool no_skip;
} led_play_req_t;

#define LED_PLAY_QUEUE_SIZE 4
static led_play_req_t play_queue[LED_PLAY_QUEUE_SIZE];
static volatile int play_q_head = 0;
static volatile int play_q_tail = 0;

static bool play_q_is_empty(void) { return play_q_head == play_q_tail; }
static bool play_q_is_full(void) { return ((play_q_tail + 1) % LED_PLAY_QUEUE_SIZE) == play_q_head; }
static void play_q_push(const led_play_req_t *req)
{
    if (play_q_is_full())
    {
        // Drop oldest to make room
        play_q_head = (play_q_head + 1) % LED_PLAY_QUEUE_SIZE;
    }
    play_queue[play_q_tail] = *req;
    play_q_tail = (play_q_tail + 1) % LED_PLAY_QUEUE_SIZE;
}
static bool play_q_pop(led_play_req_t *out)
{
    if (play_q_is_empty())
        return false;
    *out = play_queue[play_q_head];
    play_q_head = (play_q_head + 1) % LED_PLAY_QUEUE_SIZE;
    return true;
}

static void start_animation(const led_anim_step_t *steps, bool no_skip)
{
    led_ctx.steps = steps;
    led_ctx.pc = steps;
    led_ctx.loop_sp = -1;
    led_ctx.sleep_until_ms = 0;
    led_ctx.current_no_skip = no_skip;
    if (power_mgr_tired_event_group)
        xEventGroupSetBits(power_mgr_tired_event_group, LED_TIRED_BIT);
}

esp_err_t led_mgr_play_ex(const led_anim_step_t *steps, bool no_skip)
{
    led_play_req_t req = {.steps = steps, .no_skip = no_skip};
    if (led_ctx.pc == NULL)
    {
        start_animation(req.steps, req.no_skip);
    }
    else if (!led_ctx.current_no_skip)
    {
        // Preempt current animation
        start_animation(req.steps, req.no_skip);
    }
    else
    {
        // Queue for later
        play_q_push(&req);
    }
    return ESP_OK;
}

esp_err_t led_mgr_play(const led_anim_step_t *steps)
{
    return led_mgr_play_ex(steps, false);
}

esp_err_t led_mgr_stop(void)
{
    led_ctx.steps = NULL;
    led_ctx.pc = NULL;
    led_ctx.loop_sp = -1;
    led_ctx.sleep_until_ms = 0;
    led_ctx.current_no_skip = false;
    led_ctx.transition_active = false;
    // Turn LED off
    ws2812_set_color_with_brightness((led_color_t)LED_COLOR_OFF, 0.0f);
    if (power_mgr_tired_event_group)
        xEventGroupClearBits(power_mgr_tired_event_group, LED_TIRED_BIT);
    return ESP_OK;
}

void led_mgr_set_default_color(led_color_t color)
{
    led_ctx.default_color = color;
}

static void led_task(void *pvParameters)
{
    TickType_t last_wake_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "LED task started");

    // Ensure LED is off initially
    ws2812_set_color_with_brightness((led_color_t)LED_COLOR_OFF, 0.0f);

    while (1)
    {
        uint64_t now_ms = millis();

        // If a smooth transition is active, update LED output progressively
        if (led_ctx.transition_active)
        {
            uint64_t elapsed = (now_ms - led_ctx.trans_start_ms);
            float t = 0.0f;
            if (led_ctx.trans_duration_ms > 0)
            {
                t = (float)elapsed / (float)led_ctx.trans_duration_ms;
                if (t >= 1.0f)
                    t = 1.0f;
            }

            float br = led_ctx.trans_from_brightness + (led_ctx.trans_to_brightness - led_ctx.trans_from_brightness) * t;
            led_color_t c;
            c.r = (uint8_t)((int)led_ctx.trans_from_color.r + (int)((led_ctx.trans_to_color.r - led_ctx.trans_from_color.r) * t));
            c.g = (uint8_t)((int)led_ctx.trans_from_color.g + (int)((led_ctx.trans_to_color.g - led_ctx.trans_from_color.g) * t));
            c.b = (uint8_t)((int)led_ctx.trans_from_color.b + (int)((led_ctx.trans_to_color.b - led_ctx.trans_from_color.b) * t));

            ws2812_set_color_with_brightness(c, br);

            if (t >= 1.0f)
            {
                led_ctx.transition_active = false;
                led_ctx.current_color = led_ctx.trans_to_color;
                led_ctx.current_brightness = led_ctx.trans_to_brightness;
            }
        }

        if (led_ctx.pc == NULL)
        {
            // Idle - ensure tired bit is clear
            if (power_mgr_tired_event_group)
                xEventGroupClearBits(power_mgr_tired_event_group, LED_TIRED_BIT);
        }
        else
        {
            // Interpreter loop: process as many opcodes as possible without sleeping
            bool progressed = false;
            for (int i = 0; i < 16; ++i) // small safeguard to avoid starving
            {
                if (led_ctx.pc == NULL)
                    break;

                if (led_ctx.sleep_until_ms)
                {
                    if (now_ms >= led_ctx.sleep_until_ms)
                    {
                        led_ctx.sleep_until_ms = 0;
                    }
                    else
                    {
                        // Still sleeping
                        break;
                    }
                }

                const led_anim_step_t *step = led_ctx.pc;
                switch (step->op)
                {
                case LED_ANIM_OP_SET_COLOR:
                {
                    led_color_t c;
                    if (step->data.set.use_default)
                        c = led_ctx.default_color;
                    else if (step->data.set.use_palette)
                        c = palette_to_color(step->data.set.palette);
                    else
                        c = step->data.set.color;
                    led_set_color_now(c);
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_SET_COLOR_SMOOTH:
                {
                    sample_current_output(&led_ctx.trans_from_color, &led_ctx.trans_from_brightness, now_ms);
                    if (step->data.set.use_palette)
                        led_ctx.trans_to_color = palette_to_color(step->data.set.palette);
                    else if (step->data.set.use_default)
                        led_ctx.trans_to_color = led_ctx.default_color;
                    else
                        led_ctx.trans_to_color = step->data.set.color;
                    led_ctx.trans_to_brightness = LED_BRIGHTNESS_SCALE;
                    led_ctx.trans_start_ms = now_ms;
                    led_ctx.trans_duration_ms = 300; // 0.3s
                    led_ctx.transition_active = true;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_SET_COLOR_SMOOTH_SLOW:
                {
                    sample_current_output(&led_ctx.trans_from_color, &led_ctx.trans_from_brightness, now_ms);
                    if (step->data.set.use_palette)
                        led_ctx.trans_to_color = palette_to_color(step->data.set.palette);
                    else if (step->data.set.use_default)
                        led_ctx.trans_to_color = led_ctx.default_color;
                    else
                        led_ctx.trans_to_color = step->data.set.color;
                    led_ctx.trans_to_brightness = LED_BRIGHTNESS_SCALE;
                    led_ctx.trans_start_ms = now_ms;
                    led_ctx.trans_duration_ms = 800; // 0.8s
                    led_ctx.transition_active = true;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_TURN_OFF:
                    led_set_color_now((led_color_t)LED_COLOR_OFF);
                    led_ctx.pc++;
                    progressed = true;
                    break;
                case LED_ANIM_OP_SET_DEFAULT_SMOOTH:
                {
                    sample_current_output(&led_ctx.trans_from_color, &led_ctx.trans_from_brightness, now_ms);
                    led_ctx.trans_to_color = led_ctx.default_color;

                    // target brightness is nominal scale
                    led_ctx.trans_to_brightness = LED_BRIGHTNESS_SCALE;
                    led_ctx.trans_start_ms = now_ms;
                    led_ctx.trans_duration_ms = 300; // 0.3s
                    led_ctx.transition_active = true;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_TURN_OFF_SMOOTH:
                {
                    sample_current_output(&led_ctx.trans_from_color, &led_ctx.trans_from_brightness, now_ms);
                    led_ctx.trans_to_color = (led_color_t)LED_COLOR_OFF;
                    led_ctx.trans_to_brightness = 0.0f;
                    led_ctx.trans_start_ms = now_ms;
                    led_ctx.trans_duration_ms = 300; // 0.3s
                    led_ctx.transition_active = true;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_SET_DEFAULT_SMOOTH_SLOW:
                {
                    sample_current_output(&led_ctx.trans_from_color, &led_ctx.trans_from_brightness, now_ms);
                    led_ctx.trans_to_color = led_ctx.default_color;
                    led_ctx.trans_to_brightness = LED_BRIGHTNESS_SCALE;
                    led_ctx.trans_start_ms = now_ms;
                    led_ctx.trans_duration_ms = 800; // 0.8s
                    led_ctx.transition_active = true;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_TURN_OFF_SMOOTH_SLOW:
                {
                    sample_current_output(&led_ctx.trans_from_color, &led_ctx.trans_from_brightness, now_ms);
                    led_ctx.trans_to_color = (led_color_t)LED_COLOR_OFF;
                    led_ctx.trans_to_brightness = 0.0f;
                    led_ctx.trans_start_ms = now_ms;
                    led_ctx.trans_duration_ms = 800; // 0.8s
                    led_ctx.transition_active = true;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_SLEEP_MS:
                    led_ctx.sleep_until_ms = now_ms + step->data.sleep.duration_ms;
                    led_ctx.pc++;
                    progressed = true;
                    break;
                case LED_ANIM_OP_LOOP_START:
                    if (led_ctx.loop_sp + 1 < (int)(sizeof(led_ctx.loop_stack) / sizeof(led_ctx.loop_stack[0])))
                    {
                        led_ctx.loop_sp++;
                        led_ctx.loop_stack[led_ctx.loop_sp].loop_start = led_ctx.pc + 1;
                        led_ctx.loop_stack[led_ctx.loop_sp].remaining = step->data.loop.count;
                        led_ctx.pc++;
                        progressed = true;
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Loop stack overflow; ignoring loop start");
                        led_ctx.pc++;
                    }
                    break;
                case LED_ANIM_OP_LOOP_END:
                    if (led_ctx.loop_sp >= 0)
                    {
                        if (led_ctx.loop_stack[led_ctx.loop_sp].remaining == 0)
                        {
                            // infinite
                            led_ctx.pc = led_ctx.loop_stack[led_ctx.loop_sp].loop_start;
                        }
                        else if (led_ctx.loop_stack[led_ctx.loop_sp].remaining > 1)
                        {
                            led_ctx.loop_stack[led_ctx.loop_sp].remaining--;
                            led_ctx.pc = led_ctx.loop_stack[led_ctx.loop_sp].loop_start;
                        }
                        else
                        {
                            // loop finished
                            led_ctx.loop_sp--;
                            led_ctx.pc++;
                        }
                        progressed = true;
                    }
                    else
                    {
                        // unmatched end -> just proceed
                        led_ctx.pc++;
                    }
                    break;
                case LED_ANIM_OP_END:
                {
                    // End of program; try to start next queued animation
                    led_ctx.pc = NULL;
                    led_ctx.steps = NULL;
                    led_ctx.current_no_skip = false;
                    led_play_req_t next;
                    if (play_q_pop(&next))
                    {
                        start_animation(next.steps, next.no_skip);
                    }
                    // Leave LED state; tired bit will be cleared if idle next tick
                    progressed = true;
                    break;
                }
                }
            }

            (void)progressed;
        }

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(LED_UPDATE_PERIOD_MS));
    }
}

static void ws2812_init_rmt_items(void)
{
    uint32_t counter_clk_hz = 0;
    rmt_get_counter_clock(WS2812_RMT_CHANNEL, &counter_clk_hz);

    float ratio = (float)counter_clk_hz / 1000000000.0;

    led_ctx.ws2812_bit0.level0 = 1;
    led_ctx.ws2812_bit0.duration0 = (uint32_t)(ratio * WS2812_T0H_NS);
    led_ctx.ws2812_bit0.level1 = 0;
    led_ctx.ws2812_bit0.duration1 = (uint32_t)(ratio * WS2812_T0L_NS);

    led_ctx.ws2812_bit1.level0 = 1;
    led_ctx.ws2812_bit1.duration0 = (uint32_t)(ratio * WS2812_T1H_NS);
    led_ctx.ws2812_bit1.level1 = 0;
    led_ctx.ws2812_bit1.duration1 = (uint32_t)(ratio * WS2812_T1L_NS);
}

static void ws2812_set_color_with_brightness(led_color_t color, float brightness)
{
    // Apply brightness scaling
    uint8_t r = (uint8_t)(color.r * brightness);
    uint8_t g = (uint8_t)(color.g * brightness);
    uint8_t b = (uint8_t)(color.b * brightness);

    rmt_item32_t items[24];
    uint32_t color_val = (g << 16) | (r << 8) | b; // GRB format

    for (int i = 0; i < 24; i++)
    {
        items[i] = (color_val & (1 << (23 - i))) ? led_ctx.ws2812_bit1 : led_ctx.ws2812_bit0;
    }

    rmt_write_items(WS2812_RMT_CHANNEL, items, 24, false);
}

static void led_set_color_now(led_color_t color)
{
    led_ctx.current_color = color;
    led_ctx.current_brightness = LED_BRIGHTNESS_SCALE;
    led_ctx.transition_active = false; // cancel any ongoing transition
    ws2812_set_color_with_brightness(led_ctx.current_color, led_ctx.current_brightness);
}

static uint64_t millis(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}

static void sample_current_output(led_color_t *out_color, float *out_brightness, uint64_t now_ms)
{
    if (!out_color || !out_brightness)
        return;
    if (led_ctx.transition_active && led_ctx.trans_duration_ms > 0)
    {
        uint64_t elapsed = now_ms - led_ctx.trans_start_ms;
        float t = (float)elapsed / (float)led_ctx.trans_duration_ms;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        out_color->r = (uint8_t)((int)led_ctx.trans_from_color.r + (int)((led_ctx.trans_to_color.r - led_ctx.trans_from_color.r) * t));
        out_color->g = (uint8_t)((int)led_ctx.trans_from_color.g + (int)((led_ctx.trans_to_color.g - led_ctx.trans_from_color.g) * t));
        out_color->b = (uint8_t)((int)led_ctx.trans_from_color.b + (int)((led_ctx.trans_to_color.b - led_ctx.trans_from_color.b) * t));
        *out_brightness = led_ctx.trans_from_brightness + (led_ctx.trans_to_brightness - led_ctx.trans_from_brightness) * t;
    }
    else
    {
        *out_color = led_ctx.current_color;
        *out_brightness = led_ctx.current_brightness;
    }
}

static led_color_t palette_to_color(led_palette_t p)
{
    switch (p)
    {
    case LED_PAL_GREEN:
        return (led_color_t)LED_COLOR_GREEN;
    case LED_PAL_ORANGE:
        return (led_color_t)LED_COLOR_ORANGE;
    case LED_PAL_BLUE:
        return (led_color_t)LED_COLOR_BLUE;
    case LED_PAL_MAGENTA:
        return (led_color_t)LED_COLOR_PURPLE;
    case LED_PAL_RED:
    default:
        return (led_color_t)LED_COLOR_RED;
    }
}
