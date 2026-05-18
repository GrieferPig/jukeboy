/**
 * @file status_bar.c
 * Always-on status bar rendered in lv_layer_top().
 *
 * Layout (128×10 px):
 *   Left:  up to 5 open-iconic 8×8 icons (battery, bolt, signal, bluetooth, arrow-down)
 *   Right: title text in tiny5 font
 */

#include "status_bar.h"
#include "lvgl.h"
#include <string.h>

/* ── open-iconic codepoints (font starts at Unicode 0x40 = 64) ── */
/* battery-full  = codepoint 91  = 0x5B = "[" */
/* bolt          = codepoint 96  = 0x60 = "`" */
/* signal        = codepoint 253 = 0xFD → UTF-8 0xC3 0xBD */
/* bluetooth     = codepoint 94  = 0x5E = "^" */
/* arrow-thick-bottom = codepoint 79 = 0x4F = "O" */
/* battery-empty = codepoint 90  = 0x5A = "Z"  */
#define ICON_BATTERY_FULL "["  /* U+005B */
#define ICON_BATTERY_EMPTY "Z" /* U+005A */
#define ICON_BOLT "`"          /* U+0060 */
#define ICON_SIGNAL "\xC3\xBD" /* U+00FD */
#define ICON_BLUETOOTH "^"     /* U+005E */
#define ICON_ARROW_DOWN "O"    /* U+004F */

LV_FONT_DECLARE(lv_font_open_iconic_1x);
LV_FONT_DECLARE(lv_font_tiny5_5x3);

#define SCREEN_W 128
#define BAR_H 10
#define TITLE_MARGIN_RIGHT 2
#define TITLE_MASK_LEFT (SCREEN_W / 3)
#define TITLE_MASK_RIGHT ((SCREEN_W * 2) / 3) - 3
#define TITLE_MASK_W (TITLE_MASK_RIGHT - TITLE_MASK_LEFT)
#define TITLE_PARK_X SCREEN_W
#define TITLE_Y 0

/* ── private state ────────────────────────────────────────────── */
static struct
{
    lv_obj_t *bar; /* container on lv_layer_top() */
    lv_obj_t *icon_battery;
    lv_obj_t *icon_bolt;
    lv_obj_t *icon_signal;
    lv_obj_t *icon_bluetooth;
    lv_obj_t *icon_arrow;
    lv_obj_t *title_mask;
    lv_obj_t *lbl_title_a;
    lv_obj_t *lbl_title_b;
    lv_obj_t *title_active;
    lv_obj_t *title_inactive;
    lv_obj_t *title_outgoing;
    int32_t title_hide_x;
    bool title_hide_when_le;
    bool title_animating;
} s;

/* ── helpers ──────────────────────────────────────────────────── */
static lv_obj_t *make_icon(lv_obj_t *parent, const char *txt, int32_t x)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_open_iconic_1x, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_pos(lbl, x, 1); /* 1 px top padding */
    return lbl;
}

static void set_icon_visible(lv_obj_t *icon, bool visible)
{
    if (!icon)
        return;

    if (visible)
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_title_label(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, &lv_font_tiny5_5x3, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_width(lbl, LV_SIZE_CONTENT);
    lv_obj_set_pos(lbl, TITLE_PARK_X, TITLE_Y);

    return lbl;
}

static lv_obj_t *make_title_mask(lv_obj_t *parent)
{
    lv_obj_t *mask = lv_obj_create(parent);

    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, TITLE_MASK_W, BAR_H);
    lv_obj_set_pos(mask, TITLE_MASK_LEFT, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0);

    return mask;
}

static int32_t title_final_x(lv_obj_t *label)
{
    lv_obj_update_layout(label);
    return LV_MAX(0, SCREEN_W - TITLE_MARGIN_RIGHT - lv_obj_get_width(label));
}

static int32_t title_incoming_start_x(lv_obj_t *label, int direction)
{
    lv_obj_update_layout(label);

    if (direction >= 0)
        return SCREEN_W;

    return SCREEN_W / 3;
}

static int32_t title_outgoing_end_x(int direction)
{
    return (direction >= 0) ? TITLE_MASK_LEFT : SCREEN_W;
}

static void title_clear_outgoing_hide_rule(void);

static void title_set_outgoing_hide_rule(lv_obj_t *label, int direction)
{
    if (direction >= 0)
    {
        s.title_outgoing = label;
        s.title_hide_x = TITLE_MASK_LEFT;
        s.title_hide_when_le = true;
        return;
    }

    title_clear_outgoing_hide_rule();
}

static void title_clear_outgoing_hide_rule(void)
{
    s.title_outgoing = NULL;
    s.title_hide_x = 0;
    s.title_hide_when_le = true;
}

static void title_x_anim(void *obj, int32_t x)
{
    if (obj == s.title_outgoing)
    {
        bool should_hide = s.title_hide_when_le ? (x <= s.title_hide_x) : (x >= s.title_hide_x);

        if (should_hide)
        {
            lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            return;
        }
    }

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(obj, x);
}

static void place_title_final(lv_obj_t *label)
{
    lv_obj_set_pos(label, title_final_x(label), TITLE_Y);
}

