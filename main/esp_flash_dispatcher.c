/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp32/himem.h"
#include "esp_flash_dispatcher.h"
#include "power_mgmt_service.h"

static const char *TAG = "flash_dispatcher";

// Queue storage, semaphores, and task stacks must stay in byte-addressable internal RAM.
#define FLASH_DISPATCHER_MEM_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define FLASH_DISPATCHER_PENDING_BUFFER_SIZE (256U * 1024U)
#define FLASH_DISPATCHER_ARENA_ALIGNMENT 8U
#define FLASH_DISPATCHER_ARENA_ALIGN_UP(value) \
    (((size_t)(value) + FLASH_DISPATCHER_ARENA_ALIGNMENT - 1U) & ~(size_t)(FLASH_DISPATCHER_ARENA_ALIGNMENT - 1U))
#define FLASH_DISPATCHER_ARENA_NULL_OFFSET UINT32_MAX

extern esp_err_t __real_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t size);
extern esp_err_t __real_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t size);
extern esp_err_t __real_esp_flash_erase_region(esp_flash_t *chip, uint32_t start_address, uint32_t size);
extern esp_err_t __real_esp_flash_erase_chip(esp_flash_t *chip);
extern esp_err_t __real_esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address, const void *buffer, uint32_t length);

typedef enum
{
    FLASH_OP_READ = 0,
    FLASH_OP_ERASE_REGION,
    FLASH_OP_ERASE_CHIP,
    FLASH_OP_FLUSH_PENDING_WRITES,
} flash_operation_t;

typedef struct
{
    uint32_t payload_size;
    uint32_t next_block_offset;
    uint32_t prev_block_offset;
    uint32_t next_free_block_offset;
    uint32_t prev_free_block_offset;
    uint8_t is_free;
    uint8_t reserved[3];
} flash_arena_block_header_t;

typedef struct
{
    uint32_t next_node_offset;
    esp_flash_t *chip;
    uint32_t address;
    uint32_t size;
    uint8_t encrypted;
    uint8_t reserved[3];
} flash_pending_write_node_t;

#define FLASH_DISPATCHER_PENDING_NODE_SIZE \
    FLASH_DISPATCHER_ARENA_ALIGN_UP(sizeof(flash_pending_write_node_t))
#define FLASH_DISPATCHER_ARENA_MIN_SPLIT_SIZE \
    (sizeof(flash_arena_block_header_t) + FLASH_DISPATCHER_ARENA_ALIGNMENT)

typedef struct
{
    flash_operation_t op;
    esp_flash_t *chip;
    union
    {
        struct
        {
            void *buffer;
            uint32_t address;
            size_t size;
        } read;
        struct
        {
            uint32_t start_address;
            size_t size;
        } erase_region;
    } args;
} flash_operation_request_t;

typedef struct
{
    esp_err_t err;
    uint64_t duration_us;
} flash_operation_result_t;

typedef struct
{
    QueueHandle_t queue;
    QueueHandle_t result_queue;
    SemaphoreHandle_t op_lock;
    SemaphoreHandle_t pending_lock;
    TaskHandle_t task;
    bool dispatcher_initialized;
    esp_himem_handle_t write_buffer_handle;
    esp_himem_rangehandle_t write_buffer_range;
    uint8_t *write_buffer_ptr;
    size_t write_buffer_capacity;
    size_t pending_write_count;
    size_t pending_write_bytes;
    uint32_t pending_head_offset;
    uint32_t pending_tail_offset;
    uint32_t arena_first_block_offset;
    uint32_t arena_first_free_block_offset;
    uint64_t last_flush_time_us;
} flash_dispatcher_context_t;

static flash_dispatcher_context_t s_flash_dispatcher_ctx;

static void flash_dispatcher_release_pending_buffer(void)
{
    if (s_flash_dispatcher_ctx.write_buffer_range != NULL)
    {
        esp_himem_free_map_range(s_flash_dispatcher_ctx.write_buffer_range);
        s_flash_dispatcher_ctx.write_buffer_range = NULL;
    }

    if (s_flash_dispatcher_ctx.write_buffer_handle != NULL)
    {
        esp_himem_free(s_flash_dispatcher_ctx.write_buffer_handle);
        s_flash_dispatcher_ctx.write_buffer_handle = NULL;
    }

    if (s_flash_dispatcher_ctx.write_buffer_ptr != NULL)
    {
        heap_caps_free(s_flash_dispatcher_ctx.write_buffer_ptr);
        s_flash_dispatcher_ctx.write_buffer_ptr = NULL;
    }
}

static inline flash_arena_block_header_t *flash_dispatcher_arena_block_at(uint8_t *arena_ptr,
                                                                          uint32_t block_offset)
{
    if (block_offset == FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        return NULL;
    }

    return (flash_arena_block_header_t *)(arena_ptr + block_offset);
}

static inline flash_pending_write_node_t *flash_dispatcher_pending_node_at(uint8_t *arena_ptr,
                                                                           uint32_t node_offset)
{
    if (node_offset == FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        return NULL;
    }

    return (flash_pending_write_node_t *)(arena_ptr + node_offset);
}

static inline uint32_t flash_dispatcher_arena_block_payload_offset(uint32_t block_offset)
{
    return block_offset + (uint32_t)sizeof(flash_arena_block_header_t);
}

