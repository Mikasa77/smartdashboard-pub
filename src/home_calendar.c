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

#include "home_calendar.h"
#include "home_events_panel.h"   /* for HEP close-reveals on idle */
#include "home_ha_bridge.h"      /* home_ha_bridge_refresh_calendar */
#include "dashboard_lv_utils.h"
#include "calendar_service.h"
#include "event_edit.h"
#include "dashboard_colours.h"
#include "dashboard_icons.h"
#include "dashboard_log.h"
#include "lvgl.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#endif
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "home_calendar";

#define CAL_MAX_CELLS  42    /* 6 weeks × 7 days                               */
#define CAL_DOT_D       6    /* event dot diameter (px)                         */

typedef struct {
    lv_obj_t *cell;
    lv_obj_t *lbl;   /* day number label */
    lv_obj_t *dot;   /* event indicator dot, hidden when no events             */
    int        day;  /* 1-31; 0 = empty padding cell                           */
} cal_cell_t;

typedef enum { CAL_VIEW_DAY = 0, CAL_VIEW_MONTH, CAL_VIEW_YEAR } cal_view_t;

/* ── Module state ─────────────────────────────────────────────────────────── */

static cal_cell_t  s_cal_cells[CAL_MAX_CELLS];
static int         s_cal_cell_count = 0;
static lv_obj_t   *s_cal_clip      = NULL;   /* overflow-hidden zone for grid slide */
static lv_obj_t   *s_cal_grid_obj  = NULL;   /* current grid widget               */
static lv_obj_t   *s_cal_month_lbl = NULL;   /* month/year label in header        */
static lv_timer_t *s_cal_idle_timer = NULL;  /* fires HC_IDLE_RETURN_MS after last cell tap */
static lv_obj_t   *s_cal_nav_old_grid   = NULL;  /* old grid kept alive during slide-out */
static bool        s_cal_nav_animating  = false; /* guard: ignore nav taps mid-transition */
static cal_view_t  s_cal_view     = CAL_VIEW_DAY;
static int         s_cal_yr_start = 0;   /* first year shown in year grid */

/* Calendar swipe state */
static int32_t  s_cal_swipe_start_x  = 0;
static int32_t  s_cal_swipe_start_y  = 0;
static bool     s_cal_swipe_locked   = false;
static bool     s_cal_swipe_is_x     = false;
static bool     s_cal_swipe_consumed = false; /* suppresses cell-click after a swipe */

static int s_cal_year  = 0;
static int s_cal_month = 0;   /* 0-11                                          */
static int s_sel_day   = 0;   /* selected day (1-31) in the displayed month    */
static int s_today_day = 0;
static int s_today_month = 0; /* 1-12                                          */
static int s_today_year  = 0;

/* Grid descriptor arrays */
static int32_t s_cal_col_dsc[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
    LV_GRID_TEMPLATE_LAST
};

static int32_t s_cal_row_dsc[] = {
    36,
    56, 56, 56, 56, 56, 56,
    LV_GRID_TEMPLATE_LAST
};

/* Borrowed event list — owned and updated by home_ha_bridge.
   The static array in home_ha_bridge.c has program lifetime, so the
   pointer remains valid until shutdown. */
static const ha_calendar_event_t *s_events_ref   = NULL;
static int                        s_events_count = 0;

/* Selection callback (single subscriber) */
static home_calendar_selection_cb_t s_sel_cb = NULL;

/* ── Selection callback helper ────────────────────────────────────────────── */

static void _fire_selection_changed(void)
{
    if (s_sel_cb) s_sel_cb(s_cal_year, s_cal_month + 1, s_sel_day);
}

/* ── Animation helper ─────────────────────────────────────────────────────── */

static void anim_x_exec(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, v);
}

/* ── Calendar helpers ─────────────────────────────────────────────────────── */

