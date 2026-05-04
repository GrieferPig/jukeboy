#include "companion_api_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "mbedtls/md.h"

#include "audio_output_switch.h"
#include "bluetooth_service.h"
#include "cartridge_service.h"
#include "lastfm_service.h"
#include "play_history_service.h"
#include "player_service.h"
#include "wifi_service.h"

#define COMPANION_API_TASK_NAME "companion_api"
#define COMPANION_API_TASK_STACK_WORDS 8192
#define COMPANION_API_TASK_PRIORITY 4
#define COMPANION_API_TASK_CORE 1
#define COMPANION_API_QUEUE_DEPTH 16
#define COMPANION_API_SPP_CHUNK_MAX_LEN 512
#define COMPANION_API_FRAME_MAX_LEN 2048
#define COMPANION_API_HEADER_LEN 12
#define COMPANION_API_TLV_HEADER_LEN 4
#define COMPANION_API_HEARTBEAT_MS 5000
#define COMPANION_API_PAIRING_TIMEOUT_MS 120000
#define COMPANION_API_SECRET_LEN 32
#define COMPANION_API_AUTH_NONCE_LEN 16
#define COMPANION_API_HMAC_LEN 32
#define COMPANION_API_NVS_NAMESPACE "companion_api"
#define COMPANION_API_NVS_RECORD_VERSION 1U
#define COMPANION_API_PSRAM_ALLOC_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define COMPANION_API_STACK_ALLOC_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

#define COMPANION_API_MAGIC0 ((uint8_t)'J')
#define COMPANION_API_MAGIC1 ((uint8_t)'C')
#define COMPANION_API_VERSION 1U

static const char *TAG = "companion_api";

typedef enum
{
    COMPANION_API_FRAME_REQUEST = 1,
    COMPANION_API_FRAME_RESPONSE = 2,
    COMPANION_API_FRAME_EVENT = 3,
    COMPANION_API_FRAME_HEARTBEAT = 4,
    COMPANION_API_FRAME_ERROR = 5,
} companion_api_frame_type_t;

typedef enum
{
    COMPANION_API_OP_HELLO = 0x0001,
    COMPANION_API_OP_CAPABILITIES = 0x0002,
    COMPANION_API_OP_PING = 0x0003,
    COMPANION_API_OP_PAIR_BEGIN = 0x0010,
    COMPANION_API_OP_PAIR_STATUS = 0x0011,
    COMPANION_API_OP_PAIR_CANCEL = 0x0012,
    COMPANION_API_OP_AUTH_CHALLENGE = 0x0013,
    COMPANION_API_OP_AUTH_PROOF = 0x0014,
    COMPANION_API_OP_TRUSTED_LIST = 0x0015,
    COMPANION_API_OP_TRUSTED_REVOKE = 0x0016,
    COMPANION_API_OP_SNAPSHOT = 0x0100,
    COMPANION_API_OP_PLAYBACK_STATUS = 0x0101,
    COMPANION_API_OP_PLAYBACK_CONTROL = 0x0102,
    COMPANION_API_OP_LIBRARY_ALBUM = 0x0110,
    COMPANION_API_OP_LIBRARY_TRACK_PAGE = 0x0111,
    COMPANION_API_OP_WIFI_STATUS = 0x0200,
    COMPANION_API_OP_WIFI_SCAN_START = 0x0201,
    COMPANION_API_OP_WIFI_SCAN_RESULTS = 0x0202,
    COMPANION_API_OP_WIFI_CONNECT = 0x0203,
    COMPANION_API_OP_WIFI_CONNECT_SLOT = 0x0204,
    COMPANION_API_OP_WIFI_DISCONNECT = 0x0205,
    COMPANION_API_OP_WIFI_AUTORECONNECT = 0x0206,
    COMPANION_API_OP_LASTFM_STATUS = 0x0300,
    COMPANION_API_OP_LASTFM_CONTROL = 0x0301,
    COMPANION_API_OP_HISTORY_SUMMARY = 0x0400,
    COMPANION_API_OP_HISTORY_ALBUM_PAGE = 0x0401,
    COMPANION_API_OP_BT_AUDIO_STATUS = 0x0500,
    COMPANION_API_OP_BT_AUDIO_CONTROL = 0x0501,
} companion_api_opcode_t;

typedef enum
{
    COMPANION_API_ERR_OK = 0,
    COMPANION_API_ERR_BAD_FRAME = 1,
    COMPANION_API_ERR_UNSUPPORTED_VERSION = 2,
    COMPANION_API_ERR_UNKNOWN_OPCODE = 3,
    COMPANION_API_ERR_AUTH_REQUIRED = 4,
    COMPANION_API_ERR_INVALID_ARG = 5,
    COMPANION_API_ERR_INVALID_STATE = 6,
    COMPANION_API_ERR_NO_MEM = 7,
    COMPANION_API_ERR_TIMEOUT = 8,
    COMPANION_API_ERR_NOT_FOUND = 9,
    COMPANION_API_ERR_INTERNAL = 10,
} companion_api_error_t;

typedef enum
{
    COMPANION_API_TLV_STATUS = 0x0001,
    COMPANION_API_TLV_ERROR_CODE = 0x0002,
    COMPANION_API_TLV_ERROR_MESSAGE = 0x0003,
    COMPANION_API_TLV_PROTOCOL_VERSION = 0x0004,
    COMPANION_API_TLV_FEATURE_BITS = 0x0005,
    COMPANION_API_TLV_MAX_FRAME = 0x0006,
    COMPANION_API_TLV_MTU = 0x0007,
    COMPANION_API_TLV_MAX_PAYLOAD = 0x0008,
    COMPANION_API_TLV_REQUEST_ID = 0x0009,

    COMPANION_API_TLV_AUTHENTICATED = 0x0100,
    COMPANION_API_TLV_CLIENT_ID = 0x0101,
    COMPANION_API_TLV_APP_NAME = 0x0102,
    COMPANION_API_TLV_SHARED_SECRET = 0x0103,
    COMPANION_API_TLV_BUTTON_SEQUENCE = 0x0104,
    COMPANION_API_TLV_PAIRING_PENDING = 0x0105,
    COMPANION_API_TLV_PAIRING_PROGRESS = 0x0106,
    COMPANION_API_TLV_PAIRING_REQUIRED = 0x0107,
    COMPANION_API_TLV_AUTH_NONCE = 0x0108,
    COMPANION_API_TLV_AUTH_HMAC = 0x0109,
    COMPANION_API_TLV_TRUSTED_COUNT = 0x010A,
    COMPANION_API_TLV_CREATED_AT = 0x010B,

    COMPANION_API_TLV_GENERATION = 0x0200,
    COMPANION_API_TLV_UPTIME_MS = 0x0201,
    COMPANION_API_TLV_QUEUE_FREE = 0x0202,
    COMPANION_API_TLV_RX_FRAMES = 0x0203,
    COMPANION_API_TLV_TX_FRAMES = 0x0204,
    COMPANION_API_TLV_RX_ERRORS = 0x0205,

    COMPANION_API_TLV_PLAYING = 0x0300,
    COMPANION_API_TLV_PAUSED = 0x0301,
    COMPANION_API_TLV_TRACK_INDEX = 0x0302,
    COMPANION_API_TLV_TRACK_COUNT = 0x0303,
    COMPANION_API_TLV_POSITION_SEC = 0x0304,
    COMPANION_API_TLV_STARTED_AT = 0x0305,
    COMPANION_API_TLV_DURATION_SEC = 0x0306,
    COMPANION_API_TLV_VOLUME_PERCENT = 0x0307,
    COMPANION_API_TLV_PLAYBACK_MODE = 0x0308,
    COMPANION_API_TLV_TRACK_TITLE = 0x0309,
    COMPANION_API_TLV_TRACK_ARTIST = 0x030A,
    COMPANION_API_TLV_TRACK_FILE = 0x030B,
    COMPANION_API_TLV_ACTION = 0x030C,
    COMPANION_API_TLV_VALUE = 0x030D,
    COMPANION_API_TLV_OUTPUT_TARGET = 0x030E,

    COMPANION_API_TLV_CARTRIDGE_STATUS = 0x0400,
    COMPANION_API_TLV_CARTRIDGE_MOUNTED = 0x0401,
    COMPANION_API_TLV_CARTRIDGE_CHECKSUM = 0x0402,
    COMPANION_API_TLV_METADATA_VERSION = 0x0403,
    COMPANION_API_TLV_ALBUM_NAME = 0x0404,
    COMPANION_API_TLV_ALBUM_ARTIST = 0x0405,
    COMPANION_API_TLV_ALBUM_DESCRIPTION = 0x0406,
    COMPANION_API_TLV_ALBUM_YEAR = 0x0407,
    COMPANION_API_TLV_ALBUM_DURATION = 0x0408,
    COMPANION_API_TLV_ALBUM_GENRE = 0x0409,
    COMPANION_API_TLV_OFFSET = 0x040A,
    COMPANION_API_TLV_COUNT = 0x040B,
    COMPANION_API_TLV_RETURNED_COUNT = 0x040C,

    COMPANION_API_TLV_WIFI_STATE = 0x0500,
    COMPANION_API_TLV_WIFI_INTERNET = 0x0501,
    COMPANION_API_TLV_WIFI_AUTORECONNECT = 0x0502,
    COMPANION_API_TLV_WIFI_ACTIVE_SLOT = 0x0503,
    COMPANION_API_TLV_WIFI_PREFERRED_SLOT = 0x0504,
    COMPANION_API_TLV_WIFI_IP = 0x0505,
    COMPANION_API_TLV_WIFI_SSID = 0x0506,
    COMPANION_API_TLV_WIFI_PASSWORD = 0x0507,
    COMPANION_API_TLV_WIFI_SLOT = 0x0508,
    COMPANION_API_TLV_WIFI_RSSI = 0x0509,
    COMPANION_API_TLV_WIFI_CHANNEL = 0x050A,
    COMPANION_API_TLV_WIFI_AUTHMODE = 0x050B,

    COMPANION_API_TLV_LASTFM_HAS_AUTH_URL = 0x0600,
    COMPANION_API_TLV_LASTFM_HAS_TOKEN = 0x0601,
    COMPANION_API_TLV_LASTFM_HAS_SESSION = 0x0602,
    COMPANION_API_TLV_LASTFM_BUSY = 0x0603,
    COMPANION_API_TLV_LASTFM_SCROBBLING = 0x0604,
    COMPANION_API_TLV_LASTFM_NOW_PLAYING = 0x0605,
    COMPANION_API_TLV_LASTFM_PENDING_COMMANDS = 0x0606,
    COMPANION_API_TLV_LASTFM_PENDING_SCROBBLES = 0x0607,
    COMPANION_API_TLV_LASTFM_SUCCESSFUL = 0x0608,
    COMPANION_API_TLV_LASTFM_FAILED = 0x0609,
    COMPANION_API_TLV_LASTFM_AUTH_URL = 0x060A,
    COMPANION_API_TLV_LASTFM_USERNAME = 0x060B,

    COMPANION_API_TLV_HISTORY_ALBUM_COUNT = 0x0700,
    COMPANION_API_TLV_HISTORY_TRACK_COUNT = 0x0701,
    COMPANION_API_TLV_HISTORY_PLAY_COUNT = 0x0702,
    COMPANION_API_TLV_HISTORY_FIRST_SEEN = 0x0703,
    COMPANION_API_TLV_HISTORY_LAST_SEEN = 0x0704,

    COMPANION_API_TLV_BT_A2DP_CONNECTED = 0x0800,
    COMPANION_API_TLV_BT_BONDED_COUNT = 0x0801,
} companion_api_tlv_type_t;

