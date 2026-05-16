#include "display_service.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "display_framebuffer.h"

#define DISPLAY_SVC_TASK_STACK_SIZE 6144
#define DISPLAY_SVC_TASK_PRIORITY 3
#define DISPLAY_SVC_DRAW_BUFFER_LINES 16
#define DISPLAY_SVC_LVGL_TICK_PERIOD_MS 2
#define DISPLAY_SVC_TASK_MIN_DELAY_MS 5
#define DISPLAY_SVC_TASK_MAX_DELAY_MS 20
#define DISPLAY_SVC_BOX_SIZE 18
#define DISPLAY_SVC_ANIM_TIME_MS 900

static const char *TAG = "display_svc";

static TaskHandle_t s_task_handle;
static esp_timer_handle_t s_lvgl_tick_timer;
static lv_disp_draw_buf_t s_lvgl_draw_buf;
static lv_color_t s_lvgl_draw_buffer[DISPLAY_FRAMEBUFFER_WIDTH * DISPLAY_SVC_DRAW_BUFFER_LINES];
static lv_disp_drv_t s_lvgl_disp_drv;
static lv_disp_t *s_lvgl_disp;
static bool s_initialized;

static TickType_t display_service_delay_ticks(uint32_t delay_ms)
{
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    return delay_ticks == 0 ? 1 : delay_ticks;
}

static bool display_service_color_is_on(lv_color_t color)
{
    return lv_color_brightness(color) >= 64;
}

static void display_service_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(DISPLAY_SVC_LVGL_TICK_PERIOD_MS);
}

static void display_service_flush_cb(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_map)
{
    lv_area_t clipped = *area;
    uint8_t *framebuffer = NULL;
    esp_err_t err;
    int32_t source_width;

    (void)disp_drv;

    if (clipped.x2 < 0 || clipped.y2 < 0 ||
        clipped.x1 >= DISPLAY_FRAMEBUFFER_WIDTH || clipped.y1 >= DISPLAY_FRAMEBUFFER_HEIGHT)
    {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    if (clipped.x1 < 0)
    {
        clipped.x1 = 0;
    }
    if (clipped.y1 < 0)
    {
        clipped.y1 = 0;
    }
    if (clipped.x2 >= DISPLAY_FRAMEBUFFER_WIDTH)
    {
        clipped.x2 = DISPLAY_FRAMEBUFFER_WIDTH - 1;
    }
    if (clipped.y2 >= DISPLAY_FRAMEBUFFER_HEIGHT)
    {
        clipped.y2 = DISPLAY_FRAMEBUFFER_HEIGHT - 1;
    }

    source_width = lv_area_get_width(area);
    err = display_framebuffer_lock(&framebuffer);
    if (err == ESP_OK)
    {
        for (int32_t y = clipped.y1; y <= clipped.y2; y++)
        {
            uint8_t page = (uint8_t)(y >> 3);
            uint8_t bit_mask = (uint8_t)(1U << (y & 0x07U));
            size_t dst_row_offset = (size_t)page * DISPLAY_FRAMEBUFFER_WIDTH;
            size_t src_row_offset = (size_t)(y - area->y1) * source_width;

            for (int32_t x = clipped.x1; x <= clipped.x2; x++)
            {
                size_t src_index = src_row_offset + (size_t)(x - area->x1);
                uint8_t *dst = &framebuffer[dst_row_offset + x];

                if (display_service_color_is_on(color_map[src_index]))
                {
                    *dst |= bit_mask;
                }
                else
                {
                    *dst &= (uint8_t)~bit_mask;
                }
            }
        }

        display_framebuffer_mark_dirty_area_locked((uint16_t)clipped.x1,
                                                   (uint16_t)clipped.y1,
                                                   (uint16_t)clipped.x2,
                                                   (uint16_t)clipped.y2);
        err = display_framebuffer_flush_dirty_locked();
        display_framebuffer_unlock();
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "LVGL flush failed: %s", esp_err_to_name(err));
    }

    lv_disp_flush_ready(disp_drv);
}

static void display_service_anim_x_cb(void *var, int32_t value)
{
    lv_obj_set_x((lv_obj_t *)var, value);
}

