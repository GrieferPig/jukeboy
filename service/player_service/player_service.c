#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "decoder/esp_audio_dec.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_audio_types.h"

#include "cartridge_service.h"
#include "player_service.h"

#define PLAYER_SVC_PCM_STREAM_BUF_SIZE (16 * 1024)
#define PLAYER_SVC_CHUNK_BUF_COUNT 2
#define PLAYER_SVC_CHUNK_MAX_BYTES 4096
#define PLAYER_SVC_READER_TASK_STACK 4096
#define PLAYER_SVC_READER_TASK_PRIORITY 5
#define PLAYER_SVC_DECODER_TASK_STACK (8192 + 4096)
#define PLAYER_SVC_DECODER_TASK_PRIORITY 6
#define PLAYER_SVC_TASK_STACK 4096
#define PLAYER_SVC_TASK_PRIORITY 5
#define PLAYER_SVC_QUEUE_DEPTH 8
#define PLAYER_SVC_MAX_TRACKS 128
#define PLAYER_SVC_MAX_TRACK_NAME_LEN 256
#define PLAYER_SVC_PCM_FRAME_BYTES (48000 * 2 * 20 / 1000 * (int)sizeof(int16_t))

static const char *TAG = "player_svc";

ESP_EVENT_DEFINE_BASE(PLAYER_SERVICE_EVENT);

typedef struct __attribute__((packed))
{
    uint32_t header_len_in_blocks;
    uint32_t lookup_table_len;
} player_service_opu_file_header_t;

typedef struct
{
    uint8_t *data;
    size_t len;
    size_t chunk_index;
    bool is_eof;
    bool release_buf;
} player_service_chunk_msg_t;

typedef enum
{
    PLAYER_SVC_CMD_CARTRIDGE_INSERTED,
    PLAYER_SVC_CMD_TRACK_COMPLETE,
} player_service_cmd_t;

typedef struct
{
    player_service_cmd_t cmd;
} player_service_msg_t;

static StreamBufferHandle_t s_pcm_stream;
static QueueHandle_t s_chunk_queue;
static SemaphoreHandle_t s_buf_pool_sem;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_reader_task;
static TaskHandle_t s_decoder_task;
static TaskHandle_t s_service_task;
static bool s_initialised;
static bool s_playing;
static size_t s_playlist_count;
static size_t s_playlist_index;
static char s_current_track[PLAYER_SVC_MAX_TRACK_NAME_LEN];
static uint8_t s_decoder_pcm_buf[PLAYER_SVC_PCM_FRAME_BYTES];
static esp_audio_dec_info_t s_decoder_info;
static esp_audio_dec_out_frame_t s_decoder_out_frame;
EXT_RAM_BSS_ATTR static char s_playlist[PLAYER_SVC_MAX_TRACKS][PLAYER_SVC_MAX_TRACK_NAME_LEN];

static void player_service_post_event(player_service_event_id_t event_id, const void *data, size_t len)
{
    esp_event_post(PLAYER_SERVICE_EVENT, event_id, data, len, 0);
}

static bool player_service_queue_cmd(player_service_cmd_t cmd)
{
    player_service_msg_t msg = {.cmd = cmd};
    return s_cmd_queue && xQueueSend(s_cmd_queue, &msg, 0) == pdPASS;
}

static void player_service_send_eof_message(void)
{
    player_service_chunk_msg_t eof = {.is_eof = true};
    xQueueSend(s_chunk_queue, &eof, portMAX_DELAY);
}

static bool player_service_has_opu_extension(const char *name)
{
    size_t len;

    if (!name)
    {
        return false;
    }

    len = strlen(name);
    if (len < 4)
    {
        return false;
    }

    return tolower((unsigned char)name[len - 4]) == '.' &&
           tolower((unsigned char)name[len - 3]) == 'o' &&
           tolower((unsigned char)name[len - 2]) == 'p' &&
           tolower((unsigned char)name[len - 1]) == 'u';
}