typedef enum
{
    COMPANION_API_PLAYBACK_ACTION_NEXT = 1,
    COMPANION_API_PLAYBACK_ACTION_PREVIOUS = 2,
    COMPANION_API_PLAYBACK_ACTION_PAUSE_TOGGLE = 3,
    COMPANION_API_PLAYBACK_ACTION_FAST_FORWARD = 4,
    COMPANION_API_PLAYBACK_ACTION_REWIND = 5,
    COMPANION_API_PLAYBACK_ACTION_PLAY_INDEX = 6,
    COMPANION_API_PLAYBACK_ACTION_SEEK_SECONDS = 7,
    COMPANION_API_PLAYBACK_ACTION_SET_VOLUME_PERCENT = 8,
    COMPANION_API_PLAYBACK_ACTION_SET_MODE = 9,
    COMPANION_API_PLAYBACK_ACTION_SET_OUTPUT_TARGET = 10,
} companion_api_playback_action_t;

typedef enum
{
    COMPANION_API_LASTFM_ACTION_SET_AUTH_URL = 1,
    COMPANION_API_LASTFM_ACTION_REQUEST_TOKEN = 2,
    COMPANION_API_LASTFM_ACTION_AUTH = 3,
    COMPANION_API_LASTFM_ACTION_LOGOUT = 4,
    COMPANION_API_LASTFM_ACTION_SET_SCROBBLING = 5,
    COMPANION_API_LASTFM_ACTION_SET_NOW_PLAYING = 6,
} companion_api_lastfm_action_t;

typedef enum
{
    COMPANION_API_BT_ACTION_CONNECT_LAST = 1,
    COMPANION_API_BT_ACTION_PAIR_BEST = 2,
    COMPANION_API_BT_ACTION_DISCONNECT = 3,
} companion_api_bt_action_t;

typedef enum
{
    COMPANION_API_MSG_RX_CHUNK,
    COMPANION_API_MSG_HID_BUTTON,
    COMPANION_API_MSG_CONSOLE_CONFIRM_PAIRING,
    COMPANION_API_MSG_CONSOLE_CANCEL_PAIRING,
    COMPANION_API_MSG_CONSOLE_REVOKE_CLIENT,
    COMPANION_API_MSG_CONSOLE_REVOKE_ALL,
    COMPANION_API_MSG_LINK_CONNECTED,
    COMPANION_API_MSG_LINK_DISCONNECTED,
} companion_api_msg_type_t;

typedef struct
{
    companion_api_msg_type_t type;
    uint16_t len;
    union
    {
        uint8_t bytes[COMPANION_API_SPP_CHUNK_MAX_LEN];
        hid_button_t button;
        char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
    } data;
    SemaphoreHandle_t completion_semaphore;
    esp_err_t *result_out;
} companion_api_msg_t;

typedef struct
{
    uint32_t version;
    uint8_t valid;
    uint8_t reserved[3];
    char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
    char app_name[COMPANION_API_APP_NAME_MAX_LEN + 1];
    uint8_t secret[COMPANION_API_SECRET_LEN];
    uint32_t created_at_unix;
} companion_api_trusted_record_t;

typedef struct
{
    uint8_t *data;
    size_t capacity;
    size_t len;
} companion_api_writer_t;

static QueueHandle_t s_msg_queue;
static TaskHandle_t s_task_handle;
static StackType_t *s_task_stack;
static StaticTask_t s_task_tcb;
static uint8_t *s_rx_buffer;
static uint8_t *s_tx_frame;
static bool s_initialised;
static bool s_authenticated;
static bool s_pairing_pending;
static bool s_auth_challenge_pending;
static uint8_t s_pairing_progress;
static uint8_t s_pending_secret[COMPANION_API_SECRET_LEN];
static hid_button_t s_pending_sequence[4];
static char s_pending_client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
static char s_pending_app_name[COMPANION_API_APP_NAME_MAX_LEN + 1];
static uint32_t s_pending_pair_request_id;
static TickType_t s_pairing_deadline;
static uint8_t s_auth_nonce[COMPANION_API_AUTH_NONCE_LEN];
static char s_auth_client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
static char s_active_client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
static size_t s_rx_len;
static uint32_t s_event_generation;
static uint32_t s_rx_frames;
static uint32_t s_tx_frames;
static uint32_t s_rx_errors;
static uint32_t s_dropped_rx_chunks;
static uint8_t s_trusted_client_count;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t companion_api_read_u16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t companion_api_read_u32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void companion_api_write_u16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void companion_api_write_u32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
    data[2] = (uint8_t)((value >> 16) & 0xFFU);
    data[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static void companion_api_zero_secret(uint8_t *secret, size_t len)
{
    volatile uint8_t *cursor = (volatile uint8_t *)secret;
    while (cursor && len-- > 0)
    {
        *cursor++ = 0;
    }
}

static uint32_t companion_api_unix_time_or_zero(void)
{
    time_t now = time(NULL);
    if (now <= 0 || (uint64_t)now > UINT32_MAX)
    {
        return 0;
    }
    return (uint32_t)now;
}

static void companion_api_touch_generation(void)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_event_generation++;
    if (s_event_generation == 0)
    {
        s_event_generation = 1;
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

static void companion_api_writer_init(companion_api_writer_t *writer)
{
    writer->data = s_tx_frame + COMPANION_API_HEADER_LEN;
    writer->capacity = COMPANION_API_FRAME_MAX_LEN - COMPANION_API_HEADER_LEN;
    writer->len = 0;
}

static bool companion_api_append_tlv(companion_api_writer_t *writer,
                                     uint16_t type,
                                     const void *value,
                                     uint16_t len)
{
    if (!writer || !value || writer->len + COMPANION_API_TLV_HEADER_LEN + len > writer->capacity)
    {
        return false;
    }

    companion_api_write_u16(writer->data + writer->len, type);
    companion_api_write_u16(writer->data + writer->len + 2, len);
    memcpy(writer->data + writer->len + COMPANION_API_TLV_HEADER_LEN, value, len);
    writer->len += COMPANION_API_TLV_HEADER_LEN + len;
    return true;
}

static bool companion_api_append_bool(companion_api_writer_t *writer, uint16_t type, bool value)
{
    uint8_t encoded = value ? 1 : 0;
    return companion_api_append_tlv(writer, type, &encoded, sizeof(encoded));
}

static bool companion_api_append_u8(companion_api_writer_t *writer, uint16_t type, uint8_t value)
{
    return companion_api_append_tlv(writer, type, &value, sizeof(value));
}

static bool companion_api_append_u16(companion_api_writer_t *writer, uint16_t type, uint16_t value)
{
    uint8_t encoded[2];
    companion_api_write_u16(encoded, value);
    return companion_api_append_tlv(writer, type, encoded, sizeof(encoded));
}

static bool companion_api_append_u32(companion_api_writer_t *writer, uint16_t type, uint32_t value)
{
    uint8_t encoded[4];
    companion_api_write_u32(encoded, value);
    return companion_api_append_tlv(writer, type, encoded, sizeof(encoded));
}

static bool companion_api_append_string(companion_api_writer_t *writer, uint16_t type, const char *value)
{
    size_t len;

    if (!value)
    {
        value = "";
    }

    len = strlen(value);
    if (len > UINT16_MAX)
    {
        len = UINT16_MAX;
    }

    return companion_api_append_tlv(writer, type, value, (uint16_t)len);
}

static bool companion_api_find_tlv(const uint8_t *payload,
                                   size_t payload_len,
                                   uint16_t type,
                                   const uint8_t **value_out,
                                   uint16_t *len_out)
{
    size_t offset = 0;

    while (offset + COMPANION_API_TLV_HEADER_LEN <= payload_len)
    {
        uint16_t tlv_type = companion_api_read_u16(payload + offset);
        uint16_t tlv_len = companion_api_read_u16(payload + offset + 2);
        offset += COMPANION_API_TLV_HEADER_LEN;
        if (offset + tlv_len > payload_len)
        {
            return false;
        }
        if (tlv_type == type)
        {
            if (value_out)
            {
                *value_out = payload + offset;
            }
            if (len_out)
            {
                *len_out = tlv_len;
            }
            return true;
        }
        offset += tlv_len;
    }

    return false;
}

static bool companion_api_tlv_u8(const uint8_t *payload, size_t payload_len, uint16_t type, uint8_t *value_out)
{
    const uint8_t *value;
    uint16_t len;
    if (!companion_api_find_tlv(payload, payload_len, type, &value, &len) || len != 1)
    {
        return false;
    }
    *value_out = value[0];
    return true;
}

static bool companion_api_tlv_u32(const uint8_t *payload, size_t payload_len, uint16_t type, uint32_t *value_out)
{
    const uint8_t *value;
    uint16_t len;
    if (!companion_api_find_tlv(payload, payload_len, type, &value, &len) || len != 4)
    {
        return false;
    }
    *value_out = companion_api_read_u32(value);
    return true;
}

static bool companion_api_tlv_string(const uint8_t *payload,
                                     size_t payload_len,
                                     uint16_t type,
                                     char *buffer,
                                     size_t buffer_len)
{
    const uint8_t *value;
    uint16_t len;
    size_t copy_len;

    if (!buffer || buffer_len == 0 ||
        !companion_api_find_tlv(payload, payload_len, type, &value, &len))
    {
        return false;
    }

    copy_len = len;
    if (copy_len >= buffer_len)
    {
        copy_len = buffer_len - 1;
    }
    memcpy(buffer, value, copy_len);
    buffer[copy_len] = '\0';
    return true;
}

static companion_api_error_t companion_api_error_from_esp(esp_err_t err)
{
    switch (err)
    {
    case ESP_OK:
        return COMPANION_API_ERR_OK;
    case ESP_ERR_INVALID_ARG:
        return COMPANION_API_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE:
        return COMPANION_API_ERR_INVALID_STATE;
    case ESP_ERR_NO_MEM:
        return COMPANION_API_ERR_NO_MEM;
    case ESP_ERR_TIMEOUT:
        return COMPANION_API_ERR_TIMEOUT;
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_NVS_NOT_FOUND:
        return COMPANION_API_ERR_NOT_FOUND;
    default:
        return COMPANION_API_ERR_INTERNAL;
    }
}

static const char *companion_api_error_name(companion_api_error_t error)
{
    switch (error)
    {
    case COMPANION_API_ERR_OK:
        return "ok";
    case COMPANION_API_ERR_BAD_FRAME:
        return "bad_frame";
    case COMPANION_API_ERR_UNSUPPORTED_VERSION:
        return "unsupported_version";
    case COMPANION_API_ERR_UNKNOWN_OPCODE:
        return "unknown_opcode";
    case COMPANION_API_ERR_AUTH_REQUIRED:
        return "auth_required";
    case COMPANION_API_ERR_INVALID_ARG:
        return "invalid_arg";
    case COMPANION_API_ERR_INVALID_STATE:
        return "invalid_state";
    case COMPANION_API_ERR_NO_MEM:
        return "no_mem";
    case COMPANION_API_ERR_TIMEOUT:
        return "timeout";
    case COMPANION_API_ERR_NOT_FOUND:
        return "not_found";
    default:
        return "internal";
    }
}

static bool companion_api_secure_equal(const uint8_t *left, const uint8_t *right, size_t len)
{
    uint8_t diff = 0;
    for (size_t index = 0; index < len; index++)
    {
        diff |= (uint8_t)(left[index] ^ right[index]);
    }
    return diff == 0;
}

static bool companion_api_hmac_sha256(const uint8_t *secret,
                                      const uint8_t *data,
                                      size_t data_len,
                                      uint8_t *out_hmac)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || !secret || !data || !out_hmac)
    {
        return false;
    }
    return mbedtls_md_hmac(info, secret, COMPANION_API_SECRET_LEN, data, data_len, out_hmac) == 0;
}

