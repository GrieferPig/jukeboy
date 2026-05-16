#include "a2dp_coprocessor_service.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"

#include "pin_defs.h"
#include "power_mgmt_service.h"

#define A2DP_UART_NUM UART_NUM_1
#define A2DP_UART_BAUD 115200
#define A2DP_UART_RX_BUF_SIZE 2048
#define A2DP_UART_TX_BUF_SIZE 2048
#define A2DP_UART_MAX_PAYLOAD 256
#define A2DP_UART_TASK_STACK_SIZE 4096
#define A2DP_UART_TASK_PRIORITY 5
#define A2DP_UART_READ_CHUNK 128
#define A2DP_COMMAND_TIMEOUT_MS 1000
#define A2DP_SYNC_0 0xA2
#define A2DP_SYNC_1 0xD2
#define A2DP_HEADER_LEN 4
#define A2DP_CRC_LEN 2
#define A2DP_EN_PULSE_LOW_MS 50
#define A2DP_BOOT_WAIT_MS 1200
#define A2DP_SHUTDOWN_PRIORITY (POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_BLUETOOTH + 10)

typedef enum
{
    UART_CMD_GET_STATUS = 0x01,
    UART_CMD_START_DISCOVERY = 0x02,
    UART_CMD_PAIR_BEST = 0x03,
    UART_CMD_CONNECT_LAST = 0x04,
    UART_CMD_CONNECT_ADDR = 0x05,
    UART_CMD_DISCONNECT = 0x06,
    UART_CMD_PAIR_CONFIRM = 0x07,
    UART_CMD_AUDIO_START = 0x08,
    UART_CMD_AUDIO_SUSPEND = 0x09,
    UART_CMD_MEDIA_KEY = 0x0A,
    UART_CMD_SET_VOLUME = 0x0B,
    UART_CMD_GET_METADATA = 0x0C,
    UART_CMD_GET_PLAY_STATUS = 0x0D,
    UART_CMD_REGISTER_NOTIFICATION = 0x0E,
    UART_CMD_LIST_BONDED = 0x0F,
    UART_CMD_UNBOND = 0x10,
    UART_CMD_GET_DISCOVERY_RESULTS = 0x11,
    UART_CMD_SHUTDOWN = 0x12,
} a2dp_uart_command_t;

typedef enum
{
    UART_EVT_ACK = 0x80,
    UART_EVT_STATUS = 0x81,
    UART_EVT_DISCOVERY_RESULT = 0x82,
    UART_EVT_DISCOVERY_DONE = 0x83,
    UART_EVT_PAIRING_CONFIRM = 0x84,
    UART_EVT_AUTH_COMPLETE = 0x85,
    UART_EVT_A2DP_CONNECTION = 0x86,
    UART_EVT_A2DP_AUDIO = 0x87,
    UART_EVT_AVRCP_PASSTHROUGH = 0x88,
    UART_EVT_AVRCP_REMOTE_COMMAND = 0x89,
    UART_EVT_AVRCP_VOLUME = 0x8A,
    UART_EVT_AVRCP_METADATA = 0x8B,
    UART_EVT_AVRCP_PLAY_STATUS = 0x8C,
    UART_EVT_AVRCP_NOTIFICATION = 0x8D,
    UART_EVT_BONDED_LIST = 0x8E,
    UART_EVT_DISCOVERY_LIST = 0x8F,
    UART_EVT_ERROR = 0xFF,
} a2dp_uart_event_t;

typedef enum
{
    PARSER_SYNC_0,
    PARSER_SYNC_1,
    PARSER_HEADER,
    PARSER_PAYLOAD,
    PARSER_CRC_0,
    PARSER_CRC_1,
} parser_state_t;

typedef struct
{
    parser_state_t state;
    uint8_t header[A2DP_HEADER_LEN];
    uint8_t payload[A2DP_UART_MAX_PAYLOAD];
    size_t header_pos;
    size_t payload_pos;
    uint8_t type;
    uint8_t seq;
    uint16_t len;
    uint16_t rx_crc;
} a2dp_uart_parser_t;

static const char *TAG = "a2dp_coproc";