static int player_service_casecmp(const char *lhs, const char *rhs)
{
    while (*lhs && *rhs)
    {
        int lhs_char = tolower((unsigned char)*lhs);
        int rhs_char = tolower((unsigned char)*rhs);
        if (lhs_char != rhs_char)
        {
            return lhs_char - rhs_char;
        }
        lhs++;
        rhs++;
    }
    return tolower((unsigned char)*lhs) - tolower((unsigned char)*rhs);
}

static int player_service_playlist_compare(const void *lhs, const void *rhs)
{
    return player_service_casecmp((const char *)lhs, (const char *)rhs);
}

static void player_service_scan_playlist(void)
{
    DIR *dir;
    struct dirent *entry;
    const char *mount_point = cartridge_service_get_mount_point();

    s_playlist_count = 0;
    s_playlist_index = 0;

    if (!mount_point)
    {
        ESP_LOGE(TAG, "cartridge mount point unavailable");
        return;
    }

    dir = opendir(mount_point);
    if (!dir)
    {
        ESP_LOGE(TAG, "failed to open playlist root %s", mount_point);
        return;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
        {
            continue;
        }

        if (!player_service_has_opu_extension(entry->d_name))
        {
            continue;
        }

        if (s_playlist_count >= PLAYER_SVC_MAX_TRACKS)
        {
            ESP_LOGW(TAG, "playlist full, ignoring remaining files");
            break;
        }

        strncpy(s_playlist[s_playlist_count], entry->d_name, PLAYER_SVC_MAX_TRACK_NAME_LEN - 1);
        s_playlist[s_playlist_count][PLAYER_SVC_MAX_TRACK_NAME_LEN - 1] = '\0';
        s_playlist_count++;
    }

    closedir(dir);

    if (s_playlist_count > 1)
    {
        qsort(s_playlist, s_playlist_count, sizeof(s_playlist[0]), player_service_playlist_compare);
    }

    ESP_LOGI(TAG, "playlist contains %u .opu file(s)", (unsigned)s_playlist_count);
    player_service_post_event(PLAYER_SVC_EVENT_PLAYLIST_READY, &s_playlist_count, sizeof(s_playlist_count));
}