static void companion_api_nvs_slot_key(size_t slot, char *key, size_t key_len)
{
    snprintf(key, key_len, "slot%u", (unsigned)slot);
}

static esp_err_t companion_api_load_record(size_t slot, companion_api_trusted_record_t *record)
{
    nvs_handle_t handle;
    char key[8];
    size_t record_size = sizeof(*record);
    esp_err_t err;

    if (!record || slot >= COMPANION_API_MAX_TRUSTED_CLIENTS)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(record, 0, sizeof(*record));
    err = nvs_open(COMPANION_API_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    companion_api_nvs_slot_key(slot, key, sizeof(key));
    err = nvs_get_blob(handle, key, record, &record_size);
    nvs_close(handle);
    if (err != ESP_OK)
    {
        memset(record, 0, sizeof(*record));
        return err;
    }

    if (record_size != sizeof(*record) || record->version != COMPANION_API_NVS_RECORD_VERSION || !record->valid)
    {
        memset(record, 0, sizeof(*record));
        return ESP_ERR_NOT_FOUND;
    }

    record->client_id[COMPANION_API_CLIENT_ID_MAX_LEN] = '\0';
    record->app_name[COMPANION_API_APP_NAME_MAX_LEN] = '\0';
    return ESP_OK;
}

static uint8_t companion_api_count_trusted_clients(void)
{
    uint8_t count = 0;
    companion_api_trusted_record_t record;

    for (size_t slot = 0; slot < COMPANION_API_MAX_TRUSTED_CLIENTS; slot++)
    {
        if (companion_api_load_record(slot, &record) == ESP_OK)
        {
            count++;
        }
    }

    return count;
}

static esp_err_t companion_api_find_record(const char *client_id,
                                           companion_api_trusted_record_t *record_out,
                                           size_t *slot_out)
{
    companion_api_trusted_record_t record;

    if (!client_id || client_id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t slot = 0; slot < COMPANION_API_MAX_TRUSTED_CLIENTS; slot++)
    {
        if (companion_api_load_record(slot, &record) == ESP_OK && strcmp(record.client_id, client_id) == 0)
        {
            if (record_out)
            {
                *record_out = record;
            }
            if (slot_out)
            {
                *slot_out = slot;
            }
            return ESP_OK;
        }
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t companion_api_store_record(const char *client_id,
                                            const char *app_name,
                                            const uint8_t *secret)
{
    nvs_handle_t handle;
    companion_api_trusted_record_t record;
    char key[8];
    size_t slot = COMPANION_API_MAX_TRUSTED_CLIENTS;
    esp_err_t err;

    if (!client_id || client_id[0] == '\0' || !secret)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (companion_api_find_record(client_id, NULL, &slot) != ESP_OK)
    {
        for (size_t candidate = 0; candidate < COMPANION_API_MAX_TRUSTED_CLIENTS; candidate++)
        {
            if (companion_api_load_record(candidate, &record) != ESP_OK)
            {
                slot = candidate;
                break;
            }
        }
    }

    if (slot >= COMPANION_API_MAX_TRUSTED_CLIENTS)
    {
        companion_api_zero_secret(record.secret, sizeof(record.secret));
        return ESP_ERR_NO_MEM;
    }

    memset(&record, 0, sizeof(record));
    record.version = COMPANION_API_NVS_RECORD_VERSION;
    record.valid = 1;
    strncpy(record.client_id, client_id, sizeof(record.client_id) - 1);
    if (app_name)
    {
        strncpy(record.app_name, app_name, sizeof(record.app_name) - 1);
    }
    memcpy(record.secret, secret, COMPANION_API_SECRET_LEN);
    record.created_at_unix = companion_api_unix_time_or_zero();

    err = nvs_open(COMPANION_API_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        companion_api_zero_secret(record.secret, sizeof(record.secret));
        return err;
    }

    companion_api_nvs_slot_key(slot, key, sizeof(key));
    err = nvs_set_blob(handle, key, &record, sizeof(record));
    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    companion_api_zero_secret(record.secret, sizeof(record.secret));

    uint8_t trusted_count = companion_api_count_trusted_clients();
    taskENTER_CRITICAL(&s_state_lock);
    s_trusted_client_count = trusted_count;
    taskEXIT_CRITICAL(&s_state_lock);
    companion_api_touch_generation();
    return err;
}

static esp_err_t companion_api_revoke_client_internal(const char *client_id)
{
    nvs_handle_t handle;
    char key[8];
    size_t slot;
    esp_err_t err;

    if (companion_api_find_record(client_id, NULL, &slot) != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_open(COMPANION_API_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    companion_api_nvs_slot_key(slot, key, sizeof(key));
    err = nvs_erase_key(handle, key);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK && strcmp(s_active_client_id, client_id) == 0)
    {
        s_authenticated = false;
        s_active_client_id[0] = '\0';
    }

    uint8_t trusted_count = companion_api_count_trusted_clients();
    taskENTER_CRITICAL(&s_state_lock);
    s_trusted_client_count = trusted_count;
    taskEXIT_CRITICAL(&s_state_lock);
    companion_api_touch_generation();
    return err;
}

static esp_err_t companion_api_revoke_all_internal(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(COMPANION_API_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t slot = 0; slot < COMPANION_API_MAX_TRUSTED_CLIENTS; slot++)
    {
        char key[8];
        companion_api_nvs_slot_key(slot, key, sizeof(key));
        esp_err_t erase_err = nvs_erase_key(handle, key);
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND)
        {
            err = erase_err;
            break;
        }
    }

    if (err == ESP_OK)
    {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK)
    {
        s_authenticated = false;
        s_active_client_id[0] = '\0';
        uint8_t trusted_count = companion_api_count_trusted_clients();
        taskENTER_CRITICAL(&s_state_lock);
        s_trusted_client_count = trusted_count;
        taskEXIT_CRITICAL(&s_state_lock);
        companion_api_touch_generation();
    }
    return err;
}

static esp_err_t companion_api_send_frame(companion_api_frame_type_t frame_type,
                                          uint16_t opcode,
                                          uint32_t request_id,
                                          size_t payload_len)
{
    size_t frame_len = COMPANION_API_HEADER_LEN + payload_len;
    size_t offset = 0;
    size_t chunk_len;
    size_t max_payload;
    esp_err_t err;

    if (!s_tx_frame || frame_len > COMPANION_API_FRAME_MAX_LEN)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    if (!bluetooth_service_is_spp_connected() || !bluetooth_service_spp_notifications_enabled())
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_tx_frame[0] = COMPANION_API_MAGIC0;
    s_tx_frame[1] = COMPANION_API_MAGIC1;
    s_tx_frame[2] = COMPANION_API_VERSION;
    s_tx_frame[3] = (uint8_t)frame_type;
    companion_api_write_u16(s_tx_frame + 4, opcode);
    companion_api_write_u32(s_tx_frame + 6, request_id);
    companion_api_write_u16(s_tx_frame + 10, (uint16_t)payload_len);

    max_payload = bluetooth_service_spp_get_max_payload();
    if (max_payload == 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    while (offset < frame_len)
    {
        chunk_len = frame_len - offset;
        if (chunk_len > max_payload)
        {
            chunk_len = max_payload;
        }
        err = bluetooth_service_spp_send_data(s_tx_frame + offset, chunk_len);
        if (err != ESP_OK)
        {
            return err;
        }
        offset += chunk_len;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_tx_frames++;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static esp_err_t companion_api_send_error(uint16_t opcode,
                                          uint32_t request_id,
                                          companion_api_error_t error)
{
    companion_api_writer_t writer;
    companion_api_writer_init(&writer);
    companion_api_append_u16(&writer, COMPANION_API_TLV_ERROR_CODE, (uint16_t)error);
    companion_api_append_string(&writer, COMPANION_API_TLV_ERROR_MESSAGE, companion_api_error_name(error));
    return companion_api_send_frame(COMPANION_API_FRAME_ERROR, opcode, request_id, writer.len);
}

static esp_err_t companion_api_send_ok(uint16_t opcode,
                                       uint32_t request_id,
                                       companion_api_writer_t *writer)
{
    companion_api_writer_t local_writer;

    if (!writer)
    {
        companion_api_writer_init(&local_writer);
        writer = &local_writer;
    }

    return companion_api_send_frame(COMPANION_API_FRAME_RESPONSE, opcode, request_id, writer->len);
}

static bool companion_api_opcode_requires_auth(uint16_t opcode)
{
    switch (opcode)
    {
    case COMPANION_API_OP_HELLO:
    case COMPANION_API_OP_CAPABILITIES:
    case COMPANION_API_OP_PING:
    case COMPANION_API_OP_PAIR_BEGIN:
    case COMPANION_API_OP_PAIR_STATUS:
    case COMPANION_API_OP_PAIR_CANCEL:
    case COMPANION_API_OP_AUTH_CHALLENGE:
    case COMPANION_API_OP_AUTH_PROOF:
        return false;
    default:
        return true;
    }
}

static void companion_api_append_pairing_status(companion_api_writer_t *writer)
{
    companion_api_append_bool(writer, COMPANION_API_TLV_PAIRING_PENDING, s_pairing_pending);
    companion_api_append_u8(writer, COMPANION_API_TLV_PAIRING_PROGRESS, s_pairing_progress);
    companion_api_append_u8(writer, COMPANION_API_TLV_PAIRING_REQUIRED, 4);
    companion_api_append_string(writer, COMPANION_API_TLV_CLIENT_ID, s_pending_client_id);
    companion_api_append_string(writer, COMPANION_API_TLV_APP_NAME, s_pending_app_name);
    if (s_pairing_pending)
    {
        uint8_t sequence[4];
        for (size_t index = 0; index < 4; index++)
        {
            sequence[index] = (uint8_t)s_pending_sequence[index];
        }
        companion_api_append_tlv(writer, COMPANION_API_TLV_BUTTON_SEQUENCE, sequence, sizeof(sequence));
    }
}

static esp_err_t companion_api_send_pairing_status(uint32_t request_id, companion_api_frame_type_t frame_type)
{
    companion_api_writer_t writer;
    companion_api_writer_init(&writer);
    companion_api_append_pairing_status(&writer);
    return companion_api_send_frame(frame_type, COMPANION_API_OP_PAIR_STATUS, request_id, writer.len);
}

static void companion_api_clear_pairing(void)
{
    s_pairing_pending = false;
    s_pairing_progress = 0;
    s_pending_pair_request_id = 0;
    s_pairing_deadline = 0;
    memset(s_pending_sequence, 0, sizeof(s_pending_sequence));
    memset(s_pending_client_id, 0, sizeof(s_pending_client_id));
    memset(s_pending_app_name, 0, sizeof(s_pending_app_name));
    companion_api_zero_secret(s_pending_secret, sizeof(s_pending_secret));
}

static esp_err_t companion_api_complete_pairing(void)
{
    esp_err_t err;

    if (!s_pairing_pending)
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = companion_api_store_record(s_pending_client_id, s_pending_app_name, s_pending_secret);
    if (err != ESP_OK)
    {
        return err;
    }

    s_authenticated = true;
    strncpy(s_active_client_id, s_pending_client_id, sizeof(s_active_client_id) - 1);
    companion_api_clear_pairing();
    companion_api_touch_generation();
    (void)companion_api_send_pairing_status(0, COMPANION_API_FRAME_EVENT);
    return ESP_OK;
}

static esp_err_t companion_api_apply_pairing_button(hid_button_t button)
{
    if (!s_pairing_pending || s_pairing_progress >= 4)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xTaskGetTickCount() > s_pairing_deadline)
    {
        companion_api_clear_pairing();
        companion_api_touch_generation();
        return ESP_ERR_TIMEOUT;
    }

    if (button != s_pending_sequence[s_pairing_progress])
    {
        s_pairing_progress = 0;
        companion_api_touch_generation();
        (void)companion_api_send_pairing_status(0, COMPANION_API_FRAME_EVENT);
        return ESP_ERR_INVALID_ARG;
    }

    s_pairing_progress++;
    companion_api_touch_generation();
    if (s_pairing_progress >= 4)
    {
        return companion_api_complete_pairing();
    }

    (void)companion_api_send_pairing_status(0, COMPANION_API_FRAME_EVENT);
    return ESP_OK;
}

static void companion_api_append_playback_status(companion_api_writer_t *writer)
{
    player_service_snapshot_t snapshot;
    if (player_service_get_snapshot(&snapshot) != ESP_OK)
    {
        return;
    }

    companion_api_append_bool(writer, COMPANION_API_TLV_PLAYING, snapshot.is_playing);
    companion_api_append_bool(writer, COMPANION_API_TLV_PAUSED, snapshot.is_paused);
    companion_api_append_u32(writer, COMPANION_API_TLV_CARTRIDGE_CHECKSUM, snapshot.cartridge_checksum);
    companion_api_append_u32(writer, COMPANION_API_TLV_TRACK_INDEX, snapshot.track_index);
    companion_api_append_u32(writer, COMPANION_API_TLV_TRACK_COUNT, snapshot.track_count);
    companion_api_append_u32(writer, COMPANION_API_TLV_POSITION_SEC, snapshot.playback_position_sec);
    companion_api_append_u32(writer, COMPANION_API_TLV_STARTED_AT, snapshot.track_started_unix);
    companion_api_append_u32(writer, COMPANION_API_TLV_DURATION_SEC, snapshot.track_duration_sec);
    companion_api_append_u8(writer, COMPANION_API_TLV_VOLUME_PERCENT, snapshot.volume_percent);
    companion_api_append_u8(writer, COMPANION_API_TLV_PLAYBACK_MODE, (uint8_t)snapshot.playback_mode);
    companion_api_append_string(writer, COMPANION_API_TLV_TRACK_TITLE, snapshot.track_title);
    companion_api_append_string(writer, COMPANION_API_TLV_TRACK_FILE, snapshot.filename);
    companion_api_append_u8(writer, COMPANION_API_TLV_OUTPUT_TARGET, (uint8_t)audio_output_switch_get_target());
}

static void companion_api_append_cartridge_status(companion_api_writer_t *writer)
{
    companion_api_append_u8(writer, COMPANION_API_TLV_CARTRIDGE_STATUS, (uint8_t)cartridge_service_get_status());
    companion_api_append_bool(writer, COMPANION_API_TLV_CARTRIDGE_MOUNTED, cartridge_service_is_mounted());
    companion_api_append_u32(writer, COMPANION_API_TLV_CARTRIDGE_CHECKSUM, cartridge_service_get_metadata_checksum());
    companion_api_append_u32(writer, COMPANION_API_TLV_METADATA_VERSION, cartridge_service_get_metadata_version());
    companion_api_append_u32(writer, COMPANION_API_TLV_TRACK_COUNT, (uint32_t)cartridge_service_get_metadata_track_count());
}

static void companion_api_append_wifi_status(companion_api_writer_t *writer)
{
    esp_netif_ip_info_t ip_info;
    companion_api_append_u8(writer, COMPANION_API_TLV_WIFI_STATE, (uint8_t)wifi_service_get_state());
    companion_api_append_bool(writer, COMPANION_API_TLV_WIFI_INTERNET, wifi_service_has_internet());
    companion_api_append_bool(writer, COMPANION_API_TLV_WIFI_AUTORECONNECT, wifi_service_get_autoreconnect());
    companion_api_append_u8(writer, COMPANION_API_TLV_WIFI_ACTIVE_SLOT, (uint8_t)(wifi_service_get_active_slot() + 1));
    companion_api_append_u8(writer, COMPANION_API_TLV_WIFI_PREFERRED_SLOT, (uint8_t)(wifi_service_get_preferred_slot() + 1));
    if (wifi_service_get_ip_info(&ip_info) == ESP_OK)
    {
        companion_api_append_u32(writer, COMPANION_API_TLV_WIFI_IP, ip_info.ip.addr);
    }
}

static void companion_api_append_lastfm_status(companion_api_writer_t *writer)
{
    lastfm_service_status_t status;
    lastfm_service_get_status(&status);
    companion_api_append_bool(writer, COMPANION_API_TLV_LASTFM_HAS_AUTH_URL, status.has_auth_url);
    companion_api_append_bool(writer, COMPANION_API_TLV_LASTFM_HAS_TOKEN, status.has_token);
    companion_api_append_bool(writer, COMPANION_API_TLV_LASTFM_HAS_SESSION, status.has_session);
    companion_api_append_bool(writer, COMPANION_API_TLV_LASTFM_BUSY, status.busy);
    companion_api_append_bool(writer, COMPANION_API_TLV_LASTFM_SCROBBLING, status.scrobbling_enabled);
    companion_api_append_bool(writer, COMPANION_API_TLV_LASTFM_NOW_PLAYING, status.now_playing_enabled);
    companion_api_append_u32(writer, COMPANION_API_TLV_LASTFM_PENDING_COMMANDS, status.pending_commands);
    companion_api_append_u32(writer, COMPANION_API_TLV_LASTFM_PENDING_SCROBBLES, status.pending_scrobbles);
    companion_api_append_u32(writer, COMPANION_API_TLV_LASTFM_SUCCESSFUL, status.successful_scrobbles);
    companion_api_append_u32(writer, COMPANION_API_TLV_LASTFM_FAILED, status.failed_scrobbles);
    companion_api_append_string(writer, COMPANION_API_TLV_LASTFM_AUTH_URL, status.auth_url);
    companion_api_append_string(writer, COMPANION_API_TLV_LASTFM_USERNAME, status.username);
}

static void companion_api_append_history_summary(companion_api_writer_t *writer)
{
    companion_api_append_u32(writer, COMPANION_API_TLV_HISTORY_ALBUM_COUNT, (uint32_t)play_history_service_get_album_count());
    companion_api_append_u32(writer, COMPANION_API_TLV_HISTORY_TRACK_COUNT, (uint32_t)play_history_service_get_track_count());
}

static void companion_api_append_bt_audio_status(companion_api_writer_t *writer)
{
    companion_api_append_bool(writer, COMPANION_API_TLV_BT_A2DP_CONNECTED, bluetooth_service_is_a2dp_connected());
    companion_api_append_u32(writer, COMPANION_API_TLV_BT_BONDED_COUNT, (uint32_t)bluetooth_service_get_bonded_device_count());
}

static void companion_api_append_auth_status(companion_api_writer_t *writer)
{
    companion_api_append_bool(writer, COMPANION_API_TLV_AUTHENTICATED, s_authenticated);
    companion_api_append_string(writer, COMPANION_API_TLV_CLIENT_ID, s_active_client_id);
    companion_api_append_u8(writer, COMPANION_API_TLV_TRUSTED_COUNT, s_trusted_client_count);
}

static esp_err_t companion_api_handle_capabilities(uint16_t opcode, uint32_t request_id)
{
    companion_api_writer_t writer;
    companion_api_writer_init(&writer);
    companion_api_append_u16(&writer, COMPANION_API_TLV_PROTOCOL_VERSION, COMPANION_API_VERSION);
    companion_api_append_u16(&writer, COMPANION_API_TLV_MAX_FRAME, COMPANION_API_FRAME_MAX_LEN);
    companion_api_append_u16(&writer, COMPANION_API_TLV_MTU, (uint16_t)bluetooth_service_spp_get_mtu());
    companion_api_append_u16(&writer, COMPANION_API_TLV_MAX_PAYLOAD, (uint16_t)bluetooth_service_spp_get_max_payload());
    companion_api_append_u32(&writer, COMPANION_API_TLV_FEATURE_BITS, 0x0000007FU);
    companion_api_append_auth_status(&writer);
    companion_api_append_pairing_status(&writer);
    return companion_api_send_ok(opcode, request_id, &writer);
}

static esp_err_t companion_api_handle_hello(uint16_t opcode, uint32_t request_id)
{
    companion_api_writer_t writer;
    companion_api_writer_init(&writer);
    companion_api_append_string(&writer, COMPANION_API_TLV_APP_NAME, "Jukeboy firmware");
    companion_api_append_u16(&writer, COMPANION_API_TLV_PROTOCOL_VERSION, COMPANION_API_VERSION);
    companion_api_append_auth_status(&writer);
    return companion_api_send_ok(opcode, request_id, &writer);
}

static esp_err_t companion_api_handle_pair_begin(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    const uint8_t *secret;
    const uint8_t *sequence;
    uint16_t secret_len;
    uint16_t sequence_len;
    char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
    char app_name[COMPANION_API_APP_NAME_MAX_LEN + 1];

    if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_CLIENT_ID, client_id, sizeof(client_id)) ||
        !companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_APP_NAME, app_name, sizeof(app_name)) ||
        !companion_api_find_tlv(payload, payload_len, COMPANION_API_TLV_SHARED_SECRET, &secret, &secret_len) ||
        !companion_api_find_tlv(payload, payload_len, COMPANION_API_TLV_BUTTON_SEQUENCE, &sequence, &sequence_len) ||
        secret_len != COMPANION_API_SECRET_LEN || sequence_len != 4 || client_id[0] == '\0')
    {
        return companion_api_send_error(COMPANION_API_OP_PAIR_BEGIN, request_id, COMPANION_API_ERR_INVALID_ARG);
    }

    for (size_t index = 0; index < 4; index++)
    {
        if (sequence[index] >= HID_BUTTON_COUNT)
        {
            return companion_api_send_error(COMPANION_API_OP_PAIR_BEGIN, request_id, COMPANION_API_ERR_INVALID_ARG);
        }
    }

    companion_api_clear_pairing();
    s_pairing_pending = true;
    s_pairing_progress = 0;
    s_pending_pair_request_id = request_id;
    s_pairing_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(COMPANION_API_PAIRING_TIMEOUT_MS);
    strncpy(s_pending_client_id, client_id, sizeof(s_pending_client_id) - 1);
    strncpy(s_pending_app_name, app_name, sizeof(s_pending_app_name) - 1);
    memcpy(s_pending_secret, secret, COMPANION_API_SECRET_LEN);
    for (size_t index = 0; index < 4; index++)
    {
        s_pending_sequence[index] = (hid_button_t)sequence[index];
    }

    companion_api_touch_generation();
    return companion_api_send_pairing_status(request_id, COMPANION_API_FRAME_RESPONSE);
}

static esp_err_t companion_api_handle_auth_challenge(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    companion_api_trusted_record_t record;
    char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
    companion_api_writer_t writer;

    if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_CLIENT_ID, client_id, sizeof(client_id)) ||
        companion_api_find_record(client_id, &record, NULL) != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_AUTH_CHALLENGE, request_id, COMPANION_API_ERR_NOT_FOUND);
    }

    esp_fill_random(s_auth_nonce, sizeof(s_auth_nonce));
    strncpy(s_auth_client_id, client_id, sizeof(s_auth_client_id) - 1);
    s_auth_challenge_pending = true;

    companion_api_writer_init(&writer);
    companion_api_append_string(&writer, COMPANION_API_TLV_CLIENT_ID, client_id);
    companion_api_append_tlv(&writer, COMPANION_API_TLV_AUTH_NONCE, s_auth_nonce, sizeof(s_auth_nonce));
    companion_api_zero_secret(record.secret, sizeof(record.secret));
    return companion_api_send_ok(COMPANION_API_OP_AUTH_CHALLENGE, request_id, &writer);
}

