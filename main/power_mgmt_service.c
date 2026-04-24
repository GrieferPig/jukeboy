#include "power_mgmt_service.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_defs.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include <soc/soc.h>

ESP_EVENT_DEFINE_BASE(POWER_MGMT_SERVICE_EVENT);

#define POWER_MGMT_EVENT_POST_TIMEOUT_MS 1000
#define POWER_MGMT_MAX_SHUTDOWN_CALLBACKS 8
#define POWER_MGMT_DAC_MUTE_SETTLE_MS 1

static const char *TAG = "power_mgmt_svc";

typedef struct
{
    gpio_num_t gate_pin;
    const char *name;
} power_mgmt_rail_config_t;

typedef struct
{
    power_mgmt_service_shutdown_callback_t callback;
    void *user_ctx;
    int priority;
} power_mgmt_service_shutdown_entry_t;

static SemaphoreHandle_t s_service_lock;
static power_mgmt_service_shutdown_entry_t s_shutdown_callbacks[POWER_MGMT_MAX_SHUTDOWN_CALLBACKS];
static size_t s_shutdown_callback_count;
static size_t s_rail_refcount[POWER_MGMT_RAIL_COUNT];
static power_mgmt_rail_override_t s_rail_override[POWER_MGMT_RAIL_COUNT];
static bool s_rail_enabled[POWER_MGMT_RAIL_COUNT];
static i2s_chan_handle_t s_dac_i2s_channel;
static power_mgmt_service_dac_prepare_off_callback_t s_dac_prepare_off_callback;
static void *s_dac_prepare_off_user_ctx;
static bool s_dac_muted = true;
static bool s_initialized;

static const power_mgmt_rail_config_t s_rail_configs[POWER_MGMT_RAIL_COUNT] = {
    [POWER_MGMT_RAIL_DAC] = {
        .gate_pin = HAL_DAC_POWER_GATE_PIN,
        .name = "dac",
    },
    [POWER_MGMT_RAIL_LED] = {
        .gate_pin = HAL_LED_GATE_PIN,
        .name = "led",
    },
};

static const i2s_std_gpio_config_t s_dac_i2s_gpio_connected = {
    .mclk = I2S_GPIO_UNUSED,
    .bclk = HAL_I2S_BCLK_PIN,
    .ws = HAL_I2S_WS_PIN,
    .dout = HAL_I2S_DATA_PIN,
    .din = I2S_GPIO_UNUSED,
    .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
    },
};

static const i2s_std_gpio_config_t s_dac_i2s_gpio_disconnected = {
    .mclk = I2S_GPIO_UNUSED,
    .bclk = I2S_GPIO_UNUSED,
    .ws = I2S_GPIO_UNUSED,
    .dout = I2S_GPIO_UNUSED,
    .din = I2S_GPIO_UNUSED,
    .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
    },
};

#define CUSTOM_DOWNLOAD_MAGIC_WORD 0xDEADBEEF
#define CUSTOM_FLAG_ADDR ((volatile uint32_t *)0x3FF81FFC)

static bool power_mgmt_service_is_valid_rail(power_mgmt_rail_t rail)
{
    return rail >= 0 && rail < POWER_MGMT_RAIL_COUNT;
}

static bool power_mgmt_service_effective_enabled(power_mgmt_rail_t rail)
{
    switch (s_rail_override[rail])
    {
    case POWER_MGMT_OVERRIDE_FORCE_ON:
        return true;
    case POWER_MGMT_OVERRIDE_FORCE_OFF:
        return false;
    case POWER_MGMT_OVERRIDE_AUTO:
    default:
        return s_rail_refcount[rail] > 0;
    }
}

static esp_err_t power_mgmt_service_configure_gpio_disabled(gpio_num_t pin)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    return gpio_config(&cfg);
}

static esp_err_t power_mgmt_service_configure_dac_mute_locked(bool enabled)
{
    if (!enabled)
    {
        return power_mgmt_service_configure_gpio_disabled(HAL_DAC_MUTE_PIN);
    }

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << HAL_DAC_MUTE_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK)
    {
        return err;
    }

    return gpio_set_level(HAL_DAC_MUTE_PIN, s_dac_muted ? 0 : 1);
}

static esp_err_t power_mgmt_service_reconfig_dac_i2s_gpio_locked(bool enabled)
{
    if (s_dac_i2s_channel == NULL)
    {
        return ESP_OK;
    }

    return i2s_channel_reconfig_std_gpio(s_dac_i2s_channel,
                                         enabled ? &s_dac_i2s_gpio_connected : &s_dac_i2s_gpio_disconnected);
}

