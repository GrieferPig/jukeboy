#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

    ESP_EVENT_DECLARE_BASE(AUDIO_OUTPUT_SWITCH_EVENT);

    typedef int32_t (*audio_output_pcm_provider_t)(uint8_t *data, int32_t len, void *user_ctx);

    typedef enum
    {
        AUDIO_OUTPUT_TARGET_A2DP = 0,
        AUDIO_OUTPUT_TARGET_I2S = 1,
    } audio_output_target_t;

    typedef enum
    {
        AUDIO_OUTPUT_SWITCH_EVENT_TARGET_CHANGED,
    } audio_output_switch_event_id_t;

    esp_err_t audio_output_switch_init(void);
    esp_err_t audio_output_switch_set_provider(audio_output_pcm_provider_t provider, void *user_ctx);
    esp_err_t audio_output_switch_select(audio_output_target_t target);
    audio_output_target_t audio_output_switch_get_target(void);
    const char *audio_output_switch_target_name(audio_output_target_t target);

#ifdef __cplusplus
}
#endif