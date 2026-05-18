#include "display_service.h"

#include <stdbool.h>
#include <stdint.h>

#include "a2dp_coprocessor_service.h"
#include "cartridge_service.h"
#include "companion_api_service.h"
#include "display_framebuffer.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hid_service.h"
#include "lvgl.h"
#include "player_service.h"
#include "ui/ui.h"
#include "wifi_service.h"

#define DISPLAY_SVC_TASK_STACK_SIZE 8192
#define DISPLAY_SVC_TASK_PRIORITY 3
#define DISPLAY_SVC_DRAW_BUFFER_LINES 16
#define DISPLAY_SVC_LVGL_TICK_PERIOD_MS 5
#define DISPLAY_SVC_TASK_MIN_DELAY_MS 10
#define DISPLAY_SVC_TASK_MAX_DELAY_MS 20
#define DISPLAY_SVC_INPUT_POLL_MS 10
#define DISPLAY_SVC_PLAYER_POLL_MS 100
#define DISPLAY_SVC_STATUS_POLL_MS 250
#define DISPLAY_SVC_ACTIVE_REFR_PERIOD_MS 16 /* 62 Hz: screen transitions / input */
#define DISPLAY_SVC_IDLE_REFR_PERIOD_MS 67   /* 15 Hz: steady-state */
#define DISPLAY_SVC_ACTIVE_TIMEOUT_MS 10000  /* ms without input before reverting to idle */

static const char *TAG = "display_svc";

static TaskHandle_t s_task_handle;
static esp_timer_handle_t s_lvgl_tick_timer;
static lv_display_t *s_lvgl_display;
/* 1bpp packed buffer; +8 bytes extra for palette header prepended by LVGL.
 * Must be 4-byte aligned: lv_display_set_buffers asserts buf == lv_draw_buf_align(buf, cf). */
static uint8_t s_lvgl_draw_buffer[((DISPLAY_FRAMEBUFFER_WIDTH + 7) / 8) * DISPLAY_SVC_DRAW_BUFFER_LINES + 8]
    __attribute__((aligned(4)));
static bool s_initialized;
static uint32_t s_previous_button_state;
static uint32_t s_last_input_poll_ms;
static uint32_t s_last_player_poll_ms;
static uint32_t s_last_status_poll_ms;
static uint32_t s_last_activity_ms;
static uint32_t s_current_refr_period;

static TickType_t display_service_delay_ticks(uint32_t delay_ms)
{
    TickType_t delay_ticks = pdMS_TO_TICKS(delay_ms);
    return delay_ticks == 0 ? 1 : delay_ticks;
}

static bool display_service_pixel_is_on(const uint8_t *px_map, int32_t stride_bytes, int32_t x, int32_t y)
{
    /* px_map is a packed 1bpp bitmap; stride_bytes = bytes per row.
     * For LV_COLOR_FORMAT_I1 the palette (8 bytes for 2×lv_color32_t) is prepended. */
    const uint8_t *row = px_map + (size_t)y * stride_bytes;
    return (row[x >> 3] >> (7 - (x & 7))) & 1u;
}

static bool display_service_elapsed(uint32_t now_ms, uint32_t last_ms, uint32_t period_ms)
{
    return (uint32_t)(now_ms - last_ms) >= period_ms;
}

static inline uint32_t display_service_button_mask(hid_button_t button)
{
    return 1UL << (uint32_t)button;
}

static player_service_playback_mode_t display_service_next_playback_mode(player_service_playback_mode_t mode)
{
    switch (mode)
    {
    case PLAYER_SVC_MODE_SEQUENTIAL:
        return PLAYER_SVC_MODE_SINGLE_REPEAT;
    case PLAYER_SVC_MODE_SINGLE_REPEAT:
        return PLAYER_SVC_MODE_SHUFFLE;
    case PLAYER_SVC_MODE_SHUFFLE:
    default:
        return PLAYER_SVC_MODE_SEQUENTIAL;
    }
}

static void display_service_lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(DISPLAY_SVC_LVGL_TICK_PERIOD_MS);
}

