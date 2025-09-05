#pragma once

#include <stdint.h>
#include "pindef.h"

// Enum for playback commands
typedef enum
{
    CMD_PLAY_TRACK,
    CMD_TOGGLE_PAUSE,
    CMD_NEXT_TRACK,
    CMD_PREV_TRACK,
    CMD_SET_VOLUME_SHIFT,
    CMD_TOGGLE_SHUFFLE,
    CMD_FFWD_10SEC,
    CMD_REWIND_5SEC,
    CMD_VOLUME_INC,
    CMD_VOLUME_DEC,
    CMD_SHUTDOWN,  // New: Gracefully shut down the daemon task
    // Add other commands here
} CommandType;

// Struct to hold command data
typedef struct
{
    CommandType type;
    union
    {
        int track_number;
        int volume_shift; // Volume shift value for CMD_SET_VOLUME_SHIFT
    } params;
} AudioCommand;

/**
 * @brief Initializes the audio player system and starts all related tasks.
 * This should be called once in your main setup() function.
 * @param track_count The total number of tracks available on the SD card.
 */
void audio_player_init();

/**
 * @brief Sends a command to the audio player's control task.
 * @param cmd The command to be executed.
 */
void audio_player_send_command(const AudioCommand *cmd);