static inline uint32_t flash_dispatcher_arena_payload_to_block_offset(uint32_t payload_offset)
{
    return payload_offset - (uint32_t)sizeof(flash_arena_block_header_t);
}

static inline uint8_t *flash_dispatcher_pending_node_data_ptr(uint8_t *arena_ptr,
                                                              uint32_t node_offset)
{
    return arena_ptr + node_offset + FLASH_DISPATCHER_PENDING_NODE_SIZE;
}

static inline size_t flash_dispatcher_pending_allocation_size(uint32_t data_size)
{
    return FLASH_DISPATCHER_PENDING_NODE_SIZE + FLASH_DISPATCHER_ARENA_ALIGN_UP(data_size);
}

static esp_err_t flash_dispatcher_map_pending_buffer_locked(int flags, uint8_t **out_ptr)
{
    ESP_RETURN_ON_FALSE(out_ptr != NULL, ESP_ERR_INVALID_ARG, TAG, "pending buffer output is null");
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.write_buffer_handle != NULL ||
                            s_flash_dispatcher_ctx.write_buffer_ptr != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "pending write buffer is not allocated");

    if (s_flash_dispatcher_ctx.write_buffer_ptr != NULL)
    {
        *out_ptr = s_flash_dispatcher_ctx.write_buffer_ptr;
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.write_buffer_range == NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "pending write buffer range is already allocated");

    esp_err_t err = esp_himem_alloc_map_range(FLASH_DISPATCHER_PENDING_BUFFER_SIZE,
                                              &s_flash_dispatcher_ctx.write_buffer_range);
    if (err != ESP_OK)
    {
        ESP_EARLY_LOGE(TAG, "alloc pending write buffer range failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_himem_map(s_flash_dispatcher_ctx.write_buffer_handle,
                        s_flash_dispatcher_ctx.write_buffer_range,
                        0,
                        0,
                        FLASH_DISPATCHER_PENDING_BUFFER_SIZE,
                        flags,
                        (void **)out_ptr);
    if (err != ESP_OK)
    {
        ESP_EARLY_LOGE(TAG, "map pending write buffer failed: %s", esp_err_to_name(err));
        esp_himem_free_map_range(s_flash_dispatcher_ctx.write_buffer_range);
        s_flash_dispatcher_ctx.write_buffer_range = NULL;
    }
    return err;
}

