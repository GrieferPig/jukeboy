#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_attr.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"
#include "esp_event.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"

#include "bluetooth_service.h"
#include "player_service.h"
#include "power_mgmt_service.h"
#include "wifi_service.h"

#define BT_SVC_QUEUE_DEPTH 24
#define BT_SVC_API_QUEUE_TIMEOUT_MS 5
#define BT_SVC_MAX_CMDS_PER_PASS 6
#define BT_SVC_SHUTDOWN_POLL_MS 5
#define BT_SVC_MAX_DISCOVERED_DEVICES 16
#define BT_SVC_DISCOVERY_LENGTH 10
#define BT_SVC_NAME_CLASSIC "bttest-a2dp"
#define BT_SVC_NAME_BLE_SPP "ESP_SPP_SERVER"
#define BT_SVC_BLE_SPP_APP_ID 0x56
#define BT_SVC_BLE_SPP_SERVICE_UUID 0xABF0
#define BT_SVC_BLE_SPP_DATA_RECEIVE_UUID 0xABF1
#define BT_SVC_BLE_SPP_DATA_NOTIFY_UUID 0xABF2
#define BT_SVC_BLE_SPP_COMMAND_RECEIVE_UUID 0xABF3
#define BT_SVC_BLE_SPP_COMMAND_NOTIFY_UUID 0xABF4
#define BT_SVC_BLE_SPP_DATA_MAX_LEN 512
#define BT_SVC_BLE_SPP_COMMAND_MAX_LEN 20
#define BT_SVC_BLE_SPP_STATUS_MAX_LEN 20
#define BT_SVC_BLE_SPP_MTU_SIZE 512

static const char *TAG = "bt_svc";

typedef enum
{
    BT_SVC_BLE_SPP_IDX_SVC = 0,
    BT_SVC_BLE_SPP_IDX_DATA_RECV_CHAR,
    BT_SVC_BLE_SPP_IDX_DATA_RECV_VAL,
    BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_CHAR,
    BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_VAL,
    BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_CFG,
    BT_SVC_BLE_SPP_IDX_COMMAND_CHAR,
    BT_SVC_BLE_SPP_IDX_COMMAND_VAL,
    BT_SVC_BLE_SPP_IDX_STATUS_CHAR,
    BT_SVC_BLE_SPP_IDX_STATUS_VAL,
    BT_SVC_BLE_SPP_IDX_STATUS_CFG,
    BT_SVC_BLE_SPP_IDX_NB,
} bt_svc_ble_spp_attr_index_t;

static const uint8_t s_ble_spp_adv_data[] = {
    0x02,
    ESP_BLE_AD_TYPE_FLAG,
    0x02,
    0x03,
    ESP_BLE_AD_TYPE_16SRV_CMPL,
    0xF0,
    0xAB,
    0x0F,
    ESP_BLE_AD_TYPE_NAME_CMPL,
    'E',
    'S',
    'P',
    '_',
    'S',
    'P',
    'P',
    '_',
    'S',
    'E',
    'R',
    'V',
    'E',
    'R',
};

static esp_ble_adv_params_t s_ble_spp_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static const uint16_t s_ble_spp_primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_ble_spp_character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_ble_spp_character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t s_ble_spp_char_prop_read_write =
    ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static const uint8_t s_ble_spp_char_prop_read_notify =
    ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_INDICATE;
static const uint16_t s_ble_spp_service_uuid = BT_SVC_BLE_SPP_SERVICE_UUID;
static const uint16_t s_ble_spp_data_receive_uuid = BT_SVC_BLE_SPP_DATA_RECEIVE_UUID;
static const uint16_t s_ble_spp_data_notify_uuid = BT_SVC_BLE_SPP_DATA_NOTIFY_UUID;
static const uint16_t s_ble_spp_command_uuid = BT_SVC_BLE_SPP_COMMAND_RECEIVE_UUID;
static const uint16_t s_ble_spp_status_uuid = BT_SVC_BLE_SPP_COMMAND_NOTIFY_UUID;
static uint8_t s_ble_spp_data_receive_val[20] = {0};
static uint8_t s_ble_spp_data_notify_val[20] = {0};
static uint8_t s_ble_spp_data_notify_ccc[2] = {0x00, 0x00};
static uint8_t s_ble_spp_command_val[BT_SVC_BLE_SPP_COMMAND_MAX_LEN] = {0};
static uint8_t s_ble_spp_status_val[BT_SVC_BLE_SPP_STATUS_MAX_LEN] = {0};
static uint8_t s_ble_spp_status_ccc[2] = {0x00, 0x00};

static const esp_gatts_attr_db_t s_ble_spp_gatt_db[BT_SVC_BLE_SPP_IDX_NB] = {
    [BT_SVC_BLE_SPP_IDX_SVC] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_primary_service_uuid,
          ESP_GATT_PERM_READ,
          sizeof(s_ble_spp_service_uuid),
          sizeof(s_ble_spp_service_uuid),
          (uint8_t *)&s_ble_spp_service_uuid}},
    [BT_SVC_BLE_SPP_IDX_DATA_RECV_CHAR] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_character_declaration_uuid,
          ESP_GATT_PERM_READ,
          sizeof(uint8_t),
          sizeof(uint8_t),
          (uint8_t *)&s_ble_spp_char_prop_read_write}},
    [BT_SVC_BLE_SPP_IDX_DATA_RECV_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_data_receive_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          BT_SVC_BLE_SPP_DATA_MAX_LEN,
          sizeof(s_ble_spp_data_receive_val),
          s_ble_spp_data_receive_val}},
    [BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_CHAR] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_character_declaration_uuid,
          ESP_GATT_PERM_READ,
          sizeof(uint8_t),
          sizeof(uint8_t),
          (uint8_t *)&s_ble_spp_char_prop_read_notify}},
    [BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_data_notify_uuid,
          ESP_GATT_PERM_READ,
          BT_SVC_BLE_SPP_DATA_MAX_LEN,
          sizeof(s_ble_spp_data_notify_val),
          s_ble_spp_data_notify_val}},
    [BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_CFG] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_character_client_config_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          sizeof(uint16_t),
          sizeof(s_ble_spp_data_notify_ccc),
          s_ble_spp_data_notify_ccc}},
    [BT_SVC_BLE_SPP_IDX_COMMAND_CHAR] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_character_declaration_uuid,
          ESP_GATT_PERM_READ,
          sizeof(uint8_t),
          sizeof(uint8_t),
          (uint8_t *)&s_ble_spp_char_prop_read_write}},
    [BT_SVC_BLE_SPP_IDX_COMMAND_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_command_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          BT_SVC_BLE_SPP_COMMAND_MAX_LEN,
          sizeof(s_ble_spp_command_val),
          s_ble_spp_command_val}},
    [BT_SVC_BLE_SPP_IDX_STATUS_CHAR] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_character_declaration_uuid,
          ESP_GATT_PERM_READ,
          sizeof(uint8_t),
          sizeof(uint8_t),
          (uint8_t *)&s_ble_spp_char_prop_read_notify}},
    [BT_SVC_BLE_SPP_IDX_STATUS_VAL] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_status_uuid,
          ESP_GATT_PERM_READ,
          BT_SVC_BLE_SPP_STATUS_MAX_LEN,
          sizeof(s_ble_spp_status_val),
          s_ble_spp_status_val}},
    [BT_SVC_BLE_SPP_IDX_STATUS_CFG] =
        {{ESP_GATT_AUTO_RSP},
         {ESP_UUID_LEN_16,
          (uint8_t *)&s_ble_spp_character_client_config_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
          sizeof(uint16_t),
          sizeof(s_ble_spp_status_ccc),
          s_ble_spp_status_ccc}},
};