static a2dp_uart_parser_t s_parser;
static TaskHandle_t s_task_handle;
static SemaphoreHandle_t s_command_mutex;
static SemaphoreHandle_t s_ack_sem;
static SemaphoreHandle_t s_response_sem;
static bool s_initialised;
static bool s_en_pin_configured;
static bool s_coprocessor_awake;
static uint8_t s_next_seq = 1;
static a2dp_coprocessor_status_t s_status;
static a2dp_coprocessor_pairing_confirm_t s_pairing_confirm;
static a2dp_coprocessor_bonded_device_t s_bonded_devices[A2DP_COPROCESSOR_MAX_LIST_ITEMS];
static size_t s_bonded_count;
static a2dp_coprocessor_scan_entry_t s_discovery_entries[A2DP_COPROCESSOR_MAX_LIST_ITEMS];
static size_t s_discovery_count;
static a2dp_coprocessor_event_cb_t s_event_cb;
static void *s_event_cb_ctx;
static bool s_ack_pending;
static uint8_t s_ack_seq;
static uint8_t s_ack_command;
static esp_err_t s_ack_status;
static bool s_response_pending;
static uint8_t s_response_seq;
static uint8_t s_response_type;
static esp_err_t s_response_status;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static uint16_t crc16_update(uint16_t crc, uint8_t data)
{
    crc ^= (uint16_t)data << 8;
    for (int bit = 0; bit < 8; bit++)
    {
        crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t crc16_frame(uint8_t type, uint8_t seq, uint16_t len, const uint8_t *payload)
{
    uint16_t crc = 0xFFFF;
    crc = crc16_update(crc, type);
    crc = crc16_update(crc, seq);
    crc = crc16_update(crc, (uint8_t)(len & 0xFF));
    crc = crc16_update(crc, (uint8_t)(len >> 8));
    for (uint16_t index = 0; index < len; index++)
    {
        crc = crc16_update(crc, payload[index]);
    }
    return crc;
}

static void put_u16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

static void a2dp_emit(a2dp_coprocessor_event_t event, const a2dp_coprocessor_event_data_t *data)
{
    a2dp_coprocessor_event_cb_t callback;
    void *ctx;

    taskENTER_CRITICAL(&s_state_lock);
    callback = s_event_cb;
    ctx = s_event_cb_ctx;
    taskEXIT_CRITICAL(&s_state_lock);

    if (callback)
    {
        callback(event, data, ctx);
    }
}

static uint8_t next_seq(void)
{
    uint8_t seq;

    taskENTER_CRITICAL(&s_state_lock);
    seq = s_next_seq++;
    if (s_next_seq == 0)
    {
        s_next_seq = 1;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    return seq == 0 ? 1 : seq;
}

static void drain_semaphore(SemaphoreHandle_t sem)
{
    while (sem && xSemaphoreTake(sem, 0) == pdTRUE)
    {
    }
}

static bool a2dp_en_pin_available(void)
{
    return HAL_A2DP_EN_PIN >= 0 && HAL_A2DP_EN_PIN < GPIO_NUM_MAX;
}

static uint64_t a2dp_en_pin_mask(void)
{
    return a2dp_en_pin_available() ? (1ULL << (uint32_t)HAL_A2DP_EN_PIN) : 0;
}

static void a2dp_mark_awake(bool awake)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_coprocessor_awake = awake;
    if (!awake)
    {
        s_status.coprocessor_seen = false;
        s_status.a2dp_connected = false;
        s_status.i2s_running = false;
        s_status.discovery_running = false;
        memset(s_status.connected_bda, 0, sizeof(s_status.connected_bda));
    }
    taskEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t a2dp_configure_en_pin(void)
{
    if (!a2dp_en_pin_available())
    {
        return ESP_OK;
    }
    if (s_en_pin_configured)
    {
        return ESP_OK;
    }

    gpio_config_t cfg = {
        .pin_bit_mask = a2dp_en_pin_mask(),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    err = gpio_set_level(HAL_A2DP_EN_PIN, 1);
    if (err == ESP_OK)
    {
        s_en_pin_configured = true;
    }
    return err;
}

static esp_err_t a2dp_pulse_en_pin(void)
{
    esp_err_t err = a2dp_configure_en_pin();
    if (err != ESP_OK || !a2dp_en_pin_available())
    {
        return err;
    }

    ESP_LOGI(TAG, "coprocessor not responding, pulsing EN GPIO %d", HAL_A2DP_EN_PIN);
    err = gpio_set_level(HAL_A2DP_EN_PIN, 0);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(A2DP_EN_PULSE_LOW_MS));

    err = gpio_set_level(HAL_A2DP_EN_PIN, 1);
    if (err != ESP_OK)
    {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(A2DP_BOOT_WAIT_MS));
    return ESP_OK;
}

static esp_err_t uart_send_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint8_t prefix[2 + A2DP_HEADER_LEN];
    uint8_t crc_bytes[A2DP_CRC_LEN];
    uint16_t crc;

    if (len > 0 && !payload)
    {
        return ESP_ERR_INVALID_ARG;
    }

    prefix[0] = A2DP_SYNC_0;
    prefix[1] = A2DP_SYNC_1;
    prefix[2] = type;
    prefix[3] = seq;
    put_u16(&prefix[4], len);
    crc = crc16_frame(type, seq, len, payload);
    put_u16(crc_bytes, crc);

    if (uart_write_bytes(A2DP_UART_NUM, prefix, sizeof(prefix)) != (int)sizeof(prefix))
    {
        return ESP_FAIL;
    }
    if (len > 0 && uart_write_bytes(A2DP_UART_NUM, payload, len) != (int)len)
    {
        return ESP_FAIL;
    }
    if (uart_write_bytes(A2DP_UART_NUM, crc_bytes, sizeof(crc_bytes)) != (int)sizeof(crc_bytes))
    {
        return ESP_FAIL;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_status.tx_frames++;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static void finish_response(uint8_t seq, uint8_t type, esp_err_t status)
{
    bool matched = false;

    taskENTER_CRITICAL(&s_state_lock);
    if (s_response_pending && s_response_seq == seq && s_response_type == type)
    {
        s_response_status = status;
        s_response_pending = false;
        matched = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (matched)
    {
        xSemaphoreGive(s_response_sem);
    }
}

static void finish_ack(uint8_t seq, uint8_t command, esp_err_t status)
{
    bool matched = false;

    taskENTER_CRITICAL(&s_state_lock);
    s_status.last_command = command;
    s_status.last_command_status = status;
    if (s_ack_pending && s_ack_seq == seq)
    {
        s_ack_command = command;
        s_ack_status = status;
        s_ack_pending = false;
        matched = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (matched)
    {
        xSemaphoreGive(s_ack_sem);
    }
}

static esp_err_t send_command(uint8_t command,
                              const uint8_t *payload,
                              uint16_t len,
                              uint8_t response_type)
{
    esp_err_t err;
    uint8_t seq;
    TickType_t timeout = pdMS_TO_TICKS(A2DP_COMMAND_TIMEOUT_MS);

    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (len > 0 && !payload)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_command_mutex, timeout) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    seq = next_seq();
    drain_semaphore(s_ack_sem);
    drain_semaphore(s_response_sem);

    taskENTER_CRITICAL(&s_state_lock);
    s_ack_pending = true;
    s_ack_seq = seq;
    s_ack_command = command;
    s_ack_status = ESP_ERR_TIMEOUT;
    if (response_type != 0)
    {
        s_response_pending = true;
        s_response_seq = seq;
        s_response_type = response_type;
        s_response_status = ESP_ERR_TIMEOUT;
    }
    else
    {
        s_response_pending = false;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    err = uart_send_frame(command, seq, payload, len);
    if (err == ESP_OK && response_type != 0)
    {
        if (xSemaphoreTake(s_response_sem, timeout) != pdTRUE)
        {
            err = ESP_ERR_TIMEOUT;
        }
        else
        {
            taskENTER_CRITICAL(&s_state_lock);
            err = s_response_status;
            taskEXIT_CRITICAL(&s_state_lock);
        }
    }
    if (err == ESP_OK)
    {
        if (xSemaphoreTake(s_ack_sem, timeout) != pdTRUE)
        {
            err = ESP_ERR_TIMEOUT;
        }
        else
        {
            taskENTER_CRITICAL(&s_state_lock);
            err = s_ack_status;
            taskEXIT_CRITICAL(&s_state_lock);
        }
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_ack_pending = false;
    s_response_pending = false;
    if (err == ESP_ERR_TIMEOUT)
    {
        s_status.command_timeouts++;
        s_status.last_command = command;
        s_status.last_command_status = err;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    xSemaphoreGive(s_command_mutex);
    return err;
}

static a2dp_coprocessor_scan_entry_t *find_discovery_entry(const uint8_t *bda)
{
    for (size_t index = 0; index < s_discovery_count; index++)
    {
        if (memcmp(s_discovery_entries[index].bda, bda, A2DP_COPROCESSOR_ADDR_LEN) == 0)
        {
            return &s_discovery_entries[index];
        }
    }

    if (s_discovery_count >= A2DP_COPROCESSOR_MAX_LIST_ITEMS)
    {
        return NULL;
    }

    a2dp_coprocessor_scan_entry_t *entry = &s_discovery_entries[s_discovery_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->bda, bda, A2DP_COPROCESSOR_ADDR_LEN);
    return entry;
}

static void handle_status(uint8_t seq, const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 28)
    {
        finish_response(seq, UART_EVT_STATUS, ESP_ERR_INVALID_SIZE);
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_coprocessor_awake = true;
    s_status.coprocessor_seen = true;
    s_status.initialised = (payload[0] & 0x01) != 0;
    s_status.discovery_running = (payload[0] & 0x02) != 0;
    s_status.a2dp_connected = (payload[0] & 0x04) != 0;
    s_status.i2s_running = (payload[0] & 0x08) != 0;
    memcpy(s_status.connected_bda, &payload[1], A2DP_COPROCESSOR_ADDR_LEN);
    s_status.audio_state = payload[7];
    s_status.local_volume = payload[8];
    s_status.buffered_bytes = get_u32(&payload[9]);
    s_status.capacity_bytes = get_u32(&payload[13]);
    s_status.underrun_count = get_u32(&payload[17]);
    s_status.overrun_count = get_u32(&payload[21]);
    s_status.sample_rate_hz = (uint32_t)payload[25] * 1000U;
    s_status.bits_per_sample = payload[26];
    s_status.channel_count = payload[27];
    event_data.data.status = s_status;
    taskEXIT_CRITICAL(&s_state_lock);

    finish_response(seq, UART_EVT_STATUS, ESP_OK);
    a2dp_emit(A2DP_COPROCESSOR_EVENT_STATUS, &event_data);
}

static void handle_discovery_result(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};
    bool emit = false;

    if (len < 12)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    a2dp_coprocessor_scan_entry_t *entry = find_discovery_entry(payload);
    if (entry)
    {
        size_t name_len = payload[11];
        if (name_len > len - 12)
        {
            name_len = len - 12;
        }
        if (name_len > A2DP_COPROCESSOR_MAX_DEVICE_NAME_LEN)
        {
            name_len = A2DP_COPROCESSOR_MAX_DEVICE_NAME_LEN;
        }
        memcpy(entry->bda, payload, A2DP_COPROCESSOR_ADDR_LEN);
        entry->rssi = (int8_t)payload[6];
        entry->cod = get_u32(&payload[7]);
        memcpy(entry->name, &payload[12], name_len);
        entry->name[name_len] = '\0';
        event_data.data.discovery_result = *entry;
        emit = true;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    if (emit)
    {
        a2dp_emit(A2DP_COPROCESSOR_EVENT_DISCOVERY_RESULT, &event_data);
    }
}

static void handle_addr_list(uint8_t seq, uint8_t type, const uint8_t *payload, uint16_t len)
{
    size_t count;
    esp_err_t status = ESP_OK;
    bool extended_bonded_list = false;

    if (len < 1)
    {
        finish_response(seq, type, ESP_ERR_INVALID_SIZE);
        return;
    }

    count = payload[0];
    if (count > A2DP_COPROCESSOR_MAX_LIST_ITEMS)
    {
        count = A2DP_COPROCESSOR_MAX_LIST_ITEMS;
    }
    if (type == UART_EVT_BONDED_LIST)
    {
        extended_bonded_list = len != 1 + count * A2DP_COPROCESSOR_ADDR_LEN;
    }

    if (!extended_bonded_list && len < 1 + count * A2DP_COPROCESSOR_ADDR_LEN)
    {
        status = ESP_ERR_INVALID_SIZE;
        count = 0;
    }

    taskENTER_CRITICAL(&s_state_lock);
    if (type == UART_EVT_BONDED_LIST)
    {
        size_t parsed_count = 0;
        memset(s_bonded_devices, 0, sizeof(s_bonded_devices));

        if (status == ESP_OK && extended_bonded_list)
        {
            size_t offset = 1;
            for (size_t index = 0; index < count; index++)
            {
                if (offset + A2DP_COPROCESSOR_ADDR_LEN + 1 > len)
                {
                    status = ESP_ERR_INVALID_SIZE;
                    parsed_count = 0;
                    memset(s_bonded_devices, 0, sizeof(s_bonded_devices));
                    break;
                }

                memcpy(s_bonded_devices[parsed_count].bda, &payload[offset], A2DP_COPROCESSOR_ADDR_LEN);
                offset += A2DP_COPROCESSOR_ADDR_LEN;

                uint8_t name_len = payload[offset++];
                if (offset + name_len > len)
                {
                    status = ESP_ERR_INVALID_SIZE;
                    parsed_count = 0;
                    memset(s_bonded_devices, 0, sizeof(s_bonded_devices));
                    break;
                }

                size_t copy_len = name_len;
                if (copy_len > A2DP_COPROCESSOR_MAX_DEVICE_NAME_LEN)
                {
                    copy_len = A2DP_COPROCESSOR_MAX_DEVICE_NAME_LEN;
                }
                if (copy_len > 0)
                {
                    memcpy(s_bonded_devices[parsed_count].name, &payload[offset], copy_len);
                    s_bonded_devices[parsed_count].name[copy_len] = '\0';
                }
                offset += name_len;
                parsed_count++;
            }
        }
        else if (status == ESP_OK)
        {
            for (size_t index = 0; index < count; index++)
            {
                memcpy(s_bonded_devices[index].bda, &payload[1 + index * A2DP_COPROCESSOR_ADDR_LEN], A2DP_COPROCESSOR_ADDR_LEN);
            }
            parsed_count = count;
        }
        s_bonded_count = status == ESP_OK ? parsed_count : 0;
    }
    else
    {
        for (size_t index = 0; index < count; index++)
        {
            (void)find_discovery_entry(&payload[1 + index * A2DP_COPROCESSOR_ADDR_LEN]);
        }
    }
    taskEXIT_CRITICAL(&s_state_lock);

    finish_response(seq, type, status);
    a2dp_emit(type == UART_EVT_BONDED_LIST ? A2DP_COPROCESSOR_EVENT_BONDED_LIST : A2DP_COPROCESSOR_EVENT_DISCOVERY_LIST, NULL);
}

static void handle_pairing_confirm(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 10)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_pairing_confirm.pending = true;
    memcpy(s_pairing_confirm.remote_bda, payload, A2DP_COPROCESSOR_ADDR_LEN);
    s_pairing_confirm.numeric_value = get_u32(&payload[6]);
    event_data.data.pairing_confirm = s_pairing_confirm;
    taskEXIT_CRITICAL(&s_state_lock);

    a2dp_emit(A2DP_COPROCESSOR_EVENT_PAIRING_CONFIRM, &event_data);
}

static void handle_auth_complete(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 7)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_pairing_confirm.pending = false;
    memcpy(event_data.data.auth.bda, payload, A2DP_COPROCESSOR_ADDR_LEN);
    event_data.data.auth.status = payload[6];
    taskEXIT_CRITICAL(&s_state_lock);

    a2dp_emit(A2DP_COPROCESSOR_EVENT_AUTH_COMPLETE, &event_data);
}

static void handle_a2dp_connection(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 7)
    {
        return;
    }
    event_data.data.a2dp_connection.state = payload[0];
    memcpy(event_data.data.a2dp_connection.remote_bda, &payload[1], A2DP_COPROCESSOR_ADDR_LEN);

    taskENTER_CRITICAL(&s_state_lock);
    s_status.a2dp_connected = payload[0] == A2DP_COPROCESSOR_CONNECTION_STATE_CONNECTED;
    if (s_status.a2dp_connected)
    {
        memcpy(s_status.connected_bda, &payload[1], A2DP_COPROCESSOR_ADDR_LEN);
    }
    else
    {
        memset(s_status.connected_bda, 0, sizeof(s_status.connected_bda));
        s_status.i2s_running = false;
    }
    taskEXIT_CRITICAL(&s_state_lock);

    a2dp_emit(A2DP_COPROCESSOR_EVENT_A2DP_CONNECTION_STATE, &event_data);
}

static void handle_a2dp_audio(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 7)
    {
        return;
    }
    event_data.data.a2dp_audio.state = payload[0];
    memcpy(event_data.data.a2dp_audio.remote_bda, &payload[1], A2DP_COPROCESSOR_ADDR_LEN);

    taskENTER_CRITICAL(&s_state_lock);
    s_status.audio_state = payload[0];
    taskEXIT_CRITICAL(&s_state_lock);

    a2dp_emit(A2DP_COPROCESSOR_EVENT_A2DP_AUDIO_STATE, &event_data);
}

static void handle_avrcp_passthrough(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 3)
    {
        return;
    }
    event_data.data.avrc_passthrough.key_code = payload[0];
    event_data.data.avrc_passthrough.key_state = payload[1];
    event_data.data.avrc_passthrough.rsp_code = payload[2];
    a2dp_emit(A2DP_COPROCESSOR_EVENT_AVRCP_PASSTHROUGH_RSP, &event_data);
}

static void handle_avrcp_remote_command(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 2)
    {
        return;
    }
    event_data.data.avrc_remote_command.key_code = payload[0];
    event_data.data.avrc_remote_command.key_state = payload[1];
    a2dp_emit(A2DP_COPROCESSOR_EVENT_AVRCP_REMOTE_COMMAND, &event_data);
}

static void handle_avrcp_volume(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 2)
    {
        return;
    }
    event_data.data.avrc_volume.volume = payload[0];
    event_data.data.avrc_volume.from_remote_target = payload[1] != 0;
    taskENTER_CRITICAL(&s_state_lock);
    s_status.local_volume = payload[0];
    taskEXIT_CRITICAL(&s_state_lock);
    a2dp_emit(A2DP_COPROCESSOR_EVENT_AVRCP_VOLUME, &event_data);
}

static void handle_avrcp_metadata(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};
    size_t text_len;

    if (len < 2)
    {
        return;
    }
    event_data.data.avrc_metadata.attr_id = payload[0];
    text_len = payload[1];
    if (text_len > len - 2)
    {
        text_len = len - 2;
    }
    if (text_len > sizeof(event_data.data.avrc_metadata.text) - 1)
    {
        text_len = sizeof(event_data.data.avrc_metadata.text) - 1;
    }
    memcpy(event_data.data.avrc_metadata.text, &payload[2], text_len);
    event_data.data.avrc_metadata.text[text_len] = '\0';
    a2dp_emit(A2DP_COPROCESSOR_EVENT_AVRCP_METADATA, &event_data);
}

