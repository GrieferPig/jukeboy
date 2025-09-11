#include "hid_mgr.h"
#include "hid_event_system.h"
#include "pindef.h"
#include "led_mgr.h"
#include "macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_system.h"
#include <stdlib.h>
#include "power_mgr.h"
#include <string.h>

static const char *TAG = "hid_mgr";

// Button configuration structure
typedef struct
{
    gpio_num_t gpio_num;
    const char *name;
} button_config_t;

// Button configurations (removed command coupling)
static const button_config_t button_configs[] = {
    {BTN1_GPIO, "BTN1"},
    {BTN2_GPIO, "BTN2"},
    {BTN3_GPIO, "BTN3"},
    {BTN4_GPIO, "BTN4"},
    {BTN5_GPIO, "BTN5"},
    {BTN6_GPIO, "BTN6"},
};

#define NUM_BUTTONS (sizeof(button_configs) / sizeof(button_config_t))

// Internal event structure
typedef struct
{
    gpio_num_t gpio_num;
    bool pressed;
    TickType_t timestamp;
} internal_button_event_t;

// Button state tracking
typedef struct
{
    bool current_state;
    bool previous_state;
    TickType_t press_start_time;
    TickType_t last_release_time;
    bool long_press_sent;
    int press_count;
    TimerHandle_t double_press_timer;
    // --- added for deferred single press dispatch ---
    bool pending_release;
    uint32_t pending_release_duration;
} button_state_t;

// Global state
static QueueHandle_t button_queue = NULL;
static TaskHandle_t hid_task_handle = NULL;
static TimerHandle_t hid_can_sleep_timer = NULL;
static button_state_t button_states[NUM_BUTTONS];

// Restart combination tracking
static TickType_t restart_combo_start_time = 0;
static bool restart_combo_active = false;

#define HID_CAN_SLEEP_TIMEOUT_MS 10000

// Forward declarations
static void hid_task(void *pvParameters);
static void hid_can_sleep_timer_callback(TimerHandle_t xTimer);
static void double_press_timer_callback(TimerHandle_t xTimer);
static void process_button_event(gpio_num_t gpio_num, bool pressed, TickType_t timestamp);
static void check_restart_combo(void);
static int gpio_to_button_index(gpio_num_t gpio_num);

// GPIO interrupt handler
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    internal_button_event_t event;
    event.gpio_num = (gpio_num_t)(uintptr_t)arg;
    event.pressed = (gpio_get_level(event.gpio_num) == 0);
    event.timestamp = xTaskGetTickCountFromISR();

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(button_queue, &event, &xHigherPriorityTaskWoken);

    // Reset the can sleep timer and set tired bit
    if (hid_can_sleep_timer != NULL)
    {
        xTimerResetFromISR(hid_can_sleep_timer, &xHigherPriorityTaskWoken);
        xEventGroupSetBitsFromISR(power_mgr_tired_event_group, HID_TIRED_BIT, &xHigherPriorityTaskWoken);
    }

    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
    }
}

static int gpio_to_button_index(gpio_num_t gpio_num)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        if (button_configs[i].gpio_num == gpio_num)
        {
            return i;
        }
    }
    return -1;
}

static void double_press_timer_callback(TimerHandle_t xTimer)
{
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        if (button_states[i].double_press_timer == xTimer)
        {
            button_state_t *st = &button_states[i];
            if (st->press_count == 1)
            {
                // Confirmed single short press: dispatch PRESS then RELEASE
                ESP_LOGI(TAG, "Single press confirmed (timer) GPIO %d", button_configs[i].gpio_num);
                hid_event_data_t press_evt = {
                    .gpio_num = button_configs[i].gpio_num,
                    .event_type = HID_EVENT_PRESS,
                    .duration_ms = 0,
                    .combo_mask = 0};
                hid_event_dispatch(&press_evt);

                if (st->pending_release)
                {
                    hid_event_data_t rel_evt = {
                        .gpio_num = button_configs[i].gpio_num,
                        .event_type = HID_EVENT_RELEASE,
                        .duration_ms = st->pending_release_duration,
                        .combo_mask = 0};
                    hid_event_dispatch(&rel_evt);
                }
            }
            // reset state
            st->press_count = 0;
            st->pending_release = false;
            st->pending_release_duration = 0;
            break;
        }
    }
}