static esp_err_t companion_api_handle_auth_proof(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    companion_api_trusted_record_t record;
    char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
    const uint8_t *provided_hmac;
    uint16_t hmac_len;
    uint8_t expected_hmac[COMPANION_API_HMAC_LEN];
    companion_api_writer_t writer;
    esp_err_t err;

    if (!s_auth_challenge_pending ||
        !companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_CLIENT_ID, client_id, sizeof(client_id)) ||
        strcmp(client_id, s_auth_client_id) != 0 ||
        !companion_api_find_tlv(payload, payload_len, COMPANION_API_TLV_AUTH_HMAC, &provided_hmac, &hmac_len) ||
        hmac_len != COMPANION_API_HMAC_LEN)
    {
        return companion_api_send_error(COMPANION_API_OP_AUTH_PROOF, request_id, COMPANION_API_ERR_INVALID_ARG);
    }

    err = companion_api_find_record(client_id, &record, NULL);
    if (err != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_AUTH_PROOF, request_id, companion_api_error_from_esp(err));
    }

    if (!companion_api_hmac_sha256(record.secret, s_auth_nonce, sizeof(s_auth_nonce), expected_hmac) ||
        !companion_api_secure_equal(expected_hmac, provided_hmac, sizeof(expected_hmac)))
    {
        companion_api_zero_secret(record.secret, sizeof(record.secret));
        return companion_api_send_error(COMPANION_API_OP_AUTH_PROOF, request_id, COMPANION_API_ERR_INVALID_ARG);
    }

    s_authenticated = true;
    s_auth_challenge_pending = false;
    strncpy(s_active_client_id, client_id, sizeof(s_active_client_id) - 1);
    companion_api_zero_secret(record.secret, sizeof(record.secret));
    companion_api_touch_generation();

    companion_api_writer_init(&writer);
    companion_api_append_auth_status(&writer);
    return companion_api_send_ok(COMPANION_API_OP_AUTH_PROOF, request_id, &writer);
}

