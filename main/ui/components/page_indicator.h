#ifndef UI_PAGE_INDICATOR_H
#define UI_PAGE_INDICATOR_H

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdbool.h>
#include "lvgl.h"
#include <stdint.h>

#define UI_PAGE_COUNT 4

    /**
     * Create the page-indicator dots and time labels as children of lv_layer_top().
     * Must be called after lv_init() and display setup.
     */
    void ui_page_indicator_create(void);

    /**
     * Highlight the dot at @p active_index (0-based).
     */
    void ui_page_indicator_set_page(int active_index);

    /**
     * Update the elapsed / total time labels shown inline with the page dots.
     */
    void ui_page_indicator_set_time(uint32_t elapsed_ms, uint32_t total_ms);

    /**
     * Animate the time labels into or out of the reserved bottom strip.
     * `visible = false` slides them downward with ease-in.
     * `visible = true` slides them back up with ease-out.
     */
    void ui_page_indicator_animate_time_labels(bool visible);

#ifdef __cplusplus
}
#endif
#endif /* UI_PAGE_INDICATOR_H */