static void title_anim_completed_cb(lv_anim_t *a)
{
    (void)a;

    s.title_animating = false;
    title_clear_outgoing_hide_rule();
    lv_anim_del(s.title_inactive, title_x_anim);
    lv_obj_add_flag(s.title_inactive, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s.title_inactive, TITLE_PARK_X, TITLE_Y);
    place_title_final(s.title_active);
}

static void start_title_anim(lv_obj_t *label,
                             int32_t start_x,
                             int32_t end_x,
                             uint32_t duration,
                             lv_anim_path_cb_t path_cb,
                             lv_anim_ready_cb_t completed_cb)
{
    lv_anim_t anim;

    lv_anim_del(label, title_x_anim);
    lv_obj_set_pos(label, start_x, TITLE_Y);

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, label);
    lv_anim_set_exec_cb(&anim, title_x_anim);
    lv_anim_set_values(&anim, start_x, end_x);
    lv_anim_set_duration(&anim, duration);
    lv_anim_set_path_cb(&anim, path_cb);
    if (completed_cb)
        lv_anim_set_ready_cb(&anim, completed_cb);
    lv_anim_start(&anim);
}

/* ── public API ───────────────────────────────────────────────── */
void ui_status_bar_create(void)
{
    lv_obj_t *top = lv_layer_top();

    /* Full-width bar at y=0, height=10 */
    s.bar = lv_obj_create(top);
    lv_obj_remove_style_all(s.bar);
    lv_obj_set_size(s.bar, SCREEN_W, BAR_H);
    lv_obj_set_pos(s.bar, 0, 0);
    lv_obj_set_style_bg_color(s.bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.bar, LV_OPA_COVER, 0);

    /* Icons – keep the download arrow fully left of the title mask. */
    s.icon_battery = make_icon(s.bar, ICON_BATTERY_FULL, 1);
    s.icon_bolt = make_icon(s.bar, ICON_BOLT, 10);
    s.icon_signal = make_icon(s.bar, ICON_SIGNAL, 19);
    s.icon_bluetooth = make_icon(s.bar, ICON_BLUETOOTH, 28);
    s.icon_arrow = make_icon(s.bar, ICON_ARROW_DOWN, 34);

    s.lbl_title_a = make_title_label(s.bar, "Now Playing");
    s.lbl_title_b = make_title_label(s.bar, "Now Playing");
    s.title_mask = make_title_mask(s.bar);
    s.title_active = s.lbl_title_a;
    s.title_inactive = s.lbl_title_b;
    s.title_outgoing = NULL;
    s.title_hide_x = 0;
    s.title_hide_when_le = true;
    s.title_animating = false;
    place_title_final(s.title_active);
    lv_obj_add_flag(s.title_inactive, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s.title_mask);
}

void ui_status_bar_update(const ui_status_bar_data_t *data)
{
    if (!data)
        return;

    lv_label_set_text(s.icon_battery,
                      data->battery_full ? ICON_BATTERY_FULL : ICON_BATTERY_EMPTY);
    set_icon_visible(s.icon_bolt, data->charging);
    set_icon_visible(s.icon_signal, data->signal);
    set_icon_visible(s.icon_bluetooth, data->bluetooth);
    set_icon_visible(s.icon_arrow, data->downloading);

    if (data->title)
        ui_status_bar_set_title(data->title);
}

void ui_status_bar_set_title(const char *title)
{
    if (!title)
        return;

    lv_anim_del(s.lbl_title_a, title_x_anim);
    lv_anim_del(s.lbl_title_b, title_x_anim);
    title_clear_outgoing_hide_rule();
    s.title_animating = false;

    lv_label_set_text(s.title_active, title);
    lv_obj_clear_flag(s.title_active, LV_OBJ_FLAG_HIDDEN);
    place_title_final(s.title_active);

    lv_obj_add_flag(s.title_inactive, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s.title_inactive, TITLE_PARK_X, TITLE_Y);
}

void ui_status_bar_animate_title(const char *title,
                                 int direction,
                                 uint32_t duration,
                                 lv_anim_path_cb_t path_cb)
{
    lv_obj_t *incoming;
    lv_obj_t *outgoing;
    int32_t outgoing_end_x;

    if (!title || !path_cb)
        return;

    if (!s.title_animating && strcmp(lv_label_get_text(s.title_active), title) == 0)
        return;

    incoming = s.title_inactive;
    outgoing = s.title_active;

    lv_label_set_text(incoming, title);
    lv_obj_clear_flag(incoming, LV_OBJ_FLAG_HIDDEN);

    s.title_active = incoming;
    s.title_inactive = outgoing;
    s.title_animating = true;

    title_set_outgoing_hide_rule(outgoing, direction);
    outgoing_end_x = title_outgoing_end_x(direction);

    start_title_anim(outgoing,
                     lv_obj_get_x(outgoing),
                     outgoing_end_x,
                     duration,
                     path_cb,
                     NULL);
    start_title_anim(incoming,
                     title_incoming_start_x(incoming, direction),
                     title_final_x(incoming),
                     duration,
                     path_cb,
                     title_anim_completed_cb);
}
