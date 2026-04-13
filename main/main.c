/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_flash_dispatcher.h"
#include "esp_gap_bt_api.h"
#include "esp_littlefs.h"
#include "esp_eth.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_pm.h"

#include "audio_output_switch.h"
#include "bluetooth_service.h"
#include "cartridge_service.h"
#include "console_service.h"
#include "ftp_service.h"
#include "i2s_service.h"
#include "player_service.h"
#include "power_mgmt_service.h"
#include "qemu_pcm_service.h"
#include "ramdisk_service.h"
#include "runtime_env.h"
#include "storage_paths.h"
#include "wifi_service.h"

static const char *TAG = "main";
static const bool QEMU_PCM_SERVICE_ENABLED = true;

void app_main(void)
{
    const esp_flash_dispatcher_config_t flash_dispatcher_cfg = ESP_FLASH_DISPATCHER_DEFAULT_CONFIG;
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

    ESP_ERROR_CHECK(power_mgmt_service_init());
    ESP_ERROR_CHECK(esp_flash_dispatcher_init(&flash_dispatcher_cfg));

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(ramdisk_service_init());
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&littlefs_cfg));

    if (running_in_qemu)
    {
        cartridge_service_config_t cart_cfg = CARTRIDGE_SERVICE_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(cartridge_service_init(&cart_cfg));

#if CONFIG_ETH_USE_OPENETH
        ESP_LOGW(TAG,
                 "skipping QEMU open_eth init; the ESP32 cache model still faults after DHCP on the emulated Ethernet path");
#endif /* CONFIG_ETH_USE_OPENETH */

        ESP_ERROR_CHECK(ftp_service_init());
        ESP_ERROR_CHECK(player_service_init());

        if (QEMU_PCM_SERVICE_ENABLED)
        {
            ESP_ERROR_CHECK(qemu_pcm_service_init());
            ESP_ERROR_CHECK(qemu_pcm_service_register_pcm_provider(player_service_qemu_pcm_provider, NULL));
            ESP_ERROR_CHECK(qemu_pcm_service_start_audio());
        }

        ESP_ERROR_CHECK(console_service_init());
        return;
    }

    cartridge_service_config_t cart_cfg = CARTRIDGE_SERVICE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(cartridge_service_init(&cart_cfg));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));

    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(uint8_t));

    ESP_ERROR_CHECK(bluetooth_service_init());
    ESP_ERROR_CHECK(wifi_service_init());
    ESP_ERROR_CHECK(i2s_service_init());
    ESP_ERROR_CHECK(audio_output_switch_init());

    ESP_ERROR_CHECK(bluetooth_service_register_48k_sbc_endpoint());

    ESP_ERROR_CHECK(audio_output_switch_set_provider(player_service_pcm_provider, NULL));
    ESP_ERROR_CHECK(audio_output_switch_select(AUDIO_OUTPUT_TARGET_I2S));

    ESP_ERROR_CHECK(ftp_service_init());
    ESP_ERROR_CHECK(player_service_init());
    ESP_ERROR_CHECK(console_service_init());
}