ESP_EVENT_DEFINE_BASE(BLUETOOTH_SERVICE_EVENT);

typedef enum
{
    BT_SVC_CMD_PAIR_BEST,
    BT_SVC_CMD_CONNECT_LAST_BONDED,
    BT_SVC_CMD_PAIRING_CONFIRM_REQUEST,
    BT_SVC_CMD_PAIRING_CONFIRM_REPLY,
    BT_SVC_CMD_DISCONNECT_A2DP,
    BT_SVC_CMD_START_AUDIO,
    BT_SVC_CMD_SUSPEND_AUDIO,
    BT_SVC_CMD_SEND_MEDIA_KEY,
    BT_SVC_CMD_DISCOVERY_RESULT,
    BT_SVC_CMD_DISCOVERY_STATE,
    BT_SVC_CMD_AUTH_COMPLETE,
    BT_SVC_CMD_A2DP_CONNECTION_STATE,
    BT_SVC_CMD_A2DP_AUDIO_STATE,
    BT_SVC_CMD_AVRCP_PASSTHROUGH_RSP,
    BT_SVC_CMD_AVRCP_PASSTHROUGH_CMD,
    BT_SVC_CMD_AVRCP_SET_ABS_VOL,
    BT_SVC_CMD_AVRCP_REG_VOL_NOTIFY,
    BT_SVC_CMD_AVRCP_CT_VOL_CHANGE,
    BT_SVC_CMD_AVRCP_CT_RN_CAPS,
    BT_SVC_CMD_SPP_CONNECTED,
    BT_SVC_CMD_SPP_DISCONNECTED,
} bluetooth_service_cmd_t;

typedef struct
{
    bool valid;
    esp_bd_addr_t bda;
    int8_t rssi;
    uint32_t cod;
    char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
} bluetooth_service_discovery_result_t;

typedef struct
{
    bluetooth_service_cmd_t cmd;
    union
    {
        bluetooth_service_discovery_result_t discovery_result;
        esp_bt_gap_discovery_state_t discovery_state;
        struct
        {
            esp_bd_addr_t bda;
            esp_bt_status_t status;
            char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
        } auth;
        struct
        {
            esp_bd_addr_t bda;
            uint32_t numeric_value;
            bool accept;
        } pairing_confirm;
        struct
        {
            esp_a2d_connection_state_t state;
            esp_bd_addr_t remote_bda;
        } a2dp_connection;
        struct
        {
            esp_a2d_audio_state_t state;
            esp_bd_addr_t remote_bda;
        } a2dp_audio;
        struct
        {
            uint8_t key_code;
            uint8_t key_state;
            esp_avrc_rsp_t rsp_code;
        } avrc_rsp;
        struct
        {
            uint8_t key_code;
            uint8_t key_state;
        } avrc_cmd;
        struct
        {
            uint8_t volume;
        } abs_vol;
        struct
        {
            uint8_t event_id;
        } reg_ntf;
        struct
        {
            uint8_t volume; /* 0..127 from remote TG */
        } ct_vol_change;
        esp_avrc_rn_evt_cap_mask_t ct_rn_caps;
        struct
        {
            esp_bd_addr_t remote_bda;
        } spp_remote;
        uint8_t media_key;
    } data;
} bluetooth_service_msg_t;

static QueueHandle_t s_cmd_queue;
static bool s_initialised;
static bool s_pair_request_pending;
static bool s_discovery_running;
static bool s_a2dp_connected;
static esp_bd_addr_t s_connected_bda;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t s_avrc_tl;
static bool s_avrc_rn_vol_registered;
static bool s_avrc_ct_vol_registered;
static bool s_spp_connected;
static bool s_spp_data_notifications_enabled;
static bool s_spp_data_indications_enabled;
static bool s_spp_status_notifications_enabled;
static uint16_t s_spp_conn_id = 0xFFFF;
static uint16_t s_spp_mtu = 23;
static esp_gatt_if_t s_spp_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_spp_handle_table[BT_SVC_BLE_SPP_IDX_NB];
static esp_bd_addr_t s_spp_remote_bda;
EXT_RAM_BSS_ATTR static bluetooth_service_discovery_result_t s_discovered[BT_SVC_MAX_DISCOVERED_DEVICES];
static size_t s_discovered_count;
static bluetooth_service_pcm_provider_t s_pcm_provider;
static void *s_pcm_provider_ctx;
static bool s_pairing_confirm_pending;
static esp_bd_addr_t s_pairing_confirm_bda;
static uint32_t s_pairing_confirm_numeric_value;
static bluetooth_service_connection_cb_t s_connection_cb;
static void *s_connection_cb_ctx;
static bluetooth_service_media_key_cb_t s_media_key_cb;
static void *s_media_key_cb_ctx;

static void bt_svc_post_event(bluetooth_service_event_id_t event_id, const void *data, size_t len)
{
    esp_event_post(BLUETOOTH_SERVICE_EVENT, event_id, data, len, 0);
}

static bool bt_svc_queue_send(const bluetooth_service_msg_t *msg)
{
    return s_cmd_queue && xQueueSend(s_cmd_queue, msg, 0) == pdPASS;
}

static TickType_t bt_svc_queue_timeout_ticks(void)
{
    TickType_t timeout_ticks = pdMS_TO_TICKS(BT_SVC_API_QUEUE_TIMEOUT_MS);
    return timeout_ticks == 0 ? 1 : timeout_ticks;
}

static TickType_t bt_svc_shutdown_poll_ticks(void)
{
    TickType_t poll_ticks = pdMS_TO_TICKS(BT_SVC_SHUTDOWN_POLL_MS);
    return poll_ticks == 0 ? 1 : poll_ticks;
}

static uint8_t bt_svc_spp_find_attr_index(uint16_t handle)
{
    for (size_t index = 0; index < BT_SVC_BLE_SPP_IDX_NB; index++)
    {
        if (s_spp_handle_table[index] == handle)
        {
            return (uint8_t)index;
        }
    }

    return UINT8_MAX;
}

static void bt_svc_spp_reset_link_state(void)
{
    s_spp_connected = false;
    s_spp_data_notifications_enabled = false;
    s_spp_data_indications_enabled = false;
    s_spp_status_notifications_enabled = false;
    s_spp_conn_id = 0xFFFF;
    s_spp_mtu = 23;
    memset(s_spp_remote_bda, 0, sizeof(s_spp_remote_bda));
}

