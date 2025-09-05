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
#include <stdlib.h>
#include "../hid/power_mgr.h"
#include "soc/rtc.h" // For RTC_SLOW_ATTR
#include "pindef.h"

// --- Constants and Globals ---
static const char *TAG = "AudioPlayer";

// Audio format constants
#define TJA_HEADER_SIZE 512U
#define ADPCM_BLOCK_SIZE 44032U
#define I2S_WRITE_CHUNK_SIZE 2048U
#define MAX_PLAYLIST_TRACKS 256 // Maximum possible tracks (00.tja to FF.tja)
#define RTC_STATE_MAGIC 0xDEADBEEF

// Add global SD card handle
static sdmmc_card_t *g_card = NULL;

// --- RTC Persistent State ---
typedef struct
{
    uint32_t magic;
    uint8_t volume_level;
    int current_track;
    uint32_t current_position_ms;
    bool shuffle_enabled;
    int playlist_position;
    int playlist[MAX_PLAYLIST_TRACKS];
} rtc_playback_state_t;

RTC_NOINIT_ATTR static rtc_playback_state_t rtc_state;

// --- Playback State Management ---
typedef enum
{
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_PAUSED
} PlaybackState;

typedef enum
{
    INSERTED,
    REMOVED,
} CartPresenceState;

volatile PlaybackState g_playback_state = STATE_STOPPED;
volatile int g_current_track = 0;
volatile int g_total_tracks = 0;
volatile uint8_t g_volume_shift = 2; // Default to 25% volume
volatile uint8_t g_volume_level = 4; // Volume level 1-10, 11=mute
volatile bool g_shuffle_enabled = false;
volatile int g_playlist_position = 0;

// Add flag to track SD initialization
static bool g_sd_initialized = false;

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
QueueHandle_t audio_command_queue = NULL;
QueueHandle_t full_buffer_queue = NULL;
QueueHandle_t empty_buffer_queue = NULL;
QueueHandle_t cart_presence_notify_queue = NULL;
EventGroupHandle_t player_event_group = NULL;
TimerHandle_t audio_can_sleep_timer = NULL;

// Event bits for the event group
#define TRACK_FINISHED_BIT (1 << 0)
#define READER_TASK_EXITED_BIT (1 << 1)
#define DECODER_TASK_EXITED_BIT (1 << 2)

// --- Buffers ---
static uint8_t *adpcm_buffer_A;
static uint8_t *adpcm_buffer_B;
static int16_t *i2s_write_chunk_buffer;

// --- Task Forward Declarations ---
void audio_player_task(void *pvParameters);
void sd_reader_task(void *pvParameters);
void decoder_task(void *pvParameters);
static void save_playback_state();
static void init_sd_if_needed();       // Forward declaration for helper function
static void cleanup_on_cart_removal(); // Forward declaration for helper function

// --- Helper Functions ---

/**
 * @brief Callback for the audio player idle timer. Clears the tired bit.
 */
static void audio_can_sleep_timer_callback(TimerHandle_t xTimer)
{
    ESP_LOGI(TAG, "Audio player idle timer expired, clearing tired bit.");
    xEventGroupClearBits(power_mgr_tired_event_group, AUDIO_PLAYER_TIRED_BIT);
}

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

    // Save state before stopping
    save_playback_state();

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
    // Clear tired bit
    // Start a timer to clear the tired bit after a delay
    if (audio_can_sleep_timer != NULL)
    {
        ESP_LOGI(TAG, "Starting 15-second idle timer before clearing tired bit.");
        xTimerStart(audio_can_sleep_timer, 0);
    }
}

/**
 * @brief Starts playback of a specific track number.
 * @param track_number The track to play.
 */
static void start_playback(int track_number)
{
    // Stop the idle timer if it's running, as we are now active
    if (audio_can_sleep_timer != NULL)
    {
        xTimerStop(audio_can_sleep_timer, 0);
    }
    // Set tired bit
    xEventGroupSetBits(power_mgr_tired_event_group, AUDIO_PLAYER_TIRED_BIT);

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
        int j = 1 + (rand() % (i)); // Random between 1 and i
        int temp = g_playlist[i];
        g_playlist[i] = g_playlist[j];
        g_playlist[j] = temp;
    }

    g_playlist_position = 0;
    ESP_LOGI(TAG, "Created shuffled playlist, current track %d at position 0", g_current_track);
}

/**
 * @brief Saves the current playback state to RTC memory.
 */
