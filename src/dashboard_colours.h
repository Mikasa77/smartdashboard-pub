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

/* Canonical Dark Mode palette — source of truth: Claude Design React prototype.
   Legacy ha_dashboard values are superseded. Light Mode palette TBD. */

#define COL_BG        0x1a1a1a   /* screen background         */
#define COL_SURFACE   0x242424   /* card surface              */
#define COL_SHEET     0x2a2a2a   /* modal sheet surface       */
#define COL_INPUT     0x1e1e1e   /* input / picker background */
#define COL_PAST_BG   0x22262a   /* past event card bg        */
#define COL_BORDER    0x333333   /* borders and inset wells   */
#define COL_ACCENT    0xf59e0b   /* amber — active / current  */
#define COL_PAST      0x5c7a8a   /* blue-grey — past / muted  */
#define COL_SCROLLBAR 0x243037   /* scrollbar thumb           */
#define COL_EDIT      0x4a90d9   /* edit action (swipe)       */
#define COL_DELETE    0xc0392b   /* delete action (swipe)     */
#define COL_TEXT      0xe5e5e5   /* body text                 */
#define COL_TEXT_DIM  0xa3a3a3   /* dim text                  */
#define COL_TEXT_MUTE 0x6b6b6b   /* muted text                */
