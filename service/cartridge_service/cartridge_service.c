#include <string.h>
#include <stdio.h>

#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cartridge_service.h"

static const char *TAG = "cartridge_svc";

ESP_EVENT_DEFINE_BASE(CARTRIDGE_SERVICE_EVENT);

static cartridge_service_config_t s_config;
static bool s_initialised = false;
static bool s_mounted = false;
static sdmmc_card_t *s_card = NULL;

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
    TaskHandle_t notify_task;
} cartridge_read_request_t;

static QueueHandle_t s_read_queue = NULL;
static TaskHandle_t s_reader_task = NULL;

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

        /* If filename changed, close old and open new */
        if (s_open_file && strcmp(s_open_filename, req.filename) != 0)
        {
            fclose(s_open_file);
            s_open_file = NULL;
            s_open_filename[0] = '\0';
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
                                                 tskNO_AFFINITY);
        if (ret != pdPASS)
        {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "cartridge service init (CLK=%d CMD=%d D0=%d mount=%s)",
             s_config.clk_gpio, s_config.cmd_gpio,
             s_config.d0_gpio, s_config.mount_point);
    cartridge_service_post_event(CARTRIDGE_SVC_EVENT_STARTED);
    if (cartridge_service_is_inserted())
    {
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
        return ESP_OK;
    }

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

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
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "SD card mounted at %s", s_config.mount_point);
    cartridge_service_post_event(CARTRIDGE_SVC_EVENT_MOUNTED);
    return ESP_OK;
}

esp_err_t cartridge_service_unmount(void)
{
    if (!s_mounted)
    {
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(s_config.mount_point, s_card);
    s_card = NULL;
    s_mounted = false;
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
    strncpy(req.filename, filename, sizeof(req.filename) - 1);
    req.offset = offset;
    req.notify_task = notify_task;

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
    if (s_open_file)
    {
        fclose(s_open_file);
        s_open_file = NULL;
    }
    s_open_filename[0] = '\0';
}
