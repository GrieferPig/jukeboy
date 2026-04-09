#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "nvs.h"

#include "decoder/esp_audio_dec.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_audio_types.h"

#include "cartridge_service.h"
#include "jukeboy_formats.h"
#include "player_service.h"
#include "power_mgmt_service.h"

#include "esp_random.h"

#define PLAYER_SVC_PCM_STREAM_BUF_SIZE (16 * 1024)
#define PLAYER_SVC_CHUNK_BUF_COUNT 2
#define PLAYER_SVC_CHUNK_MAX_BYTES (24 * 1024)
#define PLAYER_SVC_READER_TASK_STACK 4096
#define PLAYER_SVC_READER_TASK_PRIORITY 5
#define PLAYER_SVC_DECODER_TASK_STACK (8192 + 4096)
#define PLAYER_SVC_DECODER_TASK_PRIORITY 6
#define PLAYER_SVC_TASK_STACK 4096
#define PLAYER_SVC_TASK_PRIORITY 5
#define PLAYER_SVC_QUEUE_DEPTH 8
#define PLAYER_SVC_MAX_TRACK_FILENAME_LEN 16
#define PLAYER_SVC_MAX_TRACK_TITLE_LEN JUKEBOY_JBM_TRACK_NAME_BYTES
#define PLAYER_SVC_FULL_PATH_LEN 32
#define PLAYER_SVC_PCM_FRAME_BYTES (48000 * 2 * 20 / 1000 * (int)sizeof(int16_t))
#define PLAYER_SVC_FAST_SEEK_SECONDS 5U
#define PLAYER_SVC_PREVIOUS_RESTART_SECONDS 7U
#define PLAYER_SVC_VOLUME_Q8_SHIFT 8
#define PLAYER_SVC_VOLUME_Q8_ONE (1U << PLAYER_SVC_VOLUME_Q8_SHIFT)
#define PLAYER_SVC_CHUNK_CRC_BYTES sizeof(uint32_t)
#define PLAYER_SVC_QUEUE_POLL_MS 50
#define PLAYER_SVC_CMD_TIMEOUT_MS 1000
#define PLAYER_SVC_STOP_POLL_MS 10
#define PLAYER_SVC_STOP_TIMEOUT_LOOPS 200
#define PLAYER_SVC_OPUS_MAX_FRAME_LEN 1275
#define PLAYER_SVC_NVS_NAMESPACE "player_service"
#define PLAYER_SVC_NVS_KEY_RESUME "resume"
#define PLAYER_SVC_RESUME_STATE_VERSION 1U

#define PLAYER_SVC_VOLUME_LEVEL_COUNT 11
#define PLAYER_SVC_DEFAULT_VOLUME_LEVEL (PLAYER_SVC_VOLUME_LEVEL_COUNT - 1)

static const char *TAG = "player_svc";
static const uint16_t s_volume_gain_q8[PLAYER_SVC_VOLUME_LEVEL_COUNT] = {
    0,
    26,
    51,
    77,
    102,
    128,
    154,
    179,
    205,
    230,
    256,
};

ESP_EVENT_DEFINE_BASE(PLAYER_SERVICE_EVENT);

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
    PLAYER_SVC_CMD_CARTRIDGE_REMOVED,
    PLAYER_SVC_CMD_TRACK_COMPLETE,
    PLAYER_SVC_CMD_CONTROL,
    PLAYER_SVC_CMD_PERSIST_FOR_SHUTDOWN,
} player_service_cmd_t;

typedef struct
{
    player_service_cmd_t cmd;
    player_service_control_t control;
    uint32_t generation;
    SemaphoreHandle_t completion_semaphore;
    esp_err_t *result_out;
} player_service_msg_t;

typedef struct
{
    char filename[PLAYER_SVC_MAX_TRACK_FILENAME_LEN];
    char title[PLAYER_SVC_MAX_TRACK_TITLE_LEN];
} player_service_playlist_entry_t;

typedef struct
{
    char filename[PLAYER_SVC_MAX_TRACK_FILENAME_LEN];
    uint32_t *lookup_table;
    size_t lookup_table_len;
    size_t previous_restart_offset;
    size_t data_offset;
    size_t total_file_size;
} player_service_track_info_t;

typedef struct
{
    uint32_t version;
    uint32_t cartridge_checksum;
    uint32_t track_count;
    uint32_t current_track_num;
    uint32_t current_sec;
} player_service_resume_state_t;

typedef enum
{
    READER_CMD_START,
} reader_cmd_t;

typedef struct
{
    reader_cmd_t cmd;
    uint32_t generation;
    size_t start_chunk;
} reader_cmd_msg_t;

typedef enum
{
    DECODER_CMD_START,
} decoder_cmd_t;

typedef struct
{
    decoder_cmd_t cmd;
    uint32_t generation;
} decoder_cmd_msg_t;

#define PLAYER_SVC_READER_IDLE_BIT (1 << 0)
#define PLAYER_SVC_DECODER_IDLE_BIT (1 << 1)
#define PLAYER_SVC_PIPELINE_IDLE_BITS (PLAYER_SVC_READER_IDLE_BIT | PLAYER_SVC_DECODER_IDLE_BIT)

static StreamBufferHandle_t s_pcm_stream;
static QueueHandle_t s_chunk_queue;
static SemaphoreHandle_t s_buf_pool_sem;
static QueueHandle_t s_cmd_queue;
static QueueHandle_t s_reader_cmd_queue;
static QueueHandle_t s_decoder_cmd_queue;
static EventGroupHandle_t s_pipeline_event_group;
static TaskHandle_t s_reader_task;
static TaskHandle_t s_decoder_task;
static TaskHandle_t s_service_task;
static bool s_initialised;
static bool s_playing;
static bool s_paused;
static size_t s_playlist_count;
static size_t s_playlist_index;
static size_t s_resume_chunk;
static volatile size_t s_current_chunk;
static char s_current_track[PLAYER_SVC_MAX_TRACK_TITLE_LEN];
static uint8_t s_decoder_pcm_buf[PLAYER_SVC_PCM_FRAME_BYTES];
static esp_audio_dec_info_t s_decoder_info;
static esp_audio_dec_out_frame_t s_decoder_out_frame;
static player_service_track_info_t s_track_info;
static volatile bool s_stop_requested;
static uint32_t s_active_generation;
static uint32_t s_cancelled_generation;
static uint8_t s_volume_level = PLAYER_SVC_DEFAULT_VOLUME_LEVEL;
static player_service_playback_mode_t s_playback_mode = PLAYER_SVC_MODE_SEQUENTIAL;
static player_service_playlist_entry_t *s_playlist = NULL;
static size_t s_playlist_capacity = 0;
static size_t *s_shuffle_order = NULL; /* PSRAM array; length == s_playlist_count */
static size_t s_shuffle_position = 0;  /* position of current track in s_shuffle_order; SIZE_MAX = not started */

static void player_service_post_event(player_service_event_id_t event_id, const void *data, size_t len)
{
    esp_event_post(PLAYER_SERVICE_EVENT, event_id, data, len, 0);
}

