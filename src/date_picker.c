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

#include "date_picker.h"
#include "sheet.h"
#include "dashboard_screen.h"
#include "dashboard_colours.h"
#include "dashboard_log.h"
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static const char *TAG = "date_picker";

/* ── View types ───────────────────────────────────────────────────────────── */

typedef enum { DP_VIEW_DAY, DP_VIEW_MONTH, DP_VIEW_YEAR } dp_view_t;

/* ── Module state ─────────────────────────────────────────────────────────── */

static struct {
    lv_obj_t  *container;
    lv_obj_t  *backdrop;
    lv_obj_t  *parent_container;   /* sheet to lift; NULL = debug */
    lv_obj_t  *view_area;
    lv_obj_t  *hdr_btn;
    lv_obj_t  *hdr_lbl;
    dp_view_t  view;
    struct tm  selected;           /* currently chosen date */
    int        disp_year;
    int        disp_month;         /* 0-based */
    int        year_start;         /* first year shown in year grid */
    date_picker_select_cb_t on_select;
    void      *user_data;
    bool       is_open;

    /* drag-to-dismiss */
    lv_point_t drag_start_pt;
    int32_t    drag_start_y;
    bool       dragging;
    int32_t    open_y;
} s_dp;

/* ── Grid descriptor arrays (must stay alive while grids are in use) ──────── */

static int32_t s_col7_dsc[9];
static int32_t s_row_day_dsc[9];
static int32_t s_col4_dsc[6];
static int32_t s_row3_dsc[5];

static void _init_dscs(void)
{
    static bool done = false;
    if (done) return;
    done = true;

    for (int i = 0; i < 7; i++) s_col7_dsc[i] = LV_GRID_FR(1);
    s_col7_dsc[7] = LV_GRID_TEMPLATE_LAST;

    s_row_day_dsc[0] = 36;
    for (int i = 1; i <= 6; i++) s_row_day_dsc[i] = 56;
    s_row_day_dsc[7] = LV_GRID_TEMPLATE_LAST;

    for (int i = 0; i < 4; i++) s_col4_dsc[i] = LV_GRID_FR(1);
    s_col4_dsc[4] = LV_GRID_TEMPLATE_LAST;

    for (int i = 0; i < 3; i++) s_row3_dsc[i] = LV_GRID_FR(1);
    s_row3_dsc[3] = LV_GRID_TEMPLATE_LAST;
}

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void _build_day_view(void);
static void _build_month_view(void);
static void _build_year_view(void);
static void _update_header(void);
static void _close_internal(bool animated);

/* ── Animation helpers ────────────────────────────────────────────────────── */

static void _close_anim_done(lv_anim_t *a)
{
    (void)a;
    if (s_dp.container) {
        lv_obj_delete(s_dp.container);
        s_dp.container = NULL;
    }
    s_dp.is_open = false;
    LOG_D(TAG, "Date picker cleaned up");
}

/* ── View helpers ─────────────────────────────────────────────────────────── */

