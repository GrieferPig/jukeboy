#ifdef BUILD_FACTORY_APP

#include "esp_partition.h"
#include "esp_log.h"
#include "part_mgr.h"
#include "mbedtls/sha256.h"
#include <string.h>

// Need to check return value
esp_err_t part_mgr_get_partition(part_mgr_partition_type_t type, esp_partition_t **partition)
{
    if (partition == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    const esp_partition_t *found_partition = NULL;

    switch (type)
    {
    case PART_MGR_PARTITION_TYPE_APP:
        found_partition = esp_partition_find_first(OTA_PARTITION_TYPE, OTA_PARTITION_SUBTYPE, OTA_PARTITION_LABEL);
        break;
    case PART_MGR_PARTITION_TYPE_OTADATA:
        found_partition = esp_partition_find_first(OTADATA_PARTITION_TYPE, OTADATA_PARTITION_SUBTYPE, OTADATA_PARTITION_LABEL);
        break;
    case PART_MGR_PARTITION_TYPE_LITTLEFS:
        found_partition = esp_partition_find_first(LITTLEFS_PARTITION_TYPE, LITTLEFS_PARTITION_SUBTYPE, LITTLEFS_PARTITION_LABEL);
        break;
    case PART_MGR_PARTITION_TYPE_NVS:
        found_partition = esp_partition_find_first(NVS_PARTITION_TYPE, NVS_PARTITION_SUBTYPE, NVS_PARTITION_LABEL);
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (found_partition == NULL)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // Ensure the partition is of the expected type and subtype
    ESP_LOGI("part_mgr", "Found partition: type=%d, subtype=%d, label=%s", found_partition->type, found_partition->subtype, found_partition->label);
    switch (type)
    {
    case PART_MGR_PARTITION_TYPE_APP:
        if (found_partition->type != OTA_PARTITION_TYPE || found_partition->subtype != OTA_PARTITION_SUBTYPE)
        {
            return ESP_ERR_NOT_FOUND;
        }
        break;
    case PART_MGR_PARTITION_TYPE_OTADATA:
        if (found_partition->type != OTADATA_PARTITION_TYPE || found_partition->subtype != OTADATA_PARTITION_SUBTYPE)
        {
            return ESP_ERR_NOT_FOUND;
        }
        break;
    case PART_MGR_PARTITION_TYPE_LITTLEFS:
        if (found_partition->type != LITTLEFS_PARTITION_TYPE || found_partition->subtype != LITTLEFS_PARTITION_SUBTYPE)
        {
            return ESP_ERR_NOT_FOUND;
        }
        break;
    case PART_MGR_PARTITION_TYPE_NVS:
        if (found_partition->type != NVS_PARTITION_TYPE || found_partition->subtype != NVS_PARTITION_SUBTYPE)
        {
            return ESP_ERR_NOT_FOUND;
        }
        break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    *partition = (esp_partition_t *)found_partition;
    return ESP_OK;
}

esp_err_t part_mgr_erase_partition(const esp_partition_t *partition)
{
    if (partition == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

esp_err_t part_mgr_write_partition(const esp_partition_t *partition, const void *data, size_t size, size_t offset)
{
    if (partition == NULL || data == NULL || size == 0 || offset + size > partition->size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_partition_write(partition, offset, data, size);
    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

esp_err_t part_mgr_read_partition(const esp_partition_t *partition, size_t offset, void *data, size_t size)
{
    if (partition == NULL || data == NULL || size == 0 || offset + size > partition->size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_partition_read(partition, offset, data, size);
    if (err != ESP_OK)
    {
        return err;
    }

    return ESP_OK;
}

esp_err_t part_mgr_verify_partition(const esp_partition_t *partition, const uint8_t *sha256_hash, size_t size)
{
    if (partition == NULL || sha256_hash == NULL || size == 0 || size > partition->size)
    {
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_sha256_context sha_ctx;
    uint8_t calculated_hash[32];
    uint8_t read_buffer[1024];
    esp_err_t err = ESP_OK;

    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts(&sha_ctx, 0); // 0 for SHA-256

    uint32_t bytes_read = 0;
    while (bytes_read < size)
    {
        size_t to_read = (size - bytes_read) > sizeof(read_buffer)
                             ? sizeof(read_buffer)
                             : (size - bytes_read);

        err = esp_partition_read(partition, bytes_read, read_buffer, to_read);
        if (err != ESP_OK)
        {
            goto cleanup;
        }
        mbedtls_sha256_update(&sha_ctx, read_buffer, to_read);
        bytes_read += to_read;
    }

    mbedtls_sha256_finish(&sha_ctx, calculated_hash);

    if (memcmp(calculated_hash, sha256_hash, 32) != 0)
    {
        err = ESP_FAIL; // Hash mismatch
    }

cleanup:
    mbedtls_sha256_free(&sha_ctx);
    return err;
}

#endif // BUILD_FACTORY_APP