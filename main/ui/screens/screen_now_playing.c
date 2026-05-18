/*
 * Renders the Now-Playing screen onto a 128×42 lv_canvas that sits in the
 * content area (y = 10 … 51) of the 128×64 display.
 * The bottom strip (y = 52 … 63) is reserved for time labels and pagination.
 *
 * Visual layout (all y values are canvas-relative, i.e. 0 = top of canvas):
 *
 *   y=1   Album name     Tiny5 5x3     (line_height=9)
 *   y=10  Track name     Chicago 20    (line_height=19)
 *   y=30  Artist name    Chicago 12    (line_height=12)
 *
 * Progress band:
 *   A white parallelogram covers the LEFT portion of the canvas.
 *   Its right edge is a slanted line:
 *     – at y=0  the edge is at  x = p*(W + SLANT)
 *     – at y=H-1 the edge is at  x = p*(W + SLANT) - SLANT
 *   where p = elapsed/total and W = CANVAS_W.
 *
 * Text colour is split at the diagonal:
 *   – LEFT of the edge  → black text  (on white band)
 *   – RIGHT of the edge → white text  (on black background)
 *
 * Marquee:
 *   A timer running at up to ~62 Hz advances each text's scroll offset by
 *   1 px per tick when the rendered text width exceeds the available column
 *   width. The timer is paused entirely when the page is inactive or static.
 *   A gap of 20 px is inserted between the end of the text and its loop-wrap,
 *   and the text holds at offset 0 for 2 seconds after each wrap. Scroll speed
 *   eases in after the hold and eases out before the wrap.
 */

#include "screen_now_playing.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

/* ── display / canvas geometry ───────────────────────────────── */
#define SCREEN_W 128
#define SCREEN_H 64
#define STATUS_H 10
#define BOTTOM_STRIP_H 12
#define CANVAS_W SCREEN_W
#define CANVAS_H (SCREEN_H - STATUS_H - BOTTOM_STRIP_H) /* 42 */
#define CANVAS_Y STATUS_H                               /* 10 */

/* ── progress-band slope ─────────────────────────────────────── */
#define SLANT 18 /* px difference between top-edge x and bottom-edge x */
#define BAND_TOP_RIGHT_X (CANVAS_W + SLANT)
#define BAND_BOTTOM_RIGHT_X CANVAS_W
#define BAND_ANIM_MS 250
#define BAND_ANIM_THRESHOLD 0.05f

/* ── text row geometry (canvas-relative y) ───────────────────── */
#define ROW_ALBUM_Y 1
#define ROW_TRACK_Y 10
#define ROW_ARTIST_Y 30
#define TEXT_MARGIN_X 2
#define PLAY_DOT_DIAMETER 4
#define PLAY_DOT_RADIUS (PLAY_DOT_DIAMETER / 2)
#define PLAY_DOT_PULSE_OUTER_RADIUS 4
#define PLAY_DOT_MARGIN_RIGHT 2
#define PLAY_DOT_TEXT_GAP 4
#define PLAY_DOT_RESERVED_W ((PLAY_DOT_PULSE_OUTER_RADIUS * 2) + PLAY_DOT_MARGIN_RIGHT + PLAY_DOT_TEXT_GAP)
#define PLAY_DOT_CENTER_X (CANVAS_W - PLAY_DOT_MARGIN_RIGHT - PLAY_DOT_PULSE_OUTER_RADIUS)
#define PLAY_DOT_CENTER_Y (ROW_ARTIST_Y + FONT_ARTIST->line_height / 2)
#define PLAY_DOT_PULSE_MS 1000u

