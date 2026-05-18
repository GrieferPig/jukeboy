/**
 * @file ui.c
 * Top-level UI manager.
 *
 * Creates:
 *   – 4 page screens (screen 0 = Now Playing, screens 1-3 = stubs)
 *   – Always-on status bar  (via lv_layer_top)
 *   – Always-on page indicator (via lv_layer_top)
 *
 * Page switching hides the old screen, shows the new one, updates the
 * page-indicator dot, and notifies the Now-Playing screen to pause/resume
 * its marquee timer.
 */

#include "ui.h"
#include "components/status_bar.h"
#include "components/page_indicator.h"
#include "screens/screen_now_playing.h"
#include "screens/screen_stub.h"
#include "lvgl.h"

#define PAGE_TRANSITION_MS 250
#define PAGE_REQUEST_QUEUE_LEN 32

static const char *const STUB_NAMES[UI_PAGE_COUNT] = {
    "Now Playing", /* slot 0 – replaced by the real screen */
    "Playlist",
    "Equaliser",
    "Settings",
};

static struct
{
    lv_obj_t *screens[UI_PAGE_COUNT];
    lv_timer_t *transition_timer;
    int queued_pages[PAGE_REQUEST_QUEUE_LEN];
    uint8_t queue_head;
    uint8_t queue_len;
    int current_page;
    int transition_to_page;
    bool transition_in_progress;
} s;

/* Quintic ease-out: fast start, decelerates smoothly to rest.
 * eased = 1 - (1-t)^5  — very steep initial motion, gentle landing. */
static int32_t ease_out_path(const lv_anim_t *a)
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
    eased = 1.0 - inv * inv * inv * inv * inv;

    delta = (double)(a->end_value - a->start_value) * eased;
    return a->start_value + (int32_t)(delta >= 0.0 ? delta + 0.5 : delta - 0.5);
}

static bool queue_page_request(int index)
{
    uint8_t pos;

    if (s.queue_len >= PAGE_REQUEST_QUEUE_LEN)
        return false;

    pos = (uint8_t)((s.queue_head + s.queue_len) % PAGE_REQUEST_QUEUE_LEN);
    s.queued_pages[pos] = index;
    s.queue_len++;
    return true;
}

static bool dequeue_page_request(int *index)
{
    if (s.queue_len == 0)
        return false;

    *index = s.queued_pages[s.queue_head];
    s.queue_head = (uint8_t)((s.queue_head + 1) % PAGE_REQUEST_QUEUE_LEN);
    s.queue_len--;
    return true;
}

static int current_navigation_base_page(void)
{
    if (s.queue_len > 0)
    {
        uint8_t tail = (uint8_t)((s.queue_head + s.queue_len - 1) % PAGE_REQUEST_QUEUE_LEN);
        return s.queued_pages[tail];
    }

    if (s.transition_in_progress)
        return s.transition_to_page;

    return (s.current_page >= 0) ? s.current_page : 0;
}

static void arm_transition_timer(void);

static int transition_direction_for_pages(int from, int to)
{
    int forward = (to - from + UI_PAGE_COUNT) % UI_PAGE_COUNT;
    int backward = (from - to + UI_PAGE_COUNT) % UI_PAGE_COUNT;

    return (forward <= backward) ? 1 : -1;
}

static void load_page_with_transition(lv_obj_t *old_screen, lv_obj_t *new_screen, int direction)
{
    lv_scr_load_anim_t anim_type = (direction >= 0)
                                       ? LV_SCR_LOAD_ANIM_MOVE_LEFT
                                       : LV_SCR_LOAD_ANIM_MOVE_RIGHT;

    lv_scr_load_anim(new_screen, anim_type, PAGE_TRANSITION_MS, 0, false);

    /* lv_scr_load_anim uses linear easing by default.  Patch the two running
     * animations to use our ease-out path immediately after they are started. */
    lv_anim_t *a_new = lv_anim_get(new_screen, NULL);
    if (a_new)
        lv_anim_set_path_cb(a_new, ease_out_path);
    lv_anim_t *a_old = lv_anim_get(old_screen, NULL);
    if (a_old)
        lv_anim_set_path_cb(a_old, ease_out_path);
}