static lv_obj_t *_make_grid(lv_obj_t *parent, int32_t *col_dsc, int32_t *row_dsc,
                              int32_t pad_col, int32_t pad_row)
{
    lv_obj_t *g = lv_obj_create(parent);
    lv_obj_set_size(g, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g, 0, 0);
    lv_obj_set_style_pad_all(g, 0, 0);
    lv_obj_set_style_pad_column(g, pad_col, 0);
    lv_obj_set_style_pad_row(g, pad_row, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(g, LV_LAYOUT_GRID);
    lv_obj_set_style_grid_column_dsc_array(g, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(g, row_dsc, 0);
    return g;
}

/* ── Day callbacks ────────────────────────────────────────────────────────── */

static void _day_clicked_cb(lv_event_t *e)
{
    int day = (int)(intptr_t)lv_event_get_user_data(e);
    s_dp.selected.tm_year = s_dp.disp_year - 1900;
    s_dp.selected.tm_mon  = s_dp.disp_month;
    s_dp.selected.tm_mday = day;
    s_dp.selected.tm_hour = 0;
    s_dp.selected.tm_min  = 0;
    s_dp.selected.tm_sec  = 0;
    mktime(&s_dp.selected);
    LOG_D(TAG, "Day selected: %d/%d/%d", day, s_dp.disp_month + 1, s_dp.disp_year);
    date_picker_select_cb_t cb = s_dp.on_select;
    void *ud = s_dp.user_data;
    struct tm sel = s_dp.selected;
    date_picker_close();
    if (cb) cb(sel, ud);
}

/* ── Month callbacks ──────────────────────────────────────────────────────── */

static void _month_clicked_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    s_dp.disp_month = m;
    LOG_D(TAG, "Month selected: %d — drilling to day view", m + 1);
    _build_day_view();
    _update_header();
}

/* ── Year callbacks ───────────────────────────────────────────────────────── */

static void _year_clicked_cb(lv_event_t *e)
{
    int y = (int)(intptr_t)lv_event_get_user_data(e);
    s_dp.disp_year = y;
    LOG_D(TAG, "Year selected: %d — drilling to month view", y);
    _build_month_view();
    _update_header();
}

/* ── Header navigation — handles all three views (</>  navigate day months,
   month years, and year pages respectively) ────────────────────────────────── */

static void _prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_dp.view == DP_VIEW_DAY) {
        s_dp.disp_month--;
        if (s_dp.disp_month < 0) { s_dp.disp_month = 11; s_dp.disp_year--; }
        _build_day_view();
    } else if (s_dp.view == DP_VIEW_MONTH) {
        s_dp.disp_year--;
        _build_month_view();
    } else {
        s_dp.year_start -= 12;
        _build_year_view();
    }
    _update_header();
}

static void _next_cb(lv_event_t *e)
{
    (void)e;
    if (s_dp.view == DP_VIEW_DAY) {
        s_dp.disp_month++;
        if (s_dp.disp_month > 11) { s_dp.disp_month = 0; s_dp.disp_year++; }
        _build_day_view();
    } else if (s_dp.view == DP_VIEW_MONTH) {
        s_dp.disp_year++;
        _build_month_view();
    } else {
        s_dp.year_start += 12;
        _build_year_view();
    }
    _update_header();
}

static void _hdr_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_dp.view == DP_VIEW_DAY) {
        _build_month_view();
    } else if (s_dp.view == DP_VIEW_MONTH) {
        s_dp.year_start = (s_dp.disp_year / 12) * 12;
        _build_year_view();
    }
    _update_header();
}

/* ── Header label update ──────────────────────────────────────────────────── */

