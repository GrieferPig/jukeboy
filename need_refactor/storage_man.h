#pragma once
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <storage_struct.h>
#include <LittleFS.h>

static const char *TAG_STORAGE = "StorageManager"; // Tag for logging

static const char *ALBUM_DATABASE_FILENAME = "/albums.bin";    // File to store album data
static const char *TRACK_DATABASE_FILENAME = "/tracks.bin";    // File to store track data
static const char *FRIENDS_DATABASE_FILENAME = "/friends.bin"; // File to store friends data
static const char *STATS_DATABASE_FILENAME = "/stats.bin";     // File to store personal stats
static const char *LAST_STATUS_FILENAME = "/last_status.bin";  // File to store last status

inline void pauseAndRestart()
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // Delay to allow logs to flush
    ESP.restart();                   // Restart the ESP32
}

enum StorageManagerActionType
{
    ACTION_ADD_ALBUM,
    ACTION_UPDATE_STATUS,
    ACTION_ADD_FRIEND,

    ACTION_INCREMENT_TRACK_PLAYCOUNT, // Increment track playcount

    ACTION_GET_ALBUM,   // Get album data
    ACTION_GET_TRACK,   // Get track data
    ACTION_GET_FRIENDS, // Get friends list
    ACTION_GET_STATS,   // Get personal stats

};

struct StorageManagerAction
{
    StorageManagerActionType actionType;
    void *data;                            // Pointer to data associated with the action
    SemaphoreHandle_t completionSemaphore; // For blocking GET operations
};

// wild name
// album id and track id can be invalid when submitting
struct StorageManagerAlbumSubmittingPayload
{
    local_album_t album;   // Album data to submit
    local_track_t *tracks; // Pointer to an array of local_track_t structures
    size_t trackCount;     // Number of tracks in the array
};

// storage namespaces
#define STORAGE_NAMESPACE_STATS "stats"
#define STORAGE_NAMESPACE_LAST_STATUS "last_status"

QueueHandle_t xStorageManagerQueue;
TaskHandle_t xStorageManagerTaskHandle = NULL;

void storageManagerTask(void *pvParameters);

void storageManagerStart()
{
    // Create the storage manager task
    BaseType_t xReturned = xTaskCreate(
        storageManagerTask,          /* Task function. */
        "StorageManagerTask",        /* Name of task. */
        10000,                       /* Stack size of task (bytes) */
        NULL,                        /* Parameter of the task */
        1,                           /* Priority of the task */
        &xStorageManagerTaskHandle); /* Task handle */

    if (xReturned != pdPASS)
    {
        ESP_LOGE(TAG_STORAGE, "Failed to create Storage Manager Task");
        pauseAndRestart(); // Restart if task creation fails
    }
}