static void handle_avrcp_play_status(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 9)
    {
        return;
    }
    event_data.data.avrc_play_status.song_length = get_u32(&payload[0]);
    event_data.data.avrc_play_status.song_position = get_u32(&payload[4]);
    event_data.data.avrc_play_status.play_status = payload[8];
    a2dp_emit(A2DP_COPROCESSOR_EVENT_AVRCP_PLAY_STATUS, &event_data);
}

static void handle_avrcp_notification(const uint8_t *payload, uint16_t len)
{
    a2dp_coprocessor_event_data_t event_data = {0};

    if (len < 1)
    {
        return;
    }
    event_data.data.avrc_notification.event_id = payload[0];
    event_data.data.avrc_notification.param_len = len - 1;
    if (event_data.data.avrc_notification.param_len > sizeof(event_data.data.avrc_notification.param))
    {
        event_data.data.avrc_notification.param_len = sizeof(event_data.data.avrc_notification.param);
    }
    memcpy(event_data.data.avrc_notification.param, &payload[1], event_data.data.avrc_notification.param_len);
    a2dp_emit(A2DP_COPROCESSOR_EVENT_AVRCP_NOTIFICATION, &event_data);
}

static void handle_frame(uint8_t type, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_coprocessor_awake = true;
    s_status.coprocessor_seen = true;
    s_status.rx_frames++;
    taskEXIT_CRITICAL(&s_state_lock);

    switch ((a2dp_uart_event_t)type)
    {
    case UART_EVT_ACK:
        if (len >= 5)
        {
            finish_ack(seq, payload[0], (esp_err_t)get_u32(&payload[1]));
        }
        else
        {
            finish_ack(seq, 0, ESP_ERR_INVALID_SIZE);
        }
        break;
    case UART_EVT_STATUS:
        handle_status(seq, payload, len);
        break;
    case UART_EVT_DISCOVERY_RESULT:
        handle_discovery_result(payload, len);
        break;
    case UART_EVT_DISCOVERY_DONE:
        taskENTER_CRITICAL(&s_state_lock);
        s_status.discovery_running = false;
        taskEXIT_CRITICAL(&s_state_lock);
        a2dp_emit(A2DP_COPROCESSOR_EVENT_DISCOVERY_DONE, NULL);
        break;
    case UART_EVT_PAIRING_CONFIRM:
        handle_pairing_confirm(payload, len);
        break;
    case UART_EVT_AUTH_COMPLETE:
        handle_auth_complete(payload, len);
        break;
    case UART_EVT_A2DP_CONNECTION:
        handle_a2dp_connection(payload, len);
        break;
    case UART_EVT_A2DP_AUDIO:
        handle_a2dp_audio(payload, len);
        break;
    case UART_EVT_AVRCP_PASSTHROUGH:
        handle_avrcp_passthrough(payload, len);
        break;
    case UART_EVT_AVRCP_REMOTE_COMMAND:
        handle_avrcp_remote_command(payload, len);
        break;
    case UART_EVT_AVRCP_VOLUME:
        handle_avrcp_volume(payload, len);
        break;
    case UART_EVT_AVRCP_METADATA:
        handle_avrcp_metadata(payload, len);
        break;
    case UART_EVT_AVRCP_PLAY_STATUS:
        handle_avrcp_play_status(payload, len);
        break;
    case UART_EVT_AVRCP_NOTIFICATION:
        handle_avrcp_notification(payload, len);
        break;
    case UART_EVT_BONDED_LIST:
    case UART_EVT_DISCOVERY_LIST:
        handle_addr_list(seq, type, payload, len);
        break;
    case UART_EVT_ERROR:
    default:
        break;
    }
}

