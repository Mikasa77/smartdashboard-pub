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
#include <stdint.h>
#include <stdbool.h>

/* NVS namespace for credentials: "dashboard" (same as dashboard_config).
 * Key constants come from ha_config.h (HA_NVS_KEY_HOST, _PORT, _TOKEN). */

/* Load credentials from NVS (Firmware) or ha_config.h compile-time
 * defaults (Simulator). Call once in app_main before ha_ws_client_start(). */
void        ha_credentials_init(void);

const char *ha_credentials_get_host(void);
uint16_t    ha_credentials_get_port(void);
const char *ha_credentials_get_token(void);

/* Write new credentials to NVS and update the runtime cache.
 * Calls ha_ws_client_reconnect() so the new credentials are picked up
 * immediately without restarting the device. */
void        ha_credentials_set(const char *host, uint16_t port,
                                const char *token);
