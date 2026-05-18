/**
 * @file page_indicator.c
 * Always-on page-dot indicator rendered in lv_layer_top().
 *
 * Layout: 4 dots centred horizontally near the bottom of the 128×64 screen.
 * Each dot is a 3×3 square: active = white filled, inactive = hollow (1-px border).
 * Time labels sit above the dots: elapsed on the left, total on the right.
 * Reserved bottom strip: y = 50..63.
 */

#include "page_indicator.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdbool.h>

LV_FONT_DECLARE(lv_font_tiny5_5x3);

/* ── layout constants ──────────────────────────────────────────────── */
#define SCREEN_W 128
#define SCREEN_H 64
#define BOTTOM_STRIP_Y 52
#define TIME_Y BOTTOM_STRIP_Y
#define TIME_HIDDEN_Y SCREEN_H
#define DOT_Y (BOTTOM_STRIP_Y + 3)
#define TIME_ANIM_MS 180
#define TIME_SETTLE_MS 110
#define TIME_ENTER_STAGGER_MS 100
#define TIME_OVERSHOOT_PX 2

#define DOT_SIZE 3 /* dot side length (pixels) */
#define DOT_GAP 4  /* centre-to-centre spacing */

/* ── private state ────────────────────────────────────────────── */
static struct
{
    lv_obj_t *dots[UI_PAGE_COUNT];
    lv_obj_t *lbl_elapsed;
    lv_obj_t *lbl_total;
} s;

static void animate_label_y(lv_obj_t *obj, int32_t target_y, lv_anim_path_cb_t path_cb);
static void animate_label_y_with_completion(lv_obj_t *obj,
                                            int32_t target_y,
                                            lv_anim_path_cb_t path_cb,
                                            lv_anim_completed_cb_t completed_cb,
                                            uint32_t duration,
                                            uint32_t delay);

static int32_t aggressive_ease_in_path(const lv_anim_t *a)
{
    double t = (a->duration > 0) ? (double)a->act_time / (double)a->duration : 1.0;
    double eased;
    double delta;

    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;

    eased = t * t * t * t;
    delta = (double)(a->end_value - a->start_value) * eased;

    return a->start_value + (int32_t)(delta >= 0.0 ? delta + 0.5 : delta - 0.5);
}

static int32_t aggressive_ease_out_path(const lv_anim_t *a)
{
    double t = (a->duration > 0) ? (double)a->act_time / (double)a->duration : 1.0;
    double inv;
    double eased;
    double delta;

    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;

    inv = 1.0 - t;
    eased = 1.0 - inv * inv * inv * inv;
    delta = (double)(a->end_value - a->start_value) * eased;

    return a->start_value + (int32_t)(delta >= 0.0 ? delta + 0.5 : delta - 0.5);
}

static void time_label_entry_settle_cb(lv_anim_t *a)
{
    animate_label_y_with_completion(a->var,
                                    TIME_Y,
                                    lv_anim_path_ease_in_out,
                                    NULL,
                                    TIME_SETTLE_MS,
                                    0);
}

static void time_label_exit_settle_cb(lv_anim_t *a)
{
    animate_label_y_with_completion(a->var,
                                    TIME_HIDDEN_Y,
                                    lv_anim_path_ease_in_out,
                                    NULL,
                                    TIME_SETTLE_MS,
                                    0);
}

static void animate_label_y(lv_obj_t *obj, int32_t target_y, lv_anim_path_cb_t path_cb)
{
    lv_anim_t anim;

    if (!obj)
        return;

    lv_anim_delete(obj, (lv_anim_exec_xcb_t)lv_obj_set_y);
    if (lv_obj_get_y(obj) == target_y)
        return;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&anim, lv_obj_get_y(obj), target_y);
    lv_anim_set_duration(&anim, TIME_ANIM_MS);
    lv_anim_set_path_cb(&anim, path_cb);
    lv_anim_start(&anim);
}

static void animate_label_y_with_completion(lv_obj_t *obj,
                                            int32_t target_y,
                                            lv_anim_path_cb_t path_cb,
                                            lv_anim_completed_cb_t completed_cb,
                                            uint32_t duration,
                                            uint32_t delay)
{
    lv_anim_t anim;

    if (!obj)
        return;

    lv_anim_delete(obj, (lv_anim_exec_xcb_t)lv_obj_set_y);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&anim, lv_obj_get_y(obj), target_y);
    lv_anim_set_delay(&anim, delay);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, path_cb);
    if (completed_cb)
        lv_anim_set_completed_cb(&anim, completed_cb);
    lv_anim_start(&anim);
}

