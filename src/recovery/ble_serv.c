#ifdef BUILD_FACTORY_APP

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "zlib.h"
#include "mbedtls/sha256.h"
#include "macros.h"
#include "part_mgr.h"

#include "ble_serv.h"

#define TAG "BLE_SERV"

// --- UUID 定义 (必须与 Python 客户端匹配) ---
// OTA_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define OTA_SERVICE_UUID_VAL 0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f, 0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f
// DATA_CHAR_UUID "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define DATA_CHAR_UUID_VAL 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
// CONTROL_CHAR_UUID "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
#define CONTROL_CHAR_UUID_VAL 0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e

// --- 控制命令/通知定义 ---
#define CMD_FLASH_PART 0x00 // (was CMD_START_OTA)
#define CMD_SEND_HASH 0x01
#define CMD_ERASE_PART 0x02
#define CMD_VERIFY_PART 0x03
#define CMD_REBOOT 0x04

#define NOTIFY_CMD_OK 0x10 // (was NOTIFY_OTA_OK)
#define NOTIFY_HASH_OK 0x11
#define NOTIFY_ERR_HASH 0x81
#define NOTIFY_ERR_WRITE 0x82
#define NOTIFY_ERR_ZLIB 0x83
#define NOTIFY_ERR_CMD 0x84
#define NOTIFY_ERR_PARTITION 0x85

// OTA 状态管理
static bool ota_in_progress = false;
static uint32_t ota_offset = 0;
static uint32_t total_decompressed_bytes = 0;
static uint16_t conn_handle_global = BLE_HS_CONN_HANDLE_NONE;
static uint16_t control_attr_handle;
static uint8_t remote_sha_hash[32];
static esp_partition_t *ota_partition = NULL;

// ZLIB 流式解压相关
#define ZLIB_CHUNK_SIZE 1024
static z_stream d_stream;
static uint8_t zlib_in_buf[ZLIB_CHUNK_SIZE];
static uint8_t zlib_out_buf[ZLIB_CHUNK_SIZE * 4];

SemaphoreHandle_t cmd_done_sem;

#define BLE_DEVICE_NAME "tj_needs_your_help"

// 前向
static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg);

static int ble_gap_event(struct ble_gap_event *event, void *arg);
static void ble_app_advertise(void);

// --- GATT 服务定义 ---
static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(OTA_SERVICE_UUID_VAL),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                // 数据特征: 用于接收固件数据
                .uuid = BLE_UUID128_DECLARE(DATA_CHAR_UUID_VAL),
                .flags = BLE_GATT_CHR_F_WRITE_NO_RSP,
                .access_cb = gatt_svr_chr_access,
                .arg = (void *)1,
            },
            {
                // 控制特征: 用于启动/停止 OTA 和发送/接收状态
                .uuid = BLE_UUID128_DECLARE(CONTROL_CHAR_UUID_VAL),
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                .access_cb = gatt_svr_chr_access,
                .val_handle = &control_attr_handle,
                .arg = (void *)2,
            },
            {0} /* 特征列表结束 */
        },
    },
    {0} /* 服务列表结束 */
};

static void notify_client(uint8_t status_code)
{
    if (conn_handle_global != BLE_HS_CONN_HANDLE_NONE)
    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&status_code, sizeof(status_code));
        ble_gattc_notify_custom(conn_handle_global, control_attr_handle, om);
        ESP_LOGI(TAG, "Notifying client with status: 0x%02X", status_code);
    }
}

