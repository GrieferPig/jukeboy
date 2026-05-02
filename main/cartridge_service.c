#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cartridge_service.h"
#include "runtime_env.h"

static const char *TAG = "cartridge_svc";

ESP_EVENT_DEFINE_BASE(CARTRIDGE_SERVICE_EVENT);

static cartridge_service_config_t s_config;
static bool s_initialised = false;
static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;
static cartridge_status_t s_status = CARTRIDGE_STATUS_EMPTY;
static StaticSemaphore_t s_state_mutex_storage;
static SemaphoreHandle_t s_state_mutex;
static uint8_t *s_metadata_blob = NULL;
static size_t s_metadata_blob_len = 0;
static const jukeboy_jbm_header_t *s_metadata_header = NULL;
static const jukeboy_jbm_track_t *s_metadata_tracks = NULL;
static uint32_t s_mount_generation = 1;
EXT_RAM_BSS_ATTR static uint8_t s_metadata_blob_storage[JUKEBOY_JBM_MAX_SIZE_BYTES];

/* ---- Double-buffer async read state ---- */

EXT_RAM_BSS_ATTR static uint8_t s_read_buf[2][CARTRIDGE_SERVICE_READ_BUF_SIZE];
static uint8_t s_buf_index = 0;

static FILE *s_open_file = NULL;
static char s_open_filename[256] = {0};

/* Result of the last completed read */
static const uint8_t *s_result_data = NULL;
static size_t s_result_len = 0;
static esp_err_t s_result_status = ESP_ERR_NOT_FINISHED;

/* Internal reader task */
#define CARTRIDGE_READER_TASK_STACK 4096
#define CARTRIDGE_READER_TASK_PRIORITY 6

typedef struct
{
    char filename[256];
    size_t offset;
    uint32_t generation;
    TaskHandle_t notify_task;
} cartridge_read_request_t;

static QueueHandle_t s_read_queue = NULL;
static TaskHandle_t s_reader_task = NULL;

static uint32_t cartridge_service_crc32(const uint8_t *data, size_t len)
{
    return esp_rom_crc32_le(0, data, (uint32_t)len);
}

static void cartridge_service_close_file_locked(void)
{
    if (s_open_file)
    {
        fclose(s_open_file);
        s_open_file = NULL;
    }
    s_open_filename[0] = '\0';
}

static void cartridge_service_set_status(cartridge_status_t status)
{
    s_status = status;
}

static void cartridge_service_free_metadata(void)
{
    memset(s_metadata_blob_storage, 0, sizeof(s_metadata_blob_storage));
    s_metadata_blob = NULL;
    s_metadata_blob_len = 0;
    s_metadata_header = NULL;
    s_metadata_tracks = NULL;
}

static void cartridge_service_sanitize_metadata_strings(jukeboy_jbm_header_t *header)
{
    jukeboy_jbm_track_t *tracks;

    if (!header)
    {
        return;
    }

    header->album_name[JUKEBOY_JBM_ALBUM_NAME_BYTES - 1] = '\0';
    header->album_description[JUKEBOY_JBM_ALBUM_DESCRIPTION_BYTES - 1] = '\0';
    header->artist[JUKEBOY_JBM_ARTIST_BYTES - 1] = '\0';
    header->genre[JUKEBOY_JBM_GENRE_BYTES - 1] = '\0';
    for (size_t index = 0; index < JUKEBOY_JBM_TAG_COUNT; ++index)
    {
        header->tag[index][JUKEBOY_JBM_TAG_BYTES - 1] = '\0';
    }

    tracks = (jukeboy_jbm_track_t *)((uint8_t *)header + sizeof(*header));
    for (size_t index = 0; index < header->track_count; ++index)
    {
        tracks[index].track_name[JUKEBOY_JBM_TRACK_NAME_BYTES - 1] = '\0';
        tracks[index].artists[JUKEBOY_JBM_TRACK_ARTISTS_BYTES - 1] = '\0';
    }
}