static esp_err_t companion_api_handle_trusted_list(uint32_t request_id)
{
    companion_api_writer_t writer;
    companion_api_trusted_record_t record;
    companion_api_writer_init(&writer);
    companion_api_append_u8(&writer, COMPANION_API_TLV_TRUSTED_COUNT, companion_api_count_trusted_clients());
    for (size_t slot = 0; slot < COMPANION_API_MAX_TRUSTED_CLIENTS; slot++)
    {
        if (companion_api_load_record(slot, &record) == ESP_OK)
        {
            companion_api_append_string(&writer, COMPANION_API_TLV_CLIENT_ID, record.client_id);
            companion_api_append_string(&writer, COMPANION_API_TLV_APP_NAME, record.app_name);
            companion_api_append_u32(&writer, COMPANION_API_TLV_CREATED_AT, record.created_at_unix);
            companion_api_zero_secret(record.secret, sizeof(record.secret));
        }
    }
    return companion_api_send_ok(COMPANION_API_OP_TRUSTED_LIST, request_id, &writer);
}

static esp_err_t companion_api_handle_snapshot(uint16_t opcode, uint32_t request_id)
{
    companion_api_writer_t writer;
    companion_api_writer_init(&writer);
    companion_api_append_u32(&writer, COMPANION_API_TLV_GENERATION, s_event_generation);
    companion_api_append_u32(&writer, COMPANION_API_TLV_UPTIME_MS, (uint32_t)(esp_timer_get_time() / 1000ULL));
    companion_api_append_auth_status(&writer);
    companion_api_append_pairing_status(&writer);
    companion_api_append_playback_status(&writer);
    companion_api_append_cartridge_status(&writer);
    companion_api_append_wifi_status(&writer);
    companion_api_append_lastfm_status(&writer);
    companion_api_append_history_summary(&writer);
    companion_api_append_bt_audio_status(&writer);
    return companion_api_send_ok(opcode, request_id, &writer);
}

static esp_err_t companion_api_handle_playback_control(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    uint8_t action;
    uint32_t value = 0;
    esp_err_t err = ESP_OK;

    if (!companion_api_tlv_u8(payload, payload_len, COMPANION_API_TLV_ACTION, &action))
    {
        return companion_api_send_error(COMPANION_API_OP_PLAYBACK_CONTROL, request_id, COMPANION_API_ERR_INVALID_ARG);
    }
    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_VALUE, &value);

    switch (action)
    {
    case COMPANION_API_PLAYBACK_ACTION_NEXT:
        err = player_service_request_control(PLAYER_SVC_CONTROL_NEXT);
        break;
    case COMPANION_API_PLAYBACK_ACTION_PREVIOUS:
        err = player_service_request_control(PLAYER_SVC_CONTROL_PREVIOUS);
        break;
    case COMPANION_API_PLAYBACK_ACTION_PAUSE_TOGGLE:
        err = player_service_request_control(PLAYER_SVC_CONTROL_PAUSE);
        break;
    case COMPANION_API_PLAYBACK_ACTION_FAST_FORWARD:
        err = player_service_request_control(PLAYER_SVC_CONTROL_FAST_FORWARD);
        break;
    case COMPANION_API_PLAYBACK_ACTION_REWIND:
        err = player_service_request_control(PLAYER_SVC_CONTROL_FAST_BACKWARD);
        break;
    case COMPANION_API_PLAYBACK_ACTION_PLAY_INDEX:
        err = player_service_play_track_by_index(value);
        break;
    case COMPANION_API_PLAYBACK_ACTION_SEEK_SECONDS:
        err = player_service_seek_to_seconds(value);
        break;
    case COMPANION_API_PLAYBACK_ACTION_SET_VOLUME_PERCENT:
        if (value > 100)
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            player_service_set_volume_absolute((uint8_t)((value * 127U + 50U) / 100U));
        }
        break;
    case COMPANION_API_PLAYBACK_ACTION_SET_MODE:
        err = player_service_set_playback_mode((player_service_playback_mode_t)value);
        break;
    case COMPANION_API_PLAYBACK_ACTION_SET_OUTPUT_TARGET:
        err = audio_output_switch_select((audio_output_target_t)value);
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_PLAYBACK_CONTROL,
                                        request_id,
                                        companion_api_error_from_esp(err));
    }
    companion_api_touch_generation();
    return companion_api_handle_snapshot(COMPANION_API_OP_PLAYBACK_CONTROL, request_id);
}

static esp_err_t companion_api_handle_library_album(uint32_t request_id)
{
    companion_api_writer_t writer;
    companion_api_writer_init(&writer);
    companion_api_append_cartridge_status(&writer);
    companion_api_append_string(&writer, COMPANION_API_TLV_ALBUM_NAME, cartridge_service_get_album_name());
    companion_api_append_string(&writer, COMPANION_API_TLV_ALBUM_ARTIST, cartridge_service_get_album_artist());
    companion_api_append_string(&writer, COMPANION_API_TLV_ALBUM_DESCRIPTION, cartridge_service_get_album_description());
    companion_api_append_u32(&writer, COMPANION_API_TLV_ALBUM_YEAR, cartridge_service_get_album_year());
    companion_api_append_u32(&writer, COMPANION_API_TLV_ALBUM_DURATION, cartridge_service_get_album_duration_sec());
    companion_api_append_string(&writer, COMPANION_API_TLV_ALBUM_GENRE, cartridge_service_get_album_genre());
    return companion_api_send_ok(COMPANION_API_OP_LIBRARY_ALBUM, request_id, &writer);
}