static void save_playback_state()
{
    if (g_total_tracks == 0)
        return;

    rtc_state.magic = RTC_STATE_MAGIC;
    rtc_state.volume_level = g_volume_level;
    rtc_state.current_track = g_current_track;
    rtc_state.current_position_ms = g_current_position_ms;
    rtc_state.shuffle_enabled = g_shuffle_enabled;
    rtc_state.playlist_position = g_playlist_position;

    if (g_shuffle_enabled && g_playlist)
    {
        memcpy(rtc_state.playlist, g_playlist, g_total_tracks * sizeof(int));
    }

    ESP_LOGI(TAG, "Playback state saved to RTC memory.");
}

/**
 * @brief Loads playback state from RTC memory if available.
 */
static void load_playback_state()
{
    if (rtc_state.magic != RTC_STATE_MAGIC)
    {
        ESP_LOGI(TAG, "No valid playback state found in RTC memory.");
        return;
    }

    // Check if the stored track is valid with the current file system
    if (rtc_state.current_track < 0 || rtc_state.current_track >= g_total_tracks)
    {
        ESP_LOGW(TAG, "RTC state has invalid track number (%d). Ignoring.", rtc_state.current_track);
        rtc_state.magic = 0; // Invalidate state
        return;
    }

    g_volume_level = rtc_state.volume_level;
    g_current_track = rtc_state.current_track;
    g_current_position_ms = rtc_state.current_position_ms;
    g_shuffle_enabled = rtc_state.shuffle_enabled;
    g_playlist_position = rtc_state.playlist_position;

    update_volume_shift(); // Apply loaded volume

    if (g_shuffle_enabled)
    {
        if (!g_playlist)
        {
            g_playlist = (int *)malloc(g_total_tracks * sizeof(int));
        }
        if (g_playlist)
        {
            memcpy(g_playlist, rtc_state.playlist, g_total_tracks * sizeof(int));
            ESP_LOGI(TAG, "Restored shuffled playlist from RTC.");
        }
        else
        {
            ESP_LOGE(TAG, "Failed to allocate memory for restored playlist!");
            g_shuffle_enabled = false; // Disable shuffle if allocation fails
        }
    }

    // If there's a playback position, set up for a seek on the first play
    if (g_current_position_ms > 0)
    {
        g_seek_position_ms = g_current_position_ms;
        g_seek_requested = true;
    }

    ESP_LOGI(TAG, "Restored playback state from RTC: Track %d, Pos %lums, Vol %d, Shuffle %s",
             g_current_track, g_current_position_ms, g_volume_level, g_shuffle_enabled ? "On" : "Off");

    rtc_state.magic = 0; // Invalidate after loading to prevent re-loading stale data on next boot without a save
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

    while (g_playback_state != STATE_STOPPED)
    {
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

    i2s_channel_disable(g_tx_handle);
    i2s_del_channel(g_tx_handle);
    g_tx_handle = NULL;
    ESP_LOGI(TAG, "Decoder task exiting.");
    xEventGroupSetBits(player_event_group, DECODER_TASK_EXITED_BIT);
    vTaskDelete(NULL);
}

esp_err_t init_sdcard()
{
    esp_err_t ret;
    // Remove local card declaration: sdmmc_card_t *card = NULL;
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
        return ret;
    }

    // Configure SPI device
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_CS_PIN;
    slot_config.host_id = SPI2_HOST;

    // Mount the SD card
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &g_card);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
    }
    else
    {
        ESP_LOGI(TAG, "SD card mounted successfully");
    }
    return ret;
}

/**
 * @brief Main Audio Player Control Task
 * Manages state and handles commands.
 */
