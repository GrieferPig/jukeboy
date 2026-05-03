#include "audio_output_switch.h"

#include <stdbool.h>
#include <string.h>

#include "esp_event.h"

#include "bluetooth_service.h"
#include "i2s_service.h"

static audio_output_pcm_provider_t s_provider;
static void *s_provider_ctx;
static audio_output_target_t s_target = AUDIO_OUTPUT_TARGET_I2S;
static bool s_initialised;

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

static esp_err_t audio_output_switch_unregister_target(audio_output_target_t target)
{
    audio_output_register_pcm_provider_fn register_fn = audio_output_switch_get_register_fn(target);
    esp_err_t err;

    if (register_fn)
    {
        err = register_fn(NULL, NULL);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    switch (target)
    {
    case AUDIO_OUTPUT_TARGET_BLUETOOTH:
        return bluetooth_service_suspend_audio();
    case AUDIO_OUTPUT_TARGET_I2S:
        return i2s_service_suspend_audio();
    default:
        return ESP_OK;
    }
}

static esp_err_t audio_output_switch_register_target(audio_output_target_t target)
{
    audio_output_register_pcm_provider_fn register_fn = audio_output_switch_get_register_fn(target);
    esp_err_t err;

    if (!register_fn)
    {
        return ESP_ERR_INVALID_ARG;
    }

    err = register_fn(audio_output_switch_dispatch_pcm, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    switch (target)
    {
    case AUDIO_OUTPUT_TARGET_BLUETOOTH:
        if (!bluetooth_service_is_initialised() || !bluetooth_service_is_a2dp_connected())
        {
            register_fn(NULL, NULL);
            return ESP_ERR_INVALID_STATE;
        }
        err = bluetooth_service_start_audio();
        if (err != ESP_OK)
        {
            register_fn(NULL, NULL);
        }
        return err;
    case AUDIO_OUTPUT_TARGET_I2S:
        err = i2s_service_start_audio();
        if (err != ESP_OK)
        {
            register_fn(NULL, NULL);
        }
        return err;
    default:
        register_fn(NULL, NULL);
        return ESP_ERR_INVALID_ARG;
    }
}

static void audio_output_switch_on_bt_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)arg;
    (void)base;

    if (id != BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE || !event_data)
    {
        return;
    }

    esp_a2d_connection_state_t state = *(esp_a2d_connection_state_t *)event_data;
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED)
    {
        if (s_target != AUDIO_OUTPUT_TARGET_BLUETOOTH)
        {
            audio_output_switch_select(AUDIO_OUTPUT_TARGET_BLUETOOTH);
        }
    }
    else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
    {
        if (s_target == AUDIO_OUTPUT_TARGET_BLUETOOTH)
        {
            audio_output_switch_select(AUDIO_OUTPUT_TARGET_I2S);
        }
    }
}

esp_err_t audio_output_switch_init(void)
{
    esp_err_t err;

    if (s_initialised)
    {
        return ESP_OK;
    }

    err = esp_event_handler_register(BLUETOOTH_SERVICE_EVENT,
                                     BLUETOOTH_SVC_EVENT_A2DP_CONNECTION_STATE,
                                     audio_output_switch_on_bt_event,
                                     NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    s_initialised = true;
    return ESP_OK;
}

esp_err_t audio_output_switch_set_provider(audio_output_pcm_provider_t provider, void *user_ctx)
{
    s_provider = provider;
    s_provider_ctx = user_ctx;

    return audio_output_switch_register_target(s_target);
}

esp_err_t audio_output_switch_select(audio_output_target_t target)
{
    esp_err_t err;
    audio_output_target_t previous_target = s_target;

    if (target == previous_target)
    {
        return ESP_OK;
    }

    if (target == AUDIO_OUTPUT_TARGET_BLUETOOTH &&
        (!bluetooth_service_is_initialised() || !bluetooth_service_is_a2dp_connected()))
    {
        return ESP_ERR_INVALID_STATE;
    }

    err = audio_output_switch_unregister_target(previous_target);
    if (err != ESP_OK)
    {
        return err;
    }

    s_target = target;
    err = audio_output_switch_register_target(target);
    if (err != ESP_OK)
    {
        s_target = previous_target;
        audio_output_switch_register_target(previous_target);
        return err;
    }

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
    case AUDIO_OUTPUT_TARGET_BLUETOOTH:
        return "a2dp";
    case AUDIO_OUTPUT_TARGET_I2S:
        return "i2s";
    default:
        return "unknown";
    }
}