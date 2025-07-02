#pragma once

#define MAX_ALBUMS 512           // Maximum number of albums to store
#define MAX_TRACKS_PER_ALBUM 128 // Maximum number of tracks per album
#define MAX_TRACKS 4096          // Maximum number of tracks to store
#define MAX_FRIENDS 64           // Maximum number of friends to store
#define MAX_TAG_LEN 16           // Maximum length of a tag

#define INVALID_ALBUM_ID 0xFFFF // Invalid album ID
#define INVALID_TRACK_ID 0xFFFF // Invalid track ID

// Structures used to deserialize Last.fm API queries
// TODO: implement lastfm online func
// struct Lastfm_Image_t
// {
//     String size;
//     String url;
// };

// struct Lastfm_Tag_t
// {
//     String name;
//     String url;
// };

// struct Lastfm_TrackArtist_t
// {
//     String name;
//     String mbid;
//     String url;
// };

// struct Lastfm_Track_t
// {
//     int rank;
//     String name;
//     int duration;
//     String mbid; // Can be empty
//     String url;
//     struct
//     {
//         int fulltrack;
//         bool is_streamable;
//     } streamable;
//     Lastfm_TrackArtist_t artist;
// };

// struct Lastfm_Album_struct_t
// {
//     String name;
//     String artist_name;
//     String id;
//     String mbid;
//     String url;
//     String releasedate;
//     Lastfm_Image_t images[3]; // Fixed-size array of structs containing Strings
//     long listeners;
//     long playcount;
//     Lastfm_Tag_t toptags[5]; // Assuming max 5 tags
//     int num_toptags = 0;
//     Lastfm_Track_t tracks_list[10]; // Assuming max 10 tracks
//     int num_tracks = 0;
// };

typedef struct common_date_t
{
    uint8_t year; // years since 1970
    uint8_t month;
    uint8_t day;
} common_date_t;

// A stripped down version of this struct for local storage and inter-device communication
typedef struct local_album_t
{
    // these are persistent data stored in littlefs
    uint16_t album_id; // global id for referencing, starting from 0
    char name[32];
    char artist_name[64];
    char url[64];
    uint16_t release_year;          // years since 1970
    char tags[4][MAX_TAG_LEN];      // 4 maximum tags, each tag is a string of MAX_TAG_LEN characters
    uint8_t track_count;            // not including track references as this needs to be sent in packets of at most 246 bytes
    common_date_t first_added_date; // date when the album was first added to the library

    // these fields are not persistent
    uint16_t playcount;
    uint16_t last_listen_date_offset; // offset in days from the first added date
} local_album_t;

#define LOCAL_ALBUM_T_SIZE sizeof(local_album_t)

// decoupled from album_t for easier management
typedef struct local_track_t
{
    uint16_t track_id; // global id for referencing, starting from 0
    uint16_t album_id; // cannot be null even if it's a single
    char name[64];
    char artist_name[64];

    uint16_t playcount;
    common_date_t first_listen_date;
    uint16_t last_listen_date_offset; // offset in days from the first listen date
} local_track_t;

#define LOCAL_TRACK_T_SIZE sizeof(local_track_t)

typedef struct last_status
{
    uint16_t album_id;
    uint16_t track_id;
    uint16_t timestamp; // last paused timestamp in seconds
    uint8_t volume;
} last_status;

// used to track album/track database and sharing status
typedef struct personal_stats_t
{
    char username[32];
    char bio[64];
    char url[64];
    uint8_t friend_count; // number of friends, maximum 64

    uint16_t fav_albums_id[5];  // 5 maximum
    uint16_t fav_tracks_id[15]; // 15 maximum
    uint16_t total_albums;      // total number of albums in the library
    uint16_t total_tracks;      // total number of tracks in the library
    uint32_t total_playcount;   // total playcount of all tracks in the library
    uint32_t total_playtime;    // total playtime of all tracks in the library in minutes
} personal_stats_t;

#define PERSONAL_STATS_T_SIZE sizeof(personal_stats_t)

typedef struct friend_t
{
    uint8_t mac[6];           // MAC address of the friend
    personal_stats_t stats;   // Personal stats of the friend
    common_date_t first_seen; // Date when the friend was first seen
    common_date_t last_seen;  // Date when the friend was last seen
} friend_t;

#define FRIEND_T_SIZE sizeof(friend_t)

// .tja audio file format
// the header uses the first 512 bytes of the file
typedef struct tja_header_t
{
    char magic[4];      // "TJA\0"
    uint8_t version;    // Version of the TJA format
    uint8_t format;     // Format of the TJA file (now only adpcm)
    uint32_t len_pages; // 512 byte aligned
    uint32_t checksum;  // crc32 of the file (excl. header)
    char padding[18];   // Padding to make this section 32 bytes

    // audio metadata (starting at offset 32)
    char name[64];        // Track name
    char artist_name[64]; // Artist name
} tja_header_t;

#define TJA_HEADER_T_SIZE sizeof(tja_header_t)

// album.tjm metadata file
// this fits within 512 bytes
typedef struct album_tjm_header_t
{
    char magic[4];      // "TJM\0"
    uint8_t version;    // Version of the TJM format
    uint32_t len_pages; // 512 byte aligned
    // TODO: checksum would be never used, remove?
    uint32_t checksum; // crc32 of the file (excl. header)
    char padding[19];  // Padding to make this section 32 bytes

    // album metadata (starting at offset 32)
    char name[32];             // Album name
    char artist_name[64];      // Artist name
    char url[64];              // URL of the album
    uint16_t release_year;     // Release year (years since 1970)
    char tags[4][MAX_TAG_LEN]; // 4 maximum tags
    uint8_t track_count;       // Number of tracks in the album
} album_tjm_header_t;

#define ALBUM_TJM_HEADER_T_SIZE sizeof(album_tjm_header_t)

// Empty album and track structures for initialization
// Do not use {0} to initialize as we need invalid IDs

static const local_album_t EMPTY_ALBUM = {
    .album_id = INVALID_ALBUM_ID,
    .name = "",
    .artist_name = "",
    .url = "",
    .release_year = 0,
    .tags = {"", "", "", ""},
    .track_count = 0,
    .first_added_date = {0, 0, 0},
    .playcount = 0,
    .last_listen_date_offset = 0};

static const local_track_t EMPTY_TRACK = {
    .track_id = INVALID_TRACK_ID,
    .album_id = INVALID_ALBUM_ID,
    .name = "",
    .artist_name = "",
    .playcount = 0,
    .first_listen_date = {0, 0, 0},
    .last_listen_date_offset = 0};
