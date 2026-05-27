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

/*
 * HA WebSocket Client — single persistent connection to Home Assistant.
 *
 * All public functions must be called from the LVGL thread.
 * All callbacks fire on the LVGL thread via lv_async_call.
 *
 * Configuration: copy src/ha_config.h.example → src/ha_config.h (git-ignored)
 * and fill in HA_HOST, HA_PORT_STR, HA_TOKEN.
 */

/* State delivered to ha_state_cb_t. Valid only during the callback. */
typedef struct {
    const char *entity_id;
    const char *state;           /* "on", "21.5", "playing", etc.           */
    const char *attributes_json; /* full attributes object as JSON string    */
} ha_state_t;

/* One calendar event delivered in ha_calendar_cb_t. */
typedef struct {
    char summary[128];
    char start[32];   /* ISO 8601: "2025-01-15T08:30:00+00:00" or "2025-01-20" */
    char end[32];
    char uid[128];    /* HA/Google event UID — empty if not provided            */
    int  all_day;     /* 1 = date-only event, 0 = has a time component         */
} ha_calendar_event_t;

typedef void (*ha_state_cb_t)(const ha_state_t *state, void *user_data);
typedef void (*ha_calendar_cb_t)(const ha_calendar_event_t *events, int count,
                                  void *user_data);

/* Start the HA WebSocket background thread (Simulator) / FreeRTOS task
   (Firmware). Call once from the LVGL thread after lv_init(). */
void ha_ws_client_start(void);

/* Disconnect and reconnect using the current ha_credentials values.
   Call after ha_credentials_set() to pick up new host/port/token. */
void ha_ws_client_reconnect(void);

/* Register a callback for entity_id. Fires on the LVGL thread each time
   the entity changes state. Registration survives reconnects. */
void ha_subscribe(const char *entity_id, ha_state_cb_t cb, void *user_data);

/* Fire-and-forget HA service call. data_json is the service_data JSON object
   string, or NULL for no data. */
void ha_call_service(const char *domain, const char *service,
                     const char *data_json);

/* Last connection error as a short human-readable string, e.g.
 * "Cannot resolve hostname". Empty string when connected and authenticated. */
const char *ha_ws_get_conn_error(void);

/* Fetch calendar events for entity_id in [start_iso, end_iso).
   cb fires once on the LVGL thread (count=0 on error or empty range). */
void ha_get_calendar_events(const char *entity_id,
                             const char *start_iso,
                             const char *end_iso,
                             ha_calendar_cb_t cb,
                             void *user_data);
