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
     * bt_audio, bt_media_key, bt_bonded), and system commands
     * (reboot, meminfo, telemetry).
     */
    esp_err_t console_service_init(void);

    /**
     * Run console-owned periodic work from the application super loop.
     */
    void console_service_process_once(void);

#ifdef __cplusplus
}
#endif
