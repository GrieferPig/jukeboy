#include <string.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
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
#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/ip_addr.h"

#include "power_mgmt_service.h"
#include "wifi_service.h"

/* ── Constants ──────────────────────────────────────────────────────── */

static const char *TAG = "wifi_svc";

#define CMD_QUEUE_DEPTH 10
#define RECONNECT_PERIOD_MS 30000
#define SERVICE_API_QUEUE_TIMEOUT_MS 5
#define WIFI_SERVICE_MUTEX_TIMEOUT_MS 1
#define WIFI_SERVICE_MAX_CMDS_PER_PASS 4
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
    CMD_EVT_CONNECTIVITY_OK,
    CMD_EVT_CONNECTIVITY_FAILED,
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
static esp_event_handler_instance_t s_wifi_event_handler_instance;
static esp_event_handler_instance_t s_ip_event_handler_instance;
EXT_RAM_BSS_ATTR static wifi_svc_scan_result_t s_scan_results;
static esp_netif_t *s_sta_netif;
static TimerHandle_t s_reconnect_timer;
static volatile wifi_svc_state_t s_state = WIFI_SVC_STATE_IDLE;
static volatile bool s_autoreconnect = true;
static bool s_sntp_initialised = false;
static bool s_dns_lookup_active = false;
static esp_netif_ip_info_t s_ip_info;
static bool s_have_ip_info = false;
static bool s_initialised;

static void wifi_zero_buffer(void *buffer, size_t len)
{
    volatile uint8_t *cursor = (volatile uint8_t *)buffer;

    while (cursor && len-- > 0)
    {
        *cursor++ = 0;
    }
}

static esp_err_t wifi_copy_cstring(char *dest, size_t dest_len, const char *src)
{
    size_t copy_len;

    if (!dest || dest_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!src)
    {
        dest[0] = '\0';
        return ESP_OK;
    }

    copy_len = strnlen(src, dest_len);
    if (copy_len >= dest_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(dest, src, copy_len);
    dest[copy_len] = '\0';
    return ESP_OK;
}

/* ── Helpers: NVS ───────────────────────────────────────────────────── */

static esp_err_t nvs_save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(h, NVS_KEY_SSID, ssid ? ssid : "");
    if (err == ESP_OK)
    {
        err = nvs_set_str(h, NVS_KEY_PASS, password ? password : "");
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
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

static TickType_t wifi_service_queue_timeout_ticks(void)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(SERVICE_API_QUEUE_TIMEOUT_MS);
    return timeout_ticks == 0 ? 1 : timeout_ticks;
}

static TickType_t wifi_service_mutex_timeout_ticks(void)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(WIFI_SERVICE_MUTEX_TIMEOUT_MS);
    return timeout_ticks == 0 ? 1 : timeout_ticks;
}

