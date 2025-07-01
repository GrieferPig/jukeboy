#include <Arduino.h>
#include <SD.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h" // Include for FreeRTOS queues
#include "driver/i2s_std.h" // The native ESP-IDF I2S standard mode driver
#include "esp_log.h"
#include "adpcm_decoder.h"  // Your custom decoder library
#include "storage_struct.h" // Include your storage structure definitions

volatile uint8_t g_volume_shift = 2; // Start at 25% volume (shift right by 2)

// --- Hardware Pins ---
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

// --- Buffers & RTOS Handles ---
static uint8_t adpcm_buffer_A[ADPCM_BLOCK_SIZE];
static uint8_t adpcm_buffer_B[ADPCM_BLOCK_SIZE];
static const size_t I2S_WRITE_CHUNK_SIZE = 2048;
static int16_t i2s_write_chunk_buffer[I2S_WRITE_CHUNK_SIZE / sizeof(int16_t)];

// NEW: Two queues for robust producer/consumer pipeline
QueueHandle_t full_buffer_queue;
QueueHandle_t empty_buffer_queue;

TaskHandle_t sdReaderTaskHandle = NULL;
TaskHandle_t decoderTaskHandle = NULL;

// Forward declarations for our tasks
void sd_reader_task(void *pvParameters);
void decoder_task(void *pvParameters);
void profiler_task(void *pvParameters);

// --- Gain Application Function (Integer-based) ---
static inline void apply_gain(int16_t *pcm_buffer, size_t sample_count, uint8_t volume_shift)
{
    if (volume_shift == 0)
    {
        return;
    }
    if (volume_shift > 15)
    {
        volume_shift = 15;
    }
    for (size_t i = 0; i < sample_count; ++i)
    {
        pcm_buffer[i] = pcm_buffer[i] >> volume_shift;
    }
}

// --- Arduino Setup Function ---
void setup()
{
    Serial.begin(115200);
    ESP_LOGI(TAG, "Starting up...");

    // Create two queues, each capable of holding pointers to our two buffers.
    full_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));
    empty_buffer_queue = xQueueCreate(2, sizeof(uint8_t *));

    if (full_buffer_queue == NULL || empty_buffer_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create queues. Restarting.");
        ESP.restart();
    }

    // Pre-fill the empty queue with our available buffers.
    uint8_t *buf_a_ptr = adpcm_buffer_A;
    uint8_t *buf_b_ptr = adpcm_buffer_B;
    xQueueSend(empty_buffer_queue, &buf_a_ptr, 0);
    xQueueSend(empty_buffer_queue, &buf_b_ptr, 0);

    xTaskCreate(sd_reader_task, "SDReaderTask", 4096, NULL, 5, &sdReaderTaskHandle);
    xTaskCreate(decoder_task, "DecoderTask", 4096, NULL, 10, &decoderTaskHandle);
    xTaskCreate(profiler_task, "ProfilerTask", 4096, NULL, 1, NULL);
}

void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}

// --- SD Reader Task (Producer) ---
void sd_reader_task(void *pvParameters)
{
    ESP_LOGI(TAG, "SD Reader Task started.");

    SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
    if (!SD.begin(SD_CS_PIN))
    {
        ESP_LOGE(TAG, "SD Card mount failed. Restarting.");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }
    ESP_LOGI(TAG, "SD Card initialized.");

    File audioFile = SD.open("/00.tja", FILE_READ);
    if (!audioFile || !audioFile.seek(TJA_HEADER_SIZE))
    {
        ESP_LOGE(TAG, "Failed to open or seek /00.tja. Restarting.");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }

    while (true)
    {
        // 1. Wait for an empty buffer to become available.
        uint8_t *buffer_to_fill = NULL;
        if (xQueueReceive(empty_buffer_queue, &buffer_to_fill, portMAX_DELAY) != pdPASS)
        {
            continue; // Wait forever for a free buffer
        }

        // 2. Read one full ADPCM block into the provided empty buffer.
        size_t bytesRead = audioFile.read(buffer_to_fill, ADPCM_BLOCK_SIZE);
        if (bytesRead < ADPCM_BLOCK_SIZE)
        {
            ESP_LOGI(TAG, "End of file. Looping.");
            audioFile.seek(TJA_HEADER_SIZE);
            // Re-read into the same buffer to ensure it's full
            audioFile.read(buffer_to_fill, ADPCM_BLOCK_SIZE);
        }

        // 3. Send the pointer to the now-full buffer to the decoder.
        xQueueSend(full_buffer_queue, &buffer_to_fill, portMAX_DELAY);
    }
}

// --- Decoder Task (Consumer) ---
void decoder_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Decoder Task started.");

    i2s_chan_handle_t tx_handle;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));
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

    DecoderContext decoder_ctx;

    while (true)
    {
        uint8_t *adpcm_block_to_decode = NULL;

        // 1. Wait for a pointer to a full ADPCM block from the reader task.
        if (xQueueReceive(full_buffer_queue, &adpcm_block_to_decode, portMAX_DELAY) != pdPASS)
        {
            continue;
        }

        // 2. Initialize the decoder context with the new block's data and header.
        adpcm_decoder_init(&decoder_ctx, adpcm_block_to_decode, ADPCM_BLOCK_SIZE);

        // 3. Decode the block in small chunks until it's finished.
        while (true)
        {
            size_t pcm_bytes_decoded = adpcm_decode_chunk(
                &decoder_ctx,
                i2s_write_chunk_buffer,
                I2S_WRITE_CHUNK_SIZE);

            if (pcm_bytes_decoded == 0)
            {
                break; // Exit inner loop, this block is done.
            }

            apply_gain(i2s_write_chunk_buffer, pcm_bytes_decoded / 2, g_volume_shift);

            size_t bytes_written = 0;
            i2s_channel_write(tx_handle, i2s_write_chunk_buffer, pcm_bytes_decoded, &bytes_written, portMAX_DELAY);
        }

        // 4. Return the now-processed buffer to the empty queue so the reader can use it.
        xQueueSend(empty_buffer_queue, &adpcm_block_to_decode, portMAX_DELAY);
    }
}

void profiler_task(void *pvParameters)
{
    char stats_buffer[2048]; // Buffer to hold the formatted stats string

    while (true)
    {
        // Wait for 5 seconds before printing stats again
        vTaskDelay(pdMS_TO_TICKS(1000));

        ESP_LOGI("Profiler", "--- TASK RUN-TIME STATS ---");
        vTaskGetRunTimeStats(stats_buffer);
        printf("%s\n", stats_buffer);
        ESP_LOGI("Profiler", "---------------------------\n");
    }
}