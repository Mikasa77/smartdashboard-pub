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

/* ── Tunable layout constants ─────────────────────────────────────────────── */

#define KB_HANDLE_ZONE_H   5      /* height of the drag-handle strip */
#define KB_HANDLE_BAR_W    40      /* width of the visual handle pip */
#define KB_HANDLE_BAR_H     4      /* thickness of the handle pip */
#define KB_HANDLE_BAR_R     2      /* corner radius of the handle pip */

#define KB_H              260      /* keyboard widget height (4 rows + space bar) */
#define KB_BOT_PAD         5      /* space below keyboard inside sheet */
#define KB_SHEET_H        260

#define KB_ANIM_OPEN_MS   360
#define KB_ANIM_CLOSE_MS  300
#define KB_ANIM_SNAP_MS   200
#define KB_DISMISS_PX     120
#define KB_RESIST_FACTOR  0.15f
#define KB_CAPS_DOUBLE_TAP_MS  400   /* max ms between two shift presses to latch caps */

/* ── API ──────────────────────────────────────────────────────────────────── */

typedef void (*keyboard_close_cb_t)(void *user_data);

bool keyboard_is_open(void);

/* Slide a keyboard sheet up from the bottom. The caller owns textarea and is
   responsible for creating, positioning, and reading it; this module only
   attaches lv_keyboard_set_textarea() to it.
   parent_container — sheet to lift (NULL in debug/standalone contexts).
   on_close — fired by keyboard_close(); NOT fired by keyboard_close_silent(). */
void keyboard_open(lv_obj_t *parent_container,
                   lv_obj_t *textarea,
                   keyboard_close_cb_t on_close,
                   void *user_data);

/* Animated dismiss; fires on_close if set. */
void keyboard_close(void);

/* Immediate destroy, no animation, no callback — for same-sheet transitions. */
void keyboard_close_silent(void);