static esp_err_t bt_svc_spp_send_value(uint16_t handle,
                                       const uint8_t *value,
                                       size_t value_len,
                                       bool need_confirm)
{
    size_t max_payload;

    if (!value || value_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_spp_connected || s_spp_gatts_if == ESP_GATT_IF_NONE || s_spp_conn_id == 0xFFFF || handle == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    max_payload = s_spp_mtu > 3 ? (size_t)(s_spp_mtu - 3) : 0;
    if (max_payload == 0)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (value_len > max_payload)
    {
        value_len = max_payload;
    }

    return esp_ble_gatts_send_indicate(s_spp_gatts_if,
                                       s_spp_conn_id,
                                       handle,
                                       (uint16_t)value_len,
                                       (uint8_t *)value,
                                       need_confirm);
}

static esp_err_t bt_svc_spp_send_data(const uint8_t *value, size_t value_len)
{
    if (!s_spp_data_notifications_enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    return bt_svc_spp_send_value(s_spp_handle_table[BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_VAL],
                                 value,
                                 value_len,
                                 s_spp_data_indications_enabled);
}

static esp_err_t bt_svc_spp_send_status(const char *status)
{
    size_t status_len;

    if (!status || !s_spp_status_notifications_enabled)
    {
        return ESP_ERR_INVALID_STATE;
    }

    status_len = strlen(status);
    if (status_len == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (status_len > BT_SVC_BLE_SPP_STATUS_MAX_LEN)
    {
        status_len = BT_SVC_BLE_SPP_STATUS_MAX_LEN;
    }

    return bt_svc_spp_send_value(s_spp_handle_table[BT_SVC_BLE_SPP_IDX_STATUS_VAL],
                                 (const uint8_t *)status,
                                 status_len,
                                 false);
}

static void bt_svc_set_pending_pairing_confirm(const esp_bd_addr_t bda, uint32_t numeric_value)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_pairing_confirm_pending = true;
    memcpy(s_pairing_confirm_bda, bda, sizeof(s_pairing_confirm_bda));
    s_pairing_confirm_numeric_value = numeric_value;
    taskEXIT_CRITICAL(&s_state_lock);
}

static void bt_svc_clear_pending_pairing_confirm(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_pairing_confirm_pending = false;
    memset(s_pairing_confirm_bda, 0, sizeof(s_pairing_confirm_bda));
    s_pairing_confirm_numeric_value = 0;
    taskEXIT_CRITICAL(&s_state_lock);
}

static bool bt_svc_get_pending_pairing_confirm(bluetooth_service_pairing_confirm_t *confirm)
{
    bool pending;

    if (!confirm)
    {
        return false;
    }

    memset(confirm, 0, sizeof(*confirm));
    taskENTER_CRITICAL(&s_state_lock);
    pending = s_pairing_confirm_pending;
    confirm->pending = pending;
    if (pending)
    {
        memcpy(confirm->remote_bda, s_pairing_confirm_bda, sizeof(confirm->remote_bda));
        confirm->numeric_value = s_pairing_confirm_numeric_value;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static bool bt_svc_take_pending_pairing_confirm(esp_bd_addr_t bda, uint32_t *numeric_value)
{
    bool pending;

    taskENTER_CRITICAL(&s_state_lock);
    pending = s_pairing_confirm_pending;
    if (pending)
    {
        memcpy(bda, s_pairing_confirm_bda, ESP_BD_ADDR_LEN);
        if (numeric_value)
        {
            *numeric_value = s_pairing_confirm_numeric_value;
        }
        s_pairing_confirm_pending = false;
        memset(s_pairing_confirm_bda, 0, sizeof(s_pairing_confirm_bda));
        s_pairing_confirm_numeric_value = 0;
    }
    taskEXIT_CRITICAL(&s_state_lock);
    return pending;
}

static esp_err_t bt_svc_validate_a2dp_state(bool require_connected, const char *action)
{
    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (require_connected)
    {
        if (!s_a2dp_connected)
        {
            ESP_LOGW(TAG, "%s rejected: no A2DP device connected", action);
            return ESP_ERR_INVALID_STATE;
        }
    }
    else if (s_a2dp_connected)
    {
        ESP_LOGW(TAG, "%s rejected: A2DP device already connected", action);
        return ESP_ERR_INVALID_STATE;
    }

    return ESP_OK;
}

static void bt_svc_invoke_connection_callback(bluetooth_service_connection_event_t event,
                                              const esp_bd_addr_t remote_bda)
{
    if (s_connection_cb)
    {
        s_connection_cb(event, remote_bda, s_connection_cb_ctx);
    }
}

static void bt_svc_reset_discovery_results(void)
{
    memset(s_discovered, 0, sizeof(s_discovered));
    s_discovered_count = 0;
}

static bluetooth_service_discovery_result_t *bt_svc_find_discovery_result(const esp_bd_addr_t bda)
{
    for (size_t index = 0; index < s_discovered_count; index++)
    {
        if (memcmp(s_discovered[index].bda, bda, ESP_BD_ADDR_LEN) == 0)
        {
            return &s_discovered[index];
        }
    }
    return NULL;
}

static bool bt_svc_is_a2dp_sink_candidate(uint32_t cod)
{
    if (!esp_bt_gap_is_valid_cod(cod))
    {
        return false;
    }
    return esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV;
}

static void bt_svc_select_and_connect_best_sink(void)
{
    bluetooth_service_discovery_result_t *best = NULL;

    for (size_t index = 0; index < s_discovered_count; index++)
    {
        if (!s_discovered[index].valid)
        {
            continue;
        }
        if (!best || s_discovered[index].rssi > best->rssi)
        {
            best = &s_discovered[index];
        }
    }

    bt_svc_post_event(BLUETOOTH_SVC_EVENT_DISCOVERY_DONE, NULL, 0);

    if (!best)
    {
        ESP_LOGW(TAG, "no A2DP sink candidates found");
        return;
    }

    ESP_LOGI(TAG,
             "connecting to best sink " ESP_BD_ADDR_STR " (%s, RSSI %d)",
             ESP_BD_ADDR_HEX(best->bda),
             best->name[0] ? best->name : "unknown",
             best->rssi);
    esp_a2d_source_connect(best->bda);
}

static esp_err_t bt_svc_connect_last_bonded_device(void)
{
    int device_count = esp_bt_gap_get_bond_device_num();
    esp_bd_addr_t *devices;
    esp_err_t err;

    if (device_count <= 0)
    {
        ESP_LOGW(TAG, "no bonded Bluetooth devices available");
        return ESP_ERR_NOT_FOUND;
    }

    devices = calloc((size_t)device_count, sizeof(*devices));
    if (!devices)
    {
        return ESP_ERR_NO_MEM;
    }

    err = esp_bt_gap_get_bond_device_list(&device_count, devices);
    if (err != ESP_OK)
    {
        free(devices);
        return err;
    }

    if (device_count <= 0)
    {
        free(devices);
        ESP_LOGW(TAG, "bonded device list unexpectedly empty");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "connecting to last bonded sink " ESP_BD_ADDR_STR,
             ESP_BD_ADDR_HEX(devices[device_count - 1]));
    err = esp_a2d_source_connect(devices[device_count - 1]);
    free(devices);
    return err;
}

static void bt_svc_store_discovery_result(const bluetooth_service_discovery_result_t *result)
{
    bluetooth_service_discovery_result_t *slot = bt_svc_find_discovery_result(result->bda);
    if (!slot)
    {
        if (s_discovered_count >= BT_SVC_MAX_DISCOVERED_DEVICES)
        {
            return;
        }
        slot = &s_discovered[s_discovered_count++];
    }

    *slot = *result;
    slot->valid = true;
}

static int32_t bt_svc_a2dp_data_cb(uint8_t *data, int32_t len)
{
    bluetooth_service_pcm_provider_t provider;
    void *provider_ctx;
    int32_t filled = 0;

    if (!data || len <= 0)
    {
        return 0;
    }

    memset(data, 0, (size_t)len);

    taskENTER_CRITICAL(&s_state_lock);
    provider = s_pcm_provider;
    provider_ctx = s_pcm_provider_ctx;
    taskEXIT_CRITICAL(&s_state_lock);

    if (provider)
    {
        filled = provider(data, len, provider_ctx);
        if (filled < 0)
        {
            filled = 0;
        }
        if (filled < len)
        {
            memset(data + filled, 0, (size_t)(len - filled));
            filled = len;
        }
    }
    else
    {
        filled = len;
    }

    return filled;
}

static void bt_svc_gap_bt_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    bluetooth_service_msg_t msg = {0};

    switch (event)
    {
    case ESP_BT_GAP_DISC_RES_EVT:
    {
        bluetooth_service_discovery_result_t result = {0};
        uint8_t eir_length = 0;
        uint8_t *eir_name = NULL;

        memcpy(result.bda, param->disc_res.bda, ESP_BD_ADDR_LEN);
        result.rssi = INT8_MIN;

        for (int prop_index = 0; prop_index < param->disc_res.num_prop; prop_index++)
        {
            esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[prop_index];
            switch (prop->type)
            {
            case ESP_BT_GAP_DEV_PROP_COD:
                result.cod = *(uint32_t *)prop->val;
                break;
            case ESP_BT_GAP_DEV_PROP_RSSI:
                result.rssi = *(int8_t *)prop->val;
                break;
            case ESP_BT_GAP_DEV_PROP_BDNAME:
                if (prop->len > 0)
                {
                    size_t copy_len = prop->len < ESP_BT_GAP_MAX_BDNAME_LEN ? prop->len : ESP_BT_GAP_MAX_BDNAME_LEN;
                    memcpy(result.name, prop->val, copy_len);
                    result.name[copy_len] = '\0';
                }
                break;
            case ESP_BT_GAP_DEV_PROP_EIR:
                eir_name = esp_bt_gap_resolve_eir_data((uint8_t *)prop->val,
                                                       ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                       &eir_length);
                if (eir_name && eir_length > 0 && !result.name[0])
                {
                    size_t copy_len = eir_length < ESP_BT_GAP_MAX_BDNAME_LEN ? eir_length : ESP_BT_GAP_MAX_BDNAME_LEN;
                    memcpy(result.name, eir_name, copy_len);
                    result.name[copy_len] = '\0';
                }
                break;
            default:
                break;
            }
        }

        if (!bt_svc_is_a2dp_sink_candidate(result.cod))
        {
            break;
        }

        result.valid = true;
        msg.cmd = BT_SVC_CMD_DISCOVERY_RESULT;
        msg.data.discovery_result = result;
        bt_svc_queue_send(&msg);
        break;
    }
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        msg.cmd = BT_SVC_CMD_DISCOVERY_STATE;
        msg.data.discovery_state = param->disc_st_chg.state;
        bt_svc_queue_send(&msg);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        msg.cmd = BT_SVC_CMD_AUTH_COMPLETE;
        memcpy(msg.data.auth.bda, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        msg.data.auth.status = param->auth_cmpl.stat;
        strncpy(msg.data.auth.name,
                (const char *)param->auth_cmpl.device_name,
                sizeof(msg.data.auth.name) - 1);
        bt_svc_queue_send(&msg);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
    {
        esp_bt_pin_code_t pin_code;
        memset(pin_code, '0', sizeof(pin_code));
        esp_bt_gap_pin_reply(param->pin_req.bda,
                             true,
                             param->pin_req.min_16_digit ? 16 : 4,
                             pin_code);
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        msg.cmd = BT_SVC_CMD_PAIRING_CONFIRM_REQUEST;
        memcpy(msg.data.pairing_confirm.bda, param->cfm_req.bda, ESP_BD_ADDR_LEN);
        msg.data.pairing_confirm.numeric_value = param->cfm_req.num_val;
        bt_svc_queue_send(&msg);
        break;
    default:
        break;
    }
}

static void bt_svc_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    bluetooth_service_msg_t msg = {0};

    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
        msg.cmd = BT_SVC_CMD_A2DP_CONNECTION_STATE;
        msg.data.a2dp_connection.state = param->conn_stat.state;
        memcpy(msg.data.a2dp_connection.remote_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
        bt_svc_queue_send(&msg);
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        msg.cmd = BT_SVC_CMD_A2DP_AUDIO_STATE;
        msg.data.a2dp_audio.state = param->audio_stat.state;
        memcpy(msg.data.a2dp_audio.remote_bda, param->audio_stat.remote_bda, ESP_BD_ADDR_LEN);
        bt_svc_queue_send(&msg);
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG,
                 "audio cfg from " ESP_BD_ADDR_STR " codec_type=%d",
                 ESP_BD_ADDR_HEX(param->audio_cfg.remote_bda),
                 param->audio_cfg.mcc.type);
        break;
    default:
        break;
    }
}

static void bt_svc_avrc_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    bluetooth_service_msg_t msg = {0};

    switch (event)
    {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG,
                 "AVRCP %s " ESP_BD_ADDR_STR,
                 param->conn_stat.connected ? "connected" : "disconnected",
                 ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
        if (param->conn_stat.connected)
        {
            s_avrc_ct_vol_registered = false;
            s_avrc_tl = (s_avrc_tl + 1) & 0x0F;
            esp_avrc_ct_send_get_rn_capabilities_cmd(s_avrc_tl);
        }
        break;
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        msg.cmd = BT_SVC_CMD_AVRCP_CT_RN_CAPS;
        msg.data.ct_rn_caps = param->get_rn_caps_rsp.evt_set;
        bt_svc_queue_send(&msg);
        break;
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        if (param->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
        {
            msg.cmd = BT_SVC_CMD_AVRCP_CT_VOL_CHANGE;
            msg.data.ct_vol_change.volume = param->change_ntf.event_parameter.volume;
            bt_svc_queue_send(&msg);
        }
        break;
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        msg.cmd = BT_SVC_CMD_AVRCP_PASSTHROUGH_RSP;
        msg.data.avrc_rsp.key_code = param->psth_rsp.key_code;
        msg.data.avrc_rsp.key_state = param->psth_rsp.key_state;
        msg.data.avrc_rsp.rsp_code = param->psth_rsp.rsp_code;
        bt_svc_queue_send(&msg);
        break;
    default:
        break;
    }
}

static player_service_control_t bt_svc_player_control_from_key(uint8_t key_code, bool *handled)
{
    if (handled)
    {
        *handled = true;
    }

    switch (key_code)
    {
    case ESP_AVRC_PT_CMD_FORWARD:
        return PLAYER_SVC_CONTROL_NEXT;
    case ESP_AVRC_PT_CMD_BACKWARD:
        return PLAYER_SVC_CONTROL_PREVIOUS;
    case ESP_AVRC_PT_CMD_FAST_FORWARD:
        return PLAYER_SVC_CONTROL_FAST_FORWARD;
    case ESP_AVRC_PT_CMD_REWIND:
        return PLAYER_SVC_CONTROL_FAST_BACKWARD;
    case ESP_AVRC_PT_CMD_VOL_UP:
        return PLAYER_SVC_CONTROL_VOLUME_UP;
    case ESP_AVRC_PT_CMD_VOL_DOWN:
        return PLAYER_SVC_CONTROL_VOLUME_DOWN;
    case ESP_AVRC_PT_CMD_PAUSE:
    case ESP_AVRC_PT_CMD_PLAY:
    case ESP_AVRC_PT_CMD_STOP:
        return PLAYER_SVC_CONTROL_PAUSE;
    default:
        if (handled)
        {
            *handled = false;
        }
        return PLAYER_SVC_CONTROL_PAUSE;
    }
}

static void bt_svc_avrc_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    bluetooth_service_msg_t msg = {0};

    switch (event)
    {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG,
                 "AVRCP TG %s " ESP_BD_ADDR_STR,
                 param->conn_stat.connected ? "connected" : "disconnected",
                 ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
        break;
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        msg.cmd = BT_SVC_CMD_AVRCP_PASSTHROUGH_CMD;
        msg.data.avrc_cmd.key_code = param->psth_cmd.key_code;
        msg.data.avrc_cmd.key_state = param->psth_cmd.key_state;
        bt_svc_queue_send(&msg);
        break;
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
        ESP_LOGI(TAG, "AVRCP TG set_abs_vol=%u (remote CT->our TG)",
                 (unsigned)param->set_abs_vol.volume);
        msg.cmd = BT_SVC_CMD_AVRCP_SET_ABS_VOL;
        msg.data.abs_vol.volume = param->set_abs_vol.volume;
        bt_svc_queue_send(&msg);
        break;
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        msg.cmd = BT_SVC_CMD_AVRCP_REG_VOL_NOTIFY;
        msg.data.reg_ntf.event_id = param->reg_ntf.event_id;
        bt_svc_queue_send(&msg);
        break;
    default:
        break;
    }
}

static void bt_svc_gap_ble_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    esp_err_t err;

    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        err = esp_ble_gap_start_advertising(&s_ble_spp_adv_params);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "BLE SPP advertising start failed: %s", esp_err_to_name(err));
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGW(TAG, "BLE SPP advertising did not start: status=%d", param->adv_start_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
        if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGW(TAG, "BLE SPP advertising did not stop cleanly: status=%d", param->adv_stop_cmpl.status);
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG,
                 "BLE conn params update status=%d interval=%d latency=%d timeout=%d",
                 param->update_conn_params.status,
                 param->update_conn_params.conn_int,
                 param->update_conn_params.latency,
                 param->update_conn_params.timeout);
        break;
    default:
        break;
    }
}

