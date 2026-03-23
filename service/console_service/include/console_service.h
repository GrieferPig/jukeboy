#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Initialise the console service: starts a UART REPL, registers
     * WiFi commands (wifi_connect, wifi_disconnect, wifi_scan, wifi_status,
     * wifi_autoreconnect), Bluetooth commands (bt_pair, bt_disconnect,
     * bt_audio, bt_media_key, bt_bonded), system commands (reboot, meminfo, telemetry),
     * and starts the periodic telemetry worker owned by the console service.
     */
    esp_err_t console_service_init(void);

#ifdef __cplusplus
}
#endif