static void _update_header(void)
{
    char buf[32];
    if (s_dp.view == DP_VIEW_DAY) {
        struct tm t = {0};
        t.tm_year = s_dp.disp_year - 1900;
        t.tm_mon  = s_dp.disp_month;
        t.tm_mday = 1;
        mktime(&t);
        strftime(buf, sizeof(buf), "%B %Y", &t);
        lv_obj_add_flag(s_dp.hdr_btn, LV_OBJ_FLAG_CLICKABLE);
    } else if (s_dp.view == DP_VIEW_MONTH) {
        snprintf(buf, sizeof(buf), "%d", s_dp.disp_year);
        lv_obj_add_flag(s_dp.hdr_btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        snprintf(buf, sizeof(buf), "%d - %d", s_dp.year_start, s_dp.year_start + 11);
        lv_obj_remove_flag(s_dp.hdr_btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(s_dp.hdr_lbl, buf);
}

/* ── Day view ─────────────────────────────────────────────────────────────── */

static void _build_day_view(void)
{
    lv_obj_clean(s_dp.view_area);
    s_dp.view = DP_VIEW_DAY;

    lv_obj_t *grid = _make_grid(s_dp.view_area, s_col7_dsc, s_row_day_dsc, 4, 4);

    static const char *dow[] = {"Mo","Tu","We","Th","Fr","Sa","Su"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *lbl = lv_label_create(grid);
        lv_label_set_text(lbl, dow[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MUTE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_grid_cell(lbl, LV_GRID_ALIGN_CENTER, i, 1,
                                  LV_GRID_ALIGN_CENTER, 0, 1);
    }

    struct tm first = {0};
    first.tm_year = s_dp.disp_year - 1900;
    first.tm_mon  = s_dp.disp_month;
    first.tm_mday = 1;
    mktime(&first);
    int col0 = (first.tm_wday == 0) ? 6 : first.tm_wday - 1;

    struct tm next_m = first;
    next_m.tm_mon++;
    next_m.tm_mday = 0;
    mktime(&next_m);
    int days_in_month = next_m.tm_mday;

    time_t now_t = time(NULL);
    struct tm now_local = *localtime(&now_t);
    int today_y = now_local.tm_year + 1900;
    int today_m = now_local.tm_mon;
    int today_d = now_local.tm_mday;

    bool same_month = (s_dp.disp_year == today_y && s_dp.disp_month == today_m);
    bool sel_same   = (s_dp.disp_year == (s_dp.selected.tm_year + 1900) &&
                       s_dp.disp_month == s_dp.selected.tm_mon);

    for (int d = 1; d <= days_in_month; d++) {
        int idx  = col0 + d - 1;
        int grow = idx / 7 + 1;
        int gcol = idx % 7;

        bool is_today = same_month && (d == today_d);
        bool is_sel   = sel_same   && (d == s_dp.selected.tm_mday);

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        if (is_sel) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, DP_CELL_R, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
        } else if (is_today) {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_border_width(cell, DP_CELL_BORDER_W, 0);
            lv_obj_set_style_radius(cell, DP_CELL_R, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
        }

        lv_obj_t *lbl = lv_label_create(cell);
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", d);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        uint32_t txt_col = (is_sel) ? COL_BG : COL_TEXT;
        lv_obj_set_style_text_color(lbl, lv_color_hex(txt_col), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(cell, _day_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)d);

        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, gcol, 1,
                                   LV_GRID_ALIGN_STRETCH, grow, 1);
    }
}

/* ── Month view ───────────────────────────────────────────────────────────── */

static void _build_month_view(void)
{
    lv_obj_clean(s_dp.view_area);
    s_dp.view = DP_VIEW_MONTH;

    lv_obj_t *grid = _make_grid(s_dp.view_area, s_col4_dsc, s_row3_dsc, 8, 8);

    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };

    int sel_month = (s_dp.disp_year == (s_dp.selected.tm_year + 1900))
                    ? s_dp.selected.tm_mon : -1;

    for (int m = 0; m < 12; m++) {
        int col = m % 4;
        int row = m / 4;
        bool is_sel = (m == sel_month);

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        if (is_sel) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, DP_CELL_R, 0);
        } else {
            lv_obj_set_style_bg_color(cell, lv_color_hex(COL_BORDER), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, DP_CELL_R, 0);
        }

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, months[m]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(is_sel ? COL_BG : COL_TEXT), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(cell, _month_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)m);
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1,
                                   LV_GRID_ALIGN_STRETCH, row, 1);
    }
}

/* ── Year view ────────────────────────────────────────────────────────────── */

/* 3-column 4-row grid for 12 years */
static int32_t s_col3_dsc[5];
static int32_t s_row4_dsc[6];

static void _init_year_dscs(void)
{
    static bool done = false;
    if (done) return;
    done = true;
    for (int i = 0; i < 3; i++) s_col3_dsc[i] = LV_GRID_FR(1);
    s_col3_dsc[3] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < 4; i++) s_row4_dsc[i] = LV_GRID_FR(1);
    s_row4_dsc[4] = LV_GRID_TEMPLATE_LAST;
}

static void _build_year_view(void)
{
    _init_year_dscs();
    lv_obj_clean(s_dp.view_area);
    s_dp.view = DP_VIEW_YEAR;

    lv_obj_t *grid = _make_grid(s_dp.view_area, s_col3_dsc, s_row4_dsc, 8, 8);

    int sel_year = s_dp.selected.tm_year + 1900;

    for (int i = 0; i < 12; i++) {
        int y    = s_dp.year_start + i;
        int col  = i % 3;
        int row  = i / 3;
        bool is_sel = (y == sel_year);

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        if (is_sel) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, DP_CELL_R, 0);
        } else {
            lv_obj_set_style_bg_color(cell, lv_color_hex(COL_BORDER), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, DP_CELL_R, 0);
        }

        lv_obj_t *lbl = lv_label_create(cell);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", y);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(is_sel ? COL_BG : COL_TEXT), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(cell, _year_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)y);
        lv_obj_set_grid_cell(cell, LV_GRID_ALIGN_STRETCH, col, 1,
                                   LV_GRID_ALIGN_STRETCH, row, 1);
    }

}

/* ── Handle drag-to-dismiss ───────────────────────────────────────────────── */

static void _handle_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &s_dp.drag_start_pt);
    s_dp.drag_start_y = lv_obj_get_y(s_dp.container);
    s_dp.dragging     = true;
    lv_anim_delete(s_dp.container, NULL);
}

