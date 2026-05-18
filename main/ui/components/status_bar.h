#ifndef UI_STATUS_BAR_H
#define UI_STATUS_BAR_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"

    /** Data supplied to ui_status_bar_update() */
    typedef struct
    {
        bool battery_full; /**< true = battery-full icon, false = battery-empty */
        bool charging;     /**< show bolt (charging) icon */
        bool signal;       /**< show signal icon */
        bool bluetooth;    /**< show bluetooth icon */
        bool downloading;  /**< show arrow-down icon */
        const char *title; /**< right-side label, e.g. "Now Playing" */
    } ui_status_bar_data_t;

    /**
     * Create the status-bar as children of lv_layer_top().
     * Must be called after lv_init() and display setup.
     */
    void ui_status_bar_create(void);

    /** Refresh the status bar with new data. */
    void ui_status_bar_update(const ui_status_bar_data_t *data);

    /** Set the status-bar title immediately without animation. */
    void ui_status_bar_set_title(const char *title);

    /**
     * Animate the status-bar title horizontally to a new title.
     * `direction > 0` = swipe left, `direction < 0` = swipe right.
     */
    void ui_status_bar_animate_title(const char *title,
                                     int direction,
                                     uint32_t duration,
                                     lv_anim_path_cb_t path_cb);

#ifdef __cplusplus
}
#endif
#endif /* UI_STATUS_BAR_H */
