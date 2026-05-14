#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define BLUETOOTH_SERVICE_ADDR_LEN 6

    ESP_EVENT_DECLARE_BASE(BLUETOOTH_SERVICE_EVENT);

    typedef enum
    {
        BLUETOOTH_SVC_EVENT_STARTED,
        BLUETOOTH_SVC_EVENT_SPP_READY,
        BLUETOOTH_SVC_EVENT_SPP_CONNECTED,
        BLUETOOTH_SVC_EVENT_SPP_DISCONNECTED,
    } bluetooth_service_event_id_t;

    typedef enum
    {
        BLUETOOTH_SVC_CONNECTION_EVENT_SPP_CONNECTED,
        BLUETOOTH_SVC_CONNECTION_EVENT_SPP_DISCONNECTED,
    } bluetooth_service_connection_event_t;

    typedef void (*bluetooth_service_connection_cb_t)(bluetooth_service_connection_event_t event,
                                                      void *user_ctx);
    typedef void (*bluetooth_service_spp_rx_cb_t)(const uint8_t *data,
                                                  size_t len,
                                                  void *user_ctx);

    esp_err_t bluetooth_service_init(void);
    void bluetooth_service_process_once(void);
    void bluetooth_service_register_connection_callback(bluetooth_service_connection_cb_t callback, void *user_ctx);
    esp_err_t bluetooth_service_register_spp_rx_callback(bluetooth_service_spp_rx_cb_t callback, void *user_ctx);
    esp_err_t bluetooth_service_spp_send_data(const uint8_t *data, size_t len);
    bool bluetooth_service_is_initialised(void);
    bool bluetooth_service_is_spp_connected(void);
    bool bluetooth_service_spp_notifications_enabled(void);
    size_t bluetooth_service_spp_get_mtu(void);
    size_t bluetooth_service_spp_get_max_payload(void);
    esp_err_t bluetooth_service_shutdown(void);

#ifdef __cplusplus
}
#endif