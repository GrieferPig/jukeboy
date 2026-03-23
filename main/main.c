/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/queue.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_a2dp_api.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "decoder/impl/esp_opus_dec.h"
#include "decoder/esp_audio_dec.h"
#include "esp_audio_types.h"
#include "bluetooth_service.h"
#include "wifi_service.h"
#include "console_service.h"
#include "cartridge_service.h"

static const char *TAG = "main";

/* ---- .opu file format ---- */
#define OPU_FILENAME "test.opu"

typedef struct __attribute__((packed))
{
    uint32_t header_len_in_blocks;
    uint32_t lookup_table_len;
} opu_file_header_t;

/* ---- Audio player configuration ---- */
#define PCM_STREAM_BUF_SIZE (16 * 1024)
#define OPU_FILE_PATH "/sdcard/" OPU_FILENAME
#define OPUS_PCM_FRAME_BYTES (48000 * 2 * 20 / 1000 * (int)sizeof(int16_t)) /* 3840 bytes per 20ms stereo frame */
#define READER_TASK_STACK 4096
#define READER_TASK_PRIORITY 5
#define DECODER_TASK_STACK (8192 + 4096)
#define DECODER_TASK_PRIORITY 6

/* ---- Shared state ---- */

/* Stream buffer: decoder (producer) → PCM provider callback (consumer) */
static StreamBufferHandle_t s_pcm_stream = NULL;

/* Queue from reader → decoder: carries pointers to 128KB PSRAM buffers */
typedef struct
{
    uint8_t *data;
    size_t len;
    size_t chunk_index;
    bool is_eof;
} chunk_msg_t;

static QueueHandle_t s_chunk_queue = NULL;
static TaskHandle_t s_reader_task = NULL;
static TaskHandle_t s_decoder_task = NULL;
static uint8_t s_decoder_pcm_buf[OPUS_PCM_FRAME_BYTES];
static esp_audio_dec_info_t s_decoder_info;
static esp_audio_dec_out_frame_t s_decoder_out_frame;

static void send_eof_message(void)
{
    chunk_msg_t eof = {.data = NULL, .len = 0, .chunk_index = 0, .is_eof = true};
    xQueueSend(s_chunk_queue, &eof, portMAX_DELAY);
}

/* ---- PCM provider for BT A2DP data callback ---- */

static int32_t opus_pcm_provider(uint8_t *data, int32_t len, void *user_ctx)
{
    (void)user_ctx;

    if (!s_pcm_stream)
    {
        memset(data, 0, (size_t)len);
        return len;
    }

    size_t received = xStreamBufferReceive(s_pcm_stream, data, (size_t)len, 0);
    if (received < (size_t)len)
    {
        memset(data + received, 0, (size_t)len - received);
    }
    return len;
}

/* ---- Reader task ---- */

