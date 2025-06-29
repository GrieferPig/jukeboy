/*
    NotSoSimplewavShuffle

    Turns the Pico into a basic wav shuffle player and plays all the wavs
    in the root directory of an SD card.  Hook up an earphone to pins 0, 1,
    and GND to hear the PWM output.  Wire up an SD card to the pins specified
    below.

    Copyright (c) 2024 Earle F. Philhower, III <earlephilhower@yahoo.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>   // Added for FreeRTOS tasks
#include "esp_now_handler.h" // Include the new ESP-NOW handler
#include "audio_player.h"
#include "web_server.h"
#include "esp_random.h"
#include "esp_log.h" // Added for ESP_LOGx macros
#include <esp_pm.h>
#include "storage_man.h"
#include "dummy_data.h"          // Include dummy data
static const char *TAG = "Main"; // Added for ESP_LOGx

// Forward declarations for task functions are now in their respective .h files

void setup()
{
    vTaskDelay(pdMS_TO_TICKS(2000));

    esp_pm_config_t pm_config = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true};
    ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    ESP_LOGI(TAG, "System Setup: Starting");

    // if (!LittleFS.begin())
    // {
    //     ESP_LOGE(TAG, "System Setup: Failed to mount LittleFS. Restarting ESP...");
    //     while (1) // Loop to ensure restart happens
    //     {
    //         delay(1000);
    //         ESP.restart();
    //     }
    // }
    // ESP_LOGI(TAG, "System Setup: LittleFS mounted.");

    // Set device as a Wi-Fi Station
    // WiFi.mode(WIFI_STA);
    // WiFi.setChannel(1);

    // pinMode(8, OUTPUT);   // LED on GPIO 8 (used by audio_player.cpp)
    // digitalWrite(8, LOW); // Ensure LED is off initially

    // srand(esp_random()); // Initialize random seed with a random number
    // ESP_LOGI(TAG, "System Setup: Random seed initialized.");

    // if (initEspNowComms())
    // {
    //     ESP_LOGI(TAG, "ESP-NOW Communications initialized successfully via handler.");
    // }
    // else
    // {
    //     ESP_LOGE(TAG, "Failed to initialize ESP-NOW Communications via handler. System Halted.");
    //     // Handle initialization failure, e.g., by halting or retrying
    //     ESP.restart(); // Restart the ESP32 to retry initialization
    // }

    // // Create Web Server Task
    // xTaskCreate(
    //     webServerTask,   /* Task function. */
    //     "WebServerTask", /* name of task. */
    //     10000,           /* Stack size of task (bytes) */
    //     NULL,            /* parameter of the task */
    //     1,               /* priority of the task (0 is lowest) */
    //     NULL             /* Task handle to keep track of created task */
    // );
    // ESP_LOGI(TAG, "System Setup: WebServerTask created.");

    // Create Audio Player Task
    // xTaskCreate(
    //     audioPlayerTask,   /* Task function. */
    //     "AudioPlayerTask", /* name of task. */
    //     10000,             /* Stack size of task (bytes) */
    //     NULL,              /* parameter of the task */
    //     1,                 /* priority of the task (0 is lowest) */
    //     NULL               /* Task handle to keep track of created task */
    // );
    // ESP_LOGI(TAG, "System Setup: AudioPlayerTask created.");

    storageManagerStart(); // Start the storage manager task
    ESP_LOGI(TAG, "System Setup: StorageManagerTask created and started.");

    while (xStorageManagerQueue == NULL)
    {
        // Wait for the storage manager queue to be created
        ESP_LOGI(TAG, "Waiting for Storage Manager Queue to be created...");
        vTaskDelay(pdMS_TO_TICKS(100)); // Delay to avoid busy-waiting
    }

    // Test storage manager with dummy data
    ESP_LOGI(TAG, "Main: Adding dummy_album_1 to storage queue.");
    StorageManagerAlbumSubmittingPayload payload1 = {
        .album = dummy_album_1,
        .tracks = (local_track_t[]){dummy_track_1, dummy_track_2}, // Actual tracks array
        .trackCount = 2                                            // Matches dummy album 1's track_count
    };
    storageManagerAction(StorageManagerActionType::ACTION_ADD_ALBUM, &payload1, sizeof(payload1));

    ESP_LOGI(TAG, "Main: Adding dummy_album_2 to storage queue.");
    StorageManagerAlbumSubmittingPayload payload2 = {
        .album = dummy_album_2,
        .tracks = (local_track_t[]){dummy_track_3}, // Actual track array
        .trackCount = 1                             // Matches dummy album 2's track_count
    };
    storageManagerAction(StorageManagerActionType::ACTION_ADD_ALBUM, &payload2, sizeof(payload2));

    storageManagerSync(); // Ensure all actions are processed before proceeding
    ESP_LOGI(TAG, "Main: Fetching personal stats from storage.");
    // Initialize empty personal stats structure
    personal_stats_t *stats = (personal_stats_t *)malloc(sizeof(personal_stats_t));
    if (stats == NULL)
    {
        ESP_LOGE(TAG, "Main: Failed to allocate memory for personal stats.");
        return; // Exit if memory allocation fails
    }
    storageManagerAction(StorageManagerActionType::ACTION_GET_STATS, stats, sizeof(personal_stats_t), true);
    if (stats != NULL)
    {
        ESP_LOGI(TAG, "Main: Fetched personal stats from storage.");
        // print the stats for debugging
        ESP_LOGI(TAG, "Username: %s", stats->username);
        ESP_LOGI(TAG, "Bio: %s", stats->bio);
        ESP_LOGI(TAG, "URL: %s", stats->url);
        ESP_LOGI(TAG, "Friend Count: %d", stats->friend_count);
        ESP_LOGI(TAG, "Total Albums: %d", stats->total_albums);
        ESP_LOGI(TAG, "Total Tracks: %d", stats->total_tracks);
        ESP_LOGI(TAG, "Total Playcount: %d", stats->total_playcount);
        ESP_LOGI(TAG, "Total Playtime: %d", stats->total_playtime);
        ESP_LOGI(TAG, "Favorite Albums:");
        for (int i = 0; i < 5; i++)
        {
            ESP_LOGI(TAG, "  Album %d ID: %d", i + 1, stats->fav_albums_id[i]);
        }
        ESP_LOGI(TAG, "Favorite Tracks:");
        for (int i = 0; i < 5; i++)
        {
            ESP_LOGI(TAG, "  Track %d ID: %d", i + 1, stats->fav_tracks_id[i]);
        }

        // Free the stats data after use
        free(stats);
    }
    else
    {
        ESP_LOGE(TAG, "Main: Failed to fetch personal stats from storage.");
    }
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow processing and log observation before restart
    // ESP.restart();
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}