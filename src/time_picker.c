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

#include "time_picker.h"
#include "sheet.h"
#include "dashboard_screen.h"
#include "dashboard_colours.h"
#include "dashboard_log.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

static const char *TAG = "time_picker";

/* ── Static gradient descriptors (stored by pointer in LVGL — must not be stack) */

static lv_grad_dsc_t s_grad_top;
static lv_grad_dsc_t s_grad_bot;
static bool s_grads_inited = false;

static void _init_grads(void)
{
    if (s_grads_inited) return;
    s_grads_inited = true;

    s_grad_top.dir          = LV_GRAD_DIR_VER;
    s_grad_top.stops_count  = 2;
    s_grad_top.stops[0].color = lv_color_hex(COL_SHEET);
    s_grad_top.stops[0].opa   = LV_OPA_COVER;
    s_grad_top.stops[0].frac  = 0;
    s_grad_top.stops[1].color = lv_color_hex(COL_SHEET);
    s_grad_top.stops[1].opa   = LV_OPA_TRANSP;
    s_grad_top.stops[1].frac  = 255;

    s_grad_bot.dir          = LV_GRAD_DIR_VER;
    s_grad_bot.stops_count  = 2;
    s_grad_bot.stops[0].color = lv_color_hex(COL_SHEET);
    s_grad_bot.stops[0].opa   = LV_OPA_TRANSP;
    s_grad_bot.stops[0].frac  = 0;
    s_grad_bot.stops[1].color = lv_color_hex(COL_SHEET);
    s_grad_bot.stops[1].opa   = LV_OPA_COVER;
    s_grad_bot.stops[1].frac  = 255;
}

/* ── Module state ─────────────────────────────────────────────────────────── */

static struct {
    lv_obj_t  *container;
    lv_obj_t  *backdrop;
    lv_obj_t  *parent_container;
    lv_obj_t  *hour_col;   /* the scrollable child, NOT the wrapper */
    lv_obj_t  *min_col;
    int        cur_hour;
    int        cur_minute;
    time_picker_select_cb_t on_select;
    void      *user_data;
    bool       is_open;
} s_tp;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void _close_internal(bool animated);

/* ── Animation helpers ────────────────────────────────────────────────────── */

static void _close_anim_done(lv_anim_t *a)
{
    (void)a;
    if (s_tp.container) {
        lv_obj_delete(s_tp.container);
        s_tp.container = NULL;
    }
    s_tp.is_open = false;
    LOG_D(TAG, "Time picker cleaned up");
}

/* ── Roller label colour update ───────────────────────────────────────────── */

/* items in col are lv_obj_t containers; first child is the label */
static void _update_col_colours(lv_obj_t *col, int sel_idx, int total)
{
    for (int i = 0; i < total; i++) {
        lv_obj_t *item = lv_obj_get_child(col, i);
        if (!item) continue;
        lv_obj_t *lbl = lv_obj_get_child(item, 0);
        if (!lbl) continue;
        int dist = abs(i - sel_idx);
        if (dist == 0) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
        } else if (dist == 1) {
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DIM), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
        } else {
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MUTE), 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        }
    }
}

/* ── Scroll-end callbacks ─────────────────────────────────────────────────── */

static void _hour_scroll_end_cb(lv_event_t *e)
{
    lv_obj_t *col = lv_event_get_target(e);
    int32_t scroll_y = lv_obj_get_scroll_y(col);
    int32_t idx = (scroll_y + TP_ITEM_H / 2) / TP_ITEM_H;
    if (idx < 0) idx = 0;
    if (idx >= TP_HOUR_TOTAL) idx = TP_HOUR_TOTAL - 1;
    s_tp.cur_hour = ((idx % TP_HOUR_COUNT) + TP_HOUR_COUNT) % TP_HOUR_COUNT;
    _update_col_colours(s_tp.hour_col, idx, TP_HOUR_TOTAL);
    LOG_D(TAG, "Hour scroll end — idx=%d hour=%d", (int)idx, s_tp.cur_hour);
}

