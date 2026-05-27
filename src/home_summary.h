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

/* ── Layout constants for the summary (top) section ──────────────────────── */
#define HOME_CHIP_GAP  18   /* horizontal gap between top-row condition chips (px) */

/* Build the top section of the Home tab — clock row + condition chips +
   indoor cards. Sets *out_lbl_time to the clock label so the caller can
   pass it to clock_service_init. Mount now_playing_init() yourself after
   this function returns; this module does not call it. */
void home_summary_init(lv_obj_t *parent, lv_obj_t **out_lbl_time);

/* Register this module's HA subscriptions. Called by home_ha_bridge at
   init time. Subscribes to: weather, pollen, heating, kitchen temp,
   kitchen humidity, condition icon. */
void home_summary_register_subscriptions(void);
