#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define A2DP_COPROCESSOR_ADDR_LEN 6
#define A2DP_COPROCESSOR_MAX_DEVICE_NAME_LEN 248
#define A2DP_COPROCESSOR_MAX_LIST_ITEMS 16

#define A2DP_COPROCESSOR_MEDIA_KEY_PLAY 0x44
#define A2DP_COPROCESSOR_MEDIA_KEY_STOP 0x45
#define A2DP_COPROCESSOR_MEDIA_KEY_PAUSE 0x46
#define A2DP_COPROCESSOR_MEDIA_KEY_REWIND 0x48
#define A2DP_COPROCESSOR_MEDIA_KEY_FAST_FORWARD 0x49
#define A2DP_COPROCESSOR_MEDIA_KEY_FORWARD 0x4B
#define A2DP_COPROCESSOR_MEDIA_KEY_BACKWARD 0x4C
#define A2DP_COPROCESSOR_MEDIA_KEY_VOL_UP 0x41
#define A2DP_COPROCESSOR_MEDIA_KEY_VOL_DOWN 0x42
#define A2DP_COPROCESSOR_MEDIA_KEY_MUTE 0x43
#define A2DP_COPROCESSOR_MEDIA_KEY_STATE_PRESSED 0x00
#define A2DP_COPROCESSOR_MEDIA_KEY_STATE_RELEASED 0x01
#define A2DP_COPROCESSOR_CONNECTION_STATE_CONNECTED 0x02

    typedef uint8_t a2dp_coprocessor_addr_t[A2DP_COPROCESSOR_ADDR_LEN];

    typedef struct
    {
        a2dp_coprocessor_addr_t bda;
        int8_t rssi;
        uint32_t cod;
        char name[A2DP_COPROCESSOR_MAX_DEVICE_NAME_LEN + 1];
    } a2dp_coprocessor_scan_entry_t;

    typedef struct
    {
        bool pending;
        a2dp_coprocessor_addr_t remote_bda;
        uint32_t numeric_value;
    } a2dp_coprocessor_pairing_confirm_t;

    typedef struct
    {
        bool initialised;
        bool coprocessor_seen;
        bool discovery_running;
        bool a2dp_connected;
        bool i2s_running;
        uint8_t audio_state;
        a2dp_coprocessor_addr_t connected_bda;
        uint8_t local_volume;
        uint32_t buffered_bytes;
        uint32_t capacity_bytes;
        uint32_t underrun_count;
        uint32_t overrun_count;
        uint32_t sample_rate_hz;
        uint8_t bits_per_sample;
        uint8_t channel_count;
        uint32_t rx_frames;
        uint32_t tx_frames;
        uint32_t rx_errors;
        uint32_t crc_errors;
        uint32_t command_timeouts;
        uint8_t last_command;
        esp_err_t last_command_status;
    } a2dp_coprocessor_status_t;

    typedef enum
    {
        A2DP_COPROCESSOR_EVENT_STARTED,
        A2DP_COPROCESSOR_EVENT_STATUS,
        A2DP_COPROCESSOR_EVENT_DISCOVERY_RESULT,
        A2DP_COPROCESSOR_EVENT_DISCOVERY_DONE,
        A2DP_COPROCESSOR_EVENT_PAIRING_CONFIRM,
        A2DP_COPROCESSOR_EVENT_AUTH_COMPLETE,
        A2DP_COPROCESSOR_EVENT_A2DP_CONNECTION_STATE,
        A2DP_COPROCESSOR_EVENT_A2DP_AUDIO_STATE,
        A2DP_COPROCESSOR_EVENT_AVRCP_PASSTHROUGH_RSP,
        A2DP_COPROCESSOR_EVENT_AVRCP_REMOTE_COMMAND,
        A2DP_COPROCESSOR_EVENT_AVRCP_VOLUME,
        A2DP_COPROCESSOR_EVENT_AVRCP_METADATA,
        A2DP_COPROCESSOR_EVENT_AVRCP_PLAY_STATUS,
        A2DP_COPROCESSOR_EVENT_AVRCP_NOTIFICATION,
        A2DP_COPROCESSOR_EVENT_BONDED_LIST,
        A2DP_COPROCESSOR_EVENT_DISCOVERY_LIST,
        A2DP_COPROCESSOR_EVENT_ERROR,
    } a2dp_coprocessor_event_t;

    typedef struct
    {
        union
        {
            a2dp_coprocessor_status_t status;
            a2dp_coprocessor_scan_entry_t discovery_result;
            a2dp_coprocessor_pairing_confirm_t pairing_confirm;
            struct
            {
                a2dp_coprocessor_addr_t bda;
                uint8_t status;
            } auth;
            struct
            {
                uint8_t state;
                a2dp_coprocessor_addr_t remote_bda;
            } a2dp_connection;
            struct
            {
                uint8_t state;
                a2dp_coprocessor_addr_t remote_bda;
            } a2dp_audio;
            struct
            {
                uint8_t key_code;
                uint8_t key_state;
                uint8_t rsp_code;
            } avrc_passthrough;
            struct
            {
                uint8_t key_code;
                uint8_t key_state;
            } avrc_remote_command;
            struct
            {
                uint8_t volume;
                bool from_remote_target;
            } avrc_volume;
            struct
            {
                uint8_t attr_id;
                char text[128];
            } avrc_metadata;
            struct
            {
                uint32_t song_length;
                uint32_t song_position;
                uint8_t play_status;
            } avrc_play_status;
            struct
            {
                uint8_t event_id;
                uint8_t param[16];
                size_t param_len;
            } avrc_notification;
            struct
            {
                uint8_t event_type;
                esp_err_t status;
            } error;
        } data;
    } a2dp_coprocessor_event_data_t;

    typedef void (*a2dp_coprocessor_event_cb_t)(a2dp_coprocessor_event_t event,
                                                const a2dp_coprocessor_event_data_t *data,
                                                void *user_ctx);

    esp_err_t a2dp_coprocessor_service_init(void);
    void a2dp_coprocessor_service_process_once(void);
    void a2dp_coprocessor_service_register_event_callback(a2dp_coprocessor_event_cb_t callback, void *user_ctx);

    esp_err_t a2dp_coprocessor_service_refresh_status(void);
    void a2dp_coprocessor_service_get_status(a2dp_coprocessor_status_t *status_out);
    bool a2dp_coprocessor_service_is_initialised(void);
    bool a2dp_coprocessor_service_is_a2dp_connected(void);
    bool a2dp_coprocessor_service_is_discovery_running(void);

    esp_err_t a2dp_coprocessor_service_pair_best_a2dp_sink(void);
    esp_err_t a2dp_coprocessor_service_start_discovery(void);
    esp_err_t a2dp_coprocessor_service_connect_last_bonded_a2dp_device(void);
    esp_err_t a2dp_coprocessor_service_connect_a2dp(const a2dp_coprocessor_addr_t remote_bda);
    esp_err_t a2dp_coprocessor_service_disconnect_a2dp(void);
    esp_err_t a2dp_coprocessor_service_get_pending_pairing_confirm(a2dp_coprocessor_pairing_confirm_t *confirm);
    esp_err_t a2dp_coprocessor_service_reply_pairing_confirm(bool accept);
    esp_err_t a2dp_coprocessor_service_start_audio(void);
    esp_err_t a2dp_coprocessor_service_suspend_audio(void);
    esp_err_t a2dp_coprocessor_service_send_media_key(uint8_t key_code);
    esp_err_t a2dp_coprocessor_service_set_absolute_volume(uint8_t volume);
    esp_err_t a2dp_coprocessor_service_get_metadata(uint8_t attr_mask);
    esp_err_t a2dp_coprocessor_service_get_play_status(void);
    esp_err_t a2dp_coprocessor_service_register_notification(uint8_t event_id, uint32_t event_parameter);

    esp_err_t a2dp_coprocessor_service_refresh_bonded_devices(void);
    size_t a2dp_coprocessor_service_get_bonded_device_count(void);
    esp_err_t a2dp_coprocessor_service_get_bonded_devices(size_t *count,
                                                          a2dp_coprocessor_addr_t *devices);
    esp_err_t a2dp_coprocessor_service_refresh_discovery_results(void);
    esp_err_t a2dp_coprocessor_service_get_discovery_results(a2dp_coprocessor_scan_entry_t *out_entries,
                                                             size_t *count);
    esp_err_t a2dp_coprocessor_service_unbond(const a2dp_coprocessor_addr_t addr);

#ifdef __cplusplus
}
#endif