/* ── marquee ─────────────────────────────────────────────────── */
#define MARQUEE_GAP_PX 20   /* blank gap before text loops */
#define MARQUEE_TIMER_MS 33 /* ~30 Hz while animated */
#define MARQUEE_SPEED 1     /* px per tick */
#define MARQUEE_FP_SHIFT 8
#define MARQUEE_FP_ONE (1 << MARQUEE_FP_SHIFT)
#define MARQUEE_SPEED_FP_MAX (MARQUEE_SPEED * MARQUEE_FP_ONE)
#define MARQUEE_SPEED_FP_MIN (MARQUEE_FP_ONE / 4)
#define MARQUEE_EASE_DISTANCE_PX 24
#define MARQUEE_LOOP_HOLD_MS 2000
#define MARQUEE_HOLD_TICKS ((MARQUEE_LOOP_HOLD_MS + MARQUEE_TIMER_MS - 1) / MARQUEE_TIMER_MS)
/* ── dither ──────────────────────────────────────────────────── */
#define DITHER_MARGIN 11 /* px top/bottom inside canvas for dither */
#define DITHER_OFFSET_Y -1
#define DITHER_STEP_X 4
#define DITHER_STEP_Y 2
#define DITHER_PHASE_ALT_ROW 2
#define DITHER_COLOR_LO 0xFFu
#define DITHER_COLOR_HI 0xFFu

/* ── fonts ───────────────────────────────────────────────────── */
LV_FONT_DECLARE(lv_font_tiny5_5x3);
LV_FONT_DECLARE(lv_font_chicago_ftf_8);
LV_FONT_DECLARE(lv_font_chicago_ftf_12);
LV_FONT_DECLARE(lv_font_chicago_ftf_20);

#define FONT_ALBUM (&lv_font_tiny5_5x3)
#define FONT_TRACK (&lv_font_chicago_ftf_20)
#define FONT_ARTIST (&lv_font_chicago_ftf_12)

/* ── canvas draw buffer ──────────────────────────────────────── */
LV_DRAW_BUF_DEFINE_STATIC(s_draw_buf, CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_NATIVE);

/* ── private state ───────────────────────────────────────────── */
typedef struct
{
    int32_t offset_fp;
    int32_t offset; /* current scroll offset (px) */
    int32_t width;  /* text pixel width (cached on text change) */
    uint16_t hold_ticks;
} marquee_t;

static struct
{
    lv_obj_t *screen;
    lv_obj_t *canvas;

    /* current data (owned copies) */
    char album[64];
    char track[64];
    char artist[64];
    uint32_t elapsed_ms;
    uint32_t total_ms;
    bool playing;
    int32_t band_offset_x;
    int32_t band_target_offset_x;
    bool band_animating;

    marquee_t m_album;
    marquee_t m_track;
    marquee_t m_artist;

    lv_timer_t *marquee_timer;
    bool active;
    bool dirty;              /* true = redraw needed */
    int32_t last_dot_radius; /* last rendered pulse-dot radius; -1 = not yet drawn */
} s;

static void redraw_canvas(void);

/* ─────────────────────────────────────────────────────────────── */
/*  Internal helpers                                               */
/* ─────────────────────────────────────────────────────────────── */

static inline float normalized_progress(uint32_t elapsed_ms, uint32_t total_ms)
{
    float progress = (total_ms > 0) ? (float)elapsed_ms / (float)total_ms : 0.0f;

    if (progress < 0.0f)
        progress = 0.0f;
    if (progress > 1.0f)
        progress = 1.0f;

    return progress;
}

