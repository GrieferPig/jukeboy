#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define DISPLAY_FRAMEBUFFER_WIDTH 128
#define DISPLAY_FRAMEBUFFER_HEIGHT 64
#define DISPLAY_FRAMEBUFFER_PAGE_COUNT (DISPLAY_FRAMEBUFFER_HEIGHT / 8)
#define DISPLAY_FRAMEBUFFER_SIZE (DISPLAY_FRAMEBUFFER_WIDTH * DISPLAY_FRAMEBUFFER_HEIGHT / 8)

    esp_err_t display_framebuffer_init(void);
    esp_err_t display_framebuffer_lock(uint8_t **framebuffer_out);
    void display_framebuffer_unlock(void);
    void display_framebuffer_mark_dirty_area_locked(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
    esp_err_t display_framebuffer_flush_dirty_locked(void);

#ifdef __cplusplus
}
#endif