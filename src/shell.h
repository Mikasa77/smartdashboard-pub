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

/* Initialise the Shell: builds the Nav Bar from the tab registry and
   creates a content container for each registered tab.
   Call after all tab_register() calls, before the render loop. */
void shell_init(void);

/* Programmatically activate a tab by index (0-based, matches tab_register order).
   No-op if idx is out of range. */
void shell_activate_tab(int idx);
