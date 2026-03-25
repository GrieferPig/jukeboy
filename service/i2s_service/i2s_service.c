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
#define I2S_SVC_PULL_BYTES 3840
#define I2S_SVC_PULL_PERIOD_MS 20

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
static bool s_running;
static i2s_service_pcm_provider_t s_provider;
static void *s_provider_ctx;
static uint8_t s_scratch[I2S_SVC_PULL_BYTES];

static void i2s_service_task(void *param)
{
    (void)param;
    i2s_service_msg_t msg;

    for (;;)
    {
        if (xQueueReceive(s_cmd_queue, &msg, pdMS_TO_TICKS(I2S_SVC_PULL_PERIOD_MS)) == pdPASS)
        {
            switch (msg.cmd)
            {
            case I2S_SVC_CMD_START:
                s_running = true;
                break;
            case I2S_SVC_CMD_SUSPEND:
                s_running = false;
                break;
            case I2S_SVC_CMD_REGISTER_PROVIDER:
                s_provider = msg.provider;
                s_provider_ctx = msg.user_ctx;
                break;
            default:
                break;
            }
        }

        if (s_running && s_provider)
        {
            memset(s_scratch, 0, sizeof(s_scratch));
            s_provider(s_scratch, (int32_t)sizeof(s_scratch), s_provider_ctx);
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