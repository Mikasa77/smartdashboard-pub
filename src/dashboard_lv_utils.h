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

/* ── Shared layout constant ───────────────────────────────────────────────── */
#define HOME_CARD_RADIUS  12   /* card border-radius used by obj_card (px)     */

/* Common LVGL object style helpers used across multiple home-tab modules. */

/* Strip all default styling: transparent bg, no border, no radius, no padding,
   no scrolling. Useful for layout containers that hold styled children. */
void obj_clear(lv_obj_t *o);

/* Apply the canonical "card" surface style: filled with COL_SURFACE,
   rounded with HOME_CARD_RADIUS, no border, no padding. */
void obj_card(lv_obj_t *o);
