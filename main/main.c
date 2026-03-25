/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"

#include "audio_output_switch.h"
#include "bluetooth_service.h"
#include "cartridge_service.h"
#include "console_service.h"
#include "i2s_service.h"
#include "player_service.h"
#include "wifi_service.h"

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

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

    ESP_ERROR_CHECK(player_service_init());
    ESP_ERROR_CHECK(console_service_init());
}