static esp_err_t flash_dispatcher_unmap_pending_buffer_locked(uint8_t *buffer_ptr)
{
    if (buffer_ptr == NULL)
    {
        return ESP_OK;
    }

    if (s_flash_dispatcher_ctx.write_buffer_ptr != NULL)
    {
        ESP_RETURN_ON_FALSE(buffer_ptr == s_flash_dispatcher_ctx.write_buffer_ptr,
                            ESP_ERR_INVALID_ARG,
                            TAG,
                            "pending write buffer pointer mismatch");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.write_buffer_range != NULL,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "pending write buffer range is not allocated");

    esp_err_t err = esp_himem_unmap(s_flash_dispatcher_ctx.write_buffer_range,
                                    buffer_ptr,
                                    FLASH_DISPATCHER_PENDING_BUFFER_SIZE);
    if (err != ESP_OK)
    {
        ESP_EARLY_LOGE(TAG, "unmap pending write buffer failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_himem_free_map_range(s_flash_dispatcher_ctx.write_buffer_range);
    if (err != ESP_OK)
    {
        ESP_EARLY_LOGE(TAG, "free pending write buffer range failed: %s", esp_err_to_name(err));
        return err;
    }

    s_flash_dispatcher_ctx.write_buffer_range = NULL;
    return err;
}

static void flash_dispatcher_arena_init_locked(uint8_t *arena_ptr)
{
    flash_arena_block_header_t *initial_block = (flash_arena_block_header_t *)arena_ptr;

    memset(initial_block, 0, sizeof(*initial_block));
    initial_block->payload_size = FLASH_DISPATCHER_PENDING_BUFFER_SIZE - (uint32_t)sizeof(*initial_block);
    initial_block->next_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    initial_block->prev_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    initial_block->next_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    initial_block->prev_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    initial_block->is_free = 1;

    s_flash_dispatcher_ctx.pending_head_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    s_flash_dispatcher_ctx.pending_tail_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    s_flash_dispatcher_ctx.arena_first_block_offset = 0;
    s_flash_dispatcher_ctx.arena_first_free_block_offset = 0;
    s_flash_dispatcher_ctx.pending_write_count = 0;
    s_flash_dispatcher_ctx.pending_write_bytes = 0;
}

static esp_err_t flash_dispatcher_init_direct_pending_buffer(void)
{
    s_flash_dispatcher_ctx.write_buffer_ptr = heap_caps_calloc(1,
                                                               FLASH_DISPATCHER_PENDING_BUFFER_SIZE,
                                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_flash_dispatcher_ctx.write_buffer_ptr == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    flash_dispatcher_arena_init_locked(s_flash_dispatcher_ctx.write_buffer_ptr);
    return ESP_OK;
}

static void flash_dispatcher_arena_remove_free_block_locked(uint8_t *arena_ptr,
                                                            uint32_t block_offset)
{
    flash_arena_block_header_t *block = flash_dispatcher_arena_block_at(arena_ptr, block_offset);
    if (block == NULL)
    {
        return;
    }

    if (block->prev_free_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        flash_dispatcher_arena_block_at(arena_ptr, block->prev_free_block_offset)->next_free_block_offset =
            block->next_free_block_offset;
    }
    else
    {
        s_flash_dispatcher_ctx.arena_first_free_block_offset = block->next_free_block_offset;
    }

    if (block->next_free_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        flash_dispatcher_arena_block_at(arena_ptr, block->next_free_block_offset)->prev_free_block_offset =
            block->prev_free_block_offset;
    }

    block->next_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    block->prev_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
}

static void flash_dispatcher_arena_insert_free_block_locked(uint8_t *arena_ptr,
                                                            uint32_t block_offset)
{
    flash_arena_block_header_t *block = flash_dispatcher_arena_block_at(arena_ptr, block_offset);
    if (block == NULL)
    {
        return;
    }

    block->is_free = 1;
    block->prev_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    block->next_free_block_offset = s_flash_dispatcher_ctx.arena_first_free_block_offset;

    if (s_flash_dispatcher_ctx.arena_first_free_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        flash_dispatcher_arena_block_at(arena_ptr, s_flash_dispatcher_ctx.arena_first_free_block_offset)->prev_free_block_offset =
            block_offset;
    }

    s_flash_dispatcher_ctx.arena_first_free_block_offset = block_offset;
}

static void flash_dispatcher_arena_coalesce_with_next_locked(uint8_t *arena_ptr,
                                                             uint32_t block_offset)
{
    flash_arena_block_header_t *block = flash_dispatcher_arena_block_at(arena_ptr, block_offset);

    while (block != NULL && block->next_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        flash_arena_block_header_t *next_block = flash_dispatcher_arena_block_at(arena_ptr,
                                                                                 block->next_block_offset);
        if (next_block == NULL || !next_block->is_free)
        {
            break;
        }

        flash_dispatcher_arena_remove_free_block_locked(arena_ptr, block->next_block_offset);
        block->payload_size += (uint32_t)sizeof(*next_block) + next_block->payload_size;
        block->next_block_offset = next_block->next_block_offset;
        if (block->next_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
        {
            flash_dispatcher_arena_block_at(arena_ptr, block->next_block_offset)->prev_block_offset =
                block_offset;
        }
    }
}

static esp_err_t flash_dispatcher_arena_alloc_locked(uint8_t *arena_ptr,
                                                     size_t payload_size,
                                                     uint32_t *out_payload_offset)
{
    ESP_RETURN_ON_FALSE(out_payload_offset != NULL, ESP_ERR_INVALID_ARG, TAG, "payload output is null");

    const size_t aligned_payload_size = FLASH_DISPATCHER_ARENA_ALIGN_UP(payload_size);

    for (uint32_t block_offset = s_flash_dispatcher_ctx.arena_first_free_block_offset;
         block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET;)
    {
        flash_arena_block_header_t *block = flash_dispatcher_arena_block_at(arena_ptr, block_offset);
        const uint32_t next_free_block_offset = block->next_free_block_offset;

        if (block->payload_size >= aligned_payload_size)
        {
            flash_dispatcher_arena_remove_free_block_locked(arena_ptr, block_offset);

            const size_t remaining_payload_size = block->payload_size - aligned_payload_size;
            if (remaining_payload_size >= FLASH_DISPATCHER_ARENA_MIN_SPLIT_SIZE)
            {
                const uint32_t split_block_offset = flash_dispatcher_arena_block_payload_offset(block_offset) +
                                                    (uint32_t)aligned_payload_size;
                flash_arena_block_header_t *split_block = flash_dispatcher_arena_block_at(arena_ptr,
                                                                                          split_block_offset);

                memset(split_block, 0, sizeof(*split_block));
                split_block->payload_size = (uint32_t)(remaining_payload_size - sizeof(*split_block));
                split_block->prev_block_offset = block_offset;
                split_block->next_block_offset = block->next_block_offset;
                split_block->next_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
                split_block->prev_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
                split_block->is_free = 1;

                if (block->next_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
                {
                    flash_dispatcher_arena_block_at(arena_ptr, block->next_block_offset)->prev_block_offset =
                        split_block_offset;
                }

                block->next_block_offset = split_block_offset;
                block->payload_size = (uint32_t)aligned_payload_size;
                flash_dispatcher_arena_insert_free_block_locked(arena_ptr, split_block_offset);
            }

            block->is_free = 0;
            block->next_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
            block->prev_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
            *out_payload_offset = flash_dispatcher_arena_block_payload_offset(block_offset);
            return ESP_OK;
        }

        block_offset = next_free_block_offset;
    }

    return ESP_ERR_NO_MEM;
}

static void flash_dispatcher_arena_free_locked(uint8_t *arena_ptr, uint32_t payload_offset)
{
    uint32_t block_offset = flash_dispatcher_arena_payload_to_block_offset(payload_offset);
    flash_arena_block_header_t *block = flash_dispatcher_arena_block_at(arena_ptr, block_offset);
    if (block == NULL)
    {
        return;
    }

    block->is_free = 1;
    block->next_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    block->prev_free_block_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;

    flash_dispatcher_arena_coalesce_with_next_locked(arena_ptr, block_offset);

    if (block->prev_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        flash_arena_block_header_t *prev_block = flash_dispatcher_arena_block_at(arena_ptr,
                                                                                 block->prev_block_offset);
        if (prev_block != NULL && prev_block->is_free)
        {
            const uint32_t prev_block_offset = block->prev_block_offset;

            flash_dispatcher_arena_remove_free_block_locked(arena_ptr, prev_block_offset);
            prev_block->payload_size += (uint32_t)sizeof(*block) + block->payload_size;
            prev_block->next_block_offset = block->next_block_offset;
            if (block->next_block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
            {
                flash_dispatcher_arena_block_at(arena_ptr, block->next_block_offset)->prev_block_offset =
                    prev_block_offset;
            }
            block_offset = prev_block_offset;
        }
    }

    flash_dispatcher_arena_insert_free_block_locked(arena_ptr, block_offset);
}

static size_t flash_dispatcher_arena_free_bytes_locked(uint8_t *arena_ptr)
{
    size_t free_bytes = 0;

    for (uint32_t block_offset = s_flash_dispatcher_ctx.arena_first_free_block_offset;
         block_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET;)
    {
        flash_arena_block_header_t *block = flash_dispatcher_arena_block_at(arena_ptr, block_offset);
        const uint32_t next_free_block_offset = block->next_free_block_offset;

        free_bytes += block->payload_size;
        block_offset = next_free_block_offset;
    }

    return free_bytes;
}

static uint32_t flash_dispatcher_find_covering_pending_node_offset_locked(uint8_t *arena_ptr,
                                                                          esp_flash_t *chip,
                                                                          uint32_t address,
                                                                          uint32_t size,
                                                                          bool encrypted)
{
    uint32_t covering_node_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    const uint32_t write_end = address + size;

    for (uint32_t node_offset = s_flash_dispatcher_ctx.pending_head_offset;
         node_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET;)
    {
        flash_pending_write_node_t *node = flash_dispatcher_pending_node_at(arena_ptr, node_offset);
        const uint32_t next_node_offset = node->next_node_offset;
        const uint32_t node_end = node->address + node->size;

        if (node->chip == chip && node->encrypted == encrypted &&
            address >= node->address && write_end <= node_end)
        {
            covering_node_offset = node_offset;
        }

        node_offset = next_node_offset;
    }

    return covering_node_offset;
}

static void flash_dispatcher_append_pending_node_locked(uint8_t *arena_ptr, uint32_t node_offset)
{
    flash_pending_write_node_t *node = flash_dispatcher_pending_node_at(arena_ptr, node_offset);
    node->next_node_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;

    if (s_flash_dispatcher_ctx.pending_tail_offset == FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        s_flash_dispatcher_ctx.pending_head_offset = node_offset;
        s_flash_dispatcher_ctx.pending_tail_offset = node_offset;
        return;
    }

    flash_dispatcher_pending_node_at(arena_ptr, s_flash_dispatcher_ctx.pending_tail_offset)->next_node_offset =
        node_offset;
    s_flash_dispatcher_ctx.pending_tail_offset = node_offset;
}

static esp_err_t flash_dispatcher_flush_pending_locked(uint64_t *duration_us)
{
    uint8_t *arena_ptr = NULL;
    const uint64_t start_us = esp_timer_get_time();
    esp_err_t err = ESP_OK;

    if (s_flash_dispatcher_ctx.pending_head_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        err = flash_dispatcher_map_pending_buffer_locked(0, &arena_ptr);
        if (err != ESP_OK)
        {
            goto exit;
        }
    }

    while (s_flash_dispatcher_ctx.pending_head_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        const uint32_t node_offset = s_flash_dispatcher_ctx.pending_head_offset;
        flash_pending_write_node_t *node = flash_dispatcher_pending_node_at(arena_ptr, node_offset);
        uint8_t *data = flash_dispatcher_pending_node_data_ptr(arena_ptr, node_offset);
        const uint32_t next_node_offset = node->next_node_offset;

        if (node->encrypted)
        {
            err = __real_esp_flash_write_encrypted(node->chip,
                                                   node->address,
                                                   data,
                                                   node->size);
        }
        else
        {
            err = __real_esp_flash_write(node->chip,
                                         data,
                                         node->address,
                                         node->size);
        }

        if (err != ESP_OK)
        {
            break;
        }

        s_flash_dispatcher_ctx.pending_head_offset = next_node_offset;
        if (next_node_offset == FLASH_DISPATCHER_ARENA_NULL_OFFSET)
        {
            s_flash_dispatcher_ctx.pending_tail_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
        }
        s_flash_dispatcher_ctx.pending_write_count--;
        s_flash_dispatcher_ctx.pending_write_bytes -= node->size;
        flash_dispatcher_arena_free_locked(arena_ptr, node_offset);
    }

exit:
    s_flash_dispatcher_ctx.last_flush_time_us = esp_timer_get_time() - start_us;
    if (duration_us != NULL)
    {
        *duration_us = s_flash_dispatcher_ctx.last_flush_time_us;
    }
    if (arena_ptr != NULL)
    {
        esp_err_t unmap_err = flash_dispatcher_unmap_pending_buffer_locked(arena_ptr);
        if (err == ESP_OK && unmap_err != ESP_OK)
        {
            err = unmap_err;
        }
    }

    return err;
}

static esp_err_t flash_dispatcher_overlay_pending_reads_locked(esp_flash_t *chip,
                                                               void *buffer,
                                                               uint32_t address,
                                                               size_t size)
{
    if (s_flash_dispatcher_ctx.pending_head_offset == FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        return ESP_OK;
    }

    uint8_t *arena_ptr = NULL;
    esp_err_t err = flash_dispatcher_map_pending_buffer_locked(ESP_HIMEM_MAPFLAG_RO, &arena_ptr);
    if (err != ESP_OK)
    {
        return err;
    }

    uint8_t *out = (uint8_t *)buffer;
    const uint32_t read_end = address + (uint32_t)size;

    for (uint32_t node_offset = s_flash_dispatcher_ctx.pending_head_offset;
         node_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET;)
    {
        flash_pending_write_node_t *node = flash_dispatcher_pending_node_at(arena_ptr, node_offset);
        const uint32_t next_node_offset = node->next_node_offset;

        if (node->chip == chip && !node->encrypted)
        {
            const uint32_t node_end = node->address + node->size;
            const uint32_t overlap_start = address > node->address ? address : node->address;
            const uint32_t overlap_end = read_end < node_end ? read_end : node_end;

            if (overlap_end > overlap_start)
            {
                const size_t dst_offset = overlap_start - address;
                const size_t src_offset = overlap_start - node->address;
                const size_t overlap_len = overlap_end - overlap_start;

                memcpy(out + dst_offset,
                       flash_dispatcher_pending_node_data_ptr(arena_ptr, node_offset) + src_offset,
                       overlap_len);
            }
        }

        node_offset = next_node_offset;
    }

    return flash_dispatcher_unmap_pending_buffer_locked(arena_ptr);
}

static esp_err_t flash_dispatcher_queue_write(esp_flash_t *chip,
                                              const void *buffer,
                                              uint32_t address,
                                              uint32_t size,
                                              bool encrypted)
{
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.dispatcher_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "flash dispatcher is not initialized");
    ESP_RETURN_ON_FALSE(buffer != NULL, ESP_ERR_INVALID_ARG, TAG, "write buffer is null");

    if (size == 0)
    {
        return ESP_OK;
    }

    if (encrypted)
    {
        ESP_RETURN_ON_FALSE((address % 16U) == 0U, ESP_ERR_INVALID_ARG, TAG, "encrypted write address must be 16-byte aligned");
        ESP_RETURN_ON_FALSE((size % 16U) == 0U, ESP_ERR_INVALID_SIZE, TAG, "encrypted write length must be a multiple of 16");
    }

    xSemaphoreTake(s_flash_dispatcher_ctx.pending_lock, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    uint8_t *arena_ptr = NULL;
    size_t arena_free_bytes = 0;

    err = flash_dispatcher_map_pending_buffer_locked(0, &arena_ptr);
    if (err != ESP_OK)
    {
        goto exit;
    }

    const uint32_t covering_node_offset = flash_dispatcher_find_covering_pending_node_offset_locked(arena_ptr,
                                                                                                    chip,
                                                                                                    address,
                                                                                                    size,
                                                                                                    encrypted);
    if (covering_node_offset != FLASH_DISPATCHER_ARENA_NULL_OFFSET)
    {
        flash_pending_write_node_t *covering_node = flash_dispatcher_pending_node_at(arena_ptr,
                                                                                     covering_node_offset);
        memcpy(flash_dispatcher_pending_node_data_ptr(arena_ptr, covering_node_offset) +
                   (address - covering_node->address),
               buffer,
               size);
        goto exit;
    }

    const size_t alloc_size = flash_dispatcher_pending_allocation_size(size);
    uint32_t node_offset = FLASH_DISPATCHER_ARENA_NULL_OFFSET;
    err = flash_dispatcher_arena_alloc_locked(arena_ptr, alloc_size, &node_offset);
    if (err != ESP_OK)
    {
        arena_free_bytes = flash_dispatcher_arena_free_bytes_locked(arena_ptr);
        goto exit;
    }

    flash_pending_write_node_t *node = flash_dispatcher_pending_node_at(arena_ptr, node_offset);
    memset(node, 0, sizeof(*node));
    node->chip = chip;
    node->address = address;
    node->size = size;
    node->encrypted = encrypted;

    memcpy(flash_dispatcher_pending_node_data_ptr(arena_ptr, node_offset), buffer, size);
    flash_dispatcher_append_pending_node_locked(arena_ptr, node_offset);

    s_flash_dispatcher_ctx.pending_write_count++;
    s_flash_dispatcher_ctx.pending_write_bytes += size;

exit:
    if (arena_ptr != NULL)
    {
        esp_err_t unmap_err = flash_dispatcher_unmap_pending_buffer_locked(arena_ptr);
        if (err == ESP_OK && unmap_err != ESP_OK)
        {
            err = unmap_err;
        }
    }

    xSemaphoreGive(s_flash_dispatcher_ctx.pending_lock);
    if (err == ESP_ERR_NO_MEM)
    {
        ESP_EARLY_LOGE(TAG,
                       "pending write arena exhausted (%u writes, %u pending bytes, %u free bytes)",
                       (unsigned)s_flash_dispatcher_ctx.pending_write_count,
                       (unsigned)s_flash_dispatcher_ctx.pending_write_bytes,
                       (unsigned)arena_free_bytes);
    }
    return err;
}

static esp_err_t flash_dispatcher_wait_for_request_locked(const flash_operation_request_t *request,
                                                          const char *op_name,
                                                          flash_operation_result_t *out_result)
{
    if (xQueueSend(s_flash_dispatcher_ctx.queue, request, portMAX_DELAY) != pdTRUE)
    {
        ESP_EARLY_LOGE(TAG, "Failed to send %s request to queue", op_name ? op_name : "flash");
        return ESP_ERR_TIMEOUT;
    }

    if (xQueueReceive(s_flash_dispatcher_ctx.result_queue, out_result, portMAX_DELAY) != pdTRUE)
    {
        ESP_EARLY_LOGE(TAG, "Failed to receive %s result from queue", op_name ? op_name : "flash");
        return ESP_ERR_TIMEOUT;
    }

    return out_result->err;
}

static esp_err_t flash_dispatcher_shutdown_callback(void *user_ctx)
{
    (void)user_ctx;

    if (!s_flash_dispatcher_ctx.dispatcher_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_flash_dispatcher_flush_writes(NULL);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "failed to flush queued writes during shutdown: %s", esp_err_to_name(err));
    }

    return err;
}

static void flash_dispatcher_task(void *arg)
{
    (void)arg;

    flash_operation_request_t request;
    while (true)
    {
        if (xQueueReceive(s_flash_dispatcher_ctx.queue, &request, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        flash_operation_result_t result = {
            .err = ESP_FAIL,
            .duration_us = 0,
        };

        switch (request.op)
        {
        case FLASH_OP_READ:
            result.err = __real_esp_flash_read(request.chip,
                                               request.args.read.buffer,
                                               request.args.read.address,
                                               request.args.read.size);
            break;

        case FLASH_OP_ERASE_REGION:
            xSemaphoreTake(s_flash_dispatcher_ctx.pending_lock, portMAX_DELAY);
            result.err = flash_dispatcher_flush_pending_locked(NULL);
            xSemaphoreGive(s_flash_dispatcher_ctx.pending_lock);
            if (result.err == ESP_OK)
            {
                result.err = __real_esp_flash_erase_region(request.chip,
                                                           request.args.erase_region.start_address,
                                                           request.args.erase_region.size);
            }
            break;

        case FLASH_OP_ERASE_CHIP:
            xSemaphoreTake(s_flash_dispatcher_ctx.pending_lock, portMAX_DELAY);
            result.err = flash_dispatcher_flush_pending_locked(NULL);
            xSemaphoreGive(s_flash_dispatcher_ctx.pending_lock);
            if (result.err == ESP_OK)
            {
                result.err = __real_esp_flash_erase_chip(request.chip);
            }
            break;

        case FLASH_OP_FLUSH_PENDING_WRITES:
            xSemaphoreTake(s_flash_dispatcher_ctx.pending_lock, portMAX_DELAY);
            result.err = flash_dispatcher_flush_pending_locked(&result.duration_us);
            xSemaphoreGive(s_flash_dispatcher_ctx.pending_lock);
            break;

        default:
            ESP_EARLY_LOGE(TAG, "Unsupported flash operation type: %d", (int)request.op);
            result.err = ESP_ERR_INVALID_ARG;
            break;
        }

        if (xQueueSend(s_flash_dispatcher_ctx.result_queue, &result, portMAX_DELAY) != pdTRUE)
        {
            ESP_EARLY_LOGE(TAG, "Failed to send result to queue");
        }
    }
}

esp_err_t esp_flash_dispatcher_init(const esp_flash_dispatcher_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(cfg != NULL, ESP_ERR_INVALID_ARG, TAG, "config is null");

    esp_err_t err;

    if (s_flash_dispatcher_ctx.dispatcher_initialized ||
        s_flash_dispatcher_ctx.queue != NULL ||
        s_flash_dispatcher_ctx.task != NULL)
    {
        ESP_EARLY_LOGE(TAG, "flash dispatcher already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    s_flash_dispatcher_ctx.queue = xQueueCreateWithCaps(cfg->queue_size,
                                                        sizeof(flash_operation_request_t),
                                                        FLASH_DISPATCHER_MEM_CAPS);
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.queue, ESP_ERR_NO_MEM, TAG, "create flash operation queue failed");

    s_flash_dispatcher_ctx.result_queue = xQueueCreateWithCaps(1,
                                                               sizeof(flash_operation_result_t),
                                                               FLASH_DISPATCHER_MEM_CAPS);
    if (s_flash_dispatcher_ctx.result_queue == NULL)
    {
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.queue);
        s_flash_dispatcher_ctx.queue = NULL;
        ESP_EARLY_LOGE(TAG, "create flash operation result queue failed");
        return ESP_ERR_NO_MEM;
    }

    s_flash_dispatcher_ctx.op_lock = xSemaphoreCreateMutexWithCaps(FLASH_DISPATCHER_MEM_CAPS);
    if (s_flash_dispatcher_ctx.op_lock == NULL)
    {
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.result_queue);
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.queue);
        s_flash_dispatcher_ctx.result_queue = NULL;
        s_flash_dispatcher_ctx.queue = NULL;
        ESP_EARLY_LOGE(TAG, "create operation lock failed");
        return ESP_ERR_NO_MEM;
    }

    s_flash_dispatcher_ctx.pending_lock = xSemaphoreCreateMutexWithCaps(FLASH_DISPATCHER_MEM_CAPS);
    if (s_flash_dispatcher_ctx.pending_lock == NULL)
    {
        vSemaphoreDeleteWithCaps(s_flash_dispatcher_ctx.op_lock);
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.result_queue);
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.queue);
        s_flash_dispatcher_ctx.op_lock = NULL;
        s_flash_dispatcher_ctx.result_queue = NULL;
        s_flash_dispatcher_ctx.queue = NULL;
        ESP_EARLY_LOGE(TAG, "create pending lock failed");
        return ESP_ERR_NO_MEM;
    }

    err = esp_himem_alloc(FLASH_DISPATCHER_PENDING_BUFFER_SIZE,
                          &s_flash_dispatcher_ctx.write_buffer_handle);
    if (err == ESP_OK)
    {
        uint8_t *arena_ptr = NULL;
        err = flash_dispatcher_map_pending_buffer_locked(0, &arena_ptr);
        if (err == ESP_OK)
        {
            flash_dispatcher_arena_init_locked(arena_ptr);
            err = flash_dispatcher_unmap_pending_buffer_locked(arena_ptr);
        }

        if (err != ESP_OK)
        {
            flash_dispatcher_release_pending_buffer();
            ESP_LOGW(TAG,
                     "himem pending buffer unavailable (%s), falling back to direct PSRAM buffer",
                     esp_err_to_name(err));
        }
    }
    else
    {
        ESP_LOGW(TAG,
                 "himem allocation unavailable (%s), falling back to direct PSRAM buffer",
                 esp_err_to_name(err));
    }

    if (err != ESP_OK)
    {
        err = flash_dispatcher_init_direct_pending_buffer();
        if (err != ESP_OK)
        {
            goto himem_fail;
        }
    }

    BaseType_t rc = xTaskCreatePinnedToCoreWithCaps(flash_dispatcher_task,
                                                    "flash_dispatcher",
                                                    cfg->task_stack_size,
                                                    NULL,
                                                    cfg->task_priority,
                                                    &s_flash_dispatcher_ctx.task,
                                                    cfg->task_core_id,
                                                    FLASH_DISPATCHER_MEM_CAPS);
    if (rc != pdPASS)
    {
        err = ESP_ERR_NO_MEM;
        goto himem_fail;
    }

    s_flash_dispatcher_ctx.dispatcher_initialized = true;

    err = power_mgmt_service_register_shutdown_callback(flash_dispatcher_shutdown_callback,
                                                        NULL,
                                                        POWER_MGMT_SERVICE_SHUTDOWN_PRIORITY_FLASH);
    if (err != ESP_OK)
    {
        goto himem_fail;
    }

    s_flash_dispatcher_ctx.write_buffer_capacity = FLASH_DISPATCHER_PENDING_BUFFER_SIZE;
    s_flash_dispatcher_ctx.last_flush_time_us = 0;
    return ESP_OK;

himem_fail:
    s_flash_dispatcher_ctx.dispatcher_initialized = false;
    if (s_flash_dispatcher_ctx.task != NULL)
    {
        vTaskDelete(s_flash_dispatcher_ctx.task);
        s_flash_dispatcher_ctx.task = NULL;
    }
    flash_dispatcher_release_pending_buffer();
    if (s_flash_dispatcher_ctx.pending_lock != NULL)
    {
        vSemaphoreDeleteWithCaps(s_flash_dispatcher_ctx.pending_lock);
        s_flash_dispatcher_ctx.pending_lock = NULL;
    }
    if (s_flash_dispatcher_ctx.op_lock != NULL)
    {
        vSemaphoreDeleteWithCaps(s_flash_dispatcher_ctx.op_lock);
        s_flash_dispatcher_ctx.op_lock = NULL;
    }
    if (s_flash_dispatcher_ctx.result_queue != NULL)
    {
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.result_queue);
        s_flash_dispatcher_ctx.result_queue = NULL;
    }
    if (s_flash_dispatcher_ctx.queue != NULL)
    {
        vQueueDeleteWithCaps(s_flash_dispatcher_ctx.queue);
        s_flash_dispatcher_ctx.queue = NULL;
    }

    ESP_EARLY_LOGE(TAG, "flash dispatcher initialization failed: %s", esp_err_to_name(err));
    return err;
}

esp_err_t esp_flash_dispatcher_flush_writes(uint64_t *write_time_us)
{
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.dispatcher_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "flash dispatcher is not initialized");

    flash_operation_request_t request = {
        .op = FLASH_OP_FLUSH_PENDING_WRITES,
        .chip = NULL,
    };
    flash_operation_result_t result = {
        .err = ESP_FAIL,
        .duration_us = 0,
    };

    xSemaphoreTake(s_flash_dispatcher_ctx.op_lock, portMAX_DELAY);
    esp_err_t err = flash_dispatcher_wait_for_request_locked(&request, "flash flush", &result);
    xSemaphoreGive(s_flash_dispatcher_ctx.op_lock);

    if (write_time_us != NULL)
    {
        *write_time_us = result.duration_us;
    }
    return err;
}

