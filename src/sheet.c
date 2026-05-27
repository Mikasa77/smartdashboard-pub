/*
 * Copyright (C) 2026 Peter Jones
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "sheet.h"
#include "dashboard_screen.h"
#include "dashboard_colours.h"
#include "dashboard_log.h"
#include <stddef.h>
#include <stdbool.h>

static const char *TAG = "sheet";

/* ── Layout / animation constants ────────────────────────────────────────── */

#define HANDLE_ZONE_H    36     /* height of the drag-handle zone              */
#define HANDLE_BAR_W     40     /* visible pill width                          */
#define HANDLE_BAR_H      4     /* visible pill height                         */
#define HANDLE_BAR_R      2     /* pill corner radius                          */

#define ANIM_OPEN_MS    360     /* spring-in duration                          */
#define ANIM_CLOSE_MS   300     /* spring-out duration                         */
#define ANIM_SNAP_MS    200     /* snap-back to open position                  */
#define DISMISS_PX      120     /* drag distance past open_y that triggers dismiss */
#define RESIST_FACTOR  0.15f    /* drag resistance beyond open position        */

/* ── Module state ─────────────────────────────────────────────────────────── */

static struct {
    lv_obj_t  *wrapper;    /* transparent full-screen root inside lv_layer_top() */
    lv_obj_t  *container;  /* the sheet panel (child of wrapper)                 */
    int32_t    open_y;     /* y of container when fully open                     */
    int32_t    closed_y;   /* y of container when fully off-screen               */
    bool       top_anchor; /* true = slides down from top; false = up from bottom */
    bool       is_open;    /* false once close animation begins                  */

    /* drag tracking */
    lv_point_t press_start;
    int32_t    drag_start_y;
    bool       dragging;

    /* optional one-shot callback fired when close begins */
    sheet_close_cb_t close_cb;
    void            *close_ud;
} s_sheet;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void _do_close(void);

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void _close_anim_done(lv_anim_t *a)
{
    (void)a;
    if (s_sheet.wrapper) {
        lv_obj_delete(s_sheet.wrapper);
        s_sheet.wrapper   = NULL;
        s_sheet.container = NULL;
    }
    LOG_D(TAG, "Sheet cleaned up");
}

void sheet_animate_y(lv_obj_t *obj, int32_t from_y, int32_t to_y,
                     uint32_t duration_ms, lv_anim_path_cb_t path_cb,
                     lv_anim_ready_cb_t ready_cb)
{
    lv_anim_delete(obj, NULL);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_values(&a, from_y, to_y);
    lv_anim_set_duration(&a, duration_ms);
    lv_anim_set_path_cb(&a, path_cb);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

lv_obj_t *sheet_make_backdrop(lv_event_cb_t clicked_cb)
{
    lv_obj_t *bd = lv_obj_create(lv_layer_top());
    lv_obj_set_size(bd, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(bd, 0, 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bd, 0, 0);
    lv_obj_set_style_radius(bd, 0, 0);
    lv_obj_set_style_pad_all(bd, 0, 0);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bd, clicked_cb, LV_EVENT_CLICKED, NULL);
    return bd;
}

/* ── Event callbacks ──────────────────────────────────────────────────────── */

static void _backdrop_cb(lv_event_t *e)
{
    (void)e;
    sheet_close();
}

static void _handle_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &s_sheet.press_start);
    s_sheet.drag_start_y = lv_obj_get_y(s_sheet.container);
    s_sheet.dragging     = true;
    lv_anim_delete(s_sheet.container, NULL);
}

static void _handle_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_sheet.dragging) return;

    lv_point_t cur;
    lv_indev_get_point(lv_indev_get_act(), &cur);

    int32_t delta_y = cur.y - s_sheet.press_start.y;
    int32_t new_y   = s_sheet.drag_start_y + delta_y;

    if (s_sheet.top_anchor) {
        /* Top sheet: resist dragging DOWN past open position */
        if (new_y > s_sheet.open_y) {
            int32_t overshoot = new_y - s_sheet.open_y;
            new_y = s_sheet.open_y + (int32_t)(overshoot * RESIST_FACTOR);
        }
    } else {
        /* Bottom sheet: resist dragging UP past open position */
        if (new_y < s_sheet.open_y) {
            int32_t overshoot = s_sheet.open_y - new_y;
            new_y = s_sheet.open_y - (int32_t)(overshoot * RESIST_FACTOR);
        }
    }

    lv_obj_set_y(s_sheet.container, new_y);
}

