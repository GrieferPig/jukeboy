#include "runtime_env.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG = "runtime_env";

bool app_is_running_in_qemu(void)
{
    uint8_t factory_mac[6] = {0};
    static const uint8_t zero_mac[sizeof(factory_mac)] = {0};
    esp_err_t err = esp_efuse_mac_get_default(factory_mac);

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG,
                 "failed to read factory eFuse MAC (%s); assuming real hardware",
                 esp_err_to_name(err));
        return false;
    }

    if (memcmp(factory_mac, zero_mac, sizeof(factory_mac)) == 0)
    {
        return true;
    }

    return false;
}