/* -1 = no events on this day; 0 = past day with events; 1 = today/future with events */
static int cal_event_status_for_day(int day)
{
    int month_1 = s_cal_month + 1;
    bool has_any = false;

    for (int i = 0; i < s_events_count && !has_any; i++) {
        if (cal_event_on_day(&s_events_ref[i], s_cal_year, month_1, day))
            has_any = true;
    }

    if (!has_any) {
        char date_str[11];
        snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", s_cal_year, month_1, day);
        has_any = calendar_service_has_events_on(date_str);
    }

    if (!has_any) return -1;

    bool is_past = (s_cal_year < s_today_year) ||
                   (s_cal_year == s_today_year && month_1 < s_today_month) ||
                   (s_cal_year == s_today_year && month_1 == s_today_month &&
                    day < s_today_day);
    return is_past ? 0 : 1;
}

/* Rebuild cell selection highlight and event dots for the current month view. */
static void cal_update_cells(void)
{
    int month_1 = s_cal_month + 1;
    bool is_cur_month = (s_cal_year == s_today_year && month_1 == s_today_month);

    for (int i = 0; i < s_cal_cell_count; i++) {
        cal_cell_t *cc = &s_cal_cells[i];
        if (!cc->cell || cc->day == 0) continue;

        bool is_today    = is_cur_month && cc->day == s_today_day;
        bool is_selected = (cc->day == s_sel_day);

        if (is_today && is_selected) {
            lv_obj_set_style_bg_color(cc->cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_bg_opa(cc->cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cc->cell, 8, 0);
            lv_obj_set_style_text_color(cc->lbl, lv_color_hex(COL_BG), 0);
        } else if (is_selected) {
            lv_obj_set_style_bg_color(cc->cell, lv_color_hex(COL_SURFACE), 0);
            lv_obj_set_style_bg_opa(cc->cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cc->cell, 8, 0);
            lv_obj_set_style_text_color(cc->lbl, lv_color_hex(COL_TEXT), 0);
        } else if (is_today) {
            lv_obj_set_style_bg_color(cc->cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_bg_opa(cc->cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cc->cell, 8, 0);
            lv_obj_set_style_text_color(cc->lbl, lv_color_hex(COL_BG), 0);
        } else {
            lv_obj_set_style_bg_opa(cc->cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(cc->lbl, lv_color_hex(COL_TEXT), 0);
        }

        int status = cal_event_status_for_day(cc->day);
        if (status < 0 || is_today) {
            lv_obj_add_flag(cc->dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(cc->dot, LV_OBJ_FLAG_HIDDEN);
            uint32_t col = (status == 0) ? COL_PAST : COL_ACCENT;
            lv_obj_set_style_bg_color(cc->dot, lv_color_hex(col), 0);
        }
    }
}

/* Forward declarations */
static void build_cal_grid(lv_obj_t *clip_parent);
static void build_cal_month_view(void);
static void build_cal_year_view(void);
static void _cal_update_header(void);
static void cal_hdr_btn_cb(lv_event_t *e);
static void cal_idle_reset(void);

/* Idle timer — fires once after HC_IDLE_RETURN_MS with no cell interaction. */
static void cal_idle_cb(lv_timer_t *t)
{
    (void)t;
    home_events_panel_close_all_reveals();
    LOG_D(TAG, "Calendar idle timer fired — returning to today");
    lv_timer_delete(s_cal_idle_timer);
    s_cal_idle_timer = NULL;

    /* If on a different month OR in month/year view, jump back to today day-view */
    bool month_changed = false;
    if (s_cal_year != s_today_year || s_cal_month + 1 != s_today_month
        || s_cal_view != CAL_VIEW_DAY) {
        month_changed = true;
        s_cal_year  = s_today_year;
        s_cal_month = s_today_month - 1;
        s_cal_view  = CAL_VIEW_DAY;
        _cal_update_header();

        lv_obj_clean(s_cal_clip);
        s_cal_grid_obj      = NULL;
        s_cal_cell_count    = 0;
        s_events_count      = 0;
        build_cal_grid(s_cal_clip);
    }

    s_sel_day = s_today_day;
    cal_update_cells();
    _fire_selection_changed();
    /* Re-fetch only when idle-return crossed a month boundary — if we were
       already on today's month, the cache is still valid. */
    if (month_changed) home_ha_bridge_refresh_calendar();
}

/* (Re)start the idle timer. Called on every cell tap. */
static void cal_idle_reset(void)
{
    if (s_cal_idle_timer) {
        lv_timer_reset(s_cal_idle_timer);
    } else {
        s_cal_idle_timer = lv_timer_create(cal_idle_cb, HC_IDLE_RETURN_MS, NULL);
        lv_timer_set_repeat_count(s_cal_idle_timer, 1);
    }
}

/* ── Idle-timer bridge: called from home_events_panel ────────────────────── */

void home_calendar_idle_reset(void)
{
    if (s_cal_idle_timer) cal_idle_reset();
}

void home_calendar_idle_pause(void)
{
    if (s_cal_idle_timer) { lv_timer_delete(s_cal_idle_timer); s_cal_idle_timer = NULL; }
}

int home_calendar_idle_active(void)
{
    return s_cal_idle_timer != NULL ? 1 : 0;
}

/* Click handler for each day cell. */
static void cal_cell_cb(lv_event_t *e)
{
    if (s_cal_swipe_consumed) { s_cal_swipe_consumed = false; return; }
    int day = (int)(intptr_t)lv_event_get_user_data(e);
    s_sel_day = day;
    cal_update_cells();
    _fire_selection_changed();
    if (day == s_today_day && s_cal_year == s_today_year &&
        s_cal_month + 1 == s_today_month) {
        if (s_cal_idle_timer) { lv_timer_delete(s_cal_idle_timer); s_cal_idle_timer = NULL; }
    } else {
        cal_idle_reset();
    }
}

static void _cal_update_header(void)
{
    if (!s_cal_month_lbl) return;
    char buf[32];
    if (s_cal_view == CAL_VIEW_DAY) {
        struct tm t = {0};
        t.tm_year = s_cal_year - 1900;
        t.tm_mon  = s_cal_month;
        t.tm_mday = 1;
        mktime(&t);
        strftime(buf, sizeof(buf), "%B %Y", &t);
    } else if (s_cal_view == CAL_VIEW_MONTH) {
        snprintf(buf, sizeof(buf), "%d", s_cal_year);
    } else {
        snprintf(buf, sizeof(buf), "%d - %d", s_cal_yr_start, s_cal_yr_start + 11);
    }
    lv_label_set_text(s_cal_month_lbl, buf);
}

static void _cal_month_cell_cb(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    s_cal_month = m;
    bool back_to_today = (s_cal_year == s_today_year &&
                          s_cal_month + 1 == s_today_month);
    s_sel_day           = back_to_today ? s_today_day : 1;
    s_cal_view          = CAL_VIEW_DAY;
    s_events_count      = 0;
    s_cal_cell_count    = 0;
    lv_obj_clean(s_cal_clip);
    s_cal_grid_obj = NULL;
    build_cal_grid(s_cal_clip);
    _cal_update_header();
    _fire_selection_changed();
    home_ha_bridge_refresh_calendar();
    if (!back_to_today) cal_idle_reset();
}

static void build_cal_month_view(void)
{
    if (!s_cal_clip) return;
    lv_obj_clean(s_cal_clip);
    s_cal_grid_obj   = NULL;
    s_cal_cell_count = 0;
    s_cal_view       = CAL_VIEW_MONTH;

    static const char *months[] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    static int32_t col4_dsc[5] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };
    static int32_t row3_dsc[4] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };

    lv_obj_t *grid = lv_obj_create(s_cal_clip);
    lv_obj_set_size(grid, LV_PCT(100), HC_GRID_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_style_grid_column_dsc_array(grid, col4_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(grid, row3_dsc, 0);

    for (int m = 0; m < 12; m++) {
        bool is_sel = (m == s_cal_month);

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(cell,
            lv_color_hex(is_sel ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 8, 0);

        lv_obj_t *lbl = lv_label_create(cell);
        lv_label_set_text(lbl, months[m]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(is_sel ? COL_BG : COL_TEXT), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(cell, _cal_month_cell_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)m);
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_STRETCH, m % 4, 1,
            LV_GRID_ALIGN_STRETCH, m / 4, 1);
    }
}

static void _cal_year_cell_cb(lv_event_t *e)
{
    int y = (int)(intptr_t)lv_event_get_user_data(e);
    s_cal_year = y;
    build_cal_month_view();
    _cal_update_header();
}

static void build_cal_year_view(void)
{
    if (!s_cal_clip) return;
    lv_obj_clean(s_cal_clip);
    s_cal_grid_obj   = NULL;
    s_cal_cell_count = 0;
    s_cal_view       = CAL_VIEW_YEAR;

    static int32_t col3_dsc[4] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };
    static int32_t row4_dsc[5] = {
        LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1),
        LV_GRID_TEMPLATE_LAST
    };

    lv_obj_t *grid = lv_obj_create(s_cal_clip);
    lv_obj_set_size(grid, LV_PCT(100), HC_GRID_H);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 8, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_style_grid_column_dsc_array(grid, col3_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(grid, row4_dsc, 0);

    for (int i = 0; i < 12; i++) {
        int y      = s_cal_yr_start + i;
        bool is_sel = (y == s_cal_year);

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(cell,
            lv_color_hex(is_sel ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 8, 0);

        lv_obj_t *lbl = lv_label_create(cell);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", y);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(is_sel ? COL_BG : COL_TEXT), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(cell, _cal_year_cell_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)y);
        lv_obj_set_grid_cell(cell,
            LV_GRID_ALIGN_STRETCH, i % 3, 1,
            LV_GRID_ALIGN_STRETCH, i / 3, 1);
    }
}

static void cal_nav_out_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_cal_nav_old_grid) { lv_obj_delete(s_cal_nav_old_grid); s_cal_nav_old_grid = NULL; }
}

static void cal_nav_in_done_cb(lv_anim_t *a)
{
    (void)a;
    s_cal_nav_animating = false;
}

static void cal_nav(int delta)
{
    if (s_cal_nav_animating) return;
    s_cal_nav_old_grid  = s_cal_grid_obj;
    s_cal_nav_animating = true;

    s_cal_month += delta;
    if (s_cal_month < 0)  { s_cal_month = 11; s_cal_year--; }
    if (s_cal_month > 11) { s_cal_month = 0;  s_cal_year++; }

    bool back_to_today = (s_cal_year == s_today_year &&
                          s_cal_month + 1 == s_today_month);
    s_sel_day = back_to_today ? s_today_day : 1;

    _cal_update_header();

    /* Build new grid with empty cache so no stale dots appear */
    s_events_count   = 0;
    s_cal_cell_count = 0;
    build_cal_grid(s_cal_clip);

    /* Position new grid off-screen in the incoming direction */
    int32_t in_from = delta > 0 ? HC_NAV_SLIDE_PX : -HC_NAV_SLIDE_PX;
    lv_obj_set_x(s_cal_grid_obj, in_from);

    _fire_selection_changed();
    home_ha_bridge_refresh_calendar();
    if (!back_to_today) cal_idle_reset();

    /* Slide old grid out */
    lv_anim_t out_a;
    lv_anim_init(&out_a);
    lv_anim_set_var(&out_a, s_cal_nav_old_grid);
    lv_anim_set_values(&out_a, 0, delta > 0 ? -HC_NAV_SLIDE_PX : HC_NAV_SLIDE_PX);
    lv_anim_set_exec_cb(&out_a, anim_x_exec);
    lv_anim_set_duration(&out_a, HC_NAV_OUT_MS);
    lv_anim_set_path_cb(&out_a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&out_a, cal_nav_out_done_cb);
    lv_anim_start(&out_a);

    /* Slide new grid in simultaneously */
    lv_anim_t in_a;
    lv_anim_init(&in_a);
    lv_anim_set_var(&in_a, s_cal_grid_obj);
    lv_anim_set_values(&in_a, in_from, 0);
    lv_anim_set_exec_cb(&in_a, anim_x_exec);
    lv_anim_set_duration(&in_a, HC_NAV_IN_MS);
    lv_anim_set_path_cb(&in_a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&in_a, cal_nav_in_done_cb);
    lv_anim_start(&in_a);
}

static void cal_hdr_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cal_nav_animating) return;
    if (s_cal_view == CAL_VIEW_DAY) {
        build_cal_month_view();
    } else if (s_cal_view == CAL_VIEW_MONTH) {
        s_cal_yr_start = (s_cal_year / 12) * 12;
        build_cal_year_view();
    }
    /* CAL_VIEW_YEAR: header tap is no-op */
    _cal_update_header();
    cal_idle_reset();
}

