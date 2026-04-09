#include <string.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "power_mgmt_service.h"
#include "wifi_service.h"

/* ── Constants ──────────────────────────────────────────────────────── */

static const char *TAG = "wifi_svc";

#define SVC_TASK_STACK_SIZE 4096
#define SVC_TASK_PRIORITY 5
#define SVC_TASK_CORE 0
#define CMD_QUEUE_DEPTH 10
#define RECONNECT_PERIOD_MS 30000
#define NVS_NAMESPACE "wifi_svc"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"
#define DNS_TEST_HOST "www.gstatic.com"
#define DNS_TEST_PORT "80"

/* ── Event base definition ──────────────────────────────────────────── */

ESP_EVENT_DEFINE_BASE(WIFI_SERVICE_EVENT);

/* ── Internal command IDs ───────────────────────────────────────────── */

typedef enum
{
    CMD_CONNECT,
    CMD_DISCONNECT,
    CMD_SCAN,
    CMD_SET_AUTORECONNECT,
    CMD_RECONNECT_TICK,
    CMD_EVT_DISCONNECTED,
    CMD_EVT_SCAN_DONE,
    CMD_EVT_GOT_IP,
} wifi_svc_cmd_t;

/* ── Command message ────────────────────────────────────────────────── */

typedef struct
{
    wifi_svc_cmd_t cmd;
    union
    {
        struct
        {
            char ssid[33];
            char password[65];
        } connect;
        bool autoreconnect;
        uint8_t disconnect_reason;
        esp_netif_ip_info_t ip_info;
    } payload;
} wifi_svc_msg_t;

/* ── Module state ───────────────────────────────────────────────────── */

static QueueHandle_t s_cmd_queue;
static SemaphoreHandle_t s_scan_mutex;
static TaskHandle_t s_task_handle;
static esp_event_handler_instance_t s_wifi_event_handler_instance;
static esp_event_handler_instance_t s_ip_event_handler_instance;
EXT_RAM_BSS_ATTR static wifi_svc_scan_result_t s_scan_results;
static esp_netif_t *s_sta_netif;
static TimerHandle_t s_reconnect_timer;
static volatile wifi_svc_state_t s_state = WIFI_SVC_STATE_IDLE;
static volatile bool s_autoreconnect = true;
static bool s_sntp_initialised = false;
static esp_netif_ip_info_t s_ip_info;
static bool s_have_ip_info = false;
static bool s_initialised;

/* Keep the WiFi service stack internal: this task calls NVS/flash-backed APIs. */
static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[SVC_TASK_STACK_SIZE];

/* ── Helpers: NVS ───────────────────────────────────────────────────── */

static esp_err_t nvs_save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
        return err;
    nvs_set_str(h, NVS_KEY_SSID, ssid);
    nvs_set_str(h, NVS_KEY_PASS, password);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t nvs_load_creds(char *ssid, size_t ssid_len,
                                char *password, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
        return err;
    err = nvs_get_str(h, NVS_KEY_SSID, ssid, &ssid_len);
    if (err == ESP_OK)
    {
        err = nvs_get_str(h, NVS_KEY_PASS, password, &pass_len);
    }
    nvs_close(h);
    return err;
}

/* ── Helpers: Event broadcast ───────────────────────────────────────── */

static void post_event(wifi_svc_event_id_t id, const void *data, size_t len)
{
    esp_event_post(WIFI_SERVICE_EVENT, id, data, len, 0);
}

static bool queue_internal_cmd(const wifi_svc_msg_t *msg)
{
    return s_cmd_queue && xQueueSend(s_cmd_queue, msg, 0) == pdPASS;
}

static esp_err_t wifi_service_connect_saved_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    wifi_config_t cfg = {0};
    cfg.sta.listen_interval = 3;
    esp_err_t err = nvs_load_creds(ssid, sizeof(ssid), password, sizeof(password));

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "no saved WiFi credentials available for reconnect: %s", esp_err_to_name(err));
        return err;
    }

    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to apply saved WiFi config: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_connect failed using saved credentials: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "connecting using saved credentials for SSID '%s'", ssid);
    s_state = WIFI_SVC_STATE_CONNECTING;
    s_have_ip_info = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    post_event(WIFI_SVC_EVENT_CONNECTING, NULL, 0);
    return ESP_OK;
}

/* ── DNS connectivity test (runs in service task context) ───────────── */

