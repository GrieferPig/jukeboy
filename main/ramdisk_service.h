#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define RAMDISK_SERVICE_MOUNT_PATH "/tmp"
#define RAMDISK_SERVICE_SIZE_BYTES (1024 * 1024)

    esp_err_t ramdisk_service_init(void);

#ifdef __cplusplus
}
#endif