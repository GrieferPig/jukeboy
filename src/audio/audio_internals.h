#ifndef AUDIO_INTERNALS_H
#define AUDIO_INTERNALS_H

#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "adpcm_decoder.h"
#include "audio_player.h" // For AudioCommand struct

/**
 * @brief Extern declarations for shared global variables and RTOS handles.
 *
 * The 'extern' keyword tells the compiler that these variables exist, but their
 * actual memory is defined in one of the .cpp files (e.g., audio_control_task.cpp).
 * This allows all tasks to access the same shared resources.
 */

// Queues for inter-task communication
extern QueueHandle_t g_audio_command_queue; // For receiving commands from the public API
extern QueueHandle_t g_full_buffer_queue;   // For sending full ADPCM buffers to the decoder
extern QueueHandle_t g_empty_buffer_queue;  // For returning empty ADPCM buffers to the reader

// Task Handles for control (e.g., suspend/resume)
extern TaskHandle_t g_sdReaderTaskHandle;
extern TaskHandle_t g_decoderTaskHandle;

// Shared state controlled by audio_control_task
extern volatile uint8_t g_volume_shift;
extern volatile int g_current_track_number;

/**
 * @brief Function prototypes for the tasks.
 *
 * This allows the main .ino/.cpp file to create the tasks without needing to
 * include all of their implementation files.
 */
void audio_control_task(void *pvParameters);
void sd_reader_task(void *pvParameters);
void decoder_task(void *pvParameters);

#endif // AUDIO_INTERNALS_H
