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

#pragma once
#include "lvgl.h"
#include <stdbool.h>

/* ── Scrollbar ────────────────────────────────────────────────────────────── */
#define HEP_SCROLLBAR_W       12
#define HEP_SCROLLBAR_PAD     (HEP_SCROLLBAR_W + 12)
#define HEP_SCROLLBAR_RADIUS  (HEP_SCROLLBAR_W / 2)

/* ── Row geometry ─────────────────────────────────────────────────────────── */
#define HEP_ROW_H        56
#define HEP_ROW_GAP      10
#define HEP_PANEL_ROWS    4
#define HEP_PANEL_H      (HEP_ROW_H * HEP_PANEL_ROWS + HEP_ROW_GAP * (HEP_PANEL_ROWS - 1))
#define HEP_ROW_RADIUS   18

/* ── Column widths ────────────────────────────────────────────────────────── */
#define HEP_CONTENT_W   720
#define HEP_TIME_W      116
#define HEP_DEL_W       190

/* ── Label offsets ────────────────────────────────────────────────────────── */
#define HEP_TIME_PAD_X    20
#define HEP_TIME_PAD_Y     0
#define HEP_TITLE_PAD_X   18
#define HEP_TITLE_PAD_Y    0

/* ── Edit panel ───────────────────────────────────────────────────────────── */
#define HEP_EDIT_ICON_X    6
#define HEP_EDIT_ICON_Y    0
#define HEP_EDIT_LBL_X    36
#define HEP_EDIT_LBL_Y     0

/* ── Delete panel ─────────────────────────────────────────────────────────── */
#define HEP_DEL_LBL_PAD_R 18
#define HEP_DEL_ICON_GAP   6
#define HEP_DEL_ICON_X    90
#define HEP_DEL_ICON_Y     0

/* ── Swipe / reveal ───────────────────────────────────────────────────────── */
#define HEP_RIGHT_THR    60
#define HEP_LEFT_THR     60
#define HEP_EDIT_REVEAL 110
#define HEP_DEL_REVEAL  115

/* ── Shadow / bounce / button states ──────────────────────────────────────── */
#define HEP_SHADOW_WIDTH     18
#define HEP_SHADOW_OPA      180
#define HEP_SHADOW_OFFSET_X   0
#define HEP_BOUNCE_PX        12
#define HEP_BOUNCE_MS       200
#define HEP_BTN_PRESSED_OPA  50

/* ── Heading gap ──────────────────────────────────────────────────────────── */
#define HEP_HDR_GAP  16   /* gap between date heading and event list (px) */

/* ── Unified display item (HA + local events merged for rendering) ───────── */

typedef struct {
    char time_str[12];
    char title[128];
    char uid[128];    /* HA event UID; empty for local events */
    char date[11];    /* "YYYY-MM-DD" */
    bool is_past;
    bool all_day;
    int  sort_min;    /* minutes since midnight; -1 for all-day */
    int  event_id;    /* >0 = local cal_service id; -1 = HA event */
    int  hour;        /* -1 = all-day */
    int  minute;
} ev_item_t;

/* Build the events section (heading + scrollable list). Caller owns the
   parent layout — typically a vertical-flex column. */
void home_events_panel_init(lv_obj_t *parent);

/* Replace the panel content with items[]. The panel sorts items[] in-place
   and renders; it does not own the storage. Items are then copied into
   per-row state. */
void home_events_panel_show_day(int year, int mon_1, int day,
                                 ev_item_t *items, int count);

/* Update the date heading ("Today" / "Tuesday 21 May" / etc.) without
   touching the list. */
void home_events_panel_set_heading(int year, int mon_1, int day);

/* Spring closed any row currently showing the edit or delete reveal.
   Called by home_calendar on idle return and on cell-tap. */
void home_events_panel_close_all_reveals(void);