static void reader_task(void *param)
{
    (void)param;
    size_t total_file_size = 0;

    /* First read: get header + lookup table + start of data */
    esp_err_t err = cartridge_service_read_chunk_async(OPU_FILENAME, 0, xTaskGetCurrentTaskHandle());
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "first async read request failed: %s", esp_err_to_name(err));
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    const uint8_t *buf = NULL;
    size_t buf_len = 0;
    err = cartridge_service_get_read_result(&buf, &buf_len);
    if (err != ESP_OK || buf_len < sizeof(opu_file_header_t))
    {
        ESP_LOGE(TAG, "failed to read file header");
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }

    /* Parse header */
    opu_file_header_t header;
    memcpy(&header, buf, sizeof(header));
    size_t data_offset = (size_t)header.header_len_in_blocks * 512U;
    size_t num_chunks = header.lookup_table_len;

    if (data_offset == 0 || num_chunks == 0)
    {
        ESP_LOGE(TAG, "invalid file header (data_offset=%u, chunks=%u)",
                 (unsigned)data_offset, (unsigned)num_chunks);
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }

    /* Load lookup table into PSRAM */
    size_t lt_bytes = num_chunks * sizeof(uint32_t);
    uint32_t *lookup_table = (uint32_t *)heap_caps_malloc(lt_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lookup_table)
    {
        ESP_LOGE(TAG, "failed to allocate lookup table (%u bytes)", (unsigned)lt_bytes);
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }

    FILE *opus_file = fopen(OPU_FILE_PATH, "rb");
    if (!opus_file)
    {
        ESP_LOGE(TAG, "failed to open %s for file size", OPU_FILE_PATH);
        free(lookup_table);
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }

    if (fseek(opus_file, 0, SEEK_END) != 0)
    {
        ESP_LOGE(TAG, "failed to seek %s", OPU_FILE_PATH);
        fclose(opus_file);
        free(lookup_table);
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }

    long file_size = ftell(opus_file);
    fclose(opus_file);
    if (file_size <= 0 || (size_t)file_size <= data_offset)
    {
        ESP_LOGE(TAG, "invalid file size %ld for data offset %u", file_size, (unsigned)data_offset);
        free(lookup_table);
        send_eof_message();
        vTaskDelete(NULL);
        return;
    }
    total_file_size = (size_t)file_size;

    size_t lt_offset = sizeof(opu_file_header_t);
    if (lt_offset + lt_bytes <= buf_len)
    {
        /* Lookup table fits in first read */
        memcpy(lookup_table, buf + lt_offset, lt_bytes);
    }
    else
    {
        /* Copy what we have from first read, then read the rest */
        size_t available = (lt_offset < buf_len) ? (buf_len - lt_offset) : 0;
        if (available > 0)
        {
            memcpy(lookup_table, buf + lt_offset, available);
        }

        size_t remaining = lt_bytes - available;
        size_t file_pos = lt_offset + available;
        /* Align to 4KB for remaining reads */
        size_t aligned_pos = file_pos & ~0xFFFU;
        size_t skip = file_pos - aligned_pos;

        while (remaining > 0)
        {
            err = cartridge_service_read_chunk_async(OPU_FILENAME, aligned_pos, xTaskGetCurrentTaskHandle());
            if (err != ESP_OK)
            {
                break;
            }
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            const uint8_t *rd = NULL;
            size_t rd_len = 0;
            err = cartridge_service_get_read_result(&rd, &rd_len);
            if (err != ESP_OK || rd_len <= skip)
            {
                break;
            }

            size_t usable = rd_len - skip;
            size_t copy = (usable < remaining) ? usable : remaining;
            memcpy((uint8_t *)lookup_table + (lt_bytes - remaining), rd + skip, copy);
            remaining -= copy;
            aligned_pos += CARTRIDGE_SERVICE_READ_BUF_SIZE;
            skip = 0;
        }

        if (remaining > 0)
        {
            ESP_LOGE(TAG, "failed to read full lookup table");
            free(lookup_table);
            send_eof_message();
            vTaskDelete(NULL);
            return;
        }
    }

    size_t total_data_bytes = total_file_size - data_offset;
    ESP_LOGI(TAG, "streaming %u chunks, data_offset=%u", (unsigned)num_chunks, (unsigned)data_offset);

    for (size_t chunk_index = 0; chunk_index < num_chunks; ++chunk_index)
    {
        size_t chunk_start = lookup_table[chunk_index];
        size_t chunk_end = (chunk_index + 1 < num_chunks) ? lookup_table[chunk_index + 1] : total_data_bytes;
        if (chunk_end < chunk_start)
        {
            err = ESP_ERR_INVALID_SIZE;
            ESP_LOGE(TAG, "invalid chunk offsets %u -> %u",
                     (unsigned)chunk_start,
                     (unsigned)chunk_end);
            break;
        }

        size_t chunk_len = chunk_end - chunk_start;
        if (chunk_len == 0)
        {
            continue;
        }

        uint8_t *chunk_buf = (uint8_t *)heap_caps_malloc(chunk_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!chunk_buf)
        {
            err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "failed to allocate chunk %u (%u bytes)",
                     (unsigned)chunk_index,
                     (unsigned)chunk_len);
            break;
        }

        size_t copied = 0;
        while (copied < chunk_len)
        {
            size_t file_pos = data_offset + chunk_start + copied;
            size_t aligned_pos = file_pos & ~0xFFFU;
            size_t skip = file_pos - aligned_pos;

            err = cartridge_service_read_chunk_async(OPU_FILENAME, aligned_pos, xTaskGetCurrentTaskHandle());
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "read request failed for chunk %u at offset %u: %s",
                         (unsigned)chunk_index,
                         (unsigned)aligned_pos,
                         esp_err_to_name(err));
                break;
            }

            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            const uint8_t *rd = NULL;
            size_t rd_len = 0;
            err = cartridge_service_get_read_result(&rd, &rd_len);
            if (err != ESP_OK || rd_len <= skip)
            {
                ESP_LOGE(TAG, "read result invalid for chunk %u at offset %u",
                         (unsigned)chunk_index,
                         (unsigned)aligned_pos);
                break;
            }

            size_t usable = rd_len - skip;
            size_t copy_len = chunk_len - copied;
            if (copy_len > usable)
            {
                copy_len = usable;
            }

            memcpy(chunk_buf + copied, rd + skip, copy_len);
            copied += copy_len;
        }

        if (copied != chunk_len)
        {
            if (err == ESP_OK)
            {
                err = ESP_FAIL;
            }
            free(chunk_buf);
            break;
        }

        chunk_msg_t msg = {
            .data = chunk_buf,
            .len = chunk_len,
            .chunk_index = chunk_index,
            .is_eof = false,
        };
        xQueueSend(s_chunk_queue, &msg, portMAX_DELAY);

        err = ESP_OK;
    }

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "reader task stopped on error: %s", esp_err_to_name(err));
    }

    if (lookup_table)
    {
        free(lookup_table);
    }

    send_eof_message();
    cartridge_service_close_file();
    ESP_LOGI(TAG, "reader task done");
    vTaskDelete(NULL);
}

