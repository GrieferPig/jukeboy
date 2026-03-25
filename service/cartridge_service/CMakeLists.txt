idf_component_register(SRCS "cartridge_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES sdmmc fatfs log esp_psram freertos esp_event)