static bool player_service_queue_cmd(player_service_cmd_t cmd)
{
    player_service_msg_t msg = {.cmd = cmd};
    return s_cmd_queue && xQueueSend(s_cmd_queue, &msg, 0) == pdPASS;
}

static bool player_service_queue_control(player_service_control_t control)
{
    player_service_msg_t msg = {
        .cmd = PLAYER_SVC_CMD_CONTROL,
        .control = control,
    };
    return s_cmd_queue && xQueueSend(s_cmd_queue, &msg, 0) == pdPASS;
}

static bool player_service_queue_track_complete(uint32_t generation)
{
    player_service_msg_t msg = {
        .cmd = PLAYER_SVC_CMD_TRACK_COMPLETE,
        .generation = generation,
    };
    return s_cmd_queue && xQueueSend(s_cmd_queue, &msg, 0) == pdPASS;
}

static const char *player_service_playlist_filename(size_t index)
{
    if (index >= s_playlist_count)
    {
        return NULL;
    }

    return s_playlist[index].filename;
}

static const char *player_service_playlist_title(size_t index)
{
    if (index >= s_playlist_count)
    {
        return NULL;
    }

    if (s_playlist[index].title[0] != '\0')
    {
        return s_playlist[index].title;
    }

    return s_playlist[index].filename;
}

static bool player_service_should_stop(uint32_t generation)
{
    if (generation == 0)
    {
        return false;
    }

    return s_stop_requested || generation != s_active_generation;
}

static void player_service_reset_pipeline(void)
{
    if (s_reader_cmd_queue)
    {
        xQueueReset(s_reader_cmd_queue);
    }

    if (s_decoder_cmd_queue)
    {
        xQueueReset(s_decoder_cmd_queue);
    }

    if (s_chunk_queue)
    {
        xQueueReset(s_chunk_queue);
    }

    if (s_pcm_stream)
    {
        xStreamBufferReset(s_pcm_stream);
    }

    if (s_buf_pool_sem)
    {
        while (uxSemaphoreGetCount(s_buf_pool_sem) < PLAYER_SVC_CHUNK_BUF_COUNT)
        {
            xSemaphoreGive(s_buf_pool_sem);
        }
    }
}

static bool player_service_queue_chunk_message(const player_service_chunk_msg_t *msg, uint32_t generation)
{
    while (xQueueSend(s_chunk_queue, msg, pdMS_TO_TICKS(PLAYER_SVC_QUEUE_POLL_MS)) != pdPASS)
    {
        if (player_service_should_stop(generation))
        {
            return false;
        }
    }

    return true;
}

static bool player_service_send_eof_message(uint32_t generation)
{
    player_service_chunk_msg_t eof = {.is_eof = true};
    return player_service_queue_chunk_message(&eof, generation);
}

static bool player_service_wait_for_notification(uint32_t generation)
{
    while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(PLAYER_SVC_QUEUE_POLL_MS)) == 0)
    {
        if (player_service_should_stop(generation))
        {
            return false;
        }
    }

    return true;
}

static void player_service_free_track_info(void)
{
    if (s_track_info.lookup_table)
    {
        free(s_track_info.lookup_table);
    }

    memset(&s_track_info, 0, sizeof(s_track_info));
}

static inline uint32_t player_service_crc32(const uint8_t *data, size_t len)
{
    return esp_rom_crc32_le(0, data, (uint32_t)len);
}

static size_t player_service_clamp_chunk_index(size_t chunk_index)
{
    if (s_track_info.lookup_table_len == 0)
    {
        return 0;
    }

    if (chunk_index >= s_track_info.lookup_table_len)
    {
        return s_track_info.lookup_table_len - 1;
    }

    return chunk_index;
}

static size_t player_service_fast_seek_step(void)
{
    if (s_track_info.lookup_table_len == 0)
    {
        return 0;
    }

    return PLAYER_SVC_FAST_SEEK_SECONDS;
}

static size_t player_service_current_file_offset(void)
{
    size_t chunk_index;

    if (s_track_info.lookup_table_len == 0)
    {
        return 0;
    }

    chunk_index = s_paused ? s_resume_chunk : s_current_chunk;
    chunk_index = player_service_clamp_chunk_index(chunk_index);
    return s_track_info.lookup_table[chunk_index];
}

static size_t player_service_current_second(void)
{
    if (s_track_info.lookup_table_len == 0)
    {
        return 0;
    }

    return s_paused ? player_service_clamp_chunk_index(s_resume_chunk)
                    : player_service_clamp_chunk_index(s_current_chunk);
}

static bool player_service_should_restart_current_track(void)
{
    if (s_track_info.lookup_table_len <= PLAYER_SVC_PREVIOUS_RESTART_SECONDS)
    {
        return false;
    }

    return player_service_current_file_offset() > s_track_info.previous_restart_offset;
}

static esp_err_t player_service_erase_saved_playback_status_locked(nvs_handle_t handle)
{
    esp_err_t err = nvs_erase_key(handle, PLAYER_SVC_NVS_KEY_RESUME);

    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        return ESP_OK;
    }

    if (err != ESP_OK)
    {
        return err;
    }

    return nvs_commit(handle);
}