static inline int32_t band_offset_x_from_progress(float progress)
{
    float value = progress * (float)BAND_TOP_RIGHT_X - (float)BAND_TOP_RIGHT_X;

    return (int32_t)(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

/** Return the x-coordinate of the progress-band right edge at canvas row y. */
static inline int32_t band_edge_x_from_offset(int32_t band_offset_x, int32_t y)
{
    float top_x = (float)band_offset_x + (float)BAND_TOP_RIGHT_X;
    float bot_x = (float)band_offset_x + (float)BAND_BOTTOM_RIGHT_X;
    float edge = top_x + (bot_x - top_x) * (float)y / (float)(CANVAS_H - 1);
    if (edge < 0)
        edge = 0;
    if (edge > CANVAS_W)
        edge = CANVAS_W;
    return (int32_t)edge;
}

static int32_t artist_avail_w(void)
{
    return CANVAS_W - TEXT_MARGIN_X * 2 - PLAY_DOT_RESERVED_W;
}

/** Measure single-line pixel width of @p text rendered with @p font. */
static int32_t measure_text_w(const char *text, const lv_font_t *font)
{
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    return sz.x;
}

/** Update cached text width for a marquee slot when text changes. */
static void marquee_reset(marquee_t *m, const char *text, const lv_font_t *font,
                          int32_t avail_w)
{
    m->offset_fp = 0;
    m->width = measure_text_w(text, font);
    m->offset = 0;
    m->hold_ticks = 0;
    (void)avail_w;
}

static bool now_playing_marquee_active(const marquee_t *m, int32_t avail_w)
{
    return m->width > avail_w;
}

static bool now_playing_redraw_timer_required(void)
{
    int32_t avail_w = CANVAS_W - TEXT_MARGIN_X * 2;

    return s.playing ||
           now_playing_marquee_active(&s.m_album, avail_w) ||
           now_playing_marquee_active(&s.m_track, avail_w) ||
           now_playing_marquee_active(&s.m_artist, artist_avail_w());
}

static void now_playing_update_redraw_timer(void)
{
    if (s.marquee_timer == NULL)
    {
        return;
    }

    if (!s.active || !now_playing_redraw_timer_required())
    {
        lv_timer_pause(s.marquee_timer);
        return;
    }

    lv_timer_set_period(s.marquee_timer, MARQUEE_TIMER_MS);
    lv_timer_resume(s.marquee_timer);
}

/** Compute the quantised playback-dot radius for the current tick without drawing.
 * Returns PLAY_DOT_RADIUS when paused, 0-2 when playing. */
static int32_t now_playing_dot_radius(void)
{
    if (!s.playing)
        return PLAY_DOT_RADIUS;

    uint32_t phase = lv_tick_get() % PLAY_DOT_PULSE_MS;
    uint32_t half = PLAY_DOT_PULSE_MS / 2u;
    uint32_t ramp = (phase <= half) ? phase : (PLAY_DOT_PULSE_MS - phase);
    uint32_t t = (ramp * 255u + (half / 2u)) / half;

    /* smoothstep_u8 not yet declared here; inline the same arithmetic */
    uint32_t t2 = (t * t + 127u) / 255u;
    uint32_t inv = 3u * 255u - 2u * t;
    uint32_t eased = (t2 * inv + 127u) / 255u;

    return (int32_t)((2 * (int32_t)eased + 127) / 255);
}

static uint32_t smoothstep_u8(uint32_t t)
{
    uint32_t t2 = (t * t + 127u) / 255u;
    uint32_t inv = 3u * 255u - 2u * t;

    return (t2 * inv + 127u) / 255u;
}

static void progress_band_anim_exec(void *var, int32_t value)
{
    int32_t *band_offset_x = var;

    *band_offset_x = value;
    s.dirty = true;

    if (lv_timer_get_paused(s.marquee_timer))
    {
        redraw_canvas();
        s.dirty = false;
    }
}

static void progress_band_anim_completed_cb(lv_anim_t *a)
{
    (void)a;

    s.band_animating = false;
    s.band_offset_x = s.band_target_offset_x;
    s.dirty = true;

    if (lv_timer_get_paused(s.marquee_timer))
    {
        redraw_canvas();
        s.dirty = false;
    }
}

static void set_band_offset_x(int32_t band_offset_x)
{
    lv_anim_delete(&s.band_offset_x, progress_band_anim_exec);
    s.band_animating = false;
    s.band_offset_x = band_offset_x;
    s.band_target_offset_x = band_offset_x;
}

static void animate_band_offset_x(int32_t band_offset_x)
{
    lv_anim_t anim;

    if (s.band_offset_x == band_offset_x)
        return;

    lv_anim_delete(&s.band_offset_x, progress_band_anim_exec);

    s.band_target_offset_x = band_offset_x;
    s.band_animating = true;

    lv_anim_init(&anim);
    lv_anim_set_var(&anim, &s.band_offset_x);
    lv_anim_set_exec_cb(&anim, progress_band_anim_exec);
    lv_anim_set_values(&anim, s.band_offset_x, band_offset_x);
    lv_anim_set_duration(&anim, BAND_ANIM_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&anim, progress_band_anim_completed_cb);
    lv_anim_start(&anim);
}

static int32_t marquee_step_fp(const marquee_t *m)
{
    int32_t loop_at_fp = (m->width + MARQUEE_GAP_PX) << MARQUEE_FP_SHIFT;
    int32_t ease_dist_fp = MARQUEE_EASE_DISTANCE_PX << MARQUEE_FP_SHIFT;
    int32_t remaining_fp = loop_at_fp - m->offset_fp;
    uint32_t start_t = 255u;
    uint32_t end_t = 255u;
    uint32_t factor;

    if (m->offset_fp < ease_dist_fp)
        start_t = (uint32_t)(((int64_t)m->offset_fp * 255u) / ease_dist_fp);

    if (remaining_fp < ease_dist_fp)
        end_t = (uint32_t)(((int64_t)LV_MAX(remaining_fp, 0) * 255u) / ease_dist_fp);

    factor = LV_MIN(smoothstep_u8(start_t), smoothstep_u8(end_t));

    return MARQUEE_SPEED_FP_MIN +
           (int32_t)(((MARQUEE_SPEED_FP_MAX - MARQUEE_SPEED_FP_MIN) * factor + 127u) / 255u);
}

/** Advance marquee scroll by one tick. */
static void marquee_tick(marquee_t *m, int32_t avail_w)
{
    if (m->width <= avail_w)
    {
        m->offset_fp = 0;
        m->offset = 0;
        m->hold_ticks = 0;
        return;
    }

    if (m->hold_ticks > 0)
    {
        m->hold_ticks--;
        return;
    }

    m->offset_fp += marquee_step_fp(m);

    if (m->offset_fp >= ((m->width + MARQUEE_GAP_PX) << MARQUEE_FP_SHIFT))
    {
        m->offset_fp = 0;
        m->offset = 0;
        m->hold_ticks = MARQUEE_HOLD_TICKS;
        return;
    }

    m->offset = m->offset_fp >> MARQUEE_FP_SHIFT;
}

/**
 * Draw @p text at (x, y) in the layer, applying a left-clip for black text
 * and a right-clip for white text, so that text colour inverts at the
 * progress-band edge.
 *
 * @param layer     Target layer (canvas layer)
 * @param font      Font to use
 * @param text      Text to render
 * @param x         Left edge of the text box (canvas coords)
 * @param y         Top edge of the text box (canvas coords)
 * @param w         Available width for the text box
 * @param scroll_x  Marquee scroll offset (shift text left by this many px)
 * @param split_x   x-coordinate of band edge at row y (canvas coords)
 * @param full_clip Full clip area to restore after drawing
 */
static void draw_text_split_ex(lv_layer_t *layer,
                               const lv_font_t *font,
                               const char *text,
                               int32_t x, int32_t y, int32_t w,
                               int32_t scroll_x, int32_t split_x,
                               const lv_area_t *full_clip,
                               bool invert_colors)
{
    int32_t fh = font->line_height;

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.opa = LV_OPA_COVER;
    dsc.sel_start = LV_DRAW_LABEL_NO_TXT_SEL;
    dsc.sel_end = LV_DRAW_LABEL_NO_TXT_SEL;
    dsc.ofs_x = -scroll_x;
    dsc.ofs_y = 0;
    dsc.text = text;
    dsc.text_static = 1;

    /* wide "logical" coord so text isn't wrapped */
    lv_area_t coords = {x, y, x + 32767, y + fh - 1};

    /* ── left of split_x ── */
    lv_area_t left_clip = {x, y, LV_MIN(x + w - 1, split_x - 1), y + fh - 1};
    if (left_clip.x1 <= left_clip.x2)
    {
        /* main: black on white (left), invert: white on white (invisible/ghost) or simply swap */
        dsc.color = invert_colors ? lv_color_white() : lv_color_black();
        layer->_clip_area = left_clip;
        lv_draw_label(layer, &dsc, &coords);
    }

    /* ── right of split_x ── */
    lv_area_t right_clip = {LV_MAX(x, split_x), y, x + w - 1, y + fh - 1};
    if (right_clip.x1 <= right_clip.x2)
    {
        /* main: white on black (right), invert: black on black */
        dsc.color = invert_colors ? lv_color_black() : lv_color_white();
        layer->_clip_area = right_clip;
        lv_draw_label(layer, &dsc, &coords);
    }

    /* restore full clip */
    layer->_clip_area = *full_clip;
}

static void draw_text_split(lv_layer_t *layer,
                            const lv_font_t *font,
                            const char *text,
                            int32_t x, int32_t y, int32_t w,
                            int32_t scroll_x, int32_t split_x,
                            const lv_area_t *full_clip)
{
    draw_text_split_ex(layer, font, text, x, y, w, scroll_x, split_x, full_clip, false);
}

static void fill_checkerboard_dither(int32_t band_offset_x)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(s.canvas);
    uint32_t stride = db->header.stride;
    uint8_t *data = lv_draw_buf_goto_xy(db, 0, 0); /* skip palette bytes for I1 indexed format */

    for (int32_t y = DITHER_MARGIN + DITHER_OFFSET_Y; y < CANVAS_H - DITHER_MARGIN + DITHER_OFFSET_Y; y += DITHER_STEP_Y)
    {
        int32_t ex = band_edge_x_from_offset(band_offset_x, y);
        int32_t row_index = (y - (DITHER_MARGIN + DITHER_OFFSET_Y)) / DITHER_STEP_Y;
        int32_t phase = (row_index & 1) ? DITHER_PHASE_ALT_ROW : 0;
        int32_t start_x = ex + ((phase - (ex & (DITHER_STEP_X - 1)) + DITHER_STEP_X) & (DITHER_STEP_X - 1));

        for (int32_t x = start_x; x < CANVAS_W; x += DITHER_STEP_X)
        {
            /* LV_COLOR_FORMAT_I1: MSB-first packed bits */
            uint8_t *byte = data + (size_t)y * stride + (size_t)(x >> 3);
            *byte |= (uint8_t)(0x80u >> (x & 7));
        }
    }
}

static void draw_playback_dot(void)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(s.canvas);
    uint32_t stride = db->header.stride;
    uint8_t *data = lv_draw_buf_goto_xy(db, 0, 0); /* skip palette bytes for I1 indexed format */
    int32_t current_radius = 0;

    if (s.playing)
    {
        uint32_t phase = lv_tick_get() % PLAY_DOT_PULSE_MS;
        uint32_t half = PLAY_DOT_PULSE_MS / 2u;
        uint32_t ramp = (phase <= half) ? phase : (PLAY_DOT_PULSE_MS - phase);
        uint32_t t = (ramp * 255u + (half / 2u)) / half;
        uint32_t eased = smoothstep_u8(t);

        current_radius = (int32_t)((2 * (int32_t)eased + 127) / 255);
    }
    else
    {
        current_radius = PLAY_DOT_RADIUS;
    }

    if (current_radius <= 0)
        return;

    int32_t r2 = current_radius * current_radius;

    for (int32_t y = PLAY_DOT_CENTER_Y - current_radius; y <= PLAY_DOT_CENTER_Y + current_radius; y++)
    {
        if (y < 0 || y >= CANVAS_H)
            continue;

        int32_t edge_x = band_edge_x_from_offset(s.band_offset_x, y);
        int32_t dy = y - PLAY_DOT_CENTER_Y;

        for (int32_t x = PLAY_DOT_CENTER_X - current_radius; x <= PLAY_DOT_CENTER_X + current_radius; x++)
        {
            int32_t dx = x - PLAY_DOT_CENTER_X;
            int32_t dist2 = dx * dx + dy * dy;

            if (x < 0 || x >= CANVAS_W)
                continue;

            if (dist2 > r2)
                continue;

            bool white = (x >= edge_x);
            /* LV_COLOR_FORMAT_I1: MSB-first packed bits */
            uint8_t *byte = data + (size_t)y * stride + (size_t)(x >> 3);
            if (white)
                *byte |= (uint8_t)(0x80u >> (x & 7));
            else
                *byte &= (uint8_t)~(uint8_t)(0x80u >> (x & 7));
        }
    }
}

