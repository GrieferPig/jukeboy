#include "hid_mgr.h"
#include "pindef.h"
#include "audio/audio_player.h"
#include "led_mgr.h"
#include "macros.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_system.h"
#include <stdlib.h>
#include "power_mgr.h"

static const char *TAG = "hid_mgr";

// Button configuration structure
typedef struct
{
    gpio_num_t gpio_num;
    CommandType cmd_type;
    const char *name;
} button_config_t;

// Button configurations
static const button_config_t button_configs[] = {
    {BTN1_GPIO, CMD_NEXT_TRACK, "BTN1 (Next Track)"},
    {BTN2_GPIO, CMD_TOGGLE_PAUSE, "BTN2 (Pause/Unpause)"},
    {BTN3_GPIO, CMD_PREV_TRACK, "BTN3 (Previous Track)"},
    {BTN4_GPIO, CMD_TOGGLE_SHUFFLE, "BTN4 (Toggle Shuffle)"},
    {BTN5_GPIO, CMD_VOLUME_INC, "BTN5 (Volume Up)"},
    {BTN6_GPIO, CMD_VOLUME_DEC, "BTN6 (Volume Down)"},
};

#define NUM_BUTTONS (sizeof(button_configs) / sizeof(button_config_t))

// Queue for button events
static QueueHandle_t button_queue = NULL;
static TaskHandle_t hid_task_handle = NULL;

// Restart combination tracking
static bool button_states[NUM_BUTTONS] = {false};
static TickType_t restart_combo_start_time = 0;
static bool restart_combo_active = false;

TimerHandle_t hid_can_sleep_timer = NULL;
#define HID_CAN_SLEEP_TIMEOUT_MS 10000
#define RESTART_COMBO_DURATION_MS 3000

// Button event structure
typedef struct
{
    gpio_num_t gpio_num;
    CommandType cmd_type;
} button_event_t;

// GPIO interrupt handler
static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    button_event_t event;
    event.gpio_num = (gpio_num_t)(uintptr_t)arg;

    // Find the corresponding command type
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        if (button_configs[i].gpio_num == event.gpio_num)
        {
            event.cmd_type = button_configs[i].cmd_type;
            break;
        }
    }

    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(button_queue, &event, &xHigherPriorityTaskWoken);

    // Reset the can sleep timer
    if (hid_can_sleep_timer != NULL)
    {
        xTimerResetFromISR(hid_can_sleep_timer, &xHigherPriorityTaskWoken);
        // Set the tired bit for HID
        xEventGroupSetBitsFromISR(power_mgr_tired_event_group, HID_TIRED_BIT, &xHigherPriorityTaskWoken);
    }

    if (xHigherPriorityTaskWoken)
    {
        portYIELD_FROM_ISR();
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
        // Start tracking the restart combination
        restart_combo_active = true;
        restart_combo_start_time = xTaskGetTickCount();
        ESP_LOGI(TAG, "Restart combination detected, starting timer...");
    }
    else if (!restart_combo && restart_combo_active)
    {
        // Combination released, cancel restart
        restart_combo_active = false;
        ESP_LOGI(TAG, "Restart combination cancelled");
    }
    else if (restart_combo_active && restart_combo)
    {
        // Check if combination has been held long enough
        TickType_t current_time = xTaskGetTickCount();
        TickType_t elapsed_ticks = current_time - restart_combo_start_time;
        uint32_t elapsed_ms = pdTICKS_TO_MS(elapsed_ticks);

        if (elapsed_ms >= RESTART_COMBO_DURATION_MS)
        {
            ESP_LOGW(TAG, "Restart combination held for %d ms, restarting system...", elapsed_ms);

            // Blink red LED for 1 second before restart
            led_color_t red_color = LED_COLOR_RED;
            led_color_t off_color = LED_COLOR_OFF;

            for (int i = 0; i < 5; i++) // 5 blinks = 1 second (200ms per cycle)
            {
                led_mgr_set_color(red_color);
                vTaskDelay(pdMS_TO_TICKS(100)); // On for 100ms
                led_mgr_set_color(off_color);
                vTaskDelay(pdMS_TO_TICKS(100)); // Off for 100ms
            }

            esp_restart();
        }
    }
}

static void hid_can_sleep_timer_callback(TimerHandle_t xTimer)
{
    // Timer reached, indicate that HID can sleep
    ESP_LOGI(TAG, "HID can sleep now");
    xEventGroupClearBitsFromISR(power_mgr_tired_event_group, HID_TIRED_BIT);
}