static void parser_reset(a2dp_uart_parser_t *parser)
{
    parser->state = PARSER_SYNC_0;
    parser->header_pos = 0;
    parser->payload_pos = 0;
    parser->len = 0;
    parser->rx_crc = 0;
}

static void parser_feed(a2dp_uart_parser_t *parser, uint8_t byte)
{
    switch (parser->state)
    {
    case PARSER_SYNC_0:
        if (byte == A2DP_SYNC_0)
        {
            parser->state = PARSER_SYNC_1;
        }
        break;
    case PARSER_SYNC_1:
        parser->state = byte == A2DP_SYNC_1 ? PARSER_HEADER : PARSER_SYNC_0;
        parser->header_pos = 0;
        break;
    case PARSER_HEADER:
        parser->header[parser->header_pos++] = byte;
        if (parser->header_pos == A2DP_HEADER_LEN)
        {
            parser->type = parser->header[0];
            parser->seq = parser->header[1];
            parser->len = (uint16_t)parser->header[2] | ((uint16_t)parser->header[3] << 8);
            if (parser->len > A2DP_UART_MAX_PAYLOAD)
            {
                taskENTER_CRITICAL(&s_state_lock);
                s_status.rx_errors++;
                taskEXIT_CRITICAL(&s_state_lock);
                parser_reset(parser);
            }
            else
            {
                parser->payload_pos = 0;
                parser->state = parser->len == 0 ? PARSER_CRC_0 : PARSER_PAYLOAD;
            }
        }
        break;
    case PARSER_PAYLOAD:
        parser->payload[parser->payload_pos++] = byte;
        if (parser->payload_pos == parser->len)
        {
            parser->state = PARSER_CRC_0;
        }
        break;
    case PARSER_CRC_0:
        parser->rx_crc = byte;
        parser->state = PARSER_CRC_1;
        break;
    case PARSER_CRC_1:
    {
        parser->rx_crc |= (uint16_t)byte << 8;
        uint16_t expected = crc16_frame(parser->type, parser->seq, parser->len, parser->payload);
        if (expected == parser->rx_crc)
        {
            handle_frame(parser->type, parser->seq, parser->payload, parser->len);
        }
        else
        {
            taskENTER_CRITICAL(&s_state_lock);
            s_status.crc_errors++;
            taskEXIT_CRITICAL(&s_state_lock);
        }
        parser_reset(parser);
        break;
    }
    default:
        parser_reset(parser);
        break;
    }
}