static esp_err_t cartridge_service_validate_metadata(uint8_t *blob, size_t blob_len)
{
    jukeboy_jbm_header_t *header;
    size_t expected_size;
    uint32_t stored_checksum;
    uint32_t computed_checksum;

    if (!blob || blob_len < sizeof(jukeboy_jbm_header_t))
    {
        return ESP_ERR_INVALID_SIZE;
    }

    header = (jukeboy_jbm_header_t *)blob;
    if (header->version != JUKEBOY_JBM_VERSION)
    {
        ESP_LOGE(TAG, "unsupported metadata version %lu", (unsigned long)header->version);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (header->track_count > JUKEBOY_MAX_TRACK_FILES)
    {
        ESP_LOGE(TAG, "metadata track count %lu exceeds max %u",
                 (unsigned long)header->track_count,
                 (unsigned)JUKEBOY_MAX_TRACK_FILES);
        return ESP_ERR_INVALID_SIZE;
    }

    expected_size = sizeof(jukeboy_jbm_header_t) +
                    ((size_t)header->track_count * sizeof(jukeboy_jbm_track_t));
    if (blob_len != expected_size)
    {
        ESP_LOGE(TAG, "metadata size mismatch: expected %u got %u",
                 (unsigned)expected_size,
                 (unsigned)blob_len);
        return ESP_ERR_INVALID_SIZE;
    }

    stored_checksum = header->checksum;
    header->checksum = 0;
    computed_checksum = cartridge_service_crc32(blob, blob_len);
    header->checksum = stored_checksum;
    if (computed_checksum != stored_checksum)
    {
        ESP_LOGE(TAG, "metadata crc32 mismatch: expected 0x%08lx got 0x%08lx",
                 (unsigned long)stored_checksum,
                 (unsigned long)computed_checksum);
        return ESP_ERR_INVALID_CRC;
    }

    cartridge_service_sanitize_metadata_strings(header);

    return ESP_OK;
}

static esp_err_t cartridge_service_load_metadata(void)
{
    char full_path[320];
    FILE *metadata_file = NULL;
    uint8_t *blob = NULL;
    long file_size;
    esp_err_t err;

    cartridge_service_free_metadata();
    if (!s_mounted)
    {
        cartridge_service_set_status(CARTRIDGE_STATUS_EMPTY);
        return ESP_ERR_INVALID_STATE;
    }

    snprintf(full_path, sizeof(full_path), "%s/%s", s_config.mount_point, JUKEBOY_JBM_FILENAME);
    metadata_file = fopen(full_path, "rb");
    if (!metadata_file)
    {
        ESP_LOGE(TAG, "metadata file not found: %s", full_path);
        cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(metadata_file, 0, SEEK_END) != 0)
    {
        fclose(metadata_file);
        cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
        return ESP_FAIL;
    }

    file_size = ftell(metadata_file);
    if (file_size <= 0 || (unsigned long)file_size > JUKEBOY_JBM_MAX_SIZE_BYTES)
    {
        fclose(metadata_file);
        cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fseek(metadata_file, 0, SEEK_SET) != 0)
    {
        fclose(metadata_file);
        cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
        return ESP_FAIL;
    }

    blob = s_metadata_blob_storage;

    if (fread(blob, 1, (size_t)file_size, metadata_file) != (size_t)file_size)
    {
        fclose(metadata_file);
        cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
        return ESP_FAIL;
    }

    fclose(metadata_file);

    err = cartridge_service_validate_metadata(blob, (size_t)file_size);
    if (err != ESP_OK)
    {
        cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
        return err;
    }

    s_metadata_blob = blob;
    s_metadata_blob_len = (size_t)file_size;
    s_metadata_header = (const jukeboy_jbm_header_t *)blob;
    s_metadata_tracks = (const jukeboy_jbm_track_t *)(blob + sizeof(jukeboy_jbm_header_t));
    cartridge_service_set_status(CARTRIDGE_STATUS_READY);
    ESP_LOGI(TAG, "metadata loaded: %lu track(s), %u bytes",
             (unsigned long)s_metadata_header->track_count,
             (unsigned)s_metadata_blob_len);
    return ESP_OK;
}

static void cartridge_service_post_event(cartridge_service_event_id_t event_id)
{
    esp_event_post(CARTRIDGE_SERVICE_EVENT, event_id, NULL, 0, 0);
}

static void cartridge_reader_task(void *param)
{
    (void)param;
    cartridge_read_request_t req;

    for (;;)
    {
        if (xQueueReceive(s_read_queue, &req, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);

        if (!s_mounted || req.generation != s_mount_generation)
        {
            s_result_data = NULL;
            s_result_len = 0;
            s_result_status = ESP_ERR_INVALID_STATE;
            xSemaphoreGive(s_state_mutex);
            if (req.notify_task)
            {
                xTaskNotifyGive(req.notify_task);
            }
            continue;
        }

        /* If filename changed, close old and open new */
        if (s_open_file && strcmp(s_open_filename, req.filename) != 0)
        {
            cartridge_service_close_file_locked();
        }

        if (!s_open_file)
        {
            char full_path[320];
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     s_config.mount_point, req.filename);
            s_open_file = fopen(full_path, "rb");
            if (!s_open_file)
            {
                ESP_LOGE(TAG, "failed to open %s", full_path);
                s_result_data = NULL;
                s_result_len = 0;
                s_result_status = ESP_ERR_NOT_FOUND;
                xSemaphoreGive(s_state_mutex);
                if (req.notify_task)
                {
                    xTaskNotifyGive(req.notify_task);
                }
                continue;
            }
            strncpy(s_open_filename, req.filename, sizeof(s_open_filename) - 1);
            s_open_filename[sizeof(s_open_filename) - 1] = '\0';
        }

        uint8_t *buf = s_read_buf[s_buf_index];
        s_buf_index ^= 1;

        if (fseek(s_open_file, (long)req.offset, SEEK_SET) != 0)
        {
            ESP_LOGE(TAG, "fseek to %u failed", (unsigned)req.offset);
            s_result_data = buf;
            s_result_len = 0;
            s_result_status = ESP_FAIL;
            xSemaphoreGive(s_state_mutex);
            if (req.notify_task)
            {
                xTaskNotifyGive(req.notify_task);
            }
            continue;
        }

        size_t bytes_read = fread(buf, 1, CARTRIDGE_SERVICE_READ_BUF_SIZE, s_open_file);
        s_result_data = buf;
        s_result_len = bytes_read;
        s_result_status = ESP_OK;

        ESP_LOGD(TAG, "read %u bytes at offset %u", (unsigned)bytes_read, (unsigned)req.offset);

        xSemaphoreGive(s_state_mutex);

        if (req.notify_task)
        {
            xTaskNotifyGive(req.notify_task);
        }
    }
}

esp_err_t cartridge_service_init(const cartridge_service_config_t *config)
{
    if (!config)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_config = *config;
    s_initialised = true;

    if (!s_state_mutex)
    {
        s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_storage);
        if (!s_state_mutex)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create the async reader task and its request queue */
    if (!s_read_queue)
    {
        s_read_queue = xQueueCreate(1, sizeof(cartridge_read_request_t));
        if (!s_read_queue)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_reader_task)
    {
        BaseType_t ret = xTaskCreatePinnedToCore(cartridge_reader_task,
                                                 "cart_reader",
                                                 CARTRIDGE_READER_TASK_STACK,
                                                 NULL,
                                                 CARTRIDGE_READER_TASK_PRIORITY,
                                                 &s_reader_task,
                                                 0);
        if (ret != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "cartridge service init (CLK=%d CMD=%d D0=%d mount=%s)",
             s_config.clk_gpio, s_config.cmd_gpio,
             s_config.d0_gpio, s_config.mount_point);
    cartridge_service_post_event(CARTRIDGE_SVC_EVENT_STARTED);
    cartridge_service_set_status(CARTRIDGE_STATUS_EMPTY);
    if (cartridge_service_is_inserted())
    {
        esp_err_t err = cartridge_service_mount();
        if (err != ESP_OK)
        {
            cartridge_service_set_status(CARTRIDGE_STATUS_INVALID);
            ESP_LOGE(TAG, "failed to prepare inserted cartridge: %s", esp_err_to_name(err));
        }
        cartridge_service_post_event(CARTRIDGE_SVC_EVENT_INSERTED);
    }
    return ESP_OK;
}

esp_err_t cartridge_service_mount(void)
{
    if (!s_initialised)
    {
        ESP_LOGE(TAG, "cartridge_service_init() must be called first");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_mounted)
    {
        ESP_LOGW(TAG, "already mounted at %s", s_config.mount_point);
        (void)cartridge_service_load_metadata();
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = app_is_running_in_qemu(),
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    if (mount_cfg.format_if_mount_failed)
    {
        ESP_LOGW(TAG, "QEMU runtime detected: formatting SD card if FAT mount fails");
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT; /* 20 MHz */

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = s_config.clk_gpio;
    slot_cfg.cmd = s_config.cmd_gpio;
    slot_cfg.d0 = s_config.d0_gpio;
    slot_cfg.width = 1;
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(s_config.mount_point, &host,
                                            &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE(TAG, "failed to mount FAT filesystem on SD card");
        }
        else
        {
            ESP_LOGE(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_mount_generation++;
    if (s_mount_generation == 0)
    {
        s_mount_generation = 1;
    }
    xSemaphoreGive(s_state_mutex);
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", s_config.mount_point);
    if (cartridge_service_load_metadata() != ESP_OK)
    {
        ESP_LOGW(TAG, "cartridge metadata is invalid");
    }
    cartridge_service_post_event(CARTRIDGE_SVC_EVENT_MOUNTED);
    return ESP_OK;
}

esp_err_t cartridge_service_unmount(void)
{
    if (!s_mounted)
    {
        return ESP_OK;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    esp_vfs_fat_sdcard_unmount(s_config.mount_point, s_card);
    cartridge_service_close_file_locked();
    cartridge_service_free_metadata();
    s_card = NULL;
    s_mounted = false;
    s_mount_generation++;
    if (s_mount_generation == 0)
    {
        s_mount_generation = 1;
    }
    cartridge_service_set_status(CARTRIDGE_STATUS_EMPTY);
    xSemaphoreGive(s_state_mutex);
    ESP_LOGI(TAG, "SD card unmounted");
    cartridge_service_post_event(CARTRIDGE_SVC_EVENT_UNMOUNTED);
    return ESP_OK;
}

const char *cartridge_service_get_mount_point(void)
{
    return s_config.mount_point;
}

bool cartridge_service_is_inserted(void)
{
    /* Stub: no card-detect GPIO available yet; report always inserted. */
    return true;
}

bool cartridge_service_is_mounted(void)
{
    return s_mounted;
}

cartridge_status_t cartridge_service_get_status(void)
{
    return s_status;
}

const char *cartridge_service_status_name(cartridge_status_t status)
{
    switch (status)
    {
    case CARTRIDGE_STATUS_EMPTY:
        return "empty";
    case CARTRIDGE_STATUS_READY:
        return "ready";
    case CARTRIDGE_STATUS_INVALID:
        return "invalid";
    default:
        return "unknown";
    }
}

const jukeboy_jbm_header_t *cartridge_service_get_metadata_header(void)
{
    return s_metadata_header;
}

uint32_t cartridge_service_get_metadata_version(void)
{
    return s_metadata_header ? s_metadata_header->version : 0;
}

uint32_t cartridge_service_get_metadata_checksum(void)
{
    return s_metadata_header ? s_metadata_header->checksum : 0;
}

size_t cartridge_service_get_metadata_track_count(void)
{
    return s_metadata_header ? (size_t)s_metadata_header->track_count : 0;
}

const jukeboy_jbm_track_t *cartridge_service_get_metadata_track(size_t index)
{
    if (!s_metadata_header || index >= s_metadata_header->track_count)
    {
        return NULL;
    }

    return &s_metadata_tracks[index];
}

const char *cartridge_service_get_album_name(void)
{
    return s_metadata_header ? s_metadata_header->album_name : NULL;
}

const char *cartridge_service_get_album_description(void)
{
    return s_metadata_header ? s_metadata_header->album_description : NULL;
}

const char *cartridge_service_get_album_artist(void)
{
    return s_metadata_header ? s_metadata_header->artist : NULL;
}

uint32_t cartridge_service_get_album_year(void)
{
    return s_metadata_header ? s_metadata_header->year : 0;
}

uint32_t cartridge_service_get_album_duration_sec(void)
{
    return s_metadata_header ? s_metadata_header->duration_sec : 0;
}

const char *cartridge_service_get_album_genre(void)
{
    return s_metadata_header ? s_metadata_header->genre : NULL;
}

const char *cartridge_service_get_album_tag(size_t index)
{
    if (!s_metadata_header || index >= JUKEBOY_JBM_TAG_COUNT)
    {
        return NULL;
    }

    return s_metadata_header->tag[index];
}

const char *cartridge_service_get_track_name(size_t index)
{
    const jukeboy_jbm_track_t *track = cartridge_service_get_metadata_track(index);
    return track ? track->track_name : NULL;
}

const char *cartridge_service_get_track_artists(size_t index)
{
    const jukeboy_jbm_track_t *track = cartridge_service_get_metadata_track(index);
    return track ? track->artists : NULL;
}

uint32_t cartridge_service_get_track_duration_sec(size_t index)
{
    const jukeboy_jbm_track_t *track = cartridge_service_get_metadata_track(index);
    return track ? track->duration_sec : 0;
}

uint32_t cartridge_service_get_track_file_num(size_t index)
{
    const jukeboy_jbm_track_t *track = cartridge_service_get_metadata_track(index);
    return track ? track->file_num : 0;
}

esp_err_t cartridge_service_read_chunk_async(const char *filename,
                                             size_t offset,
                                             TaskHandle_t notify_task)
{
    if (!filename || !notify_task)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_mounted)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_read_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    cartridge_read_request_t req = {0};
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.offset = offset;
    req.generation = s_mount_generation;
    req.notify_task = notify_task;
    xSemaphoreGive(s_state_mutex);

    if (xQueueSend(s_read_queue, &req, 0) != pdTRUE)
    {
        ESP_LOGE(TAG, "reader queue full — a read is already in flight");
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

esp_err_t cartridge_service_get_read_result(const uint8_t **out_data,
                                            size_t *out_len)
{
    if (!out_data || !out_len)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *out_data = s_result_data;
    *out_len = s_result_len;
    return s_result_status;
}

void cartridge_service_close_file(void)
{
    if (!s_state_mutex)
    {
        cartridge_service_close_file_locked();
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    cartridge_service_close_file_locked();
    xSemaphoreGive(s_state_mutex);
}