esp_err_t esp_flash_dispatcher_get_status(esp_flash_dispatcher_status_t *status)
{
    ESP_RETURN_ON_FALSE(status != NULL, ESP_ERR_INVALID_ARG, TAG, "status is null");
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.dispatcher_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "flash dispatcher is not initialized");

    uint8_t *arena_ptr = NULL;
    esp_err_t err;

    xSemaphoreTake(s_flash_dispatcher_ctx.pending_lock, portMAX_DELAY);
    status->pending_write_count = s_flash_dispatcher_ctx.pending_write_count;
    status->pending_write_bytes = s_flash_dispatcher_ctx.pending_write_bytes;
    status->last_flush_time_us = s_flash_dispatcher_ctx.last_flush_time_us;

    err = flash_dispatcher_map_pending_buffer_locked(ESP_HIMEM_MAPFLAG_RO, &arena_ptr);
    if (err == ESP_OK)
    {
        status->arena_free_bytes = flash_dispatcher_arena_free_bytes_locked(arena_ptr);
        esp_err_t unmap_err = flash_dispatcher_unmap_pending_buffer_locked(arena_ptr);
        if (err == ESP_OK && unmap_err != ESP_OK)
        {
            err = unmap_err;
        }
    }
    xSemaphoreGive(s_flash_dispatcher_ctx.pending_lock);
    return err;
}