static void uart_task(void *param)
{
    (void)param;
    uint8_t buffer[A2DP_UART_READ_CHUNK];
    parser_reset(&s_parser);

    for (;;)
    {
        int bytes = uart_read_bytes(A2DP_UART_NUM,
                                    buffer,
                                    sizeof(buffer),
                                    pdMS_TO_TICKS(100));
        for (int index = 0; index < bytes; index++)
        {
            parser_feed(&s_parser, buffer[index]);
        }
    }
}

static esp_err_t a2dp_coprocessor_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;
    return a2dp_coprocessor_service_shutdown();
}

esp_err_t a2dp_coprocessor_service_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = A2DP_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err;

    if (s_initialised)
    {
        return ESP_OK;
    }

    s_command_mutex = xSemaphoreCreateMutex();
    s_ack_sem = xSemaphoreCreateBinary();
    s_response_sem = xSemaphoreCreateBinary();
    if (!s_command_mutex || !s_ack_sem || !s_response_sem)
    {
        return ESP_ERR_NO_MEM;
    }

    err = a2dp_configure_en_pin();
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_driver_install(A2DP_UART_NUM,
                              A2DP_UART_RX_BUF_SIZE,
                              A2DP_UART_TX_BUF_SIZE,
                              0,
                              NULL,
                              0);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_param_config(A2DP_UART_NUM, &uart_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = uart_set_pin(A2DP_UART_NUM,
                       HAL_A2DP_UART_TX_PIN,
                       HAL_A2DP_UART_RX_PIN,
                       UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE);
    if (err != ESP_OK)
    {
        return err;
    }

    if (xTaskCreatePinnedToCore(uart_task,
                                "a2dp_uart",
                                A2DP_UART_TASK_STACK_SIZE,
                                NULL,
                                A2DP_UART_TASK_PRIORITY,
                                &s_task_handle,
                                tskNO_AFFINITY) != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    taskENTER_CRITICAL(&s_state_lock);
    s_initialised = true;
    memset(&s_status, 0, sizeof(s_status));
    s_status.sample_rate_hz = 48000;
    s_status.bits_per_sample = 16;
    s_status.channel_count = 2;
    taskEXIT_CRITICAL(&s_state_lock);

    ESP_LOGI(TAG,
             "A2DP coprocessor UART ready: uart=%d baud=%d tx=%d rx=%d en=%d",
             A2DP_UART_NUM,
             A2DP_UART_BAUD,
             HAL_A2DP_UART_TX_PIN,
             HAL_A2DP_UART_RX_PIN,
             HAL_A2DP_EN_PIN);
    err = power_mgmt_service_register_shutdown_callback(a2dp_coprocessor_shutdown_callback,
                                                        NULL,
                                                        A2DP_SHUTDOWN_PRIORITY);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }
    a2dp_emit(A2DP_COPROCESSOR_EVENT_STARTED, NULL);
    return ESP_OK;
}