static esp_err_t player_service_write_playback_status(size_t track_index, size_t current_sec)
{
    const jukeboy_jbm_header_t *metadata = cartridge_service_get_metadata_header();
    nvs_handle_t handle;
    player_service_resume_state_t status = {
        .version = PLAYER_SVC_RESUME_STATE_VERSION,
        .cartridge_checksum = metadata ? metadata->checksum : 0,
        .track_count = metadata ? metadata->track_count : 0,
        .current_track_num = (uint32_t)track_index,
        .current_sec = (uint32_t)current_sec,
    };
    esp_err_t err;

    if (!cartridge_service_is_mounted() || !metadata)
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = nvs_open(PLAYER_SVC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to open NVS namespace %s: %s",
                 PLAYER_SVC_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, PLAYER_SVC_NVS_KEY_RESUME, &status, sizeof(status));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to write playback status to NVS: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static esp_err_t player_service_persist_current_playback_status(void)
{
    if (!s_playing || s_playlist_count == 0)
    {
        return ESP_OK;
    }

    return player_service_write_playback_status(s_playlist_index, player_service_current_second());
}

static esp_err_t player_service_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;

    if (!s_initialised || s_service_task == NULL || s_cmd_queue == NULL)
    {
        return ESP_OK;
    }

    StaticSemaphore_t completion_storage;
    SemaphoreHandle_t completion_semaphore = xSemaphoreCreateBinaryStatic(&completion_storage);
    player_service_msg_t msg = {
        .cmd = PLAYER_SVC_CMD_PERSIST_FOR_SHUTDOWN,
        .completion_semaphore = completion_semaphore,
    };
    esp_err_t result = ESP_FAIL;
    msg.result_out = &result;

    if (completion_semaphore == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(PLAYER_SVC_CMD_TIMEOUT_MS)) != pdPASS)
    {
        vSemaphoreDelete(completion_semaphore);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(completion_semaphore, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        vSemaphoreDelete(completion_semaphore);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(completion_semaphore);
    return result;
}

static bool player_service_load_saved_playback_status(size_t *track_index, size_t *start_chunk)
{
    const jukeboy_jbm_header_t *metadata = cartridge_service_get_metadata_header();
    nvs_handle_t handle;
    player_service_resume_state_t status;
    size_t status_size = sizeof(status);
    esp_err_t err;

    if (!track_index || !start_chunk || !cartridge_service_is_mounted() || !metadata)
    {
        return false;
    }

    err = nvs_open(PLAYER_SVC_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to open NVS namespace %s: %s",
                 PLAYER_SVC_NVS_NAMESPACE,
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_get_blob(handle, PLAYER_SVC_NVS_KEY_RESUME, &status, &status_size);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(handle);
        return false;
    }

    if (err != ESP_OK || status_size != sizeof(status))
    {
        ESP_LOGW(TAG, "discarding malformed resume state from NVS");
        (void)player_service_erase_saved_playback_status_locked(handle);
        nvs_close(handle);
        return false;
    }

    if (status.version != PLAYER_SVC_RESUME_STATE_VERSION)
    {
        ESP_LOGW(TAG, "discarding resume state with unsupported version %lu",
                 (unsigned long)status.version);
        (void)player_service_erase_saved_playback_status_locked(handle);
        nvs_close(handle);
        return false;
    }

    if (status.cartridge_checksum != metadata->checksum)
    {
        ESP_LOGI(TAG,
                 "stored resume belongs to cartridge checksum 0x%08lx, current cartridge is 0x%08lx; clearing saved state",
                 (unsigned long)status.cartridge_checksum,
                 (unsigned long)metadata->checksum);
        (void)player_service_erase_saved_playback_status_locked(handle);
        nvs_close(handle);
        return false;
    }

    if (status.track_count != metadata->track_count || (size_t)status.current_track_num >= s_playlist_count)
    {
        ESP_LOGW(TAG,
                 "discarding resume state with invalid track index %lu for %lu-track cartridge",
                 (unsigned long)status.current_track_num,
                 (unsigned long)metadata->track_count);
        (void)player_service_erase_saved_playback_status_locked(handle);
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);

    *track_index = (size_t)status.current_track_num;
    *start_chunk = (size_t)status.current_sec;
    return true;
}

static size_t player_service_get_seek_target(bool forward)
{
    size_t base_chunk;
    size_t step = player_service_fast_seek_step();

    if (s_track_info.lookup_table_len == 0)
    {
        return 0;
    }

    base_chunk = s_paused ? s_resume_chunk : s_current_chunk;
    base_chunk = player_service_clamp_chunk_index(base_chunk);

    if (forward)
    {
        if (base_chunk + step >= s_track_info.lookup_table_len)
        {
            return s_track_info.lookup_table_len - 1;
        }
        return base_chunk + step;
    }

    if (step >= base_chunk)
    {
        return 0;
    }

    return base_chunk - step;
}

static esp_err_t player_service_load_track_info(const char *filename, player_service_track_info_t *info)
{
    char full_path[PLAYER_SVC_FULL_PATH_LEN];
    uint32_t *lookup_table = NULL;
    const uint8_t *buf = NULL;
    size_t buf_len = 0;
    size_t total_file_size = 0;
    esp_err_t err;

    if (!filename || !info)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));

    err = cartridge_service_read_chunk_async(filename, 0, xTaskGetCurrentTaskHandle());
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "first async read request failed for %s: %s", filename, esp_err_to_name(err));
        return err;
    }

    if (!player_service_wait_for_notification(0))
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = cartridge_service_get_read_result(&buf, &buf_len);
    if (err != ESP_OK || buf_len < sizeof(jukeboy_jba_header_t))
    {
        ESP_LOGE(TAG, "failed to read file header for %s", filename);
        return err == ESP_OK ? ESP_FAIL : err;
    }

    jukeboy_jba_header_t header;
    memcpy(&header, buf, sizeof(header));

    if (header.version != JUKEBOY_JBA_VERSION)
    {
        ESP_LOGE(TAG, "unsupported file version %u for %s", (unsigned)header.version, filename);
        return ESP_ERR_NOT_SUPPORTED;
    }

    info->data_offset = (size_t)header.header_len_in_blocks * 512U;
    info->lookup_table_len = header.lookup_table_len;
    if (info->data_offset == 0 || info->lookup_table_len == 0)
    {
        ESP_LOGE(TAG, "invalid file header for %s", filename);
        return ESP_FAIL;
    }

    size_t lt_bytes = info->lookup_table_len * sizeof(uint32_t);
    size_t min_header_bytes = sizeof(jukeboy_jba_header_t) + lt_bytes;
    lookup_table = (uint32_t *)heap_caps_malloc(lt_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lookup_table)
    {
        ESP_LOGE(TAG, "failed to allocate lookup table (%u bytes)", (unsigned)lt_bytes);
        return ESP_ERR_NO_MEM;
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", cartridge_service_get_mount_point(), filename);
    struct stat st;
    if (stat(full_path, &st) != 0)
    {
        ESP_LOGE(TAG, "failed to stat %s for file size", full_path);
        free(lookup_table);
        return ESP_ERR_NOT_FOUND;
    }

    long file_size = st.st_size;
    if (file_size <= 0 || (size_t)file_size <= info->data_offset)
    {
        ESP_LOGE(TAG, "invalid file size %ld for %s", file_size, full_path);
        free(lookup_table);
        return ESP_FAIL;
    }
    total_file_size = (size_t)file_size;

    if (info->data_offset < min_header_bytes)
    {
        ESP_LOGE(TAG, "data offset too small for %s", filename);
        free(lookup_table);
        return ESP_FAIL;
    }

    size_t lt_offset = sizeof(jukeboy_jba_header_t);
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

            if (!player_service_wait_for_notification(0))
            {
                err = ESP_ERR_INVALID_STATE;
                break;
            }

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
            free(lookup_table);
            return err == ESP_OK ? ESP_FAIL : err;
        }
    }

    size_t total_data_bytes = total_file_size - info->data_offset;
    if (lookup_table[0] != 0)
    {
        ESP_LOGE(TAG, "lookup table must start at offset 0 for %s", filename);
        free(lookup_table);
        return ESP_FAIL;
    }

    for (size_t index = 0; index < info->lookup_table_len; ++index)
    {
        size_t chunk_start = lookup_table[index];
        size_t chunk_end = (index + 1U < info->lookup_table_len) ? lookup_table[index + 1U] : total_data_bytes;

        if (chunk_start > total_data_bytes || chunk_end > total_data_bytes || chunk_end < chunk_start)
        {
            ESP_LOGE(TAG, "invalid lookup table entry %u for %s", (unsigned)index, filename);
            free(lookup_table);
            return ESP_FAIL;
        }

        if ((chunk_end - chunk_start) <= PLAYER_SVC_CHUNK_CRC_BYTES)
        {
            ESP_LOGE(TAG, "chunk %u too small for crc32 in %s", (unsigned)index, filename);
            free(lookup_table);
            return ESP_FAIL;
        }

        if ((chunk_end - chunk_start) > PLAYER_SVC_CHUNK_MAX_BYTES)
        {
            ESP_LOGE(TAG, "chunk %u exceeds max size in %s", (unsigned)index, filename);
            free(lookup_table);
            return ESP_FAIL;
        }
    }

    strncpy(info->filename, filename, sizeof(info->filename) - 1);
    info->filename[sizeof(info->filename) - 1] = '\0';
    info->lookup_table = lookup_table;
    info->previous_restart_offset = (info->lookup_table_len > PLAYER_SVC_PREVIOUS_RESTART_SECONDS)
                                        ? lookup_table[PLAYER_SVC_PREVIOUS_RESTART_SECONDS]
                                        : 0;
    info->total_file_size = total_file_size;
    return ESP_OK;
}

