#include "ftp_service.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"

#include "cartridge_service.h"
#include "power_mgmt_service.h"
#include "ramdisk_service.h"
#include "storage_paths.h"
#include "wifi_service.h"

#include "ftp.h"

#define FTP_SERVICE_TASK_STACK_SIZE 4096
#define FTP_SERVICE_TASK_PRIORITY 4
#define FTP_SERVICE_STOP_TIMEOUT_MS 3000
#define FTP_SERVICE_TASK_DELAY_TICKS 1

static const char *TAG = "ftp_svc";

#define FTP_SERVICE_MAX_ROOT_ENTRIES 3

static bool ftp_network_is_up(void)
{
    if (wifi_service_get_state() == WIFI_SVC_STATE_CONNECTED)
        return true;
    /* Accept any other netif that has obtained an IP (e.g. Ethernet in QEMU). */
    esp_netif_t *netif = esp_netif_next_unsafe(NULL);
    while (netif != NULL)
    {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0)
            return true;
        netif = esp_netif_next_unsafe(netif);
    }
    return false;
}

static SemaphoreHandle_t s_lock;
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[FTP_SERVICE_TASK_STACK_SIZE];
static TaskHandle_t s_task_handle;
static bool s_initialized;
static bool s_stop_requested;

extern const char *MOUNT_POINT;
extern char ftp_user[FTP_USER_PASS_LEN_MAX + 1];
extern char ftp_pass[FTP_USER_PASS_LEN_MAX + 1];

EventGroupHandle_t xEventTask = NULL;
int FTP_TASK_FINISH_BIT = BIT2;

static void ftp_service_apply_config(void)
{
    MOUNT_POINT = FTP_SERVICE_ROOT_PATH;
    snprintf(ftp_user, sizeof(ftp_user), "%s", CONFIG_FTP_USER);
    snprintf(ftp_pass, sizeof(ftp_pass), "%s", CONFIG_FTP_PASSWORD);
}

static size_t ftp_service_collect_root_entries(const char **entries, size_t capacity)
{
    size_t count = 0;

    if (entries == NULL || capacity == 0)
    {
        return 0;
    }

    entries[count++] = APP_LITTLEFS_MOUNT_PATH;
    if (count < capacity)
    {
        entries[count++] = RAMDISK_SERVICE_MOUNT_PATH;
    }

    const char *cartridge_mount_point = cartridge_service_get_mount_point();
    if (count < capacity && cartridge_service_is_mounted() && cartridge_mount_point != NULL && cartridge_mount_point[0] == '/')
    {
        bool duplicate = false;
        for (size_t index = 0; index < count; index++)
        {
            if (strcmp(entries[index], cartridge_mount_point) == 0)
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
        {
            entries[count++] = cartridge_mount_point;
        }
    }

    return count;
}

static bool ftp_service_root_ready(void)
{
    if (strcmp(FTP_SERVICE_ROOT_PATH, "/") == 0)
    {
        return ftp_service_get_root_entry_count() > 0;
    }

    struct stat st;
    return stat(FTP_SERVICE_ROOT_PATH, &st) == 0 && S_ISDIR(st.st_mode);
}

static esp_err_t ftp_service_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return ftp_service_disable();
}

static void ftp_service_task(void *user_ctx)
{
    (void)user_ctx;
    TickType_t last_tick = xTaskGetTickCount();

    for (;;)
    {
        bool stop_requested;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        stop_requested = s_stop_requested;
        xSemaphoreGive(s_lock);

        if (stop_requested)
        {
            break;
        }

        TickType_t now = xTaskGetTickCount();
        uint32_t elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - last_tick);
        last_tick = now;

        int result = ftp_run(elapsed_ms);
        if (result < 0)
        {
            ESP_LOGW(TAG, "ftp_run exited with %d", result);
            break;
        }

        // Delay for at least one scheduler tick. Converting 1 ms to ticks under
        // a 100 Hz FreeRTOS tick rate yields 0 ticks and can starve the idle task.
        vTaskDelay(FTP_SERVICE_TASK_DELAY_TICKS);
    }

    ftp_reset();
    ftp_deinit();

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_task_handle = NULL;
    s_stop_requested = false;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "FTP service stopped");
    vTaskDelete(NULL);
}

