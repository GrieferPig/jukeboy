#include "hid_event_system.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "hid_event_system";

// Event listeners storage: per button, per event type
static hid_listener_registration_t listeners[MAX_BUTTONS][HID_EVENT_TYPE_COUNT][MAX_LISTENERS_PER_BUTTON];
static int listener_counts[MAX_BUTTONS][HID_EVENT_TYPE_COUNT] = {0};
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

esp_err_t hid_event_register_listener_ex(gpio_num_t gpio_num,
                                         hid_event_type_t event_type,
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

    if (event_type < 0 || event_type >= HID_EVENT_TYPE_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (listener_counts[button_idx][event_type] >= MAX_LISTENERS_PER_BUTTON)
    {
        ESP_LOGE(TAG, "Max listeners reached for GPIO %d event %d", gpio_num, event_type);
        return ESP_FAIL;
    }

    // Find insertion point to maintain priority order (highest first)
    int insert_idx = listener_counts[button_idx][event_type];
    for (int i = 0; i < listener_counts[button_idx][event_type]; i++)
    {
        if (priority > listeners[button_idx][event_type][i].priority)
        {
            insert_idx = i;
            break;
        }
    }

    // Shift existing listeners down
    for (int i = listener_counts[button_idx][event_type]; i > insert_idx; i--)
    {
        listeners[button_idx][event_type][i] = listeners[button_idx][event_type][i - 1];
    }

    // Insert new listener
    listeners[button_idx][event_type][insert_idx].callback = callback;
    listeners[button_idx][event_type][insert_idx].user_data = user_data;
    listeners[button_idx][event_type][insert_idx].priority = priority;
    listeners[button_idx][event_type][insert_idx].active = true;
    listener_counts[button_idx][event_type]++;

    ESP_LOGI(TAG, "Registered listener for GPIO %d event %d with priority %d", gpio_num, event_type, priority);
    return ESP_OK;
}

esp_err_t hid_event_unregister_listener_ex(gpio_num_t gpio_num,
                                           hid_event_type_t event_type,
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

    if (event_type < 0 || event_type >= HID_EVENT_TYPE_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Find and remove listener
    for (int i = 0; i < listener_counts[button_idx][event_type]; i++)
    {
        if (listeners[button_idx][event_type][i].callback == callback)
        {
            // Shift remaining listeners up
            for (int j = i; j < listener_counts[button_idx][event_type] - 1; j++)
            {
                listeners[button_idx][event_type][j] = listeners[button_idx][event_type][j + 1];
            }
            listener_counts[button_idx][event_type]--;
            ESP_LOGI(TAG, "Unregistered listener for GPIO %d event %d", gpio_num, event_type);
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

esp_err_t hid_event_set_listener_enabled_ex(gpio_num_t gpio_num,
                                            hid_event_type_t event_type,
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

    if (event_type < 0 || event_type >= HID_EVENT_TYPE_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < listener_counts[button_idx][event_type]; i++)
    {
        if (listeners[button_idx][event_type][i].callback == callback)
        {
            listeners[button_idx][event_type][i].active = enabled;
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

    if (event->event_type < 0 || event->event_type >= HID_EVENT_TYPE_COUNT)
    {
        return false;
    }

    // Call listeners for this specific event type in priority order (highest first)
    for (int i = 0; i < listener_counts[button_idx][event->event_type]; i++)
    {
        if (listeners[button_idx][event->event_type][i].active && listeners[button_idx][event->event_type][i].callback)
        {
            bool consumed = listeners[button_idx][event->event_type][i].callback(event, listeners[button_idx][event->event_type][i].user_data);
            if (consumed)
            {
                ESP_LOGD(TAG, "Event consumed by listener %d for GPIO %d", i, event->gpio_num);
                return true; // Event consumed, stop propagation
            }
        }
    }

    return false; // Event not consumed
}

// Backward-compat wrappers operating across all event types
esp_err_t hid_event_register_listener(gpio_num_t gpio_num,
                                      hid_event_listener_t callback,
                                      void *user_data,
                                      int priority)
{
    for (int et = 0; et < HID_EVENT_TYPE_COUNT; ++et)
    {
        esp_err_t r = hid_event_register_listener_ex(gpio_num, (hid_event_type_t)et, callback, user_data, priority);
        if (r != ESP_OK)
            return r;
    }
    return ESP_OK;
}

esp_err_t hid_event_unregister_listener(gpio_num_t gpio_num,
                                        hid_event_listener_t callback)
{
    esp_err_t last = ESP_ERR_NOT_FOUND;
    for (int et = 0; et < HID_EVENT_TYPE_COUNT; ++et)
    {
        esp_err_t r = hid_event_unregister_listener_ex(gpio_num, (hid_event_type_t)et, callback);
        if (r == ESP_OK)
            last = ESP_OK;
    }
    return last;
}

esp_err_t hid_event_set_listener_enabled(gpio_num_t gpio_num,
                                         hid_event_listener_t callback,
                                         bool enabled)
{
    esp_err_t last = ESP_ERR_NOT_FOUND;
    for (int et = 0; et < HID_EVENT_TYPE_COUNT; ++et)
    {
        esp_err_t r = hid_event_set_listener_enabled_ex(gpio_num, (hid_event_type_t)et, callback, enabled);
        if (r == ESP_OK)
            last = ESP_OK;
    }
    return last;
}

bool hid_event_has_listener(gpio_num_t gpio_num, hid_event_type_t event_type)
{
    if (!initialized)
        return false;
    int button_idx = gpio_to_button_index(gpio_num);
    if (button_idx < 0 || event_type < 0 || event_type >= HID_EVENT_TYPE_COUNT)
        return false;
    for (int i = 0; i < listener_counts[button_idx][event_type]; ++i)
    {
        if (listeners[button_idx][event_type][i].callback && listeners[button_idx][event_type][i].active)
            return true;
    }
    return false;
}
