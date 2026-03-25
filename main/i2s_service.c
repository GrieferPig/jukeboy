#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "i2s_service.h"

#define I2S_SVC_TASK_STACK_SIZE 2048
#define I2S_SVC_TASK_PRIORITY 5
#define I2S_SVC_QUEUE_DEPTH 4

static const char *TAG = "i2s_svc";

typedef enum
{
    I2S_SVC_CMD_START,
    I2S_SVC_CMD_SUSPEND,
    I2S_SVC_CMD_REGISTER_PROVIDER,
} i2s_service_cmd_t;

typedef struct
{
    i2s_service_cmd_t cmd;
    i2s_service_pcm_provider_t provider;
    void *user_ctx;
} i2s_service_msg_t;

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task_handle;

static void i2s_service_task(void *param)
{
    (void)param;
    i2s_service_msg_t msg;

    for (;;)
    {
        xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY);

        switch (msg.cmd)
        {
        case I2S_SVC_CMD_START:
        case I2S_SVC_CMD_SUSPEND:
        case I2S_SVC_CMD_REGISTER_PROVIDER:
            /* Stub: no real I2S hardware — just consume and discard commands. */
            break;
        default:
            break;
        }
    }
}

static esp_err_t i2s_service_send(const i2s_service_msg_t *msg)
{
    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_queue, msg, pdMS_TO_TICKS(1000)) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t i2s_service_init(void)
{
    if (s_cmd_queue)
    {
        return ESP_OK;
    }

    s_cmd_queue = xQueueCreate(I2S_SVC_QUEUE_DEPTH, sizeof(i2s_service_msg_t));
    if (!s_cmd_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(i2s_service_task,
                                "i2s_svc",
                                I2S_SVC_TASK_STACK_SIZE,
                                NULL,
                                I2S_SVC_TASK_PRIORITY,
                                &s_task_handle,
                                0) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "I2S service started (stub transmitter)");
    return ESP_OK;
}

esp_err_t i2s_service_start_audio(void)
{
    i2s_service_msg_t msg = {.cmd = I2S_SVC_CMD_START};
    return i2s_service_send(&msg);
}

esp_err_t i2s_service_suspend_audio(void)
{
    i2s_service_msg_t msg = {.cmd = I2S_SVC_CMD_SUSPEND};
    return i2s_service_send(&msg);
}

esp_err_t i2s_service_register_pcm_provider(i2s_service_pcm_provider_t provider, void *user_ctx)
{
    i2s_service_msg_t msg = {
        .cmd = I2S_SVC_CMD_REGISTER_PROVIDER,
        .provider = provider,
        .user_ctx = user_ctx,
    };
    return i2s_service_send(&msg);
}