esp_err_t __wrap_esp_flash_read(esp_flash_t *chip, void *buffer, uint32_t address, uint32_t size)
{
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.dispatcher_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "flash dispatcher is not initialized");

    flash_operation_request_t request = {
        .op = FLASH_OP_READ,
        .chip = chip,
        .args.read = {
            .buffer = buffer,
            .address = address,
            .size = size,
        },
    };
    flash_operation_result_t result = {
        .err = ESP_FAIL,
        .duration_us = 0,
    };

    xSemaphoreTake(s_flash_dispatcher_ctx.op_lock, portMAX_DELAY);
    esp_err_t err = flash_dispatcher_wait_for_request_locked(&request, "flash read", &result);
    if (err == ESP_OK)
    {
        xSemaphoreTake(s_flash_dispatcher_ctx.pending_lock, portMAX_DELAY);
        // Apply queued plain writes so callers can observe deferred writes before a flush.
        // Encrypted writes are not overlaid here because esp_flash_read() returns raw flash bytes.
        err = flash_dispatcher_overlay_pending_reads_locked(chip, buffer, address, size);
        xSemaphoreGive(s_flash_dispatcher_ctx.pending_lock);
    }
    xSemaphoreGive(s_flash_dispatcher_ctx.op_lock);
    return err;
}

