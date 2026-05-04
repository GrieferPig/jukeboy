#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define LASTFM_SERVICE_BASE_URL_MAX_LEN 127
#define LASTFM_SERVICE_USERNAME_MAX_LEN 127

    typedef struct
    {
        bool has_auth_url;
        bool has_token;
        bool has_session;
        bool busy;
        bool scrobbling_enabled;
        bool now_playing_enabled;
        bool now_playing_active;
        bool command_queue_ready;
        bool scrobble_queue_ready;
        uint32_t pending_commands;
        uint32_t pending_scrobbles;
        uint32_t command_queue_capacity;
        uint32_t scrobble_queue_capacity;
        uint32_t successful_scrobbles;
        uint32_t failed_scrobbles;
        char auth_url[LASTFM_SERVICE_BASE_URL_MAX_LEN + 1];
        char username[LASTFM_SERVICE_USERNAME_MAX_LEN + 1];
    } lastfm_service_status_t;

    esp_err_t lastfm_service_init(void);
    esp_err_t lastfm_service_set_auth_url(const char *url);
    esp_err_t lastfm_service_request_auth(const char *username, const char *password);
    esp_err_t lastfm_service_request_token(void);
    esp_err_t lastfm_service_logout(void);
    esp_err_t lastfm_service_send_scrobble(uint32_t album_checksum, uint32_t track_index);
    esp_err_t lastfm_service_set_scrobbling_enabled(bool enabled);
    esp_err_t lastfm_service_set_now_playing_enabled(bool enabled);
    void lastfm_service_get_status(lastfm_service_status_t *status);

#ifdef __cplusplus
}
#endif