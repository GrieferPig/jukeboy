idf_component_register(SRCS "main.c"
                       "audio_output_switch.c"
                       "bluetooth_service.c"
                       "cartridge_service.c"
                       "console_service.c"
                       "esp_flash_dispatcher.c"
                       "i2s_service.c"
                       "player_service.c"
                       "power_mgmt_service.c"
                       "wifi_service.c"
                   PRIV_REQUIRES spi_flash bt esp_psram nvs_flash esp_hw_support esp_audio_codec esp_ringbuf freertos esp_event esp_wifi esp_netif lwip log console fatfs sdmmc heap esp_system esp_timer esp_vfs_console esp_driver_i2s esp_driver_gpio
                   INCLUDE_DIRS ".")

target_link_libraries(${COMPONENT_LIB} INTERFACE
                      "-Wl,--wrap=esp_flash_read"
                      "-Wl,--wrap=esp_flash_write"
                      "-Wl,--wrap=esp_flash_write_encrypted"
                      "-Wl,--wrap=esp_flash_erase_region"
                      "-Wl,--wrap=esp_flash_erase_chip")
