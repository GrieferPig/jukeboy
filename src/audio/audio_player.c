#include "audio_player.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "audio/adpcm_decoder.h" // Assuming your custom decoder library
#include "storage_struct.h"      // Assuming your storage structure definitions
#include "macros.h"              // For utility macros like `unwrap_basetype`, `todo`, etc.
#include "esp_vfs_fat.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include <dirent.h>
#include <string.h>
#include <ctype.h>

// --- Constants and Globals ---
static const char *TAG = "AudioPlayer";

// SD Card SPI Pins (Update these to your board's configuration)
#ifdef ESP32_WROOM
#define SD_CS_PIN 5
#define SD_SCK_PIN 18
#define SD_MOSI_PIN 23
#define SD_MISO_PIN 19
#else
#define SD_CS_PIN 3
#define SD_SCK_PIN 1
#define SD_MOSI_PIN 2
#define SD_MISO_PIN 0
#endif

// Audio format constants
#define TJA_HEADER_SIZE 512U
#define ADPCM_BLOCK_SIZE 44032U
#define I2S_WRITE_CHUNK_SIZE 2048U

// --- Playback State Management ---
typedef enum
{
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_PAUSED
} PlaybackState;

volatile PlaybackState g_playback_state = STATE_STOPPED;
volatile int g_current_track = 0;
volatile int g_total_tracks = 0;
volatile uint8_t g_volume_shift = 2; // Default to 25% volume
volatile uint8_t g_volume_level = 4; // Volume level 1-10, 11=mute
volatile bool g_shuffle_enabled = false;
volatile int g_playlist_position = 0;

// Shuffle playlist: holds track indices in play order
static int *g_playlist = NULL;

// Time tracking for seeking
volatile uint32_t g_current_position_ms = 0;
volatile uint32_t g_seek_position_ms = 0;
volatile bool g_seek_requested = false;

// --- RTOS Handles ---
TaskHandle_t playerTaskHandle = NULL;
TaskHandle_t readerTaskHandle = NULL;
TaskHandle_t decoderTaskHandle = NULL;
i2s_chan_handle_t g_tx_handle = NULL; // Share I2S handle for pausing
QueueHandle_t audio_command_queue;
QueueHandle_t full_buffer_queue;
QueueHandle_t empty_buffer_queue;
EventGroupHandle_t player_event_group;

// Event bits for the event group
#define TRACK_FINISHED_BIT (1 << 0)
#define READER_TASK_EXITED_BIT (1 << 1)
#define DECODER_TASK_EXITED_BIT (1 << 2)

// --- Buffers ---
static uint8_t adpcm_buffer_A[ADPCM_BLOCK_SIZE];
static uint8_t adpcm_buffer_B[ADPCM_BLOCK_SIZE];
static int16_t i2s_write_chunk_buffer[I2S_WRITE_CHUNK_SIZE / sizeof(int16_t)];

// --- Task Forward Declarations ---
void audio_player_task(void *pvParameters);
void sd_reader_task(void *pvParameters);
void decoder_task(void *pvParameters);

// --- Helper Functions ---

/**
 * @brief Applies volume control to a PCM buffer using bit-shifting.
 */
static inline void apply_gain(int16_t *pcm_buffer, size_t sample_count)
{
    if (g_volume_shift == 0)
        return;
    uint8_t shift = (g_volume_shift > 15) ? 15 : g_volume_shift;
    for (size_t i = 0; i < sample_count; ++i)
    {
        pcm_buffer[i] = pcm_buffer[i] >> shift;
    }
}

/**
 * @brief Stops playback, cleans up tasks, queues, and I2S.
 */
