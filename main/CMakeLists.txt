idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES spi_flash bt esp_psram nvs_flash esp_hw_support wifi_service console_service bluetooth_service cartridge_service esp_audio_codec esp_ringbuf freertos
                       INCLUDE_DIRS "")
