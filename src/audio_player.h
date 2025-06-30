#ifndef AUDIO_PLAYER_H
#define AUDIO_PLAYER_H

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// Audio Command Types
typedef enum
{
    CMD_PLAY_NEXT,
    CMD_PLAY_PREVIOUS,
    CMD_RESTART_TRACK,
    CMD_JUMP_TO_POSITION_SECONDS, // Parameter: uint32_t seconds
    CMD_FAST_FORWARD_SECONDS,     // Parameter: uint32_t seconds
    CMD_REWIND_SECONDS,           // Parameter: uint32_t seconds
    CMD_SET_GAIN,                 // Parameter: float gain_value (0.0 to 0.2)
    CMD_TOGGLE_PAUSE,
    CMD_GET_CURRENT_TRACK_INFO, // Logs info
    CMD_GET_PLAYBACK_STATUS,    // Logs info
    CMD_SET_PLAYBACK_MODE,      // Parameter: PlaybackMode mode
    // TODO: impl CMD_SET_GAIN_ADD, CMD_SET_GAIN_SUB
    CMD_SET_GAIN_ADD, // 15 levels of gain increase
    CMD_SET_GAIN_SUB
} AudioCommandType;

// Playback Mode
typedef enum
{
    PLAYBACK_MODE_LOOP,
    PLAYBACK_MODE_SHUFFLE
} PlaybackMode;

// Audio Command Structure
typedef struct
{
    AudioCommandType type;
    union
    {
        uint32_t seconds;
        float gain_value;
        PlaybackMode mode;
    } params;
} AudioCommand;

// Extern declaration for the audio command queue
extern QueueHandle_t audioCommandQueue;

// Forward declaration for the audio player task
void audioPlayerTask(void *pvParameter);

// Public interface functions to control audio
void audio_play_next();
void audio_play_previous();
void audio_restart_track();
void audio_jump_to_position_seconds(uint32_t seconds);
void audio_fast_forward_seconds(uint32_t seconds);
void audio_rewind_seconds(uint32_t seconds);
void audio_set_gain(float gain); // Gain will be clamped 0.0f to 0.2f
void audio_toggle_pause();
void audio_get_current_track_info();
void audio_get_playback_status();
void audio_set_playback_mode(PlaybackMode mode);

#endif // AUDIO_PLAYER_H