static void cal_nav_prev_cb(lv_event_t *e)
{
    (void)e;
    if (s_cal_view == CAL_VIEW_DAY) {
        cal_nav(-1);
    } else if (!s_cal_nav_animating) {
        if (s_cal_view == CAL_VIEW_MONTH) {
            s_cal_year--;
            build_cal_month_view();
        } else {
            s_cal_yr_start -= 12;
            build_cal_year_view();
        }
        _cal_update_header();
        cal_idle_reset();
    }
}

static void cal_nav_next_cb(lv_event_t *e)
{
    (void)e;
    if (s_cal_view == CAL_VIEW_DAY) {
        cal_nav(+1);
    } else if (!s_cal_nav_animating) {
        if (s_cal_view == CAL_VIEW_MONTH) {
            s_cal_year++;
            build_cal_month_view();
        } else {
            s_cal_yr_start += 12;
            build_cal_year_view();
        }
        _cal_update_header();
        cal_idle_reset();
    }
}

/* ── Calendar swipe gesture ───────────────────────────────────────────────── */

static void cal_swipe_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        s_cal_swipe_start_x  = p.x;
        s_cal_swipe_start_y  = p.y;
        s_cal_swipe_locked   = false;
        s_cal_swipe_is_x     = false;
        s_cal_swipe_consumed = false;
    }
    else if (code == LV_EVENT_PRESSING) {
        if (s_cal_swipe_locked) return;
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t ddx = p.x - s_cal_swipe_start_x;
        int32_t ddy = p.y - s_cal_swipe_start_y;
        if (LV_ABS(ddx) > 8 || LV_ABS(ddy) > 8) {
            s_cal_swipe_locked = true;
            s_cal_swipe_is_x   = LV_ABS(ddx) >= LV_ABS(ddy);
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_cal_swipe_locked || !s_cal_swipe_is_x) return;
        if (s_cal_view != CAL_VIEW_DAY) return;
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - s_cal_swipe_start_x;
        if (LV_ABS(dx) >= HC_SWIPE_COMMIT_PX && !s_cal_nav_animating) {
            s_cal_swipe_consumed = true;
            cal_nav(dx < 0 ? +1 : -1);
        }
    }
}

