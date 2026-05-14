#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "bluetooth_service.h"
#include "power_mgmt_service.h"

#define BT_SVC_NAME_BLE_SPP "ESP_SPP_SERVER"
#define BT_SVC_BLE_SPP_SERVICE_UUID 0xABF0
#define BT_SVC_BLE_SPP_DATA_RECEIVE_UUID 0xABF1
#define BT_SVC_BLE_SPP_DATA_NOTIFY_UUID 0xABF2
#define BT_SVC_BLE_SPP_COMMAND_RECEIVE_UUID 0xABF3
#define BT_SVC_BLE_SPP_COMMAND_NOTIFY_UUID 0xABF4
#define BT_SVC_BLE_SPP_DATA_MAX_LEN 512
#define BT_SVC_BLE_SPP_COMMAND_MAX_LEN 20
#define BT_SVC_BLE_SPP_STATUS_MAX_LEN 20
#define BT_SVC_BLE_SPP_MTU_SIZE 512
#define BT_SVC_ADV_MIN_INTERVAL_MS 20
#define BT_SVC_ADV_MAX_INTERVAL_MS 40

ESP_EVENT_DEFINE_BASE(BLUETOOTH_SERVICE_EVENT);

void ble_store_config_init(void);

static const char *TAG = "bt_svc";

static const ble_uuid16_t s_spp_service_uuid = BLE_UUID16_INIT(BT_SVC_BLE_SPP_SERVICE_UUID);
static const ble_uuid16_t s_spp_data_receive_uuid = BLE_UUID16_INIT(BT_SVC_BLE_SPP_DATA_RECEIVE_UUID);
static const ble_uuid16_t s_spp_data_notify_uuid = BLE_UUID16_INIT(BT_SVC_BLE_SPP_DATA_NOTIFY_UUID);
static const ble_uuid16_t s_spp_command_uuid = BLE_UUID16_INIT(BT_SVC_BLE_SPP_COMMAND_RECEIVE_UUID);
static const ble_uuid16_t s_spp_status_uuid = BLE_UUID16_INIT(BT_SVC_BLE_SPP_COMMAND_NOTIFY_UUID);

static uint8_t s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_spp_data_notify_handle;
static uint16_t s_spp_status_handle;
static size_t s_spp_mtu = 23;
static bool s_initialised;
static bool s_host_started;
static bool s_spp_connected;
static bool s_spp_data_notifications_enabled;
static bool s_spp_status_notifications_enabled;
static bluetooth_service_connection_cb_t s_connection_cb;
static void *s_connection_cb_ctx;
static bluetooth_service_spp_rx_cb_t s_spp_rx_cb;
static void *s_spp_rx_cb_ctx;

static int bt_svc_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);
static int bt_svc_gap_event(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_spp_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s_spp_data_receive_uuid.u,
                .access_cb = bt_svc_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_spp_data_notify_uuid.u,
                .access_cb = bt_svc_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_spp_data_notify_handle,
            },
            {
                .uuid = &s_spp_command_uuid.u,
                .access_cb = bt_svc_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_spp_status_uuid.u,
                .access_cb = bt_svc_chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_spp_status_handle,
            },
            {0},
        },
    },
    {0},
};

static void bt_svc_post_event(bluetooth_service_event_id_t event_id, const void *data, size_t len)
{
    (void)esp_event_post(BLUETOOTH_SERVICE_EVENT, event_id, data, len, 0);
}

static void bt_svc_invoke_connection_callback(bluetooth_service_connection_event_t event)
{
    if (s_connection_cb)
    {
        s_connection_cb(event, s_connection_cb_ctx);
    }
}

static esp_err_t bt_svc_notify(uint16_t value_handle, const uint8_t *data, size_t len)
{
    struct os_mbuf *om;
    int rc;

    if (!s_spp_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data && len > 0)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > BT_SVC_BLE_SPP_DATA_MAX_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (!om)
    {
        return ESP_ERR_NO_MEM;
    }

    rc = ble_gatts_notify_custom(s_conn_handle, value_handle, om);
    if (rc != 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t bt_svc_send_status(const char *status)
{
    if (!s_spp_status_notifications_enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return bt_svc_notify(s_spp_status_handle, (const uint8_t *)status, strlen(status));
}

static int bt_svc_handle_write(uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt)
{
    uint8_t buffer[BT_SVC_BLE_SPP_DATA_MAX_LEN];
    uint16_t copied_len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, buffer, sizeof(buffer), &copied_len);
    if (rc != 0)
    {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &s_spp_command_uuid.u) == 0)
    {
        (void)attr_handle;
        if (copied_len > BT_SVC_BLE_SPP_COMMAND_MAX_LEN)
        {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        (void)bt_svc_send_status("OK");
        return 0;
    }

    if (ble_uuid_cmp(ctxt->chr->uuid, &s_spp_data_receive_uuid.u) == 0)
    {
        (void)attr_handle;
        if (s_spp_rx_cb)
        {
            s_spp_rx_cb(buffer, copied_len, s_spp_rx_cb_ctx);
        }
        else if (s_spp_data_notifications_enabled)
        {
            (void)bt_svc_notify(s_spp_data_notify_handle, buffer, copied_len);
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static int bt_svc_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return 0;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        return bt_svc_handle_write(attr_handle, ctxt);
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static void bt_svc_start_advertising(void)
{
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_gap_adv_params adv_params = {0};
    const char *name = ble_svc_gap_device_name();
    int rc;

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv_fields.name = (uint8_t *)name;
    adv_fields.name_len = strlen(name);
    adv_fields.name_is_complete = 1;
    adv_fields.uuids16 = (ble_uuid16_t *)&s_spp_service_uuid;
    adv_fields.num_uuids16 = 1;
    adv_fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "failed to set BLE advertising data: rc=%d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(BT_SVC_ADV_MIN_INTERVAL_MS);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(BT_SVC_ADV_MAX_INTERVAL_MS);

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, bt_svc_gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "failed to start BLE advertising: rc=%d", rc);
    }
}

static int bt_svc_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            s_spp_connected = true;
            s_conn_handle = event->connect.conn_handle;
            bt_svc_post_event(BLUETOOTH_SVC_EVENT_SPP_CONNECTED, NULL, 0);
            bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_SPP_CONNECTED);
        }
        else
        {
            bt_svc_start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_spp_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_spp_data_notifications_enabled = false;
        s_spp_status_notifications_enabled = false;
        bt_svc_post_event(BLUETOOTH_SVC_EVENT_SPP_DISCONNECTED, NULL, 0);
        bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_SPP_DISCONNECTED);
        bt_svc_start_advertising();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        bt_svc_start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_spp_data_notify_handle)
        {
            s_spp_data_notifications_enabled = event->subscribe.cur_notify || event->subscribe.cur_indicate;
        }
        else if (event->subscribe.attr_handle == s_spp_status_handle)
        {
            s_spp_status_notifications_enabled = event->subscribe.cur_notify || event->subscribe.cur_indicate;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        s_spp_mtu = event->mtu.value;
        break;
    default:
        break;
    }

    return 0;
}

