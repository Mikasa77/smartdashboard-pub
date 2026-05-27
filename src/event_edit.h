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
#include "calendar_service.h"

/* ── Layout constants ─────────────────────────────────────────────────────── */

#define EE_PAD_H        32    /* left/right inset inside the sheet content */
#define EE_PAD_T        24    /* top padding */
#define EE_PAD_B        32    /* bottom padding */
#define EE_ROW_GAP      12    /* vertical gap between form rows */
#define EE_FIELD_H      48    /* height of each form field (date/time/title) */
#define EE_FIELD_R      10    /* field border radius */
#define EE_FIELD_PAD_H  14    /* horizontal padding inside a field */
#define EE_SEG_H        44    /* repeat segmented-control height */
#define EE_SEG_R         8    /* segment button corner radius */
#define EE_SECTION_GAP  20    /* extra gap before the Save button */
#define EE_SAVE_H       56    /* save button height */
#define EE_SAVE_R       12    /* save button corner radius */

/* Open the Edit Sheet in create mode for the currently-selected day.
   The keyboard opens automatically with the sheet. */
void event_edit_open_create(int year, int month_1, int day);

/* Open the Edit Sheet in edit mode, pre-filling all fields from ev.
   Keyboard opens only when the title field is tapped. */
void event_edit_open_edit(const cal_event_t *ev);
