idf_component_register(SRCS "console_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES console wifi_service bluetooth_service cartridge_service bt esp_system esp_wifi heap log freertos esp_netif)