/** Redraw the entire canvas. */
static void redraw_canvas(void)
{
    int32_t top_x = band_edge_x_from_offset(s.band_offset_x, 0);
    int32_t bot_x = band_edge_x_from_offset(s.band_offset_x, CANVAS_H - 1);

    /* ── 1. clear buffer to black + sparse framebuffer dither ───── */
    {
        lv_draw_buf_t *db = lv_canvas_get_draw_buf(s.canvas);
        uint32_t stride = db->header.stride;           /* bytes per row */
        uint8_t *data = lv_draw_buf_goto_xy(db, 0, 0); /* skip palette bytes for I1 indexed format */

        memset(data, 0, (size_t)CANVAS_H * stride);
    }

    fill_checkerboard_dither(s.band_offset_x);

    /* ── 2. vector layer: white band + text ─────────────────────── */
    lv_layer_t layer;
    lv_canvas_init_layer(s.canvas, &layer);
    lv_area_t full = {0, 0, CANVAS_W - 1, CANVAS_H - 1};
    layer._clip_area = full;

    /* white progress band: filled rect for the rectangular body + triangle for the slant.
     * Using rect+triangle instead of two triangles avoids the 1px gap that appears along
     * a shared diagonal when a parallelogram is split into two triangles. */
    if (top_x > 0 || bot_x > 0)
    {
        /* Rectangular body: left edge to bottom-right corner */
        lv_draw_rect_dsc_t rect_dsc;
        lv_draw_rect_dsc_init(&rect_dsc);
        rect_dsc.bg_color = lv_color_white();
        rect_dsc.bg_opa = LV_OPA_COVER;
        rect_dsc.radius = 0;
        lv_area_t band_rect = {s.band_offset_x, 0,
                               s.band_offset_x + BAND_BOTTOM_RIGHT_X, CANVAS_H - 1};
        lv_draw_rect(&layer, &rect_dsc, &band_rect);

        /* Slant triangle: shares the rect's right column, so no gap at the join */
        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = lv_color_white();
        tri.opa = LV_OPA_COVER;
        tri.p[0] = (lv_point_precise_t){s.band_offset_x + BAND_BOTTOM_RIGHT_X, 0};
        tri.p[1] = (lv_point_precise_t){s.band_offset_x + BAND_TOP_RIGHT_X, 0};
        tri.p[2] = (lv_point_precise_t){s.band_offset_x + BAND_BOTTOM_RIGHT_X, CANVAS_H - 1};
        lv_draw_triangle(&layer, &tri);
    }

    /* text rows */
    int32_t avail_w = CANVAS_W - TEXT_MARGIN_X * 2; /* 124 */

    /* album */
    {
        int32_t cy = ROW_ALBUM_Y + FONT_ALBUM->line_height / 2;
        int32_t sx = band_edge_x_from_offset(s.band_offset_x, cy);
        draw_text_split(&layer, FONT_ALBUM, s.album,
                        TEXT_MARGIN_X, ROW_ALBUM_Y, avail_w,
                        s.m_album.offset, sx, &full);
        /* second copy for seamless loop */
        if (s.m_album.width > avail_w)
            draw_text_split(&layer, FONT_ALBUM, s.album,
                            TEXT_MARGIN_X, ROW_ALBUM_Y, avail_w,
                            s.m_album.offset - (s.m_album.width + MARQUEE_GAP_PX),
                            sx, &full);
    }

    /* track */
    {
        int32_t cy = ROW_TRACK_Y + FONT_TRACK->line_height / 2;
        int32_t sx = band_edge_x_from_offset(s.band_offset_x, cy);

        /* Drop shadow: draw offset (2, 2) version with INVERSE inversion logic */
        draw_text_split_ex(&layer, FONT_TRACK, s.track,
                           TEXT_MARGIN_X + 2, ROW_TRACK_Y + 2, avail_w,
                           s.m_track.offset, sx, &full, true);
        if (s.m_track.width > avail_w)
            draw_text_split_ex(&layer, FONT_TRACK, s.track,
                               TEXT_MARGIN_X + 2, ROW_TRACK_Y + 2, avail_w,
                               s.m_track.offset - (s.m_track.width + MARQUEE_GAP_PX),
                               sx, &full, true);

        /* main text */
        draw_text_split(&layer, FONT_TRACK, s.track,
                        TEXT_MARGIN_X, ROW_TRACK_Y, avail_w,
                        s.m_track.offset, sx, &full);
        /* second copy for seamless loop */
        if (s.m_track.width > avail_w)
            draw_text_split(&layer, FONT_TRACK, s.track,
                            TEXT_MARGIN_X, ROW_TRACK_Y, avail_w,
                            s.m_track.offset - (s.m_track.width + MARQUEE_GAP_PX),
                            sx, &full);
    }

    /* artist */
    {
        int32_t artist_w = artist_avail_w();
        int32_t cy = ROW_ARTIST_Y + FONT_ARTIST->line_height / 2;
        int32_t sx = band_edge_x_from_offset(s.band_offset_x, cy);
        draw_text_split(&layer, FONT_ARTIST, s.artist,
                        TEXT_MARGIN_X, ROW_ARTIST_Y, artist_w,
                        s.m_artist.offset, sx, &full);
        /* second copy for seamless loop */
        if (s.m_artist.width > artist_w)
            draw_text_split(&layer, FONT_ARTIST, s.artist,
                            TEXT_MARGIN_X, ROW_ARTIST_Y, artist_w,
                            s.m_artist.offset - (s.m_artist.width + MARQUEE_GAP_PX),
                            sx, &full);
    }

    lv_canvas_finish_layer(s.canvas, &layer);
    draw_playback_dot();
    s.last_dot_radius = now_playing_dot_radius();
}

