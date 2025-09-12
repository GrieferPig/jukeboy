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
} led_mgr_ctx_t;

static led_mgr_ctx_t led_ctx = {0};

// Forward declarations
static void led_task(void *pvParameters);
static void ws2812_init_rmt_items(void);
static void ws2812_set_color_with_brightness(led_color_t color, float brightness);
static void led_set_color_now(led_color_t color);
static uint64_t millis(void);

esp_err_t led_mgr_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing LED Manager...");

    // Initialize context
    memset(&led_ctx, 0, sizeof(led_ctx));
    led_ctx.steps = NULL;
    led_ctx.pc = NULL;
    led_ctx.loop_sp = -1;
    led_ctx.sleep_until_ms = 0;
    led_ctx.default_color = (led_color_t)LED_COLOR_GREEN;
    led_ctx.current_color = (led_color_t)LED_COLOR_OFF;
    led_ctx.current_brightness = 0.0f;

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

esp_err_t led_mgr_play(const led_anim_step_t *steps)
{
    led_ctx.steps = steps;
    led_ctx.pc = steps;
    led_ctx.loop_sp = -1;
    led_ctx.sleep_until_ms = 0;
    // Mark as active to prevent sleep
    if (power_mgr_tired_event_group)
        xEventGroupSetBits(power_mgr_tired_event_group, LED_TIRED_BIT);
    return ESP_OK;
}

esp_err_t led_mgr_stop(void)
{
    led_ctx.steps = NULL;
    led_ctx.pc = NULL;
    led_ctx.loop_sp = -1;
    led_ctx.sleep_until_ms = 0;
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
                    led_color_t c = step->data.set.use_default ? led_ctx.default_color : step->data.set.color;
                    led_set_color_now(c);
                    led_ctx.pc++;
                    progressed = true;
                    break;
                }
                case LED_ANIM_OP_TURN_OFF:
                    led_set_color_now((led_color_t)LED_COLOR_OFF);
                    led_ctx.pc++;
                    progressed = true;
                    break;
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
                    // End of program
                    led_ctx.pc = NULL;
                    led_ctx.steps = NULL;
                    // Leave LED in last state but clear tired after a small grace period handled by idle branch
                    progressed = true;
                    break;
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
    ws2812_set_color_with_brightness(led_ctx.current_color, led_ctx.current_brightness);
}

static uint64_t millis(void)
{
    return (uint64_t)esp_timer_get_time() / 1000ULL;
}