/* ── Calendar grid ────────────────────────────────────────────────────────── */

/* Build (or rebuild) the day cells for s_cal_year/s_cal_month into clip_parent.
   Stores cell refs in s_cal_cells[] and the grid widget in s_cal_grid_obj. */
static void build_cal_grid(lv_obj_t *clip_parent)
{
    s_cal_cell_count = 0;

    /* First day of month weekday offset (Mon=0 … Sun=6) */
    struct tm first = {0};
    first.tm_year = s_cal_year - 1900;
    first.tm_mon  = s_cal_month;
    first.tm_mday = 1;
    mktime(&first);
    int col0 = (first.tm_wday == 0) ? 6 : first.tm_wday - 1;

    /* Days in month */
    struct tm next = first;
    next.tm_mon++;
    next.tm_mday = 0;
    mktime(&next);
    int days = next.tm_mday;

    lv_obj_t *grid = lv_obj_create(clip_parent);
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);
    lv_obj_set_style_pad_column(grid, 4, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);
    lv_obj_set_style_grid_column_dsc_array(grid, s_cal_col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(grid, s_cal_row_dsc, 0);
    s_cal_grid_obj = grid;

    static const char *day_names[] = {"Mo","Tu","We","Th","Fr","Sa","Su"};
    for (int i = 0; i < 7; i++) {
        lv_obj_t *lbl = lv_label_create(grid);
        lv_label_set_text(lbl, day_names[i]);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MUTE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_grid_cell(lbl,
                             LV_GRID_ALIGN_CENTER, i, 1,
                             LV_GRID_ALIGN_CENTER, 0, 1);
    }

    bool is_cur_month = (s_cal_year == s_today_year &&
                         s_cal_month + 1 == s_today_month);

    for (int d = 1; d <= days && s_cal_cell_count < CAL_MAX_CELLS; d++) {
        int grid_idx = col0 + d - 1;
        int grow     = grid_idx / 7 + 1;
        int gcol     = grid_idx % 7;
        bool is_today = is_cur_month && (d == s_today_day);

        lv_obj_t *cell = lv_obj_create(grid);
        lv_obj_set_style_border_width(cell, 0, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);

        if (is_today) {
            lv_obj_set_style_bg_color(cell, lv_color_hex(COL_ACCENT), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(cell, 8, 0);
        } else {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        }

        /* Day number label — centred, shifted up slightly to leave dot space */
        lv_obj_t *lbl = lv_label_create(cell);
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", d);
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl,
                                    lv_color_hex(is_today ? COL_BG : COL_TEXT), 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);
        lv_obj_set_y(lbl, -4);
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        /* Event dot — small circle, bottom-centre, hidden until events known */
        lv_obj_t *dot = lv_obj_create(cell);
        lv_obj_set_size(dot, CAL_DOT_D, CAL_DOT_D);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_set_style_pad_all(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_align(dot, LV_ALIGN_BOTTOM_MID);
        lv_obj_set_y(dot, -3);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);

        lv_obj_add_event_cb(cell, cal_cell_cb,       LV_EVENT_CLICKED,    (void *)(intptr_t)d);
        lv_obj_add_event_cb(cell, cal_swipe_event_cb, LV_EVENT_PRESSED,    NULL);
        lv_obj_add_event_cb(cell, cal_swipe_event_cb, LV_EVENT_PRESSING,   NULL);
        lv_obj_add_event_cb(cell, cal_swipe_event_cb, LV_EVENT_RELEASED,   NULL);
        lv_obj_add_event_cb(cell, cal_swipe_event_cb, LV_EVENT_PRESS_LOST, NULL);

        lv_obj_set_grid_cell(cell,
                             LV_GRID_ALIGN_STRETCH, gcol, 1,
                             LV_GRID_ALIGN_STRETCH, grow, 1);

        s_cal_cells[s_cal_cell_count].cell = cell;
        s_cal_cells[s_cal_cell_count].lbl  = lbl;
        s_cal_cells[s_cal_cell_count].dot  = dot;
        s_cal_cells[s_cal_cell_count].day  = d;
        s_cal_cell_count++;
    }

    /* Apply selection + dot state immediately so today/selected cells render
       correctly before the HA calendar response arrives. */
    cal_update_cells();
}

