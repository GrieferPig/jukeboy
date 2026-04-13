#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "qemu_pcm_service.h"

#define QEMU_PCM_SVC_REG_BASE 0x3ff4f000u
#define QEMU_PCM_SVC_MAGIC 0x514d4350u
#define QEMU_PCM_SVC_BUFFER_SIZE 0x10000u

#define QEMU_PCM_SVC_REG_MAGIC 0x00u
#define QEMU_PCM_SVC_REG_BUFFER0_ADDR 0x08u
#define QEMU_PCM_SVC_REG_BUFFER1_ADDR 0x0cu
#define QEMU_PCM_SVC_REG_BUFFER_SIZE 0x10u
#define QEMU_PCM_SVC_REG_CURRENT_BUFFER 0x20u
#define QEMU_PCM_SVC_REG_QUEUED_MASK 0x24u
#define QEMU_PCM_SVC_REG_BUFFER0_LENGTH 0x2cu
#define QEMU_PCM_SVC_REG_BUFFER1_LENGTH 0x30u
#define QEMU_PCM_SVC_REG_BUFFER0_SUBMIT 0x34u
#define QEMU_PCM_SVC_REG_BUFFER1_SUBMIT 0x38u
#define QEMU_PCM_SVC_REG_CONTROL 0x3cu

#define QEMU_PCM_SVC_CONTROL_RESET BIT(0)

#define QEMU_PCM_SVC_FRAME_BYTES 4u
#define QEMU_PCM_SVC_SUBMIT_TARGET_BYTES (48000u * 2u * 2u * 40u / 1000u)

#define QEMU_PCM_SVC_TASK_STACK_SIZE 3072
#define QEMU_PCM_SVC_TASK_PRIORITY 6
#define QEMU_PCM_SVC_QUEUE_DEPTH 4
#define QEMU_PCM_SVC_POLL_MS 5

static const char *TAG = "qemu_pcm_svc";

typedef enum
{
    QEMU_PCM_SVC_CMD_START,
    QEMU_PCM_SVC_CMD_SUSPEND,
    QEMU_PCM_SVC_CMD_REGISTER_PROVIDER,
} qemu_pcm_service_cmd_t;

typedef struct
{
    qemu_pcm_service_cmd_t cmd;
    qemu_pcm_service_pcm_provider_t provider;
    void *user_ctx;
} qemu_pcm_service_msg_t;

static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_task_handle;
static qemu_pcm_service_pcm_provider_t s_pcm_provider;
static void *s_pcm_provider_ctx;
static bool s_running;
static uint8_t *s_buffer_ptrs[2];

static inline uint32_t qemu_pcm_service_reg_read(uint32_t offset)
{
    return *(volatile uint32_t *)(QEMU_PCM_SVC_REG_BASE + offset);
}

static inline void qemu_pcm_service_reg_write(uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(QEMU_PCM_SVC_REG_BASE + offset) = value;
}

static bool qemu_pcm_service_buffer_is_free(unsigned index, uint32_t current_buffer, uint32_t queued_mask)
{
    return current_buffer != index && (queued_mask & BIT(index)) == 0;
}

static bool qemu_pcm_service_fill_buffer(unsigned index)
{
    uint8_t *buffer;
    uint32_t submit_len = QEMU_PCM_SVC_SUBMIT_TARGET_BYTES;
    int32_t filled = 0;

    if (index >= 2 || s_buffer_ptrs[index] == NULL)
    {
        return false;
    }

    buffer = s_buffer_ptrs[index];
    if (submit_len > QEMU_PCM_SVC_BUFFER_SIZE)
    {
        submit_len = QEMU_PCM_SVC_BUFFER_SIZE;
    }

    if (s_pcm_provider)
    {
        filled = s_pcm_provider(buffer, (int32_t)submit_len, s_pcm_provider_ctx);
    }

    if (filled <= 0)
    {
        return false;
    }

    if ((uint32_t)filled > submit_len)
    {
        filled = (int32_t)submit_len;
    }

    filled &= ~(int32_t)(QEMU_PCM_SVC_FRAME_BYTES - 1u);
    if (filled <= 0)
    {
        return false;
    }

    qemu_pcm_service_reg_write(index == 0 ? QEMU_PCM_SVC_REG_BUFFER0_LENGTH : QEMU_PCM_SVC_REG_BUFFER1_LENGTH,
                               (uint32_t)filled);
    qemu_pcm_service_reg_write(index == 0 ? QEMU_PCM_SVC_REG_BUFFER0_SUBMIT : QEMU_PCM_SVC_REG_BUFFER1_SUBMIT, 1);
    return true;
}

static void qemu_pcm_service_fill_free_buffers(void)
{
    uint32_t current_buffer = qemu_pcm_service_reg_read(QEMU_PCM_SVC_REG_CURRENT_BUFFER);
    uint32_t queued_mask = qemu_pcm_service_reg_read(QEMU_PCM_SVC_REG_QUEUED_MASK);

    for (unsigned index = 0; index < 2; ++index)
    {
        if (!qemu_pcm_service_buffer_is_free(index, current_buffer, queued_mask))
        {
            continue;
        }

        if (!qemu_pcm_service_fill_buffer(index))
        {
            continue;
        }

        queued_mask |= BIT(index);
        if (current_buffer == UINT32_MAX)
        {
            current_buffer = index;
        }
    }
}