static esp_err_t power_mgmt_service_apply_dac_pin_state_locked(bool enabled)
{
    esp_err_t err;

    if (enabled)
    {
        s_dac_muted = true;

        err = gpio_set_level(HAL_DAC_POWER_GATE_PIN, 0);
        if (err != ESP_OK)
        {
            return err;
        }

        err = power_mgmt_service_configure_dac_mute_locked(true);
        if (err != ESP_OK)
        {
            return err;
        }

        return power_mgmt_service_reconfig_dac_i2s_gpio_locked(true);
    }

    s_dac_muted = true;

    err = power_mgmt_service_configure_dac_mute_locked(true);
    if (err != ESP_OK)
    {
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(POWER_MGMT_DAC_MUTE_SETTLE_MS));

    if (s_dac_prepare_off_callback != NULL)
    {
        err = s_dac_prepare_off_callback(s_dac_prepare_off_user_ctx);
        if (err != ESP_OK)
        {
            return err;
        }
    }

    err = power_mgmt_service_reconfig_dac_i2s_gpio_locked(false);
    if (err != ESP_OK)
    {
        return err;
    }

    err = power_mgmt_service_configure_gpio_disabled(HAL_I2S_BCLK_PIN);
    if (err != ESP_OK)
    {
        return err;
    }

    err = power_mgmt_service_configure_gpio_disabled(HAL_I2S_WS_PIN);
    if (err != ESP_OK)
    {
        return err;
    }

    err = power_mgmt_service_configure_gpio_disabled(HAL_I2S_DATA_PIN);
    if (err != ESP_OK)
    {
        return err;
    }

    err = power_mgmt_service_configure_dac_mute_locked(false);
    if (err != ESP_OK)
    {
        return err;
    }

    return gpio_set_level(HAL_DAC_POWER_GATE_PIN, 1);
}

static esp_err_t power_mgmt_service_apply_locked(power_mgmt_rail_t rail)
{
    bool enabled = power_mgmt_service_effective_enabled(rail);
    bool was_enabled = s_rail_enabled[rail];
    esp_err_t err;

    if (enabled == was_enabled)
    {
        return ESP_OK;
    }

    if (enabled)
    {
        if (rail == POWER_MGMT_RAIL_DAC)
        {
            err = power_mgmt_service_apply_dac_pin_state_locked(true);
        }
        else
        {
            err = gpio_set_level(s_rail_configs[rail].gate_pin, 0);
        }
    }
    else
    {
        if (rail == POWER_MGMT_RAIL_DAC)
        {
            err = power_mgmt_service_apply_dac_pin_state_locked(false);
        }
        else
        {
            err = gpio_set_level(s_rail_configs[rail].gate_pin, 1);
        }
    }

    if (err != ESP_OK)
    {
        return err;
    }

    s_rail_enabled[rail] = enabled;
    xSemaphoreGive(s_service_lock);

    err = esp_event_post(POWER_MGMT_SERVICE_EVENT,
                         enabled ? POWER_MGMT_SERVICE_EVENT_RAIL_ON : POWER_MGMT_SERVICE_EVENT_RAIL_OFF,
                         &rail,
                         sizeof(rail),
                         pdMS_TO_TICKS(POWER_MGMT_EVENT_POST_TIMEOUT_MS));

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG,
                 "rail %s -> %s (refcount=%u override=%u)",
                 s_rail_configs[rail].name,
                 enabled ? "on" : "off",
                 (unsigned)s_rail_refcount[rail],
                 (unsigned)s_rail_override[rail]);
    }

    return err;
}