// data will be malloc'd by the caller regardless of action type
// e.g. for GET actions, the caller will malloc an empty struct to be filled
// similarly, SET actions will have data malloc'd by the caller
// also, this will block if the queue is full
void storageManagerAction(StorageManagerActionType actionType, void *data, size_t dataSize, bool isGet = false)
{
    if (xStorageManagerQueue == NULL)
    {
        ESP_LOGE(TAG_STORAGE, "Storage Manager Queue is not initialized");
        pauseAndRestart(); // Consistent with original behavior
        return;            // Should not be reached
    }

    StorageManagerAction action_item;
    action_item.actionType = actionType;
    action_item.completionSemaphore = NULL; // Initialize semaphore handle
    void *dataForQueue = NULL;

    if (isGet)
    {
        // For GET actions, data is a pointer to caller's buffer to be filled
        if (data == NULL)
        {
            ESP_LOGE(TAG_STORAGE, "Data pointer is NULL for GET action");
            pauseAndRestart();
            return;
        }
        dataForQueue = data; // Use caller's buffer

        action_item.completionSemaphore = xSemaphoreCreateBinary();
        if (action_item.completionSemaphore == NULL)
        {
            ESP_LOGE(TAG_STORAGE, "Failed to create semaphore for GET action");
            pauseAndRestart(); // Critical failure
            return;
        }
    }
    else
    {
        // For non-GET actions, do a deep copy of the data
        // since we don't know when the data will go out of scope
        if (dataSize > 0 && data != NULL)
        {
            dataForQueue = malloc(dataSize);
            if (dataForQueue == NULL)
            {
                ESP_LOGE(TAG_STORAGE, "Failed to allocate memory for data copy (non-GET)");
                pauseAndRestart();
                return;
            }
            memcpy(dataForQueue, data, dataSize);
        }
        else
        {
            dataForQueue = NULL; // No data or zero size
        }
    }

    action_item.data = dataForQueue;

    ESP_LOGI(TAG_STORAGE, "Queueing action type %d.", action_item.actionType);
    if (xQueueSend(xStorageManagerQueue, &action_item, portMAX_DELAY) == pdPASS)
    {
        ESP_LOGI(TAG_STORAGE, "Action type %d successfully queued.", action_item.actionType);

        if (isGet && action_item.completionSemaphore != NULL)
        {
            ESP_LOGI(TAG_STORAGE, "Waiting for GET action type %d to complete...", action_item.actionType);
            if (xSemaphoreTake(action_item.completionSemaphore, portMAX_DELAY) == pdTRUE)
            {
                ESP_LOGI(TAG_STORAGE, "GET action type %d completed.", action_item.actionType);
            }
            else
            {
                ESP_LOGE(TAG_STORAGE, "Failed to take semaphore for GET action type %d (timeout or error).", action_item.actionType);
                // should not happen as portMAX_DELAY ensures it waits indefinitely
                pauseAndRestart();
            }
            vSemaphoreDelete(action_item.completionSemaphore);
            action_item.completionSemaphore = NULL;
        }
    }
    else
    {
        ESP_LOGE(TAG_STORAGE, "Failed to send action type %d to queue.", action_item.actionType);
        // Cleanup allocated resources before restart
        if (!isGet && dataForQueue != NULL)
        {
            free(dataForQueue); // This was the dataCopy for non-GET
        }
        if (isGet && action_item.completionSemaphore != NULL)
        {
            // Semaphore was created but queue send failed
            vSemaphoreDelete(action_item.completionSemaphore);
            action_item.completionSemaphore = NULL;
        }
        ESP.restart(); // TODO: better error handling
    }
}

void initPersonalStats(char *username, char *bio, char *url)
{
    // Initialize personal stats with provided username, bio, and URL
    File statsFile = LittleFS.open(STATS_DATABASE_FILENAME, FILE_WRITE);
    if (!statsFile)
    {
        ESP_LOGE(TAG_STORAGE, "Failed to open personal stats file for writing");
        pauseAndRestart(); // Restart if file opening fails
    }
    // Initialize the personal stats file with default values
    personal_stats_t stats = {0}; // Zero-initialize the struct

    // Copy strings to the struct fields
    strncpy(stats.username, username, sizeof(stats.username) - 1);
    strncpy(stats.bio, bio, sizeof(stats.bio) - 1);
    strncpy(stats.url, url, sizeof(stats.url) - 1);

    // Ensure null termination
    stats.username[sizeof(stats.username) - 1] = '\0';
    stats.bio[sizeof(stats.bio) - 1] = '\0';

    // fill albums with invalid IDs
    for (int i = 0; i < 5; i++)
    {
        stats.fav_albums_id[i] = 0xFFFF; // Set favorite albums to invalid ID
    }
    for (int i = 0; i < 15; i++)
    {
        stats.fav_tracks_id[i] = 0xFFFF; // Set favorite tracks to invalid ID
    }

    stats.url[sizeof(stats.url) - 1] = '\0';
    size_t writtenSize = statsFile.write((uint8_t *)&stats, sizeof(personal_stats_t)); // Write the personal stats to the file
    if (writtenSize != sizeof(personal_stats_t))
    {
        ESP_LOGE(TAG_STORAGE, "Failed to write personal stats to file, written size: %zu", writtenSize);
        statsFile.close(); // Close the file before restarting
        pauseAndRestart(); // Restart if write fails
    }
    ESP_LOGI(TAG_STORAGE, "Personal stats initialized with username: %s", username);
    statsFile.close(); // Close the file after writing
}

bool readPersonalStats(personal_stats_t *stats)
{
    // Read personal stats from the file
    File statsFile = LittleFS.open(STATS_DATABASE_FILENAME, FILE_READ);
    if (!statsFile)
    {
        ESP_LOGE(TAG_STORAGE, "Failed to open personal stats file for reading");
        return false; // Return false if file opening fails
    }
    size_t readSize = statsFile.read((uint8_t *)stats, sizeof(personal_stats_t)); // Read personal stats from file
    if (readSize != sizeof(personal_stats_t))
    {
        ESP_LOGE(TAG_STORAGE, "Failed to read personal stats from file, read size: %zu", readSize);
        statsFile.close(); // Close the file before returning
        return false;      // Return false if read fails
    }
    statsFile.close(); // Close the file after reading
    return true;       // Return true if read is successful
}

