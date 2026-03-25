#pragma once

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

    esp_err_t player_service_init(void);
    int32_t player_service_pcm_provider(uint8_t *data, int32_t len, void *user_ctx);

#ifdef __cplusplus
}
#endif