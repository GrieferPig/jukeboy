#include "audio_output_switch.h"

#include "bluetooth_service.h"
#include "i2s_service.h"

static audio_output_pcm_provider_t s_provider;
static void *s_provider_ctx;
static audio_output_register_pcm_provider_fn s_active_register_fn;

static esp_err_t audio_output_switch_register_bluetooth(audio_output_pcm_provider_t provider, void *user_ctx)
{
    return bluetooth_service_register_pcm_provider(provider, user_ctx);
}

static esp_err_t audio_output_switch_register_i2s(audio_output_pcm_provider_t provider, void *user_ctx)
{
    return i2s_service_register_pcm_provider(provider, user_ctx);
}

static audio_output_register_pcm_provider_fn audio_output_switch_get_register_fn(audio_output_target_t target)
{
    switch (target)
    {
    case AUDIO_OUTPUT_TARGET_BLUETOOTH:
        return audio_output_switch_register_bluetooth;
    case AUDIO_OUTPUT_TARGET_I2S:
        return audio_output_switch_register_i2s;
    default:
        return NULL;
    }
}

esp_err_t audio_output_switch_set_provider(audio_output_pcm_provider_t provider, void *user_ctx)
{
    s_provider = provider;
    s_provider_ctx = user_ctx;

    if (!s_active_register_fn)
    {
        return ESP_OK;
    }

    return s_active_register_fn(s_provider, s_provider_ctx);
}

audio_output_register_pcm_provider_fn audio_output_switch_select(audio_output_target_t target)
{
    audio_output_register_pcm_provider_fn next_register_fn = audio_output_switch_get_register_fn(target);

    if (s_active_register_fn)
    {
        s_active_register_fn(NULL, NULL);
    }

    s_active_register_fn = next_register_fn;
    if (s_active_register_fn)
    {
        s_active_register_fn(s_provider, s_provider_ctx);
    }

    return s_active_register_fn;
}