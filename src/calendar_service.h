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
#include <stdbool.h>
#include "ha_ws_client.h"   /* for ha_calendar_event_t */

#define CAL_SVC_MAX  128   /* max locally-created events */

typedef enum {
    CAL_REPEAT_NONE    = 0,
    CAL_REPEAT_DAILY   = 1,
    CAL_REPEAT_WEEKLY  = 2,
    CAL_REPEAT_MONTHLY = 3,
    CAL_REPEAT_YEARLY  = 4,
} cal_repeat_t;

typedef struct {
    int          id;          /* 0 = empty slot; assigned by calendar_service_add() */
    char         title[128];
    char         date[11];    /* "YYYY-MM-DD" */
    char         uid[128];    /* HA/Google event UID; empty for locally-created events */
    int          hour;        /* -1 = all-day / no time */
    int          minute;
    cal_repeat_t repeat;
} cal_event_t;

typedef void (*cal_changed_cb_t)(void *user_data);

/* Call once at startup before any other calendar_service call. */
void calendar_service_init(void);

/* Register a callback fired on the LVGL thread whenever the event store changes. */
void calendar_service_set_changed_cb(cal_changed_cb_t cb, void *user_data);

/* Add a new event. Returns the assigned id (>0), or -1 if the store is full. */
int  calendar_service_add(const cal_event_t *ev);

/* Update an existing event. ev->id must match a previously-added event. */
void calendar_service_update(const cal_event_t *ev);

/* Remove an event by id. No-op if id not found. */
void calendar_service_remove(int id);

/* Fill out[] with all events on date_str ("YYYY-MM-DD"). Returns count. */
int  calendar_service_get_for_day(const char *date_str, cal_event_t *out, int max_out);

/* Copy event by id into *out. Returns false if not found. */
bool calendar_service_get_by_id(int id, cal_event_t *out);

/* Returns true if any event exists on date_str. */
bool calendar_service_has_events_on(const char *date_str);

/* ── Shared event-on-day predicates (used by home_calendar + home_ha_bridge) ─ */

/* True if ev occurs on the local date (year, mon_1, day). Parses the start
 * field as YYYY-MM-DD for both date-only and timed events (the ISO 8601
 * prefix is consumed by sscanf). */
bool cal_event_on_day(const ha_calendar_event_t *ev,
                       int year, int mon_1, int day);

/* True if ev spans the local date — used for multi-day all-day events. */
bool cal_event_spans_day(const ha_calendar_event_t *ev,
                          int year, int mon_1, int day);
