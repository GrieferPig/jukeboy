#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(PLAYER_SERVICE_EVENT);

    typedef enum
    {
        PLAYER_SVC_EVENT_STARTED,
        PLAYER_SVC_EVENT_PLAYLIST_READY,
        PLAYER_SVC_EVENT_TRACK_STARTED,
        PLAYER_SVC_EVENT_TRACK_FINISHED,
    } player_service_event_id_t;

    typedef enum
    {
        PLAYER_SVC_CONTROL_NEXT,
        PLAYER_SVC_CONTROL_PREVIOUS,
        PLAYER_SVC_CONTROL_FAST_FORWARD,
        PLAYER_SVC_CONTROL_FAST_BACKWARD,
        PLAYER_SVC_CONTROL_VOLUME_UP,
        PLAYER_SVC_CONTROL_VOLUME_DOWN,
        PLAYER_SVC_CONTROL_PAUSE,
    } player_service_control_t;

    esp_err_t player_service_init(void);
    esp_err_t player_service_request_control(player_service_control_t control);
    bool player_service_is_paused(void);
    uint8_t player_service_get_volume_percent(void);
    void player_service_set_volume_absolute(uint8_t avrc_vol);
    int32_t player_service_pcm_provider(uint8_t *data, int32_t len, void *user_ctx);

#ifdef __cplusplus
}
#endif