static void dns_connectivity_test(void)
{
    struct addrinfo hints = {.ai_family = AF_INET};
    struct addrinfo *res = NULL;
    int ret = getaddrinfo(DNS_TEST_HOST, DNS_TEST_PORT, &hints, &res);
    if (ret == 0 && res)
    {
        ESP_LOGI(TAG, "connectivity test passed (%s resolved)", DNS_TEST_HOST);
        post_event(WIFI_SVC_EVENT_CONNECTIVITY_OK, NULL, 0);
        freeaddrinfo(res);
    }
    else
    {
        ESP_LOGW(TAG, "connectivity test failed (DNS resolve %s)", DNS_TEST_HOST);
    }
}

/* ── SNTP initialisation (called once after first connectivity) ──── */

static void sntp_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synchronised");
    post_event(WIFI_SVC_EVENT_SNTP_SYNCED, NULL, 0);
}

static void init_sntp(void)
{
    if (s_sntp_initialised)
        return;
    s_sntp_initialised = true;

    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.sync_cb = sntp_sync_cb;
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP initialised (pool.ntp.org)");
}

/* ── WiFi / IP event handlers (run in default event task) ───────────── */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *evt = data;
        wifi_svc_msg_t msg = {
            .cmd = CMD_EVT_DISCONNECTED,
            .payload.disconnect_reason = evt->reason,
        };
        if (!queue_internal_cmd(&msg))
        {
            ESP_LOGW(TAG, "dropped disconnect event");
        }
    }
    else if (id == WIFI_EVENT_SCAN_DONE)
    {
        wifi_svc_msg_t msg = {.cmd = CMD_EVT_SCAN_DONE};
        if (!queue_internal_cmd(&msg))
        {
            ESP_LOGW(TAG, "dropped scan-done event");
        }
    }
    else if (id == WIFI_EVENT_STA_START)
    {
        ESP_LOGI(TAG, "STA started");
    }
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *evt = data;
        wifi_svc_msg_t msg = {
            .cmd = CMD_EVT_GOT_IP,
            .payload.ip_info = evt->ip_info,
        };
        if (!queue_internal_cmd(&msg))
        {
            ESP_LOGW(TAG, "dropped got-ip event");
        }
    }
}

/* ── Reconnect timer callback (FreeRTOS timer daemon context) ───────── */

static void reconnect_timer_cb(TimerHandle_t timer)
{
    wifi_svc_msg_t msg = {.cmd = CMD_RECONNECT_TICK};
    xQueueSend(s_cmd_queue, &msg, 0);
}

static esp_err_t wifi_service_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return wifi_service_shutdown();
}

/* ── Service task ───────────────────────────────────────────────────── */

