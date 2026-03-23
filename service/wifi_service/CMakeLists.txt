idf_component_register(SRCS "wifi_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip log esp_timer freertos)