static void player_service_reader_task(void *param)
{
    const char *filename = (const char *)param;
    char full_path[PLAYER_SVC_MAX_TRACK_NAME_LEN + 64];
    size_t total_file_size = 0;
    uint32_t *lookup_table = NULL;
    const uint8_t *buf = NULL;
    size_t buf_len = 0;
    esp_err_t err;

    err = cartridge_service_read_chunk_async(filename, 0, xTaskGetCurrentTaskHandle());
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "first async read request failed for %s: %s", filename, esp_err_to_name(err));
        goto done;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    err = cartridge_service_get_read_result(&buf, &buf_len);
    if (err != ESP_OK || buf_len < sizeof(player_service_opu_file_header_t))
    {
        ESP_LOGE(TAG, "failed to read file header for %s", filename);
        goto done;
    }

    player_service_opu_file_header_t header;
    memcpy(&header, buf, sizeof(header));
    size_t data_offset = (size_t)header.header_len_in_blocks * 512U;
    size_t num_chunks = header.lookup_table_len;

    if (data_offset == 0 || num_chunks == 0)
    {
        ESP_LOGE(TAG, "invalid file header for %s", filename);
        err = ESP_FAIL;
        goto done;
    }

    size_t lt_bytes = num_chunks * sizeof(uint32_t);
    lookup_table = (uint32_t *)heap_caps_malloc(lt_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lookup_table)
    {
        ESP_LOGE(TAG, "failed to allocate lookup table (%u bytes)", (unsigned)lt_bytes);
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", cartridge_service_get_mount_point(), filename);
    FILE *opus_file = fopen(full_path, "rb");
    if (!opus_file)
    {
        ESP_LOGE(TAG, "failed to open %s for file size", full_path);
        err = ESP_ERR_NOT_FOUND;
        goto done;
    }

    if (fseek(opus_file, 0, SEEK_END) != 0)
    {
        ESP_LOGE(TAG, "failed to seek %s", full_path);
        fclose(opus_file);
        err = ESP_FAIL;
        goto done;
    }

    long file_size = ftell(opus_file);
    fclose(opus_file);
    if (file_size <= 0 || (size_t)file_size <= data_offset)
    {
        ESP_LOGE(TAG, "invalid file size %ld for %s", file_size, full_path);
        err = ESP_FAIL;
        goto done;
    }
    total_file_size = (size_t)file_size;

    size_t lt_offset = sizeof(player_service_opu_file_header_t);
    if (lt_offset + lt_bytes <= buf_len)
    {
        memcpy(lookup_table, buf + lt_offset, lt_bytes);
    }
    else
    {
        size_t available = (lt_offset < buf_len) ? (buf_len - lt_offset) : 0;
        if (available > 0)
        {
            memcpy(lookup_table, buf + lt_offset, available);
        }

        size_t remaining = lt_bytes - available;
        size_t file_pos = lt_offset + available;
        size_t aligned_pos = file_pos & ~0xFFFU;
        size_t skip = file_pos - aligned_pos;

        while (remaining > 0)
        {
            err = cartridge_service_read_chunk_async(filename, aligned_pos, xTaskGetCurrentTaskHandle());
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
            ESP_LOGE(TAG, "failed to read full lookup table for %s", filename);
            err = ESP_FAIL;
            goto done;
        }
    }

    size_t total_data_bytes = total_file_size - data_offset;
    const uint8_t *rd = NULL;
    size_t rd_len = 0;
    size_t window_start = 0;

    for (size_t chunk_index = 0; chunk_index < num_chunks; ++chunk_index)
    {
        size_t chunk_start = lookup_table[chunk_index];
        size_t chunk_end = (chunk_index + 1 < num_chunks) ? lookup_table[chunk_index + 1] : total_data_bytes;
        configASSERT(chunk_end >= chunk_start && (chunk_end - chunk_start) <= PLAYER_SVC_CHUNK_MAX_BYTES);

        size_t chunk_len = chunk_end - chunk_start;
        if (chunk_len == 0)
        {
            continue;
        }

        bool fits = (rd != NULL) &&
                    (chunk_start >= window_start) &&
                    (chunk_end <= window_start + rd_len);

        if (!fits)
        {
            if (rd != NULL)
            {
                player_service_chunk_msg_t flush = {.release_buf = true};
                xQueueSend(s_chunk_queue, &flush, portMAX_DELAY);
            }

            xSemaphoreTake(s_buf_pool_sem, portMAX_DELAY);

            err = cartridge_service_read_chunk_async(filename,
                                                     data_offset + chunk_start,
                                                     xTaskGetCurrentTaskHandle());
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "read request failed for %s chunk %u: %s",
                         filename,
                         (unsigned)chunk_index,
                         esp_err_to_name(err));
                xSemaphoreGive(s_buf_pool_sem);
                rd = NULL;
                goto done;
            }

            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

            err = cartridge_service_get_read_result(&rd, &rd_len);
            if (err != ESP_OK || rd_len < chunk_len)
            {
                ESP_LOGE(TAG, "read result invalid for %s chunk %u", filename, (unsigned)chunk_index);
                xSemaphoreGive(s_buf_pool_sem);
                rd = NULL;
                if (err == ESP_OK)
                {
                    err = ESP_FAIL;
                }
                goto done;
            }

            window_start = chunk_start;
        }

        player_service_chunk_msg_t msg = {
            .data = (uint8_t *)(rd + (chunk_start - window_start)),
            .len = chunk_len,
            .chunk_index = chunk_index,
            .is_eof = false,
            .release_buf = false,
        };
        xQueueSend(s_chunk_queue, &msg, portMAX_DELAY);
    }

    err = ESP_OK;

done:
    if (lookup_table)
    {
        free(lookup_table);
    }
    player_service_send_eof_message();
    cartridge_service_close_file();
    s_reader_task = NULL;
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "reader task stopped on error for %s: %s", filename, esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "reader task done for %s", filename);
    }
    vTaskDelete(NULL);
}

