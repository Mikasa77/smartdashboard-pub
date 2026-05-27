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

#include "keyboard.h"
#include "sheet.h"
#include "dashboard_screen.h"
#include "dashboard_colours.h"
#include "dashboard_log.h"
#include <stdbool.h>
#include <string.h>

static const char *TAG = "keyboard";

/* ── Module state ─────────────────────────────────────────────────────────── */

static struct {
    lv_obj_t  *container;
    lv_obj_t  *backdrop;
    lv_obj_t  *parent_container;
    keyboard_close_cb_t on_close;
    void      *user_data;
    bool       is_open;

    /* drag-to-dismiss */
    lv_point_t drag_start_pt;
    int32_t    drag_start_y;
    bool       dragging;
    int32_t    open_y;
} s_kb;

static struct {
    lv_keyboard_mode_t prev_mode;
    uint32_t           last_shift_ms;
    bool               momentary;
    bool               latched;
    bool               programmatic;
} s_shift;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void _close_internal(bool animated, bool fire_callback);

/* ── Animation helpers ────────────────────────────────────────────────────── */

static void _close_anim_done(lv_anim_t *a)
{
    (void)a;
    if (s_kb.container) {
        lv_obj_delete(s_kb.container);
        s_kb.container = NULL;
    }
    s_kb.is_open = false;
    LOG_D(TAG, "Keyboard cleaned up");
}

/* ── Handle drag-to-dismiss ───────────────────────────────────────────────── */

static void _handle_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &s_kb.drag_start_pt);
    s_kb.drag_start_y = lv_obj_get_y(s_kb.container);
    s_kb.dragging     = true;
    lv_anim_delete(s_kb.container, NULL);
}

static void _handle_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_kb.dragging) return;
    lv_point_t cur;
    lv_indev_get_point(lv_indev_get_act(), &cur);
    int32_t delta_y = cur.y - s_kb.drag_start_pt.y;
    int32_t new_y   = s_kb.drag_start_y + delta_y;
    if (new_y < s_kb.open_y) {
        int32_t over = s_kb.open_y - new_y;
        new_y = s_kb.open_y - (int32_t)(over * KB_RESIST_FACTOR);
    }
    lv_obj_set_y(s_kb.container, new_y);
}

static void _handle_released(lv_event_t *e)
{
    (void)e;
    if (!s_kb.dragging) return;
    s_kb.dragging = false;
    int32_t cur_y = lv_obj_get_y(s_kb.container);
    int32_t dist  = cur_y - s_kb.open_y;
    LOG_D(TAG, "Drag release %+d px from open", (int)dist);
    if (dist > KB_DISMISS_PX) {
        keyboard_close();
    } else {
        sheet_animate_y(s_kb.container, cur_y, s_kb.open_y, KB_ANIM_SNAP_MS, lv_anim_path_ease_out, NULL);
    }
}

/* ── Backdrop ─────────────────────────────────────────────────────────────── */

static void _backdrop_clicked_cb(lv_event_t *e)
{
    (void)e;
    keyboard_close();
}

/* ── lv_keyboard OK / hide button ────────────────────────────────────────── */

static void _kb_widget_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        keyboard_close();
    }
}

static void _kb_mode_changed_cb(lv_event_t *e)
{
    lv_obj_t *kb = (lv_obj_t *)lv_event_get_target(e);
    if (s_shift.programmatic) return;

    lv_keyboard_mode_t curr = lv_keyboard_get_mode(kb);
    lv_keyboard_mode_t prev = s_shift.prev_mode;

    if (prev == LV_KEYBOARD_MODE_TEXT_LOWER &&
        curr == LV_KEYBOARD_MODE_TEXT_UPPER) {
        /* Shift pressed: begin momentary upper */
        s_shift.last_shift_ms = lv_tick_get();
        s_shift.momentary     = true;

    } else if (prev == LV_KEYBOARD_MODE_TEXT_UPPER &&
               curr == LV_KEYBOARD_MODE_TEXT_LOWER) {
        /* Shift pressed while upper */
        if (s_shift.latched) {
            /* Caps was latched — unlock, stay lower */
            s_shift.latched = false;
        } else if (s_shift.momentary &&
                   lv_tick_get() - s_shift.last_shift_ms <= KB_CAPS_DOUBLE_TAP_MS) {
            /* Double-tap: latch caps lock */
            s_shift.programmatic = true;
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_UPPER);
            s_shift.programmatic = false;
            s_shift.latched   = true;
            s_shift.momentary = false;
        }

    } else if (prev == LV_KEYBOARD_MODE_TEXT_UPPER &&
               curr == LV_KEYBOARD_MODE_TEXT_UPPER) {
        /* Character typed while upper — auto-return unless latched */
        if (s_shift.momentary && !s_shift.latched) {
            s_shift.programmatic = true;
            lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_TEXT_LOWER);
            s_shift.programmatic = false;
            s_shift.momentary    = false;
        }
    }

    s_shift.prev_mode = lv_keyboard_get_mode(kb);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool keyboard_is_open(void)
{
    return s_kb.is_open;
}

