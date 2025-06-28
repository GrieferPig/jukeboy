#pragma once

#include <ArduinoJson.h>
#include <Arduino.h>

#define MAX_ALBUMS 512           // Maximum number of albums to store
#define MAX_TRACKS_PER_ALBUM 128 // Maximum number of tracks per album
#define MAX_TRACKS 4096          // Maximum number of tracks to store
#define MAX_FRIENDS 50           // Maximum number of friends to store
#define MAX_TAG_LEN 15           // Maximum length of a tag

#define INVALID_ALBUM_ID 0xFFFF // Invalid album ID
#define INVALID_TRACK_ID 0xFFFF // Invalid track ID

// Structures used to deserialize Last.fm API queries
// TODO: implement lastfm online func
struct Lastfm_Image_t
{
    String size;
    String url;
};

struct Lastfm_Tag_t
{
    String name;
    String url;
};

struct Lastfm_TrackArtist_t
{
    String name;
    String mbid;
    String url;
};

struct Lastfm_Track_t
{
    int rank;
    String name;
    int duration;
    String mbid; // Can be empty
    String url;
    struct
    {
        int fulltrack;
        bool is_streamable;
    } streamable;
    Lastfm_TrackArtist_t artist;
};

struct Lastfm_Album_struct_t
{
    String name;
    String artist_name;
    String id;
    String mbid;
    String url;
    String releasedate;
    Lastfm_Image_t images[3]; // Fixed-size array of structs containing Strings
    long listeners;
    long playcount;
    Lastfm_Tag_t toptags[5]; // Assuming max 5 tags
    int num_toptags = 0;
    Lastfm_Track_t tracks_list[10]; // Assuming max 10 tracks
    int num_tracks = 0;
};

struct common_date_t
{
    uint8_t year; // years since 1970
    uint8_t month;
    uint8_t day;
};

// A stripped down version of this struct for local storage and inter-device communication
struct local_album_t
{
    // these are persistent data stored in littlefs
    uint16_t album_id; // global id for referencing, starting from 0
    char name[31];
    char artist_name[61];
    char url[61];
    uint8_t release_year;           // years since 1970
    char tags[5][MAX_TAG_LEN];      // 5 maximum tags, each tag is a string of MAX_TAG_LEN characters
    uint8_t track_count;            // not including track references as this needs to be sent in packets of at most 246 bytes
    common_date_t first_added_date; // date when the album was first added to the library

    // these fields are not persistent
    uint16_t playcount;
    uint16_t last_listen_date_offset; // offset in days from the first added date
};

#define LOCAL_ALBUM_T_SIZE sizeof(local_album_t)

// decoupled from album_t for easier management
struct local_track_t
{
    uint16_t track_id; // global id for referencing, starting from 0
    uint16_t album_id; // cannot be null even if it's a single
    char name[21];
    char artist_name[21];
    char filename[2]; // hex encoded filename, no null terminator, 256 tracks max

    uint16_t playcount;
    common_date_t first_listen_date;
    uint16_t last_listen_date_offset; // offset in days from the first listen date
};

#define LOCAL_TRACK_T_SIZE sizeof(local_track_t)

struct last_status
{
    uint16_t album_id;
    uint16_t track_id;
    uint16_t timestamp; // last paused timestamp in seconds
    uint8_t volume;
};

// used to track album/track database and sharing status
struct personal_stats_t
{
    char username[31];
    char bio[71];
    char url[71];
    uint8_t friend_count; // number of friends, maximum 50

    uint16_t fav_albums_id[5];  // 5 maximum
    uint16_t fav_tracks_id[15]; // 15 maximum
    uint16_t total_albums;      // total number of albums in the library
    uint16_t total_tracks;      // total number of tracks in the library
    uint32_t total_playcount;   // total playcount of all tracks in the library
    uint32_t total_playtime;    // total playtime of all tracks in the library in minutes
};

#define PERSONAL_STATS_T_SIZE sizeof(personal_stats_t)

struct friend_t
{
    uint8_t mac[6];           // MAC address of the friend
    personal_stats_t stats;   // Personal stats of the friend
    common_date_t first_seen; // Date when the friend was first seen
    common_date_t last_seen;  // Date when the friend was last seen
};

#define FRIEND_T_SIZE sizeof(friend_t)

static const local_album_t EMPTY_ALBUM = {
    .album_id = INVALID_ALBUM_ID,
    .name = "",
    .artist_name = "",
    .url = "",
    .release_year = 0,
    .tags = {"", "", "", "", ""},
    .track_count = 0,
    .first_added_date = {0, 0, 0},
    .playcount = 0,
    .last_listen_date_offset = 0};

static const local_track_t EMPTY_TRACK = {
    .track_id = INVALID_TRACK_ID,
    .album_id = INVALID_ALBUM_ID,
    .name = "",
    .artist_name = "",
    .filename = "",
    .playcount = 0,
    .first_listen_date = {0, 0, 0},
    .last_listen_date_offset = 0};