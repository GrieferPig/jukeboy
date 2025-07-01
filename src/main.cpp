#include <Arduino.h>
#include "utils/macros.h"
#include "audio/audio_player.h"
#include "audio/audio_internals.h" // Needed for task handles
#include "utils/profile.h"
#include "utils/serial_interface.h"

const char *TAG = "main";

void setup()
{
    Serial.begin(115200);
    ESP_LOGI("main", "Starting up...");

    // Start the profiler task to monitor system performance
    unwrap_basetype(profiler_start(), "Failed to start profiler task");

    // Initialize the audio player. It will automatically detect tracks on the SD card.
    audio_player_init();

    // Initialize the serial interface for testing commands
    serial_interface_init();

    // Example: send a command to start playing track 0 after a short delay
    delay(2000);
    ESP_LOGI("main", "Sending command to play track 0...");
    AudioCommand play_cmd;
    play_cmd.type = CMD_PLAY_TRACK;
    play_cmd.params.track_number = 0;
    audio_player_send_command(play_cmd);
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // Let FreeRTOS tasks handle everything
}