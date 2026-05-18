#ifndef UI_SCREEN_NOW_PLAYING_H
#define UI_SCREEN_NOW_PLAYING_H

#ifdef __cplusplus
extern "C"
{
#endif

#include "lvgl.h"
#include <stdint.h>

    /** Live data for the Now-Playing screen. */
    typedef struct
    {
        const char *album;   /**< Album name  (NULL keeps previous value) */
        const char *track;   /**< Track name  (NULL keeps previous value) */
        const char *artist;  /**< Artist name (NULL keeps previous value) */
        uint32_t elapsed_ms; /**< Elapsed playback time in milliseconds */
        uint32_t total_ms;   /**< Total track duration in milliseconds */
        bool playing;        /**< true = pulse the playback dot, false = keep it stationary */
    } ui_now_playing_data_t;

    /**
     * Create and return the Now-Playing screen object.
     * The caller should NOT load it; ui_init() manages screen loading.
     */
    lv_obj_t *screen_now_playing_create(void);

    /**
     * Refresh the screen with new track / progress data.
     * Safe to call at any time; internally rate-limited by the redraw timer.
     */
    void screen_now_playing_update(const ui_now_playing_data_t *data);

    /**
     * Must be called when this screen becomes active so the marquee timer starts.
     */
    void screen_now_playing_on_activate(void);

    /**
     * Must be called when this screen becomes inactive so the timer pauses.
     */
    void screen_now_playing_on_deactivate(void);

#ifdef __cplusplus
}
#endif
#endif /* UI_SCREEN_NOW_PLAYING_H */
