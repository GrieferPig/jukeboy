#include "led_mgr.h"
#include "pindef.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "driver/rmt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
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
#define LED_QUEUE_SIZE 10
#define LED_TASK_STACK_SIZE 3072
#define LED_TASK_PRIORITY 3
#define LED_UPDATE_PERIOD_MS 20
#define LED_AUTO_OFF_TIMEOUT_S 15
#define LED_FADE_DURATION_MS 500
#define LED_BRIGHTNESS_SCALE 0.2f
#define LED_FLASH_PERIOD_MS 200

// LED states
typedef enum
{
    LED_STATE_OFF,
    LED_STATE_FADING_IN,
    LED_STATE_ON,
    LED_STATE_FADING_OUT,
    LED_STATE_FLASHING
} led_state_t;

// LED manager context
typedef struct
{
    QueueHandle_t event_queue;
    TaskHandle_t task_handle;
    TimerHandle_t auto_off_timer;

    led_state_t state;
    led_color_t current_color;
    led_color_t target_color;
    led_color_t default_color;
    led_color_t flash_color;

    uint32_t fade_start_time;
    uint32_t flash_start_time;
    float current_brightness;
    bool flash_on;

    rmt_item32_t ws2812_bit0;
    rmt_item32_t ws2812_bit1;
} led_mgr_ctx_t;

static led_mgr_ctx_t led_ctx = {0};

// Forward declarations
static void led_task(void *pvParameters);
static void auto_off_timer_callback(TimerHandle_t timer);
static void ws2812_init_rmt_items(void);
static void ws2812_set_color_with_brightness(led_color_t color, float brightness);
static float ease_in_out_cubic(float t);
static led_color_t interpolate_color(led_color_t from, led_color_t to, float t);

esp_err_t led_mgr_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing LED Manager...");

    // Initialize context
    memset(&led_ctx, 0, sizeof(led_ctx));
    led_ctx.state = LED_STATE_OFF;
    led_ctx.default_color = (led_color_t)LED_COLOR_GREEN;
    led_ctx.current_color = (led_color_t)LED_COLOR_OFF;
    led_ctx.target_color = (led_color_t)LED_COLOR_OFF;
    led_ctx.current_brightness = 0.0f;

    // Create event queue
    led_ctx.event_queue = xQueueCreate(LED_QUEUE_SIZE, sizeof(led_event_t));
    if (led_ctx.event_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create event queue");
        return ESP_FAIL;
    }

    // Create auto-off timer
    led_ctx.auto_off_timer = xTimerCreate(
        "led_auto_off",
        pdMS_TO_TICKS(LED_AUTO_OFF_TIMEOUT_S * 1000),
        pdFALSE, // One-shot timer
        NULL,
        auto_off_timer_callback);
    if (led_ctx.auto_off_timer == NULL)
    {
        ESP_LOGE(TAG, "Failed to create auto-off timer");
        ret = ESP_FAIL;
        goto cleanup;
    }

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

    // Turn off LED initially
    ws2812_set_color_with_brightness((led_color_t)LED_COLOR_OFF, 0.0f);

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

    // Delete timer
    if (led_ctx.auto_off_timer != NULL)
    {
        xTimerDelete(led_ctx.auto_off_timer, portMAX_DELAY);
        led_ctx.auto_off_timer = NULL;
    }

    // Clean up RMT
    rmt_driver_uninstall(WS2812_RMT_CHANNEL);

    // Delete queue
    if (led_ctx.event_queue != NULL)
    {
        vQueueDelete(led_ctx.event_queue);
        led_ctx.event_queue = NULL;
    }

    ESP_LOGI(TAG, "LED Manager deinitialized");
    return ESP_OK;
}