static void _handle_pressing(lv_event_t *e)
{
    (void)e;
    if (!s_dp.dragging) return;
    lv_point_t cur;
    lv_indev_get_point(lv_indev_get_act(), &cur);
    int32_t delta_y = cur.y - s_dp.drag_start_pt.y;
    int32_t new_y   = s_dp.drag_start_y + delta_y;
    /* Resist dragging up past open_y */
    if (new_y < s_dp.open_y) {
        int32_t over = s_dp.open_y - new_y;
        new_y = s_dp.open_y - (int32_t)(over * DP_RESIST_FACTOR);
    }
    lv_obj_set_y(s_dp.container, new_y);
}

static void _handle_released(lv_event_t *e)
{
    (void)e;
    if (!s_dp.dragging) return;
    s_dp.dragging = false;
    int32_t cur_y = lv_obj_get_y(s_dp.container);
    int32_t dist  = cur_y - s_dp.open_y;
    LOG_D(TAG, "Drag release %+d px from open", (int)dist);
    if (dist > DP_DISMISS_PX) {
        date_picker_close();
    } else {
        sheet_animate_y(s_dp.container, cur_y, s_dp.open_y, DP_ANIM_SNAP_MS, lv_anim_path_ease_out, NULL);
    }
}

/* ── Internal close ───────────────────────────────────────────────────────── */

static void _backdrop_clicked_cb(lv_event_t *e)
{
    (void)e;
    date_picker_close();
}

