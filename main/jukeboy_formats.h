#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define JUKEBOY_JBA_VERSION 0x1U
#define JUKEBOY_JBA_HEADER_BLOCK_SIZE 512U
#define JUKEBOY_JBM_VERSION 1U
#define JUKEBOY_JBS_VERSION 1U
#define JUKEBOY_MAX_TRACK_FILES 999U
#define JUKEBOY_JBM_FILENAME "album.jbm"
#define JUKEBOY_JBS_FILENAME "playback.jbs"

#define JUKEBOY_JBM_ALBUM_NAME_BYTES 128U
#define JUKEBOY_JBM_ALBUM_DESCRIPTION_BYTES 1024U
#define JUKEBOY_JBM_ARTIST_BYTES 256U
#define JUKEBOY_JBM_GENRE_BYTES 64U
#define JUKEBOY_JBM_TAG_COUNT 5U
#define JUKEBOY_JBM_TAG_BYTES 32U
#define JUKEBOY_JBM_TRACK_NAME_BYTES 128U
#define JUKEBOY_JBM_TRACK_ARTISTS_BYTES 256U

    typedef struct __attribute__((packed))
    {
        uint8_t version;
        uint32_t header_len_in_blocks;
        uint32_t lookup_table_len;
    } jukeboy_jba_header_t;

    typedef struct __attribute__((packed))
    {
        uint32_t version;
        uint32_t checksum;
        char album_name[JUKEBOY_JBM_ALBUM_NAME_BYTES];
        char album_description[JUKEBOY_JBM_ALBUM_DESCRIPTION_BYTES];
        char artist[JUKEBOY_JBM_ARTIST_BYTES];
        uint32_t year;
        uint32_t duration_sec;
        char genre[JUKEBOY_JBM_GENRE_BYTES];
        char tag[JUKEBOY_JBM_TAG_COUNT][JUKEBOY_JBM_TAG_BYTES];
        uint32_t track_count;
    } jukeboy_jbm_header_t;

    typedef struct __attribute__((packed))
    {
        char track_name[JUKEBOY_JBM_TRACK_NAME_BYTES];
        char artists[JUKEBOY_JBM_TRACK_ARTISTS_BYTES];
        uint32_t duration_sec;
        uint32_t file_num;
    } jukeboy_jbm_track_t;

    typedef struct __attribute__((packed))
    {
        uint32_t version;
        uint32_t current_track_num;
        uint32_t current_sec;
    } jukeboy_jbs_status_t;

    _Static_assert(sizeof(jukeboy_jba_header_t) == 9, "Unexpected .jba header size");
    _Static_assert(sizeof(jukeboy_jbm_header_t) == 1652, "Unexpected .jbm header size");
    _Static_assert(sizeof(jukeboy_jbm_track_t) == 392, "Unexpected .jbm track size");
    _Static_assert(sizeof(jukeboy_jbs_status_t) == 12, "Unexpected .jbs status size");

#ifdef __cplusplus
}
#endif