/* ── timer callbacks ─────────────────────────────────────────── */

static void marquee_timer_cb(lv_timer_t *t)
{
    (void)t;

    int32_t avail_w = CANVAS_W - TEXT_MARGIN_X * 2;
    int32_t artist_w = artist_avail_w();
    bool need_redraw = false;

    /* advance each marquee that is actually scrolling */
    int32_t prev;

    prev = s.m_album.offset;
    marquee_tick(&s.m_album, avail_w);
    if (s.m_album.offset != prev)
        need_redraw = true;

    prev = s.m_track.offset;
    marquee_tick(&s.m_track, avail_w);
    if (s.m_track.offset != prev)
        need_redraw = true;

    prev = s.m_artist.offset;
    marquee_tick(&s.m_artist, artist_w);
    if (s.m_artist.offset != prev)
        need_redraw = true;

    if (s.playing)
    {
        int32_t cur_radius = now_playing_dot_radius();
        if (cur_radius != s.last_dot_radius)
            need_redraw = true;
    }

    if (need_redraw || s.dirty)
    {
        redraw_canvas();
        s.dirty = false;
    }
}

/* ─────────────────────────────────────────────────────────────── */
/*  Public API                                                      */
/* ─────────────────────────────────────────────────────────────── */

lv_obj_t *screen_now_playing_create(void)
{
    /* default text */
    strncpy(s.album, "Album Name", sizeof(s.album) - 1);
    strncpy(s.track, "Track Name", sizeof(s.track) - 1);
    strncpy(s.artist, "Artist Name", sizeof(s.artist) - 1);
    s.elapsed_ms = 0;
    s.total_ms = 0;
    s.playing = false;
    s.band_offset_x = band_offset_x_from_progress(0.0f);
    s.band_target_offset_x = s.band_offset_x;
    s.band_animating = false;
    s.active = false;
    s.last_dot_radius = -1;

    /* screen */
    s.screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s.screen);
    lv_obj_set_style_bg_color(s.screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.screen, LV_OPA_COVER, 0);
    lv_obj_set_size(s.screen, SCREEN_W, SCREEN_H);

    /* canvas */
    LV_DRAW_BUF_INIT_STATIC(s_draw_buf);
    s.canvas = lv_canvas_create(s.screen);
    lv_obj_set_pos(s.canvas, 0, CANVAS_Y);
    lv_canvas_set_draw_buf(s.canvas, &s_draw_buf);
    /* Set palette for I1 indexed format: index 0 = opaque black, index 1 = opaque white */
    lv_canvas_set_palette(s.canvas, 0, (lv_color32_t){.blue = 0, .green = 0, .red = 0, .alpha = 255});
    lv_canvas_set_palette(s.canvas, 1, (lv_color32_t){.blue = 255, .green = 255, .red = 255, .alpha = 255});
    lv_canvas_fill_bg(s.canvas, lv_color_black(), LV_OPA_COVER);

    /* init marquee widths */
    marquee_reset(&s.m_album, s.album, FONT_ALBUM,
                  CANVAS_W - TEXT_MARGIN_X * 2);
    marquee_reset(&s.m_track, s.track, FONT_TRACK,
                  CANVAS_W - TEXT_MARGIN_X * 2);
    marquee_reset(&s.m_artist, s.artist, FONT_ARTIST,
                  artist_avail_w());

    /* marquee + redraw timer (created paused; started in on_activate) */
    s.marquee_timer = lv_timer_create(marquee_timer_cb, MARQUEE_TIMER_MS, NULL);
    lv_timer_pause(s.marquee_timer);

    /* initial draw */
    s.dirty = true;
    redraw_canvas();

    return s.screen;
}