esp_err_t __wrap_esp_flash_write(esp_flash_t *chip, const void *buffer, uint32_t address, uint32_t size)
{
    return flash_dispatcher_queue_write(chip, buffer, address, size, false);
}

esp_err_t __wrap_esp_flash_write_encrypted(esp_flash_t *chip, uint32_t address, const void *buffer, uint32_t size)
{
    return flash_dispatcher_queue_write(chip, buffer, address, size, true);
}

esp_err_t __wrap_esp_flash_erase_region(esp_flash_t *chip, uint32_t start_address, uint32_t size)
{
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.dispatcher_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "flash dispatcher is not initialized");

    flash_operation_request_t request = {
        .op = FLASH_OP_ERASE_REGION,
        .chip = chip,
        .args.erase_region = {
            .start_address = start_address,
            .size = size,
        },
    };
    flash_operation_result_t result = {
        .err = ESP_FAIL,
        .duration_us = 0,
    };

    xSemaphoreTake(s_flash_dispatcher_ctx.op_lock, portMAX_DELAY);
    esp_err_t err = flash_dispatcher_wait_for_request_locked(&request, "flash erase_region", &result);
    xSemaphoreGive(s_flash_dispatcher_ctx.op_lock);
    return err;
}

esp_err_t __wrap_esp_flash_erase_chip(esp_flash_t *chip)
{
    ESP_RETURN_ON_FALSE(s_flash_dispatcher_ctx.dispatcher_initialized,
                        ESP_ERR_INVALID_STATE,
                        TAG,
                        "flash dispatcher is not initialized");

    flash_operation_request_t request = {
        .op = FLASH_OP_ERASE_CHIP,
        .chip = chip,
    };
    flash_operation_result_t result = {
        .err = ESP_FAIL,
        .duration_us = 0,
    };

    xSemaphoreTake(s_flash_dispatcher_ctx.op_lock, portMAX_DELAY);
    esp_err_t err = flash_dispatcher_wait_for_request_locked(&request, "flash erase_chip", &result);
    xSemaphoreGive(s_flash_dispatcher_ctx.op_lock);
    return err;
}