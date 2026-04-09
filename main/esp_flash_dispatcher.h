/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Configuration for the flash dispatcher task.
     * The dispatcher serializes flash operations to a dedicated task.
     */
    typedef struct
    {
        uint32_t task_stack_size;
        uint32_t task_priority;
        BaseType_t task_core_id;
        uint32_t queue_size;
    } esp_flash_dispatcher_config_t;

    typedef struct
    {
        size_t pending_write_count;
        size_t pending_write_bytes;
        size_t arena_free_bytes;
        uint64_t last_flush_time_us;
    } esp_flash_dispatcher_status_t;

/**
 * @brief Default configuration to init flash dispatcher.
 */
#define ESP_FLASH_DISPATCHER_DEFAULT_CONFIG { \
    .task_stack_size = 2048,                  \
    .task_priority = 10,                      \
    .task_core_id = tskNO_AFFINITY,           \
    .queue_size = 1,                          \
}

    /**
     * @brief Initialize flash dispatcher.
     *
     * @param[in] cfg Configuration structure.
     *
     * @return
     *      - ESP_OK on success
     *      - ESP_ERR_INVALID_ARG if cfg is NULL
     *      - ESP_ERR_NO_MEM if there is no memory for allocating dispatcher resources
     *      - ESP_ERR_INVALID_STATE if the dispatcher is already initialized
     */
    esp_err_t esp_flash_dispatcher_init(const esp_flash_dispatcher_config_t *cfg);

    /**
     * @brief Flush all queued flash writes to the actual flash chip.
     *
     * @param[out] write_time_us Optional pointer receiving elapsed flush time in microseconds.
     *
     * @return
     *      - ESP_OK on success
     *      - ESP_ERR_INVALID_STATE if the dispatcher is not initialized
     *      - Or a flash error code if a queued write fails
     */
    esp_err_t esp_flash_dispatcher_flush_writes(uint64_t *write_time_us);

    /**
     * @brief Query queued write status and the last recorded flush time.
     *
     * @param[out] status Status snapshot.
     *
     * @return
     *      - ESP_OK on success
     *      - ESP_ERR_INVALID_ARG if status is NULL
     *      - ESP_ERR_INVALID_STATE if the dispatcher is not initialized
     */
    esp_err_t esp_flash_dispatcher_get_status(esp_flash_dispatcher_status_t *status);

#ifdef __cplusplus
}
#endif