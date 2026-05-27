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

#include "calendar_service.h"
#include "dashboard_log.h"
#include "ha_ws_client.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "cal_svc";

static cal_event_t      s_events[CAL_SVC_MAX];
static int              s_next_id       = 1;
static cal_changed_cb_t s_changed_cb    = NULL;
static void            *s_changed_ud    = NULL;

static void _fire_changed(void)
{
    if (s_changed_cb) s_changed_cb(s_changed_ud);
}

void calendar_service_init(void)
{
    memset(s_events, 0, sizeof(s_events));
    s_next_id = 1;
    LOG_I(TAG, "Initialised");
}

void calendar_service_set_changed_cb(cal_changed_cb_t cb, void *user_data)
{
    s_changed_cb = cb;
    s_changed_ud = user_data;
}

int calendar_service_add(const cal_event_t *ev)
{
    for (int i = 0; i < CAL_SVC_MAX; i++) {
        if (s_events[i].id == 0) {
            s_events[i]    = *ev;
            s_events[i].id = s_next_id++;
            LOG_I(TAG, "Add id=%d \"%s\" %s %02d:%02d repeat=%d",
                  s_events[i].id, s_events[i].title, s_events[i].date,
                  s_events[i].hour, s_events[i].minute, s_events[i].repeat);
            _fire_changed();
            return s_events[i].id;
        }
    }
    LOG_W(TAG, "Store full — add rejected");
    return -1;
}

void calendar_service_update(const cal_event_t *ev)
{
    for (int i = 0; i < CAL_SVC_MAX; i++) {
        if (s_events[i].id == ev->id) {
            s_events[i] = *ev;
            LOG_I(TAG, "Update id=%d \"%s\" %s %02d:%02d repeat=%d",
                  ev->id, ev->title, ev->date, ev->hour, ev->minute, ev->repeat);
            _fire_changed();
            return;
        }
    }
    LOG_W(TAG, "Update: id=%d not found", ev->id);
}

void calendar_service_remove(int id)
{
    for (int i = 0; i < CAL_SVC_MAX; i++) {
        if (s_events[i].id == id) {
            LOG_I(TAG, "Remove id=%d \"%s\"", id, s_events[i].title);
            memset(&s_events[i], 0, sizeof(s_events[i]));
            _fire_changed();
            return;
        }
    }
    LOG_W(TAG, "Remove: id=%d not found", id);
}

int calendar_service_get_for_day(const char *date_str, cal_event_t *out, int max_out)
{
    int n = 0;
    for (int i = 0; i < CAL_SVC_MAX && n < max_out; i++) {
        /* TODO: repeat not implemented — exact date match only, no RRULE expansion */
        if (s_events[i].id > 0 && strncmp(s_events[i].date, date_str, 10) == 0)
            out[n++] = s_events[i];
    }
    return n;
}

bool calendar_service_get_by_id(int id, cal_event_t *out)
{
    for (int i = 0; i < CAL_SVC_MAX; i++) {
        if (s_events[i].id == id) {
            *out = s_events[i];
            return true;
        }
    }
    return false;
}

bool calendar_service_has_events_on(const char *date_str)
{
    for (int i = 0; i < CAL_SVC_MAX; i++) {
        if (s_events[i].id > 0 && strncmp(s_events[i].date, date_str, 10) == 0)
            return true;
    }
    return false;
}

/* ── Shared event-on-day predicates ────────────────────────────────────────── */

/* True when event start date matches the given year/month(1-12)/day. */
bool cal_event_on_day(const ha_calendar_event_t *ev, int year, int mon_1, int day)
{
    int ey, em, ed;
    if (sscanf(ev->start, "%d-%d-%d", &ey, &em, &ed) == 3)
        return ey == year && em == mon_1 && ed == day;
    return false;
}

/* True when the selected day falls within the event's span.
 * All-day event end dates from Google Calendar are exclusive (day after last day).
 * Timed events use the end date portion inclusively.
 * Used for the event list — dots keep cal_event_on_day (start date only). */
bool cal_event_spans_day(const ha_calendar_event_t *ev, int year, int mon_1, int day)
{
    int sy, sm, sd;
    if (sscanf(ev->start, "%d-%d-%d", &sy, &sm, &sd) != 3) return false;
    int start_int = sy * 10000 + sm * 100 + sd;
    int day_int   = year * 10000 + mon_1 * 100 + day;
    if (day_int < start_int) return false;

    int ey, em, ed;
    if (sscanf(ev->end, "%d-%d-%d", &ey, &em, &ed) != 3)
        return day_int == start_int;
    int end_int = ey * 10000 + em * 100 + ed;

    return ev->all_day ? (day_int < end_int) : (day_int <= end_int);
}