static void qemu_pcm_service_process_cmd(const qemu_pcm_service_msg_t *msg)
{
    switch (msg->cmd)
    {
    case QEMU_PCM_SVC_CMD_START:
        if (!s_running)
        {
            qemu_pcm_service_reg_write(QEMU_PCM_SVC_REG_CONTROL, QEMU_PCM_SVC_CONTROL_RESET);
            s_running = true;
            ESP_LOGI(TAG, "QEMU PCM playback enabled");
        }
        break;

    case QEMU_PCM_SVC_CMD_SUSPEND:
        if (s_running)
        {
            s_running = false;
            qemu_pcm_service_reg_write(QEMU_PCM_SVC_REG_CONTROL, QEMU_PCM_SVC_CONTROL_RESET);
            ESP_LOGI(TAG, "QEMU PCM playback disabled");
        }
        break;

    case QEMU_PCM_SVC_CMD_REGISTER_PROVIDER:
        s_pcm_provider = msg->provider;
        s_pcm_provider_ctx = msg->user_ctx;
        break;

    default:
        break;
    }
}

static void qemu_pcm_service_task(void *param)
{
    (void)param;
    qemu_pcm_service_msg_t msg;

    for (;;)
    {
        if (!s_running)
        {
            if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdPASS)
            {
                qemu_pcm_service_process_cmd(&msg);
            }
            continue;
        }

        while (xQueueReceive(s_cmd_queue, &msg, 0) == pdPASS)
        {
            qemu_pcm_service_process_cmd(&msg);
        }

        if (!s_running)
        {
            continue;
        }

        qemu_pcm_service_fill_free_buffers();
        vTaskDelay(pdMS_TO_TICKS(QEMU_PCM_SVC_POLL_MS));
    }
}

static esp_err_t qemu_pcm_service_send(const qemu_pcm_service_msg_t *msg)
{
    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return xQueueSend(s_cmd_queue, msg, pdMS_TO_TICKS(1000)) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t qemu_pcm_service_init(void)
{
    uint32_t buffer_size;

    if (s_cmd_queue)
    {
        return ESP_OK;
    }

    if (qemu_pcm_service_reg_read(QEMU_PCM_SVC_REG_MAGIC) != QEMU_PCM_SVC_MAGIC)
    {
        ESP_LOGE(TAG, "QEMU PCM device not detected at 0x%08x", QEMU_PCM_SVC_REG_BASE);
        return ESP_ERR_NOT_SUPPORTED;
    }

    buffer_size = qemu_pcm_service_reg_read(QEMU_PCM_SVC_REG_BUFFER_SIZE);
    if (buffer_size != QEMU_PCM_SVC_BUFFER_SIZE)
    {
        ESP_LOGE(TAG, "unexpected QEMU PCM buffer size %u", (unsigned)buffer_size);
        return ESP_ERR_INVALID_SIZE;
    }

    s_buffer_ptrs[0] = (uint8_t *)(uintptr_t)qemu_pcm_service_reg_read(QEMU_PCM_SVC_REG_BUFFER0_ADDR);
    s_buffer_ptrs[1] = (uint8_t *)(uintptr_t)qemu_pcm_service_reg_read(QEMU_PCM_SVC_REG_BUFFER1_ADDR);
    qemu_pcm_service_reg_write(QEMU_PCM_SVC_REG_CONTROL, QEMU_PCM_SVC_CONTROL_RESET);

    s_cmd_queue = xQueueCreate(QEMU_PCM_SVC_QUEUE_DEPTH, sizeof(qemu_pcm_service_msg_t));
    if (!s_cmd_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(qemu_pcm_service_task,
                                "qemu_pcm_svc",
                                QEMU_PCM_SVC_TASK_STACK_SIZE,
                                NULL,
                                QEMU_PCM_SVC_TASK_PRIORITY,
                                &s_task_handle,
                                1) != pdPASS)
    {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "QEMU PCM service ready (buffer0=%p buffer1=%p size=%u)",
             (void *)s_buffer_ptrs[0], (void *)s_buffer_ptrs[1], (unsigned)buffer_size);
    return ESP_OK;
}

esp_err_t qemu_pcm_service_start_audio(void)
{
    qemu_pcm_service_msg_t msg = {.cmd = QEMU_PCM_SVC_CMD_START};
    return qemu_pcm_service_send(&msg);
}

esp_err_t qemu_pcm_service_suspend_audio(void)
{
    qemu_pcm_service_msg_t msg = {.cmd = QEMU_PCM_SVC_CMD_SUSPEND};
    return qemu_pcm_service_send(&msg);
}

esp_err_t qemu_pcm_service_register_pcm_provider(qemu_pcm_service_pcm_provider_t provider, void *user_ctx)
{
    qemu_pcm_service_msg_t msg = {
        .cmd = QEMU_PCM_SVC_CMD_REGISTER_PROVIDER,
        .provider = provider,
        .user_ctx = user_ctx,
    };
    return qemu_pcm_service_send(&msg);
}