static int handle_control_write(struct ble_gatt_access_ctxt *ctxt)
{
    if (ctxt->om->om_len < 1)
    {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint8_t command = ctxt->om->om_data[0];
    part_mgr_partition_type_t part_type;
    esp_err_t err;

    // For commands that require a partition type
    if (command == CMD_FLASH_PART || command == CMD_ERASE_PART || command == CMD_VERIFY_PART)
    {
        if (ctxt->om->om_len < 2)
        {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        part_type = (part_mgr_partition_type_t)ctxt->om->om_data[1];
        if (part_type != PART_MGR_PARTITION_TYPE_APP && part_type != PART_MGR_PARTITION_TYPE_LITTLEFS)
        {
            ESP_LOGE(TAG, "Invalid or unsupported partition type for command %d: %d", command, part_type);
            notify_client(NOTIFY_ERR_PARTITION);
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }
        err = part_mgr_get_partition(part_type, &ota_partition);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to get partition type %d", part_type);
            notify_client(NOTIFY_ERR_PARTITION);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }

    switch (command)
    {
    case CMD_FLASH_PART:
        ESP_LOGI(TAG, "CMD_FLASH_PART received for partition type %d.", part_type);
        ota_in_progress = false; // 先复位
        if (part_mgr_erase_partition(ota_partition) != ESP_OK)
        {
            ESP_LOGE(TAG, "Flash erase failed!");
            notify_client(NOTIFY_ERR_WRITE);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        // 初始化 ZLIB
        d_stream.zalloc = Z_NULL;
        d_stream.zfree = Z_NULL;
        d_stream.opaque = Z_NULL;
        if (inflateInit(&d_stream) != Z_OK)
        {
            ESP_LOGE(TAG, "zlib inflateInit failed!");
            notify_client(NOTIFY_ERR_ZLIB);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        ota_offset = 0;
        total_decompressed_bytes = 0;
        ota_in_progress = true;
        ESP_LOGI(TAG, "Flash process started. Ready to receive data for partition %s.", ota_partition->label);
        notify_client(NOTIFY_CMD_OK);
        break;

    case CMD_SEND_HASH:
        if (ctxt->om->om_len != 33)
        { // 1 byte command + 32 bytes hash
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        ESP_LOGI(TAG, "CMD_SEND_HASH received. All data has been transferred.");
        ota_in_progress = false;
        inflateEnd(&d_stream); // 清理 zlib
        memcpy(remote_sha_hash, &ctxt->om->om_data[1], 32);

        ESP_LOGI(TAG, "Verifying partition %s...", ota_partition->label);
        err = part_mgr_verify_partition(ota_partition, remote_sha_hash, total_decompressed_bytes);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Verification SUCCESS!");
            notify_client(NOTIFY_HASH_OK);
        }
        else
        {
            ESP_LOGE(TAG, "Verification FAILED! Hash mismatch or read error.");
            notify_client(NOTIFY_ERR_HASH);
        }
        break;

    case CMD_ERASE_PART:
        ESP_LOGI(TAG, "CMD_ERASE_PART received for partition %s.", ota_partition->label);
        err = part_mgr_erase_partition(ota_partition);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Partition erased successfully.");
            notify_client(NOTIFY_CMD_OK);
        }
        else
        {
            ESP_LOGE(TAG, "Partition erase failed.");
            notify_client(NOTIFY_ERR_WRITE);
        }
        break;

    case CMD_VERIFY_PART:
    {
        // 1 (cmd) + 1 (part_type) + 32 (hash) + 4 (size) = 38 bytes
        if (ctxt->om->om_len != 38)
        {
            ESP_LOGE(TAG, "CMD_VERIFY_PART: Invalid length %d, expected 38", ctxt->om->om_len);
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        uint32_t size;
        // Payload: [cmd:1][part_type:1][hash:32][size:4]
        memcpy(remote_sha_hash, &ctxt->om->om_data[2], 32);
        memcpy(&size, &ctxt->om->om_data[34], sizeof(size));

        ESP_LOGI(TAG, "CMD_VERIFY_PART received for partition %s, size %d.", ota_partition->label, size);
        err = part_mgr_verify_partition(ota_partition, remote_sha_hash, size);
        if (err == ESP_OK)
        {
            ESP_LOGI(TAG, "Verification SUCCESS!");
            notify_client(NOTIFY_HASH_OK);
        }
        else
        {
            ESP_LOGE(TAG, "Verification FAILED!");
            notify_client(NOTIFY_ERR_HASH);
        }
        break;
    }

    case CMD_REBOOT:
        ESP_LOGI(TAG, "CMD_REBOOT received. Rebooting...");
        notify_client(NOTIFY_CMD_OK);
        vTaskDelay(pdMS_TO_TICKS(1000)); // Give time for notification to send
        esp_restart();
        break;

    default:
        ESP_LOGW(TAG, "Unknown command: 0x%02X", command);
        notify_client(NOTIFY_ERR_CMD);
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    return 0;
}

static int handle_data_write(struct ble_gatt_access_ctxt *ctxt)
{
    if (!ota_in_progress)
    {
        ESP_LOGW(TAG, "Received data but OTA not in progress. Ignoring.");
        return 0; // 静默忽略
    }

    int ret;
    d_stream.avail_in = ctxt->om->om_len;
    d_stream.next_in = ctxt->om->om_data;

    do
    {
        d_stream.avail_out = sizeof(zlib_out_buf);
        d_stream.next_out = zlib_out_buf;
        ret = inflate(&d_stream, Z_NO_FLUSH);

        if (ret < 0 && ret != Z_BUF_ERROR)
        {
            ESP_LOGE(TAG, "ZLIB decompression error: %d", ret);
            notify_client(NOTIFY_ERR_ZLIB);
            ota_in_progress = false;
            inflateEnd(&d_stream);
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        size_t have = sizeof(zlib_out_buf) - d_stream.avail_out;
        if (have > 0)
        {
            if (part_mgr_write_partition(ota_partition, zlib_out_buf, have, ota_offset) != ESP_OK)
            {
                ESP_LOGE(TAG, "Flash write failed at offset %u", ota_offset);
                notify_client(NOTIFY_ERR_WRITE);
                ota_in_progress = false;
                inflateEnd(&d_stream);
                return BLE_ATT_ERR_INSUFFICIENT_RES;
            }
            ota_offset += have;
            total_decompressed_bytes += have;
        }
    } while (d_stream.avail_out == 0);

    return 0;
}

static int gatt_svr_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;
    int char_id = (int)arg;

    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        if (char_id == 1)
        { // Data Characteristic
            rc = handle_data_write(ctxt);
        }
        else if (char_id == 2)
        { // Control Characteristic
            rc = handle_control_write(ctxt);
        }
        else
        {
            rc = BLE_ATT_ERR_UNLIKELY;
        }
        return rc;

    // 其他操作可以加在这里，如读操作
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static void ble_app_advertise(void)
{
    struct ble_hs_adv_fields adv_fields;
    struct ble_hs_adv_fields rsp_fields;
    const char *name;
    int rc;

    // Clear the advertisement and scan response fields
    memset(&adv_fields, 0, sizeof(adv_fields));
    memset(&rsp_fields, 0, sizeof(rsp_fields));

    // Set advertising data - include flags and UUID
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    // Only include the service UUID in the advertisement
    adv_fields.uuids128 = (ble_uuid128_t[]){
        BLE_UUID128_INIT(0x4b, 0x91, 0x31, 0xc3, 0xc9, 0xc5, 0xcc, 0x8f, 0x9e, 0x45, 0xb5, 0x1f, 0x01, 0xc2, 0xaf, 0x4f),
    };
    adv_fields.num_uuids128 = 1;
    adv_fields.uuids128_is_complete = 1;

    // Set the advertisement data
    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error setting advertisement data; rc=%d", rc);
        return;
    }

    // Set scan response data - include device name and TX power
    name = ble_svc_gap_device_name();
    rsp_fields.name = (uint8_t *)name;
    rsp_fields.name_len = strlen(name);
    rsp_fields.name_is_complete = 1;

    rsp_fields.tx_pwr_lvl_is_present = 1;
    rsp_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    // Set the scan response data
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error setting scan response data; rc=%d", rc);
        return;
    }

    /* Begin advertising. */
    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0)
    {
        ESP_LOGE(TAG, "Error enabling advertisement; rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started successfully");
}

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "Device connected; conn_handle=%d", event->connect.conn_handle);
        conn_handle_global = event->connect.conn_handle;
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Device disconnected");
        conn_handle_global = BLE_HS_CONN_HANDLE_NONE;
        ota_in_progress = false; // 连接断开，终止OTA
        // 重启广播
        ble_app_advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete");
        break;
    default:
        break;
    }
    return 0;
}

void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run(); // This function will return only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// Callback for BLE stack sync
void ble_sync_cb(void)
{
    ESP_LOGI(TAG, "BLE stack synchronized.");
    ble_app_advertise();
};

// Callback for BLE stack reset
void ble_reset_cb(int reason)
{
    ESP_LOGE(TAG, "BLE stack reset.");
};

void ble_serv_init(void)
{
    // Get the OTA partition
    if (part_mgr_get_partition(PART_MGR_PARTITION_TYPE_APP, &ota_partition) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get OTA partition");
        return;
    }
    ESP_LOGI(TAG, "OTA partition found: %s", ota_partition->label);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svr_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svr_svcs));

    ble_hs_cfg.sync_cb = ble_sync_cb;
    ble_hs_cfg.reset_cb = ble_reset_cb;

    nimble_port_freertos_init(ble_host_task);
}

#endif // BUILD_FACTORY_APP