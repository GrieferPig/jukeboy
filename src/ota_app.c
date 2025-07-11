/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"
#include "esp_log.h"
#include "esp_adc/adc_continuous.h"

// Define the GPIO pin for ADC reading. GPIO36 is ADC1_CHANNEL_0.
#define ADC_INPUT_PIN ADC_CHANNEL_0

// Define ADC configuration parameters
#define READ_LEN 256
#define ADC_UNIT ADC_UNIT_1
#define ADC_CONV_MODE ADC_CONV_MODE_CONTINUOUS
#define ADC_ATTEN ADC_ATTEN_DB_11

static const char *TAG = "ADC_CONTINUOUS_EXAMPLE";

// ADC channel configuration
static adc_channel_t channel[1] = {ADC_INPUT_PIN};

// Callback function for when a conversion frame is done
// This is optional but useful for debugging or complex applications.
static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    // In this example, we don't do anything in the callback.
    // In a real application, you could use this to signal a task that data is ready.
    return false;
}

void ota_app_main(void)
{
    // Variable to hold the ADC continuous mode handle
    adc_continuous_handle_t handle = NULL;

    // --- 1. ADC Continuous Mode Handle Initialization ---
    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));
    ESP_LOGI(TAG, "ADC handle created.");

    // --- 2. ADC Continuous Mode Configuration ---
    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 20 * 1000, // 20 kHz
        .conv_mode = ADC_CONV_SINGLE_UNIT_1,
        .format = ADC_DIGI_OUTPUT_FORMAT_TYPE1,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = 1;
    for (int i = 0; i < 1; i++)
    {
        adc_pattern[i].atten = ADC_ATTEN;
        adc_pattern[i].channel = channel[i] & 0x7;
        adc_pattern[i].unit = ADC_UNIT;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%x", i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%x", i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%x", i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));
    ESP_LOGI(TAG, "ADC configuration set.");

    // --- 3. Register Callbacks (Optional) ---
    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = s_conv_done_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    ESP_LOGI(TAG, "Callbacks registered.");

    // --- 4. Start ADC Continuous Conversion ---
    ESP_ERROR_CHECK(adc_continuous_start(handle));
    ESP_LOGI(TAG, "ADC continuous mode started.");

    // --- 5. Main Loop to Read Data ---
    // Buffer to store the results
    uint8_t result[READ_LEN] = {0};
    // Variable to store the number of bytes read
    uint32_t ret_num = 0;

    while (1)
    {
        // Read the conversion results
        esp_err_t ret = adc_continuous_read(handle, result, READ_LEN, &ret_num, 0);

        if (ret == ESP_OK)
        {
            // If data is successfully read, process it
            for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES)
            {
                adc_digi_output_data_t *p = (adc_digi_output_data_t *)&result[i];
                uint32_t chan_num = p->type1.channel;
                uint32_t data = p->type1.data;

                // Check if the data is from the channel we are interested in
                if (chan_num == ADC_INPUT_PIN)
                {
                    ESP_LOGI(TAG, "ADC Channel[%d] Raw Data: %d", chan_num, data);
                }
            }
        }
        else if (ret == ESP_ERR_TIMEOUT)
        {
            // This can happen if the read timeout is set to 0 and no data is available.
            // It's not necessarily an error.
        }

        // Add a small delay to prevent the task from hogging the CPU
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    // --- 6. Stop and Deinitialize ADC (This part is not reached in this example) ---
    ESP_ERROR_CHECK(adc_continuous_stop(handle));
    ESP_LOGI(TAG, "ADC continuous mode stopped.");
    ESP_ERROR_CHECK(adc_continuous_deinit(handle));
    ESP_LOGI(TAG, "ADC handle de-initialized.");
}