static esp_err_t player_service_ensure_track_loaded(size_t index)
{
    player_service_track_info_t new_info;
    esp_err_t err;
    const char *filename;

    if (index >= s_playlist_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    filename = player_service_playlist_filename(index);
    if (!filename)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_track_info.lookup_table && strcmp(s_track_info.filename, filename) == 0)
    {
        return ESP_OK;
    }

    err = player_service_load_track_info(filename, &new_info);
    if (err != ESP_OK)
    {
        return err;
    }

    player_service_free_track_info();
    s_track_info = new_info;
    return ESP_OK;
}

static bool player_service_wait_for_idle_bits(EventBits_t bits, const char *timeout_message)
{
    EventBits_t current_bits = xEventGroupWaitBits(s_pipeline_event_group,
                                                   bits,
                                                   pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(PLAYER_SVC_STOP_POLL_MS * PLAYER_SVC_STOP_TIMEOUT_LOOPS));
    if ((current_bits & bits) != bits)
    {
        ESP_LOGW(TAG, "%s", timeout_message);
        return false;
    }

    return true;
}

static void player_service_wait_for_playback_stop(void)
{
    player_service_wait_for_idle_bits(PLAYER_SVC_PIPELINE_IDLE_BITS,
                                      "timed out waiting for pipeline tasks to become idle");

    player_service_reset_pipeline();
}

static void player_service_queue_track_complete_when_reader_idle(uint32_t generation)
{
    xEventGroupSetBits(s_pipeline_event_group, PLAYER_SVC_DECODER_IDLE_BIT);
    player_service_wait_for_idle_bits(PLAYER_SVC_READER_IDLE_BIT,
                                      "timed out waiting for reader task to become idle before track complete");
    player_service_queue_track_complete(generation);
}

static bool player_service_pipeline_idle(void)
{
    EventBits_t bits = xEventGroupGetBits(s_pipeline_event_group);
    return (bits & PLAYER_SVC_PIPELINE_IDLE_BITS) == PLAYER_SVC_PIPELINE_IDLE_BITS;
}

static void player_service_stop_playback(bool cancelled)
{
    if (player_service_pipeline_idle())
    {
        player_service_reset_pipeline();
        return;
    }

    if (cancelled)
    {
        s_cancelled_generation = s_active_generation;
    }

    s_stop_requested = true;
    player_service_wait_for_playback_stop();
}

static inline int16_t player_service_scale_sample(int16_t sample, uint16_t gain_q8)
{
    /* gain_q8 is guaranteed to be <= 256.
       We can safely multiply and right-shift without clipping. */
    return (int16_t)(((int32_t)sample * gain_q8) >> PLAYER_SVC_VOLUME_Q8_SHIFT);
}

static void player_service_apply_volume(uint8_t *data, size_t len)
{
    uint16_t gain_q8;
    int16_t *samples;
    size_t sample_count;

    if (!data || len < sizeof(int16_t))
    {
        return;
    }

    gain_q8 = s_volume_gain_q8[s_volume_level];
    if (gain_q8 >= PLAYER_SVC_VOLUME_Q8_ONE)
    {
        return;
    }

    samples = (int16_t *)data;
    sample_count = len / sizeof(int16_t);
    for (size_t index = 0; index < sample_count; ++index)
    {
        samples[index] = player_service_scale_sample(samples[index], gain_q8);
    }
}

static bool player_service_format_track_filename(char *buffer, size_t buffer_len, uint32_t file_num)
{
    if (!buffer || buffer_len == 0 || file_num > JUKEBOY_MAX_TRACK_FILES)
    {
        return false;
    }

    return snprintf(buffer, buffer_len, "%03lu.jba", (unsigned long)file_num) > 0;
}

/**
 * @brief (Re-)generate a Fisher-Yates shuffled permutation of [0, s_playlist_count).
 *
 * After the call, s_shuffle_order[0..s_playlist_count-1] holds every playlist
 * index exactly once in a random order.  The first entry is guaranteed to differ
 * from s_playlist_index (when s_playlist_count > 1) so that entering or wrapping
 * within shuffle mode never immediately repeats the current track.
 *
 * No-op if s_shuffle_order is NULL or the playlist is empty.
 */
static void player_service_build_shuffle_order(void)
{
    if (!s_shuffle_order || s_playlist_count == 0)
    {
        return;
    }

    for (size_t i = 0; i < s_playlist_count; i++)
    {
        s_shuffle_order[i] = i;
    }
    for (size_t i = s_playlist_count - 1; i > 0; i--)
    {
        size_t j = (size_t)(esp_random() % (uint32_t)(i + 1));
        size_t tmp = s_shuffle_order[i];
        s_shuffle_order[i] = s_shuffle_order[j];
        s_shuffle_order[j] = tmp;
    }

    /* If the first entry would immediately repeat the current track, swap it
     * with a random other position. */
    if (s_playlist_count > 1 && s_shuffle_order[0] == s_playlist_index)
    {
        size_t j = 1 + (size_t)(esp_random() % (uint32_t)(s_playlist_count - 1));
        size_t tmp = s_shuffle_order[0];
        s_shuffle_order[0] = s_shuffle_order[j];
        s_shuffle_order[j] = tmp;
    }
}

static void player_service_free_playlist(void)
{
    if (s_playlist)
    {
        heap_caps_free(s_playlist);
        s_playlist = NULL;
    }

    if (s_shuffle_order)
    {
        heap_caps_free(s_shuffle_order);
        s_shuffle_order = NULL;
    }

    s_playlist_count = 0;
    s_playlist_capacity = 0;
    s_shuffle_position = 0;
}

