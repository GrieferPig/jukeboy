#include <netdb.h>
#include <stdio.h>
#include <string.h>

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
#define NVS_KEY_LAST_SLOT "last_slot"
#define NVS_KEY_AUTORECONNECT "autoreconnect"
#define DNS_TEST_HOST "www.gstatic.com"
#define DNS_TEST_PORT "80"
#define WIFI_SVC_INVALID_SLOT (-1)

/* ── Event base definition ──────────────────────────────────────────── */

ESP_EVENT_DEFINE_BASE(WIFI_SERVICE_EVENT);

/* ── Internal command IDs ───────────────────────────────────────────── */

typedef enum
{
    CMD_CONNECT_SLOT,
    CMD_DISCONNECT,
    CMD_SCAN,
    CMD_SET_AUTORECONNECT,
    CMD_RECONNECT_TIMER,
    CMD_RECONNECT_NOW,
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
        uint8_t slot_index;
        bool autoreconnect;
        uint8_t disconnect_reason;
        esp_netif_ip_info_t ip_info;
    } payload;
} wifi_svc_msg_t;

typedef enum
{
    DISCONNECT_EXPECT_NONE,
    DISCONNECT_EXPECT_USER,
    DISCONNECT_EXPECT_HANDOFF,
} wifi_svc_expected_disconnect_t;

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
static volatile bool s_connectivity_ok = false;
static esp_netif_ip_info_t s_ip_info;
static bool s_have_ip_info = false;
static bool s_initialised;
static int s_last_successful_slot = WIFI_SVC_INVALID_SLOT;
static int s_current_slot = WIFI_SVC_INVALID_SLOT;
static bool s_reconnect_sequence_active = false;
static uint8_t s_reconnect_order[WIFI_SVC_SLOT_COUNT];
static size_t s_reconnect_order_len = 0;
static size_t s_reconnect_order_index = 0;
static wifi_svc_expected_disconnect_t s_expected_disconnect = DISCONNECT_EXPECT_NONE;

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

static bool wifi_service_slot_index_valid(uint8_t slot_index)
{
    return slot_index < WIFI_SVC_SLOT_COUNT;
}

static void wifi_service_reset_ip_state(void)
{
    s_have_ip_info = false;
    s_dns_lookup_active = false;
    s_connectivity_ok = false;
    memset(&s_ip_info, 0, sizeof(s_ip_info));
}

static void wifi_service_clear_reconnect_sequence(void)
{
    s_reconnect_sequence_active = false;
    s_reconnect_order_len = 0;
    s_reconnect_order_index = 0;
    memset(s_reconnect_order, 0, sizeof(s_reconnect_order));
}

