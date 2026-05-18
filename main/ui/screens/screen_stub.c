/**
 * @file screen_stub.c
 * Placeholder screen – black background with a centred white label.
 */

#include "screen_stub.h"
#include "lvgl.h"

LV_FONT_DECLARE(lv_font_silkscreen_8);

lv_obj_t *screen_stub_create(const char *name)
{
    lv_obj_t *screen = lv_obj_create(NULL);

    lv_obj_remove_style_all(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_size(screen, 128, 64);

    lv_obj_t *lbl = lv_label_create(screen);
    lv_obj_remove_style_all(lbl);
    lv_label_set_text(lbl, name ? name : "");
    lv_obj_set_style_text_font(lbl, &lv_font_silkscreen_8, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    /* Center in the content area between the status and footer overlays. */
    lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);

    return screen;
}