static void _handle_released(lv_event_t *e)
{
    (void)e;
    if (!s_sheet.dragging) return;
    s_sheet.dragging = false;

    int32_t cur_y = lv_obj_get_y(s_sheet.container);
    int32_t dist  = cur_y - s_sheet.open_y;

    LOG_D(TAG, "Drag release %+d px from open", (int)dist);

    bool dismiss = s_sheet.top_anchor ? (dist < -DISMISS_PX)   /* dragged up */
                                      : (dist >  DISMISS_PX);  /* dragged down */
    if (dismiss) {
        _do_close();
    } else {
        sheet_animate_y(s_sheet.container, cur_y, s_sheet.open_y, ANIM_SNAP_MS, lv_anim_path_ease_out, NULL);
    }
}

/* ── Internal close ───────────────────────────────────────────────────────── */

static void _do_close(void)
{
    /* Fire and clear the close callback before starting the animation so
       callers can clean up sub-panels while widget pointers are still valid. */
    if (s_sheet.close_cb) {
        sheet_close_cb_t cb = s_sheet.close_cb;
        void *ud = s_sheet.close_ud;
        s_sheet.close_cb = NULL;
        s_sheet.close_ud = NULL;
        cb(ud);
    }
    int32_t cur_y = lv_obj_get_y(s_sheet.container);
    sheet_animate_y(s_sheet.container, cur_y, s_sheet.closed_y, ANIM_CLOSE_MS, lv_anim_path_ease_in, _close_anim_done);
    LOG_D(TAG, "Sheet closing");
}

/* ── Shared open implementation ───────────────────────────────────────────── */