static void start_page_change(int index)
{
    int old_index = s.current_page;
    int direction;

    if (old_index == index)
        return;

    if (old_index == 0)
        screen_now_playing_on_deactivate();

    if (old_index == 0 && index != 0)
        ui_page_indicator_animate_time_labels(false);

    if (old_index != 0 && index == 0)
        ui_page_indicator_animate_time_labels(true);

    ui_page_indicator_set_page(index);

    if (old_index < 0)
    {
        lv_scr_load(s.screens[index]);
        s.current_page = index;
        s.transition_to_page = index;
        ui_status_bar_set_title(STUB_NAMES[index]);
        if (index == 0)
            screen_now_playing_on_activate();
        return;
    }

    direction = transition_direction_for_pages(old_index, index);
    s.transition_in_progress = true;
    s.transition_to_page = index;

    ui_status_bar_animate_title(STUB_NAMES[index],
                                direction,
                                PAGE_TRANSITION_MS,
                                ease_out_path);
    load_page_with_transition(s.screens[old_index],
                              s.screens[index],
                              direction);
    arm_transition_timer();
}

static void process_queued_page_changes(void)
{
    int next_index;

    while (dequeue_page_request(&next_index))
    {
        if (next_index == s.current_page)
            continue;

        start_page_change(next_index);
        return;
    }
}

static void transition_timer_cb(lv_timer_t *timer)
{
    (void)timer;

    if (s.transition_timer)
    {
        lv_timer_delete(s.transition_timer);
        s.transition_timer = NULL;
    }

    if (!s.transition_in_progress)
        return;

    s.transition_in_progress = false;
    s.current_page = s.transition_to_page;

    if (s.current_page == 0)
        screen_now_playing_on_activate();
    process_queued_page_changes();
}

static void arm_transition_timer(void)
{
    if (s.transition_timer)
    {
        lv_timer_delete(s.transition_timer);
        s.transition_timer = NULL;
    }

    s.transition_timer = lv_timer_create(transition_timer_cb, PAGE_TRANSITION_MS, NULL);
    if (s.transition_timer)
        lv_timer_set_repeat_count(s.transition_timer, 1);
}

/* ── public API ───────────────────────────────────────────────── */

void ui_init(void)
{
    /* Page 0: full Now-Playing screen */
    s.screens[0] = screen_now_playing_create();

    /* Pages 1-3: stub screens */
    for (int i = 1; i < UI_PAGE_COUNT; i++)
        s.screens[i] = screen_stub_create(STUB_NAMES[i]);

    /* Overlays – always on top */
    ui_status_bar_create();
    ui_page_indicator_create();

    /* Set default status-bar content */
    ui_status_bar_data_t sb = {
        .battery_full = true,
        .charging = true,
        .signal = true,
        .bluetooth = true,
        .downloading = true,
        .title = NULL,
    };
    ui_status_bar_update(&sb);
    ui_status_bar_set_title(STUB_NAMES[0]);

    /* Load the first page */
    s.current_page = -1; /* force transition */
    s.transition_timer = NULL;
    s.transition_to_page = -1;
    s.transition_in_progress = false;
    ui_set_page(0);
}

void ui_set_page(int index)
{
    if (index < 0 || index >= UI_PAGE_COUNT)
        return;

    if (s.transition_in_progress)
    {
        queue_page_request(index);
        return;
    }

    if (index == s.current_page)
        return;

    start_page_change(index);
}

void ui_cycle_page(void)
{
    ui_set_page((current_navigation_base_page() + 1) % UI_PAGE_COUNT);
}

int ui_get_current_page(void)
{
    return s.current_page;
}

void ui_update_status_bar(const ui_status_bar_data_t *data)
{
    ui_status_bar_update(data);
}

void ui_now_playing_update(const ui_now_playing_data_t *data)
{
    screen_now_playing_update(data);
    ui_page_indicator_set_time(data->elapsed_ms, data->total_ms);
}