static void bt_svc_spp_gatts_cb(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
    bluetooth_service_msg_t msg = {0};
    esp_err_t err;

    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK)
        {
            ESP_LOGW(TAG, "BLE SPP GATT app register failed: status=%d", param->reg.status);
            break;
        }

        s_spp_gatts_if = gatts_if;
        err = esp_ble_gap_set_device_name(BT_SVC_NAME_BLE_SPP);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "BLE SPP device name set failed: %s", esp_err_to_name(err));
        }

        err = esp_ble_gap_config_adv_data_raw((uint8_t *)s_ble_spp_adv_data, sizeof(s_ble_spp_adv_data));
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "BLE SPP adv data config failed: %s", esp_err_to_name(err));
        }

        err = esp_ble_gatts_create_attr_tab(s_ble_spp_gatt_db,
                                            gatts_if,
                                            BT_SVC_BLE_SPP_IDX_NB,
                                            0);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "BLE SPP attribute table create failed: %s", esp_err_to_name(err));
        }
        break;
    case ESP_GATTS_WRITE_EVT:
        if (!param->write.is_prep)
        {
            uint8_t attr_index = bt_svc_spp_find_attr_index(param->write.handle);

            switch (attr_index)
            {
            case BT_SVC_BLE_SPP_IDX_DATA_NOTIFY_CFG:
                if (param->write.len == 2)
                {
                    const uint8_t *value = param->write.value;

                    if (value[0] == 0x01 && value[1] == 0x00)
                    {
                        s_spp_data_notifications_enabled = true;
                        s_spp_data_indications_enabled = false;
                    }
                    else if (value[0] == 0x02 && value[1] == 0x00)
                    {
                        s_spp_data_notifications_enabled = true;
                        s_spp_data_indications_enabled = true;
                    }
                    else if (value[0] == 0x00 && value[1] == 0x00)
                    {
                        s_spp_data_notifications_enabled = false;
                        s_spp_data_indications_enabled = false;
                    }
                }
                break;
            case BT_SVC_BLE_SPP_IDX_STATUS_CFG:
                if (param->write.len == 2)
                {
                    const uint8_t *value = param->write.value;
                    s_spp_status_notifications_enabled =
                        ((value[0] == 0x01 || value[0] == 0x02) && value[1] == 0x00);
                }
                break;
            case BT_SVC_BLE_SPP_IDX_COMMAND_VAL:
                if (param->write.len > 0)
                {
                    err = bt_svc_spp_send_status("OK");
                    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
                    {
                        ESP_LOGW(TAG, "BLE SPP status notify failed: %s", esp_err_to_name(err));
                    }
                }
                break;
            case BT_SVC_BLE_SPP_IDX_DATA_RECV_VAL:
                if (param->write.len > 0)
                {
                    err = bt_svc_spp_send_data(param->write.value, param->write.len);
                    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
                    {
                        ESP_LOGW(TAG, "BLE SPP echo notify failed: %s", esp_err_to_name(err));
                    }
                }
                break;
            default:
                break;
            }
        }
        break;
    case ESP_GATTS_MTU_EVT:
        s_spp_mtu = param->mtu.mtu;
        break;
    case ESP_GATTS_CONF_EVT:
        if (param->conf.status != ESP_GATT_OK)
        {
            ESP_LOGW(TAG,
                     "BLE SPP indication confirm failed: status=%d handle=%u",
                     param->conf.status,
                     (unsigned)param->conf.handle);
        }
        break;
    case ESP_GATTS_START_EVT:
        if (param->start.status == ESP_GATT_OK)
        {
            bt_svc_post_event(BLUETOOTH_SVC_EVENT_SPP_READY, NULL, 0);
        }
        else
        {
            ESP_LOGW(TAG, "BLE SPP service start failed: status=%d", param->start.status);
        }
        break;
    case ESP_GATTS_CONNECT_EVT:
        s_spp_connected = true;
        s_spp_conn_id = param->connect.conn_id;
        s_spp_gatts_if = gatts_if;
        memcpy(s_spp_remote_bda, param->connect.remote_bda, ESP_BD_ADDR_LEN);
        msg.cmd = BT_SVC_CMD_SPP_CONNECTED;
        memcpy(msg.data.spp_remote.remote_bda, param->connect.remote_bda, ESP_BD_ADDR_LEN);
        bt_svc_queue_send(&msg);
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        msg.cmd = BT_SVC_CMD_SPP_DISCONNECTED;
        memcpy(msg.data.spp_remote.remote_bda, param->disconnect.remote_bda, ESP_BD_ADDR_LEN);
        bt_svc_spp_reset_link_state();
        bt_svc_queue_send(&msg);
        err = esp_ble_gap_start_advertising(&s_ble_spp_adv_params);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "BLE SPP advertising restart failed: %s", esp_err_to_name(err));
        }
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK)
        {
            ESP_LOGW(TAG, "BLE SPP attr table create failed: status=0x%x", param->add_attr_tab.status);
        }
        else if (param->add_attr_tab.num_handle != BT_SVC_BLE_SPP_IDX_NB)
        {
            ESP_LOGW(TAG,
                     "BLE SPP attr table handle mismatch: got=%u expected=%u",
                     (unsigned)param->add_attr_tab.num_handle,
                     (unsigned)BT_SVC_BLE_SPP_IDX_NB);
        }
        else
        {
            memcpy(s_spp_handle_table,
                   param->add_attr_tab.handles,
                   sizeof(s_spp_handle_table));
            err = esp_ble_gatts_start_service(s_spp_handle_table[BT_SVC_BLE_SPP_IDX_SVC]);
            if (err != ESP_OK)
            {
                ESP_LOGW(TAG, "BLE SPP start service failed: %s", esp_err_to_name(err));
            }
        }
        break;
    default:
        break;
    }
}