static esp_err_t companion_api_handle_track_page(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    companion_api_writer_t writer;
    uint32_t offset = 0;
    uint32_t count = 8;
    uint32_t returned = 0;
    size_t total = cartridge_service_get_metadata_track_count();

    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_OFFSET, &offset);
    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_COUNT, &count);
    if (count > 8)
    {
        count = 8;
    }

    companion_api_writer_init(&writer);
    companion_api_append_u32(&writer, COMPANION_API_TLV_OFFSET, offset);
    companion_api_append_u32(&writer, COMPANION_API_TLV_TRACK_COUNT, (uint32_t)total);
    for (uint32_t index = 0; index < count && (size_t)(offset + index) < total; index++)
    {
        size_t track_index = (size_t)(offset + index);
        size_t before = writer.len;
        if (!companion_api_append_u32(&writer, COMPANION_API_TLV_TRACK_INDEX, (uint32_t)track_index) ||
            !companion_api_append_string(&writer, COMPANION_API_TLV_TRACK_TITLE, cartridge_service_get_track_name(track_index)) ||
            !companion_api_append_string(&writer, COMPANION_API_TLV_TRACK_ARTIST, cartridge_service_get_track_artists(track_index)) ||
            !companion_api_append_u32(&writer, COMPANION_API_TLV_DURATION_SEC, cartridge_service_get_track_duration_sec(track_index)) ||
            !companion_api_append_u32(&writer, COMPANION_API_TLV_TRACK_FILE, cartridge_service_get_track_file_num(track_index)))
        {
            writer.len = before;
            break;
        }
        returned++;
    }
    companion_api_append_u32(&writer, COMPANION_API_TLV_RETURNED_COUNT, returned);
    return companion_api_send_ok(COMPANION_API_OP_LIBRARY_TRACK_PAGE, request_id, &writer);
}

static esp_err_t companion_api_handle_wifi_scan_results(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    wifi_svc_scan_result_t result;
    companion_api_writer_t writer;
    uint32_t offset = 0;
    uint32_t count = 8;
    uint32_t returned = 0;
    esp_err_t err = wifi_service_get_scan_results(&result);

    if (err != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_WIFI_SCAN_RESULTS, request_id, companion_api_error_from_esp(err));
    }

    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_OFFSET, &offset);
    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_COUNT, &count);
    if (count > 8)
    {
        count = 8;
    }

    companion_api_writer_init(&writer);
    companion_api_append_u32(&writer, COMPANION_API_TLV_OFFSET, offset);
    companion_api_append_u32(&writer, COMPANION_API_TLV_COUNT, result.count);
    for (uint32_t index = 0; index < count && (offset + index) < result.count; index++)
    {
        wifi_ap_record_t *record = &result.records[offset + index];
        size_t before = writer.len;
        if (!companion_api_append_string(&writer, COMPANION_API_TLV_WIFI_SSID, (const char *)record->ssid) ||
            !companion_api_append_u32(&writer, COMPANION_API_TLV_WIFI_RSSI, (uint32_t)(int32_t)record->rssi) ||
            !companion_api_append_u8(&writer, COMPANION_API_TLV_WIFI_CHANNEL, record->primary) ||
            !companion_api_append_u8(&writer, COMPANION_API_TLV_WIFI_AUTHMODE, (uint8_t)record->authmode))
        {
            writer.len = before;
            break;
        }
        returned++;
    }
    companion_api_append_u32(&writer, COMPANION_API_TLV_RETURNED_COUNT, returned);
    return companion_api_send_ok(COMPANION_API_OP_WIFI_SCAN_RESULTS, request_id, &writer);
}

static esp_err_t companion_api_handle_wifi_connect(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    char ssid[33];
    char password[65];
    esp_err_t err;

    if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_WIFI_SSID, ssid, sizeof(ssid)))
    {
        return companion_api_send_error(COMPANION_API_OP_WIFI_CONNECT, request_id, COMPANION_API_ERR_INVALID_ARG);
    }
    if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_WIFI_PASSWORD, password, sizeof(password)))
    {
        password[0] = '\0';
    }

    err = wifi_service_connect(ssid, password);
    companion_api_zero_secret((uint8_t *)password, sizeof(password));
    if (err != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_WIFI_CONNECT, request_id, companion_api_error_from_esp(err));
    }
    companion_api_touch_generation();
    return companion_api_handle_snapshot(COMPANION_API_OP_WIFI_CONNECT, request_id);
}

static esp_err_t companion_api_handle_wifi_simple(uint16_t opcode, uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    uint32_t value = 0;
    esp_err_t err = ESP_OK;

    switch (opcode)
    {
    case COMPANION_API_OP_WIFI_SCAN_START:
        err = wifi_service_scan();
        break;
    case COMPANION_API_OP_WIFI_CONNECT_SLOT:
        if (!companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_WIFI_SLOT, &value))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            err = wifi_service_connect_slot((uint8_t)value);
        }
        break;
    case COMPANION_API_OP_WIFI_DISCONNECT:
        err = wifi_service_disconnect();
        break;
    case COMPANION_API_OP_WIFI_AUTORECONNECT:
        if (!companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_VALUE, &value))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            err = wifi_service_set_autoreconnect(value != 0);
        }
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err != ESP_OK)
    {
        return companion_api_send_error(opcode, request_id, companion_api_error_from_esp(err));
    }
    companion_api_touch_generation();
    return companion_api_handle_snapshot(opcode, request_id);
}

static esp_err_t companion_api_handle_lastfm_control(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    uint8_t action;
    uint32_t value = 0;
    char text_a[LASTFM_SERVICE_BASE_URL_MAX_LEN + 1];
    char text_b[LASTFM_SERVICE_USERNAME_MAX_LEN + 1];
    esp_err_t err = ESP_OK;

    if (!companion_api_tlv_u8(payload, payload_len, COMPANION_API_TLV_ACTION, &action))
    {
        return companion_api_send_error(COMPANION_API_OP_LASTFM_CONTROL, request_id, COMPANION_API_ERR_INVALID_ARG);
    }

    switch (action)
    {
    case COMPANION_API_LASTFM_ACTION_SET_AUTH_URL:
        if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_LASTFM_AUTH_URL, text_a, sizeof(text_a)))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            err = lastfm_service_set_auth_url(text_a);
        }
        break;
    case COMPANION_API_LASTFM_ACTION_REQUEST_TOKEN:
        err = lastfm_service_request_token();
        break;
    case COMPANION_API_LASTFM_ACTION_AUTH:
        if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_LASTFM_USERNAME, text_a, sizeof(text_a)) ||
            !companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_WIFI_PASSWORD, text_b, sizeof(text_b)))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            err = lastfm_service_request_auth(text_a, text_b);
            companion_api_zero_secret((uint8_t *)text_b, sizeof(text_b));
        }
        break;
    case COMPANION_API_LASTFM_ACTION_LOGOUT:
        err = lastfm_service_logout();
        break;
    case COMPANION_API_LASTFM_ACTION_SET_SCROBBLING:
        if (!companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_VALUE, &value))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            err = lastfm_service_set_scrobbling_enabled(value != 0);
        }
        break;
    case COMPANION_API_LASTFM_ACTION_SET_NOW_PLAYING:
        if (!companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_VALUE, &value))
        {
            err = ESP_ERR_INVALID_ARG;
        }
        else
        {
            err = lastfm_service_set_now_playing_enabled(value != 0);
        }
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_LASTFM_CONTROL, request_id, companion_api_error_from_esp(err));
    }
    companion_api_touch_generation();
    return companion_api_handle_snapshot(COMPANION_API_OP_LASTFM_CONTROL, request_id);
}

static esp_err_t companion_api_handle_history_album_page(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    companion_api_writer_t writer;
    uint32_t offset = 0;
    uint32_t count = 4;
    uint32_t returned = 0;
    size_t total = play_history_service_get_album_count();

    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_OFFSET, &offset);
    (void)companion_api_tlv_u32(payload, payload_len, COMPANION_API_TLV_COUNT, &count);
    if (count > 4)
    {
        count = 4;
    }

    companion_api_writer_init(&writer);
    companion_api_append_u32(&writer, COMPANION_API_TLV_OFFSET, offset);
    companion_api_append_u32(&writer, COMPANION_API_TLV_HISTORY_ALBUM_COUNT, (uint32_t)total);
    for (uint32_t index = 0; index < count && (size_t)(offset + index) < total; index++)
    {
        play_history_album_record_t record;
        size_t before = writer.len;
        if (!play_history_service_get_album_record((size_t)(offset + index), &record))
        {
            continue;
        }
        if (!companion_api_append_u32(&writer, COMPANION_API_TLV_CARTRIDGE_CHECKSUM, record.checksum) ||
            !companion_api_append_u32(&writer, COMPANION_API_TLV_TRACK_COUNT, record.track_count) ||
            !companion_api_append_u32(&writer, COMPANION_API_TLV_HISTORY_FIRST_SEEN, record.first_seen_sequence) ||
            !companion_api_append_u32(&writer, COMPANION_API_TLV_HISTORY_LAST_SEEN, record.last_seen_sequence) ||
            !companion_api_append_string(&writer, COMPANION_API_TLV_ALBUM_NAME, record.metadata.album_name) ||
            !companion_api_append_string(&writer, COMPANION_API_TLV_ALBUM_ARTIST, record.metadata.artist))
        {
            writer.len = before;
            break;
        }
        returned++;
    }
    companion_api_append_u32(&writer, COMPANION_API_TLV_RETURNED_COUNT, returned);
    return companion_api_send_ok(COMPANION_API_OP_HISTORY_ALBUM_PAGE, request_id, &writer);
}

static esp_err_t companion_api_handle_bt_audio_control(uint32_t request_id, const uint8_t *payload, size_t payload_len)
{
    uint8_t action;
    esp_err_t err;

    if (!companion_api_tlv_u8(payload, payload_len, COMPANION_API_TLV_ACTION, &action))
    {
        return companion_api_send_error(COMPANION_API_OP_BT_AUDIO_CONTROL, request_id, COMPANION_API_ERR_INVALID_ARG);
    }

    switch (action)
    {
    case COMPANION_API_BT_ACTION_CONNECT_LAST:
        err = bluetooth_service_connect_last_bonded_a2dp_device();
        break;
    case COMPANION_API_BT_ACTION_PAIR_BEST:
        err = bluetooth_service_pair_best_a2dp_sink();
        break;
    case COMPANION_API_BT_ACTION_DISCONNECT:
        err = bluetooth_service_disconnect_a2dp();
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }

    if (err != ESP_OK)
    {
        return companion_api_send_error(COMPANION_API_OP_BT_AUDIO_CONTROL, request_id, companion_api_error_from_esp(err));
    }
    companion_api_touch_generation();
    return companion_api_handle_snapshot(COMPANION_API_OP_BT_AUDIO_CONTROL, request_id);
}

