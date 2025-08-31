#include "macros.h"
#include "audio/audio_player.h"
#include "audio/audio_internals.h" // Needed for task handles
#include "utils/profile.h"
#include "utils/serial_interface.h"
#include "hid/hid_mgr.h"
#include "hid/power_mgr.h"

#include "pindef.h"
#include <esp_log.h>

const char *TAG = "main";

void ota_app_main()
{
    ESP_LOGI(TAG, "Starting up...");

    // Initialize the power manager for ADC reading
    unwrap_esp_err(power_mgr_init(), "Failed to initialize power manager");

    // Initialize the HID manager for button controls
    unwrap_esp_err(hid_mgr_init(), "Failed to initialize HID manager");

    // Initialize the audio player. It will automatically detect tracks on the SD card.
    audio_player_init();

    // Initialize the serial interface for testing commands
    // serial_interface_init();

    // Initialize profiler
    // unwrap_basetype(profiler_init(), "Failed to initialize profiler");

    // Example: send a command to start playing track
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait a bit for initialization
    ESP_LOGI(TAG, "Sending command to play track 0...");
    AudioCommand play_cmd;
    play_cmd.type = CMD_PLAY_TRACK;
    play_cmd.params.track_number = 0;
    audio_player_send_command(&play_cmd);

    power_mgr_notify_main_initialized(); // Notify the power manager that the main task is exiting
}