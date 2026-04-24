#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "esp_log.h"

#include "i2s_service.h"
#include "pin_defs.h"
#include "power_mgmt_service.h"

/* 48 kHz, 16-bit stereo — one 20 ms Opus frame worth of PCM. */
#define I2S_SVC_SAMPLE_RATE 48000
#define I2S_SVC_FRAME_MS 20
#define I2S_SVC_PCM_BUF_BYTES \
    (I2S_SVC_SAMPLE_RATE * 2 * (int)sizeof(int16_t) * I2S_SVC_FRAME_MS / 1000)

#define I2S_SVC_DMA_DESC_NUM 4
#define I2S_SVC_DMA_FRAME_NUM 480

#define I2S_SVC_TASK_STACK_SIZE 3072
#define I2S_SVC_TASK_PRIORITY 5
#define I2S_SVC_QUEUE_DEPTH 4
#define I2S_SVC_CMD_POLL_MS 5
#define I2S_SVC_RAIL_SETTLE_MS 10
#define I2S_SVC_MUTE_SETTLE_MS 1

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
static i2s_chan_handle_t s_tx_channel;
static i2s_service_pcm_provider_t s_pcm_provider;
static void *s_pcm_provider_ctx;
static bool s_running;
static bool s_dac_rail_requested;
static uint8_t s_pcm_buf[I2S_SVC_PCM_BUF_BYTES];

static esp_err_t i2s_service_prepare_dac_power_off(void *user_ctx)
{
    (void)user_ctx;

    if (s_tx_channel == NULL || !s_running)
    {
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_disable(s_tx_channel);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "failed to stop I2S TX before DAC power off: %s", esp_err_to_name(err));
        return err;
    }

    s_running = false;
    ESP_LOGI(TAG, "I2S TX disabled for DAC power off");
    return ESP_OK;
}

static esp_err_t i2s_service_enable_output(void)
{
    esp_err_t err;

    if (!s_dac_rail_requested)
    {
        err = power_mgmt_service_rail_request(POWER_MGMT_RAIL_DAC);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "failed to request DAC rail: %s", esp_err_to_name(err));
            return err;
        }
        s_dac_rail_requested = true;
        vTaskDelay(pdMS_TO_TICKS(I2S_SVC_RAIL_SETTLE_MS));
    }

    err = i2s_channel_enable(s_tx_channel);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to enable I2S TX channel: %s", esp_err_to_name(err));
        if (s_dac_rail_requested)
        {
            power_mgmt_service_rail_release(POWER_MGMT_RAIL_DAC);
            s_dac_rail_requested = false;
        }
        return err;
    }

    err = power_mgmt_service_set_dac_muted(false);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to unmute DAC: %s", esp_err_to_name(err));
        i2s_channel_disable(s_tx_channel);
        if (s_dac_rail_requested)
        {
            power_mgmt_service_rail_release(POWER_MGMT_RAIL_DAC);
            s_dac_rail_requested = false;
        }
        return err;
    }

    s_running = true;
    ESP_LOGI(TAG, "I2S TX enabled");
    return ESP_OK;
}

static void i2s_service_disable_output(void)
{
    esp_err_t err = power_mgmt_service_set_dac_muted(true);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to mute DAC: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(I2S_SVC_MUTE_SETTLE_MS));

    err = i2s_channel_disable(s_tx_channel);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to disable I2S TX channel: %s", esp_err_to_name(err));
    }

    if (s_dac_rail_requested)
    {
        err = power_mgmt_service_rail_release(POWER_MGMT_RAIL_DAC);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to release DAC rail: %s", esp_err_to_name(err));
        }
        else
        {
            s_dac_rail_requested = false;
        }
    }

    s_running = false;
    ESP_LOGI(TAG, "I2S TX disabled");
}

static void i2s_service_process_cmd(const i2s_service_msg_t *msg)
{
    switch (msg->cmd)
    {
    case I2S_SVC_CMD_START:
        if (!s_running && s_tx_channel)
        {
            i2s_service_enable_output();
        }
        break;

    case I2S_SVC_CMD_SUSPEND:
        if (s_running && s_tx_channel)
        {
            i2s_service_disable_output();
        }
        break;

    case I2S_SVC_CMD_REGISTER_PROVIDER:
        s_pcm_provider = msg->provider;
        s_pcm_provider_ctx = msg->user_ctx;
        break;

    default:
        break;
    }
}

static void i2s_service_task(void *param)
{
    (void)param;
    i2s_service_msg_t msg;

    for (;;)
    {
        if (!s_running)
        {
            /* Idle — block until a command arrives. */
            if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdPASS)
            {
                i2s_service_process_cmd(&msg);
            }
            continue;
        }

        /* Drain any pending commands before the next write. */
        while (xQueueReceive(s_cmd_queue, &msg, 0) == pdPASS)
        {
            i2s_service_process_cmd(&msg);
        }

        if (!s_running)
        {
            continue;
        }

        /* Pull PCM from the registered provider, or silence on underrun. */
        int32_t filled = 0;
        if (s_pcm_provider)
        {
            filled = s_pcm_provider(s_pcm_buf, (int32_t)sizeof(s_pcm_buf), s_pcm_provider_ctx);
        }

        if (filled <= 0)
        {
            memset(s_pcm_buf, 0, sizeof(s_pcm_buf));
            filled = (int32_t)sizeof(s_pcm_buf);
        }
        else if ((size_t)filled < sizeof(s_pcm_buf))
        {
            memset(s_pcm_buf + filled, 0, sizeof(s_pcm_buf) - (size_t)filled);
            filled = (int32_t)sizeof(s_pcm_buf);
        }

        size_t bytes_written = 0;
        i2s_channel_write(s_tx_channel, s_pcm_buf, (size_t)filled, &bytes_written, portMAX_DELAY);
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
    esp_err_t err;

    if (s_cmd_queue)
    {
        return ESP_OK;
    }

    /* Allocate the I2S TX channel. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_SVC_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_SVC_DMA_FRAME_NUM;

    err = i2s_new_channel(&chan_cfg, &s_tx_channel, NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to allocate I2S TX channel: %s", esp_err_to_name(err));
        return err;
    }

    /* Configure standard (Philips) mode — 48 kHz, 16-bit stereo. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SVC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_GPIO_UNUSED,
            .ws = I2S_GPIO_UNUSED,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_channel, &std_cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to init I2S STD mode: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        return err;
    }

    err = power_mgmt_service_bind_dac_i2s_channel(s_tx_channel,
                                                  i2s_service_prepare_dac_power_off,
                                                  NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to bind DAC I2S pins to power management: %s", esp_err_to_name(err));
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        return err;
    }

    s_cmd_queue = xQueueCreate(I2S_SVC_QUEUE_DEPTH, sizeof(i2s_service_msg_t));
    if (!s_cmd_queue)
    {
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
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
        i2s_del_channel(s_tx_channel);
        s_tx_channel = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "I2S service started (48 kHz 16-bit stereo, BCLK=%d WS=%d DATA=%d)",
             HAL_I2S_BCLK_PIN, HAL_I2S_WS_PIN, HAL_I2S_DATA_PIN);
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