void a2dp_coprocessor_service_process_once(void)
{
}

void a2dp_coprocessor_service_register_event_callback(a2dp_coprocessor_event_cb_t callback, void *user_ctx)
{
    taskENTER_CRITICAL(&s_state_lock);
    s_event_cb = callback;
    s_event_cb_ctx = user_ctx;
    taskEXIT_CRITICAL(&s_state_lock);
}

esp_err_t a2dp_coprocessor_service_refresh_status(void)
{
    return send_command(UART_CMD_GET_STATUS, NULL, 0, UART_EVT_STATUS);
}

void a2dp_coprocessor_service_get_status(a2dp_coprocessor_status_t *status_out)
{
    if (!status_out)
    {
        return;
    }

    taskENTER_CRITICAL(&s_state_lock);
    *status_out = s_status;
    taskEXIT_CRITICAL(&s_state_lock);
}

bool a2dp_coprocessor_service_is_initialised(void)
{
    return s_initialised;
}

bool a2dp_coprocessor_service_is_awake(void)
{
    bool awake;

    taskENTER_CRITICAL(&s_state_lock);
    awake = s_coprocessor_awake;
    taskEXIT_CRITICAL(&s_state_lock);
    return awake;
}

bool a2dp_coprocessor_service_is_a2dp_connected(void)
{
    bool connected;

    taskENTER_CRITICAL(&s_state_lock);
    connected = s_status.a2dp_connected;
    taskEXIT_CRITICAL(&s_state_lock);
    return connected;
}