static esp_err_t companion_api_handle_request(uint16_t opcode,
                                              uint32_t request_id,
                                              const uint8_t *payload,
                                              size_t payload_len)
{
    if (companion_api_opcode_requires_auth(opcode) && !s_authenticated)
    {
        return companion_api_send_error(opcode, request_id, COMPANION_API_ERR_AUTH_REQUIRED);
    }

    switch (opcode)
    {
    case COMPANION_API_OP_HELLO:
        return companion_api_handle_hello(opcode, request_id);
    case COMPANION_API_OP_CAPABILITIES:
        return companion_api_handle_capabilities(opcode, request_id);
    case COMPANION_API_OP_PING:
        if (payload_len > COMPANION_API_FRAME_MAX_LEN - COMPANION_API_HEADER_LEN)
        {
            return companion_api_send_error(opcode, request_id, COMPANION_API_ERR_BAD_FRAME);
        }
        memcpy(s_tx_frame + COMPANION_API_HEADER_LEN, payload, payload_len);
        return companion_api_send_frame(COMPANION_API_FRAME_RESPONSE, opcode, request_id, payload_len);
    case COMPANION_API_OP_PAIR_BEGIN:
        return companion_api_handle_pair_begin(request_id, payload, payload_len);
    case COMPANION_API_OP_PAIR_STATUS:
        return companion_api_send_pairing_status(request_id, COMPANION_API_FRAME_RESPONSE);
    case COMPANION_API_OP_PAIR_CANCEL:
        companion_api_clear_pairing();
        companion_api_touch_generation();
        return companion_api_send_pairing_status(request_id, COMPANION_API_FRAME_RESPONSE);
    case COMPANION_API_OP_AUTH_CHALLENGE:
        return companion_api_handle_auth_challenge(request_id, payload, payload_len);
    case COMPANION_API_OP_AUTH_PROOF:
        return companion_api_handle_auth_proof(request_id, payload, payload_len);
    case COMPANION_API_OP_TRUSTED_LIST:
        return companion_api_handle_trusted_list(request_id);
    case COMPANION_API_OP_TRUSTED_REVOKE:
    {
        char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
        esp_err_t err;
        if (!companion_api_tlv_string(payload, payload_len, COMPANION_API_TLV_CLIENT_ID, client_id, sizeof(client_id)))
        {
            return companion_api_send_error(opcode, request_id, COMPANION_API_ERR_INVALID_ARG);
        }
        err = companion_api_revoke_client_internal(client_id);
        if (err != ESP_OK)
        {
            return companion_api_send_error(opcode, request_id, companion_api_error_from_esp(err));
        }
        return companion_api_handle_trusted_list(request_id);
    }
    case COMPANION_API_OP_SNAPSHOT:
    case COMPANION_API_OP_PLAYBACK_STATUS:
    case COMPANION_API_OP_WIFI_STATUS:
    case COMPANION_API_OP_LASTFM_STATUS:
    case COMPANION_API_OP_HISTORY_SUMMARY:
    case COMPANION_API_OP_BT_AUDIO_STATUS:
        return companion_api_handle_snapshot(opcode, request_id);
    case COMPANION_API_OP_PLAYBACK_CONTROL:
        return companion_api_handle_playback_control(request_id, payload, payload_len);
    case COMPANION_API_OP_LIBRARY_ALBUM:
        return companion_api_handle_library_album(request_id);
    case COMPANION_API_OP_LIBRARY_TRACK_PAGE:
        return companion_api_handle_track_page(request_id, payload, payload_len);
    case COMPANION_API_OP_WIFI_SCAN_START:
    case COMPANION_API_OP_WIFI_CONNECT_SLOT:
    case COMPANION_API_OP_WIFI_DISCONNECT:
    case COMPANION_API_OP_WIFI_AUTORECONNECT:
        return companion_api_handle_wifi_simple(opcode, request_id, payload, payload_len);
    case COMPANION_API_OP_WIFI_SCAN_RESULTS:
        return companion_api_handle_wifi_scan_results(request_id, payload, payload_len);
    case COMPANION_API_OP_WIFI_CONNECT:
        return companion_api_handle_wifi_connect(request_id, payload, payload_len);
    case COMPANION_API_OP_LASTFM_CONTROL:
        return companion_api_handle_lastfm_control(request_id, payload, payload_len);
    case COMPANION_API_OP_HISTORY_ALBUM_PAGE:
        return companion_api_handle_history_album_page(request_id, payload, payload_len);
    case COMPANION_API_OP_BT_AUDIO_CONTROL:
        return companion_api_handle_bt_audio_control(request_id, payload, payload_len);
    default:
        return companion_api_send_error(opcode, request_id, COMPANION_API_ERR_UNKNOWN_OPCODE);
    }
}

static void companion_api_parse_rx_buffer(void)
{
    size_t offset = 0;

    while (s_rx_len - offset >= COMPANION_API_HEADER_LEN)
    {
        uint8_t *frame = s_rx_buffer + offset;
        uint16_t opcode;
        uint32_t request_id;
        uint16_t payload_len;
        size_t frame_len;
        esp_err_t err;

        if (frame[0] != COMPANION_API_MAGIC0 || frame[1] != COMPANION_API_MAGIC1)
        {
            offset++;
            taskENTER_CRITICAL(&s_state_lock);
            s_rx_errors++;
            taskEXIT_CRITICAL(&s_state_lock);
            continue;
        }

        if (frame[2] != COMPANION_API_VERSION)
        {
            opcode = companion_api_read_u16(frame + 4);
            request_id = companion_api_read_u32(frame + 6);
            (void)companion_api_send_error(opcode, request_id, COMPANION_API_ERR_UNSUPPORTED_VERSION);
            offset += COMPANION_API_HEADER_LEN;
            continue;
        }

        opcode = companion_api_read_u16(frame + 4);
        request_id = companion_api_read_u32(frame + 6);
        payload_len = companion_api_read_u16(frame + 10);
        frame_len = COMPANION_API_HEADER_LEN + payload_len;
        if (frame_len > COMPANION_API_FRAME_MAX_LEN)
        {
            (void)companion_api_send_error(opcode, request_id, COMPANION_API_ERR_BAD_FRAME);
            offset += COMPANION_API_HEADER_LEN;
            continue;
        }
        if (s_rx_len - offset < frame_len)
        {
            break;
        }

        if (frame[3] != COMPANION_API_FRAME_REQUEST)
        {
            (void)companion_api_send_error(opcode, request_id, COMPANION_API_ERR_BAD_FRAME);
        }
        else
        {
            err = companion_api_handle_request(opcode, request_id, frame + COMPANION_API_HEADER_LEN, payload_len);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(TAG, "request 0x%04x response failed: %s", opcode, esp_err_to_name(err));
            }
            taskENTER_CRITICAL(&s_state_lock);
            s_rx_frames++;
            taskEXIT_CRITICAL(&s_state_lock);
        }

        offset += frame_len;
    }

    if (offset > 0)
    {
        memmove(s_rx_buffer, s_rx_buffer + offset, s_rx_len - offset);
        s_rx_len -= offset;
    }
}

static void companion_api_handle_rx_chunk(const uint8_t *data, size_t len)
{
    if (!data || len == 0)
    {
        return;
    }

    if (len > COMPANION_API_FRAME_MAX_LEN - s_rx_len)
    {
        s_rx_len = 0;
        taskENTER_CRITICAL(&s_state_lock);
        s_rx_errors++;
        taskEXIT_CRITICAL(&s_state_lock);
        (void)companion_api_send_error(0, 0, COMPANION_API_ERR_BAD_FRAME);
        return;
    }

    memcpy(s_rx_buffer + s_rx_len, data, len);
    s_rx_len += len;
    companion_api_parse_rx_buffer();
}

static void companion_api_send_heartbeat(void)
{
    companion_api_writer_t writer;
    if (!bluetooth_service_is_spp_connected() || !bluetooth_service_spp_notifications_enabled())
    {
        return;
    }
    companion_api_writer_init(&writer);
    companion_api_append_u32(&writer, COMPANION_API_TLV_UPTIME_MS, (uint32_t)(esp_timer_get_time() / 1000ULL));
    companion_api_append_u32(&writer, COMPANION_API_TLV_GENERATION, s_event_generation);
    companion_api_append_bool(&writer, COMPANION_API_TLV_AUTHENTICATED, s_authenticated);
    companion_api_append_u8(&writer, COMPANION_API_TLV_QUEUE_FREE, (uint8_t)uxQueueSpacesAvailable(s_msg_queue));
    companion_api_append_u32(&writer, COMPANION_API_TLV_RX_FRAMES, s_rx_frames);
    companion_api_append_u32(&writer, COMPANION_API_TLV_TX_FRAMES, s_tx_frames);
    companion_api_append_u32(&writer, COMPANION_API_TLV_RX_ERRORS, s_rx_errors);
    (void)companion_api_send_frame(COMPANION_API_FRAME_HEARTBEAT, COMPANION_API_OP_SNAPSHOT, 0, writer.len);
}

static void companion_api_process_msg(companion_api_msg_t *msg)
{
    esp_err_t result = ESP_OK;

    switch (msg->type)
    {
    case COMPANION_API_MSG_RX_CHUNK:
        companion_api_handle_rx_chunk(msg->data.bytes, msg->len);
        break;
    case COMPANION_API_MSG_HID_BUTTON:
        result = companion_api_apply_pairing_button(msg->data.button);
        break;
    case COMPANION_API_MSG_CONSOLE_CONFIRM_PAIRING:
        result = companion_api_complete_pairing();
        break;
    case COMPANION_API_MSG_CONSOLE_CANCEL_PAIRING:
        companion_api_clear_pairing();
        companion_api_touch_generation();
        result = ESP_OK;
        (void)companion_api_send_pairing_status(0, COMPANION_API_FRAME_EVENT);
        break;
    case COMPANION_API_MSG_CONSOLE_REVOKE_CLIENT:
        result = companion_api_revoke_client_internal(msg->data.client_id);
        break;
    case COMPANION_API_MSG_CONSOLE_REVOKE_ALL:
        result = companion_api_revoke_all_internal();
        break;
    case COMPANION_API_MSG_LINK_CONNECTED:
        s_rx_len = 0;
        companion_api_touch_generation();
        break;
    case COMPANION_API_MSG_LINK_DISCONNECTED:
        s_rx_len = 0;
        s_authenticated = false;
        s_auth_challenge_pending = false;
        s_active_client_id[0] = '\0';
        companion_api_touch_generation();
        break;
    default:
        result = ESP_ERR_INVALID_ARG;
        break;
    }

    if (msg->result_out)
    {
        *msg->result_out = result;
    }
    if (msg->completion_semaphore)
    {
        xSemaphoreGive(msg->completion_semaphore);
    }
}