static void stop_playback()
{
    ESP_LOGI(TAG, "Stopping playback...");

    // Signal tasks to exit if they are running
    g_playback_state = STATE_STOPPED;

    // Wait for tasks to exit gracefully. They will delete themselves.
    if (readerTaskHandle || decoderTaskHandle)
    {
        ESP_LOGI(TAG, "Waiting for reader and decoder tasks to exit...");
        EventBits_t bits_to_wait_for = 0;
        if (readerTaskHandle)
            bits_to_wait_for |= READER_TASK_EXITED_BIT;
        if (decoderTaskHandle)
            bits_to_wait_for |= DECODER_TASK_EXITED_BIT;

        xEventGroupWaitBits(player_event_group,
                            bits_to_wait_for,
                            pdTRUE,               // Clear bits on exit
                            pdTRUE,               // Wait for all bits
                            pdMS_TO_TICKS(1000)); // Timeout
    }

    // Tasks are now gone, it's safe to clean up.
    decoderTaskHandle = NULL;
    readerTaskHandle = NULL;

    // Delete and recreate queues to flush them
    if (full_buffer_queue)
        vQueueDelete(full_buffer_queue);
    if (empty_buffer_queue)
        vQueueDelete(empty_buffer_queue);

    full_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));
    empty_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));

    // Pre-fill the empty queue for the next playback
    uint8_t *buf_a_ptr = adpcm_buffer_A;
    uint8_t *buf_b_ptr = adpcm_buffer_B;
    xQueueSend(empty_buffer_queue, &buf_a_ptr, 0);
    xQueueSend(empty_buffer_queue, &buf_b_ptr, 0);

    ESP_LOGI(TAG, "Playback stopped and resources cleaned.");
}

/**
 * @brief Starts playback of a specific track number.
 * @param track_number The track to play.
 */
static void start_playback(int track_number)
{
    if (g_playback_state != STATE_STOPPED)
    {
        stop_playback();
    }

    if (track_number < 0 || track_number >= g_total_tracks)
    {
        ESP_LOGE(TAG, "Invalid track number: %d", track_number);
        return;
    }

    g_current_track = track_number;
    ESP_LOGI(TAG, "Starting playback for track %d", g_current_track);

    g_playback_state = STATE_PLAYING;
    xEventGroupClearBits(player_event_group, TRACK_FINISHED_BIT | READER_TASK_EXITED_BIT | DECODER_TASK_EXITED_BIT);

    // Create tasks for the new track
    xTaskCreate(sd_reader_task, "SDReaderTask", 4096, (void *)g_current_track, 5, &readerTaskHandle);
    xTaskCreate(decoder_task, "DecoderTask", 4096, NULL, 10, &decoderTaskHandle);
}

/**
 * @brief Updates volume shift based on volume level (1-10, 11=mute)
 */
static void update_volume_shift()
{
    if (g_volume_level == 11) // Mute
    {
        g_volume_shift = 15; // Maximum attenuation
    }
    else if (g_volume_level >= 1 && g_volume_level <= 10)
    {
        // Map volume level 1-10 to shift 8-0 (quieter to louder)
        g_volume_shift = 8 - ((g_volume_level - 1) * 8 / 9);
    }
}

/**
 * @brief Creates a shuffled playlist with current track first
 */
static void create_shuffled_playlist()
{
    if (!g_playlist)
    {
        g_playlist = (int *)malloc(g_total_tracks * sizeof(int));
        if (!g_playlist)
        {
            ESP_LOGE(TAG, "Failed to allocate playlist memory");
            return;
        }
    }

    // Put current track as first in playlist
    g_playlist[0] = g_current_track;

    // Fill remaining positions with other tracks
    int playlist_idx = 1;
    for (int track = 0; track < g_total_tracks; track++)
    {
        if (track != g_current_track)
        {
            g_playlist[playlist_idx++] = track;
        }
    }

    // Shuffle remaining tracks (Fisher-Yates shuffle)
    for (int i = g_total_tracks - 1; i > 1; i--)
    {
        int j = 1 + (rand() % (i - 1 + 1)); // Random between 1 and i
        int temp = g_playlist[i];
        g_playlist[i] = g_playlist[j];
        g_playlist[j] = temp;
    }

    g_playlist_position = 0;
    ESP_LOGI(TAG, "Created shuffled playlist, current track %d at position 0", g_current_track);
}