static void _min_scroll_end_cb(lv_event_t *e)
{
    lv_obj_t *col = lv_event_get_target(e);
    int32_t scroll_y = lv_obj_get_scroll_y(col);
    int32_t idx = (scroll_y + TP_ITEM_H / 2) / TP_ITEM_H;
    if (idx < 0) idx = 0;
    if (idx >= TP_MIN_TOTAL) idx = TP_MIN_TOTAL - 1;
    int norm = ((idx % TP_MIN_COUNT) + TP_MIN_COUNT) % TP_MIN_COUNT;
    s_tp.cur_minute = norm * 5;
    _update_col_colours(s_tp.min_col, idx, TP_MIN_TOTAL);
    LOG_D(TAG, "Minute scroll end — idx=%d minute=%d", (int)idx, s_tp.cur_minute);
}

/* ── Backdrop ─────────────────────────────────────────────────────────────── */

static void _backdrop_clicked_cb(lv_event_t *e)
{
    (void)e;
    time_picker_close();
}

/* ── Column builder ───────────────────────────────────────────────────────── */
/* Returns the scrollable inner column (NOT the wrapper).
   Wrapper gets flex-grow applied by caller. */

static lv_obj_t *_build_column(lv_obj_t *roller_row, int total,
                                const char *(*label_fn)(int i, char *buf))
{
    /* Non-scrollable wrapper — clips the column and holds the fade overlays */
    lv_obj_t *wrap = lv_obj_create(roller_row);
    lv_obj_set_size(wrap, 10, TP_ROLLER_H);  /* width overridden by flex-grow below */
    lv_obj_set_flex_grow(wrap, 1);
    lv_obj_set_style_bg_color(wrap, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);

    /* Scrollable flex column — fills the wrapper */
    lv_obj_t *col = lv_obj_create(wrap);
    lv_obj_set_size(col, LV_PCT(100), TP_ROLLER_H);
    lv_obj_set_pos(col, 0, 0);
    lv_obj_set_style_bg_color(col, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_style_pad_top(col, TP_PAD_ENDS, 0);
    lv_obj_set_style_pad_bottom(col, TP_PAD_ENDS, 0);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(col, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_layout(col, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(col, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_flex_cross_place(col, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_row(col, 0, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);

    char buf[8];
    for (int i = 0; i < total; i++) {
        /* Fixed-height item container — what the snap system sees.
           Explicit COL_SHEET bg so the fade overlay gradients composite
           correctly (transparent items caused default-theme grey boxes). */
        lv_obj_t *item = lv_obj_create(col);
        lv_obj_set_size(item, LV_PCT(100), TP_ITEM_H);
        lv_obj_set_style_bg_color(item, lv_color_hex(COL_SHEET), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        const char *text = label_fn(i, buf);
        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MUTE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Fade overlays — children of wrap, NOT col, so they don't scroll */
    _init_grads();

    lv_obj_t *fade_top = lv_obj_create(wrap);
    lv_obj_set_size(fade_top, LV_PCT(100), TP_FADE_H);
    lv_obj_set_pos(fade_top, 0, 0);
    lv_obj_set_style_border_width(fade_top, 0, 0);
    lv_obj_set_style_pad_all(fade_top, 0, 0);
    lv_obj_remove_flag(fade_top, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_grad(fade_top, &s_grad_top, 0);
    lv_obj_set_style_bg_opa(fade_top, LV_OPA_COVER, 0);

    lv_obj_t *fade_bot = lv_obj_create(wrap);
    lv_obj_set_size(fade_bot, LV_PCT(100), TP_FADE_H);
    lv_obj_set_pos(fade_bot, 0, TP_ROLLER_H - TP_FADE_H);
    lv_obj_set_style_border_width(fade_bot, 0, 0);
    lv_obj_set_style_pad_all(fade_bot, 0, 0);
    lv_obj_remove_flag(fade_bot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_grad(fade_bot, &s_grad_bot, 0);
    lv_obj_set_style_bg_opa(fade_bot, LV_OPA_COVER, 0);

    return col;
}

static const char *_hour_label(int i, char *buf)
{
    int h = i % TP_HOUR_COUNT;
    snprintf(buf, 8, "%d", h);
    return buf;
}

static const char *_min_label(int i, char *buf)
{
    int m = (i % TP_MIN_COUNT) * 5;
    snprintf(buf, 8, "%02d", m);
    return buf;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool time_picker_is_open(void)
{
    return s_tp.is_open;
}

void time_picker_get_current(int *hour, int *minute)
{
    if (hour)   *hour   = s_tp.cur_hour;
    if (minute) *minute = s_tp.cur_minute;
}

void time_picker_open(lv_obj_t *parent_container, int initial_hour, int initial_minute,
                      time_picker_select_cb_t on_select, void *user_data)
{
    if (s_tp.is_open) {
        LOG_W(TAG, "time_picker_open called while already open — ignored");
        return;
    }

    s_tp.parent_container = parent_container;
    s_tp.cur_hour         = initial_hour;
    s_tp.cur_minute       = initial_minute;
    s_tp.on_select        = on_select;
    s_tp.user_data        = user_data;
    s_tp.is_open          = true;

    lv_obj_t *layer = lv_layer_top();

    /* ── Backdrop — only when used standalone; the sheet's own backdrop
         handles dismiss in the edit-sheet context (parent_container=NULL) ── */
    s_tp.backdrop = NULL;
    if (parent_container) {
        s_tp.backdrop = sheet_make_backdrop(_backdrop_clicked_cb);
    }

    /* ── Outer container ──────────────────────────────────────────────────── */
    s_tp.container = lv_obj_create(layer);
    lv_obj_set_size(s_tp.container, SCREEN_W, TP_PICKER_H);
    lv_obj_set_style_bg_color(s_tp.container, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(s_tp.container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tp.container, 0, 0);
    lv_obj_set_style_radius(s_tp.container, 0, 0);
    lv_obj_set_style_pad_all(s_tp.container, 0, 0);
    lv_obj_remove_flag(s_tp.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_tp.container, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(s_tp.container, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(s_tp.container, 0, 0);
    lv_obj_add_flag(s_tp.container, LV_OBJ_FLAG_CLICKABLE);

    /* ── Handle zone ──────────────────────────────────────────────────────── */
    lv_obj_t *handle_zone = lv_obj_create(s_tp.container);
    lv_obj_set_size(handle_zone, SCREEN_W, TP_HANDLE_ZONE_H);
    lv_obj_set_style_bg_opa(handle_zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(handle_zone, 0, 0);
    lv_obj_set_style_pad_all(handle_zone, 0, 0);
    lv_obj_remove_flag(handle_zone, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Visual handle bar only in standalone (non-sub-panel) mode */
    if (parent_container) {
        lv_obj_t *bar = lv_obj_create(handle_zone);
        lv_obj_set_size(bar, TP_HANDLE_BAR_W, TP_HANDLE_BAR_H);
        lv_obj_center(bar);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BORDER), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, TP_HANDLE_BAR_R, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── Title row ────────────────────────────────────────────────────────── */
    lv_obj_t *title_row = lv_obj_create(s_tp.container);
    lv_obj_set_size(title_row, SCREEN_W, TP_TITLE_H);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_remove_flag(title_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title_lbl = lv_label_create(title_row);
    lv_label_set_text(title_lbl, "Set time");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_align(title_lbl, LV_ALIGN_CENTER);

    /* ── Column label row: HR  :  MIN ────────────────────────────────────── */
    lv_obj_t *col_hdr = lv_obj_create(s_tp.container);
    lv_obj_set_size(col_hdr, SCREEN_W, TP_COL_LABEL_H);
    lv_obj_set_style_bg_opa(col_hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col_hdr, 0, 0);
    lv_obj_set_style_pad_hor(col_hdr, 16, 0);
    lv_obj_set_style_pad_ver(col_hdr, 0, 0);
    lv_obj_remove_flag(col_hdr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_layout(col_hdr, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(col_hdr, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_main_place(col_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_flex_cross_place(col_hdr, LV_FLEX_ALIGN_CENTER, 0);

    lv_obj_t *lbl_hr = lv_label_create(col_hdr);
    lv_label_set_text(lbl_hr, "HR");
    lv_obj_set_style_text_font(lbl_hr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_hr, lv_color_hex(COL_TEXT_MUTE), 0);
    lv_obj_set_flex_grow(lbl_hr, 1);
    lv_obj_set_style_text_align(lbl_hr, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *lbl_sep_hdr = lv_label_create(col_hdr);
    lv_label_set_text(lbl_sep_hdr, "");
    lv_obj_set_width(lbl_sep_hdr, 20);

    lv_obj_t *lbl_min = lv_label_create(col_hdr);
    lv_label_set_text(lbl_min, "MIN");
    lv_obj_set_style_text_font(lbl_min, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_min, lv_color_hex(COL_TEXT_MUTE), 0);
    lv_obj_set_flex_grow(lbl_min, 1);
    lv_obj_set_style_text_align(lbl_min, LV_TEXT_ALIGN_CENTER, 0);

    /* ── Roller area: two column wrappers + colon ─────────────────────────── */
    lv_obj_t *roller_row = lv_obj_create(s_tp.container);
    lv_obj_set_size(roller_row, SCREEN_W, TP_ROLLER_H);
    lv_obj_set_style_bg_opa(roller_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(roller_row, 0, 0);
    lv_obj_set_style_pad_hor(roller_row, 16, 0);
    lv_obj_set_style_pad_ver(roller_row, 0, 0);
    lv_obj_remove_flag(roller_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(roller_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(roller_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(roller_row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(roller_row, 0, 0);

    s_tp.hour_col = _build_column(roller_row, TP_HOUR_TOTAL, _hour_label);
    lv_obj_add_event_cb(s_tp.hour_col, _hour_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* Colon separator */
    lv_obj_t *colon = lv_label_create(roller_row);
    lv_label_set_text(colon, ":");
    lv_obj_set_width(colon, 20);
    lv_obj_set_style_text_align(colon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(colon, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(colon, &lv_font_montserrat_22, 0);
    lv_obj_remove_flag(colon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_tp.min_col = _build_column(roller_row, TP_MIN_TOTAL, _min_label);
    lv_obj_add_event_cb(s_tp.min_col, _min_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    /* ── Set initial scroll positions (no animation, before open anim) ───── */
    int init_hour_idx = TP_HOUR_COUNT + initial_hour;
    int init_min_idx  = TP_MIN_COUNT  + (initial_minute / 5);

    lv_obj_scroll_to_y(s_tp.hour_col, init_hour_idx * TP_ITEM_H, LV_ANIM_OFF);
    lv_obj_scroll_to_y(s_tp.min_col,  init_min_idx  * TP_ITEM_H, LV_ANIM_OFF);

    _update_col_colours(s_tp.hour_col, init_hour_idx, TP_HOUR_TOTAL);
    _update_col_colours(s_tp.min_col,  init_min_idx,  TP_MIN_TOTAL);

    /* ── Animate up ───────────────────────────────────────────────────────── */
    lv_obj_set_pos(s_tp.container, 0, SCREEN_H);
    int32_t open_y = SCREEN_H - TP_PICKER_H;
    sheet_animate_y(s_tp.container, SCREEN_H, open_y, TP_ANIM_OPEN_MS, lv_anim_path_ease_out, NULL);

    if (parent_container) {
        sheet_lift(TP_PICKER_H);
    }

    LOG_D(TAG, "Time picker open — initial %02d:%02d", initial_hour, initial_minute);
}

/* ── Internal close ───────────────────────────────────────────────────────── */

static void _close_internal(bool animated)
{
    if (!s_tp.container) return;

    if (s_tp.backdrop) {
        lv_obj_delete(s_tp.backdrop);
        s_tp.backdrop = NULL;
    }

    if (s_tp.parent_container) {
        sheet_lower(TP_PICKER_H);
        s_tp.parent_container = NULL;
    }

    if (animated) {
        int32_t cur_y = lv_obj_get_y(s_tp.container);
        sheet_animate_y(s_tp.container, cur_y, SCREEN_H, TP_ANIM_CLOSE_MS, lv_anim_path_ease_in, _close_anim_done);
        LOG_D(TAG, "Time picker closing (animated)");
    } else {
        lv_obj_delete(s_tp.container);
        s_tp.container = NULL;
        s_tp.is_open   = false;
        LOG_D(TAG, "Time picker closed (silent)");
    }
}

void time_picker_close(void)
{
    if (!s_tp.is_open) return;
    s_tp.is_open = false;
    _close_internal(true);
}

void time_picker_close_silent(void)
{
    if (!s_tp.is_open) return;
    s_tp.is_open = false;
    _close_internal(false);
}
