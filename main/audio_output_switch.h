#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef int32_t (*audio_output_pcm_provider_t)(uint8_t *data, int32_t len, void *user_ctx);
    typedef esp_err_t (*audio_output_register_pcm_provider_fn)(audio_output_pcm_provider_t provider, void *user_ctx);

    typedef enum
    {
        AUDIO_OUTPUT_TARGET_BLUETOOTH,
        AUDIO_OUTPUT_TARGET_I2S,
    } audio_output_target_t;

    esp_err_t audio_output_switch_set_provider(audio_output_pcm_provider_t provider, void *user_ctx);
    audio_output_register_pcm_provider_fn audio_output_switch_select(audio_output_target_t target);

#ifdef __cplusplus
}
#endif