static esp_err_t bt_svc_ignore_invalid_state(esp_err_t err)
{
    return (err == ESP_ERR_INVALID_STATE) ? ESP_OK : err;
}

static esp_err_t bt_svc_wait_for_disconnect_state(bool *flag, bool target_value, TickType_t timeout_ticks)
{
    TickType_t start_ticks = xTaskGetTickCount();
    TickType_t poll_ticks = bt_svc_shutdown_poll_ticks();

    while (*flag != target_value)
    {
        bluetooth_service_process_once();
        if ((xTaskGetTickCount() - start_ticks) >= timeout_ticks)
        {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(poll_ticks);
    }

    return ESP_OK;
}

static esp_err_t bluetooth_service_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return bluetooth_service_shutdown();
}

static void bt_svc_process_msg(const bluetooth_service_msg_t *msg)
{
    switch (msg->cmd)
    {
    case BT_SVC_CMD_PAIR_BEST:
    {
        if (s_a2dp_connected)
        {
            ESP_LOGW(TAG, "ignoring pair request: A2DP device already connected");
            break;
        }
        bt_svc_reset_discovery_results();
        s_pair_request_pending = true;
        if (s_discovery_running)
        {
            esp_bt_gap_cancel_discovery();
        }
        ESP_LOGI(TAG, "starting discovery for A2DP sink pairing");
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                   BT_SVC_DISCOVERY_LENGTH,
                                   0);
        break;
    }
    case BT_SVC_CMD_PAIRING_CONFIRM_REQUEST:
    {
        bt_svc_set_pending_pairing_confirm(msg->data.pairing_confirm.bda,
                                           msg->data.pairing_confirm.numeric_value);
        ESP_LOGW(TAG,
                 "pairing confirm pending for " ESP_BD_ADDR_STR " passkey=%06lu; use 'bt confirm accept' or 'bt confirm reject'",
                 ESP_BD_ADDR_HEX(msg->data.pairing_confirm.bda),
                 (unsigned long)msg->data.pairing_confirm.numeric_value);
        break;
    }
    case BT_SVC_CMD_PAIRING_CONFIRM_REPLY:
    {
        esp_bd_addr_t remote_bda = {0};
        uint32_t numeric_value = 0;

        if (!bt_svc_take_pending_pairing_confirm(remote_bda, &numeric_value))
        {
            ESP_LOGW(TAG, "ignoring pairing confirm reply: no pending confirmation");
            break;
        }

        ESP_LOGI(TAG,
                 "%s pairing confirm for " ESP_BD_ADDR_STR " passkey=%06lu",
                 msg->data.pairing_confirm.accept ? "accepting" : "rejecting",
                 ESP_BD_ADDR_HEX(remote_bda),
                 (unsigned long)numeric_value);
        esp_bt_gap_ssp_confirm_reply(remote_bda, msg->data.pairing_confirm.accept);
        break;
    }
    case BT_SVC_CMD_CONNECT_LAST_BONDED:
    {
        if (s_a2dp_connected)
        {
            ESP_LOGW(TAG, "ignoring connect request: A2DP device already connected");
            break;
        }
        esp_err_t err = bt_svc_connect_last_bonded_device();
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "failed to connect last bonded device: %s", esp_err_to_name(err));
        }
        break;
    }
    case BT_SVC_CMD_DISCONNECT_A2DP:
    {
        if (!s_a2dp_connected)
        {
            ESP_LOGW(TAG, "ignoring disconnect request: no A2DP device connected");
            break;
        }
        esp_a2d_source_disconnect(s_connected_bda);
        break;
    }
    case BT_SVC_CMD_START_AUDIO:
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        break;
    case BT_SVC_CMD_SUSPEND_AUDIO:
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        break;
    case BT_SVC_CMD_SEND_MEDIA_KEY:
        esp_avrc_ct_send_passthrough_cmd(s_avrc_tl, msg->data.media_key, ESP_AVRC_PT_CMD_STATE_PRESSED);
        esp_avrc_ct_send_passthrough_cmd(s_avrc_tl, msg->data.media_key, ESP_AVRC_PT_CMD_STATE_RELEASED);
        s_avrc_tl = (uint8_t)((s_avrc_tl + 1U) & 0x0F);
        break;
    case BT_SVC_CMD_DISCOVERY_RESULT:
        bt_svc_store_discovery_result(&msg->data.discovery_result);
        break;
    case BT_SVC_CMD_DISCOVERY_STATE:
        s_discovery_running = (msg->data.discovery_state == ESP_BT_GAP_DISCOVERY_STARTED);
        if (msg->data.discovery_state == ESP_BT_GAP_DISCOVERY_STOPPED && s_pair_request_pending)
        {
            s_pair_request_pending = false;
            bt_svc_select_and_connect_best_sink();
        }
        break;
    case BT_SVC_CMD_AUTH_COMPLETE:
        bt_svc_clear_pending_pairing_confirm();
        if (msg->data.auth.status == ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGI(TAG,
                     "pairing complete with " ESP_BD_ADDR_STR " (%s)",
                     ESP_BD_ADDR_HEX(msg->data.auth.bda),
                     msg->data.auth.name[0] ? msg->data.auth.name : "unknown");
            bt_svc_post_event(BLUETOOTH_SVC_EVENT_PAIRING_COMPLETE,
                              msg->data.auth.bda,
                              ESP_BD_ADDR_LEN);
            bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_AUTH_COMPLETE,
                                              msg->data.auth.bda);
        }
        else
        {
            ESP_LOGW(TAG,
                     "pairing failed with " ESP_BD_ADDR_STR " status=%d",
                     ESP_BD_ADDR_HEX(msg->data.auth.bda),
                     msg->data.auth.status);
        }
        break;
    case BT_SVC_CMD_A2DP_CONNECTION_STATE:
        if (msg->data.a2dp_connection.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            s_a2dp_connected = true;
            memcpy(s_connected_bda, msg->data.a2dp_connection.remote_bda, ESP_BD_ADDR_LEN);
            // Todo: make this an option
            // disable wifi autoreconnect to avoid audio glitches
            // wifi_service_set_autoreconnect(false);
        }
        else if (msg->data.a2dp_connection.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            s_a2dp_connected = false;
            memset(s_connected_bda, 0, sizeof(s_connected_bda));
            // re-enable wifi autoreconnect
            // wifi_service_set_autoreconnect(true);
        }
        bt_svc_post_event(BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE,
                          &msg->data.a2dp_connection.state,
                          sizeof(msg->data.a2dp_connection.state));
        bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_A2DP_CONNECTION_STATE,
                                          msg->data.a2dp_connection.remote_bda);
        break;
    case BT_SVC_CMD_A2DP_AUDIO_STATE:
        bt_svc_post_event(BLUETOOTH_SVC_EVENT_A2DP_AUDIO_STATE,
                          &msg->data.a2dp_audio.state,
                          sizeof(msg->data.a2dp_audio.state));
        bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_A2DP_AUDIO_STATE,
                                          msg->data.a2dp_audio.remote_bda);
        break;
    case BT_SVC_CMD_AVRCP_PASSTHROUGH_RSP:
        if (s_media_key_cb)
        {
            s_media_key_cb(msg->data.avrc_rsp.key_code,
                           msg->data.avrc_rsp.key_state,
                           msg->data.avrc_rsp.rsp_code,
                           s_media_key_cb_ctx);
        }
        break;
    case BT_SVC_CMD_AVRCP_PASSTHROUGH_CMD:
    {
        if (msg->data.avrc_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED)
        {
            bool handled = false;
            player_service_control_t control = bt_svc_player_control_from_key(msg->data.avrc_cmd.key_code, &handled);
            if (handled)
            {
                esp_err_t err = player_service_request_control(control);
                if (err != ESP_OK)
                {
                    ESP_LOGW(TAG,
                             "player media control failed for key %u: %s",
                             (unsigned)msg->data.avrc_cmd.key_code,
                             esp_err_to_name(err));
                }
            }
            else
            {
                ESP_LOGI(TAG, "ignoring unsupported AVRCP target key %u", (unsigned)msg->data.avrc_cmd.key_code);
            }
        }
        break;
    }
    case BT_SVC_CMD_AVRCP_SET_ABS_VOL:
    {
        uint8_t avrc_vol = msg->data.abs_vol.volume;
        ESP_LOGI(TAG, "AVRCP absolute volume set: %u/127", (unsigned)avrc_vol);
        player_service_set_volume_absolute(avrc_vol);
        if (s_avrc_rn_vol_registered)
        {
            esp_avrc_rn_param_t rn = {0};
            rn.volume = avrc_vol;
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                                    ESP_AVRC_RN_RSP_CHANGED, &rn);
            s_avrc_rn_vol_registered = false;
        }
        break;
    }
    case BT_SVC_CMD_AVRCP_CT_RN_CAPS:
    {
        esp_avrc_rn_evt_cap_mask_t rn_caps = msg->data.ct_rn_caps;
        if (esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST,
                                               &rn_caps,
                                               ESP_AVRC_RN_VOLUME_CHANGE))
        {
            ESP_LOGI(TAG, "remote TG supports VOLUME_CHANGE, registering for notifications");
            s_avrc_tl = (s_avrc_tl + 1) & 0x0F;
            esp_avrc_ct_send_register_notification_cmd(s_avrc_tl,
                                                       ESP_AVRC_RN_VOLUME_CHANGE,
                                                       0);
            s_avrc_ct_vol_registered = true;
        }
        else
        {
            ESP_LOGI(TAG, "remote TG does not support VOLUME_CHANGE notifications");
        }
        break;
    }
    case BT_SVC_CMD_AVRCP_CT_VOL_CHANGE:
    {
        uint8_t remote_vol = msg->data.ct_vol_change.volume;
        ESP_LOGI(TAG, "remote TG volume change: %u/127", (unsigned)remote_vol);
        player_service_set_volume_absolute(remote_vol);
        if (s_avrc_ct_vol_registered)
        {
            s_avrc_tl = (s_avrc_tl + 1) & 0x0F;
            esp_avrc_ct_send_register_notification_cmd(s_avrc_tl,
                                                       ESP_AVRC_RN_VOLUME_CHANGE,
                                                       0);
        }
        break;
    }
    case BT_SVC_CMD_AVRCP_REG_VOL_NOTIFY:
        if (msg->data.reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE)
        {
            s_avrc_rn_vol_registered = true;
            uint8_t vol_pct = player_service_get_volume_percent();
            esp_avrc_rn_param_t rn = {0};
            rn.volume = (uint8_t)((uint32_t)vol_pct * 127U / 100U);
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                                    ESP_AVRC_RN_RSP_INTERIM, &rn);
            ESP_LOGI(TAG, "AVRCP volume change notification registered, interim vol=%u",
                     (unsigned)rn.volume);
        }
        break;
    case BT_SVC_CMD_SPP_CONNECTED:
        bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_SPP_CONNECTED,
                                          msg->data.spp_remote.remote_bda);
        break;
    case BT_SVC_CMD_SPP_DISCONNECTED:
        bt_svc_invoke_connection_callback(BLUETOOTH_SVC_CONNECTION_EVENT_SPP_DISCONNECTED,
                                          msg->data.spp_remote.remote_bda);
        break;
    default:
        break;
    }
}