static void _close_internal(bool animated)
{
    if (!s_dp.container) return;

    if (s_dp.backdrop) {
        lv_obj_delete(s_dp.backdrop);
        s_dp.backdrop = NULL;
    }

    if (s_dp.parent_container) {
        sheet_lower(DP_PICKER_H);
        s_dp.parent_container = NULL;
    }

    if (animated) {
        int32_t cur_y = lv_obj_get_y(s_dp.container);
        sheet_animate_y(s_dp.container, cur_y, SCREEN_H, DP_ANIM_CLOSE_MS, lv_anim_path_ease_in, _close_anim_done);
        LOG_D(TAG, "Date picker closing (animated)");
    } else {
        lv_obj_delete(s_dp.container);
        s_dp.container = NULL;
        s_dp.is_open   = false;
        LOG_D(TAG, "Date picker closed (silent)");
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

bool date_picker_is_open(void)
{
    return s_dp.is_open;
}

void date_picker_open(lv_obj_t *parent_container, struct tm initial_date,
                      date_picker_select_cb_t on_select, void *user_data)
{
    if (s_dp.is_open) {
        LOG_W(TAG, "date_picker_open called while already open — ignored");
        return;
    }

    _init_dscs();

    s_dp.parent_container = parent_container;
    s_dp.selected         = initial_date;
    s_dp.disp_year        = initial_date.tm_year + 1900;
    s_dp.disp_month       = initial_date.tm_mon;
    s_dp.year_start       = (s_dp.disp_year / 12) * 12;
    s_dp.on_select        = on_select;
    s_dp.user_data        = user_data;
    s_dp.is_open          = true;

    lv_obj_t *layer = lv_layer_top();

    /* ── Backdrop — only when used standalone; the sheet's own backdrop
         handles dismiss in the edit-sheet context (parent_container=NULL) ── */
    s_dp.backdrop = NULL;
    if (parent_container) {
        s_dp.backdrop = sheet_make_backdrop(_backdrop_clicked_cb);
    }

    /* ── Outer container ──────────────────────────────────────────────────── */
    s_dp.container = lv_obj_create(layer);
    lv_obj_set_size(s_dp.container, SCREEN_W, DP_PICKER_H);
    lv_obj_set_style_bg_color(s_dp.container, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(s_dp.container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dp.container, 0, 0);
    lv_obj_set_style_radius(s_dp.container, 0, 0);
    lv_obj_set_style_pad_all(s_dp.container, 0, 0);
    lv_obj_remove_flag(s_dp.container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(s_dp.container, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(s_dp.container, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(s_dp.container, 0, 0);
    /* Block clicks from passing to the sheet backdrop behind */
    lv_obj_add_flag(s_dp.container, LV_OBJ_FLAG_CLICKABLE);

    /* ── Handle zone ──────────────────────────────────────────────────────── */
    lv_obj_t *handle_zone = lv_obj_create(s_dp.container);
    lv_obj_set_size(handle_zone, SCREEN_W, DP_HANDLE_ZONE_H);
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
        lv_obj_set_size(bar, DP_HANDLE_BAR_W, DP_HANDLE_BAR_H);
        lv_obj_center(bar);
        lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BORDER), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, DP_HANDLE_BAR_R, 0);
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* ── Header row: < [Month Year btn] > ────────────────────────────────── */
    lv_obj_t *hdr = lv_obj_create(s_dp.container);
    lv_obj_set_size(hdr, SCREEN_W, DP_HEADER_H);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_style_pad_ver(hdr, 0, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(hdr, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(hdr, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);

    lv_obj_t *btn_prev = lv_obj_create(hdr);
    lv_obj_set_size(btn_prev, 40, 40);
    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_prev, 0, 0);
    lv_obj_set_style_pad_all(btn_prev, 0, 0);
    lv_obj_remove_flag(btn_prev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_prev, _prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, "<");
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_prev, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_prev);

    /* Centre button — shows Month Year; click drills up */
    s_dp.hdr_btn = lv_obj_create(hdr);
    lv_obj_set_height(s_dp.hdr_btn, 40);
    lv_obj_set_flex_grow(s_dp.hdr_btn, 1);
    lv_obj_set_style_bg_opa(s_dp.hdr_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dp.hdr_btn, 0, 0);
    lv_obj_set_style_pad_all(s_dp.hdr_btn, 0, 0);
    lv_obj_remove_flag(s_dp.hdr_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_dp.hdr_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dp.hdr_btn, _hdr_btn_cb, LV_EVENT_CLICKED, NULL);
    s_dp.hdr_lbl = lv_label_create(s_dp.hdr_btn);
    lv_obj_set_style_text_font(s_dp.hdr_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_dp.hdr_lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_width(s_dp.hdr_lbl, LV_PCT(100));
    lv_obj_set_style_text_align(s_dp.hdr_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_dp.hdr_lbl);

    lv_obj_t *btn_next = lv_obj_create(hdr);
    lv_obj_set_size(btn_next, 40, 40);
    lv_obj_set_style_bg_opa(btn_next, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_next, 0, 0);
    lv_obj_set_style_pad_all(btn_next, 0, 0);
    lv_obj_remove_flag(btn_next, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_next, _next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, ">");
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_next, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_next);

    /* ── View area ────────────────────────────────────────────────────────── */
    s_dp.view_area = lv_obj_create(s_dp.container);
    lv_obj_set_size(s_dp.view_area, SCREEN_W, DP_VIEW_AREA_H);
    lv_obj_set_style_bg_opa(s_dp.view_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_dp.view_area, 0, 0);
    lv_obj_set_style_pad_hor(s_dp.view_area, 16, 0);
    lv_obj_set_style_pad_ver(s_dp.view_area, 0, 0);
    lv_obj_remove_flag(s_dp.view_area, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Bottom padding ───────────────────────────────────────────────────── */
    lv_obj_t *bot = lv_obj_create(s_dp.container);
    lv_obj_set_size(bot, SCREEN_W, DP_BOT_PAD);
    lv_obj_set_style_bg_opa(bot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_remove_flag(bot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Build initial day view and update the header label */
    _build_day_view();
    _update_header();

    /* Position off-screen, then animate up */
    lv_obj_set_pos(s_dp.container, 0, SCREEN_H);
    s_dp.open_y = SCREEN_H - DP_PICKER_H;
    sheet_animate_y(s_dp.container, SCREEN_H, s_dp.open_y, DP_ANIM_OPEN_MS, lv_anim_path_ease_out, NULL);

    /* Lift the parent sheet if provided */
    if (parent_container) {
        sheet_lift(DP_PICKER_H);
    }

    LOG_D(TAG, "Date picker open — initial %d/%d/%d",
          initial_date.tm_mday, initial_date.tm_mon + 1,
          initial_date.tm_year + 1900);
}

void date_picker_close(void)
{
    if (!s_dp.is_open) return;
    s_dp.is_open = false;
    _close_internal(true);
}

void date_picker_close_silent(void)
{
    if (!s_dp.is_open) return;
    s_dp.is_open = false;
    _close_internal(false);
}