static void player_service_scan_playlist(void)
{
    size_t metadata_track_count = cartridge_service_get_metadata_track_count();

    player_service_free_playlist();
    s_playlist_index = 0;

    if (metadata_track_count == 0)
    {
        ESP_LOGW(TAG, "metadata contains no tracks");
        return;
    }

    s_playlist = heap_caps_calloc(metadata_track_count, sizeof(player_service_playlist_entry_t),
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_playlist)
    {
        ESP_LOGE(TAG, "failed to allocate playlist for %u tracks", (unsigned)metadata_track_count);
        return;
    }
    s_playlist_capacity = metadata_track_count;

    for (size_t index = 0; index < metadata_track_count; ++index)
    {
        const jukeboy_jbm_track_t *track = cartridge_service_get_metadata_track(index);
        if (!track)
        {
            continue;
        }

        if (!player_service_format_track_filename(s_playlist[s_playlist_count].filename,
                                                  sizeof(s_playlist[s_playlist_count].filename),
                                                  track->file_num))
        {
            ESP_LOGW(TAG, "skipping metadata track %u with invalid file_num %lu",
                     (unsigned)index,
                     (unsigned long)track->file_num);
            continue;
        }

        if (track->track_name[0] != '\0')
        {
            strncpy(s_playlist[s_playlist_count].title,
                    track->track_name,
                    sizeof(s_playlist[s_playlist_count].title) - 1);
            s_playlist[s_playlist_count].title[sizeof(s_playlist[s_playlist_count].title) - 1] = '\0';
        }
        else
        {
            strncpy(s_playlist[s_playlist_count].title,
                    s_playlist[s_playlist_count].filename,
                    sizeof(s_playlist[s_playlist_count].title) - 1);
            s_playlist[s_playlist_count].title[sizeof(s_playlist[s_playlist_count].title) - 1] = '\0';
        }

        s_playlist_count++;
    }

    if (s_playlist_count > 0)
    {
        s_shuffle_order = heap_caps_malloc(s_playlist_count * sizeof(size_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_shuffle_order)
        {
            player_service_build_shuffle_order();
        }
        else
        {
            ESP_LOGW(TAG, "failed to allocate shuffle order; shuffle falls back to random-per-step");
        }
    }
    s_shuffle_position = SIZE_MAX;

    ESP_LOGI(TAG, "playlist contains %u metadata track(s)", (unsigned)s_playlist_count);
    player_service_post_event(PLAYER_SVC_EVENT_PLAYLIST_READY, &s_playlist_count, sizeof(s_playlist_count));
}

static void player_service_reader_task(void *param)
{
    (void)param;

    for (;;)
    {
        reader_cmd_msg_t cmd;
        const char *filename;
        bool buffer_checked_out;
        esp_err_t err;
        uint32_t generation;
        size_t start_chunk;
        size_t total_data_bytes;
        const uint8_t *rd;
        size_t rd_len;
        size_t window_start;

        xEventGroupSetBits(s_pipeline_event_group, PLAYER_SVC_READER_IDLE_BIT);

        if (xQueueReceive(s_reader_cmd_queue, &cmd, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        xEventGroupClearBits(s_pipeline_event_group, PLAYER_SVC_READER_IDLE_BIT);

        if (cmd.cmd != READER_CMD_START)
        {
            continue;
        }

        filename = s_track_info.filename;
        buffer_checked_out = false;
        err = ESP_OK;
        generation = cmd.generation;
        start_chunk = cmd.start_chunk;
        rd = NULL;
        rd_len = 0;
        window_start = 0;

        if (start_chunk >= s_track_info.lookup_table_len)
        {
            err = ESP_ERR_INVALID_ARG;
            goto track_done;
        }

        total_data_bytes = s_track_info.total_file_size - s_track_info.data_offset;

        for (size_t chunk_index = start_chunk; chunk_index < s_track_info.lookup_table_len; ++chunk_index)
        {
            if (player_service_should_stop(generation))
            {
                err = ESP_OK;
                goto track_done;
            }

            size_t chunk_start = s_track_info.lookup_table[chunk_index];
            size_t chunk_end = (chunk_index + 1 < s_track_info.lookup_table_len)
                                   ? s_track_info.lookup_table[chunk_index + 1]
                                   : total_data_bytes;
            configASSERT(chunk_end >= chunk_start && (chunk_end - chunk_start) <= PLAYER_SVC_CHUNK_MAX_BYTES);

            size_t chunk_len = chunk_end - chunk_start;
            if (chunk_len <= PLAYER_SVC_CHUNK_CRC_BYTES)
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
                    if (!player_service_queue_chunk_message(&flush, generation))
                    {
                        err = ESP_OK;
                        goto track_done;
                    }
                    buffer_checked_out = false;
                }

                while (xSemaphoreTake(s_buf_pool_sem, pdMS_TO_TICKS(PLAYER_SVC_QUEUE_POLL_MS)) != pdPASS)
                {
                    if (player_service_should_stop(generation))
                    {
                        err = ESP_OK;
                        goto track_done;
                    }
                }
                buffer_checked_out = true;

                if (player_service_should_stop(generation))
                {
                    err = ESP_OK;
                    goto track_done;
                }

                err = cartridge_service_read_chunk_async(filename,
                                                         s_track_info.data_offset + chunk_start,
                                                         xTaskGetCurrentTaskHandle());
                if (err != ESP_OK)
                {
                    ESP_LOGE(TAG, "read request failed for %s chunk %u: %s",
                             filename,
                             (unsigned)chunk_index,
                             esp_err_to_name(err));
                    xSemaphoreGive(s_buf_pool_sem);
                    buffer_checked_out = false;
                    rd = NULL;
                    goto track_done;
                }

                if (!player_service_wait_for_notification(generation))
                {
                    err = ESP_OK;
                    goto track_done;
                }

                err = cartridge_service_get_read_result(&rd, &rd_len);
                if (err != ESP_OK || rd_len < chunk_len)
                {
                    ESP_LOGE(TAG, "read result invalid for %s chunk %u", filename, (unsigned)chunk_index);
                    xSemaphoreGive(s_buf_pool_sem);
                    buffer_checked_out = false;
                    rd = NULL;
                    if (err == ESP_OK)
                    {
                        err = ESP_FAIL;
                    }
                    goto track_done;
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
            if (!player_service_queue_chunk_message(&msg, generation))
            {
                err = ESP_OK;
                goto track_done;
            }
        }

    track_done:
        if (buffer_checked_out)
        {
            xSemaphoreGive(s_buf_pool_sem);
        }

        player_service_send_eof_message(generation);
        cartridge_service_close_file();

        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "reader stopped on error for %s: %s", filename, esp_err_to_name(err));
        }
        else
        {
            ESP_LOGI(TAG, "reader done for %s", filename);
        }
    }
}

static void player_service_decoder_task(void *param)
{
    (void)param;

    for (;;)
    {
        decoder_cmd_msg_t cmd;
        void *opus_handle = NULL;
        uint32_t generation;

        xEventGroupSetBits(s_pipeline_event_group, PLAYER_SVC_DECODER_IDLE_BIT);

        if (xQueueReceive(s_decoder_cmd_queue, &cmd, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        xEventGroupClearBits(s_pipeline_event_group, PLAYER_SVC_DECODER_IDLE_BIT);

        if (cmd.cmd != DECODER_CMD_START)
        {
            continue;
        }

        generation = cmd.generation;

        esp_opus_dec_cfg_t opus_cfg = ESP_OPUS_DEC_CONFIG_DEFAULT();
        opus_cfg.channel = ESP_AUDIO_DUAL;
        opus_cfg.sample_rate = ESP_AUDIO_SAMPLE_RATE_48K;
        opus_cfg.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS;
        opus_cfg.self_delimited = false;

        esp_audio_err_t aerr = esp_opus_dec_open(&opus_cfg, sizeof(opus_cfg), &opus_handle);
        if (aerr != ESP_AUDIO_ERR_OK || !opus_handle)
        {
            ESP_LOGE(TAG, "failed to open opus decoder: %d", aerr);
            player_service_queue_track_complete_when_reader_idle(generation);
            continue;
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
            if (player_service_should_stop(generation))
            {
                break;
            }

            if (xQueueReceive(s_chunk_queue, &msg, pdMS_TO_TICKS(PLAYER_SVC_QUEUE_POLL_MS)) != pdTRUE)
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

            s_current_chunk = msg.chunk_index;

            if (msg.len <= PLAYER_SVC_CHUNK_CRC_BYTES)
            {
                ESP_LOGE(TAG, "chunk %u missing crc32", (unsigned)msg.chunk_index);
                continue;
            }

            // CRC is not too important so skip it for now

            // uint32_t expected_crc = 0;
            // memcpy(&expected_crc, msg.data, sizeof(expected_crc));

            const uint8_t *chunk_data = msg.data + PLAYER_SVC_CHUNK_CRC_BYTES;
            size_t chunk_len = msg.len - PLAYER_SVC_CHUNK_CRC_BYTES;
            // uint32_t actual_crc = player_service_crc32(chunk_data, chunk_len);
            // if (actual_crc != expected_crc)
            // {
            //     ESP_LOGE(TAG,
            //              "crc32 mismatch in chunk %u: expected 0x%08lx got 0x%08lx",
            //              (unsigned)msg.chunk_index,
            //              (unsigned long)expected_crc,
            //              (unsigned long)actual_crc);
            //     continue;
            // }

            size_t cursor = 0;
            while (cursor < chunk_len)
            {
                if (player_service_should_stop(generation))
                {
                    running = false;
                    break;
                }

                const uint8_t *pkt = chunk_data + cursor;
                uint8_t byte1 = pkt[0];
                size_t hdr_size = 1;
                size_t frame_len;

                if ((chunk_len - cursor) < 1)
                {
                    break;
                }

                if (byte1 < 252)
                {
                    frame_len = byte1;
                }
                else
                {
                    if ((chunk_len - cursor) < 2)
                    {
                        ESP_LOGE(TAG, "partial packet header in chunk %u", (unsigned)msg.chunk_index);
                        break;
                    }
                    hdr_size = 2;
                    frame_len = (pkt[1] * 4) + byte1;
                }

                if (frame_len > PLAYER_SVC_OPUS_MAX_FRAME_LEN)
                {
                    ESP_LOGE(TAG, "opus frame length %u exceeds maximum %u in chunk %u",
                             (unsigned)frame_len,
                             (unsigned)PLAYER_SVC_OPUS_MAX_FRAME_LEN,
                             (unsigned)msg.chunk_index);
                    break;
                }

                size_t pkt_size = hdr_size + frame_len;
                if (pkt_size <= hdr_size)
                {
                    cursor += hdr_size;
                    continue;
                }

                if (pkt_size > (chunk_len - cursor))
                {
                    ESP_LOGE(TAG, "packet size %u exceeds remaining chunk data %u in chunk %u",
                             (unsigned)pkt_size,
                             (unsigned)(chunk_len - cursor),
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
                        if (player_service_should_stop(generation))
                        {
                            running = false;
                            break;
                        }
                        written += xStreamBufferSend(s_pcm_stream,
                                                     s_decoder_pcm_buf + written,
                                                     s_decoder_out_frame.decoded_size - written,
                                                     pdMS_TO_TICKS(PLAYER_SVC_QUEUE_POLL_MS));
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
        player_service_queue_track_complete_when_reader_idle(generation);
        ESP_LOGI(TAG, "decoder done for %s", s_current_track);
    }
}

static esp_err_t player_service_start_track(size_t index, size_t start_chunk)
{
    esp_err_t err;

    if (index >= s_playlist_count)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!player_service_pipeline_idle())
    {
        ESP_LOGW(TAG, "cannot start track while pipeline is still active");
        return ESP_ERR_INVALID_STATE;
    }

    err = player_service_ensure_track_loaded(index);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to load track metadata for %s: %s",
                 player_service_playlist_filename(index),
                 esp_err_to_name(err));
        return err;
    }

    start_chunk = player_service_clamp_chunk_index(start_chunk);

    s_active_generation++;
    if (s_active_generation == 0)
    {
        s_active_generation = 1;
    }

    strncpy(s_current_track, player_service_playlist_title(index), sizeof(s_current_track) - 1);
    s_current_track[sizeof(s_current_track) - 1] = '\0';
    s_playlist_index = index;

    /* Keep s_shuffle_position in sync with the track that is actually starting.
     * next/prev paths already set it, but this also covers saved-state restores
     * and any other direct jump to a specific index. */
    if (s_playback_mode == PLAYER_SVC_MODE_SHUFFLE && s_shuffle_order)
    {
        for (size_t i = 0; i < s_playlist_count; i++)
        {
            if (s_shuffle_order[i] == index)
            {
                s_shuffle_position = i;
                break;
            }
        }
    }

    s_playing = true;
    s_paused = false;
    s_stop_requested = false;
    s_resume_chunk = start_chunk;
    s_current_chunk = start_chunk;

    player_service_reset_pipeline();

    decoder_cmd_msg_t dec_cmd = {
        .cmd = DECODER_CMD_START,
        .generation = s_active_generation,
    };
    reader_cmd_msg_t rdr_cmd = {
        .cmd = READER_CMD_START,
        .generation = s_active_generation,
        .start_chunk = start_chunk,
    };

    if (xQueueSend(s_decoder_cmd_queue, &dec_cmd, 0) != pdPASS)
    {
        s_playing = false;
        ESP_LOGE(TAG, "failed to send start command to decoder");
        return ESP_ERR_NO_MEM;
    }

    if (xQueueSend(s_reader_cmd_queue, &rdr_cmd, 0) != pdPASS)
    {
        s_cancelled_generation = s_active_generation;
        s_stop_requested = true;
        s_playing = false;
        player_service_send_eof_message(s_active_generation);
        player_service_wait_for_playback_stop();
        ESP_LOGE(TAG, "failed to send start command to reader");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "starting track %u/%u: %s",
             (unsigned)(index + 1),
             (unsigned)s_playlist_count,
             s_current_track);
    player_service_post_event(PLAYER_SVC_EVENT_TRACK_STARTED, s_current_track, strlen(s_current_track) + 1);
    return ESP_OK;
}

static void player_service_handle_cartridge_inserted(void)
{
    cartridge_status_t cartridge_status;
    size_t start_index = 0;
    size_t start_chunk = 0;

    player_service_stop_playback(true);
    player_service_free_track_info();
    s_paused = false;
    s_playing = false;
    s_current_track[0] = '\0';

    cartridge_status = cartridge_service_get_status();
    if (cartridge_status == CARTRIDGE_STATUS_EMPTY || cartridge_status == CARTRIDGE_STATUS_INVALID)
    {
        ESP_LOGW(TAG, "cartridge not playable (status=%s)",
                 cartridge_service_status_name(cartridge_status));
        return;
    }

    if (!cartridge_service_is_mounted())
    {
        ESP_LOGW(TAG, "cartridge reported ready but is not mounted");
        return;
    }

    player_service_scan_playlist();
    if (s_playlist_count == 0)
    {
        ESP_LOGW(TAG, "no .jba files found in cartridge root");
        s_playing = false;
        return;
    }

    if (player_service_load_saved_playback_status(&start_index, &start_chunk))
    {
        ESP_LOGI(TAG, "resuming from saved playback status: track=%u sec=%u",
                 (unsigned)start_index,
                 (unsigned)start_chunk);
    }

    if (player_service_start_track(start_index, start_chunk) != ESP_OK)
    {
        s_playing = false;
    }
}

/**
 * @brief Compute the next playlist index.
 *
 * @param is_manual  True when triggered by an explicit NEXT control; false for
 *                   automatic end-of-track advance.  The distinction matters only
 *                   for @c PLAYER_SVC_MODE_SINGLE_REPEAT, where a manual NEXT
 *                   skips to the sequential next track instead of repeating.
 * @return Next playlist index (0 when the playlist is empty).
 */
static size_t player_service_next_index(bool is_manual)
{
    if (s_playlist_count == 0)
    {
        return 0;
    }

    switch (s_playback_mode)
    {
    case PLAYER_SVC_MODE_SINGLE_REPEAT:
        if (is_manual)
        {
            /* Manual skip overrides the repeat — advance sequentially. */
            return (s_playlist_index + 1U < s_playlist_count) ? (s_playlist_index + 1U) : 0;
        }
        return s_playlist_index;

    case PLAYER_SVC_MODE_SHUFFLE:
        if (s_playlist_count == 1)
        {
            return 0;
        }
        if (s_shuffle_order)
        {
            /* Advance through the pre-shuffled order; loop back without reshuffling
             * when the last entry is reached.  A fresh shuffle only happens when
             * the user switches into shuffle mode (see player_service_set_playback_mode). */
            size_t next_pos;
            if (s_shuffle_position == SIZE_MAX || s_shuffle_position + 1 >= s_playlist_count)
            {
                next_pos = 0;
            }
            else
            {
                next_pos = s_shuffle_position + 1;
            }
            s_shuffle_position = next_pos;
            return s_shuffle_order[s_shuffle_position];
        }
        /* Fallback when shuffle order allocation failed. */
        {
            size_t r;
            do
            {
                r = (size_t)(esp_random() % (uint32_t)s_playlist_count);
            } while (r == s_playlist_index);
            return r;
        }

    case PLAYER_SVC_MODE_SEQUENTIAL:
    default:
        return (s_playlist_index + 1U < s_playlist_count) ? (s_playlist_index + 1U) : 0;
    }
}

/**
 * @brief Compute the previous playlist index.
 *
 * In shuffle mode, steps backwards through the pre-shuffled order (wrapping to
 * the last entry when already at position 0).  In all other modes behaves like
 * simple sequential decrement.
 */
static size_t player_service_prev_index(void)
{
    if (s_playlist_count == 0)
    {
        return 0;
    }

    if (s_playback_mode == PLAYER_SVC_MODE_SHUFFLE && s_shuffle_order && s_playlist_count > 1)
    {
        size_t prev_pos;
        if (s_shuffle_position == SIZE_MAX || s_shuffle_position == 0)
        {
            /* Already at the start; wrap to end of the current pass. */
            prev_pos = s_playlist_count - 1;
        }
        else
        {
            prev_pos = s_shuffle_position - 1;
        }
        s_shuffle_position = prev_pos;
        return s_shuffle_order[s_shuffle_position];
    }

    return (s_playlist_index > 0) ? (s_playlist_index - 1U) : (s_playlist_count - 1U);
}

static void player_service_handle_track_complete(uint32_t generation)
{
    size_t next_index;

    if (generation == s_cancelled_generation)
    {
        return;
    }

    if (generation != s_active_generation)
    {
        return;
    }

    if (s_playing)
    {
        player_service_post_event(PLAYER_SVC_EVENT_TRACK_FINISHED, s_current_track, strlen(s_current_track) + 1);
    }

    if (s_playlist_count == 0)
    {
        s_playing = false;
        return;
    }

    next_index = player_service_next_index(false);
    if (s_playback_mode == PLAYER_SVC_MODE_SEQUENTIAL && next_index == 0 && s_playlist_count > 1)
    {
        ESP_LOGI(TAG, "playlist complete, restarting from first track");
    }

    if (player_service_start_track(next_index, 0) != ESP_OK)
    {
        s_playing = false;
    }
}

static void player_service_handle_control(player_service_control_t control)
{
    size_t target_index;
    size_t target_chunk;

    switch (control)
    {
    case PLAYER_SVC_CONTROL_NEXT:
        if (s_playlist_count == 0)
        {
            break;
        }
        target_index = player_service_next_index(true);
        s_paused = false;
        player_service_stop_playback(true);
        if (player_service_start_track(target_index, 0) != ESP_OK)
        {
            s_playing = false;
        }
        break;
    case PLAYER_SVC_CONTROL_PREVIOUS:
        if (s_playlist_count == 0)
        {
            break;
        }
        s_paused = false;
        player_service_stop_playback(true);
        if (player_service_should_restart_current_track())
        {
            target_index = s_playlist_index;
        }
        else
        {
            target_index = player_service_prev_index();
        }
        if (player_service_start_track(target_index, 0) != ESP_OK)
        {
            s_playing = false;
        }
        break;
    case PLAYER_SVC_CONTROL_FAST_FORWARD:
    case PLAYER_SVC_CONTROL_FAST_BACKWARD:
        if (!s_playing || s_track_info.lookup_table_len == 0)
        {
            break;
        }
        target_chunk = player_service_get_seek_target(control == PLAYER_SVC_CONTROL_FAST_FORWARD);
        if (target_chunk == (s_paused ? s_resume_chunk : player_service_clamp_chunk_index(s_current_chunk)))
        {
            break;
        }
        s_resume_chunk = target_chunk;
        if (!s_paused)
        {
            player_service_stop_playback(true);
            if (player_service_start_track(s_playlist_index, s_resume_chunk) != ESP_OK)
            {
                s_playing = false;
            }
        }
        break;
    case PLAYER_SVC_CONTROL_VOLUME_UP:
        if (s_volume_level + 1U < PLAYER_SVC_VOLUME_LEVEL_COUNT)
        {
            s_volume_level++;
        }
        break;
    case PLAYER_SVC_CONTROL_VOLUME_DOWN:
        if (s_volume_level > 0)
        {
            s_volume_level--;
        }
        break;
    case PLAYER_SVC_CONTROL_PAUSE:
        if (!s_playing)
        {
            break;
        }
        if (s_paused)
        {
            if (player_service_start_track(s_playlist_index, s_resume_chunk) != ESP_OK)
            {
                s_playing = false;
            }
        }
        else
        {
            s_resume_chunk = player_service_clamp_chunk_index(s_current_chunk);
            player_service_stop_playback(true);
            s_paused = true;
        }
        break;
    default:
        break;
    }
}

static void player_service_handle_cartridge_removed(void)
{
    player_service_stop_playback(true);
    player_service_free_track_info();
    player_service_free_playlist();
    s_paused = false;
    s_playing = false;
    s_current_track[0] = '\0';
    ESP_LOGI(TAG, "cartridge removed, playback stopped");
}

static void player_service_task(void *param)
{
    (void)param;
    player_service_msg_t msg;

    for (;;)
    {
        if (xQueueReceive(s_cmd_queue, &msg, pdMS_TO_TICKS(PLAYER_SVC_CMD_TIMEOUT_MS)) != pdPASS)
        {
            continue;
        }

        switch (msg.cmd)
        {
        case PLAYER_SVC_CMD_CARTRIDGE_INSERTED:
            player_service_handle_cartridge_inserted();
            break;
        case PLAYER_SVC_CMD_CARTRIDGE_REMOVED:
            player_service_handle_cartridge_removed();
            break;
        case PLAYER_SVC_CMD_TRACK_COMPLETE:
            player_service_handle_track_complete(msg.generation);
            break;
        case PLAYER_SVC_CMD_CONTROL:
            player_service_handle_control(msg.control);
            break;
        case PLAYER_SVC_CMD_PERSIST_FOR_SHUTDOWN:
        {
            esp_err_t err = player_service_persist_current_playback_status();
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "failed to persist playback status during shutdown: %s", esp_err_to_name(err));
            }
            if (msg.result_out != NULL)
            {
                *msg.result_out = err;
            }
            if (msg.completion_semaphore != NULL)
            {
                xSemaphoreGive(msg.completion_semaphore);
            }
            break;
        }
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
    else if (id == CARTRIDGE_SVC_EVENT_UNMOUNTED)
    {
        if (!player_service_queue_cmd(PLAYER_SVC_CMD_CARTRIDGE_REMOVED))
        {
            ESP_LOGW(TAG, "dropped cartridge removed event");
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
    s_reader_cmd_queue = xQueueCreate(1, sizeof(reader_cmd_msg_t));
    s_decoder_cmd_queue = xQueueCreate(1, sizeof(decoder_cmd_msg_t));
    s_pipeline_event_group = xEventGroupCreate();
    if (!s_pcm_stream || !s_chunk_queue || !s_buf_pool_sem || !s_cmd_queue ||
        !s_reader_cmd_queue || !s_decoder_cmd_queue || !s_pipeline_event_group)
    {
        return ESP_ERR_NO_MEM;
    }

    /* Mark both pipeline tasks as idle before creating them. */
    xEventGroupSetBits(s_pipeline_event_group, PLAYER_SVC_PIPELINE_IDLE_BITS);

    if (xTaskCreatePinnedToCore(player_service_reader_task,
                                "player_reader",
                                PLAYER_SVC_READER_TASK_STACK,
                                NULL,
                                PLAYER_SVC_READER_TASK_PRIORITY,
                                &s_reader_task,
                                1) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(player_service_decoder_task,
                                "player_decoder",
                                PLAYER_SVC_DECODER_TASK_STACK,
                                NULL,
                                PLAYER_SVC_DECODER_TASK_PRIORITY,
                                &s_decoder_task,
                                1) != pdPASS)
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

    ESP_ERROR_CHECK(esp_event_handler_register(CARTRIDGE_SERVICE_EVENT,
                                               CARTRIDGE_SVC_EVENT_UNMOUNTED,
                                               player_service_on_cartridge_event,
                                               NULL));

    s_initialised = true;
    ESP_ERROR_CHECK(power_mgmt_service_register_shutdown_callback(player_service_shutdown_callback,
                                                                  NULL,
                                                                  POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_PLAYER));
    player_service_post_event(PLAYER_SVC_EVENT_STARTED, NULL, 0);
    if (cartridge_service_is_inserted())
    {
        player_service_queue_cmd(PLAYER_SVC_CMD_CARTRIDGE_INSERTED);
    }
    ESP_LOGI(TAG, "player service started");
    return ESP_OK;
}

esp_err_t player_service_request_control(player_service_control_t control)
{
    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!player_service_queue_control(control))
    {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

bool player_service_is_paused(void)
{
    return s_paused;
}

uint8_t player_service_get_volume_percent(void)
{
    return (uint8_t)((s_volume_gain_q8[s_volume_level] * 100U + (PLAYER_SVC_VOLUME_Q8_ONE / 2U)) /
                     PLAYER_SVC_VOLUME_Q8_ONE);
}

void player_service_set_volume_absolute(uint8_t avrc_vol)
{
    /* Map 0..127 AVRCP volume to 0..PLAYER_SVC_VOLUME_LEVEL_COUNT-1 */
    uint8_t level = (uint8_t)((uint32_t)avrc_vol * (PLAYER_SVC_VOLUME_LEVEL_COUNT - 1U) / 127U);
    if (level >= PLAYER_SVC_VOLUME_LEVEL_COUNT)
    {
        level = PLAYER_SVC_VOLUME_LEVEL_COUNT - 1;
    }
    s_volume_level = level;
    ESP_LOGI(TAG, "volume set to %u%% (AVRCP %u)", player_service_get_volume_percent(), avrc_vol);
}

esp_err_t player_service_set_playback_mode(player_service_playback_mode_t mode)
{
    if (mode != PLAYER_SVC_MODE_SEQUENTIAL &&
        mode != PLAYER_SVC_MODE_SINGLE_REPEAT &&
        mode != PLAYER_SVC_MODE_SHUFFLE)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_playback_mode = mode;
    if (mode == PLAYER_SVC_MODE_SHUFFLE)
    {
        /* Rebuild the shuffle order so the next track is freshly randomised. */
        s_shuffle_position = SIZE_MAX;
        player_service_build_shuffle_order();
    }
    ESP_LOGI(TAG, "playback mode -> %s",
             mode == PLAYER_SVC_MODE_SINGLE_REPEAT ? "single_repeat" : mode == PLAYER_SVC_MODE_SHUFFLE ? "shuffle"
                                                                                                       : "sequential");
    return ESP_OK;
}

player_service_playback_mode_t player_service_get_playback_mode(void)
{
    return s_playback_mode;
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

    if (s_paused)
    {
        memset(data, 0, (size_t)len);
        return len;
    }

    size_t received = xStreamBufferReceive(s_pcm_stream, data, (size_t)len, 0);
    if (received < (size_t)len)
    {
        memset(data + received, 0, (size_t)len - received);
    }

    player_service_apply_volume(data, (size_t)len);
    return len;
}