static void process_button_event(gpio_num_t gpio_num, bool pressed, TickType_t timestamp)
{
    ESP_LOGI(TAG, "process_button_event: gpio=%d pressed=%d tick=%lu", gpio_num, pressed, (unsigned long)timestamp);

    int btn_idx = gpio_to_button_index(gpio_num);
    if (btn_idx < 0)
    {
        ESP_LOGI(TAG, "process_button_event: gpio %d not tracked", gpio_num);
        return;
    }

    button_state_t *state = &button_states[btn_idx];
    bool prev = state->current_state;
    state->previous_state = state->current_state;
    state->current_state = pressed;

    ESP_LOGI(TAG,
             "Button idx=%d prev_state=%d curr_state=%d press_count=%d long_sent=%d",
             btn_idx, prev, state->current_state, state->press_count, state->long_press_sent);

    if (pressed && !state->previous_state)
    {
        // Press edge
        state->press_start_time = timestamp;
        state->long_press_sent = false;

        ESP_LOGI(TAG,
                 "PRESS: gpio=%d idx=%d press_start_tick=%lu press_count=%d",
                 gpio_num, btn_idx, (unsigned long)state->press_start_time, state->press_count);

        // Check if this is within double-press window
        if (state->press_count == 1 &&
            state->double_press_timer &&
            xTimerIsTimerActive(state->double_press_timer))
        {
            // Second press detected - stop timer and increment count
            xTimerStop(state->double_press_timer, 0);
            state->press_count = 2;
            ESP_LOGI(TAG, "Second press detected within window gpio=%d", gpio_num);
        }
        else
        {
            // First press or press outside window
            state->press_count = 1;
            ESP_LOGI(TAG, "First press detected gpio=%d", gpio_num);
        }
    }
    else if (!pressed && state->previous_state)
    {
        // Release edge
        state->last_release_time = timestamp;
        uint32_t press_duration = pdTICKS_TO_MS(timestamp - state->press_start_time);
        ESP_LOGI(TAG,
                 "RELEASE: gpio=%d idx=%d duration_ms=%lu press_count=%d long_sent=%d",
                 gpio_num, btn_idx, (unsigned long)press_duration, state->press_count, state->long_press_sent);

        if (press_duration >= HID_LONG_PRESS_DURATION_MS)
        {
            ESP_LOGI(TAG, "Long press release (already reported) gpio=%d", gpio_num);
            state->press_count = 0;
        }
        else if (state->press_count == 1)
        {
            ESP_LOGI(TAG,
                     "First press complete - starting double-press window (%d ms) gpio=%d",
                     HID_DOUBLE_PRESS_WINDOW_MS, gpio_num);
            // Defer single press decision: store release, start timer
            state->pending_release = true;
            state->pending_release_duration = press_duration;
            if (state->double_press_timer)
            {
                xTimerChangePeriod(state->double_press_timer,
                                   pdMS_TO_TICKS(HID_DOUBLE_PRESS_WINDOW_MS), 0);
                xTimerStart(state->double_press_timer, 0);
            }
            ESP_LOGI(TAG, "Short press released (deferred) gpio=%d", gpio_num);
            // Do NOT dispatch RELEASE yet (PRESS will be generated on timer expiry)
        }
        else if (state->press_count == 2)
        {
            ESP_LOGI(TAG, "DOUBLE PRESS detected gpio=%d", gpio_num);

            // Stop any active timer
            if (state->double_press_timer && xTimerIsTimerActive(state->double_press_timer))
            {
                xTimerStop(state->double_press_timer, 0);
                ESP_LOGI(TAG, "Stopped double-press timer after second press gpio=%d", gpio_num);
            }

            hid_event_data_t event = {
                .gpio_num = gpio_num,
                .event_type = HID_EVENT_DOUBLE_PRESS,
                .duration_ms = press_duration,
                .combo_mask = 0};
            ESP_LOGI(TAG, "Dispatch HID_EVENT_DOUBLE_PRESS gpio=%d duration=%lu",
                     gpio_num, (unsigned long)press_duration);
            hid_event_dispatch(&event);

            hid_event_data_t release_evt = {
                .gpio_num = gpio_num,
                .event_type = HID_EVENT_RELEASE,
                .duration_ms = press_duration,
                .combo_mask = 0};
            hid_event_dispatch(&release_evt);

            state->press_count = 0;
        }
        else
        {
            // Do nothing, state will be reset on next press
        }
    }
    else if (pressed && state->previous_state)
    {
        // Held state - check for long press
        uint32_t hold_duration = pdTICKS_TO_MS(timestamp - state->press_start_time);
        ESP_LOGI(TAG,
                 "HOLD: gpio=%d idx=%d hold_ms=%lu long_sent=%d",
                 gpio_num, btn_idx, (unsigned long)hold_duration, state->long_press_sent);

        if (hold_duration >= HID_LONG_PRESS_DURATION_MS && !state->long_press_sent)
        {
            ESP_LOGI(TAG, "LONG PRESS threshold reached gpio=%d hold_ms=%lu",
                     gpio_num, (unsigned long)hold_duration);

            // Cancel double-press detection since this is a long press
            if (state->double_press_timer && xTimerIsTimerActive(state->double_press_timer))
            {
                xTimerStop(state->double_press_timer, 0);
                ESP_LOGI(TAG, "Cancelled double-press timer due to long press gpio=%d", gpio_num);
            }

            hid_event_data_t event = {
                .gpio_num = gpio_num,
                .event_type = HID_EVENT_LONG_PRESS,
                .duration_ms = hold_duration,
                .combo_mask = 0};
            ESP_LOGI(TAG, "Dispatch HID_EVENT_LONG_PRESS gpio=%d duration=%lu",
                     gpio_num, (unsigned long)hold_duration);
            hid_event_dispatch(&event);
            state->long_press_sent = true;
            state->press_count = 0; // Reset to prevent any further events
        }
    }
    else
    {
        ESP_LOGI(TAG, "No state change path matched gpio=%d", gpio_num);
    }
}

