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

    esp_err_t bluetooth_service_init(void);
    esp_err_t bluetooth_service_pair_best_a2dp_sink(void);
    esp_err_t bluetooth_service_connect_last_bonded_a2dp_device(void);
    esp_err_t bluetooth_service_disconnect_a2dp(void);
    esp_err_t bluetooth_service_start_audio(void);
    esp_err_t bluetooth_service_suspend_audio(void);
    esp_err_t bluetooth_service_send_media_key(uint8_t key_code);
    esp_err_t bluetooth_service_register_pcm_provider(bluetooth_service_pcm_provider_t provider, void *user_ctx);
    void bluetooth_service_register_connection_callback(bluetooth_service_connection_cb_t callback, void *user_ctx);
    void bluetooth_service_register_media_key_callback(bluetooth_service_media_key_cb_t callback, void *user_ctx);
    size_t bluetooth_service_get_bonded_device_count(void);
    esp_err_t bluetooth_service_get_bonded_devices(size_t *count, esp_bd_addr_t *devices);

    /**
     * Register a custom A2DP source stream endpoint configured for 48 kHz
     * stereo SBC. Must be called after bluetooth_service_init() and before
     * the first A2DP connection.
     */
    esp_err_t bluetooth_service_register_48k_sbc_endpoint(void);

#ifdef __cplusplus
}
#endif
