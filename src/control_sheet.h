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

/* ── Layout constants ─────────────────────────────────────────────────────── */

#define CTRL_DRAWER_MAX_H      420   /* max drawer height (px)              */
#define CTRL_HINT_PILL_W        32   /* gesture hint pill width (px)        */
#define CTRL_HINT_PILL_H         3   /* gesture hint pill height (px)       */
#define CTRL_SLEEP_TIMEOUT_S    30   /* display sleep timeout (seconds)     */

/* Content init function for the Control Sheet.
   Pass to sheet_open_top() — builds the full Admin Drawer. */
void control_sheet_init(lv_obj_t *parent);

/* Create the persistent swipe-hint pill flush against the bottom of the
   nav bar.  Call from shell_init() after the nav bar is placed.
   nav_bottom_y is the y coordinate of the nav bar's bottom edge. */
void control_sheet_create_hint_pill(lv_obj_t *screen, int32_t nav_bottom_y);