void audio_player_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Audio Player control task started.");

    // Initial cart presence check (non-blocking setup)
    CartPresenceState current_cart_state = gpio_get_level(CART_PRESENCE_GPIO) ? REMOVED : INSERTED;
    if (current_cart_state == INSERTED)
    {
        init_sd_if_needed();
    }

    // Main daemon loop
    while (true)
    {
        // Non-blocking check for cart presence changes
        CartPresenceState new_state;
        if (xQueueReceive(cart_presence_notify_queue, &new_state, 0) == pdPASS)
        {
            if (new_state == REMOVED)
            {
                cleanup_on_cart_removal();
                // Start/reset 15s timer
                if (audio_can_sleep_timer)
                {
                    if (xTimerIsTimerActive(audio_can_sleep_timer))
                    {
                        xTimerStop(audio_can_sleep_timer, 0);
                    }
                    xTimerChangePeriod(audio_can_sleep_timer, pdMS_TO_TICKS(15000), 0);
                    xTimerStart(audio_can_sleep_timer, 0);
                }
            }
            else
            {
                // Cancel timer if active
                if (audio_can_sleep_timer && xTimerIsTimerActive(audio_can_sleep_timer))
                {
                    xTimerStop(audio_can_sleep_timer, 0);
                }
                // Set tired bit and init SD
                xEventGroupSetBits(power_mgr_tired_event_group, AUDIO_PLAYER_TIRED_BIT);
                init_sd_if_needed();
            }
        }

        // Non-blocking check for commands
        AudioCommand cmd;
        if (xQueueReceive(audio_command_queue, &cmd, 0) == pdPASS)
        {
            // Only process commands if SD is initialized and tracks exist (for relevant cmds)
            bool can_process = g_sd_initialized && g_total_tracks > 0;
            switch (cmd.type)
            {
            case CMD_PLAY_TRACK:
                if (can_process)
                {
                    ESP_LOGI(TAG, "CMD: PLAY_TRACK %d", cmd.params.track_number);
                    stop_playback();
                    start_playback(cmd.params.track_number);
                }
                else
                {
                    ESP_LOGW(TAG, "Ignoring PLAY_TRACK: No tracks available.");
                }
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
                    // Save state on pause
                    save_playback_state();
                    // Start a timer to clear the tired bit after a delay
                    if (audio_can_sleep_timer != NULL)
                    {
                        xTimerStart(audio_can_sleep_timer, 0);
                    }
                }
                else if (g_playback_state == STATE_PAUSED)
                {
                    // Stop the idle timer if it's running
                    if (audio_can_sleep_timer != NULL)
                    {
                        xTimerStop(audio_can_sleep_timer, 0);
                    }
                    // Set tired bit
                    xEventGroupSetBits(power_mgr_tired_event_group, AUDIO_PLAYER_TIRED_BIT);
                    g_playback_state = STATE_PLAYING;
                    if (g_tx_handle)
                    {
                        i2s_channel_enable(g_tx_handle);
                        ESP_LOGI(TAG, "CMD: RESUME - I2S channel enabled.");
                    }
                    else
                    {
                        ESP_LOGI(TAG, "CMD: RESUME");
                    }
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

            case CMD_SHUTDOWN: // New command for graceful exit
                ESP_LOGI(TAG, "CMD: SHUTDOWN - Exiting daemon.");
                stop_playback();
                vTaskDelete(NULL);
                break;

            default:
                ESP_LOGW(TAG, "Unknown command type: %d", cmd.type);
                break;
            }
        }

        // Check for track finished event (non-blocking)
        EventBits_t bits = xEventGroupGetBits(player_event_group);
        if (bits & TRACK_FINISHED_BIT)
        {
            xEventGroupClearBits(player_event_group, TRACK_FINISHED_BIT);
            if (g_sd_initialized && g_total_tracks > 0)
            {
                ESP_LOGI(TAG, "Track finished. Advancing to next.");
                stop_playback();
                int next_track = get_next_track();
                start_playback(next_track);
            }
        }

        // Small delay to prevent busy-waiting
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void IRAM_ATTR cart_presense_gpio_isr_handler(void *arg)
{
    // get the current state of the pin
    CartPresenceState state = gpio_get_level(CART_PRESENCE_GPIO) ? REMOVED : INSERTED;
    // Cart removed, request change to idle state
    xQueueSendFromISR(cart_presence_notify_queue, &state, NULL);
}

// --- Public API Implementation ---

/**
 * @brief Initializes the audio player system.
 */
void audio_player_init()
{
    // Allocate buffers on heap only if not already allocated
    // so that subsequent audio_player runs can reuse these buffers
    if (adpcm_buffer_A == NULL)
    {
        adpcm_buffer_A = malloc(ADPCM_BLOCK_SIZE);
        panic_if(!adpcm_buffer_A, "Failed to allocate adpcm_buffer_A");
    }
    if (adpcm_buffer_B == NULL)
    {
        adpcm_buffer_B = malloc(ADPCM_BLOCK_SIZE);
        panic_if(!adpcm_buffer_B, "Failed to allocate adpcm_buffer_B");
    }
    if (i2s_write_chunk_buffer == NULL)
    {
        i2s_write_chunk_buffer = malloc(I2S_WRITE_CHUNK_SIZE);
        panic_if(!i2s_write_chunk_buffer, "Failed to allocate i2s_write_chunk_buffer");
    }

    // Create command queue
    audio_command_queue = xQueueCreate(10, sizeof(AudioCommand));
    panic_if(!audio_command_queue, "Failed to create audio command queue");
    // Create event group
    player_event_group = xEventGroupCreate();
    panic_if(!player_event_group, "Failed to create event group.");

    // Create the can sleep timer (now 15 seconds for both idle and cart removal)
    audio_can_sleep_timer = xTimerCreate("audio_can_sleep_timer", pdMS_TO_TICKS(15000), pdFALSE, NULL, audio_can_sleep_timer_callback);
    panic_if(!audio_can_sleep_timer, "Failed to create audio can sleep timer");

    // Create buffer queues and pre-fill the empty one
    full_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));
    empty_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));
    uint8_t *buf_a_ptr = adpcm_buffer_A;
    uint8_t *buf_b_ptr = adpcm_buffer_B;
    xQueueSend(empty_buffer_queue, &buf_a_ptr, 0);
    xQueueSend(empty_buffer_queue, &buf_b_ptr, 0);

    // Create task status change queue
    cart_presence_notify_queue = xQueueCreate(2, sizeof(CartPresenceState));

    // Register a low to high interrupt on pin CART_PRESENCE
    // when cart is removed, kill the player task
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_ANYEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << CART_PRESENCE_GPIO),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE, // External pull-up
    };
    gpio_config(&io_conf);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(CART_PRESENCE_GPIO, cart_presense_gpio_isr_handler, NULL);

    // call ISR handler once to set initial state
    cart_presense_gpio_isr_handler(NULL);
    panic_if(uxQueueMessagesWaiting(cart_presence_notify_queue) == 0, "Failed to get initial cart presence state");

    // Create and start the main player control task
    xTaskCreate(audio_player_task, "AudioPlayerTask", 4096, NULL, 8, &playerTaskHandle);

    ESP_LOGI(TAG, "Audio player initialized. Track detection will occur in the player task.");
}