static void wifi_service_task(void *arg)
{
    wifi_svc_msg_t msg;

    for (;;)
    {
        if (xQueueReceive(s_cmd_queue, &msg, portMAX_DELAY) == pdPASS)
        {
            switch (msg.cmd)
            {

            case CMD_CONNECT:
            {
                ESP_LOGI(TAG, "connecting to '%s'", msg.payload.connect.ssid);
                nvs_save_creds(msg.payload.connect.ssid,
                               msg.payload.connect.password);

                wifi_config_t cfg = {0};
                cfg.sta.listen_interval = 3;

                strncpy((char *)cfg.sta.ssid,
                        msg.payload.connect.ssid,
                        sizeof(cfg.sta.ssid) - 1);
                strncpy((char *)cfg.sta.password,
                        msg.payload.connect.password,
                        sizeof(cfg.sta.password) - 1);

                esp_wifi_disconnect(); /* in case already connected */
                esp_wifi_set_config(WIFI_IF_STA, &cfg);
                esp_wifi_connect();
                s_state = WIFI_SVC_STATE_CONNECTING;
                s_have_ip_info = false;
                memset(&s_ip_info, 0, sizeof(s_ip_info));
                post_event(WIFI_SVC_EVENT_CONNECTING, NULL, 0);
                break;
            }

            case CMD_DISCONNECT:
                ESP_LOGI(TAG, "disconnecting");
                xTimerStop(s_reconnect_timer, 0);
                esp_wifi_disconnect();
                s_state = WIFI_SVC_STATE_DISCONNECTED;
                s_have_ip_info = false;
                memset(&s_ip_info, 0, sizeof(s_ip_info));
                break;

            case CMD_SCAN:
                ESP_LOGI(TAG, "starting scan");
                s_state = WIFI_SVC_STATE_SCANNING;
                esp_wifi_scan_start(NULL, false);
                break;

            case CMD_SET_AUTORECONNECT:
            {
                bool en = msg.payload.autoreconnect;
                ESP_LOGI(TAG, "autoreconnect %s", en ? "ON" : "OFF");
                s_autoreconnect = en;
                if (!en)
                {
                    xTimerStop(s_reconnect_timer, 0);
                }
                post_event(WIFI_SVC_EVENT_AUTORECONNECT_CHANGED, &en, sizeof(en));
                break;
            }

            case CMD_RECONNECT_TICK:
            {
                int st = s_state;
                if (st != WIFI_SVC_STATE_CONNECTED &&
                    st != WIFI_SVC_STATE_CONNECTING &&
                    s_autoreconnect)
                {
                    ESP_LOGI(TAG, "auto-reconnect tick: connecting with saved credentials");
                    if (wifi_service_connect_saved_credentials() != ESP_OK)
                    {
                        s_state = WIFI_SVC_STATE_DISCONNECTED;
                    }
                }
                break;
            }

            case CMD_EVT_DISCONNECTED:
                ESP_LOGW(TAG, "disconnected (reason %d)", msg.payload.disconnect_reason);
                s_state = WIFI_SVC_STATE_DISCONNECTED;
                s_have_ip_info = false;
                memset(&s_ip_info, 0, sizeof(s_ip_info));
                post_event(WIFI_SVC_EVENT_DISCONNECTED,
                           &msg.payload.disconnect_reason,
                           sizeof(msg.payload.disconnect_reason));
                if (s_autoreconnect)
                {
                    xTimerStart(s_reconnect_timer, 0);
                }
                break;

            case CMD_EVT_SCAN_DONE:
            {
                uint16_t count = WIFI_SVC_MAX_SCAN_RESULTS;
                wifi_ap_record_t records[WIFI_SVC_MAX_SCAN_RESULTS] = {0};
                if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK)
                {
                    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                    s_scan_results.count = count;
                    memcpy(s_scan_results.records, records,
                           count * sizeof(wifi_ap_record_t));
                    xSemaphoreGive(s_scan_mutex);
                }
                else
                {
                    count = 0;
                    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
                    s_scan_results.count = 0;
                    xSemaphoreGive(s_scan_mutex);
                }

                if (s_state == WIFI_SVC_STATE_SCANNING)
                {
                    s_state = WIFI_SVC_STATE_DISCONNECTED;
                }
                post_event(WIFI_SVC_EVENT_SCAN_DONE, &s_scan_results,
                           sizeof(s_scan_results));
                ESP_LOGI(TAG, "scan done, %d APs found", count);
                break;
            }

            case CMD_EVT_GOT_IP:
                s_ip_info = msg.payload.ip_info;
                s_have_ip_info = true;
                ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&msg.payload.ip_info.ip));
                s_state = WIFI_SVC_STATE_CONNECTED;
                xTimerStop(s_reconnect_timer, 0);
                post_event(WIFI_SVC_EVENT_CONNECTED, NULL, 0);
                dns_connectivity_test();
                init_sntp();
                break;

            } /* switch */
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t wifi_service_init(void)
{
    if (s_initialised)
        return ESP_OK;

    /* Network interface */
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif)
        return ESP_FAIL;

    /* WiFi driver */
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    /* Register internal handlers */
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_wifi_event_handler_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL, &s_ip_event_handler_instance));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MAX_MODEM));

    /* IPC primitives */
    s_cmd_queue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(wifi_svc_msg_t));
    s_scan_mutex = xSemaphoreCreateMutex();
    if (!s_cmd_queue || !s_scan_mutex)
        return ESP_ERR_NO_MEM;

    /* 30 s auto-reconnect timer (initially stopped) */
    s_reconnect_timer = xTimerCreate("wifi_reconn",
                                     pdMS_TO_TICKS(RECONNECT_PERIOD_MS),
                                     pdTRUE, /* auto-reload */
                                     NULL,
                                     reconnect_timer_cb);
    if (!s_reconnect_timer)
        return ESP_ERR_NO_MEM;

    /* Service task — static allocation in PSRAM */
    s_task_handle = xTaskCreateStaticPinnedToCore(
        wifi_service_task, "wifi_svc",
        SVC_TASK_STACK_SIZE, NULL, SVC_TASK_PRIORITY,
        s_task_stack, &s_task_tcb, SVC_TASK_CORE);
    if (!s_task_handle)
        return ESP_ERR_NO_MEM;

    s_initialised = true;
    ESP_ERROR_CHECK(power_mgmt_service_register_shutdown_callback(wifi_service_shutdown_callback,
                                                                  NULL,
                                                                  POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_WIFI));

    post_event(WIFI_SVC_EVENT_STARTED, NULL, 0);
    ESP_LOGI(TAG, "WiFi service started");

    err = wifi_service_connect_saved_credentials();
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "boot-time WiFi connect skipped: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t wifi_service_connect(const char *ssid, const char *password)
{
    if (!ssid)
        return ESP_ERR_INVALID_ARG;
    wifi_svc_msg_t msg = {.cmd = CMD_CONNECT};
    strncpy(msg.payload.connect.ssid, ssid,
            sizeof(msg.payload.connect.ssid) - 1);
    if (password)
    {
        strncpy(msg.payload.connect.password, password,
                sizeof(msg.payload.connect.password) - 1);
    }
    return xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(1000)) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_disconnect(void)
{
    wifi_svc_msg_t msg = {.cmd = CMD_DISCONNECT};
    return xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(1000)) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_scan(void)
{
    wifi_svc_msg_t msg = {.cmd = CMD_SCAN};
    return xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(1000)) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_set_autoreconnect(bool enable)
{
    wifi_svc_msg_t msg = {.cmd = CMD_SET_AUTORECONNECT,
                          .payload.autoreconnect = enable};
    return xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(1000)) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_reconnect(void)
{
    wifi_svc_msg_t msg = {.cmd = CMD_RECONNECT_TICK};
    return xQueueSend(s_cmd_queue, &msg, pdMS_TO_TICKS(1000)) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

bool wifi_service_get_autoreconnect(void)
{
    return s_autoreconnect;
}

wifi_svc_state_t wifi_service_get_state(void)
{
    return s_state;
}

esp_err_t wifi_service_get_scan_results(wifi_svc_scan_result_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);
    memcpy(out, &s_scan_results, sizeof(s_scan_results));
    xSemaphoreGive(s_scan_mutex);
    return ESP_OK;
}

