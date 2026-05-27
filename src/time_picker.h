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

#define TP_HANDLE_ZONE_H   36
#define TP_HANDLE_BAR_W    40
#define TP_HANDLE_BAR_H     4
#define TP_HANDLE_BAR_R     2

#define TP_TITLE_H         52
#define TP_COL_LABEL_H     28
#define TP_ITEM_H          64
#define TP_VISIBLE_ITEMS    5
#define TP_ROLLER_H       (TP_VISIBLE_ITEMS * TP_ITEM_H)               /* 320 */
#define TP_PAD_ENDS       ((TP_ROLLER_H - TP_ITEM_H) / 2)              /* 128 */
#define TP_FADE_H         (TP_ITEM_H * 2)                              /* 128 */
#define TP_PICKER_H       (TP_HANDLE_ZONE_H + TP_TITLE_H + TP_COL_LABEL_H + TP_ROLLER_H)
/* 36+52+28+320 = 436 */

#define TP_HOUR_COUNT      24
#define TP_HOUR_SETS        3
#define TP_HOUR_TOTAL     (TP_HOUR_COUNT * TP_HOUR_SETS)   /* 72 */

#define TP_MIN_COUNT       12   /* 00,05,10,...,55 */
#define TP_MIN_SETS         3
#define TP_MIN_TOTAL      (TP_MIN_COUNT * TP_MIN_SETS)     /* 36 */

#define TP_ANIM_OPEN_MS   360
#define TP_ANIM_CLOSE_MS  300

/* ── API ──────────────────────────────────────────────────────────────────── */

typedef void (*time_picker_select_cb_t)(int hour, int minute, void *user_data);

bool time_picker_is_open(void);

/* Returns the hour/minute currently shown in the roller (valid while is_open). */
void time_picker_get_current(int *hour, int *minute);

/* Slide a drum-roller time picker up below parent_container (NULL = sub-panel mode). */
void time_picker_open(lv_obj_t *parent_container, int initial_hour, int initial_minute,
                      time_picker_select_cb_t on_select, void *user_data);

/* Animated close. */
void time_picker_close(void);

/* Immediate destroy, no animation — for same-sheet picker swaps. */
void time_picker_close_silent(void);