static void companion_api_task(void *param)
{
    (void)param;
    TickType_t heartbeat_ticks = pdMS_TO_TICKS(COMPANION_API_HEARTBEAT_MS);
    TickType_t next_heartbeat = xTaskGetTickCount() + heartbeat_ticks;

    for (;;)
    {
        companion_api_msg_t msg;
        TickType_t now = xTaskGetTickCount();
        TickType_t wait_ticks = next_heartbeat > now ? next_heartbeat - now : 0;

        if (xQueueReceive(s_msg_queue, &msg, wait_ticks) == pdPASS)
        {
            companion_api_process_msg(&msg);
        }

        now = xTaskGetTickCount();
        if (now >= next_heartbeat)
        {
            if (s_pairing_pending && now > s_pairing_deadline)
            {
                companion_api_clear_pairing();
                companion_api_touch_generation();
                (void)companion_api_send_pairing_status(0, COMPANION_API_FRAME_EVENT);
            }
            companion_api_send_heartbeat();
            next_heartbeat = now + heartbeat_ticks;
        }
    }
}

static void companion_api_queue_link_message(companion_api_msg_type_t type)
{
    companion_api_msg_t msg = {.type = type};
    if (s_msg_queue && xQueueSend(s_msg_queue, &msg, 0) != pdPASS)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_dropped_rx_chunks++;
        taskEXIT_CRITICAL(&s_state_lock);
    }
}

static void companion_api_spp_rx_cb(const uint8_t *data, size_t len, void *user_ctx)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_RX_CHUNK};
    (void)user_ctx;

    if (!data || len == 0 || !s_msg_queue)
    {
        return;
    }

    if (len > sizeof(msg.data.bytes))
    {
        len = sizeof(msg.data.bytes);
    }
    msg.len = (uint16_t)len;
    memcpy(msg.data.bytes, data, len);
    if (xQueueSend(s_msg_queue, &msg, 0) != pdPASS)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_dropped_rx_chunks++;
        taskEXIT_CRITICAL(&s_state_lock);
    }
}

static void companion_api_bt_connection_cb(bluetooth_service_connection_event_t event,
                                           const esp_bd_addr_t remote_bda,
                                           void *user_ctx)
{
    (void)remote_bda;
    (void)user_ctx;

    switch (event)
    {
    case BLUETOOTH_SVC_CONNECTION_EVENT_SPP_CONNECTED:
        companion_api_queue_link_message(COMPANION_API_MSG_LINK_CONNECTED);
        break;
    case BLUETOOTH_SVC_CONNECTION_EVENT_SPP_DISCONNECTED:
        companion_api_queue_link_message(COMPANION_API_MSG_LINK_DISCONNECTED);
        break;
    case BLUETOOTH_SVC_CONNECTION_EVENT_A2DP_CONNECTION_STATE:
    case BLUETOOTH_SVC_CONNECTION_EVENT_A2DP_AUDIO_STATE:
    case BLUETOOTH_SVC_CONNECTION_EVENT_AUTH_COMPLETE:
        companion_api_touch_generation();
        break;
    default:
        break;
    }
}

static void companion_api_hid_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_HID_BUTTON};
    (void)arg;
    (void)base;

    if (id != HID_EVENT_BUTTON_DOWN || !event_data || !s_msg_queue)
    {
        return;
    }

    msg.data.button = *(const hid_button_t *)event_data;
    (void)xQueueSend(s_msg_queue, &msg, 0);
}

static void companion_api_state_event_handler(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)event_data;
    companion_api_touch_generation();
}

static esp_err_t companion_api_queue_admin_message(companion_api_msg_t *msg)
{
    StaticSemaphore_t sem_storage;
    SemaphoreHandle_t sem;
    esp_err_t result = ESP_FAIL;

    if (!s_initialised || !s_msg_queue || !msg)
    {
        return ESP_ERR_INVALID_STATE;
    }

    sem = xSemaphoreCreateBinaryStatic(&sem_storage);
    if (!sem)
    {
        return ESP_ERR_NO_MEM;
    }

    msg->completion_semaphore = sem;
    msg->result_out = &result;
    if (xQueueSend(s_msg_queue, msg, pdMS_TO_TICKS(1000)) != pdPASS)
    {
        vSemaphoreDelete(sem);
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(sem, pdMS_TO_TICKS(5000)) != pdTRUE)
    {
        vSemaphoreDelete(sem);
        return ESP_ERR_TIMEOUT;
    }

    vSemaphoreDelete(sem);
    return result;
}

esp_err_t companion_api_service_init(void)
{
    esp_err_t err;

    if (s_initialised)
    {
        return ESP_OK;
    }

    s_rx_buffer = heap_caps_calloc(1, COMPANION_API_FRAME_MAX_LEN, COMPANION_API_PSRAM_ALLOC_CAPS);
    s_tx_frame = heap_caps_calloc(1, COMPANION_API_FRAME_MAX_LEN, COMPANION_API_PSRAM_ALLOC_CAPS);
    s_msg_queue = xQueueCreateWithCaps(COMPANION_API_QUEUE_DEPTH,
                                       sizeof(companion_api_msg_t),
                                       COMPANION_API_PSRAM_ALLOC_CAPS);
    if (!s_rx_buffer || !s_tx_frame || !s_msg_queue)
    {
        return ESP_ERR_NO_MEM;
    }

    s_task_stack = heap_caps_calloc(COMPANION_API_TASK_STACK_WORDS,
                                    sizeof(StackType_t),
                                    COMPANION_API_STACK_ALLOC_CAPS);
    if (!s_task_stack)
    {
        return ESP_ERR_NO_MEM;
    }

    s_trusted_client_count = companion_api_count_trusted_clients();

    s_task_handle = xTaskCreateStaticPinnedToCore(companion_api_task,
                                                  COMPANION_API_TASK_NAME,
                                                  COMPANION_API_TASK_STACK_WORDS,
                                                  NULL,
                                                  COMPANION_API_TASK_PRIORITY,
                                                  s_task_stack,
                                                  &s_task_tcb,
                                                  COMPANION_API_TASK_CORE);
    if (!s_task_handle)
    {
        return ESP_ERR_NO_MEM;
    }

    err = bluetooth_service_register_spp_rx_callback(companion_api_spp_rx_cb, NULL);
    if (err != ESP_OK)
    {
        return err;
    }
    bluetooth_service_register_connection_callback(companion_api_bt_connection_cb, NULL);

    ESP_ERROR_CHECK(esp_event_handler_register(HID_SERVICE_EVENT,
                                               HID_EVENT_BUTTON_DOWN,
                                               companion_api_hid_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(PLAYER_SERVICE_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               companion_api_state_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(CARTRIDGE_SERVICE_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               companion_api_state_event_handler,
                                               NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_SERVICE_EVENT,
                                               ESP_EVENT_ANY_ID,
                                               companion_api_state_event_handler,
                                               NULL));

    s_initialised = true;
    ESP_LOGI(TAG, "Companion API service started");
    return ESP_OK;
}

void companion_api_service_get_status(companion_api_status_t *status_out)
{
    if (!status_out)
    {
        return;
    }

    memset(status_out, 0, sizeof(*status_out));
    taskENTER_CRITICAL(&s_state_lock);
    status_out->initialised = s_initialised;
    status_out->authenticated = s_authenticated;
    status_out->pairing_pending = s_pairing_pending;
    status_out->pairing_progress = s_pairing_progress;
    status_out->pairing_required_count = 4;
    status_out->trusted_client_count = s_trusted_client_count;
    status_out->event_generation = s_event_generation;
    status_out->rx_frames = s_rx_frames;
    status_out->tx_frames = s_tx_frames;
    status_out->rx_errors = s_rx_errors;
    status_out->dropped_rx_chunks = s_dropped_rx_chunks;
    strncpy(status_out->active_client_id, s_active_client_id, sizeof(status_out->active_client_id) - 1);
    strncpy(status_out->pending_client_id, s_pending_client_id, sizeof(status_out->pending_client_id) - 1);
    strncpy(status_out->pending_app_name, s_pending_app_name, sizeof(status_out->pending_app_name) - 1);
    memcpy(status_out->pending_sequence, s_pending_sequence, sizeof(status_out->pending_sequence));
    taskEXIT_CRITICAL(&s_state_lock);

    status_out->spp_connected = bluetooth_service_is_spp_connected();
    status_out->spp_notifications_enabled = bluetooth_service_spp_notifications_enabled();
}

size_t companion_api_service_get_trusted_client_count(void)
{
    return companion_api_count_trusted_clients();
}

esp_err_t companion_api_service_get_trusted_client(size_t index,
                                                   companion_api_trusted_client_info_t *client_out)
{
    companion_api_trusted_record_t record;
    size_t visible_index = 0;

    if (!client_out)
    {
        return ESP_ERR_INVALID_ARG;
    }

    memset(client_out, 0, sizeof(*client_out));
    for (size_t slot = 0; slot < COMPANION_API_MAX_TRUSTED_CLIENTS; slot++)
    {
        if (companion_api_load_record(slot, &record) != ESP_OK)
        {
            continue;
        }
        if (visible_index == index)
        {
            client_out->valid = true;
            strncpy(client_out->client_id, record.client_id, sizeof(client_out->client_id) - 1);
            strncpy(client_out->app_name, record.app_name, sizeof(client_out->app_name) - 1);
            client_out->created_at_unix = record.created_at_unix;
            companion_api_zero_secret(record.secret, sizeof(record.secret));
            return ESP_OK;
        }
        companion_api_zero_secret(record.secret, sizeof(record.secret));
        visible_index++;
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t companion_api_service_console_confirm_pairing(void)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_CONSOLE_CONFIRM_PAIRING};
    return companion_api_queue_admin_message(&msg);
}

esp_err_t companion_api_service_console_cancel_pairing(void)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_CONSOLE_CANCEL_PAIRING};
    return companion_api_queue_admin_message(&msg);
}

esp_err_t companion_api_service_console_input_button(hid_button_t button)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_HID_BUTTON};
    if (button >= HID_BUTTON_COUNT)
    {
        return ESP_ERR_INVALID_ARG;
    }
    msg.data.button = button;
    return companion_api_queue_admin_message(&msg);
}

esp_err_t companion_api_service_revoke_client(const char *client_id)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_CONSOLE_REVOKE_CLIENT};
    if (!client_id || client_id[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(msg.data.client_id, client_id, sizeof(msg.data.client_id) - 1);
    return companion_api_queue_admin_message(&msg);
}

esp_err_t companion_api_service_revoke_all_clients(void)
{
    companion_api_msg_t msg = {.type = COMPANION_API_MSG_CONSOLE_REVOKE_ALL};
    return companion_api_queue_admin_message(&msg);
}