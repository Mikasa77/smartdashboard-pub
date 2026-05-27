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
#include <time.h>
#include <stdbool.h>

/* ── Tunable layout constants ─────────────────────────────────────────────── */

#define DP_HANDLE_ZONE_H   36
#define DP_HANDLE_BAR_W    40
#define DP_HANDLE_BAR_H     4
#define DP_HANDLE_BAR_R     2

#define DP_HEADER_H        52
#define DP_VIEW_AREA_H    396   /* same as CAL_GRID_H in tab_home */
#define DP_BOT_PAD         24
#define DP_PICKER_H       (DP_HANDLE_ZONE_H + DP_HEADER_H + DP_VIEW_AREA_H + DP_BOT_PAD)  /* 508 */

#define DP_CELL_R           8   /* day cell corner radius */
#define DP_CELL_BORDER_W    2   /* amber border for today */

#define DP_ANIM_OPEN_MS   360
#define DP_ANIM_CLOSE_MS  300
#define DP_ANIM_SNAP_MS   200
#define DP_DISMISS_PX     120
#define DP_RESIST_FACTOR  0.15f

/* ── API ──────────────────────────────────────────────────────────────────── */

typedef void (*date_picker_select_cb_t)(struct tm date, void *user_data);

bool date_picker_is_open(void);

/* Slide a calendar picker up below parent_container (may be NULL in debug context).
   Fires on_select with the chosen date and closes.  Only one picker open at a time. */
void date_picker_open(lv_obj_t *parent_container, struct tm initial_date,
                      date_picker_select_cb_t on_select, void *user_data);

/* Animated close (spring-out). */
void date_picker_close(void);

/* Immediate destroy, no animation — for same-sheet picker swaps. */
void date_picker_close_silent(void);