static void display_service_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    lv_area_t clipped = *area;
    uint8_t *framebuffer = NULL;
    esp_err_t err;
    int32_t stride_bytes;

    (void)disp;

    if (clipped.x2 < 0 || clipped.y2 < 0 ||
        clipped.x1 >= DISPLAY_FRAMEBUFFER_WIDTH || clipped.y1 >= DISPLAY_FRAMEBUFFER_HEIGHT)
    {
        lv_display_flush_ready(disp);
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

    /* For LV_COLOR_FORMAT_I1 the buffer starts with a 2-entry palette
     * (2 × sizeof(lv_color32_t) = 8 bytes), then the packed 1bpp rows. */
    stride_bytes = (int32_t)(((uint32_t)lv_area_get_width(area) + 7u) / 8u);
    px_map += 2u * sizeof(lv_color32_t); /* skip palette */

    err = display_framebuffer_lock(&framebuffer);
    if (err == ESP_OK)
    {
        for (int32_t y = clipped.y1; y <= clipped.y2; y++)
        {
            uint8_t page = (uint8_t)(y >> 3);
            uint8_t bit_mask = (uint8_t)(1U << (y & 0x07U));
            size_t dst_row_offset = (size_t)page * DISPLAY_FRAMEBUFFER_WIDTH;
            int32_t src_y = y - area->y1;

            for (int32_t x = clipped.x1; x <= clipped.x2; x++)
            {
                int32_t src_x = x - area->x1;
                uint8_t *dst = &framebuffer[dst_row_offset + x];

                if (display_service_pixel_is_on(px_map, stride_bytes, src_x, src_y))
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

    lv_display_flush_ready(disp);
}

static void display_service_apply_transport_control(player_service_control_t control)
{
    esp_err_t err = player_service_request_control(control);

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "player control %d failed: %s", (int)control, esp_err_to_name(err));
    }
}

static void display_service_apply_playback_mode_cycle(void)
{
    player_service_playback_mode_t current_mode = player_service_get_playback_mode();
    esp_err_t err = player_service_set_playback_mode(display_service_next_playback_mode(current_mode));

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "playback mode change failed: %s", esp_err_to_name(err));
    }
}

static void display_service_mark_active(uint32_t now_ms)
{
    s_last_activity_ms = now_ms;
}

/** Ramp the LVGL display refresh-timer period up or down based on recent input activity.
 *  Called every task iteration so transitions happen within one loop cycle.
 *  Both the display refresh timer and the animation core timer are updated together so
 *  that lv_scr_load_anim transitions also run at the higher frame rate. */
static void display_service_update_refr_period(uint32_t now_ms)
{
    lv_timer_t *rt = lv_display_get_refr_timer(s_lvgl_display);
    if (!rt)
        return;

    uint32_t target = ((uint32_t)(now_ms - s_last_activity_ms) < DISPLAY_SVC_ACTIVE_TIMEOUT_MS)
                          ? DISPLAY_SVC_ACTIVE_REFR_PERIOD_MS
                          : DISPLAY_SVC_IDLE_REFR_PERIOD_MS;

    if (s_current_refr_period != target)
    {
        s_current_refr_period = target;
        lv_timer_set_period(rt, target);
        /* Also update the animation core timer so lv_anim_t transitions
         * (including lv_scr_load_anim) step at the same rate. */
        lv_timer_t *at = lv_anim_get_timer();
        if (at)
            lv_timer_set_period(at, target);
    }
}

static void display_service_poll_buttons(uint32_t now_ms)
{
    uint32_t button_state = 0;
    uint32_t pressed_edges;
    esp_err_t err;

    if (!display_service_elapsed(now_ms, s_last_input_poll_ms, DISPLAY_SVC_INPUT_POLL_MS))
    {
        return;
    }
    s_last_input_poll_ms = now_ms;

    err = hid_service_get_button_state(&button_state);
    if (err != ESP_OK)
    {
        return;
    }

    pressed_edges = button_state & ~s_previous_button_state;
    s_previous_button_state = button_state;

    if (pressed_edges == 0)
    {
        return;
    }

    display_service_mark_active(now_ms);

    if ((pressed_edges & display_service_button_mask(HID_BUTTON_SIDE)) != 0)
    {
        ui_cycle_page();
        pressed_edges &= ~display_service_button_mask(HID_BUTTON_SIDE);
    }

    if (ui_get_current_page() != 0 || pressed_edges == 0)
    {
        return;
    }

    if ((pressed_edges & display_service_button_mask(HID_BUTTON_MAIN_1)) != 0)
    {
        display_service_apply_transport_control(PLAYER_SVC_CONTROL_NEXT);
    }
    if ((pressed_edges & display_service_button_mask(HID_BUTTON_MAIN_2)) != 0)
    {
        display_service_apply_transport_control(PLAYER_SVC_CONTROL_PAUSE);
    }
    if ((pressed_edges & display_service_button_mask(HID_BUTTON_MAIN_3)) != 0)
    {
        display_service_apply_transport_control(PLAYER_SVC_CONTROL_PREVIOUS);
    }
    if ((pressed_edges & display_service_button_mask(HID_BUTTON_MISC_1)) != 0)
    {
        display_service_apply_playback_mode_cycle();
    }
    if ((pressed_edges & display_service_button_mask(HID_BUTTON_MISC_2)) != 0)
    {
        display_service_apply_transport_control(PLAYER_SVC_CONTROL_VOLUME_UP);
    }
    if ((pressed_edges & display_service_button_mask(HID_BUTTON_MISC_3)) != 0)
    {
        display_service_apply_transport_control(PLAYER_SVC_CONTROL_VOLUME_DOWN);
    }
}

static void display_service_resolve_empty_ui_strings(cartridge_status_t status,
                                                     const char **album,
                                                     const char **track,
                                                     const char **artist)
{
    switch (status)
    {
    case CARTRIDGE_STATUS_READY:
        *album = "Jukeboy";
        *track = "No Track Selected";
        *artist = "";
        break;
    case CARTRIDGE_STATUS_INVALID:
        *album = "Cartridge Error";
        *track = "Metadata Invalid";
        *artist = "";
        break;
    case CARTRIDGE_STATUS_EMPTY:
    default:
        *album = "No Cartridge";
        *track = "Insert SD Card";
        *artist = "";
        break;
    }
}

