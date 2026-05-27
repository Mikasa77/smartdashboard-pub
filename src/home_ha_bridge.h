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

/* Initialise all home-tab HA subscriptions and the calendar fetch path.
   Call after the widget modules have been built (home_summary_init,
   home_events_panel_init, home_calendar_init) so they can receive data
   immediately on subscribe. */
void home_ha_bridge_init(void);

/* Trigger an immediate re-fetch of the current month-window from HA or
   Google Calendar. Called by event_edit.c after a successful create/update,
   by home_events_panel after a delete, and internally on day-rollover. */
void home_ha_bridge_refresh_calendar(void);