esp_err_t wifi_service_get_ip_info(esp_netif_ip_info_t *out)
{
    if (!out)
        return ESP_ERR_INVALID_ARG;
    if (!s_have_ip_info)
        return ESP_ERR_INVALID_STATE;
    *out = s_ip_info;
    return ESP_OK;
}

esp_err_t wifi_service_shutdown(void)
{
    if (!s_initialised)
    {
        return ESP_OK;
    }

    if (s_reconnect_timer)
    {
        xTimerStop(s_reconnect_timer, portMAX_DELAY);
    }

    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED)
    {
        return err;
    }

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT && err != ESP_ERR_WIFI_NOT_STARTED)
    {
        return err;
    }

    err = esp_wifi_deinit();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT)
    {
        return err;
    }

    if (s_ip_event_handler_instance != NULL)
    {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT,
                                                              IP_EVENT_STA_GOT_IP,
                                                              s_ip_event_handler_instance));
        s_ip_event_handler_instance = NULL;
    }

    if (s_wifi_event_handler_instance != NULL)
    {
        ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT,
                                                              ESP_EVENT_ANY_ID,
                                                              s_wifi_event_handler_instance));
        s_wifi_event_handler_instance = NULL;
    }

    if (s_sta_netif != NULL)
    {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_task_handle != NULL)
    {
        vTaskDelete(s_task_handle);
        s_task_handle = NULL;
    }

    if (s_reconnect_timer != NULL)
    {
        xTimerDelete(s_reconnect_timer, portMAX_DELAY);
        s_reconnect_timer = NULL;
    }

    if (s_scan_mutex != NULL)
    {
        vSemaphoreDelete(s_scan_mutex);
        s_scan_mutex = NULL;
    }

    if (s_cmd_queue != NULL)
    {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }

    memset(&s_scan_results, 0, sizeof(s_scan_results));
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    s_have_ip_info = false;
    s_sntp_initialised = false;
    s_state = WIFI_SVC_STATE_IDLE;
    s_initialised = false;

    return ESP_OK;
}