static void display_service_poll_player_snapshot(uint32_t now_ms)
{
    player_service_snapshot_t snapshot;
    ui_now_playing_data_t ui_data = {0};
    cartridge_status_t cartridge_status;
    const char *album = NULL;
    const char *track = NULL;
    const char *artist = NULL;
    const char *fallback_album = NULL;
    const char *fallback_track = NULL;
    const char *fallback_artist = NULL;

    if (!display_service_elapsed(now_ms, s_last_player_poll_ms, DISPLAY_SVC_PLAYER_POLL_MS))
    {
        return;
    }
    s_last_player_poll_ms = now_ms;

    cartridge_status = cartridge_service_get_status();
    display_service_resolve_empty_ui_strings(cartridge_status,
                                             &fallback_album,
                                             &fallback_track,
                                             &fallback_artist);
    if (player_service_get_snapshot(&snapshot) == ESP_OK)
    {
        if (snapshot.track_index != UINT32_MAX && snapshot.track_index < snapshot.track_count)
        {
            artist = cartridge_service_get_track_artists(snapshot.track_index);
        }

        album = cartridge_service_get_album_name();
        track = snapshot.track_title[0] != '\0' ? snapshot.track_title : NULL;
        if ((artist == NULL || artist[0] == '\0'))
        {
            artist = cartridge_service_get_album_artist();
        }
        if (album == NULL || album[0] == '\0')
        {
            album = fallback_album;
        }
        if (track == NULL || track[0] == '\0')
        {
            track = fallback_track;
        }
        if (artist == NULL || artist[0] == '\0')
        {
            artist = fallback_artist;
        }

        ui_data.album = album;
        ui_data.track = track;
        ui_data.artist = artist;
        ui_data.elapsed_ms = snapshot.playback_position_sec * 1000U;
        ui_data.total_ms = snapshot.track_duration_sec * 1000U;
        ui_data.playing = snapshot.is_playing && !snapshot.is_paused;
    }
    else
    {
        ui_data.album = fallback_album;
        ui_data.track = fallback_track;
        ui_data.artist = fallback_artist;
        ui_data.elapsed_ms = 0;
        ui_data.total_ms = 0;
        ui_data.playing = false;
    }

    ui_now_playing_update(&ui_data);
}

static void display_service_poll_status_bar(uint32_t now_ms)
{
    companion_api_status_t companion_status;
    wifi_svc_state_t wifi_state;
    ui_status_bar_data_t status_data;

    if (!display_service_elapsed(now_ms, s_last_status_poll_ms, DISPLAY_SVC_STATUS_POLL_MS))
    {
        return;
    }
    s_last_status_poll_ms = now_ms;

    companion_api_service_get_status(&companion_status);
    wifi_state = wifi_service_get_state();

    status_data.battery_full = true;
    status_data.charging = false;
    status_data.signal = wifi_state == WIFI_SVC_STATE_CONNECTED || wifi_service_has_internet();
    status_data.bluetooth = a2dp_coprocessor_service_is_a2dp_connected();
    status_data.downloading = companion_status.spp_connected || companion_status.authenticated;
    status_data.title = NULL;
    ui_update_status_bar(&status_data);
}

static void display_service_task(void *param)
{
    (void)param;
    s_last_activity_ms = lv_tick_get(); /* start in active state */

    for (;;)
    {
        uint32_t now_ms = lv_tick_get();
        uint32_t delay_ms;

        display_service_poll_buttons(now_ms);
        display_service_poll_player_snapshot(now_ms);
        display_service_poll_status_bar(now_ms);
        display_service_update_refr_period(now_ms);

        delay_ms = lv_timer_handler();
        if (delay_ms == LV_NO_TIMER_READY)
        {
            delay_ms = DISPLAY_SVC_TASK_MAX_DELAY_MS;
        }
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
    s_previous_button_state = 0;
    s_last_input_poll_ms = 0;
    s_last_player_poll_ms = 0;
    s_last_status_poll_ms = 0;
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
    s_lvgl_display = lv_display_create(DISPLAY_FRAMEBUFFER_WIDTH, DISPLAY_FRAMEBUFFER_HEIGHT);
    ESP_RETURN_ON_FALSE(s_lvgl_display != NULL,
                        ESP_FAIL,
                        TAG,
                        "LVGL display creation failed");
    lv_display_set_buffers(s_lvgl_display,
                           s_lvgl_draw_buffer,
                           NULL,
                           sizeof(s_lvgl_draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_lvgl_display, display_service_flush_cb);

    ui_init();
    (void)hid_service_get_button_state(&s_previous_button_state);

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
    ESP_LOGI(TAG, "LVGL UI running on OLED framebuffer");
    return ESP_OK;
}
