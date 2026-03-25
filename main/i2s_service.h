#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int32_t (*i2s_service_pcm_provider_t)(uint8_t *data, int32_t len, void *user_ctx);

    esp_err_t i2s_service_init(void);
    esp_err_t i2s_service_start_audio(void);
    esp_err_t i2s_service_suspend_audio(void);
    esp_err_t i2s_service_register_pcm_provider(i2s_service_pcm_provider_t provider, void *user_ctx);

#ifdef __cplusplus
}
#endif