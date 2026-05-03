#pragma once

#include "nvs.h"

esp_err_t nvs_get_i32_or_init(nvs_handle_t handle, const char *key, int32_t *out, int32_t default_val)
{
    esp_err_t err = nvs_get_i32(handle, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        *out = default_val;
        err = nvs_set_i32(handle, key, default_val);
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
    }
    return err;
}

esp_err_t nvs_get_str_or_init(nvs_handle_t handle, const char *key, char *out, size_t buf_len, const char *default_val)
{
    size_t required_len = buf_len;
    esp_err_t err = nvs_get_str(handle, key, out, &required_len);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        strncpy(out, default_val, buf_len - 1);
        out[buf_len - 1] = '\0'; // ensure null termination
        err = nvs_set_str(handle, key, default_val);
        if (err == ESP_OK)
        {
            err = nvs_commit(handle);
        }
    }
    return err;
}