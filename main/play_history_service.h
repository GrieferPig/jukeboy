#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "jukeboy_formats.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define PLAY_HISTORY_MAX_ALBUMS 50U
#define PLAY_HISTORY_MAX_TRACKS 1000U

    typedef struct
    {
        uint32_t checksum;
        uint32_t track_count;
        uint32_t first_seen_sequence;
        uint32_t last_seen_sequence;
        jukeboy_jbm_header_t metadata;
    } play_history_album_record_t;

    typedef struct
    {
        uint32_t cartridge_checksum;
        uint32_t track_index;
        uint32_t track_file_num;
        uint32_t play_count;
        uint32_t first_seen_sequence;
        uint32_t last_seen_sequence;
        jukeboy_jbm_track_t metadata;
    } play_history_track_record_t;

    esp_err_t play_history_service_init(void);
    void play_history_service_process_once(void);
    esp_err_t play_history_service_commit(void);
    esp_err_t play_history_service_flush(void);
    bool play_history_service_is_dirty(void);
    bool play_history_service_is_commit_in_progress(void);

    size_t play_history_service_get_album_count(void);
    size_t play_history_service_get_track_count(void);
    bool play_history_service_get_album_record(size_t slot, play_history_album_record_t *out_record);
    bool play_history_service_get_album_record_by_checksum(uint32_t checksum, play_history_album_record_t *out_record);
    size_t play_history_service_get_album_track_count(uint32_t checksum);
    bool play_history_service_get_album_track_record(uint32_t checksum,
                                                     size_t slot,
                                                     play_history_track_record_t *out_record);

    esp_err_t play_history_service_request_clear(void);
    esp_err_t play_history_service_request_rebuild(void);

#ifdef __cplusplus
}
#endif