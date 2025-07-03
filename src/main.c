#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "esp_system.h"
#include "driver/gpio.h"

// --- I2S Configuration ---
#define I2S_BCK_PIN GPIO_NUM_26
#define I2S_WS_PIN GPIO_NUM_25
#define I2S_DOUT_PIN GPIO_NUM_22
#define I2S_PORT I2S_NUM_0

// --- Sine Wave Configuration ---
#define SAMPLE_RATE (44100)
#define BITS_PER_SAMPLE (I2S_DATA_BIT_WIDTH_16BIT)
#define SINE_WAVE_FREQ_HZ (440.0) // A4 note
#define AMPLITUDE (16383 / 4)
#define BUFFER_SIZE_BYTES (1024)

// I2S channel handle
static i2s_chan_handle_t tx_handle;

/**
 * @brief Task to generate and write sine wave data to I2S
 *
 * This task generates sine wave data and writes it to the I2S TX channel
 * in stereo format (left and right channels). It uses a phase accumulator
 * to prevent floating point precision issues over time.
 */
void sine_wave_task(void *pvParameters)
{
    int16_t *sine_buffer = (int16_t *)malloc(BUFFER_SIZE_BYTES);
    if (!sine_buffer)
    {
        printf("Failed to allocate memory for sine buffer\n");
        vTaskDelete(NULL);
        return;
    }

    size_t bytes_written = 0;
    // Phase accumulator and step for continuous sine wave generation
    double phase = 0.0;
    double phase_step = 2.0 * M_PI * SINE_WAVE_FREQ_HZ / SAMPLE_RATE;

    printf("Sine wave task started. Generating stereo audio...\n");

    while (1)
    {
        // Generate a chunk of the sine wave for stereo output
        // The loop iterates over stereo frames (L/R pairs)
        for (int i = 0; i < (BUFFER_SIZE_BYTES / sizeof(int16_t) / 2); i++)
        {
            // Calculate the sample value from the current phase
            int16_t sample_value = (int16_t)(AMPLITUDE * sin(phase));

            // Write the same sample to both left and right channels for a dual-mono output
            sine_buffer[i * 2] = sample_value;     // Left channel
            sine_buffer[i * 2 + 1] = sample_value; // Right channel

            // Increment phase and wrap around 2*PI to avoid large numbers
            phase += phase_step;
            if (phase >= 2.0 * M_PI)
            {
                phase -= 2.0 * M_PI;
            }
        }

        // Write the generated stereo data to the I2S channel
        esp_err_t err = i2s_channel_write(tx_handle, sine_buffer, BUFFER_SIZE_BYTES, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK)
        {
            printf("I2S write failed: %s\n", esp_err_to_name(err));
        }
        if (bytes_written != BUFFER_SIZE_BYTES)
        {
            printf("I2S write timeout. Wrote %d bytes.\n", bytes_written);
        }
    }

    // This part is unreachable but good practice
    free(sine_buffer);
    vTaskDelete(NULL);
}

/**
 * @brief Main application entry point
 */
void app_main(void)
{
    // Set LDO EN pin to high
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << GPIO_NUM_12),
        .pull_down_en = 0,
        .pull_up_en = 0,
    };
    ESP_ERROR_CHECK(gpio_config(&io_conf));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_12, 1));
    printf("LDO EN pin set to high\n");

    printf("Starting I2S Stereo Sine Wave Example\n");

    // 1. Create a new I2S channel
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, NULL));

    // 2. Configure the I2S channel for standard mode with stereo output
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(BITS_PER_SAMPLE, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // No MCLK pin
            .bclk = I2S_BCK_PIN,
            .ws = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED, // Not used
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));

    // 3. Enable the I2S channel before writing data
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    // 4. Create the task that generates and writes the sine wave
    xTaskCreate(sine_wave_task, "sine_wave_task", 4096, NULL, 5, NULL);
}