// HID manager task
static void hid_task(void *pvParameters)
{
    button_event_t event;
    AudioCommand audio_cmd;

    ESP_LOGI(TAG, "HID task started");

    while (1)
    {
        check_restart_combo();
        // Wait for button events (with timeout to check restart combo regularly)
        if (xQueueReceive(button_queue, &event, pdMS_TO_TICKS(100)))
        {
            // Skip normal button processing if restart combination is active
            if (restart_combo_active)
            {
                continue;
            }

            // Check if button is still pressed (debounce)
            if (gpio_get_level(event.gpio_num) == 0)
            {
                ESP_LOGI(TAG, "Button pressed: GPIO %d, Command: %d", event.gpio_num, event.cmd_type);

                // Set LED color based on button pressed
                led_color_t led_color;
                switch (event.cmd_type)
                {
                case CMD_NEXT_TRACK:
                    led_color = (led_color_t)LED_COLOR_BLUE;
                    break;
                case CMD_TOGGLE_PAUSE:
                    led_color = (led_color_t)LED_COLOR_YELLOW;
                    break;
                case CMD_PREV_TRACK:
                    led_color = (led_color_t)LED_COLOR_PURPLE;
                    break;
                case CMD_TOGGLE_SHUFFLE:
                    led_color = (led_color_t)LED_COLOR_CYAN;
                    break;
                case CMD_VOLUME_INC:
                    led_color = (led_color_t)LED_COLOR_GREEN;
                    break;
                case CMD_VOLUME_DEC:
                    led_color = (led_color_t)LED_COLOR_RED;
                    break;
                default:
                    led_color = (led_color_t)LED_COLOR_WHITE;
                    break;
                }

                // Send color change to LED manager
                led_mgr_set_color(led_color);

                // Prepare audio command
                audio_cmd.type = event.cmd_type;
                audio_cmd.params.track_number = 0; // Default value, not used for most commands

                // Send command to audio player
                audio_player_send_command(&audio_cmd);

                // Wait for button release to avoid multiple triggers
                // But continue checking restart combination during the wait
                while (gpio_get_level(event.gpio_num) == 0)
                {
                    check_restart_combo();
                    vTaskDelay(pdMS_TO_TICKS(50)); // Poll every 50ms
                }
                // Additional debounce delay
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }
    }
}

esp_err_t hid_mgr_init(void)
{
    esp_err_t ret = ESP_OK;

    ESP_LOGI(TAG, "Initializing HID Manager...");

    // Initialize LED Manager first
    ret = led_mgr_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize LED Manager");
        return ret;
    }

    // Create button event queue
    button_queue = xQueueCreate(10, sizeof(button_event_t));
    if (button_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create button queue");
        led_mgr_deinit();
        return ESP_FAIL;
    }

    // Configure GPIO pins for buttons
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_NEGEDGE, // Interrupt on falling edge (button press)
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, // Enable internal pull-up
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
        ESP_LOGE(TAG, "Failed to configure GPIO pins");
        goto cleanup;
    }

    // Install GPIO ISR service
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "Failed to install GPIO ISR service");
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

    // Print GPIO initial state for debugging
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        uint32_t gpio_state = gpio_get_level(button_configs[i].gpio_num);
        ESP_LOGI(TAG, "Initial state of %s (GPIO %d): %d", button_configs[i].name, button_configs[i].gpio_num, gpio_state);
    }

    // Initialize the can sleep timer
    hid_can_sleep_timer = xTimerCreate("hid_can_sleep_timer", pdMS_TO_TICKS(HID_CAN_SLEEP_TIMEOUT_MS), pdFALSE, NULL, &hid_can_sleep_timer_callback);
    xTimerStart(hid_can_sleep_timer, 0);
    // Set the initial tired bit for HID
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

    // Remove ISR handlers
    for (int i = 0; i < NUM_BUTTONS; i++)
    {
        gpio_isr_handler_remove(button_configs[i].gpio_num);
    }

    // Uninstall GPIO ISR service
    gpio_uninstall_isr_service();

    // Delete queue
    if (button_queue != NULL)
    {
        vQueueDelete(button_queue);
        button_queue = NULL;
    }

    // Deinitialize LED Manager
    led_mgr_deinit();

    ESP_LOGI(TAG, "HID Manager deinitialized");
    return ESP_OK;
}
