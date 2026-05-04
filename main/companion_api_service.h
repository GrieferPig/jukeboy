#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "hid_service.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define COMPANION_API_CLIENT_ID_MAX_LEN 36
#define COMPANION_API_APP_NAME_MAX_LEN 32
#define COMPANION_API_MAX_TRUSTED_CLIENTS 4

    typedef struct
    {
        bool valid;
        char client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
        char app_name[COMPANION_API_APP_NAME_MAX_LEN + 1];
        uint32_t created_at_unix;
    } companion_api_trusted_client_info_t;

    typedef struct
    {
        bool initialised;
        bool spp_connected;
        bool spp_notifications_enabled;
        bool authenticated;
        bool pairing_pending;
        uint8_t pairing_progress;
        uint8_t pairing_required_count;
        uint8_t trusted_client_count;
        uint32_t event_generation;
        uint32_t rx_frames;
        uint32_t tx_frames;
        uint32_t rx_errors;
        uint32_t dropped_rx_chunks;
        char active_client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
        char pending_client_id[COMPANION_API_CLIENT_ID_MAX_LEN + 1];
        char pending_app_name[COMPANION_API_APP_NAME_MAX_LEN + 1];
        hid_button_t pending_sequence[4];
    } companion_api_status_t;

    esp_err_t companion_api_service_init(void);
    void companion_api_service_get_status(companion_api_status_t *status_out);
    size_t companion_api_service_get_trusted_client_count(void);
    esp_err_t companion_api_service_get_trusted_client(size_t index,
                                                       companion_api_trusted_client_info_t *client_out);
    esp_err_t companion_api_service_console_confirm_pairing(void);
    esp_err_t companion_api_service_console_cancel_pairing(void);
    esp_err_t companion_api_service_console_input_button(hid_button_t button);
    esp_err_t companion_api_service_revoke_client(const char *client_id);
    esp_err_t companion_api_service_revoke_all_clients(void);

#ifdef __cplusplus
}
#endif