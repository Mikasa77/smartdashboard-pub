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

/* Attach the time label and start the 1-second tick.
 * lbl_time: updated as "HH:MM"
 * On Firmware, call after esp_sntp_init() has synced the system clock. */
void clock_service_init(lv_obj_t *lbl_time);

/* Register a callback invoked (from the LVGL timer task) when the calendar
 * date changes — day, month, or year. Includes the 1970→real-time jump
 * after SNTP sync. Safe to call lv_async_call() or LVGL APIs from this
 * callback. */
typedef void (*clock_day_changed_cb_t)(void);
void clock_service_set_day_changed_cb(clock_day_changed_cb_t cb);