static esp_err_t wifi_service_connect_saved_credentials(void)
{
    char ssid[33] = {0};
    char password[65] = {0};
    wifi_config_t cfg = {0};
    esp_err_t err;

    cfg.sta.listen_interval = 3;
    err = nvs_load_creds(ssid, sizeof(ssid), password, sizeof(password));

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "no saved WiFi credentials available for reconnect: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ssid[sizeof(ssid) - 1] = '\0';
    password[sizeof(password) - 1] = '\0';

    err = wifi_copy_cstring((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    if (err == ESP_OK)
    {
        err = wifi_copy_cstring((char *)cfg.sta.password, sizeof(cfg.sta.password), password);
    }
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to apply saved WiFi config: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_connect failed using saved credentials: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "connecting using saved credentials");
    s_state = WIFI_SVC_STATE_CONNECTING;
    s_have_ip_info = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
    post_event(WIFI_SVC_EVENT_CONNECTING, NULL, 0);

cleanup:
    wifi_zero_buffer(password, sizeof(password));
    wifi_zero_buffer(cfg.sta.password, sizeof(cfg.sta.password));
    return err;
}

static void wifi_service_connectivity_dns_found_cb(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    (void)name;
    (void)callback_arg;

    wifi_svc_msg_t msg = {
        .cmd = (ipaddr != NULL) ? CMD_EVT_CONNECTIVITY_OK : CMD_EVT_CONNECTIVITY_FAILED,
    };
    if (!queue_internal_cmd(&msg))
    {
        ESP_LOGW(TAG, "dropped connectivity result event");
    }
}

static void wifi_service_start_connectivity_test(void)
{
    ip_addr_t resolved = {0};
    err_t err;

    if (s_dns_lookup_active)
    {
        return;
    }

    err = dns_gethostbyname_addrtype(DNS_TEST_HOST,
                                     &resolved,
                                     wifi_service_connectivity_dns_found_cb,
                                     NULL,
                                     LWIP_DNS_ADDRTYPE_IPV4);
    if (err == ERR_OK)
    {
        ESP_LOGI(TAG, "connectivity test passed (%s resolved)", DNS_TEST_HOST);
        post_event(WIFI_SVC_EVENT_CONNECTIVITY_OK, NULL, 0);
        return;
    }

    if (err == ERR_INPROGRESS)
    {
        s_dns_lookup_active = true;
        return;
    }

    ESP_LOGW(TAG, "connectivity test failed to start (DNS resolve %s err=%d)", DNS_TEST_HOST, (int)err);
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

static void wifi_service_process_cmd(const wifi_svc_msg_t *msg)
{
    switch (msg->cmd)
    {
    case CMD_CONNECT:
    {
        wifi_config_t cfg = {0};

        ESP_LOGI(TAG, "starting WiFi connection request");
        cfg.sta.listen_interval = 3;

        if (wifi_copy_cstring((char *)cfg.sta.ssid,
                              sizeof(cfg.sta.ssid),
                              msg->payload.connect.ssid) != ESP_OK ||
            wifi_copy_cstring((char *)cfg.sta.password,
                              sizeof(cfg.sta.password),
                              msg->payload.connect.password) != ESP_OK)
        {
            ESP_LOGW(TAG, "rejected WiFi credentials with oversized SSID or password");
            wifi_zero_buffer(cfg.sta.password, sizeof(cfg.sta.password));
            break;
        }

        (void)nvs_save_creds(msg->payload.connect.ssid,
                             msg->payload.connect.password);

        esp_wifi_disconnect(); /* in case already connected */
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
        esp_wifi_connect();
        s_state = WIFI_SVC_STATE_CONNECTING;
        s_have_ip_info = false;
        s_dns_lookup_active = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        post_event(WIFI_SVC_EVENT_CONNECTING, NULL, 0);
        wifi_zero_buffer(cfg.sta.password, sizeof(cfg.sta.password));
        break;
    }

    case CMD_DISCONNECT:
        ESP_LOGI(TAG, "disconnecting");
        xTimerStop(s_reconnect_timer, 0);
        esp_wifi_disconnect();
        s_state = WIFI_SVC_STATE_DISCONNECTED;
        s_have_ip_info = false;
        s_dns_lookup_active = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        break;

    case CMD_SCAN:
        ESP_LOGI(TAG, "starting scan");
        s_state = WIFI_SVC_STATE_SCANNING;
        esp_wifi_scan_start(NULL, false);
        break;

    case CMD_SET_AUTORECONNECT:
    {
        bool en = msg->payload.autoreconnect;
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
        wifi_svc_state_t st = s_state;
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
        ESP_LOGW(TAG, "disconnected (reason %d)", msg->payload.disconnect_reason);
        s_state = WIFI_SVC_STATE_DISCONNECTED;
        s_have_ip_info = false;
        s_dns_lookup_active = false;
        memset(&s_ip_info, 0, sizeof(s_ip_info));
        post_event(WIFI_SVC_EVENT_DISCONNECTED,
                   &msg->payload.disconnect_reason,
                   sizeof(msg->payload.disconnect_reason));
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
            if (xSemaphoreTake(s_scan_mutex, wifi_service_mutex_timeout_ticks()) == pdTRUE)
            {
                s_scan_results.count = count;
                memcpy(s_scan_results.records, records,
                       count * sizeof(wifi_ap_record_t));
                xSemaphoreGive(s_scan_mutex);
            }
            else
            {
                ESP_LOGW(TAG, "scan result publish skipped: mutex busy");
            }
        }
        else
        {
            count = 0;
            if (xSemaphoreTake(s_scan_mutex, wifi_service_mutex_timeout_ticks()) == pdTRUE)
            {
                s_scan_results.count = 0;
                xSemaphoreGive(s_scan_mutex);
            }
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
        s_ip_info = msg->payload.ip_info;
        s_have_ip_info = true;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&msg->payload.ip_info.ip));
        s_state = WIFI_SVC_STATE_CONNECTED;
        xTimerStop(s_reconnect_timer, 0);
        post_event(WIFI_SVC_EVENT_CONNECTED, NULL, 0);
        wifi_service_start_connectivity_test();
        init_sntp();
        break;

    case CMD_EVT_CONNECTIVITY_OK:
        s_dns_lookup_active = false;
        ESP_LOGI(TAG, "connectivity test passed (%s resolved)", DNS_TEST_HOST);
        post_event(WIFI_SVC_EVENT_CONNECTIVITY_OK, NULL, 0);
        break;

    case CMD_EVT_CONNECTIVITY_FAILED:
        s_dns_lookup_active = false;
        ESP_LOGW(TAG, "connectivity test failed (DNS resolve %s)", DNS_TEST_HOST);
        break;

    default:
        break;
    }
}

void wifi_service_process_once(void)
{
    wifi_svc_msg_t msg;

    if (!s_initialised || s_cmd_queue == NULL)
    {
        return;
    }

    for (size_t index = 0; index < WIFI_SERVICE_MAX_CMDS_PER_PASS; index++)
    {
        if (xQueueReceive(s_cmd_queue, &msg, 0) != pdPASS)
        {
            break;
        }

        wifi_service_process_cmd(&msg);
        if (msg.cmd == CMD_CONNECT)
        {
            wifi_zero_buffer(msg.payload.connect.password, sizeof(msg.payload.connect.password));
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
    esp_err_t err;

    if (!ssid)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_svc_msg_t msg = {.cmd = CMD_CONNECT};
    err = wifi_copy_cstring(msg.payload.connect.ssid,
                            sizeof(msg.payload.connect.ssid),
                            ssid);
    if (err != ESP_OK)
    {
        return err;
    }

    err = wifi_copy_cstring(msg.payload.connect.password,
                            sizeof(msg.payload.connect.password),
                            password ? password : "");
    if (err != ESP_OK)
    {
        return err;
    }

    return xQueueSend(s_cmd_queue, &msg, wifi_service_queue_timeout_ticks()) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_disconnect(void)
{
    wifi_svc_msg_t msg = {.cmd = CMD_DISCONNECT};
    return xQueueSend(s_cmd_queue, &msg, wifi_service_queue_timeout_ticks()) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_scan(void)
{
    wifi_svc_msg_t msg = {.cmd = CMD_SCAN};
    return xQueueSend(s_cmd_queue, &msg, wifi_service_queue_timeout_ticks()) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_set_autoreconnect(bool enable)
{
    wifi_svc_msg_t msg = {.cmd = CMD_SET_AUTORECONNECT,
                          .payload.autoreconnect = enable};
    return xQueueSend(s_cmd_queue, &msg, wifi_service_queue_timeout_ticks()) == pdPASS
               ? ESP_OK
               : ESP_ERR_TIMEOUT;
}

esp_err_t wifi_service_reconnect(void)
{
    wifi_svc_msg_t msg = {.cmd = CMD_RECONNECT_TICK};
    return xQueueSend(s_cmd_queue, &msg, wifi_service_queue_timeout_ticks()) == pdPASS
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
    if (xSemaphoreTake(s_scan_mutex, wifi_service_mutex_timeout_ticks()) != pdTRUE)
        return ESP_ERR_TIMEOUT;
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
    s_dns_lookup_active = false;
    s_sntp_initialised = false;
    s_state = WIFI_SVC_STATE_IDLE;
    s_initialised = false;

    return ESP_OK;
}