static void player_service_decoder_task(void *param)
{
    (void)param;
    void *opus_handle = NULL;

    esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
    opus_cfg.channel = ESP_AUDIO_DUAL;
    opus_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_48K;
    opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
    opus_cfg.self_delimited = false;

    esp_audio_err_t aerr = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_handle);
    if (aerr != ESP_AUDIO_ERR_OK || !opus_handle)
    {
        ESP_LOGE(TAG, "failed to open opus decoder: %d", aerr);
        s_decoder_task = NULL;
        player_service_queue_cmd(PLAYER_SVC_CMD_TRACK_COMPLETE);
        vTaskDelete(NULL);
        return;
    }

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
        player_service_chunk_msg_t msg;
        if (xQueueReceive(s_chunk_queue, &msg, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (msg.is_eof)
        {
            running = false;
            break;
        }

        if (msg.release_buf)
        {
            xSemaphoreGive(s_buf_pool_sem);
            continue;
        }

        size_t cursor = 0;
        while (cursor < msg.len)
        {
            const uint8_t *pkt = msg.data + cursor;
            uint8_t byte1 = pkt[0];
            size_t hdr_size = 1;
            size_t frame_len;

            if ((msg.len - cursor) < 1)
            {
                break;
            }

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

            esp_audio_dec_in_raw_t raw_in = {
                .buffer = (uint8_t *)(pkt + hdr_size),
                .len = (uint32_t)frame_len,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
            };

            s_decoder_out_frame.decoded_size = 0;
            esp_audio_err_t dec_err = esp_opus_dec_decode(opus_handle, &raw_in, &s_decoder_out_frame, &s_decoder_info);
            if (dec_err == ESP_AUDIO_ERR_OK && s_decoder_out_frame.decoded_size > 0)
            {
                size_t written = 0;
                while (written < s_decoder_out_frame.decoded_size)
                {
                    written += xStreamBufferSend(s_pcm_stream,
                                                 s_decoder_pcm_buf + written,
                                                 s_decoder_out_frame.decoded_size - written,
                                                 portMAX_DELAY);
                }
            }
            else if (dec_err != ESP_AUDIO_ERR_OK)
            {
                ESP_LOGE(TAG, "opus decode error in chunk %u, packet_len=%u: %d",
                         (unsigned)msg.chunk_index,
                         (unsigned)frame_len,
                         dec_err);
            }

            cursor += pkt_size;
        }
    }

    esp_opus_dec_close(opus_handle);
    s_decoder_task = NULL;
    player_service_queue_cmd(PLAYER_SVC_CMD_TRACK_COMPLETE);
    ESP_LOGI(TAG, "decoder task done for %s", s_current_track);
    vTaskDelete(NULL);
}

static void player_service_start_track(size_t index)
{
    if (index >= s_playlist_count)
    {
        return;
    }

    if (s_reader_task || s_decoder_task)
    {
        ESP_LOGW(TAG, "cannot start track while playback tasks are still active");
        return;
    }

    strncpy(s_current_track, s_playlist[index], sizeof(s_current_track) - 1);
    s_current_track[sizeof(s_current_track) - 1] = '\0';
    s_playlist_index = index;
    s_playing = true;

    xQueueReset(s_chunk_queue);
    xStreamBufferReset(s_pcm_stream);

    if (xTaskCreatePinnedToCore(player_service_decoder_task,
                                "player_decoder",
                                PLAYER_SVC_DECODER_TASK_STACK,
                                NULL,
                                PLAYER_SVC_DECODER_TASK_PRIORITY,
                                &s_decoder_task,
                                1) != pdPASS)
    {
        s_decoder_task = NULL;
        s_playing = false;
        ESP_LOGE(TAG, "failed to create decoder task");
        return;
    }

    if (xTaskCreatePinnedToCore(player_service_reader_task,
                                "player_reader",
                                PLAYER_SVC_READER_TASK_STACK,
                                s_current_track,
                                PLAYER_SVC_READER_TASK_PRIORITY,
                                &s_reader_task,
                                1) != pdPASS)
    {
        s_reader_task = NULL;
        s_playing = false;
        player_service_send_eof_message();
        ESP_LOGE(TAG, "failed to create reader task");
        return;
    }

    ESP_LOGI(TAG, "starting track %u/%u: %s",
             (unsigned)(index + 1),
             (unsigned)s_playlist_count,
             s_current_track);
    player_service_post_event(PLAYER_SVC_EVENT_TRACK_STARTED, s_current_track, strlen(s_current_track) + 1);
}