/* ── btn_add: open Edit Sheet for the currently-selected day ─────────────── */

static void _on_add_event_cb(lv_event_t *e)
{
    (void)e;
    /* Pause idle timer while the edit sheet is open */
    if (s_cal_idle_timer) { lv_timer_delete(s_cal_idle_timer); s_cal_idle_timer = NULL; }
    event_edit_open_create(s_cal_year, s_cal_month + 1, s_sel_day);
}

/* Build the card shell (header + nav buttons) then populate the day grid. */
static void build_calendar(lv_obj_t *parent)
{
    struct tm t = {0};
    t.tm_year = s_cal_year - 1900;
    t.tm_mon  = s_cal_month;
    t.tm_mday = 1;
    mktime(&t);
    char month_str[32];
    strftime(month_str, sizeof(month_str), "%B %Y", &t);

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    obj_card(card);
    lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);  /* cells use COL_SURFACE; card must be transparent so they contrast against COL_BG */
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    /* Header: < Month Year > | + */
    lv_obj_t *hdr = lv_obj_create(card);
    lv_obj_set_size(hdr, LV_PCT(100), 44);
    obj_clear(hdr);
    lv_obj_set_layout(hdr, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(hdr, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(hdr, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(hdr, 8, 0);

    lv_obj_t *btn_prev = lv_obj_create(hdr);
    lv_obj_set_size(btn_prev, 32, 32);
    obj_clear(btn_prev);
    lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_prev, cal_nav_prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, "<");
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_prev, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_prev);

    lv_obj_t *title_wrap = lv_obj_create(hdr);
    lv_obj_set_height(title_wrap, LV_SIZE_CONTENT);
    obj_clear(title_wrap);
    lv_obj_set_flex_grow(title_wrap, 1);
    lv_obj_add_flag(title_wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(title_wrap, cal_hdr_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_month = lv_label_create(title_wrap);
    lv_label_set_text(lbl_month, month_str);
    lv_obj_set_style_text_font(lbl_month, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_month, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_width(lbl_month, LV_PCT(100));
    lv_obj_set_style_text_align(lbl_month, LV_TEXT_ALIGN_CENTER, 0);
    s_cal_month_lbl = lbl_month;

    lv_obj_t *btn_next = lv_obj_create(hdr);
    lv_obj_set_size(btn_next, 32, 32);
    obj_clear(btn_next);
    lv_obj_add_flag(btn_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_next, cal_nav_next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, ">");
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(lbl_next, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_next);

    lv_obj_t *btn_add = lv_obj_create(hdr);
    lv_obj_set_size(btn_add, 40, 40);
    lv_obj_set_style_bg_color(btn_add, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(btn_add, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_add, 0, 0);
    lv_obj_set_style_radius(btn_add, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(btn_add, 0, 0);
    lv_obj_remove_flag(btn_add, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_add, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_add, _on_add_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_add = lv_label_create(btn_add);
    lv_label_set_text(lbl_add, "+");
    lv_obj_set_style_text_color(lbl_add, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(lbl_add, &lv_font_montserrat_18, 0);
    lv_obj_center(lbl_add);

    /* Clip zone — fixed height, overflow hidden; grids slide inside it */
    lv_obj_t *clip = lv_obj_create(card);
    lv_obj_set_size(clip, LV_PCT(100), HC_GRID_H);
    lv_obj_set_style_bg_opa(clip, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clip, 0, 0);
    lv_obj_set_style_pad_all(clip, 0, 0);
    lv_obj_remove_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
    /* Children are clipped to parent bounds by default in LVGL 9 — no extra flag needed */
    s_cal_clip = clip;

    build_cal_grid(s_cal_clip);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void home_calendar_init(lv_obj_t *parent)
{
    /* Initial today from OS clock — bridge will override via set_today
       after SNTP sync on firmware. */
    time_t now = time(NULL);
    struct tm t = *localtime(&now);
    s_today_day   = t.tm_mday;
    s_today_month = t.tm_mon + 1;
    s_today_year  = t.tm_year + 1900;
    s_cal_year    = s_today_year;
    s_cal_month   = t.tm_mon;
    s_sel_day     = s_today_day;

    build_calendar(parent);
}

void home_calendar_set_today(int year, int mon_1, int day)
{
    bool was_on_today = (s_cal_year      == s_today_year  &&
                         s_cal_month + 1 == s_today_month);
    bool date_jumped  = (year != s_today_year ||
                         mon_1 != s_today_month ||
                         day != s_today_day);

    s_today_year  = year;
    s_today_month = mon_1;
    s_today_day   = day;

    if (was_on_today && date_jumped) {
        s_cal_year  = year;
        s_cal_month = mon_1 - 1;
        s_sel_day   = day;
        s_cal_view  = CAL_VIEW_DAY;
        _cal_update_header();
        if (s_cal_clip && !s_cal_nav_animating) {
            lv_obj_clean(s_cal_clip);
            s_cal_grid_obj   = NULL;
            s_cal_cell_count = 0;
            build_cal_grid(s_cal_clip);
        }
        _fire_selection_changed();
    }
}

void home_calendar_set_events(const ha_calendar_event_t *events, int count)
{
    s_events_ref   = events;
    s_events_count = count;
    cal_update_cells();
}

void home_calendar_set_selection_changed_cb(home_calendar_selection_cb_t cb)
{
    s_sel_cb = cb;
}

void home_calendar_get_selected(int *year_out, int *mon_1_out, int *day_out)
{
    if (year_out)  *year_out  = s_cal_year;
    if (mon_1_out) *mon_1_out = s_cal_month + 1;
    if (day_out)   *day_out   = s_sel_day;
}

void home_calendar_get_viewed_month(int *year_out, int *mon_0_out)
{
    if (year_out)  *year_out  = s_cal_year;
    if (mon_0_out) *mon_0_out = s_cal_month;
}

void home_calendar_get_today(int *year_out, int *mon_1_out, int *day_out)
{
    if (year_out)  *year_out  = s_today_year;
    if (mon_1_out) *mon_1_out = s_today_month;
    if (day_out)   *day_out   = s_today_day;
}