esp_err_t ftp_service_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ftp_service_apply_config();
    esp_err_t err = power_mgmt_service_register_shutdown_callback(ftp_service_shutdown_callback,
                                                                  NULL,
                                                                  POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_FTP);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return err;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "FTP service initialized");
    return ESP_OK;
}

esp_err_t ftp_service_enable(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "FTP service is not initialized");
    ESP_RETURN_ON_FALSE(ftp_network_is_up(),
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "no network interface is up; connect Wi-Fi or Ethernet before enabling FTP");
    ESP_RETURN_ON_FALSE(ftp_service_root_ready(), ESP_ERR_NOT_FOUND, TAG, "FTP root is not mounted");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task_handle != NULL)
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    ftp_service_apply_config();
    if (!ftp_init())
    {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    if (!ftp_enable())
    {
        ftp_deinit();
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }

    s_stop_requested = false;
    s_task_handle = xTaskCreateStaticPinnedToCore(ftp_service_task,
                                                  "ftp_svc",
                                                  FTP_SERVICE_TASK_STACK_SIZE,
                                                  NULL,
                                                  FTP_SERVICE_TASK_PRIORITY,
                                                  s_task_stack,
                                                  &s_task_tcb,
                                                  0);
    if (s_task_handle == NULL)
    {
        ftp_deinit();
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "FTP service enabled on %s:%u", FTP_SERVICE_ROOT_PATH, FTP_SERVICE_COMMAND_PORT);
    return ESP_OK;
}

esp_err_t ftp_service_disable(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "FTP service is not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_task_handle == NULL)
    {
        xSemaphoreGive(s_lock);
        return ESP_OK;
    }

    s_stop_requested = true;
    xSemaphoreGive(s_lock);

    TickType_t start = xTaskGetTickCount();
    while (pdTICKS_TO_MS(xTaskGetTickCount() - start) < FTP_SERVICE_STOP_TIMEOUT_MS)
    {
        bool stopped;

        xSemaphoreTake(s_lock, portMAX_DELAY);
        stopped = (s_task_handle == NULL);
        xSemaphoreGive(s_lock);

        if (stopped)
        {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    return ESP_ERR_TIMEOUT;
}

bool ftp_service_is_enabled(void)
{
    if (!s_initialized || s_lock == NULL)
    {
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool enabled = (s_task_handle != NULL);
    xSemaphoreGive(s_lock);
    return enabled;
}

int ftp_service_get_state(void)
{
    if (!ftp_service_is_enabled())
    {
        return E_FTP_STE_DISABLED;
    }

    return ftp_getstate();
}

const char *ftp_service_state_name(int state)
{
    switch (state & 0xff)
    {
    case E_FTP_STE_DISABLED:
        return "disabled";
    case E_FTP_STE_START:
        return "starting";
    case E_FTP_STE_READY:
        return "ready";
    case E_FTP_STE_END_TRANSFER:
        return "ending_transfer";
    case E_FTP_STE_CONTINUE_LISTING:
        return "listing";
    case E_FTP_STE_CONTINUE_FILE_TX:
        return "sending";
    case E_FTP_STE_CONTINUE_FILE_RX:
        return "receiving";
    case E_FTP_STE_CONNECTED:
        return "connected";
    default:
        return "unknown";
    }
}

size_t ftp_service_get_root_entry_count(void)
{
    const char *entries[FTP_SERVICE_MAX_ROOT_ENTRIES];
    return ftp_service_collect_root_entries(entries, FTP_SERVICE_MAX_ROOT_ENTRIES);
}

const char *ftp_service_get_root_entry(size_t index)
{
    const char *entries[FTP_SERVICE_MAX_ROOT_ENTRIES];
    size_t count = ftp_service_collect_root_entries(entries, FTP_SERVICE_MAX_ROOT_ENTRIES);
    if (index >= count)
    {
        return NULL;
    }

    return entries[index];
}