static void check_restart_combo(void)
{
    // Check for restart combination (buttons 1, 2, and 3 held for 3+ seconds)
    bool btn1_pressed = gpio_get_level(BTN1_GPIO) == 0;
    bool btn2_pressed = gpio_get_level(BTN2_GPIO) == 0;
    bool btn3_pressed = gpio_get_level(BTN3_GPIO) == 0;
    bool restart_combo = btn1_pressed && btn2_pressed && btn3_pressed;

    if (restart_combo && !restart_combo_active)
    {
        restart_combo_active = true;
        restart_combo_start_time = xTaskGetTickCount();
        ESP_LOGI(TAG, "Restart combination detected, starting timer...");

        // Dispatch combo start event
        hid_event_data_t event = {
            .gpio_num = BTN1_GPIO, // Use first button as representative
            .event_type = HID_EVENT_COMBO_START,
            .duration_ms = 0,
            .combo_mask = (1 << 0) | (1 << 1) | (1 << 2) // BTN1, BTN2, BTN3
        };
        hid_event_dispatch(&event);
    }
    else if (!restart_combo && restart_combo_active)
    {
        restart_combo_active = false;
        ESP_LOGI(TAG, "Restart combination cancelled");

        // Dispatch combo end event
        hid_event_data_t event = {
            .gpio_num = BTN1_GPIO,
            .event_type = HID_EVENT_COMBO_END,
            .duration_ms = pdTICKS_TO_MS(xTaskGetTickCount() - restart_combo_start_time),
            .combo_mask = (1 << 0) | (1 << 1) | (1 << 2)};
        hid_event_dispatch(&event);
    }
    else if (restart_combo_active && restart_combo)
    {
        TickType_t current_time = xTaskGetTickCount();
        uint32_t elapsed_ms = pdTICKS_TO_MS(current_time - restart_combo_start_time);

        if (elapsed_ms >= HID_RESTART_COMBO_DURATION_MS)
        {
            ESP_LOGW(TAG, "Restart combination held for %lu ms, restarting system...", elapsed_ms);

            // Blink red LED for 1 second before restart
            led_color_t red_color = LED_COLOR_RED;
            led_color_t off_color = LED_COLOR_OFF;

            for (int i = 0; i < 5; i++)
            {
                led_mgr_set_color(red_color);
                vTaskDelay(pdMS_TO_TICKS(100));
                led_mgr_set_color(off_color);
                vTaskDelay(pdMS_TO_TICKS(100));
            }

            esp_restart();
        }
    }
}

static void hid_can_sleep_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "HID can sleep now");
    xEventGroupClearBits(power_mgr_tired_event_group, HID_TIRED_BIT);
}

