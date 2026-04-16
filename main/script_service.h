#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define SCRIPT_SERVICE_HOST_MODULE_NAME "jukeboy"
#define SCRIPT_SERVICE_MAX_PATH_LEN 256
#define SCRIPT_SERVICE_MAX_MESSAGE_LEN 160
#define SCRIPT_SERVICE_MAX_OUTPUT_LEN 2048

    typedef enum
    {
        SCRIPT_SERVICE_RUN_MODE_LIBC_BUILTIN = 0,
    } script_service_run_mode_t;

    typedef enum
    {
        SCRIPT_SERVICE_STATUS_UNINITIALIZED = 0,
        SCRIPT_SERVICE_STATUS_READY,
        SCRIPT_SERVICE_STATUS_BUSY,
        SCRIPT_SERVICE_STATUS_ERROR,
    } script_service_status_t;

    typedef struct
    {
        char resolved_path[SCRIPT_SERVICE_MAX_PATH_LEN];
        char message[SCRIPT_SERVICE_MAX_MESSAGE_LEN];
        char output[SCRIPT_SERVICE_MAX_OUTPUT_LEN];
        script_service_run_mode_t mode;
        uint32_t script_size_bytes;
        int32_t exit_code;
    } script_service_run_result_t;

    esp_err_t script_service_init(void);
    bool script_service_is_ready(void);
    script_service_status_t script_service_get_status(void);
    const char *script_service_status_name(script_service_status_t status);
    const char *script_service_run_mode_name(script_service_run_mode_t mode);

    size_t script_service_get_root_count(void);
    const char *script_service_get_root_label(size_t index);
    const char *script_service_get_root_path(size_t index);

    esp_err_t script_service_resolve_path(const char *path, char *out_path, size_t out_path_size);
    esp_err_t script_service_run(const char *path,
                                 int argc,
                                 const char *const *argv,
                                 script_service_run_result_t *result);

#ifdef __cplusplus
}
#endif