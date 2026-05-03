/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "nvs_flash.h"
#include "esp_partition.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
// #include "esp_flash_dispatcher.h"
#include "esp_gap_bt_api.h"
#include "esp_littlefs.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_pm.h"
#include "esp_task_wdt.h"

#include "audio_output_switch.h"
#include "bluetooth_service.h"
#include "cartridge_service.h"
#include "console_service.h"
#include "hid_service.h"
#include "i2s_service.h"
#include "lastfm_service.h"
#include "play_history_service.h"
#include "player_service.h"
#include "power_mgmt_service.h"
#include "qemu_openeth_service.h"
#include "qemu_pcm_service.h"
#include "ramdisk_service.h"
#include "runtime_env.h"
#include "script_service.h"
#include "storage_paths.h"
#include "wifi_service.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <soc/soc.h>

static const char *TAG = "main";
static const bool QEMU_PCM_SERVICE_ENABLED = true;
static const uint32_t APP_SUPER_LOOP_IDLE_MS = 1;

typedef void (*app_super_loop_service_fn_t)(void);

static TickType_t app_super_loop_idle_ticks(void)
{
    TickType_t idle_ticks = pdMS_TO_TICKS(APP_SUPER_LOOP_IDLE_MS);
    return idle_ticks == 0 ? 1 : idle_ticks;
}

static void app_run_super_loop(bool running_in_qemu)
{
    app_super_loop_service_fn_t services[5] = {
        power_mgmt_service_process_once,
        console_service_process_once,
        play_history_service_process_once,
    };
    size_t service_count = 3;

    if (running_in_qemu)
    {
        services[service_count++] = qemu_openeth_service_process_once;
    }
    else
    {
        services[service_count++] = wifi_service_process_once;
        services[service_count++] = bluetooth_service_process_once;
    }

    TickType_t idle_ticks = app_super_loop_idle_ticks();

    for (;;)
    {
        for (size_t index = 0; index < service_count; index++)
        {
            services[index]();
        }

        vTaskDelay(idle_ticks);
    }
}

static esp_err_t app_init_secure_nvs(void)
{
    const esp_partition_t *keys_partition;
    nvs_sec_cfg_t security_cfg = {0};
    esp_err_t err;

    keys_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS,
                                              "nvs_keys");
    if (!keys_partition)
    {
        ESP_LOGE(TAG, "missing nvs_keys partition for secure NVS init");
        return ESP_ERR_NOT_FOUND;
    }

    err = nvs_flash_read_security_cfg(keys_partition, &security_cfg);
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED)
    {
        ESP_LOGW(TAG, "NVS keys are not initialized; generating fresh keys");
        err = nvs_flash_generate_keys(keys_partition, &security_cfg);
    }
    else if (err == ESP_ERR_NVS_CORRUPT_KEY_PART)
    {
        ESP_LOGW(TAG, "NVS key partition is corrupt; erasing and regenerating keys");
        err = esp_partition_erase_range(keys_partition, 0, keys_partition->size);
        if (err == ESP_OK)
        {
            err = nvs_flash_generate_keys(keys_partition, &security_cfg);
        }
    }

    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_flash_secure_init(&security_cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "erasing default NVS partition before secure re-init: %s", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_secure_init(&security_cfg);
    }

    return err;
}

#include "driver/gpio.h"

void app_main(void)
{
    const esp_vfs_littlefs_conf_t littlefs_cfg = {
        .base_path = APP_LITTLEFS_MOUNT_PATH,
        .partition_label = APP_LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    const bool running_in_qemu = app_is_running_in_qemu();

    if (running_in_qemu)
    {
        ESP_LOGW(TAG,
                 "detected QEMU runtime from blank factory eFuse MAC; skipping Wi-Fi, Bluetooth, and hardware I2S init");
        /* Lock CPU at fixed frequency: QEMU does not support dynamic frequency
         * switching and asserts in pm_impl.c:on_freq_update if it is attempted. */
        esp_pm_config_t pm_config = {
            .max_freq_mhz = 240,
            .min_freq_mhz = 240,
            .light_sleep_enable = false,
        };
        ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    }
    else
    {
        esp_pm_config_t pm_config = {
            .max_freq_mhz = 240,
            .min_freq_mhz = 80,
#if CONFIG_FREERTOS_USE_TICKLESS_IDLE
            .light_sleep_enable = true,
#else
            .light_sleep_enable = false,
#endif
        };
#if !CONFIG_FREERTOS_USE_TICKLESS_IDLE
        ESP_LOGW(TAG, "tickless idle is disabled; leaving light sleep off in power management config");
#endif
        ESP_ERROR_CHECK(esp_pm_configure(&pm_config));
    }

    ESP_ERROR_CHECK(power_mgmt_service_init());

    ESP_ERROR_CHECK(app_init_secure_nvs());
    ESP_ERROR_CHECK(ramdisk_service_init());
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&littlefs_cfg));

    cartridge_service_config_t cart_cfg = CARTRIDGE_SERVICE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(cartridge_service_init(&cart_cfg));

    if (running_in_qemu)
    {
        esp_err_t eth_err = qemu_openeth_service_init();
        if (eth_err != ESP_OK)
        {
            ESP_LOGW(TAG, "QEMU OpenETH init failed: %s", esp_err_to_name(eth_err));
        }
    }
    else
    {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));

        ESP_ERROR_CHECK(esp_bluedroid_init());
        ESP_ERROR_CHECK(esp_bluedroid_enable());

        esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
        esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
        esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

        ESP_ERROR_CHECK(bluetooth_service_init());
        ESP_ERROR_CHECK(wifi_service_init());
        ESP_ERROR_CHECK(i2s_service_init());
        ESP_ERROR_CHECK(hid_service_init());
        ESP_ERROR_CHECK(audio_output_switch_init());

        ESP_ERROR_CHECK(bluetooth_service_register_48k_sbc_endpoint());

        ESP_ERROR_CHECK(audio_output_switch_set_provider(player_service_pcm_provider, NULL));
        ESP_ERROR_CHECK(audio_output_switch_select(AUDIO_OUTPUT_TARGET_I2S));
    }

    ESP_ERROR_CHECK(player_service_init());
    ESP_ERROR_CHECK(play_history_service_init());
    ESP_ERROR_CHECK(lastfm_service_init());

    if (running_in_qemu && QEMU_PCM_SERVICE_ENABLED)
    {
        esp_err_t pcm_err = qemu_pcm_service_init();
        if (pcm_err != ESP_OK)
        {
            ESP_LOGW(TAG, "QEMU PCM unavailable: %s", esp_err_to_name(pcm_err));
        }
        else
        {
            pcm_err = qemu_pcm_service_register_pcm_provider(player_service_qemu_pcm_provider, NULL);
            if (pcm_err != ESP_OK)
            {
                ESP_LOGW(TAG, "failed to register QEMU PCM provider: %s", esp_err_to_name(pcm_err));
            }
            else
            {
                pcm_err = qemu_pcm_service_start_audio();
                if (pcm_err != ESP_OK)
                {
                    ESP_LOGW(TAG, "failed to start QEMU PCM audio: %s", esp_err_to_name(pcm_err));
                }
            }
        }
    }

    if (script_service_init() != ESP_OK)
    {
        ESP_LOGW(TAG, "script service init failed; continuing without WASM console support");
    }

    ESP_ERROR_CHECK(console_service_init());
    app_run_super_loop(running_in_qemu);
}