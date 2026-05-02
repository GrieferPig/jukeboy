#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    esp_err_t qemu_openeth_service_init(void);
    void qemu_openeth_service_process_once(void);
    bool qemu_openeth_service_has_ip(void);

#ifdef __cplusplus
}
#endif