void bluetooth_service_process_once(void)
{
    bluetooth_service_msg_t msg;

    if (!s_initialised || s_cmd_queue == NULL)
    {
        return;
    }

    for (size_t index = 0; index < BT_SVC_MAX_CMDS_PER_PASS; index++)
    {
        if (xQueueReceive(s_cmd_queue, &msg, 0) != pdPASS)
        {
            break;
        }

        bt_svc_process_msg(&msg);
    }
}

esp_err_t bluetooth_service_init(void)
{
    esp_err_t err;
    esp_bt_pin_code_t pin_code = {'1', '2', '3', '4'};

    if (s_initialised)
    {
        return ESP_OK;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    s_cmd_queue = xQueueCreate(BT_SVC_QUEUE_DEPTH, sizeof(bluetooth_service_msg_t));
    if (!s_cmd_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(bt_svc_gap_bt_cb));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(bt_svc_gap_ble_cb));
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(bt_svc_spp_gatts_cb));

    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(BT_SVC_NAME_CLASSIC));
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
    ESP_ERROR_CHECK(esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED, 4, pin_code));

    ESP_ERROR_CHECK(esp_avrc_tg_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(bt_svc_avrc_tg_cb));
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    ESP_ERROR_CHECK(esp_avrc_ct_register_callback(bt_svc_avrc_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(bt_svc_a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(bt_svc_a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    esp_avrc_psth_bit_mask_t cmd_mask = {0};
    esp_avrc_rn_evt_cap_mask_t evt_mask = {0};
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_PLAY);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_PAUSE);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_STOP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_BACKWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_FAST_FORWARD);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_REWIND);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_VOL_UP);
    esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_mask, ESP_AVRC_PT_CMD_VOL_DOWN);
    ESP_ERROR_CHECK(esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_mask));
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_mask, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_mask));

    ESP_ERROR_CHECK(esp_ble_gatts_app_register(BT_SVC_BLE_SPP_APP_ID));
    ESP_ERROR_CHECK(esp_ble_gatt_set_local_mtu(BT_SVC_BLE_SPP_MTU_SIZE));

    s_initialised = true;
    ESP_ERROR_CHECK(power_mgmt_service_register_shutdown_callback(bluetooth_service_shutdown_callback,
                                                                  NULL,
                                                                  POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_BLUETOOTH));
    bt_svc_post_event(BLUETOOTH_SVC_EVENT_STARTED, NULL, 0);
    ESP_LOGI(TAG, "Bluetooth service started");

    err = bluetooth_service_connect_last_bonded_a2dp_device();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "boot-time Bluetooth reconnect queue failed: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t bluetooth_service_pair_best_a2dp_sink(void)
{
    bluetooth_service_msg_t msg = {.cmd = BT_SVC_CMD_PAIR_BEST};
    esp_err_t err = bt_svc_validate_a2dp_state(false, "pair request");
    if (err != ESP_OK)
    {
        return err;
    }
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_connect_last_bonded_a2dp_device(void)
{
    bluetooth_service_msg_t msg = {.cmd = BT_SVC_CMD_CONNECT_LAST_BONDED};
    esp_err_t err = bt_svc_validate_a2dp_state(false, "connect request");
    if (err != ESP_OK)
    {
        return err;
    }
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_get_pending_pairing_confirm(bluetooth_service_pairing_confirm_t *confirm)
{
    if (!confirm)
    {
        return ESP_ERR_INVALID_ARG;
    }

    bt_svc_get_pending_pairing_confirm(confirm);
    return ESP_OK;
}

esp_err_t bluetooth_service_reply_pairing_confirm(bool accept)
{
    bluetooth_service_msg_t msg = {
        .cmd = BT_SVC_CMD_PAIRING_CONFIRM_REPLY,
    };

    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }

    msg.data.pairing_confirm.accept = accept;
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_disconnect_a2dp(void)
{
    bluetooth_service_msg_t msg = {.cmd = BT_SVC_CMD_DISCONNECT_A2DP};
    esp_err_t err = bt_svc_validate_a2dp_state(true, "disconnect request");
    if (err != ESP_OK)
    {
        return err;
    }
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_start_audio(void)
{
    bluetooth_service_msg_t msg = {.cmd = BT_SVC_CMD_START_AUDIO};
    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_suspend_audio(void)
{
    bluetooth_service_msg_t msg = {.cmd = BT_SVC_CMD_SUSPEND_AUDIO};
    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_send_media_key(uint8_t key_code)
{
    bluetooth_service_msg_t msg = {
        .cmd = BT_SVC_CMD_SEND_MEDIA_KEY,
        .data.media_key = key_code,
    };
    if (!s_cmd_queue)
    {
        return ESP_ERR_INVALID_STATE;
    }
    return xQueueSend(s_cmd_queue, &msg, bt_svc_queue_timeout_ticks()) == pdPASS ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t bluetooth_service_register_pcm_provider(bluetooth_service_pcm_provider_t provider, void *user_ctx)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_pcm_provider = provider;
    s_pcm_provider_ctx = user_ctx;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

void bluetooth_service_register_connection_callback(bluetooth_service_connection_cb_t callback, void *user_ctx)
{
    s_connection_cb = callback;
    s_connection_cb_ctx = user_ctx;
}

void bluetooth_service_register_media_key_callback(bluetooth_service_media_key_cb_t callback, void *user_ctx)
{
    s_media_key_cb = callback;
    s_media_key_cb_ctx = user_ctx;
}

bool bluetooth_service_is_initialised(void)
{
    return s_initialised;
}

bool bluetooth_service_is_a2dp_connected(void)
{
    return s_a2dp_connected;
}

size_t bluetooth_service_get_bonded_device_count(void)
{
    int count = esp_bt_gap_get_bond_device_num();
    return count > 0 ? (size_t)count : 0;
}

esp_err_t bluetooth_service_get_bonded_devices(size_t *count, esp_bd_addr_t *devices)
{
    int device_count;

    if (!count || !devices)
    {
        return ESP_ERR_INVALID_ARG;
    }

    device_count = (int)(*count);
    if (device_count <= 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_bt_gap_get_bond_device_list(&device_count, devices);
    *count = device_count > 0 ? (size_t)device_count : 0;
    return err;
}

esp_err_t bluetooth_service_shutdown(void)
{
    if (!s_initialised)
    {
        return ESP_OK;
    }

    if (s_discovery_running)
    {
        esp_err_t err = bt_svc_ignore_invalid_state(esp_bt_gap_cancel_discovery());
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (s_a2dp_connected)
    {
        esp_err_t err = bt_svc_ignore_invalid_state(esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND));
        if (err != ESP_OK)
        {
            return err;
        }

        err = bt_svc_ignore_invalid_state(esp_a2d_source_disconnect(s_connected_bda));
        if (err != ESP_OK)
        {
            return err;
        }

        err = bt_svc_wait_for_disconnect_state(&s_a2dp_connected, false, pdMS_TO_TICKS(2000));
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (s_spp_connected)
    {
        esp_err_t err = bt_svc_ignore_invalid_state(esp_ble_gap_disconnect(s_spp_remote_bda));
        if (err != ESP_OK)
        {
            return err;
        }

        err = bt_svc_wait_for_disconnect_state(&s_spp_connected, false, pdMS_TO_TICKS(2000));
        if (err != ESP_OK)
        {
            return err;
        }
    }

    esp_err_t err = bt_svc_ignore_invalid_state(esp_ble_gap_stop_advertising());
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(bt_svc_shutdown_poll_ticks());

    if (s_spp_gatts_if != ESP_GATT_IF_NONE)
    {
        err = bt_svc_ignore_invalid_state(esp_ble_gatts_app_unregister(s_spp_gatts_if));
        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = bt_svc_ignore_invalid_state(esp_avrc_ct_deinit());
    if (err != ESP_OK)
    {
        return err;
    }

    err = bt_svc_ignore_invalid_state(esp_avrc_tg_deinit());
    if (err != ESP_OK)
    {
        return err;
    }

    err = bt_svc_ignore_invalid_state(esp_a2d_source_deinit());
    if (err != ESP_OK)
    {
        return err;
    }

    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
    {
        err = esp_bluedroid_disable();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED)
    {
        err = esp_bluedroid_deinit();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        err = esp_bt_controller_disable();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
    {
        err = esp_bt_controller_deinit();
        if (err != ESP_OK)
        {
            return err;
        }
    }

    if (s_cmd_queue != NULL)
    {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }

    s_initialised = false;
    s_pair_request_pending = false;
    s_discovery_running = false;
    s_a2dp_connected = false;
    memset(s_connected_bda, 0, sizeof(s_connected_bda));
    s_avrc_tl = 0;
    s_avrc_rn_vol_registered = false;
    s_avrc_ct_vol_registered = false;
    bt_svc_spp_reset_link_state();
    s_spp_gatts_if = ESP_GATT_IF_NONE;
    memset(s_spp_handle_table, 0, sizeof(s_spp_handle_table));
    memset(s_discovered, 0, sizeof(s_discovered));
    s_discovered_count = 0;

    return ESP_OK;
}

esp_err_t bluetooth_service_register_48k_sbc_endpoint(void)
{
    esp_a2d_mcc_t mcc = {0};
    mcc.type = ESP_A2D_MCT_SBC;
    mcc.cie.sbc_info.samp_freq = ESP_A2D_SBC_CIE_SF_48K;
    mcc.cie.sbc_info.ch_mode = ESP_A2D_SBC_CIE_CH_MODE_STEREO;
    mcc.cie.sbc_info.block_len = ESP_A2D_SBC_CIE_BLOCK_LEN_16;
    mcc.cie.sbc_info.num_subbands = ESP_A2D_SBC_CIE_NUM_SUBBANDS_8;
    mcc.cie.sbc_info.alloc_mthd = ESP_A2D_SBC_CIE_ALLOC_MTHD_LOUDNESS;
    mcc.cie.sbc_info.min_bitpool = 2;
    mcc.cie.sbc_info.max_bitpool = 53;

    esp_err_t err = esp_a2d_source_register_stream_endpoint(0, &mcc);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to register 48k SBC endpoint: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "registered 48 kHz SBC stream endpoint");
    }
    return err;
}
