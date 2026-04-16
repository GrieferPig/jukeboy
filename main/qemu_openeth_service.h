#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t qemu_openeth_service_init(void);
    esp_err_t qemu_openeth_service_wait_for_ip(uint32_t timeout_ms);
    bool qemu_openeth_service_has_ip(void);

#ifdef __cplusplus
}
#endif