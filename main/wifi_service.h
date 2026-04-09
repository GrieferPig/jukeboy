#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ── Event base ─────────────────────────────────────────────────────── */

    ESP_EVENT_DECLARE_BASE(WIFI_SERVICE_EVENT);

    typedef enum
    {
        WIFI_SVC_EVENT_STARTED,               /* service initialised & WiFi started        */
        WIFI_SVC_EVENT_SCAN_DONE,             /* scan results ready (data: wifi_svc_scan_result_t*) */
        WIFI_SVC_EVENT_CONNECTING,            /* association / authentication in progress   */
        WIFI_SVC_EVENT_CONNECTED,             /* associated + IP acquired                  */
        WIFI_SVC_EVENT_CONNECTIVITY_OK,       /* DNS connectivity test passed               */
        WIFI_SVC_EVENT_DISCONNECTED,          /* disconnected (data: uint8_t reason)        */
        WIFI_SVC_EVENT_SNTP_SYNCED,           /* NTP time synchronised                      */
        WIFI_SVC_EVENT_AUTORECONNECT_CHANGED, /* autoreconnect toggled (data: bool)         */
    } wifi_svc_event_id_t;

    /* ── Service state ──────────────────────────────────────────────────── */

    typedef enum
    {
        WIFI_SVC_STATE_IDLE,
        WIFI_SVC_STATE_SCANNING,
        WIFI_SVC_STATE_CONNECTING,
        WIFI_SVC_STATE_CONNECTED,
        WIFI_SVC_STATE_DISCONNECTED,
    } wifi_svc_state_t;

    /* ── Scan results ───────────────────────────────────────────────────── */

#define WIFI_SVC_MAX_SCAN_RESULTS 20

    typedef struct
    {
        wifi_ap_record_t records[WIFI_SVC_MAX_SCAN_RESULTS];
        uint16_t count;
    } wifi_svc_scan_result_t;

    /* ── Public API ─────────────────────────────────────────────────────── */

    /**
     * Initialise the WiFi service: creates the service task, command queue,
     * default netif, WiFi driver in STA mode, and posts WIFI_SVC_EVENT_STARTED.
     * Must be called after nvs_flash_init().
     */
    esp_err_t wifi_service_init(void);

    /** Enqueue a connect command (SSID/password saved to NVS). */
    esp_err_t wifi_service_connect(const char *ssid, const char *password);

    /** Enqueue a disconnect command (stops auto-reconnect timer). */
    esp_err_t wifi_service_disconnect(void);

    /** Enqueue an async scan command. Listen for WIFI_SVC_EVENT_SCAN_DONE. */
    esp_err_t wifi_service_scan(void);

    /** Enable or disable 30 s periodic auto-reconnect using saved credentials. */
    esp_err_t wifi_service_set_autoreconnect(bool enable);

    /**
     * @brief Immediately attempt to reconnect using saved credentials.
     *
     * No-op if already connected or a connection attempt is already in progress.
     */
    esp_err_t wifi_service_reconnect(void);

    /** Thread-safe polling: current auto-reconnect status. */
    bool wifi_service_get_autoreconnect(void);

    /** Thread-safe polling: current service state. */
    wifi_svc_state_t wifi_service_get_state(void);

    /** Copy latest scan results into caller-provided buffer under mutex. */
    esp_err_t wifi_service_get_scan_results(wifi_svc_scan_result_t *out);

    /** Copy current IP info (only valid when state == CONNECTED). */
    esp_err_t wifi_service_get_ip_info(esp_netif_ip_info_t *out);

    /** Stop Wi-Fi, tear down its task/timer/netif state, and deinitialize the network driver stack. */
    esp_err_t wifi_service_shutdown(void);

#ifdef __cplusplus
}
#endif
