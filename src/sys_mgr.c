#include "sys_mgr.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include <esp_log.h>
#include <esp_err.h>
#include <esp_efuse.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG_SYS = "sys_mgr";

// eFuse layout from tools/otatool/burnefuses.py
// BLOCK3 (32 bytes total)
//   Offset 0..3: 0x00 [pcb_rev:8] [bl_rev:8] [flags:8]
//   Offset 4..29: Owner name UTF-8 (max 26 bytes), padded with 0xFF
//   Offset 30..31: padding 0x00

static void parse_and_log(const uint8_t *blk, size_t len)
{
    if (!blk || len < 32)
    {
        ESP_LOGW(TAG_SYS, "Invalid eFuse block data");
        return;
    }

    uint8_t flags = blk[3];
    uint8_t bl_rev = blk[2];
    uint8_t pcb_rev = blk[1];

    bool is_special = (flags & 0x01) != 0;

    ESP_LOGI(TAG_SYS, "EFuse BLOCK3 raw (first 8 bytes): %02X %02X %02X %02X %02X %02X %02X %02X",
             blk[0], blk[1], blk[2], blk[3], blk[4], blk[5], blk[6], blk[7]);
    ESP_LOGI(TAG_SYS, "PCB Revision: %u", (unsigned)pcb_rev);
    ESP_LOGI(TAG_SYS, "Bootloader Revision: %u", (unsigned)bl_rev);
    ESP_LOGI(TAG_SYS, "Flags: %u (0x%02X)", (unsigned)flags, (unsigned)flags);
    ESP_LOGI(TAG_SYS, "Special Edition: %s", is_special ? "Yes" : "No");

    if (is_special)
    {
        // Owner is stored from bytes 4..29, padded by 0xFF. We need to strip trailing 0xFFs.
        const uint8_t *owner_start = &blk[4];
        size_t owner_len = 26;
        while (owner_len > 0 && owner_start[owner_len - 1] == 0xFF)
        {
            owner_len--;
        }

        char owner_buf[27];
        size_t copy_len = owner_len < sizeof(owner_buf) - 1 ? owner_len : sizeof(owner_buf) - 1;
        memcpy(owner_buf, owner_start, copy_len);
        owner_buf[copy_len] = '\0';

        // Note: content is expected to be valid UTF-8 as per burner script
        ESP_LOGI(TAG_SYS, "Owner Name: %s", owner_buf);
    }
}

static void sys_mgr_task(void *arg)
{
    (void)arg;
    // Read BLOCK3 (32 bytes) starting at offset 0
    uint8_t blk[32] = {0};
    esp_err_t err = esp_efuse_read_block(EFUSE_BLK3, blk, 0, sizeof(blk) * 8);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG_SYS, "esp_efuse_read_block failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    parse_and_log(blk, sizeof(blk));

    vTaskDelete(NULL);
}

esp_err_t sys_mgr_init(void)
{
    BaseType_t ok = xTaskCreate(sys_mgr_task, "sys_mgr", 4096, NULL, tskIDLE_PRIORITY + 3, NULL);
    if (ok != pdPASS)
    {
        return ESP_FAIL;
    }
    return ESP_OK;
}