esp_err_t led_mgr_set_color(led_color_t color)
{
    led_event_t event = {
        .type = LED_EVENT_SET_COLOR,
        .color = color};

    BaseType_t result = xQueueSend(led_ctx.event_queue, &event, pdMS_TO_TICKS(100));
    if (result != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to send color event to queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t led_mgr_turn_off(void)
{
    led_event_t event = {
        .type = LED_EVENT_TURN_OFF,
        .color = {0}};

    BaseType_t result = xQueueSend(led_ctx.event_queue, &event, pdMS_TO_TICKS(100));
    if (result != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to send turn off event to queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t led_mgr_start_flash(led_color_t color)
{
    led_event_t event = {
        .type = LED_EVENT_START_FLASH,
        .color = color};

    BaseType_t result = xQueueSend(led_ctx.event_queue, &event, pdMS_TO_TICKS(100));
    if (result != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to send flash event to queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t led_mgr_stop_flash(void)
{
    led_event_t event = {
        .type = LED_EVENT_STOP_FLASH,
        .color = {0}};

    BaseType_t result = xQueueSend(led_ctx.event_queue, &event, pdMS_TO_TICKS(100));
    if (result != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to send stop flash event to queue");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void led_task(void *pvParameters)
{
    led_event_t event;
    TickType_t last_wake_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "LED task started");

    while (1)
    {
        // Check for events (non-blocking)
        if (xQueueReceive(led_ctx.event_queue, &event, 0) == pdPASS)
        {
            switch (event.type)
            {
            case LED_EVENT_SET_COLOR:
                ESP_LOGI(TAG, "Setting color: R=%d, G=%d, B=%d",
                         event.color.r, event.color.g, event.color.b);

                // If LED is off, start fade in
                if (led_ctx.state == LED_STATE_OFF)
                {
                    led_ctx.state = LED_STATE_FADING_IN;
                    led_ctx.fade_start_time = esp_timer_get_time() / 1000; // Convert to ms
                    led_ctx.current_color = led_ctx.default_color;
                }

                // Set new target color
                led_ctx.target_color = event.color;

                // Restart auto-off timer
                xTimerReset(led_ctx.auto_off_timer, 0);
                break;

            case LED_EVENT_TURN_OFF:
                ESP_LOGI(TAG, "Turning off LED");
                if (led_ctx.state != LED_STATE_OFF && led_ctx.state != LED_STATE_FADING_OUT)
                {
                    led_ctx.state = LED_STATE_FADING_OUT;
                    led_ctx.fade_start_time = esp_timer_get_time() / 1000; // Convert to ms
                }
                xTimerStop(led_ctx.auto_off_timer, 0);
                break;

            case LED_EVENT_START_FLASH:
                ESP_LOGI(TAG, "Starting flash: R=%d, G=%d, B=%d",
                         event.color.r, event.color.g, event.color.b);
                led_ctx.flash_color = event.color;
                led_ctx.state = LED_STATE_FLASHING;
                led_ctx.flash_start_time = esp_timer_get_time() / 1000; // Convert to ms
                xTimerStop(led_ctx.auto_off_timer, 0);
                break;

            case LED_EVENT_STOP_FLASH:
                ESP_LOGI(TAG, "Stopping flash");
                if (led_ctx.state == LED_STATE_FLASHING)
                {
                    led_ctx.state = LED_STATE_FADING_OUT;
                    led_ctx.fade_start_time = esp_timer_get_time() / 1000; // Convert to ms
                }
                break;
            }
        }

        // Update LED state
        uint32_t current_time = esp_timer_get_time() / 1000; // Convert to ms

        switch (led_ctx.state)
        {
        case LED_STATE_OFF:
            // LED is off, nothing to do
            break;

        case LED_STATE_FADING_IN:
        {
            uint32_t fade_elapsed = current_time - led_ctx.fade_start_time;
            float fade_progress = (float)fade_elapsed / LED_FADE_DURATION_MS;

            if (fade_progress >= 1.0f)
            {
                // Fade in complete
                led_ctx.state = LED_STATE_ON;
                led_ctx.current_brightness = LED_BRIGHTNESS_SCALE;
                led_ctx.current_color = led_ctx.target_color;
            }
            else
            {
                // Continue fading in
                led_ctx.current_brightness = ease_in_out_cubic(fade_progress) * LED_BRIGHTNESS_SCALE;
                led_ctx.current_color = interpolate_color(led_ctx.default_color, led_ctx.target_color, fade_progress);
            }

            ws2812_set_color_with_brightness(led_ctx.current_color, led_ctx.current_brightness);
            break;
        }

        case LED_STATE_ON:
            // Smoothly interpolate to target color if it changed
            if (memcmp(&led_ctx.current_color, &led_ctx.target_color, sizeof(led_color_t)) != 0)
            {
                // Simple interpolation over 200ms
                float interp_speed = (float)LED_UPDATE_PERIOD_MS / 200.0f;
                led_ctx.current_color = interpolate_color(led_ctx.current_color, led_ctx.target_color, interp_speed);

                // Check if we're close enough to the target
                if (abs(led_ctx.current_color.r - led_ctx.target_color.r) <= 1 &&
                    abs(led_ctx.current_color.g - led_ctx.target_color.g) <= 1 &&
                    abs(led_ctx.current_color.b - led_ctx.target_color.b) <= 1)
                {
                    led_ctx.current_color = led_ctx.target_color;
                }

                ws2812_set_color_with_brightness(led_ctx.current_color, led_ctx.current_brightness);
            }
            break;

        case LED_STATE_FADING_OUT:
        {
            uint32_t fade_elapsed = current_time - led_ctx.fade_start_time;
            float fade_progress = (float)fade_elapsed / LED_FADE_DURATION_MS;

            if (fade_progress >= 1.0f)
            {
                // Fade out complete
                led_ctx.state = LED_STATE_OFF;
                led_ctx.current_brightness = 0.0f;
                led_ctx.current_color = (led_color_t)LED_COLOR_OFF;
                led_ctx.target_color = (led_color_t)LED_COLOR_OFF;
            }
            else
            {
                // Continue fading out
                led_ctx.current_brightness = (1.0f - ease_in_out_cubic(fade_progress)) * LED_BRIGHTNESS_SCALE;
            }

            ws2812_set_color_with_brightness(led_ctx.current_color, led_ctx.current_brightness);
            break;
        }

        case LED_STATE_FLASHING:
        {
            uint32_t flash_elapsed = current_time - led_ctx.flash_start_time;
            float flash_progress = (float)flash_elapsed / LED_FLASH_PERIOD_MS;

            if (flash_progress >= 1.0f)
            {
                // Flash period complete, toggle LED state
                led_ctx.flash_on = !led_ctx.flash_on;
                led_ctx.flash_start_time = current_time;

                if (led_ctx.flash_on)
                {
                    // Set color to flash color
                    led_ctx.current_color = led_ctx.flash_color;
                    led_ctx.current_brightness = LED_BRIGHTNESS_SCALE;
                }
                else
                {
                    // Turn off LED
                    led_ctx.current_color = (led_color_t)LED_COLOR_OFF;
                    led_ctx.current_brightness = 0.0f;
                }
            }

            ws2812_set_color_with_brightness(led_ctx.current_color, led_ctx.current_brightness);
            break;
        }
        }

        // Wait for next update
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(LED_UPDATE_PERIOD_MS));
    }
}

static void auto_off_timer_callback(TimerHandle_t timer)
{
    ESP_LOGI(TAG, "Auto-off timer triggered");
    led_mgr_turn_off();
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

static float ease_in_out_cubic(float t)
{
    if (t < 0.5f)
    {
        return 4.0f * t * t * t;
    }
    else
    {
        float p = 2.0f * t - 2.0f;
        return 1.0f + p * p * p / 2.0f;
    }
}

static led_color_t interpolate_color(led_color_t from, led_color_t to, float t)
{
    if (t <= 0.0f)
        return from;
    if (t >= 1.0f)
        return to;

    led_color_t result;
    result.r = (uint8_t)(from.r + (to.r - from.r) * t);
    result.g = (uint8_t)(from.g + (to.g - from.g) * t);
    result.b = (uint8_t)(from.b + (to.b - from.b) * t);

    return result;
}
