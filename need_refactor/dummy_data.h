#pragma once
#include <storage_struct.h>

// Dummy Date Initializer
common_date_t create_date(uint8_t year, uint8_t month, uint8_t day)
{
    return {year, month, day};
}

// --- Dummy Albums ---
local_album_t dummy_album_1 = {
    .album_id = 0,
    .name = "Echoes of Tomorrow",
    .artist_name = "Stellar Drifters",
    .url = "http://example.com/stellardrifters/echoes",
    .release_year = 53, // 2023 - 1970
    .tags = {"Ambient", "Sci-Fi", "Chill", "", ""},
    .track_count = 12,
    .first_added_date = create_date(53, 10, 26), // 2023
    .playcount = 150,
    .last_listen_date_offset = 594}; // days from first_added_date to 2025-06-12

local_album_t dummy_album_2 = {
    .album_id = 1,
    .name = "Concrete Jungles",
    .artist_name = "Urban Pulse",
    .url = "http://example.com/urbanpulse/jungles",
    .release_year = 51, // 2021 - 1970
    .tags = {"Hip-Hop", "Lo-Fi", "Jazz", "Beats", ""},
    .track_count = 10,
    .first_added_date = create_date(52, 1, 15), // 2022
    .playcount = 320,
    .last_listen_date_offset = 1231}; // days from first_added_date to 2025-05-30

// --- Dummy Tracks ---
local_track_t dummy_track_1 = {
    .track_id = 0,
    .album_id = 0,
    .name = "Nebula",
    .artist_name = "Stellar Drifters",
    .filename = {'0', '0'}, // hex encoded
    .playcount = 45,
    .first_listen_date = create_date(53, 10, 26), // 2023
    .last_listen_date_offset = 593};              // days from first_listen_date to 2025-06-11

local_track_t dummy_track_2 = {
    .track_id = 1,
    .album_id = 0,
    .name = "Solar Winds",
    .artist_name = "Stellar Drifters",
    .filename = {'0', '1'}, // hex encoded
    .playcount = 30,
    .first_listen_date = create_date(53, 10, 27), // 2023
    .last_listen_date_offset = 593};              // days from first_listen_date to 2025-06-12

local_track_t dummy_track_3 = {
    .track_id = 2,
    .album_id = 1,
    .name = "Metro Lines",
    .artist_name = "Urban Pulse",
    .filename = {'0', '2'}, // hex encoded
    .playcount = 80,
    .first_listen_date = create_date(52, 1, 15), // 2022
    .last_listen_date_offset = 1230};            // days from first_listen_date to 2025-05-29

// --- Dummy Last Status ---
last_status current_status = {
    .album_id = 0,
    .track_id = 1,
    .timestamp = 182, // Paused at 3 minutes and 2 seconds
    .volume = 85      // 85%
};

// --- Dummy Personal Stats ---
personal_stats_t my_stats = {
    .username = "MusicLover_25",
    .bio = "Just a user exploring the vast universe of sound.",
    .url = "http://myprofile.example.com",
    .friend_count = 1,
    .fav_albums_id = {0, 1, 0xFFFF, 0xFFFF, 0xFFFF},
    .fav_tracks_id = {0, 1, 2, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
    .total_albums = 24,
    .total_tracks = 312,
    .total_playcount = 4890,
    .total_playtime = 16300 // 978000 seconds converted to minutes
};

// --- Dummy Friend ---
friend_t friend_profile = {
    .mac = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F},
    .stats = {
        .username = "AudioPhile_7",
        .bio = "Friend with good taste in music.",
        .url = "http://friend.example.com",
        .friend_count = 5,
        .fav_albums_id = {20, 21, 0xFFFF, 0xFFFF, 0xFFFF},
        .fav_tracks_id = {150, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
        .total_albums = 150,
        .total_tracks = 1800,
        .total_playcount = 25000,
        .total_playtime = 83333},         // 5000000 seconds converted to minutes
    .first_seen = create_date(54, 8, 1),  // 2024
    .last_seen = create_date(55, 6, 10)}; // 2025