/**
 * @brief Gets the next track based on shuffle state
 */
static int get_next_track()
{
    if (g_shuffle_enabled)
    {
        g_playlist_position = (g_playlist_position + 1) % g_total_tracks;
        return g_playlist[g_playlist_position];
    }
    else
    {
        return (g_current_track + 1) % g_total_tracks;
    }
}

/**
 * @brief Gets the previous track based on shuffle state
 */
static int get_prev_track()
{
    if (g_shuffle_enabled)
    {
        g_playlist_position = (g_playlist_position - 1 + g_total_tracks) % g_total_tracks;
        return g_playlist[g_playlist_position];
    }
    else
    {
        return (g_current_track - 1 + g_total_tracks) % g_total_tracks;
    }
}

// --- Main Tasks ---

/**
 * @brief SD Reader Task (Producer)
 * Reads a single audio file from the SD card block by block.
 */
void sd_reader_task(void *pvParameters)
{
    int track_to_play = (int)pvParameters;
    // char filename[16];
    // snprintf(filename, sizeof(filename), "/%02x.tja", track_to_play);
    char filename[32];
    snprintf(filename, sizeof(filename), "/sdcard/%02x.tja", track_to_play);

    ESP_LOGI(TAG, "Reader task started for %s", filename);

    // File audioFile = SD.open(filename, FILE_READ);
    // if (!audioFile || !audioFile.seek(TJA_HEADER_SIZE))
    FILE *audioFile = fopen(filename, "rb");
    if (audioFile == NULL || fseek(audioFile, TJA_HEADER_SIZE, SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to open or seek %s", filename);
        xEventGroupSetBits(player_event_group, TRACK_FINISHED_BIT);
        vTaskDelete(NULL);
        return;
    }

    // Handle seek if requested
    if (g_seek_requested)
    {
        uint32_t seek_bytes = (g_seek_position_ms * 44100 * 4) / 1000;   // Approximate bytes for seek position
        seek_bytes = (seek_bytes / ADPCM_BLOCK_SIZE) * ADPCM_BLOCK_SIZE; // Align to block boundary
        // audioFile.seek(TJA_HEADER_SIZE + seek_bytes);
        fseek(audioFile, TJA_HEADER_SIZE + seek_bytes, SEEK_SET);
        g_current_position_ms = g_seek_position_ms;
        g_seek_requested = false;
        ESP_LOGI(TAG, "Seeked to position %lu ms", g_current_position_ms);
    }

    while (g_playback_state != STATE_STOPPED)
    {
        uint8_t *buffer_to_fill = NULL;
        if (xQueueReceive(empty_buffer_queue, &buffer_to_fill, pdMS_TO_TICKS(100)) != pdPASS)
        {
            continue;
        }

        // size_t bytesRead = audioFile.read(buffer_to_fill, ADPCM_BLOCK_SIZE);
        size_t bytesRead = fread(buffer_to_fill, 1, ADPCM_BLOCK_SIZE, audioFile);
        if (bytesRead < ADPCM_BLOCK_SIZE)
        {
            // End of file
            ESP_LOGI(TAG, "End of file: %s", filename);
            // audioFile.close();
            fclose(audioFile);
            xEventGroupSetBits(player_event_group, TRACK_FINISHED_BIT);
            break; // Exit the loop
        }

        // Update position (approximate)
        g_current_position_ms += (ADPCM_BLOCK_SIZE * 1000) / (44100 * 4);

        if (xQueueSend(full_buffer_queue, &buffer_to_fill, portMAX_DELAY) != pdPASS)
        {
            ESP_LOGE(TAG, "Failed to send to full_buffer_queue. Stopping.");
            g_playback_state = STATE_STOPPED; // Force stop on queue error
        }
    }

    // audioFile.close();
    fclose(audioFile);
    ESP_LOGI(TAG, "Reader task exiting.");
    xEventGroupSetBits(player_event_group, READER_TASK_EXITED_BIT);
    vTaskDelete(NULL);
}

/**
 * @brief Decoder Task (Consumer)
 * Decodes ADPCM blocks and sends them to the I2S peripheral.
 */
void decoder_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Decoder Task started.");

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &g_tx_handle, NULL);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };
    i2s_channel_init_std_mode(g_tx_handle, &std_cfg);
    i2s_channel_enable(g_tx_handle);

    DecoderContext decoder_ctx;
    bool i2s_is_enabled = true;

    while (g_playback_state != STATE_STOPPED)
    {
        // Handle resume state changes
        if (g_playback_state == STATE_PLAYING && !i2s_is_enabled)
        {
            i2s_channel_enable(g_tx_handle);
            i2s_is_enabled = true;
            ESP_LOGI(TAG, "I2S channel enabled (resumed).");
        }
        else if (g_playback_state == STATE_PAUSED && i2s_is_enabled)
        {
            // The player task now handles disabling, but we need to update our state
            i2s_is_enabled = false;
        }

        // If paused, wait here before trying to receive from queue
        if (g_playback_state == STATE_PAUSED)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        uint8_t *adpcm_block_to_decode = NULL;

        // Wait for a full buffer. If the reader has exited and the queue is empty, we are done.
        if (xQueueReceive(full_buffer_queue, &adpcm_block_to_decode, pdMS_TO_TICKS(100)) != pdPASS)
        {
            EventBits_t bits = xEventGroupGetBits(player_event_group);
            if ((bits & READER_TASK_EXITED_BIT) || (bits & TRACK_FINISHED_BIT))
            {
                ESP_LOGI(TAG, "Decoder detected reader exit and empty queue. Finishing up.");
                break; // Exit the main loop
            }
            continue; // Otherwise, just keep waiting for data
        }

        adpcm_decoder_init(&decoder_ctx, adpcm_block_to_decode, ADPCM_BLOCK_SIZE);

        while (true)
        {
            size_t pcm_bytes_decoded = adpcm_decode_chunk(&decoder_ctx, i2s_write_chunk_buffer, I2S_WRITE_CHUNK_SIZE);
            if (pcm_bytes_decoded == 0)
                break;

            apply_gain(i2s_write_chunk_buffer, pcm_bytes_decoded / 2);

            size_t bytes_written = 0;
            // State is guaranteed to be PLAYING here due to the check at the start of the loop
            i2s_channel_write(g_tx_handle, i2s_write_chunk_buffer, pcm_bytes_decoded, &bytes_written, portMAX_DELAY);
        }
        xQueueSend(empty_buffer_queue, &adpcm_block_to_decode, portMAX_DELAY);
    }

    if (i2s_is_enabled)
    {
        i2s_channel_disable(g_tx_handle);
    }
    i2s_del_channel(g_tx_handle);
    g_tx_handle = NULL;
    ESP_LOGI(TAG, "Decoder task exiting.");
    xEventGroupSetBits(player_event_group, DECODER_TASK_EXITED_BIT);
    vTaskDelete(NULL);
}