void screen_now_playing_update(const ui_now_playing_data_t *data)
{
    if (!data)
        return;

    int32_t avail_w = CANVAS_W - TEXT_MARGIN_X * 2;
    float old_progress = normalized_progress(s.elapsed_ms, s.total_ms);
    float new_progress = normalized_progress(data->elapsed_ms, data->total_ms);
    float progress_delta = new_progress - old_progress;
    int32_t new_band_offset_x = band_offset_x_from_progress(new_progress);
    bool text_changed = false;
    bool progress_changed = (data->elapsed_ms != s.elapsed_ms) || (data->total_ms != s.total_ms);
    bool playing_changed = (data->playing != s.playing);

    if (progress_delta < 0.0f)
        progress_delta = -progress_delta;

    if (data->album && strcmp(data->album, s.album) != 0)
    {
        strncpy(s.album, data->album, sizeof(s.album) - 1);
        s.album[sizeof(s.album) - 1] = '\0';
        marquee_reset(&s.m_album, s.album, FONT_ALBUM, avail_w);
        text_changed = true;
    }
    if (data->track && strcmp(data->track, s.track) != 0)
    {
        strncpy(s.track, data->track, sizeof(s.track) - 1);
        s.track[sizeof(s.track) - 1] = '\0';
        marquee_reset(&s.m_track, s.track, FONT_TRACK, avail_w);
        text_changed = true;
    }
    if (data->artist && strcmp(data->artist, s.artist) != 0)
    {
        strncpy(s.artist, data->artist, sizeof(s.artist) - 1);
        s.artist[sizeof(s.artist) - 1] = '\0';
        marquee_reset(&s.m_artist, s.artist, FONT_ARTIST, artist_avail_w());
        text_changed = true;
    }

    s.elapsed_ms = data->elapsed_ms;
    s.total_ms = data->total_ms;
    s.playing = data->playing;
    now_playing_update_redraw_timer();

    if (progress_changed)
    {
        bool active = !lv_timer_get_paused(s.marquee_timer);

        s.band_target_offset_x = new_band_offset_x;

        if (!active)
        {
            set_band_offset_x(new_band_offset_x);
        }
        else if (progress_delta > BAND_ANIM_THRESHOLD)
        {
            animate_band_offset_x(new_band_offset_x);
        }
        else if (!s.band_animating)
        {
            s.band_offset_x = new_band_offset_x;
        }
    }

    s.dirty = true;

    /* immediate redraw only when timer is paused (screen inactive) */
    if ((text_changed || progress_changed || playing_changed) && lv_timer_get_paused(s.marquee_timer))
    {
        redraw_canvas();
        s.dirty = false;
    }
}

void screen_now_playing_on_activate(void)
{
    s.active = true;
    s.dirty = true;
    now_playing_update_redraw_timer();

    if (lv_timer_get_paused(s.marquee_timer))
    {
        redraw_canvas();
        s.dirty = false;
    }
}

void screen_now_playing_on_deactivate(void)
{
    s.active = false;
    lv_timer_pause(s.marquee_timer);
}