/**
 * @brief Sends a command to the audio player's control task.
 */
void audio_player_send_command(const AudioCommand *cmd)
{
    if (cmd == NULL)
    {
        ESP_LOGE(TAG, "Cannot send NULL command");
        return;
    }

    if (audio_command_queue == NULL)
    {
        ESP_LOGW(TAG, "Audio player not initialized, ignoring command type %d", cmd->type);
        return;
    }

    if (!g_sd_initialized)
    {
        ESP_LOGW(TAG, "Cart not inserted, ignoring command type %d", cmd->type);
        return;
    }

    BaseType_t result = xQueueSend(audio_command_queue, cmd, 0);
    if (result != pdTRUE)
    {
        ESP_LOGW(TAG, "Failed to send command %d to audio player (queue full)", cmd->type);
    }
    else
    {
        ESP_LOGI(TAG, "Command %d sent successfully", cmd->type);
    }
}

/**
 * @brief Cleans up on cart removal.
 */
static void cleanup_on_cart_removal()
{
    stop_playback();
    esp_err_t err = esp_vfs_fat_sdcard_unmount("/sdcard", g_card);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "SD card unmounted.");
        g_card = NULL; // Reset global handle
    }
    else
    {
        ESP_LOGE(TAG, "Failed to unmount SD card: %s (0x%x)", esp_err_to_name(err), err);
    }
    // Deinit SPI bus
    if (spi_bus_free(SPI2_HOST) == ESP_OK)
    {
        ESP_LOGI(TAG, "SPI bus deinitialized.");
    }
    else
    {
        ESP_LOGE(TAG, "Failed to deinitialize SPI bus.");
    }
    // Optionally unmount SD if supported, but for now just mark as uninitialized
    g_sd_initialized = false;
    g_total_tracks = 0;
    ESP_LOGI(TAG, "Cart removed, cleaned up playback state.");
}

/**
 * @brief Initializes SD card and scans tracks if cart is inserted.
 */
static void init_sd_if_needed()
{
    if (!g_sd_initialized)
    {
        if (init_sdcard() != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize SD card. Will retry on next cart insertion.");
            return;
        }
        g_sd_initialized = true;

        // Scan tracks
        int track_count = 0;
        DIR *dir = opendir("/sdcard");
        if (dir == NULL)
        {
            ESP_LOGE(TAG, "Failed to open directory /sdcard");
            return;
        }
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
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

        // Load persisted state if tracks exist
        if (g_total_tracks > 0)
        {
            load_playback_state();
            // Auto-play: start from saved track or first track
            if (g_current_track >= 0 && g_current_track < g_total_tracks)
            {
                start_playback(g_current_track);
            }
            else
            {
                start_playback(0); // Fallback to first track
            }
        }
        update_volume_shift();
    }
}