bool writePersonalStats(const personal_stats_t *stats)
{
    // Write personal stats to the file
    File statsFile = LittleFS.open(STATS_DATABASE_FILENAME, "r+");
    if (!statsFile)
    {
        ESP_LOGE(TAG_STORAGE, "Failed to open personal stats file for writing");
        return false; // Return false if file opening fails
    }
    size_t writtenSize = statsFile.write((uint8_t *)stats, sizeof(personal_stats_t)); // Write the personal stats to the file
    if (writtenSize != sizeof(personal_stats_t))
    {
        ESP_LOGE(TAG_STORAGE, "Failed to write personal stats to file, written size: %zu", writtenSize);
        statsFile.close(); // Close the file before returning
        return false;      // Return false if write fails
    }
    ESP_LOGI(TAG_STORAGE, "Personal stats updated with username: %s", stats->username);
    statsFile.close(); // Close the file after writing
    return true;       // Return true if write is successful
}

// Note: Littlefs should be initialized before calling this function
void initFSDatabases()
{
    if (!LittleFS.begin()) // Initialize LittleFS for file storage
    {
        // format and restart if initialization fails
        ESP_LOGE(TAG_STORAGE, "Failed to initialize LittleFS, formatting and restarting");
        LittleFS.format();
        pauseAndRestart(); // Restart if file system initialization fails
    }

    // check if databases exist
    // both databases should be in sync
    if (!LittleFS.exists(ALBUM_DATABASE_FILENAME) || !LittleFS.exists(TRACK_DATABASE_FILENAME))
    {
        ESP_LOGI(TAG_STORAGE, "One or more database files do not exist!");
        ESP_LOGI(TAG_STORAGE, "Creating: %s", ALBUM_DATABASE_FILENAME);
        // Create empty album database file
        File albumFile = LittleFS.open(ALBUM_DATABASE_FILENAME, FILE_WRITE); // Create the album database file
        if (!albumFile)
        {
            ESP_LOGE(TAG_STORAGE, "Failed to create album database file");
            pauseAndRestart(); // Restart if file creation fails
        }
        albumFile.close(); // Close the file after creation
        ESP_LOGI(TAG_STORAGE, "Album database file created: %s", ALBUM_DATABASE_FILENAME);

        // Create empty track database file
        ESP_LOGI(TAG_STORAGE, "Creating: %s", TRACK_DATABASE_FILENAME);
        File trackFile = LittleFS.open(TRACK_DATABASE_FILENAME, FILE_WRITE); // Create the track database file
        if (!trackFile)
        {
            ESP_LOGE(TAG_STORAGE, "Failed to create track database file");
            pauseAndRestart(); // Restart if file creation fails
        }
        trackFile.close(); // Close the file after creation
        ESP_LOGI(TAG_STORAGE, "Track database file created: %s", TRACK_DATABASE_FILENAME);
    }
    else
    {
        ESP_LOGI(TAG_STORAGE, "Album and track database files already exist");
    }
    // TODO: more sanity check the files for corruption
    // Gather free space after creating the files
    size_t freeSpace = LittleFS.totalBytes() - LittleFS.usedBytes();
    float freeSpacePercent = (float)freeSpace / LittleFS.totalBytes() * 100.0f; // Calculate free space percentage
    ESP_LOGI(TAG_STORAGE, "LittleFS initialized with free space: %d bytes, %f percent free", freeSpace, freeSpacePercent);
}

