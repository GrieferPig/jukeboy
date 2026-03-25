idf_component_register(SRCS "player_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES cartridge_service esp_audio_codec esp_event esp_psram freertos log)