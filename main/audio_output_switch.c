#include "audio_output_switch.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "a2dp_coprocessor_service.h"
#include "i2s_service.h"

ESP_EVENT_DEFINE_BASE(AUDIO_OUTPUT_SWITCH_EVENT);

#define AUDIO_OUTPUT_SWITCH_EVENT_TIMEOUT_MS 100

static const char *TAG = "audio_switch";

static audio_output_pcm_provider_t s_provider;
static void *s_provider_ctx;
static audio_output_target_t s_target = AUDIO_OUTPUT_TARGET_I2S;
static bool s_initialised;

static bool audio_output_switch_target_valid(audio_output_target_t target)
{
    return target == AUDIO_OUTPUT_TARGET_A2DP || target == AUDIO_OUTPUT_TARGET_I2S;
}

static int32_t audio_output_switch_dispatch_pcm(uint8_t *data, int32_t len, void *user_ctx)
{
    (void)user_ctx;

    if (!data || len <= 0)
    {
        return 0;
    }

    if (!s_provider)
    {
        memset(data, 0, (size_t)len);
        return len;
    }

    return s_provider(data, len, s_provider_ctx);
}

static esp_err_t audio_output_switch_start_i2s_dispatch(void)
{
    esp_err_t err = i2s_service_register_pcm_provider(audio_output_switch_dispatch_pcm, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    err = i2s_service_start_audio();
    if (err != ESP_OK)
    {
        (void)i2s_service_register_pcm_provider(NULL, NULL);
    }
    return err;
}

static void audio_output_switch_post_target_changed(audio_output_target_t target)
{
    (void)esp_event_post(AUDIO_OUTPUT_SWITCH_EVENT,
                         AUDIO_OUTPUT_SWITCH_EVENT_TARGET_CHANGED,
                         &target,
                         sizeof(target),
                         pdMS_TO_TICKS(AUDIO_OUTPUT_SWITCH_EVENT_TIMEOUT_MS));
}

esp_err_t audio_output_switch_init(void)
{
    if (s_initialised)
    {
        return ESP_OK;
    }

    s_initialised = true;
    return ESP_OK;
}

esp_err_t audio_output_switch_set_provider(audio_output_pcm_provider_t provider, void *user_ctx)
{
    esp_err_t err;

    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }

    s_provider = provider;
    s_provider_ctx = user_ctx;

    err = audio_output_switch_start_i2s_dispatch();
    if (err == ESP_OK && s_target == AUDIO_OUTPUT_TARGET_I2S)
    {
        (void)a2dp_coprocessor_service_shutdown();
    }
    return err;
}

esp_err_t audio_output_switch_select(audio_output_target_t target)
{
    esp_err_t err = ESP_OK;
    audio_output_target_t previous_target = s_target;

    if (!s_initialised)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (!audio_output_switch_target_valid(target))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (target == previous_target)
    {
        return target == AUDIO_OUTPUT_TARGET_A2DP ? a2dp_coprocessor_service_wake_for_request() : ESP_OK;
    }

    if (target == AUDIO_OUTPUT_TARGET_A2DP)
    {
        err = a2dp_coprocessor_service_wake_for_request();
    }
    else
    {
        (void)a2dp_coprocessor_service_suspend_audio();
        err = a2dp_coprocessor_service_shutdown();
    }
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "failed to switch audio output to %s: %s",
                 audio_output_switch_target_name(target),
                 esp_err_to_name(err));
        return err;
    }

    s_target = target;
    ESP_LOGI(TAG, "audio output switched to %s", audio_output_switch_target_name(target));
    audio_output_switch_post_target_changed(target);
    return ESP_OK;
}

audio_output_target_t audio_output_switch_get_target(void)
{
    return s_target;
}

const char *audio_output_switch_target_name(audio_output_target_t target)
{
    switch (target)
    {
    case AUDIO_OUTPUT_TARGET_A2DP:
        return "a2dp";
    case AUDIO_OUTPUT_TARGET_I2S:
        return "i2s";
    default:
        return "unknown";
    }
}