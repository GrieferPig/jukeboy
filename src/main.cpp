#include <Arduino.h>
#include <SD.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h" // The native ESP-IDF I2S standard mode driver
#include "esp_log.h"
#include "adpcm_decoder.h"  // Your custom decoder library
#include "storage_struct.h" // Include your storage structure definitions

volatile float g_current_gain = 0.2f; // Start at 100% volume

// --- Hardware Pins ---
// I2S Pins
#define I2S_BCLK_PIN 7
#define I2S_LRCLK_PIN 9
#define I2S_DIN_PIN -1 // Not used for playback
#define I2S_DOUT_PIN 10

// SD Card SPI Pins
#define SD_CS_PIN 3
#define SD_SCK_PIN 1
#define SD_MOSI_PIN 2
#define SD_MISO_PIN 0

// --- TJA Format and Audio Constants ---
static const char *TAG = "BarebonePlayer";
static const uint32_t TJA_HEADER_SIZE = 512;
static const uint32_t ADPCM_BLOCK_SIZE = 44032;
static const uint32_t PCM_BUFFER_SIZE = (ADPCM_BLOCK_SIZE - 6) * 4; // Size for one full decoded block

// --- Buffers ---
// This buffer holds one block of data read from the SD card
static uint8_t adpcm_block_buffer[ADPCM_BLOCK_SIZE];
// This buffer holds the raw PCM data after decoding
static int16_t pcm_buffer[PCM_BUFFER_SIZE / sizeof(int16_t)];

// --- FreeRTOS Handles ---
TaskHandle_t playerTaskHandle = NULL;

// Forward declaration of our player task
void player_task(void *pvParameters);

// --- NEW: Function to apply gain to a PCM buffer ---
static inline void apply_gain(int16_t *pcm_buffer, size_t sample_count, float gain)
{
    for (size_t i = 0; i < sample_count; ++i)
    {
        // Apply gain as floating point multiplication
        int32_t sample = (int32_t)(pcm_buffer[i] * gain);
        // Clamp the result to the valid 16-bit range
        if (sample > 32767)
        {
            sample = 32767;
        }
        else if (sample < -32768)
        {
            sample = -32768;
        }
        pcm_buffer[i] = (int16_t)sample;
    }
}

// --- Arduino Setup Function ---
void setup()
{
    Serial.begin(115200);
    ESP_LOGI(TAG, "Starting up...");

    // Create the FreeRTOS task for the player
    xTaskCreate(
        player_task,      // Function that implements the task.
        "PlayerTask",     // Text name for the task.
        8192,             // Stack size in words (bytes for ESP-IDF). 8KB is safe.
        NULL,             // Parameter passed into the task.
        5,                // Priority of the task.
        &playerTaskHandle // Task handle to keep track of created task.
    );
}

void loop()
{
    // The main loop is empty. All logic is in the FreeRTOS task.
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// --- Player FreeRTOS Task Implementation ---
void player_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Player task started.");

    // --- Part A: Initialize I2S using ESP-IDF native driver ---
    i2s_chan_handle_t tx_handle; // Handle for the I2S TX channel

    // Configuration for the I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    // Configuration for standard I2S protocol
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK_PIN,
            .ws = (gpio_num_t)I2S_LRCLK_PIN,
            .dout = (gpio_num_t)I2S_DOUT_PIN,
            .din = (gpio_num_t)I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
    ESP_LOGI(TAG, "I2S driver installed and enabled.");

    // --- Part B: Initialize SD Card using Arduino library ---
    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN))
    {
        ESP_LOGE(TAG, "SD Card mount failed. Halting task.");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before restart
        ESP.restart();                   // Restart ESP if SD card fails to initialize
        return;
    }
    ESP_LOGI(TAG, "SD Card initialized.");

    // --- Part C: Open Audio File ---
    File audioFile = SD.open("/00.tja", FILE_READ);
    if (!audioFile)
    {
        ESP_LOGE(TAG, "Failed to open /00.tja. Halting task.");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before restart
        ESP.restart();                   // Restart ESP if SD card fails to initialize
        return;
    }

    // Skip the 512-byte header to get to the first data block
    if (!audioFile.seek(TJA_HEADER_SIZE))
    {
        ESP_LOGE(TAG, "Failed to seek past header. Halting task.");
        audioFile.close();
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before restart
        ESP.restart();                   // Restart ESP if SD card fails to initialize
        return;
    }
    ESP_LOGI(TAG, "Opened and seeked past header in 00.tja");

    // --- Part D: The Main Playback Loop ---
    while (true)
    {
        // 1. Read one full ADPCM block from the SD card
        size_t bytesRead = audioFile.read(adpcm_block_buffer, ADPCM_BLOCK_SIZE);
        if (bytesRead < ADPCM_BLOCK_SIZE)
        {
            ESP_LOGI(TAG, "End of file reached or read error. Restarting file.");
            audioFile.seek(TJA_HEADER_SIZE); // Loop the file
            continue;
        }

        // 2. Decode the ADPCM block into the PCM buffer
        size_t pcm_bytes_decoded = decode_adpcm_block(
            adpcm_block_buffer,
            pcm_buffer,
            PCM_BUFFER_SIZE);

        // 3. *** NEW: Apply the current gain to the decoded PCM data ***
        if (pcm_bytes_decoded > 0)
        {
            // The number of samples is bytes / 2
            apply_gain(pcm_buffer, pcm_bytes_decoded / 2, g_current_gain);
        }

        // 3. Write the decoded PCM data to the I2S peripheral
        if (pcm_bytes_decoded > 0)
        {
            size_t bytes_written = 0;
            esp_err_t result = i2s_channel_write(
                tx_handle,
                pcm_buffer,
                pcm_bytes_decoded,
                &bytes_written,
                portMAX_DELAY // Block forever until all data is written
            );

            if (result != ESP_OK)
            {
                ESP_LOGE(TAG, "I2S write failed! Error: %s", esp_err_to_name(result));
            }
            if (bytes_written < pcm_bytes_decoded)
            {
                ESP_LOGW(TAG, "I2S write underrun. Wrote %d/%d bytes.", bytes_written, pcm_bytes_decoded);
            }
        }
    }

    // --- Cleanup (code will not reach here in this simple loop) ---
    ESP_LOGI(TAG, "Playback finished. Disabling I2S.");
    audioFile.close();
    i2s_channel_disable(tx_handle);
    i2s_del_channel(tx_handle);
    vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second before restart
    ESP.restart();                   // Restart ESP if SD card fails to initialize
}