static void _sheet_open_impl(sheet_content_init_fn_t content_init_fn, bool top_anchor)
{
    if (s_sheet.wrapper) {
        LOG_W(TAG, "sheet_open() called while sheet already exists — ignored");
        return;
    }

    s_sheet.top_anchor = top_anchor;

    lv_obj_t *layer = lv_layer_top();

    /* ── Wrapper ──────────────────────────────────────────────────────────── */
    s_sheet.wrapper = lv_obj_create(layer);
    lv_obj_set_align(s_sheet.wrapper, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(s_sheet.wrapper, 0, 0);
    lv_obj_set_size(s_sheet.wrapper, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_opa(s_sheet.wrapper, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sheet.wrapper, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sheet.wrapper, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_sheet.wrapper, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_sheet.wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_sheet.wrapper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sheet.wrapper, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    /* ── Backdrop ─────────────────────────────────────────────────────────── */
    lv_obj_t *backdrop = lv_obj_create(s_sheet.wrapper);
    lv_obj_set_align(backdrop, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(backdrop, 0, 0);
    lv_obj_set_size(backdrop, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(backdrop, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(backdrop, 0, LV_PART_MAIN);
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(backdrop, _backdrop_cb, LV_EVENT_CLICKED, NULL);

    /* ── Sheet container ──────────────────────────────────────────────────── */
    s_sheet.container = lv_obj_create(s_sheet.wrapper);
    lv_obj_set_width(s_sheet.container, SCREEN_W);
    lv_obj_set_height(s_sheet.container, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_sheet.container, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_sheet.container, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_sheet.container, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_sheet.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_sheet.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(s_sheet.container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_sheet.container, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_sheet.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_sheet.container, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(s_sheet.container, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN);

    /* ── Handle zone factory ──────────────────────────────────────────────── */
    lv_obj_t *handle_zone = lv_obj_create(s_sheet.container);
    lv_obj_set_size(handle_zone, SCREEN_W, HANDLE_ZONE_H);
    lv_obj_set_style_bg_opa(handle_zone, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(handle_zone, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(handle_zone, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(handle_zone, 0, LV_PART_MAIN);
    lv_obj_remove_flag(handle_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(handle_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(handle_zone, _handle_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(handle_zone, _handle_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(handle_zone, _handle_released, LV_EVENT_RELEASED, NULL);

    lv_obj_t *bar = lv_obj_create(handle_zone);
    lv_obj_set_size(bar, HANDLE_BAR_W, HANDLE_BAR_H);
    lv_obj_center(bar);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, HANDLE_BAR_R, LV_PART_MAIN);
    lv_obj_set_style_pad_all(bar, 0, LV_PART_MAIN);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Content area ─────────────────────────────────────────────────────── */
    lv_obj_t *content_area = lv_obj_create(s_sheet.container);
    lv_obj_set_width(content_area, SCREEN_W);
    lv_obj_set_height(content_area, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(content_area, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(content_area, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content_area, 0, LV_PART_MAIN);
    lv_obj_remove_flag(content_area, LV_OBJ_FLAG_SCROLLABLE);

    /* For top anchor: content sits above handle — swap order by moving handle to end */
    if (top_anchor) {
        lv_obj_move_to_index(handle_zone, -1); /* move handle to after content */
    }

    if (content_init_fn) content_init_fn(content_area);

    lv_obj_set_align(s_sheet.container, LV_ALIGN_DEFAULT);
    /* Park off-screen before measuring */
    lv_obj_set_y(s_sheet.container, top_anchor ? -SCREEN_H : SCREEN_H);

    lv_obj_update_layout(layer);

    int32_t sheet_h = lv_obj_get_height(s_sheet.container);
    if (sheet_h <= HANDLE_ZONE_H) {
        LOG_W(TAG, "Sheet content has no height — check content_init_fn");
    }

    /* Tail: extends the container background below the content.
       Sub-panels (keyboard, pickers) render as lv_layer_top() siblings in
       front of this wrapper, so the tail is always hidden behind them.
       Any sub-pixel gap between the sheet bottom and a sub-panel top during
       simultaneous close animations is masked by the tail instead of exposing
       the dimmed backdrop.  Only needed for bottom-anchored sheets. */
    if (!top_anchor) {
        lv_obj_t *tail = lv_obj_create(s_sheet.container);
        lv_obj_set_size(tail, SCREEN_W, SCREEN_H);
        lv_obj_set_style_bg_color(tail, lv_color_hex(COL_SHEET), 0);
        lv_obj_set_style_bg_opa(tail, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(tail, 0, 0);
        lv_obj_set_style_pad_all(tail, 0, 0);
        lv_obj_remove_flag(tail, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    if (top_anchor) {
        s_sheet.open_y   = 0;
        s_sheet.closed_y = -sheet_h;
    } else {
        s_sheet.open_y   = SCREEN_H - sheet_h;
        s_sheet.closed_y = SCREEN_H;
    }
    s_sheet.is_open = true;

    sheet_animate_y(s_sheet.container, s_sheet.closed_y, s_sheet.open_y, ANIM_OPEN_MS, lv_anim_path_ease_out, NULL);

    LOG_D(TAG, "Sheet open (anchor=%s): height=%d open_y=%d",
          top_anchor ? "top" : "bottom", (int)sheet_h, (int)s_sheet.open_y);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void sheet_open(sheet_content_init_fn_t content_init_fn)
{
    _sheet_open_impl(content_init_fn, false);
}

void sheet_open_top(sheet_content_init_fn_t content_init_fn)
{
    _sheet_open_impl(content_init_fn, true);
}

void sheet_close(void)
{
    if (!s_sheet.wrapper || !s_sheet.is_open) return;
    s_sheet.is_open = false;
    _do_close();
}

void sheet_lift(int32_t by_px)
{
    if (!s_sheet.wrapper || !s_sheet.is_open) return;
    s_sheet.open_y -= by_px;   /* adjust from stored target, not cur_y */
    int32_t cur_y = lv_obj_get_y(s_sheet.container);
    sheet_animate_y(s_sheet.container, cur_y, s_sheet.open_y, ANIM_OPEN_MS, lv_anim_path_ease_out, NULL);
    LOG_D(TAG, "Sheet lifted %d px -> open_y=%d", (int)by_px, (int)s_sheet.open_y);
}

void sheet_lower(int32_t by_px)
{
    if (!s_sheet.wrapper || !s_sheet.is_open) return;
    s_sheet.open_y += by_px;   /* adjust from stored target, not cur_y */
    int32_t cur_y = lv_obj_get_y(s_sheet.container);
    sheet_animate_y(s_sheet.container, cur_y, s_sheet.open_y, ANIM_CLOSE_MS, lv_anim_path_ease_in, NULL);
    LOG_D(TAG, "Sheet lowered %d px -> open_y=%d", (int)by_px, (int)s_sheet.open_y);
}

lv_obj_t *sheet_get_container(void)
{
    return (s_sheet.wrapper && s_sheet.is_open) ? s_sheet.container : NULL;
}

void sheet_set_close_cb(sheet_close_cb_t cb, void *user_data)
{
    s_sheet.close_cb = cb;
    s_sheet.close_ud = user_data;
}
