#pragma once

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(POWER_MGMT_SERVICE_EVENT);

    typedef enum
    {
        POWER_MGMT_SERVICE_EVENT_SHUTDOWN,
    } power_mgmt_service_event_id_t;

    typedef esp_err_t (*power_mgmt_service_shutdown_callback_t)(void *user_ctx);

#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_PLAYER 100
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_BLUETOOTH 200
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_FTP 250
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_WIFI 300
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_FLASH 1000

    esp_err_t power_mgmt_service_init(void);
    esp_err_t power_mgmt_service_register_shutdown_callback(power_mgmt_service_shutdown_callback_t callback,
                                                            void *user_ctx,
                                                            int priority);
    esp_err_t power_mgmt_service_reboot(void);

#ifdef __cplusplus
}
#endif