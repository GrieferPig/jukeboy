#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define FTP_SERVICE_ROOT_PATH "/"
#define FTP_SERVICE_COMMAND_PORT 21
#define FTP_SERVICE_PASSIVE_PORT 2024

    esp_err_t ftp_service_init(void);
    esp_err_t ftp_service_enable(void);
    esp_err_t ftp_service_disable(void);
    bool ftp_service_is_enabled(void);
    int ftp_service_get_state(void);
    const char *ftp_service_state_name(int state);
    size_t ftp_service_get_root_entry_count(void);
    const char *ftp_service_get_root_entry(size_t index);

#ifdef __cplusplus
}
#endif