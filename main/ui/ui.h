#ifndef UI_H
#define UI_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
#include "components/status_bar.h"
#include "screens/screen_now_playing.h"

/** Total number of pages / screens. */
#define UI_PAGE_COUNT 4

    /**
     * Initialise the entire UI.
     * Call once after lv_init() and sdl_hal_init().
     * Loads the Now-Playing screen and starts the marquee timer.
     */
    void ui_init(void);

    /**
     * Switch to screen @p index (0-based).
     * 0 = Now Playing, 1-3 = stub screens.
     */
    void ui_set_page(int index);

    /** Advance to the next page, wrapping back to page 0. */
    void ui_cycle_page(void);

    /** Return the currently active page index, or -1 before the first load. */
    int ui_get_current_page(void);

    /** Convenience wrapper — update status-bar data. */
    void ui_update_status_bar(const ui_status_bar_data_t *data);

    /** Convenience wrapper — push new track/progress data to the Now-Playing screen. */
    void ui_now_playing_update(const ui_now_playing_data_t *data);

#ifdef __cplusplus
}
#endif
#endif /* UI_H */
