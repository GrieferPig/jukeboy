#include "power_mgmt_service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"

ESP_EVENT_DEFINE_BASE(POWER_MGMT_SERVICE_EVENT);

#define POWER_MGMT_EVENT_POST_TIMEOUT_MS 1000
#define POWER_MGMT_MAX_SHUTDOWN_CALLBACKS 8

static const char *TAG = "power_mgmt_svc";

typedef struct
{
    power_mgmt_service_shutdown_callback_t callback;
    void *user_ctx;
    int priority;
} power_mgmt_service_shutdown_entry_t;

static SemaphoreHandle_t s_service_lock;
static power_mgmt_service_shutdown_entry_t s_shutdown_callbacks[POWER_MGMT_MAX_SHUTDOWN_CALLBACKS];
static size_t s_shutdown_callback_count;
static bool s_initialized;

esp_err_t power_mgmt_service_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    s_service_lock = xSemaphoreCreateMutex();
    if (s_service_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t power_mgmt_service_register_shutdown_callback(power_mgmt_service_shutdown_callback_t callback,
                                                        void *user_ctx,
                                                        int priority)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG, "shutdown callback is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);

    for (size_t index = 0; index < s_shutdown_callback_count; index++)
    {
        if (s_shutdown_callbacks[index].callback == callback &&
            s_shutdown_callbacks[index].user_ctx == user_ctx)
        {
            xSemaphoreGive(s_service_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_shutdown_callback_count >= POWER_MGMT_MAX_SHUTDOWN_CALLBACKS)
    {
        xSemaphoreGive(s_service_lock);
        return ESP_ERR_NO_MEM;
    }

    size_t insert_index = s_shutdown_callback_count;
    while (insert_index > 0 && priority < s_shutdown_callbacks[insert_index - 1].priority)
    {
        s_shutdown_callbacks[insert_index] = s_shutdown_callbacks[insert_index - 1];
        insert_index--;
    }

    s_shutdown_callbacks[insert_index].callback = callback;
    s_shutdown_callbacks[insert_index].user_ctx = user_ctx;
    s_shutdown_callbacks[insert_index].priority = priority;
    s_shutdown_callback_count++;

    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}

esp_err_t power_mgmt_service_reboot(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");

    power_mgmt_service_shutdown_entry_t callbacks[POWER_MGMT_MAX_SHUTDOWN_CALLBACKS];
    size_t callback_count = 0;

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    callback_count = s_shutdown_callback_count;
    for (size_t index = 0; index < callback_count; index++)
    {
        callbacks[index] = s_shutdown_callbacks[index];
    }
    xSemaphoreGive(s_service_lock);

    esp_err_t err = esp_event_post(POWER_MGMT_SERVICE_EVENT,
                                   POWER_MGMT_SERVICE_EVENT_SHUTDOWN,
                                   NULL,
                                   0,
                                   pdMS_TO_TICKS(POWER_MGMT_EVENT_POST_TIMEOUT_MS));
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t index = 0; index < callback_count; index++)
    {
        err = callbacks[index].callback(callbacks[index].user_ctx);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "shutdown callback %u failed: %s",
                     (unsigned)index,
                     esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "shutdown callbacks complete, restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}