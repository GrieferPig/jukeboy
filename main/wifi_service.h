#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
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
#define WIFI_SVC_SLOT_COUNT 3

    typedef struct
    {
        wifi_ap_record_t records[WIFI_SVC_MAX_SCAN_RESULTS];
        uint16_t count;
    } wifi_svc_scan_result_t;

    typedef struct
    {
        bool configured;
        bool preferred;
        bool active;
        char ssid[33];
    } wifi_svc_slot_info_t;

    /* ── Public API ─────────────────────────────────────────────────────── */

    /**
     * Initialise the WiFi service: creates the internal command queue,
     * default netif, WiFi driver in STA mode, and posts WIFI_SVC_EVENT_STARTED.
     * Must be called after the application's secure NVS initialization step.
     */
    esp_err_t wifi_service_init(void);

    /** Run one bounded slice of deferred Wi-Fi work from the main super-loop. */
    void wifi_service_process_once(void);

    /** Enqueue a connect command (SSID/password saved to NVS). */
    esp_err_t wifi_service_connect(const char *ssid, const char *password);

    /** Save or replace a Wi-Fi slot (0-based slot index). */
    esp_err_t wifi_service_save_slot(uint8_t slot_index, const char *ssid, const char *password);

    /** Enqueue a connect command using a saved Wi-Fi slot (0-based slot index). */
    esp_err_t wifi_service_connect_slot(uint8_t slot_index);

    /** Enqueue a disconnect command (stops auto-reconnect timer). */
    esp_err_t wifi_service_disconnect(void);

    /** Enqueue an async scan command. Listen for WIFI_SVC_EVENT_SCAN_DONE. */
    esp_err_t wifi_service_scan(void);

    /** Enable or disable persistent 30 s periodic auto-reconnect using saved credentials. */
    esp_err_t wifi_service_set_autoreconnect(bool enable);

    /**
     * @brief Immediately attempt to reconnect using saved credentials.
     *
     * No-op if already connected or a connection attempt is already in progress.
     */
    esp_err_t wifi_service_reconnect(void);

    /** Thread-safe polling: current persisted auto-reconnect status. */
    bool wifi_service_get_autoreconnect(void);

    /** Thread-safe polling: current service state. */
    wifi_svc_state_t wifi_service_get_state(void);

    /** Thread-safe polling: true once the connectivity test has passed for the current Wi-Fi link. */
    bool wifi_service_has_internet(void);

    /** Read all saved Wi-Fi slots into the caller-provided array. */
    esp_err_t wifi_service_get_saved_slots(wifi_svc_slot_info_t *out_slots, size_t slot_count);

    /** Return the preferred slot index for auto-reconnect, or -1 when unset. */
    int wifi_service_get_preferred_slot(void);

    /** Return the current active or connecting slot index, or -1 when unset. */
    int wifi_service_get_active_slot(void);

    /** Copy latest scan results into caller-provided buffer under mutex. */
    esp_err_t wifi_service_get_scan_results(wifi_svc_scan_result_t *out);

    /** Copy current IP info (only valid when state == CONNECTED). */
    esp_err_t wifi_service_get_ip_info(esp_netif_ip_info_t *out);

    /** Stop Wi-Fi, tear down its queue/timer/netif state, and deinitialize the network driver stack. */
    esp_err_t wifi_service_shutdown(void);

#ifdef __cplusplus
}
#endif
