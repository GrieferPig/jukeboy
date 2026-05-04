#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt_defs.h"
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(BLUETOOTH_SERVICE_EVENT);

    typedef enum
    {
        BLUETOOTH_SVC_EVENT_STARTED,
        BLUETOOTH_SVC_EVENT_DISCOVERY_DONE,
        BLUETOOTH_SVC_EVENT_PAIRING_COMPLETE,
        BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE,
        BLUETOOTH_SVC_EVENT_A2DP_AUDIO_STATE,
        BLUETOOTH_SVC_EVENT_SPP_READY,
    } bluetooth_service_event_id_t;

    typedef enum
    {
        BLUETOOTH_SVC_CONNECTION_EVENT_AUTH_COMPLETE,
        BLUETOOTH_SVC_CONNECTION_EVENT_A2DP_CONNECTION_STATE,
        BLUETOOTH_SVC_CONNECTION_EVENT_A2DP_AUDIO_STATE,
        BLUETOOTH_SVC_CONNECTION_EVENT_SPP_CONNECTED,
        BLUETOOTH_SVC_CONNECTION_EVENT_SPP_DISCONNECTED,
    } bluetooth_service_connection_event_t;

    typedef int32_t (*bluetooth_service_pcm_provider_t)(uint8_t *data, int32_t len, void *user_ctx);
    typedef void (*bluetooth_service_connection_cb_t)(bluetooth_service_connection_event_t event,
                                                      const esp_bd_addr_t remote_bda,
                                                      void *user_ctx);
    typedef void (*bluetooth_service_media_key_cb_t)(uint8_t key_code,
                                                     uint8_t key_state,
                                                     esp_avrc_rsp_t rsp_code,
                                                     void *user_ctx);
    typedef void (*bluetooth_service_spp_rx_cb_t)(const uint8_t *data,
                                                  size_t len,
                                                  void *user_ctx);

    typedef struct
    {
        bool pending;
        esp_bd_addr_t remote_bda;
        uint32_t numeric_value;
    } bluetooth_service_pairing_confirm_t;

    esp_err_t bluetooth_service_init(void);
    void bluetooth_service_process_once(void);
    esp_err_t bluetooth_service_pair_best_a2dp_sink(void);
    esp_err_t bluetooth_service_connect_last_bonded_a2dp_device(void);
    esp_err_t bluetooth_service_get_pending_pairing_confirm(bluetooth_service_pairing_confirm_t *confirm);
    esp_err_t bluetooth_service_reply_pairing_confirm(bool accept);
    esp_err_t bluetooth_service_disconnect_a2dp(void);
    esp_err_t bluetooth_service_start_audio(void);
    esp_err_t bluetooth_service_suspend_audio(void);
    esp_err_t bluetooth_service_send_media_key(uint8_t key_code);
    esp_err_t bluetooth_service_register_pcm_provider(bluetooth_service_pcm_provider_t provider, void *user_ctx);
    void bluetooth_service_register_connection_callback(bluetooth_service_connection_cb_t callback, void *user_ctx);
    void bluetooth_service_register_media_key_callback(bluetooth_service_media_key_cb_t callback, void *user_ctx);
    esp_err_t bluetooth_service_register_spp_rx_callback(bluetooth_service_spp_rx_cb_t callback, void *user_ctx);
    esp_err_t bluetooth_service_spp_send_data(const uint8_t *data, size_t len);
    bool bluetooth_service_is_initialised(void);
    bool bluetooth_service_is_a2dp_connected(void);
    bool bluetooth_service_is_spp_connected(void);
    bool bluetooth_service_spp_notifications_enabled(void);
    size_t bluetooth_service_spp_get_mtu(void);
    size_t bluetooth_service_spp_get_max_payload(void);
    size_t bluetooth_service_get_bonded_device_count(void);
    esp_err_t bluetooth_service_get_bonded_devices(size_t *count, esp_bd_addr_t *devices);

    /**
     * Register a custom A2DP source stream endpoint configured for 48 kHz
     * stereo SBC. Must be called after bluetooth_service_init() and before
     * the first A2DP connection.
     */
    esp_err_t bluetooth_service_register_48k_sbc_endpoint(void);

    /** Disconnect and deinitialize the Classic Bluetooth host/controller stack owned by this app. */
    esp_err_t bluetooth_service_shutdown(void);

#ifdef __cplusplus
}
#endif
