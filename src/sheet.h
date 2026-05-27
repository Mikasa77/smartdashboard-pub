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

typedef void (*sheet_content_init_fn_t)(lv_obj_t *parent);
typedef void (*sheet_close_cb_t)(void *user_data);

/* Slide a sheet up from the bottom of the screen.
   Ignored if a sheet is already visible. */
void sheet_open(sheet_content_init_fn_t content_init_fn);

/* Slide a sheet down from the top of the screen.
   Handle zone is at the bottom; drag UP past 120 px to dismiss.
   Ignored if a sheet is already visible. */
void sheet_open_top(sheet_content_init_fn_t content_init_fn);

/* Programmatic dismiss — runs the 300 ms spring-out close animation. */
void sheet_close(void);

/* Lift/lower the current sheet to make room for a sub-picker below it.
   Callers (date_picker, time_picker) call lift on open and lower on close.
   No-ops when no sheet is open. */
void sheet_lift(int32_t by_px);
void sheet_lower(int32_t by_px);

/* Returns the sheet container object, or NULL if no sheet is open.
   Do not cache across open/close cycles. */
lv_obj_t *sheet_get_container(void);

/* Register a callback fired once when the sheet begins closing (before animation).
   Use this to clean up sub-panels or clear widget pointers.
   The callback is cleared after it fires. */
void sheet_set_close_cb(sheet_close_cb_t cb, void *user_data);

/* Shared y-position animation helper used by sub-panels (keyboard, pickers).
   Cancels any running animation on obj before starting the new one. */
void sheet_animate_y(lv_obj_t *obj, int32_t from_y, int32_t to_y,
                     uint32_t duration_ms, lv_anim_path_cb_t path_cb,
                     lv_anim_ready_cb_t ready_cb);

/* Create a transparent full-screen click-catcher on lv_layer_top().
   Used by sub-panels that need to dismiss on outside tap.
   clicked_cb is called with LV_EVENT_CLICKED. */
lv_obj_t *sheet_make_backdrop(lv_event_cb_t clicked_cb);
