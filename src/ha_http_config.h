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

/* Start the HA web config HTTP server on port 80.
 * Firmware only — call inside #ifdef ESP_PLATFORM at the call site.
 *
 * GET  /config  — HTML form pre-populated from ha_credentials.
 * POST /config  — validates fields, writes NVS via ha_credentials_set(),
 *                 triggers WebSocket reconnect, returns confirmation page. */
void ha_http_config_start(void);