/* ---- Decoder task ---- */

static void decoder_task(void *param)
{
    (void)param;
    void *opus_handle = NULL;

    /* Open opus decoder */
    esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    opus_cfg.channel = ESP_AUDIO_DUAL;
    opus_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_48K;
    opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    opus_cfg.self_delimited = false;

    esp_audio_err_t aerr = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_handle);
    if (aerr != ESP_AUDIO_ERR_OK || !opus_handle)
    {
        ESP_LOGE(TAG, "failed to open opus decoder: %d", aerr);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "opus decoder ready (48 kHz stereo, 20 ms frames)");

    memset(&s_decoder_info, 0, sizeof(s_decoder_info));
    s_decoder_out_frame = (esp_audio_dec_out_frame_t){
        .buffer = s_decoder_pcm_buf,
        .len = sizeof(s_decoder_pcm_buf),
        .needed_size = 0,
        .decoded_size = 0,
    };

    bool running = true;

    while (running)
    {
        chunk_msg_t msg;
        if (xQueueReceive(s_chunk_queue, &msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (msg.is_eof || !msg.data || msg.len == 0)
        {
            running = false;
            break;
        }

        /* Parse length-prefixed opus frames */
        size_t cursor = 0;
        while (cursor < msg.len)
        {
            if ((msg.len - cursor) < 1)
            {
                break;
            }

            const uint8_t *pkt = msg.data + cursor;
            uint8_t byte1 = pkt[0];
            size_t hdr_size = 1;
            size_t frame_len = 0;

            if (byte1 < 252)
            {
                frame_len = byte1;
            }
            else
            {
                if ((msg.len - cursor) < 2)
                {
                    ESP_LOGE(TAG, "partial packet header in chunk %u", (unsigned)msg.chunk_index);
                    break;
                }
                hdr_size = 2;
                frame_len = (pkt[1] * 4) + byte1;
            }

            size_t pkt_size = hdr_size + frame_len;
            if (pkt_size <= hdr_size)
            {
                cursor += hdr_size;
                continue;
            }

            if (pkt_size > (msg.len - cursor))
            {
                ESP_LOGE(TAG, "packet size %u exceeds remaining chunk data %u in chunk %u",
                         (unsigned)pkt_size,
                         (unsigned)(msg.len - cursor),
                         (unsigned)msg.chunk_index);
                break;
            }

            /* Decode this opus frame */
            esp_audio_dec_in_raw_t raw_in = {
                .buffer = (uint8_t *)(pkt + hdr_size),
                .len = (uint32_t)frame_len,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };

            s_decoder_out_frame.decoded_size = 0;
            esp_audio_err_t dec_err = esp_opus_dec_decode(opus_handle, &raw_in, &s_decoder_out_frame, &s_decoder_info);
            if (dec_err != ESP_AUDIO_ERR_OK)
            {
                ESP_LOGE(TAG, "opus decode error in chunk %u, packet_len=%u: %d",
                         (unsigned)msg.chunk_index,
                         (unsigned)frame_len,
                         dec_err);
            }

            if (dec_err == ESP_AUDIO_ERR_OK && s_decoder_out_frame.decoded_size > 0)
            {
                /* Write PCM to stream buffer (blocks until space available) */
                size_t written = 0;
                while (written < s_decoder_out_frame.decoded_size)
                {
                    written += xStreamBufferSend(s_pcm_stream,
                                                 s_decoder_pcm_buf + written,
                                                 s_decoder_out_frame.decoded_size - written,
                                                 portMAX_DELAY);
                }
            }

            cursor += pkt_size;
        }

        free(msg.data);
    }

    esp_opus_dec_close(opus_handle);
    ESP_LOGI(TAG, "decoder task done");
    vTaskDelete(NULL);
}

/* ---- Event handler: start playback when A2DP connects ---- */

static void on_bt_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    if (id == BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE)
    {
        esp_a2d_connection_state_t state = *(esp_a2d_connection_state_t *)event_data;
        if (state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            ESP_LOGI(TAG, "A2DP connected — starting opus audio playback");
            bluetooth_service_start_audio();

            /* Create reader and decoder tasks */
            if (!s_decoder_task)
            {
                xTaskCreatePinnedToCore(decoder_task, "opus_decoder",
                                        DECODER_TASK_STACK, NULL,
                                        DECODER_TASK_PRIORITY,
                                        &s_decoder_task, tskNO_AFFINITY);
            }
            if (!s_reader_task)
            {
                xTaskCreatePinnedToCore(reader_task, "sd_reader",
                                        READER_TASK_STACK, NULL,
                                        READER_TASK_PRIORITY,
                                        &s_reader_task, tskNO_AFFINITY);
            }
        }
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    /* Init and mount SD card */
    cartridge_service_config_t cart_cfg = CARTRIDGE_SERVICE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(cartridge_service_init(&cart_cfg));
    ESP_ERROR_CHECK(cartridge_service_mount());

    /* BT controller + Bluedroid */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    /* System services */
    ESP_ERROR_CHECK(bluetooth_service_init());
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_ERROR_CHECK(console_service_init());

    /* Register 48 kHz SBC stream endpoint */
    bluetooth_service_register_48k_sbc_endpoint();

    /* Create PCM stream buffer and chunk queue */
    s_pcm_stream = xStreamBufferCreate(PCM_STREAM_BUF_SIZE, 1);
    s_chunk_queue = xQueueCreate(1, sizeof(chunk_msg_t));

    /* Register opus PCM provider for A2DP data callback */
    ESP_ERROR_CHECK(bluetooth_service_register_pcm_provider(opus_pcm_provider, NULL));

    /* Start audio playback once A2DP connects */
    ESP_ERROR_CHECK(esp_event_handler_register(BLUETOOTH_SERVICE_EVENT,
                                               BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE,
                                               on_bt_event,
                                               NULL));

    /* Connect to last bonded A2DP device */
    bluetooth_service_connect_last_bonded_a2dp_device();
}