esp_err_t power_mgmt_service_bind_dac_i2s_channel(i2s_chan_handle_t tx_channel,
                                                  power_mgmt_service_dac_prepare_off_callback_t prepare_off_callback,
                                                  void *prepare_off_user_ctx)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(tx_channel != NULL, ESP_ERR_INVALID_ARG, TAG, "I2S channel is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);

    if (s_dac_i2s_channel != NULL && s_dac_i2s_channel != tx_channel)
    {
        xSemaphoreGive(s_service_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_dac_i2s_channel = tx_channel;
    s_dac_prepare_off_callback = prepare_off_callback;
    s_dac_prepare_off_user_ctx = prepare_off_user_ctx;
    esp_err_t err = power_mgmt_service_reconfig_dac_i2s_gpio_locked(s_rail_enabled[POWER_MGMT_RAIL_DAC]);
    xSemaphoreGive(s_service_lock);

    return err;
}

static esp_err_t power_mgmt_service_set_all_rails_off(void)
{
    esp_err_t err = ESP_OK;

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    for (size_t index = 0; index < POWER_MGMT_RAIL_COUNT; index++)
    {
        s_rail_override[index] = POWER_MGMT_OVERRIDE_FORCE_OFF;
        s_rail_refcount[index] = 0;

        esp_err_t apply_err = power_mgmt_service_apply_locked((power_mgmt_rail_t)index);
        if (err == ESP_OK && apply_err != ESP_OK)
        {
            err = apply_err;
        }
    }
    xSemaphoreGive(s_service_lock);

    return err;
}

esp_err_t power_mgmt_service_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    s_service_lock = xSemaphoreCreateMutex();
    if (s_service_lock == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t rail_gpio_cfg = {
        .pin_bit_mask = (1ULL << HAL_DAC_POWER_GATE_PIN) | (1ULL << HAL_LED_GATE_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&rail_gpio_cfg);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    err = gpio_set_level(HAL_DAC_POWER_GATE_PIN, 1);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    err = gpio_set_level(HAL_LED_GATE_PIN, 1);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    err = power_mgmt_service_configure_dac_mute_locked(false);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    err = power_mgmt_service_configure_gpio_disabled(HAL_I2S_BCLK_PIN);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    err = power_mgmt_service_configure_gpio_disabled(HAL_I2S_WS_PIN);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    err = power_mgmt_service_configure_gpio_disabled(HAL_I2S_DATA_PIN);
    if (err != ESP_OK)
    {
        vSemaphoreDelete(s_service_lock);
        s_service_lock = NULL;
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t power_mgmt_service_register_shutdown_callback(power_mgmt_service_shutdown_callback_t callback,
                                                        void *user_ctx,
                                                        int priority)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(callback != NULL, ESP_ERR_INVALID_ARG, TAG, "shutdown callback is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);

    for (size_t index = 0; index < s_shutdown_callback_count; index++)
    {
        if (s_shutdown_callbacks[index].callback == callback &&
            s_shutdown_callbacks[index].user_ctx == user_ctx)
        {
            xSemaphoreGive(s_service_lock);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_shutdown_callback_count >= POWER_MGMT_MAX_SHUTDOWN_CALLBACKS)
    {
        xSemaphoreGive(s_service_lock);
        return ESP_ERR_NO_MEM;
    }

    size_t insert_index = s_shutdown_callback_count;
    while (insert_index > 0 && priority < s_shutdown_callbacks[insert_index - 1].priority)
    {
        s_shutdown_callbacks[insert_index] = s_shutdown_callbacks[insert_index - 1];
        insert_index--;
    }

    s_shutdown_callbacks[insert_index].callback = callback;
    s_shutdown_callbacks[insert_index].user_ctx = user_ctx;
    s_shutdown_callbacks[insert_index].priority = priority;
    s_shutdown_callback_count++;

    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}

esp_err_t power_mgmt_service_rail_request(power_mgmt_rail_t rail)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(power_mgmt_service_is_valid_rail(rail), ESP_ERR_INVALID_ARG, TAG, "invalid rail");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    s_rail_refcount[rail]++;
    esp_err_t err = power_mgmt_service_apply_locked(rail);
    xSemaphoreGive(s_service_lock);
    return err;
}

esp_err_t power_mgmt_service_rail_release(power_mgmt_rail_t rail)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(power_mgmt_service_is_valid_rail(rail), ESP_ERR_INVALID_ARG, TAG, "invalid rail");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    if (s_rail_refcount[rail] == 0)
    {
        xSemaphoreGive(s_service_lock);
        ESP_LOGE(TAG, "rail %u release underflow", (unsigned)rail);
        return ESP_ERR_INVALID_STATE;
    }

    s_rail_refcount[rail]--;
    esp_err_t err = power_mgmt_service_apply_locked(rail);
    xSemaphoreGive(s_service_lock);
    return err;
}

esp_err_t power_mgmt_service_set_dac_muted(bool muted)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    if (!s_rail_enabled[POWER_MGMT_RAIL_DAC])
    {
        xSemaphoreGive(s_service_lock);
        return ESP_ERR_INVALID_STATE;
    }

    s_dac_muted = muted;
    esp_err_t err = gpio_set_level(HAL_DAC_MUTE_PIN, muted ? 0 : 1);
    xSemaphoreGive(s_service_lock);

    return err;
}

esp_err_t power_mgmt_service_rail_is_enabled(power_mgmt_rail_t rail, bool *enabled_out)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(power_mgmt_service_is_valid_rail(rail), ESP_ERR_INVALID_ARG, TAG, "invalid rail");
    ESP_RETURN_ON_FALSE(enabled_out != NULL, ESP_ERR_INVALID_ARG, TAG, "enabled output is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    *enabled_out = s_rail_enabled[rail];
    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}

esp_err_t power_mgmt_service_rail_get_refcount(power_mgmt_rail_t rail, size_t *refcount_out)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(power_mgmt_service_is_valid_rail(rail), ESP_ERR_INVALID_ARG, TAG, "invalid rail");
    ESP_RETURN_ON_FALSE(refcount_out != NULL, ESP_ERR_INVALID_ARG, TAG, "refcount output is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    *refcount_out = s_rail_refcount[rail];
    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}

esp_err_t power_mgmt_service_rail_get_override(power_mgmt_rail_t rail,
                                               power_mgmt_rail_override_t *override_out)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(power_mgmt_service_is_valid_rail(rail), ESP_ERR_INVALID_ARG, TAG, "invalid rail");
    ESP_RETURN_ON_FALSE(override_out != NULL, ESP_ERR_INVALID_ARG, TAG, "override output is null");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    *override_out = s_rail_override[rail];
    xSemaphoreGive(s_service_lock);

    return ESP_OK;
}

esp_err_t power_mgmt_service_rail_set_override(power_mgmt_rail_t rail,
                                               power_mgmt_rail_override_t override_mode)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");
    ESP_RETURN_ON_FALSE(power_mgmt_service_is_valid_rail(rail), ESP_ERR_INVALID_ARG, TAG, "invalid rail");
    ESP_RETURN_ON_FALSE(override_mode >= POWER_MGMT_OVERRIDE_AUTO &&
                            override_mode <= POWER_MGMT_OVERRIDE_FORCE_OFF,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "invalid override");

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    s_rail_override[rail] = override_mode;
    esp_err_t err = power_mgmt_service_apply_locked(rail);
    xSemaphoreGive(s_service_lock);
    return err;
}

static esp_err_t power_mgmt_service_run_shutdown_callbacks(void)
{
    power_mgmt_service_shutdown_entry_t callbacks[POWER_MGMT_MAX_SHUTDOWN_CALLBACKS];
    size_t callback_count = 0;

    xSemaphoreTake(s_service_lock, portMAX_DELAY);
    callback_count = s_shutdown_callback_count;
    for (size_t index = 0; index < callback_count; index++)
    {
        callbacks[index] = s_shutdown_callbacks[index];
    }
    xSemaphoreGive(s_service_lock);

    esp_err_t err = esp_event_post(POWER_MGMT_SERVICE_EVENT,
                                   POWER_MGMT_SERVICE_EVENT_SHUTDOWN,
                                   NULL,
                                   0,
                                   pdMS_TO_TICKS(POWER_MGMT_EVENT_POST_TIMEOUT_MS));
    if (err != ESP_OK)
    {
        return err;
    }

    for (size_t index = 0; index < callback_count; index++)
    {
        err = callbacks[index].callback(callbacks[index].user_ctx);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG,
                     "shutdown callback %u failed: %s",
                     (unsigned)index,
                     esp_err_to_name(err));
            return err;
        }
    }

    return ESP_OK;
}

static void power_mgmt_service_reboot_to_download_task(void *param)
{
    (void)param;

    ESP_LOGI(TAG, "shutdown callbacks complete, entering download mode");
    (*CUSTOM_FLAG_ADDR) = CUSTOM_DOWNLOAD_MAGIC_WORD;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

esp_err_t power_mgmt_service_reboot(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");

    esp_err_t err = power_mgmt_service_run_shutdown_callbacks();
    if (err == ESP_OK)
    {
        err = power_mgmt_service_set_all_rails_off();
    }
    if (err != ESP_OK)
    {
        return err;
    }

    ESP_LOGI(TAG, "shutdown callbacks complete, restarting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}

esp_err_t power_mgmt_service_reboot_to_download(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "power management service is not initialized");

    esp_err_t err = power_mgmt_service_run_shutdown_callbacks();
    if (err == ESP_OK)
    {
        err = power_mgmt_service_set_all_rails_off();
    }
    if (err != ESP_OK)
    {
        return err;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(power_mgmt_service_reboot_to_download_task,
                                                      "rdl",
                                                      2048,
                                                      NULL,
                                                      configMAX_PRIORITIES - 1,
                                                      NULL,
                                                      0);
    if (task_created != pdPASS)
    {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}