static esp_err_t wifi_service_build_slot_key(const char *prefix,
                                             uint8_t slot_index,
                                             char *out_key,
                                             size_t out_key_len)
{
    int written;

    if (!prefix || !out_key || out_key_len == 0 || !wifi_service_slot_index_valid(slot_index))
    {
        return ESP_ERR_INVALID_ARG;
    }

    written = snprintf(out_key, out_key_len, "%s%u", prefix, (unsigned)(slot_index + 1));
    if (written < 0 || (size_t)written >= out_key_len)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/* ── Helpers: NVS ───────────────────────────────────────────────────── */

static esp_err_t nvs_save_slot(uint8_t slot_index, const char *ssid, const char *password)
{
    char ssid_key[16];
    char pass_key[16];
    nvs_handle_t h;
    esp_err_t err;

    err = wifi_service_build_slot_key("ssid", slot_index, ssid_key, sizeof(ssid_key));
    if (err != ESP_OK)
    {
        return err;
    }

    err = wifi_service_build_slot_key("pass", slot_index, pass_key, sizeof(pass_key));
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_str(h, ssid_key, ssid ? ssid : "");
    if (err == ESP_OK)
    {
        err = nvs_set_str(h, pass_key, password ? password : "");
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_load_slot(uint8_t slot_index,
                               char *ssid,
                               size_t ssid_len,
                               char *password,
                               size_t pass_len)
{
    char ssid_key[16];
    char pass_key[16];
    nvs_handle_t h;
    esp_err_t err;
    size_t ssid_size = ssid_len;
    size_t pass_size = pass_len;

    if (!ssid || ssid_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = wifi_service_build_slot_key("ssid", slot_index, ssid_key, sizeof(ssid_key));
    if (err != ESP_OK)
    {
        return err;
    }

    err = wifi_service_build_slot_key("pass", slot_index, pass_key, sizeof(pass_key));
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_get_str(h, ssid_key, ssid, &ssid_size);
    if (err == ESP_OK && password && pass_len > 0)
    {
        err = nvs_get_str(h, pass_key, password, &pass_size);
    }
    nvs_close(h);

    if (err == ESP_OK)
    {
        ssid[ssid_len - 1] = '\0';
        if (password && pass_len > 0)
        {
            password[pass_len - 1] = '\0';
        }
        if (ssid[0] == '\0')
        {
            err = ESP_ERR_NVS_NOT_FOUND;
        }
    }

    return err;
}

static esp_err_t nvs_save_last_successful_slot(int slot_index)
{
    nvs_handle_t h;
    esp_err_t err;
    int8_t persisted_slot = WIFI_SVC_INVALID_SLOT;

    if (slot_index >= 0 && slot_index < WIFI_SVC_SLOT_COUNT)
    {
        persisted_slot = (int8_t)slot_index;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_i8(h, NVS_KEY_LAST_SLOT, persisted_slot);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static int nvs_load_last_successful_slot(void)
{
    nvs_handle_t h;
    int8_t persisted_slot = WIFI_SVC_INVALID_SLOT;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        return WIFI_SVC_INVALID_SLOT;
    }

    if (nvs_get_i8(h, NVS_KEY_LAST_SLOT, &persisted_slot) != ESP_OK)
    {
        persisted_slot = WIFI_SVC_INVALID_SLOT;
    }
    nvs_close(h);

    if (persisted_slot < 0 || persisted_slot >= WIFI_SVC_SLOT_COUNT)
    {
        return WIFI_SVC_INVALID_SLOT;
    }

    return persisted_slot;
}

static esp_err_t nvs_save_autoreconnect(bool enable)
{
    nvs_handle_t h;
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_u8(h, NVS_KEY_AUTORECONNECT, enable ? 1 : 0);
    if (err == ESP_OK)
    {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static bool nvs_load_autoreconnect(void)
{
    nvs_handle_t h;
    uint8_t persisted_value = 1;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
    {
        return true;
    }

    if (nvs_get_u8(h, NVS_KEY_AUTORECONNECT, &persisted_value) != ESP_OK)
    {
        persisted_value = 1;
    }
    nvs_close(h);

    return persisted_value != 0;
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

static esp_err_t wifi_service_connect_with_credentials(uint8_t slot_index,
                                                       const char *ssid,
                                                       const char *password)
{
    wifi_config_t cfg = {0};
    esp_err_t err;
    bool need_handoff;

    if (!wifi_service_slot_index_valid(slot_index) || !ssid)
    {
        return ESP_ERR_INVALID_ARG;
    }

    need_handoff = (s_state == WIFI_SVC_STATE_CONNECTED || s_state == WIFI_SVC_STATE_CONNECTING);
    cfg.sta.listen_interval = 3;

    err = wifi_copy_cstring((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), ssid);
    if (err == ESP_OK)
    {
        err = wifi_copy_cstring((char *)cfg.sta.password,
                                sizeof(cfg.sta.password),
                                password ? password : "");
    }
    if (err != ESP_OK)
    {
        goto cleanup;
    }

    if (s_reconnect_timer != NULL)
    {
        xTimerStop(s_reconnect_timer, 0);
    }

    if (need_handoff)
    {
        s_expected_disconnect = DISCONNECT_EXPECT_HANDOFF;
        esp_wifi_disconnect();
    }
    else
    {
        s_expected_disconnect = DISCONNECT_EXPECT_NONE;
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to apply WiFi slot %u config: %s",
                 (unsigned)(slot_index + 1),
                 esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_connect failed using WiFi slot %u: %s",
                 (unsigned)(slot_index + 1),
                 esp_err_to_name(err));
        goto cleanup;
    }

    ESP_LOGI(TAG, "connecting using WiFi slot %u ('%s')",
             (unsigned)(slot_index + 1),
             ssid);
    s_current_slot = (int)slot_index;
    s_state = WIFI_SVC_STATE_CONNECTING;
    wifi_service_reset_ip_state();
    post_event(WIFI_SVC_EVENT_CONNECTING, NULL, 0);

cleanup:
    wifi_zero_buffer(cfg.sta.password, sizeof(cfg.sta.password));
    return err;
}

static esp_err_t wifi_service_connect_saved_slot(uint8_t slot_index)
{
    char ssid[33] = {0};
    char password[65] = {0};
    esp_err_t err;

    err = nvs_load_slot(slot_index, ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi slot %u is not usable: %s",
                 (unsigned)(slot_index + 1),
                 esp_err_to_name(err));
        goto cleanup;
    }

    err = wifi_service_connect_with_credentials(slot_index, ssid, password);

cleanup:
    wifi_zero_buffer(password, sizeof(password));
    return err;
}

static bool wifi_service_slot_is_configured(uint8_t slot_index)
{
    char ssid[33] = {0};
    return nvs_load_slot(slot_index, ssid, sizeof(ssid), NULL, 0) == ESP_OK;
}

static size_t wifi_service_build_reconnect_order(uint8_t *out_slots, size_t out_slot_count)
{
    bool seen[WIFI_SVC_SLOT_COUNT] = {0};
    size_t count = 0;
    uint8_t slot_index;

    if (!out_slots || out_slot_count == 0)
    {
        return 0;
    }

    if (s_last_successful_slot >= 0 &&
        s_last_successful_slot < WIFI_SVC_SLOT_COUNT &&
        wifi_service_slot_is_configured((uint8_t)s_last_successful_slot))
    {
        out_slots[count++] = (uint8_t)s_last_successful_slot;
        seen[s_last_successful_slot] = true;
    }

    for (slot_index = 0; slot_index < WIFI_SVC_SLOT_COUNT && count < out_slot_count; slot_index++)
    {
        if (!seen[slot_index] && wifi_service_slot_is_configured(slot_index))
        {
            out_slots[count++] = slot_index;
        }
    }

    return count;
}

static esp_err_t wifi_service_try_next_reconnect_slot(void)
{
    while (s_reconnect_sequence_active && s_reconnect_order_index < s_reconnect_order_len)
    {
        uint8_t slot_index = s_reconnect_order[s_reconnect_order_index++];
        esp_err_t err = wifi_service_connect_saved_slot(slot_index);
        if (err == ESP_OK)
        {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "auto-reconnect advancing past slot %u",
                 (unsigned)(slot_index + 1));
    }

    wifi_service_clear_reconnect_sequence();
    s_current_slot = WIFI_SVC_INVALID_SLOT;
    return ESP_ERR_NVS_NOT_FOUND;
}

static esp_err_t wifi_service_start_reconnect_sequence(void)
{
    wifi_service_clear_reconnect_sequence();
    if (s_reconnect_timer != NULL)
    {
        xTimerStop(s_reconnect_timer, 0);
    }

    s_reconnect_order_len = wifi_service_build_reconnect_order(s_reconnect_order,
                                                               WIFI_SVC_SLOT_COUNT);
    if (s_reconnect_order_len == 0)
    {
        s_current_slot = WIFI_SVC_INVALID_SLOT;
        return ESP_ERR_NVS_NOT_FOUND;
    }

    s_reconnect_sequence_active = true;
    s_reconnect_order_index = 0;
    return wifi_service_try_next_reconnect_slot();
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

    s_connectivity_ok = false;

    err = dns_gethostbyname_addrtype(DNS_TEST_HOST,
                                     &resolved,
                                     wifi_service_connectivity_dns_found_cb,
                                     NULL,
                                     LWIP_DNS_ADDRTYPE_IPV4);
    if (err == ERR_OK)
    {
        s_connectivity_ok = true;
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
    wifi_svc_msg_t msg = {.cmd = CMD_RECONNECT_TIMER};
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
    case CMD_CONNECT_SLOT:
        ESP_LOGI(TAG, "starting WiFi slot %u connection request",
                 (unsigned)(msg->payload.slot_index + 1));
        wifi_service_clear_reconnect_sequence();
        if (wifi_service_connect_saved_slot(msg->payload.slot_index) != ESP_OK)
        {
            s_state = WIFI_SVC_STATE_DISCONNECTED;
            s_current_slot = WIFI_SVC_INVALID_SLOT;
        }
        break;

    case CMD_DISCONNECT:
        ESP_LOGI(TAG, "disconnecting");
        wifi_service_clear_reconnect_sequence();
        if (s_reconnect_timer != NULL)
        {
            xTimerStop(s_reconnect_timer, 0);
        }
        if (s_state == WIFI_SVC_STATE_CONNECTED || s_state == WIFI_SVC_STATE_CONNECTING)
        {
            s_expected_disconnect = DISCONNECT_EXPECT_USER;
            esp_wifi_disconnect();
        }
        else
        {
            s_expected_disconnect = DISCONNECT_EXPECT_NONE;
        }
        s_state = WIFI_SVC_STATE_DISCONNECTED;
        s_current_slot = WIFI_SVC_INVALID_SLOT;
        wifi_service_reset_ip_state();
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
        if (nvs_save_autoreconnect(en) != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to persist autoreconnect %s", en ? "ON" : "OFF");
        }
        s_autoreconnect = en;
        if (!en)
        {
            xTimerStop(s_reconnect_timer, 0);
        }
        else if (s_reconnect_timer != NULL &&
                 s_state != WIFI_SVC_STATE_CONNECTED &&
                 s_state != WIFI_SVC_STATE_CONNECTING)
        {
            xTimerReset(s_reconnect_timer, 0);
        }
        post_event(WIFI_SVC_EVENT_AUTORECONNECT_CHANGED, &en, sizeof(en));
        break;
    }

    case CMD_RECONNECT_TIMER:
    case CMD_RECONNECT_NOW:
    {
        wifi_svc_state_t st = s_state;
        if (st != WIFI_SVC_STATE_CONNECTED &&
            st != WIFI_SVC_STATE_CONNECTING &&
            (msg->cmd == CMD_RECONNECT_NOW || s_autoreconnect))
        {
            ESP_LOGI(TAG, "%s: trying configured WiFi slots",
                     msg->cmd == CMD_RECONNECT_NOW ? "manual reconnect" : "auto-reconnect tick");
            if (wifi_service_start_reconnect_sequence() != ESP_OK)
            {
                s_state = WIFI_SVC_STATE_DISCONNECTED;
                s_current_slot = WIFI_SVC_INVALID_SLOT;
                if (s_autoreconnect && s_reconnect_timer != NULL)
                {
                    xTimerStart(s_reconnect_timer, 0);
                }
            }
        }
        break;
    }

    case CMD_EVT_DISCONNECTED:
        if (s_expected_disconnect != DISCONNECT_EXPECT_NONE &&
            msg->payload.disconnect_reason == WIFI_REASON_ASSOC_LEAVE)
        {
            wifi_svc_expected_disconnect_t expected = s_expected_disconnect;
            s_expected_disconnect = DISCONNECT_EXPECT_NONE;
            if (expected == DISCONNECT_EXPECT_USER)
            {
                ESP_LOGI(TAG, "disconnect request completed");
                post_event(WIFI_SVC_EVENT_DISCONNECTED,
                           &msg->payload.disconnect_reason,
                           sizeof(msg->payload.disconnect_reason));
            }
            else
            {
                ESP_LOGI(TAG, "handoff disconnect completed");
            }
            break;
        }

        s_expected_disconnect = DISCONNECT_EXPECT_NONE;
        ESP_LOGW(TAG, "disconnected (reason %d)", msg->payload.disconnect_reason);
        s_state = WIFI_SVC_STATE_DISCONNECTED;
        wifi_service_reset_ip_state();
        post_event(WIFI_SVC_EVENT_DISCONNECTED,
                   &msg->payload.disconnect_reason,
                   sizeof(msg->payload.disconnect_reason));
        if (s_reconnect_sequence_active && wifi_service_try_next_reconnect_slot() == ESP_OK)
        {
            break;
        }
        s_current_slot = WIFI_SVC_INVALID_SLOT;
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
        s_expected_disconnect = DISCONNECT_EXPECT_NONE;
        wifi_service_clear_reconnect_sequence();
        if (s_current_slot >= 0 && s_current_slot < WIFI_SVC_SLOT_COUNT)
        {
            s_last_successful_slot = s_current_slot;
            if (nvs_save_last_successful_slot(s_last_successful_slot) != ESP_OK)
            {
                ESP_LOGW(TAG, "failed to persist preferred WiFi slot %d", s_last_successful_slot + 1);
            }
        }
        xTimerStop(s_reconnect_timer, 0);
        post_event(WIFI_SVC_EVENT_CONNECTED, NULL, 0);
        wifi_service_start_connectivity_test();
        init_sntp();
        break;

    case CMD_EVT_CONNECTIVITY_OK:
        s_dns_lookup_active = false;
        s_connectivity_ok = true;
        ESP_LOGI(TAG, "connectivity test passed (%s resolved)", DNS_TEST_HOST);
        post_event(WIFI_SVC_EVENT_CONNECTIVITY_OK, NULL, 0);
        break;

    case CMD_EVT_CONNECTIVITY_FAILED:
        s_dns_lookup_active = false;
        s_connectivity_ok = false;
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

    s_autoreconnect = nvs_load_autoreconnect();
    s_initialised = true;
    s_last_successful_slot = nvs_load_last_successful_slot();
    s_current_slot = WIFI_SVC_INVALID_SLOT;
    s_expected_disconnect = DISCONNECT_EXPECT_NONE;
    wifi_service_clear_reconnect_sequence();
    ESP_ERROR_CHECK(power_mgmt_service_register_shutdown_callback(wifi_service_shutdown_callback,
                                                                  NULL,
                                                                  POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_WIFI));

    post_event(WIFI_SVC_EVENT_STARTED, NULL, 0);
    ESP_LOGI(TAG, "WiFi service started");

    if (s_autoreconnect)
    {
        err = wifi_service_start_reconnect_sequence();
        if (err != ESP_OK)
        {
            ESP_LOGI(TAG, "boot-time WiFi connect skipped: %s", esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGI(TAG, "boot-time WiFi connect skipped: autoreconnect disabled");
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

    err = wifi_service_save_slot(0, ssid, password);
    if (err != ESP_OK)
    {
        return err;
    }

    return wifi_service_connect_slot(0);
}

esp_err_t wifi_service_save_slot(uint8_t slot_index, const char *ssid, const char *password)
{
    char ssid_copy[33] = {0};
    char password_copy[65] = {0};
    esp_err_t err;

    if (!wifi_service_slot_index_valid(slot_index) || !ssid || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = wifi_copy_cstring(ssid_copy, sizeof(ssid_copy), ssid);
    if (err == ESP_OK)
    {
        err = wifi_copy_cstring(password_copy,
                                sizeof(password_copy),
                                password ? password : "");
    }
    if (err != ESP_OK)
    {
        wifi_zero_buffer(password_copy, sizeof(password_copy));
        return err;
    }

    err = nvs_save_slot(slot_index, ssid_copy, password_copy);
    wifi_zero_buffer(password_copy, sizeof(password_copy));
    return err;
}

esp_err_t wifi_service_connect_slot(uint8_t slot_index)
{
    char ssid[33] = {0};

    if (!wifi_service_slot_index_valid(slot_index))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (nvs_load_slot(slot_index, ssid, sizeof(ssid), NULL, 0) != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND;
    }

    wifi_svc_msg_t msg = {.cmd = CMD_CONNECT_SLOT,
                          .payload.slot_index = slot_index};

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
    wifi_svc_msg_t msg = {.cmd = CMD_RECONNECT_NOW};
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

bool wifi_service_has_internet(void)
{
    return s_connectivity_ok && s_state == WIFI_SVC_STATE_CONNECTED && s_have_ip_info;
}

esp_err_t wifi_service_get_saved_slots(wifi_svc_slot_info_t *out_slots, size_t slot_count)
{
    size_t slot_index;
    int active_slot;

    if (!out_slots || slot_count < WIFI_SVC_SLOT_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_slots, 0, WIFI_SVC_SLOT_COUNT * sizeof(*out_slots));
    active_slot = wifi_service_get_active_slot();

    for (slot_index = 0; slot_index < WIFI_SVC_SLOT_COUNT; slot_index++)
    {
        wifi_svc_slot_info_t *slot = &out_slots[slot_index];
        slot->configured = nvs_load_slot((uint8_t)slot_index,
                                         slot->ssid,
                                         sizeof(slot->ssid),
                                         NULL,
                                         0) == ESP_OK;
        slot->preferred = (int)slot_index == s_last_successful_slot;
        slot->active = (int)slot_index == active_slot;
        if (!slot->configured)
        {
            slot->ssid[0] = '\0';
        }
    }

    return ESP_OK;
}

int wifi_service_get_preferred_slot(void)
{
    return s_last_successful_slot;
}

int wifi_service_get_active_slot(void)
{
    if (s_state != WIFI_SVC_STATE_CONNECTING && s_state != WIFI_SVC_STATE_CONNECTED)
    {
        return WIFI_SVC_INVALID_SLOT;
    }

    return s_current_slot;
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
    s_connectivity_ok = false;
    s_sntp_initialised = false;
    s_last_successful_slot = WIFI_SVC_INVALID_SLOT;
    s_current_slot = WIFI_SVC_INVALID_SLOT;
    s_expected_disconnect = DISCONNECT_EXPECT_NONE;
    wifi_service_clear_reconnect_sequence();
    s_state = WIFI_SVC_STATE_IDLE;
    s_initialised = false;

    return ESP_OK;
}
