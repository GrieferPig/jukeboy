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

#define PLAYER_SVC_TRACK_FILENAME_MAX_LEN 16

    typedef enum
    {
        PLAYER_SVC_EVENT_STARTED,
        PLAYER_SVC_EVENT_PLAYLIST_READY,
        PLAYER_SVC_EVENT_TRACK_STARTED,
        PLAYER_SVC_EVENT_TRACK_FINISHED,
        PLAYER_SVC_EVENT_TRACK_PAUSED,
        PLAYER_SVC_EVENT_TRACK_BECAME_COUNTABLE,
    } player_service_event_id_t;

    typedef struct
    {
        uint32_t cartridge_checksum;
        uint32_t track_index;
        uint32_t track_file_num;
        uint32_t started_at_unix;
        uint32_t playback_position_sec;
        char filename[PLAYER_SVC_TRACK_FILENAME_MAX_LEN];
    } player_service_track_event_t;

    /**
     * @brief Playback sequence mode.
     *
     * Controls how the player selects the next track after one finishes or
     * when the user requests the next track.
     */
    typedef enum
    {
        PLAYER_SVC_MODE_SEQUENTIAL,    /**< Play all tracks in order, wrapping to the first when the last finishes. Default. */
        PLAYER_SVC_MODE_SINGLE_REPEAT, /**< Repeat the current track indefinitely. An explicit NEXT/PREV still advances sequentially. */
        PLAYER_SVC_MODE_SHUFFLE,       /**< Pick the next track at random (different from the current one). */
    } player_service_playback_mode_t;

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
    int32_t player_service_qemu_pcm_provider(uint8_t *data, int32_t len, void *user_ctx);

    /**
     * @brief Set the playback sequence mode.
     *
     * Safe to call from any task; the change takes effect on the next track
     * selection (auto-advance or explicit NEXT).
     *
     * @param mode  One of @c PLAYER_SVC_MODE_SEQUENTIAL, @c PLAYER_SVC_MODE_SINGLE_REPEAT,
     *              or @c PLAYER_SVC_MODE_SHUFFLE.
     * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an unknown mode.
     */
    esp_err_t player_service_set_playback_mode(player_service_playback_mode_t mode);

    /** @brief Return the currently active playback mode. */
    player_service_playback_mode_t player_service_get_playback_mode(void);

#ifdef __cplusplus
}
#endif