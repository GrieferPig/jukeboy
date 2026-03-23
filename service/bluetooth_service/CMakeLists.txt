idf_component_register(SRCS "bluetooth_service.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES bt esp_event freertos log)