void init_sdcard()
{
    esp_err_t ret;
    sdmmc_card_t *card = NULL;
    const char *mount_point = "/sdcard";

    // Configure the SD card mount
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024};

    // Configure SPI bus
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_MOSI_PIN,
        .miso_io_num = SD_MISO_PIN,
        .sclk_io_num = SD_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
        return;
    }

    // Configure SPI device
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SPI2_HOST;

    // Mount the SD card
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "SD card mounted successfully");
}

/**
 * @brief Main Audio Player Control Task
 * Manages state and handles commands.
 */
void audio_player_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio Player control task started.");

    // Initialize SD card
    init_sdcard();
    ESP_LOGI(TAG, "SD Card initialized.");

    // --- Scan SD card for tracks ---
    // File root = SD.open("/");
    // if (!root)
    // {
    //     ESP_LOGE(TAG, "Failed to open root directory. Stopping.");
    //     vTaskDelete(NULL);
    //     return;
    // }
    // if (!root.isDirectory())
    // {
    //     ESP_LOGE(TAG, "Root is not a directory. Stopping.");
    //     // root.close();
    //     fclose(root);
    //     vTaskDelete(NULL);
    //     return;
    // }

    int track_count = 0;
    // File file = root.openNextFile();
    // while (file)
    // {
    //     if (!file.isDirectory())
    //     {
    //         const char *fname = file.name();
    //         // Check for format "xx.tja" (e.g., "0A.tja", "1F.tja")
    //         if (strlen(fname) == 6 && isxdigit(fname[0]) && isxdigit(fname[1]) && strcmp(fname + 2, ".tja") == 0)
    //         {
    //             track_count++;
    //         }
    //     }
    //     file.close();
    //     file = root.openNextFile();
    // }
    // root.close();

    DIR *dir = opendir("/sdcard");
    if (dir == NULL)
    {
        ESP_LOGE(TAG, "Failed to open directory /sdcard");
        vTaskDelete(NULL);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        // ESP_LOGI(TAG, "Found file: %s", entry->d_name);
        // Check for format "xx.tja" (e.g., "0A.tja", "1F.tja")
        if (strlen(entry->d_name) == 6 &&
            isxdigit(entry->d_name[0]) &&
            isxdigit(entry->d_name[1]) &&
            strcmp(entry->d_name + 2, ".TJA") == 0)
        {
            track_count++;
        }
    }
    closedir(dir);
    g_total_tracks = track_count;
    ESP_LOGI(TAG, "Found %d tracks on SD card.", g_total_tracks);
    if (g_total_tracks == 0)
    {
        ESP_LOGW(TAG, "No tracks found. Player will be idle.");
        todo("report no tracks to the main application");
    }

    // Initialize volume
    update_volume_shift();

    AudioCommand cmd;

    ESP_LOGI(TAG, "Audio Player control task ready. Waiting for commands...");

    while (true)
    {
        // Wait for a command from the queue OR a track finished event
        EventBits_t bits = xEventGroupWaitBits(player_event_group, TRACK_FINISHED_BIT, pdTRUE, pdFALSE, 0); // Check without waiting

        if (bits & TRACK_FINISHED_BIT)
        {
            ESP_LOGI(TAG, "Track finished event received. Advancing to next track.");
            stop_playback();
            int next_track = get_next_track();
            start_playback(next_track);
        }

        BaseType_t res = xQueueReceive(audio_command_queue, &cmd, pdMS_TO_TICKS(50));

        if (res == pdPASS)
        {
            switch (cmd.type)
            {
            case CMD_PLAY_TRACK:
                ESP_LOGI(TAG, "CMD: PLAY_TRACK %d", cmd.params.track_number);
                stop_playback();
                start_playback(cmd.params.track_number);
                break;

            case CMD_TOGGLE_PAUSE:
                if (g_playback_state == STATE_PLAYING)
                {
                    g_playback_state = STATE_PAUSED;
                    if (g_tx_handle)
                    {
                        i2s_channel_disable(g_tx_handle);
                        ESP_LOGI(TAG, "CMD: PAUSE - I2S channel disabled.");
                    }
                    else
                    {
                        ESP_LOGI(TAG, "CMD: PAUSE");
                    }
                }
                else if (g_playback_state == STATE_PAUSED)
                {
                    g_playback_state = STATE_PLAYING;
                    ESP_LOGI(TAG, "CMD: RESUME");
                }
                break;

            case CMD_NEXT_TRACK:
                ESP_LOGI(TAG, "CMD: NEXT_TRACK");
                stop_playback();
                start_playback(get_next_track());
                break;

            case CMD_PREV_TRACK:
                ESP_LOGI(TAG, "CMD: PREV_TRACK");
                stop_playback();
                start_playback(get_prev_track());
                break;

            case CMD_SET_VOLUME_SHIFT:
                ESP_LOGI(TAG, "CMD: SET_VOLUME_SHIFT to %d", cmd.params.volume_shift);
                if (cmd.params.volume_shift >= 0 && cmd.params.volume_shift <= 15)
                {
                    g_volume_shift = cmd.params.volume_shift;
                }
                break;

            case CMD_TOGGLE_SHUFFLE:
                g_shuffle_enabled = !g_shuffle_enabled;
                ESP_LOGI(TAG, "CMD: TOGGLE_SHUFFLE - shuffle %s", g_shuffle_enabled ? "enabled" : "disabled");
                if (g_shuffle_enabled)
                {
                    create_shuffled_playlist();
                }
                break;

            case CMD_FFWD_10SEC:
                ESP_LOGI(TAG, "CMD: FFWD_10SEC");
                if (g_playback_state == STATE_PLAYING || g_playback_state == STATE_PAUSED)
                {
                    g_seek_position_ms = g_current_position_ms + 10000; // 10 seconds
                    g_seek_requested = true;
                    stop_playback();
                    start_playback(g_current_track);
                }
                break;

            case CMD_REWIND_5SEC:
                ESP_LOGI(TAG, "CMD: REWIND_5SEC");
                if (g_playback_state == STATE_PLAYING || g_playback_state == STATE_PAUSED)
                {
                    g_seek_position_ms = (g_current_position_ms > 5000) ? g_current_position_ms - 5000 : 0;
                    g_seek_requested = true;
                    stop_playback();
                    start_playback(g_current_track);
                }
                break;

            case CMD_VOLUME_INC:
                if (g_volume_level < 10)
                {
                    g_volume_level++;
                    update_volume_shift();
                    ESP_LOGI(TAG, "CMD: VOLUME_INC - level %d", g_volume_level);
                }
                else if (g_volume_level == 11) // Unmute
                {
                    g_volume_level = 1;
                    update_volume_shift();
                    ESP_LOGI(TAG, "CMD: VOLUME_INC - unmuted to level 1");
                }
                break;

            case CMD_VOLUME_DEC:
                if (g_volume_level == 11)
                {
                    // Already muted, do nothing
                    break;
                }
                if (g_volume_level > 1)
                {
                    g_volume_level--;
                    update_volume_shift();
                    ESP_LOGI(TAG, "CMD: VOLUME_DEC - level %d", g_volume_level);
                }
                else if (g_volume_level == 1)
                {
                    g_volume_level = 11; // Mute
                    update_volume_shift();
                    ESP_LOGI(TAG, "CMD: VOLUME_DEC - muted");
                }
                break;
            default:
                ESP_LOGW(TAG, "Unknown command type: %d", cmd.type);
                break;
            }
        }
    }
}