bool a2dp_coprocessor_service_is_discovery_running(void)
{
    bool running;

    taskENTER_CRITICAL(&s_state_lock);
    running = s_status.discovery_running;
    taskEXIT_CRITICAL(&s_state_lock);
    return running;
}

esp_err_t a2dp_coprocessor_service_wake_for_request(void)
{
    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = a2dp_coprocessor_service_refresh_status();
    if (err == ESP_OK)
    {
        a2dp_mark_awake(true);
        return ESP_OK;
    }

    a2dp_mark_awake(false);

    err = a2dp_pulse_en_pin();
    if (err != ESP_OK)
    {
        return err;
    }

    err = a2dp_coprocessor_service_refresh_status();
    if (err == ESP_OK)
    {
        a2dp_mark_awake(true);
    }
    else
    {
        a2dp_mark_awake(false);
    }
    return err;
}

esp_err_t a2dp_coprocessor_service_shutdown(void)
{
    if (!s_initialised)
    {
        return ESP_OK;
    }

    esp_err_t err = a2dp_coprocessor_service_refresh_status();
    if (err != ESP_OK)
    {
        a2dp_mark_awake(false);
        return ESP_OK;
    }

    err = send_command(UART_CMD_SHUTDOWN, NULL, 0, 0);
    if (err == ESP_OK || err == ESP_ERR_TIMEOUT)
    {
        a2dp_mark_awake(false);
        return ESP_OK;
    }
    return err;
}

static esp_err_t send_request_command(uint8_t command,
                                      const uint8_t *payload,
                                      uint16_t len,
                                      uint8_t response_type)
{
    esp_err_t err = a2dp_coprocessor_service_wake_for_request();
    if (err != ESP_OK)
    {
        return err;
    }
    return send_command(command, payload, len, response_type);
}

static esp_err_t send_awake_only_command(uint8_t command,
                                         const uint8_t *payload,
                                         uint16_t len,
                                         uint8_t response_type)
{
    esp_err_t err = a2dp_coprocessor_service_refresh_status();
    if (err != ESP_OK)
    {
        a2dp_mark_awake(false);
        return ESP_OK;
    }
    return send_command(command, payload, len, response_type);
}

