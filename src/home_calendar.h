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
#include "ha_ws_client.h"   /* for ha_calendar_event_t */
#include <stdbool.h>

/* ── Layout constants ─────────────────────────────────────────────────────── */
#define HC_GRID_H             396   /* grid height: header(36) + 6×cell(56) + 6×gap(4) */
#define HC_NAV_SLIDE_PX       720
#define HC_NAV_OUT_MS         180
#define HC_NAV_IN_MS          220
#define HC_SWIPE_COMMIT_PX     40
#define HC_IDLE_RETURN_MS    8000   /* ms of inactivity before view returns to today */
#define HC_FETCH_BACK_MONTHS    1
#define HC_FETCH_FORWARD_MONTHS 3

/* Callback fired when the user selects a different day in the calendar
   (cell tap, swipe-nav, idle return). home_ha_bridge subscribes to this
   to push the relevant events into home_events_panel. */
typedef void (*home_calendar_selection_cb_t)(int year, int mon_1, int day);

/* Build the calendar card under parent. Calls home_calendar_set_today
   internally using the OS clock — bridge can override later. */
void home_calendar_init(lv_obj_t *parent);

/* Update "today" — call after day-rollover so the amber fill moves and
   past/future styling stays correct. */
void home_calendar_set_today(int year, int mon_1, int day);

/* Push the current month-window event list. Updates dot indicators on
   the grid. The pointer is borrowed and must remain valid for the
   lifetime of the panel. home_ha_bridge owns the backing storage as a
   static array, so this contract is satisfied for the program lifetime. */
void home_calendar_set_events(const ha_calendar_event_t *events, int count);

/* Register the selection-changed callback (single subscriber). */
void home_calendar_set_selection_changed_cb(home_calendar_selection_cb_t cb);

/* Returns the currently-selected year, mon_1, day via out params. */
void home_calendar_get_selected(int *year_out, int *mon_1_out, int *day_out);

/* Returns the currently-viewed month (may differ from selection during nav). */
void home_calendar_get_viewed_month(int *year_out, int *mon_0_out);

/* Returns "today" — used by bridge to recompute fetch window after rollover. */
void home_calendar_get_today(int *year_out, int *mon_1_out, int *day_out);

/* ── Idle-timer bridge ─────────────────────────────────────────────────────
   Called from home_events_panel.c to interact with the idle-return timer
   that lives in this module. */
void home_calendar_idle_reset(void);
void home_calendar_idle_pause(void);
int  home_calendar_idle_active(void);
