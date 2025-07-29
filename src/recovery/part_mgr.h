#ifdef BUILD_FACTORY_APP

#pragma once

#include <stddef.h>
#include "esp_err.h"
#include "esp_partition.h"

#define OTA_PARTITION_LABEL "app0"
#define OTA_PARTITION_TYPE ESP_PARTITION_TYPE_APP
#define OTA_PARTITION_SUBTYPE ESP_PARTITION_SUBTYPE_APP_OTA_0
#define OTA_PARTITION_OFFSET 0x100000
#define OTA_PARTITION_SIZE 0x280000

#define OTADATA_PARTITION_LABEL "otadata"
#define OTADATA_PARTITION_TYPE ESP_PARTITION_TYPE_DATA
#define OTADATA_PARTITION_SUBTYPE ESP_PARTITION_SUBTYPE_DATA_OTA
#define OTADATA_PARTITION_OFFSET 0xFE000
#define OTADATA_PARTITION_SIZE 0x2000

#define LITTLEFS_PARTITION_LABEL "littlefs"
#define LITTLEFS_PARTITION_TYPE ESP_PARTITION_TYPE_DATA
#define LITTLEFS_PARTITION_SUBTYPE ESP_PARTITION_SUBTYPE_DATA_LITTLEFS
#define LITTLEFS_PARTITION_OFFSET 0x10000
#define LITTLEFS_PARTITION_SIZE 0xEE000

#define NVS_PARTITION_LABEL "nvs"
#define NVS_PARTITION_TYPE ESP_PARTITION_TYPE_DATA
#define NVS_PARTITION_SUBTYPE ESP_PARTITION_SUBTYPE_DATA_NVS
#define NVS_PARTITION_OFFSET 0x9000
#define NVS_PARTITION_SIZE 0x7000

typedef enum
{
    PART_MGR_PARTITION_TYPE_APP,
    PART_MGR_PARTITION_TYPE_OTADATA,
    PART_MGR_PARTITION_TYPE_LITTLEFS,
    PART_MGR_PARTITION_TYPE_NVS,
} part_mgr_partition_type_t;

/**
 * @brief  Locate a partition by type.
 * @param  type        Partition type to find
 * @param  partition   Output pointer to the found partition
 * @return ESP_OK on success or an esp_err_t on failure
 */
esp_err_t part_mgr_get_partition(part_mgr_partition_type_t type, esp_partition_t **partition);

/**
 * @brief  Erase an entire partition.
 * @param  partition   The partition to erase
 * @return ESP_OK on success or an esp_err_t on failure
 */
esp_err_t part_mgr_erase_partition(const esp_partition_t *partition);

/**
 * @brief  Write data into a partition.
 * @param  partition   The partition to write to
 * @param  data        Pointer to data buffer
 * @param  size        Number of bytes to write
 * @param  offset      Offset within the partition
 * @return ESP_OK on success or an esp_err_t on failure
 */
esp_err_t part_mgr_write_partition(const esp_partition_t *partition, const void *data, size_t size, size_t offset);

/**
 * @brief  Read data from a partition.
 * @param  partition   The partition to read from
 * @param  offset      Offset within the partition
 * @param  data        Pointer to data buffer
 * @param  size        Number of bytes to read
 * @return ESP_OK on success or an esp_err_t on failure
 */
esp_err_t part_mgr_read_partition(const esp_partition_t *partition, size_t offset, void *data, size_t size);

/**
 * @brief  Verify a partition against a SHA256 hash.
 * @param  partition   The partition to verify
 * @param  sha256_hash The expected SHA256 hash (32 bytes)
 * @param  size        The size of the data to verify
 * @return ESP_OK on success or an esp_err_t on failure
 */
esp_err_t part_mgr_verify_partition(const esp_partition_t *partition, const uint8_t *sha256_hash, size_t size);

#endif