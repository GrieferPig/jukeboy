#include "hid_event_system.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "hid_event_system";

// Event listeners storage
static hid_listener_registration_t listeners[MAX_BUTTONS][MAX_LISTENERS_PER_BUTTON];
static int listener_counts[MAX_BUTTONS] = {0};
static bool initialized = false;

// GPIO to button index mapping
static gpio_num_t gpio_to_index_map[MAX_BUTTONS];
static int gpio_count = 0;

static int gpio_to_button_index(gpio_num_t gpio_num)
{
    for (int i = 0; i < gpio_count; i++)
    {
        if (gpio_to_index_map[i] == gpio_num)
        {
            return i;
        }
    }

    // Add new GPIO if space available
    if (gpio_count < MAX_BUTTONS)
    {
        gpio_to_index_map[gpio_count] = gpio_num;
        return gpio_count++;
    }

    return -1; // No space
}

esp_err_t hid_event_system_init(void)
{
    if (initialized)
    {
        return ESP_OK;
    }

    // Clear all listeners
    memset(listeners, 0, sizeof(listeners));
    memset(listener_counts, 0, sizeof(listener_counts));
    memset(gpio_to_index_map, 0, sizeof(gpio_to_index_map));
    gpio_count = 0;

    initialized = true;
    ESP_LOGI(TAG, "HID event system initialized");
    return ESP_OK;
}

esp_err_t hid_event_system_deinit(void)
{
    if (!initialized)
    {
        return ESP_OK;
    }

    // Clear all data
    memset(listeners, 0, sizeof(listeners));
    memset(listener_counts, 0, sizeof(listener_counts));
    gpio_count = 0;
    initialized = false;

    ESP_LOGI(TAG, "HID event system deinitialized");
    return ESP_OK;
}

esp_err_t hid_event_register_listener(gpio_num_t gpio_num,
                                      hid_event_listener_t callback,
                                      void *user_data,
                                      int priority)
{
    if (!initialized || !callback)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int button_idx = gpio_to_button_index(gpio_num);
    if (button_idx < 0)
    {
        ESP_LOGE(TAG, "No space for GPIO %d", gpio_num);
        return ESP_FAIL;
    }

    if (listener_counts[button_idx] >= MAX_LISTENERS_PER_BUTTON)
    {
        ESP_LOGE(TAG, "Max listeners reached for GPIO %d", gpio_num);
        return ESP_FAIL;
    }

    // Find insertion point to maintain priority order (highest first)
    int insert_idx = listener_counts[button_idx];
    for (int i = 0; i < listener_counts[button_idx]; i++)
    {
        if (priority > listeners[button_idx][i].priority)
        {
            insert_idx = i;
            break;
        }
    }

    // Shift existing listeners down
    for (int i = listener_counts[button_idx]; i > insert_idx; i--)
    {
        listeners[button_idx][i] = listeners[button_idx][i - 1];
    }

    // Insert new listener
    listeners[button_idx][insert_idx].callback = callback;
    listeners[button_idx][insert_idx].user_data = user_data;
    listeners[button_idx][insert_idx].priority = priority;
    listeners[button_idx][insert_idx].active = true;
    listener_counts[button_idx]++;

    ESP_LOGI(TAG, "Registered listener for GPIO %d with priority %d", gpio_num, priority);
    return ESP_OK;
}

esp_err_t hid_event_unregister_listener(gpio_num_t gpio_num,
                                        hid_event_listener_t callback)
{
    if (!initialized || !callback)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int button_idx = gpio_to_button_index(gpio_num);
    if (button_idx < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // Find and remove listener
    for (int i = 0; i < listener_counts[button_idx]; i++)
    {
        if (listeners[button_idx][i].callback == callback)
        {
            // Shift remaining listeners up
            for (int j = i; j < listener_counts[button_idx] - 1; j++)
            {
                listeners[button_idx][j] = listeners[button_idx][j + 1];
            }
            listener_counts[button_idx]--;
            ESP_LOGI(TAG, "Unregistered listener for GPIO %d", gpio_num);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t hid_event_set_listener_enabled(gpio_num_t gpio_num,
                                         hid_event_listener_t callback,
                                         bool enabled)
{
    if (!initialized || !callback)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int button_idx = gpio_to_button_index(gpio_num);
    if (button_idx < 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    for (int i = 0; i < listener_counts[button_idx]; i++)
    {
        if (listeners[button_idx][i].callback == callback)
        {
            listeners[button_idx][i].active = enabled;
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

bool hid_event_dispatch(const hid_event_data_t *event)
{
    if (!initialized || !event)
    {
        return false;
    }

    int button_idx = gpio_to_button_index(event->gpio_num);
    if (button_idx < 0)
    {
        return false;
    }

    // Call listeners in priority order (highest first)
    for (int i = 0; i < listener_counts[button_idx]; i++)
    {
        if (listeners[button_idx][i].active && listeners[button_idx][i].callback)
        {
            bool consumed = listeners[button_idx][i].callback(event, listeners[button_idx][i].user_data);
            if (consumed)
            {
                ESP_LOGD(TAG, "Event consumed by listener %d for GPIO %d", i, event->gpio_num);
                return true; // Event consumed, stop propagation
            }
        }
    }

    return false; // Event not consumed
}