static void player_service_handle_cartridge_inserted(void)
{
    if (!cartridge_service_is_mounted())
    {
        esp_err_t err = cartridge_service_mount();
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "failed to mount cartridge filesystem: %s", esp_err_to_name(err));
            return;
        }
    }

    player_service_scan_playlist();
    if (s_playlist_count == 0)
    {
        ESP_LOGW(TAG, "no .opu files found in cartridge root");
        s_playing = false;
        return;
    }

    player_service_start_track(0);
}

static void player_service_task(void *param)
{
    (void)param;
    player_service_msg_t msg;

    for (;;)
    {
        if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        switch (msg.cmd)
        {
        case PLAYER_SVC_CMD_CARTRIDGE_INSERTED:
            player_service_handle_cartridge_inserted();
            break;
        case PLAYER_SVC_CMD_TRACK_COMPLETE:
            if (s_playing)
            {
                player_service_post_event(PLAYER_SVC_EVENT_TRACK_FINISHED, s_current_track, strlen(s_current_track) + 1);
            }
            if (s_playlist_count == 0)
            {
                s_playing = false;
                break;
            }
            if (s_playlist_index + 1 < s_playlist_count)
            {
                player_service_start_track(s_playlist_index + 1);
            }
            else
            {
                s_playing = false;
                ESP_LOGI(TAG, "playlist complete");
            }
            break;
        default:
            break;
        }
    }
}

static void player_service_on_cartridge_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_data;

    if (id == CARTRIDGE_SVC_EVENT_INSERTED)
    {
        if (!player_service_queue_cmd(PLAYER_SVC_CMD_CARTRIDGE_INSERTED))
        {
            ESP_LOGW(TAG, "dropped cartridge inserted event");
        }
    }
}

esp_err_t player_service_init(void)
{
    esp_err_t err;

    if (s_initialised)
    {
        return ESP_OK;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    s_pcm_stream = xStreamBufferCreate(PLAYER_SVC_PCM_STREAM_BUF_SIZE, 1);
    s_chunk_queue = xQueueCreate(PLAYER_SVC_CHUNK_BUF_COUNT, sizeof(player_service_chunk_msg_t));
    s_buf_pool_sem = xSemaphoreCreateCounting(PLAYER_SVC_CHUNK_BUF_COUNT, PLAYER_SVC_CHUNK_BUF_COUNT);
    s_cmd_queue = xQueueCreate(PLAYER_SVC_QUEUE_DEPTH, sizeof(player_service_msg_t));
    if (!s_pcm_stream || !s_chunk_queue || !s_buf_pool_sem || !s_cmd_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(player_service_task,
                                "player_svc",
                                PLAYER_SVC_TASK_STACK,
                                NULL,
                                PLAYER_SVC_TASK_PRIORITY,
                                &s_service_task,
                                0) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(CARTRIDGE_SERVICE_EVENT,
                                               CARTRIDGE_SVC_EVENT_INSERTED,
                                               player_service_on_cartridge_event,
                                               NULL));

    s_initialised = true;
    player_service_post_event(PLAYER_SVC_EVENT_STARTED, NULL, 0);
    if (cartridge_service_is_inserted())
    {
        player_service_queue_cmd(PLAYER_SVC_CMD_CARTRIDGE_INSERTED);
    }
    ESP_LOGI(TAG, "player service started");
    return ESP_OK;
}

int32_t player_service_pcm_provider(uint8_t *data, int32_t len, void *user_ctx)
{
    (void)user_ctx;

    if (!data || len <= 0)
    {
        return 0;
    }

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