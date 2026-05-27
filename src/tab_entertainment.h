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

/* Tab init — registered with tab_register() in main.c / app_main.c. */
void tab_entertainment_init(lv_obj_t *parent);

/* Active media player entity for this tab and the Home now-playing widget.
   Defaults to HA_ENTITY_MEDIA_SONOS on init. */
const char *tab_entertainment_get_active_entity(void);
void        tab_entertainment_set_active_entity(const char *entity_id);