/* ── public API ───────────────────────────────────────────────── */
void ui_page_indicator_create(void)
{
    lv_obj_t *top = lv_layer_top();

    /* ── dots: centred horizontally ─────────────────────────────── */
    int32_t total_w = UI_PAGE_COUNT * DOT_SIZE + (UI_PAGE_COUNT - 1) * (DOT_GAP - DOT_SIZE);
    int32_t start_x = (SCREEN_W - total_w) / 2;

    for (int i = 0; i < UI_PAGE_COUNT; i++)
    {
        lv_obj_t *dot = lv_obj_create(top);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, DOT_SIZE, DOT_SIZE);
        lv_obj_set_pos(dot, start_x + i * DOT_GAP, DOT_Y);
        lv_obj_set_style_radius(dot, 0, 0); /* square dot */
        /* inactive by default: hollow (border only) */
        lv_obj_set_style_bg_opa(dot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(dot, lv_color_white(), 0);
        lv_obj_set_style_border_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 1, 0);
        s.dots[i] = dot;
    }

    /* ── time labels ─────────────────────────────────────────────── */
    /* elapsed – left side of bar */
    s.lbl_elapsed = lv_label_create(top);
    lv_obj_remove_style_all(s.lbl_elapsed);
    lv_label_set_text(s.lbl_elapsed, "00:00");
    lv_obj_set_style_text_font(s.lbl_elapsed, &lv_font_tiny5_5x3, 0);
    lv_obj_set_style_text_color(s.lbl_elapsed, lv_color_white(), 0);
    lv_obj_set_width(s.lbl_elapsed, LV_SIZE_CONTENT);
    lv_obj_set_pos(s.lbl_elapsed, 4, TIME_Y);

    /* total – right side of bar */
    s.lbl_total = lv_label_create(top);
    lv_obj_remove_style_all(s.lbl_total);
    lv_label_set_text(s.lbl_total, "00:00");
    lv_obj_set_style_text_font(s.lbl_total, &lv_font_tiny5_5x3, 0);
    lv_obj_set_style_text_color(s.lbl_total, lv_color_white(), 0);
    lv_obj_set_width(s.lbl_total, LV_SIZE_CONTENT);
    lv_obj_align(s.lbl_total, LV_ALIGN_TOP_RIGHT, -4, TIME_Y);

    /* highlight page 0 by default */
    ui_page_indicator_set_page(0);
}

void ui_page_indicator_set_page(int active_index)
{
    for (int i = 0; i < UI_PAGE_COUNT; i++)
    {
        bool is_active = (i == active_index);
        lv_obj_set_style_bg_opa(s.dots[i],
                                is_active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(s.dots[i], lv_color_white(), 0);
        lv_obj_set_style_border_opa(s.dots[i],
                                    is_active ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    }
}

void ui_page_indicator_set_time(uint32_t elapsed_ms, uint32_t total_ms)
{
    char buf[8];
    uint32_t e = elapsed_ms / 1000u;
    lv_snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(e / 60u), (unsigned)(e % 60u));
    lv_label_set_text(s.lbl_elapsed, buf);

    uint32_t t = total_ms / 1000u;
    lv_snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(t / 60u), (unsigned)(t % 60u));
    lv_label_set_text(s.lbl_total, buf);
}

void ui_page_indicator_animate_time_labels(bool visible)
{
    if (visible)
    {
        animate_label_y_with_completion(s.lbl_elapsed,
                                        TIME_Y - TIME_OVERSHOOT_PX,
                                        aggressive_ease_out_path,
                                        time_label_entry_settle_cb,
                                        TIME_ANIM_MS,
                                        0);
        animate_label_y_with_completion(s.lbl_total,
                                        TIME_Y - TIME_OVERSHOOT_PX,
                                        aggressive_ease_out_path,
                                        time_label_entry_settle_cb,
                                        TIME_ANIM_MS,
                                        TIME_ENTER_STAGGER_MS);
    }
    else
    {
        animate_label_y_with_completion(s.lbl_elapsed,
                                        TIME_HIDDEN_Y + TIME_OVERSHOOT_PX,
                                        aggressive_ease_in_path,
                                        time_label_exit_settle_cb,
                                        TIME_ANIM_MS,
                                        0);
        animate_label_y_with_completion(s.lbl_total,
                                        TIME_HIDDEN_Y + TIME_OVERSHOOT_PX,
                                        aggressive_ease_in_path,
                                        time_label_exit_settle_cb,
                                        TIME_ANIM_MS,
                                        0);
    }
}
