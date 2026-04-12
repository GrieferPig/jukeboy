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
#include "ramdisk_service.h"
#include "runtime_env.h"
#include "storage_paths.h"
#include "wifi_service.h"

static const char *TAG = "main";

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
                 "detected QEMU runtime from blank factory eFuse MAC; skipping Wi-Fi, Bluetooth, and I2S service init");
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

    cartridge_service_config_t cart_cfg = CARTRIDGE_SERVICE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(cartridge_service_init(&cart_cfg));

    if (running_in_qemu)
    {
#if CONFIG_ETH_USE_OPENETH
        /* Initialise TCP/IP stack and wired Ethernet via QEMU open_eth model.
         * -nic user,model=open_eth in the QEMU command provides the virtual NIC. */
        ESP_ERROR_CHECK(esp_netif_init());

        esp_netif_config_t eth_netif_cfg = ESP_NETIF_DEFAULT_ETH();
        esp_netif_t *eth_netif = esp_netif_new(&eth_netif_cfg);

        eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
        esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);

        eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
        phy_config.phy_addr = 1;
        phy_config.reset_gpio_num = -1;
        esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);

        esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
        esp_eth_handle_t eth_handle = NULL;
        ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &eth_handle));
        ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));
        ESP_ERROR_CHECK(esp_eth_start(eth_handle));
#endif /* CONFIG_ETH_USE_OPENETH */
    }

    if (!running_in_qemu)
    {
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
    }

    ESP_ERROR_CHECK(ftp_service_init());

    ESP_ERROR_CHECK(player_service_init());
    ESP_ERROR_CHECK(console_service_init());
}