esp_err_t a2dp_coprocessor_service_pair_best_a2dp_sink(void)
{
    esp_err_t err;

    taskENTER_CRITICAL(&s_state_lock);
    s_discovery_count = 0;
    memset(s_discovery_entries, 0, sizeof(s_discovery_entries));
    taskEXIT_CRITICAL(&s_state_lock);

    err = send_request_command(UART_CMD_PAIR_BEST, NULL, 0, 0);
    if (err == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_status.discovery_running = true;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return err;
}

esp_err_t a2dp_coprocessor_service_start_discovery(void)
{
    esp_err_t err;

    taskENTER_CRITICAL(&s_state_lock);
    s_discovery_count = 0;
    memset(s_discovery_entries, 0, sizeof(s_discovery_entries));
    taskEXIT_CRITICAL(&s_state_lock);

    err = send_request_command(UART_CMD_START_DISCOVERY, NULL, 0, 0);
    if (err == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_status.discovery_running = true;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return err;
}

esp_err_t a2dp_coprocessor_service_connect_last_bonded_a2dp_device(void)
{
    return send_request_command(UART_CMD_CONNECT_LAST, NULL, 0, 0);
}

esp_err_t a2dp_coprocessor_service_connect_a2dp(const a2dp_coprocessor_addr_t remote_bda)
{
    if (!remote_bda)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return send_request_command(UART_CMD_CONNECT_ADDR, remote_bda, A2DP_COPROCESSOR_ADDR_LEN, 0);
}

esp_err_t a2dp_coprocessor_service_disconnect_a2dp(void)
{
    return send_awake_only_command(UART_CMD_DISCONNECT, NULL, 0, 0);
}

esp_err_t a2dp_coprocessor_service_get_pending_pairing_confirm(a2dp_coprocessor_pairing_confirm_t *confirm)
{
    if (!confirm)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    *confirm = s_pairing_confirm;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t a2dp_coprocessor_service_reply_pairing_confirm(bool accept)
{
    uint8_t payload = accept ? 1 : 0;
    esp_err_t err = send_request_command(UART_CMD_PAIR_CONFIRM, &payload, sizeof(payload), 0);
    if (err == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_pairing_confirm.pending = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return err;
}

esp_err_t a2dp_coprocessor_service_start_audio(void)
{
    esp_err_t err = send_request_command(UART_CMD_AUDIO_START, NULL, 0, 0);
    if (err == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_status.i2s_running = true;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return err;
}

esp_err_t a2dp_coprocessor_service_suspend_audio(void)
{
    esp_err_t err = send_awake_only_command(UART_CMD_AUDIO_SUSPEND, NULL, 0, 0);
    if (err == ESP_OK)
    {
        taskENTER_CRITICAL(&s_state_lock);
        s_status.i2s_running = false;
        taskEXIT_CRITICAL(&s_state_lock);
    }
    return err;
}

esp_err_t a2dp_coprocessor_service_send_media_key(uint8_t key_code)
{
    return send_request_command(UART_CMD_MEDIA_KEY, &key_code, sizeof(key_code), 0);
}

esp_err_t a2dp_coprocessor_service_set_absolute_volume(uint8_t volume)
{
    if (volume > 127)
    {
        volume = 127;
    }
    return send_request_command(UART_CMD_SET_VOLUME, &volume, sizeof(volume), 0);
}

esp_err_t a2dp_coprocessor_service_get_metadata(uint8_t attr_mask)
{
    return send_request_command(UART_CMD_GET_METADATA, &attr_mask, sizeof(attr_mask), 0);
}

esp_err_t a2dp_coprocessor_service_get_play_status(void)
{
    return send_request_command(UART_CMD_GET_PLAY_STATUS, NULL, 0, 0);
}

esp_err_t a2dp_coprocessor_service_register_notification(uint8_t event_id, uint32_t event_parameter)
{
    uint8_t payload[5];
    payload[0] = event_id;
    put_u32(&payload[1], event_parameter);
    return send_request_command(UART_CMD_REGISTER_NOTIFICATION, payload, sizeof(payload), 0);
}

esp_err_t a2dp_coprocessor_service_refresh_bonded_devices(void)
{
    return send_request_command(UART_CMD_LIST_BONDED, NULL, 0, UART_EVT_BONDED_LIST);
}

size_t a2dp_coprocessor_service_get_bonded_device_count(void)
{
    size_t count;

    taskENTER_CRITICAL(&s_state_lock);
    count = s_bonded_count;
    taskEXIT_CRITICAL(&s_state_lock);
    return count;
}

esp_err_t a2dp_coprocessor_service_get_bonded_devices(size_t *count,
                                                      a2dp_coprocessor_addr_t *devices)
{
    size_t capacity;
    size_t copied;

    if (!count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    capacity = *count;
    if (capacity > 0 && !devices)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    copied = s_bonded_count < capacity ? s_bonded_count : capacity;
    for (size_t index = 0; index < copied; index++)
    {
        memcpy(devices[index], s_bonded_devices[index].bda, A2DP_COPROCESSOR_ADDR_LEN);
    }
    *count = copied;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t a2dp_coprocessor_service_get_bonded_device_entries(size_t *count,
                                                             a2dp_coprocessor_bonded_device_t *devices)
{
    size_t capacity;
    size_t copied;

    if (!count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    capacity = *count;
    if (capacity > 0 && !devices)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    copied = s_bonded_count < capacity ? s_bonded_count : capacity;
    for (size_t index = 0; index < copied; index++)
    {
        memcpy(&devices[index], &s_bonded_devices[index], sizeof(devices[index]));
    }
    *count = copied;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t a2dp_coprocessor_service_refresh_discovery_results(void)
{
    return send_request_command(UART_CMD_GET_DISCOVERY_RESULTS, NULL, 0, UART_EVT_DISCOVERY_LIST);
}

esp_err_t a2dp_coprocessor_service_get_discovery_results(a2dp_coprocessor_scan_entry_t *out_entries,
                                                         size_t *count)
{
    size_t capacity;
    size_t copied;

    if (!count)
    {
        return ESP_ERR_INVALID_ARG;
    }
    capacity = *count;
    if (capacity > 0 && !out_entries)
    {
        return ESP_ERR_INVALID_ARG;
    }

    taskENTER_CRITICAL(&s_state_lock);
    copied = s_discovery_count < capacity ? s_discovery_count : capacity;
    for (size_t index = 0; index < copied; index++)
    {
        out_entries[index] = s_discovery_entries[index];
    }
    *count = copied;
    taskEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t a2dp_coprocessor_service_unbond(const a2dp_coprocessor_addr_t addr)
{
    if (!addr)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return send_request_command(UART_CMD_UNBOND, addr, A2DP_COPROCESSOR_ADDR_LEN, 0);
}