static void hid_task(void *pvParameters)
{
    internal_button_event_t event;
    TickType_t last_combo_check = 0;

    ESP_LOGI(TAG, "HID task started");

    while (1)
    {
        // Process button events
        if (xQueueReceive(button_queue, &event, pdMS_TO_TICKS(50)))
        {
            process_button_event(event.gpio_num, event.pressed, event.timestamp);
        }

        // Check restart combo periodically
        TickType_t now = xTaskGetTickCount();
        if (pdTICKS_TO_MS(now - last_combo_check) >= 100)
        {
            check_restart_combo();
            last_combo_check = now;
        }
    }
}

esp_err_t hid_mgr_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing HID Manager...");

    // Initialize HID event system
    ret = hid_event_system_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize HID event system");
        return ret;
    }

    // Initialize LED Manager
    ret = led_mgr_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LED Manager");
        return ret;
    }

    // Initialize button states
    memset(button_states, 0, sizeof(button_states));
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        button_states[i].double_press_timer = xTimerCreate(
            "double_press_timer",
            pdMS_TO_TICKS(HID_DOUBLE_PRESS_WINDOW_MS),
            pdFALSE,
            NULL,
            double_press_timer_callback);

        if (!button_states[i].double_press_timer)
        {
            ESP_LOGE(TAG, "Failed to create double press timer for button %d", i);
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    // Create button event queue
    button_queue = xQueueCreate(20, sizeof(internal_button_event_t));
    if (button_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create button queue");
        ret = ESP_FAIL;
        goto cleanup;
    }

    // Configure GPIO pins for buttons
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE, // Both edges for press/release detection
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pin_bit_mask = 0};

    // Set pin bit mask for all buttons
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        io_conf.pin_bit_mask |= (1ULL << button_configs[i].gpio_num);
    }

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure GPIO pins, ret=%d", ret);
        goto cleanup;
    }

    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service, ret=%d", ret);
        goto cleanup;
    }

    // Add ISR handler for each button
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        ret = gpio_isr_handler_add(button_configs[i].gpio_num, gpio_isr_handler,
                                   (void *)(uintptr_t)button_configs[i].gpio_num);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to add ISR handler for %s", button_configs[i].name);
            goto cleanup;
        }
        ESP_LOGI(TAG, "Configured %s on GPIO %d", button_configs[i].name, button_configs[i].gpio_num);
    }

    // Initialize the can sleep timer
    hid_can_sleep_timer = xTimerCreate("hid_can_sleep_timer",
                                       pdMS_TO_TICKS(HID_CAN_SLEEP_TIMEOUT_MS),
                                       pdFALSE,
                                       NULL,
                                       hid_can_sleep_timer_callback);
    if (!hid_can_sleep_timer)
    {
        ESP_LOGE(TAG, "Failed to create can sleep timer");
        ret = ESP_FAIL;
        goto cleanup;
    }

    xTimerStart(hid_can_sleep_timer, 0);
    xEventGroupSetBits(power_mgr_tired_event_group, HID_TIRED_BIT);

    // Create HID task
    BaseType_t task_ret = xTaskCreate(hid_task, "hid_task", 4096, NULL, 5, &hid_task_handle);
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create HID task");
        ret = ESP_FAIL;
        goto cleanup;
    }

    ESP_LOGI(TAG, "HID Manager initialized successfully");
    return ESP_OK;

cleanup:
    hid_mgr_deinit();
    return ret;
}

esp_err_t hid_mgr_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing HID Manager...");

    // Delete task
    if (hid_task_handle != NULL)
    {
        vTaskDelete(hid_task_handle);
        hid_task_handle = NULL;
    }

    // Delete timers
    if (hid_can_sleep_timer)
    {
        xTimerDelete(hid_can_sleep_timer, portMAX_DELAY);
        hid_can_sleep_timer = NULL;
    }

    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        if (button_states[i].double_press_timer)
        {
            xTimerDelete(button_states[i].double_press_timer, portMAX_DELAY);
            button_states[i].double_press_timer = NULL;
        }
    }

    // Remove ISR handlers
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        gpio_isr_handler_remove(button_configs[i].gpio_num);
    }

    // Delete queue
    if (button_queue != NULL)
    {
        vQueueDelete(button_queue);
        button_queue = NULL;
    }

    // Deinitialize event system
    hid_event_system_deinit();

    ESP_LOGI(TAG, "HID Manager deinitialized");
    return ESP_OK;
}