void storageManagerTask(void *pvParameters)
{
    ESP_LOGI(TAG_STORAGE, "Storage Manager Task started");
    if (!LittleFS.begin()) // Initialize LittleFS for file storage
    {
        // format and restart if initialization fails
        ESP_LOGE(TAG_STORAGE, "Failed to initialize LittleFS, formatting and restarting");
        LittleFS.format();
        pauseAndRestart();
    }
    xStorageManagerQueue = xQueueCreate(10, sizeof(StorageManagerAction)); // Create a queue for action types
    if (xStorageManagerQueue == NULL)
    {
        ESP_LOGE(TAG_STORAGE, "Failed to create storage manager queue");
        pauseAndRestart();
    }

    // check if personal stats are initialized, if not, initialize them
    File statsFile = LittleFS.open(STATS_DATABASE_FILENAME, "r");
    if (!LittleFS.exists(STATS_DATABASE_FILENAME) ||
        (statsFile.size() != sizeof(personal_stats_t)))
    {
        ESP_LOGI(TAG_STORAGE, "Personal stats not initialized, initializing with default values");
        statsFile.close(); // Close the file before initializing
        initPersonalStats("default_user", "This is a default bio", "https://example.com");
    }
    else
    {
        personal_stats_t stats;
        statsFile.read((uint8_t *)&stats, sizeof(personal_stats_t)); // Read personal stats from file
        ESP_LOGI(TAG_STORAGE, "Personal stats already initialized with username: %s",
                 stats.username);
        statsFile.close(); // Close the file after reading
    }

    initFSDatabases(); // Initialize the file system databases

    while (1)
    {
        // wait for queue
        StorageManagerAction action;
        xQueueReceive(xStorageManagerQueue, &action, portMAX_DELAY);
        if (action.data == NULL)
        {
            ESP_LOGI(TAG_STORAGE, "Received action with null data");
            // TODO: Check whether is type of GET action
        }

        switch (action.actionType)
        {
        case ACTION_ADD_ALBUM:
        {
            // Album: put playcount, first/last listen date to preferences
            // rest of the album data is stored in LittleFS
            ESP_LOGI(TAG_STORAGE, "Adding album");
            StorageManagerAlbumSubmittingPayload *albumPayload = (StorageManagerAlbumSubmittingPayload *)action.data; // Cast data to StorageManagerAlbumSubmittingPayload
            if (albumPayload == NULL)
            {
                ESP_LOGE(TAG_STORAGE, "Data pointer is NULL for ADD_ALBUM action");
                pauseAndRestart(); // Restart if data pointer is NULL
            }
            local_album_t *albumData = &albumPayload->album;  // Cast data to local_album_t
            local_track_t *tracksData = albumPayload->tracks; // Pointer to tracks data
            size_t trackCount = albumPayload->trackCount;     // Number of tracks in the album
            // check whether album and tracks points to null
            if (albumData == NULL || tracksData == NULL)
            {
                ESP_LOGE(TAG_STORAGE, "Album data or tracks pointer is NULL for ADD_ALBUM action");
                pauseAndRestart(); // Restart if album data or tracks pointer is NULL
            }
            if (trackCount == 0 || trackCount > MAX_TRACKS_PER_ALBUM)
            {
                ESP_LOGE(TAG_STORAGE, "Invalid track count for ADD_ALBUM action: %zu", trackCount);
                pauseAndRestart(); // Restart if track count is invalid
            }

            // read personal stats to check album count
            personal_stats_t personalStats = {0}; // Initialize personal stats structure
            ESP_LOGI(TAG_STORAGE, "Reading personal stats...");
            if (readPersonalStats(&personalStats) == false)
            {
                ESP_LOGE(TAG_STORAGE, "Failed to read personal stats");
                pauseAndRestart(); // Restart if reading personal stats fails
            }
            ESP_LOGI(TAG_STORAGE, "Done reading personal stats.");

            // check current album count
            if (personalStats.total_albums >= MAX_ALBUMS)
            {
                // TODO: if albumCount (or trackCount + album.track_count) exceeds threshold then delete oldest album + its track
                ESP_LOGE(TAG_STORAGE, "Album count exceeds maximum limit: %d", MAX_ALBUMS);
                pauseAndRestart(); // Restart if album count exceeds maximum limit
            }

            // Now store the album data in LittleFS
            ESP_LOGI(TAG_STORAGE, "Opening album file for appending...");
            File albumFile = LittleFS.open(ALBUM_DATABASE_FILENAME, FILE_APPEND); // Open file in append mode
            if (!albumFile)
            {
                ESP_LOGE(TAG_STORAGE, "Failed to open album file for appending");
                free(albumData); // Free allocated memory for album
                while (1)
                {
                    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait indefinitely
                }
            }
            ESP_LOGI(TAG_STORAGE, "Done opening album file.");
            ESP_LOGI(TAG_STORAGE, "Appending album data...");
            albumFile.write((uint8_t *)albumData, sizeof(local_album_t)); // Append album data to file
            ESP_LOGI(TAG_STORAGE, "Done writing to album file.");

            // update tracks - optimized to open file once for all tracks
            ESP_LOGI(TAG_STORAGE, "Opening track file for appending...");
            File trackFile = LittleFS.open(TRACK_DATABASE_FILENAME, FILE_APPEND); // Open file in append mode
            if (!trackFile)
            {
                ESP_LOGE(TAG_STORAGE, "Failed to open track file for appending");
                albumFile.close(); // Close album file before restarting
                pauseAndRestart(); // Restart if file opening fails
            }
            ESP_LOGI(TAG_STORAGE, "Done opening track file.");

            // Write all tracks in one batch operation using append mode
            ESP_LOGI(TAG_STORAGE, "Appending tracks data...");
            size_t totalTracksSize = trackCount * sizeof(local_track_t);
            size_t actualWritten = trackFile.write((uint8_t *)tracksData, totalTracksSize);
            ESP_LOGI(TAG_STORAGE, "Done writing to track file.");

            if (actualWritten != totalTracksSize)
            {
                ESP_LOGE(TAG_STORAGE, "Failed to write all tracks to file. Expected: %zu, Written: %zu", totalTracksSize, actualWritten);
                trackFile.close();
                albumFile.close();
                pauseAndRestart();
            }

            ESP_LOGI(TAG_STORAGE, "Closing track file...");
            trackFile.close(); // Close the file after writing all tracks
            ESP_LOGI(TAG_STORAGE, "All %zu tracks written to file: %s", trackCount, TRACK_DATABASE_FILENAME);

            // total albums should be the last one to update as it ensures valid album data
            personalStats.total_albums++;             // Increment album count for new album
            personalStats.total_tracks += trackCount; // Increment total tracks by the number of tracks in the album
            ESP_LOGI(TAG_STORAGE, "Writing updated personal stats...");
            if (!writePersonalStats(&personalStats)) // Write updated personal stats to file
            {
                ESP_LOGE(TAG_STORAGE, "Failed to write personal stats");
                pauseAndRestart(); // Restart if writing personal stats fails
            }
            ESP_LOGI(TAG_STORAGE, "Done writing personal stats.");
            ESP_LOGI(TAG_STORAGE, "Current album count: %d", personalStats.total_albums);

            ESP_LOGI(TAG_STORAGE, "Closing album file...");
            albumFile.close(); // Close the file after writing
            ESP_LOGI(TAG_STORAGE, "Album data written to file: %s", ALBUM_DATABASE_FILENAME);
            break;
        }

        case ACTION_GET_STATS:
        {
            ESP_LOGI(TAG_STORAGE, "Getting personal stats");
            personal_stats_t *stats = (personal_stats_t *)action.data; // Cast data to personal_stats_t
            if (stats == NULL)
            {
                ESP_LOGE(TAG_STORAGE, "Data pointer is NULL for GET_STATS action");
                pauseAndRestart(); // Restart if data pointer is NULL
            }
            if (!readPersonalStats(stats)) // Read personal stats from file
            {
                ESP_LOGE(TAG_STORAGE, "Failed to read personal stats from file");
                pauseAndRestart(); // Restart if reading personal stats fails
            }
            ESP_LOGI(TAG_STORAGE, "Personal stats retrieved successfully");
            break;
        }
        default:
            ESP_LOGE(TAG_STORAGE, "Unknown action type");
            break;
        }

        // Determine if the action was a GET action based on its type
        bool wasGetAction = (action.actionType == ACTION_GET_ALBUM ||
                             action.actionType == ACTION_GET_TRACK ||
                             action.actionType == ACTION_GET_FRIENDS ||
                             action.actionType == ACTION_GET_STATS);

        // For non-GET actions, if data was allocated by storageManagerAction, free it now.
        if (!wasGetAction && action.data != NULL)
        {
            free(action.data);
            action.data = NULL;
        }

        // If a completion semaphore was provided (for GET actions), signal it.
        if (action.completionSemaphore != NULL)
        {
            xSemaphoreGive(action.completionSemaphore);
        }
    }
}

void storageManagerSync()
{
    // blocks until all actions in the queue are processed
    ESP_LOGI(TAG_STORAGE, "Waiting for storage manager queue to flush...");
    while (uxQueueMessagesWaiting(xStorageManagerQueue) > 0)
    {
        // TODO: maybe without polling?
        vTaskDelay(pdMS_TO_TICKS(100)); // Wait for 100 ms before checking again
    }
    ESP_LOGI(TAG_STORAGE, "Storage manager queue flushed");
}