static void display_service_create_demo_ui(void)
{
    lv_obj_t *screen = lv_scr_act();
    lv_obj_t *box = lv_obj_create(screen);
    lv_anim_t anim;

    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(screen, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(screen, 0, LV_PART_MAIN);

    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, DISPLAY_SVC_BOX_SIZE, DISPLAY_SVC_BOX_SIZE);
    lv_obj_set_style_bg_color(box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 2, LV_PART_MAIN);
    lv_obj_set_pos(box, 0, (DISPLAY_FRAMEBUFFER_HEIGHT - DISPLAY_SVC_BOX_SIZE) / 2);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, box);
    lv_anim_set_exec_cb(&anim, display_service_anim_x_cb);
    lv_anim_set_values(&anim, 0, DISPLAY_FRAMEBUFFER_WIDTH - DISPLAY_SVC_BOX_SIZE - 1);
    lv_anim_set_time(&anim, DISPLAY_SVC_ANIM_TIME_MS);
    lv_anim_set_playback_time(&anim, DISPLAY_SVC_ANIM_TIME_MS);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&anim);
}

static void display_service_task(void *param)
{
    (void)param;

    for (;;)
    {
        uint32_t delay_ms = lv_timer_handler();

        if (delay_ms < DISPLAY_SVC_TASK_MIN_DELAY_MS)
        {
            delay_ms = DISPLAY_SVC_TASK_MIN_DELAY_MS;
        }
        else if (delay_ms > DISPLAY_SVC_TASK_MAX_DELAY_MS)
        {
            delay_ms = DISPLAY_SVC_TASK_MAX_DELAY_MS;
        }

        vTaskDelay(display_service_delay_ticks(delay_ms));
    }
}

static void display_service_deinit(void)
{
    if (s_lvgl_tick_timer != NULL)
    {
        (void)esp_timer_stop(s_lvgl_tick_timer);
        (void)esp_timer_delete(s_lvgl_tick_timer);
        s_lvgl_tick_timer = NULL;
    }

    s_task_handle = NULL;
    s_lvgl_disp = NULL;
    s_initialized = false;
}

esp_err_t display_service_init(void)
{
    const esp_timer_create_args_t tick_timer_args = {
        .callback = display_service_lvgl_tick_cb,
        .name = "display_lvgl_tick",
    };
    esp_err_t err;

    if (s_initialized)
    {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_framebuffer_init(), TAG, "display framebuffer init failed");

    lv_init();
    lv_disp_draw_buf_init(&s_lvgl_draw_buf,
                          s_lvgl_draw_buffer,
                          NULL,
                          DISPLAY_FRAMEBUFFER_WIDTH * DISPLAY_SVC_DRAW_BUFFER_LINES);

    lv_disp_drv_init(&s_lvgl_disp_drv);
    s_lvgl_disp_drv.hor_res = DISPLAY_FRAMEBUFFER_WIDTH;
    s_lvgl_disp_drv.ver_res = DISPLAY_FRAMEBUFFER_HEIGHT;
    s_lvgl_disp_drv.flush_cb = display_service_flush_cb;
    s_lvgl_disp_drv.draw_buf = &s_lvgl_draw_buf;
    s_lvgl_disp_drv.full_refresh = 0;
    s_lvgl_disp = lv_disp_drv_register(&s_lvgl_disp_drv);
    ESP_RETURN_ON_FALSE(s_lvgl_disp != NULL, ESP_FAIL, TAG, "LVGL display registration failed");

    display_service_create_demo_ui();

    err = esp_timer_create(&tick_timer_args, &s_lvgl_tick_timer);
    if (err != ESP_OK)
    {
        display_service_deinit();
        return err;
    }

    err = esp_timer_start_periodic(s_lvgl_tick_timer, DISPLAY_SVC_LVGL_TICK_PERIOD_MS * 1000ULL);
    if (err != ESP_OK)
    {
        display_service_deinit();
        return err;
    }

    if (xTaskCreatePinnedToCore(display_service_task,
                                "display_svc",
                                DISPLAY_SVC_TASK_STACK_SIZE,
                                NULL,
                                DISPLAY_SVC_TASK_PRIORITY,
                                &s_task_handle,
                                0) != pdPASS)
    {
        display_service_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "LVGL animation running on OLED framebuffer");
    return ESP_OK;
}