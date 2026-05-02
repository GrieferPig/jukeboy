#pragma once

#include <stdbool.h>

#include "driver/i2s_common.h"
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
        POWER_MGMT_SERVICE_EVENT_RAIL_ON,
        POWER_MGMT_SERVICE_EVENT_RAIL_OFF,
    } power_mgmt_service_event_id_t;

    typedef enum
    {
        POWER_MGMT_RAIL_DAC = 0,
        POWER_MGMT_RAIL_LED,
        POWER_MGMT_RAIL_COUNT,
    } power_mgmt_rail_t;

    typedef enum
    {
        POWER_MGMT_OVERRIDE_AUTO = 0,
        POWER_MGMT_OVERRIDE_FORCE_ON,
        POWER_MGMT_OVERRIDE_FORCE_OFF,
    } power_mgmt_rail_override_t;

    typedef esp_err_t (*power_mgmt_service_shutdown_callback_t)(void *user_ctx);
    typedef esp_err_t (*power_mgmt_service_dac_prepare_off_callback_t)(void *user_ctx);

#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_PLAYER 100
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_BLUETOOTH 200
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_WIFI 300
#define POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_FLASH 1000

    esp_err_t power_mgmt_service_init(void);
    void power_mgmt_service_process_once(void);
    esp_err_t power_mgmt_service_bind_dac_i2s_channel(i2s_chan_handle_t tx_channel,
                                                      power_mgmt_service_dac_prepare_off_callback_t prepare_off_callback,
                                                      void *prepare_off_user_ctx);
    esp_err_t power_mgmt_service_register_shutdown_callback(power_mgmt_service_shutdown_callback_t callback,
                                                            void *user_ctx,
                                                            int priority);
    esp_err_t power_mgmt_service_rail_request(power_mgmt_rail_t rail);
    esp_err_t power_mgmt_service_rail_release(power_mgmt_rail_t rail);
    esp_err_t power_mgmt_service_set_dac_muted(bool muted);
    esp_err_t power_mgmt_service_rail_is_enabled(power_mgmt_rail_t rail, bool *enabled_out);
    esp_err_t power_mgmt_service_rail_get_refcount(power_mgmt_rail_t rail, size_t *refcount_out);
    esp_err_t power_mgmt_service_rail_get_override(power_mgmt_rail_t rail,
                                                   power_mgmt_rail_override_t *override_out);
    esp_err_t power_mgmt_service_rail_set_override(power_mgmt_rail_t rail,
                                                   power_mgmt_rail_override_t override_mode);
    esp_err_t power_mgmt_service_reboot(void);
    esp_err_t power_mgmt_service_reboot_to_download(void);

#ifdef __cplusplus
}
#endif