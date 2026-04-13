#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int32_t (*qemu_pcm_service_pcm_provider_t)(uint8_t *data, int32_t len, void *user_ctx);

    esp_err_t qemu_pcm_service_init(void);
    esp_err_t qemu_pcm_service_start_audio(void);
    esp_err_t qemu_pcm_service_suspend_audio(void);
    esp_err_t qemu_pcm_service_register_pcm_provider(qemu_pcm_service_pcm_provider_t provider, void *user_ctx);

#ifdef __cplusplus
}
#endif