static void bt_svc_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset: reason=%d", reason);
}

static void bt_svc_on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "failed to ensure BLE address: rc=%d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "failed to infer BLE address type: rc=%d", rc);
        return;
    }

    bt_svc_start_advertising();
}

static void bt_svc_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t bt_svc_gatt_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_svc_gap_device_name_set(BT_SVC_NAME_BLE_SPP);
    if (rc != 0)
    {
        return ESP_FAIL;
    }

    rc = ble_att_set_preferred_mtu(BT_SVC_BLE_SPP_MTU_SIZE);
    if (rc != 0)
    {
        return ESP_FAIL;
    }

    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0)
    {
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0)
    {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t bluetooth_service_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return bluetooth_service_shutdown();
}

esp_err_t bluetooth_service_init(void)
{
    esp_err_t err;

    if (s_initialised)
    {
        return ESP_OK;
    }

    err = nimble_port_init();
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to initialize NimBLE: %s", esp_err_to_name(err));
        return err;
    }

    err = bt_svc_gatt_init();
    if (err != ESP_OK)
    {
        (void)nimble_port_deinit();
        return err;
    }

    ble_hs_cfg.reset_cb = bt_svc_on_reset;
    ble_hs_cfg.sync_cb = bt_svc_on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    nimble_port_freertos_init(bt_svc_host_task);

    s_host_started = true;
    s_initialised = true;
    s_spp_mtu = 23;

    err = power_mgmt_service_register_shutdown_callback(bluetooth_service_shutdown_callback,
                                                        NULL,
                                                        30);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "failed to register Bluetooth shutdown callback: %s", esp_err_to_name(err));
    }

    bt_svc_post_event(BLUETOOTH_SVC_EVENT_STARTED, NULL, 0);
    bt_svc_post_event(BLUETOOTH_SVC_EVENT_SPP_READY, NULL, 0);
    ESP_LOGI(TAG, "NimBLE SPP service started (UUID 0x%04x)", BT_SVC_BLE_SPP_SERVICE_UUID);
    return ESP_OK;
}

void bluetooth_service_process_once(void)
{
}

void bluetooth_service_register_connection_callback(bluetooth_service_connection_cb_t callback, void *user_ctx)
{
    s_connection_cb = callback;
    s_connection_cb_ctx = user_ctx;
}

esp_err_t bluetooth_service_register_spp_rx_callback(bluetooth_service_spp_rx_cb_t callback, void *user_ctx)
{
    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_spp_rx_cb = callback;
    s_spp_rx_cb_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t bluetooth_service_spp_send_data(const uint8_t *data, size_t len)
{
    if (!s_spp_data_notifications_enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return bt_svc_notify(s_spp_data_notify_handle, data, len);
}

bool bluetooth_service_is_initialised(void)
{
    return s_initialised;
}

bool bluetooth_service_is_spp_connected(void)
{
    return s_spp_connected;
}

bool bluetooth_service_spp_notifications_enabled(void)
{
    return s_spp_data_notifications_enabled;
}

size_t bluetooth_service_spp_get_mtu(void)
{
    return s_spp_mtu;
}

size_t bluetooth_service_spp_get_max_payload(void)
{
    return s_spp_mtu > 3 ? s_spp_mtu - 3 : 0;
}

esp_err_t bluetooth_service_shutdown(void)
{
    if (!s_initialised)
    {
        return ESP_OK;
    }

    if (s_spp_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (s_host_started)
    {
        (void)nimble_port_stop();
        s_host_started = false;
    }

    (void)nimble_port_deinit();
    s_initialised = false;
    s_spp_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_spp_data_notifications_enabled = false;
    s_spp_status_notifications_enabled = false;
    return ESP_OK;
}