// --- Public API Implementation ---

/**
 * @brief Initializes the audio player system.
 */
void audio_player_init()
{
    // Create command queue
    audio_command_queue = xQueueCreate(10, sizeof(AudioCommand));
    if (!audio_command_queue)
    {
        ESP_LOGE(TAG, "Failed to create command queue.");
        return;
    }

    // Create event group
    player_event_group = xEventGroupCreate();
    if (!player_event_group)
    {
        ESP_LOGE(TAG, "Failed to create event group.");
        return;
    }

    // Create buffer queues and pre-fill the empty one
    full_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));
    empty_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));
    uint8_t *buf_a_ptr = adpcm_buffer_A;
    uint8_t *buf_b_ptr = adpcm_buffer_B;
    xQueueSend(empty_buffer_queue, &buf_a_ptr, 0);
    xQueueSend(empty_buffer_queue, &buf_b_ptr, 0);

    // Create and start the main player control task
    xTaskCreate(audio_player_task, "AudioPlayerTask", 4096, NULL, 8, &playerTaskHandle);

    ESP_LOGI(TAG, "Audio player initialized. Track detection will occur in the player task.");
}

/**
 * @brief Sends a command to the audio player's control task.
 */
void audio_player_send_command(const AudioCommand *cmd)
{
    if (audio_command_queue != NULL)
    {
        xQueueSend(audio_command_queue, cmd, 0);
    }
}