void keyboard_open(lv_obj_t *parent_container, lv_obj_t *textarea,
                   keyboard_close_cb_t on_close, void *user_data)
{
    if (s_kb.is_open) {
        LOG_W(TAG, "keyboard_open called while already open — ignored");
        return;
    }

    s_kb.parent_container = parent_container;
    s_kb.on_close         = on_close;
    s_kb.user_data        = user_data;
    s_kb.is_open          = true;
    s_kb.dragging         = false;

    lv_obj_t *layer = lv_layer_top();

    /* ── Backdrop — only when used standalone; the sheet's own backdrop
         handles dismiss in the edit-sheet context (parent_container=NULL) ── */
    s_kb.backdrop = NULL;
    if (parent_container) {
        s_kb.backdrop = sheet_make_backdrop(_backdrop_clicked_cb);
    }

    /* ── Container ────────────────────────────────────────────────────────── */
    s_kb.container = lv_obj_create(layer);
    lv_obj_set_size(s_kb.container, SCREEN_W, KB_SHEET_H);
    lv_obj_set_style_bg_color(s_kb.container, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(s_kb.container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_kb.container, 0, 0);
    lv_obj_set_style_radius(s_kb.container, 0, 0);
    lv_obj_set_style_pad_all(s_kb.container, 0, 0);
    lv_obj_remove_flag(s_kb.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_kb.container, LV_OBJ_FLAG_CLICKABLE);

    /* ── Handle zone ──────────────────────────────────────────────────────── */
    lv_obj_t *handle_zone = lv_obj_create(s_kb.container);
    lv_obj_set_size(handle_zone, SCREEN_W, KB_HANDLE_ZONE_H);
    lv_obj_set_pos(handle_zone, 0, 0);
    lv_obj_set_style_bg_opa(handle_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(handle_zone, 0, 0);
    lv_obj_set_style_pad_all(handle_zone, 0, 0);
    lv_obj_remove_flag(handle_zone, LV_OBJ_FLAG_SCROLLABLE);

    /* Drag-to-dismiss and visual handle bar only in standalone (non-sub-panel) mode */
    if (parent_container) {
        lv_obj_add_flag(handle_zone, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(handle_zone, _handle_pressed,  LV_EVENT_PRESSED,  NULL);
        lv_obj_add_event_cb(handle_zone, _handle_pressing, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(handle_zone, _handle_released, LV_EVENT_RELEASED, NULL);

        lv_obj_t *bar = lv_obj_create(handle_zone);
        lv_obj_set_size(bar, KB_HANDLE_BAR_W, KB_HANDLE_BAR_H);
        lv_obj_center(bar);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BORDER), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, KB_HANDLE_BAR_R, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── Keyboard widget — parented directly to container, no wrapper ─────── */
    lv_obj_t *kb = lv_keyboard_create(s_kb.container);
    lv_obj_set_size(kb, SCREEN_W, KB_H);
    lv_obj_set_pos(kb, 0, KB_HANDLE_ZONE_H);
    lv_keyboard_set_textarea(kb, textarea);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_BORDER),  LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(COL_TEXT),  LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(COL_BG),  LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 2, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 6, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, _kb_widget_cb, LV_EVENT_READY,  NULL);
    lv_obj_add_event_cb(kb, _kb_widget_cb, LV_EVENT_CANCEL, NULL);
    memset(&s_shift, 0, sizeof(s_shift));
    s_shift.prev_mode = LV_KEYBOARD_MODE_TEXT_LOWER;
    lv_obj_add_event_cb(kb, _kb_mode_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Animate up ───────────────────────────────────────────────────────── */
    s_kb.open_y = SCREEN_H - KB_SHEET_H;
    lv_obj_set_pos(s_kb.container, 0, SCREEN_H);
    sheet_animate_y(s_kb.container, SCREEN_H, s_kb.open_y, KB_ANIM_OPEN_MS, lv_anim_path_ease_out, NULL);

    if (parent_container) {
        sheet_lift(KB_SHEET_H);
    }

    LOG_D(TAG, "Keyboard open");
}

/* ── Internal close ───────────────────────────────────────────────────────── */

static void _close_internal(bool animated, bool fire_callback)
{
    if (!s_kb.container) return;

    if (s_kb.backdrop) {
        lv_obj_delete(s_kb.backdrop);
        s_kb.backdrop = NULL;
    }

    if (s_kb.parent_container) {
        sheet_lower(KB_SHEET_H);
        s_kb.parent_container = NULL;
    }

    /* Capture and clear callback before firing to prevent re-entrancy. */
    keyboard_close_cb_t cb = s_kb.on_close;
    void *ud = s_kb.user_data;
    s_kb.on_close   = NULL;
    s_kb.user_data  = NULL;

    if (animated) {
        int32_t cur_y = lv_obj_get_y(s_kb.container);
        sheet_animate_y(s_kb.container, cur_y, SCREEN_H, KB_ANIM_CLOSE_MS, lv_anim_path_ease_in, _close_anim_done);
        LOG_D(TAG, "Keyboard closing (animated)");
    } else {
        lv_obj_delete(s_kb.container);
        s_kb.container = NULL;
        s_kb.is_open   = false;
        LOG_D(TAG, "Keyboard closed (silent)");
    }

    if (fire_callback && cb) {
        cb(ud);
    }
}

void keyboard_close(void)
{
    if (!s_kb.is_open) return;
    s_kb.is_open = false;
    _close_internal(true, true);
}

void keyboard_close_silent(void)
{
    if (!s_kb.is_open) return;
    s_kb.is_open = false;
    _close_internal(false, false);
}
