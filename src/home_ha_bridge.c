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

#include "home_ha_bridge.h"
#include "home_summary.h"
#include "home_events_panel.h"
#include "home_calendar.h"
#include "calendar_service.h"
#include "clock_service.h"
#include "ha_ws_client.h"
#include "ha_entities.h"
#include "google_calendar.h"
#include "dashboard_config.h"
#include "dashboard_log.h"
#include "text_sub.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#endif
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "home_ha_bridge";

#define HHB_MONTH_CACHE_MAX  128

static ha_calendar_event_t s_month_cache[HHB_MONTH_CACHE_MAX];
static int                 s_month_cache_count = 0;

#ifdef ESP_PLATFORM
static TimerHandle_t s_cal_debounce_timer = NULL;
#endif

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void bridge_show_day(int year, int mon_1, int day);
static void cal_fetch_window(void);
static void on_calendar_events   (const ha_calendar_event_t *events, int count, void *ud);
static void on_gc_calendar_events(bool ok, const gc_event_t *events, int count, void *ctx);

/* ── Selection callback ────────────────────────────────────────────────────── */

static void _on_cal_selection_changed(int year, int mon_1, int day)
{
    bridge_show_day(year, mon_1, day);
}

/* ── bridge_show_day — show events for a given date ─────────────────────── */

/* Show events for (year, mon_1, day) by merging HA cache + local service. */
static void bridge_show_day(int year, int mon_1, int day)
{
#define EV_ITEM_MAX  64   /* max items per day (HA + local combined) */
#define EV_LOCAL_MAX 32   /* max local events per day */
    int today_year, today_mon_1, today_day;
    home_calendar_get_today(&today_year, &today_mon_1, &today_day);

    bool is_past = (year < today_year) ||
                   (year == today_year && mon_1 < today_mon_1) ||
                   (year == today_year && mon_1 == today_mon_1 &&
                    day < today_day);
    bool is_today = (year == today_year && mon_1 == today_mon_1 &&
                     day == today_day);

    time_t    now    = time(NULL);
    struct tm now_tm = *localtime(&now);

    ev_item_t items[EV_ITEM_MAX];
    int n = 0;

    /* ── HA events ──────────────────────────────────────────────────────── */
    for (int i = 0; i < s_month_cache_count && n < EV_ITEM_MAX; i++) {
        const ha_calendar_event_t *hev = &s_month_cache[i];
        if (!cal_event_spans_day(hev, year, mon_1, day)) continue;

        ev_item_t *it = &items[n++];
        it->event_id = -1;
        /* Continuation days (not the start date) always display as "All day". */
        bool is_continuation = !cal_event_on_day(hev, year, mon_1, day);
        it->all_day  = (hev->all_day != 0) || is_continuation;
        snprintf(it->uid,  sizeof(it->uid),  "%s", hev->uid);
        snprintf(it->date, sizeof(it->date), "%04d-%02d-%02d", year, mon_1, day);
        it->hour   = -1;
        it->minute = 0;

        if (it->all_day) {
            snprintf(it->time_str, sizeof(it->time_str), "All day");
            it->sort_min = -1;
            it->is_past  = is_past;
        } else {
            const char *tp = strchr(hev->start, 'T');
            if (!tp) tp = strchr(hev->start, ' ');
            if (tp) {
                int h = atoi(tp + 1);
                int m = atoi(tp + 4);
                it->hour   = h;
                it->minute = m;
                snprintf(it->time_str, sizeof(it->time_str), "%02d:%02d", h, m);
                it->sort_min = h * 60 + m;
                it->is_past  = is_past || (is_today &&
                               ((h < now_tm.tm_hour) ||
                                (h == now_tm.tm_hour && m <= now_tm.tm_min)));
            } else {
                snprintf(it->time_str, sizeof(it->time_str), "--:--");
                it->sort_min = 0;
                it->is_past  = is_past;
            }
        }
        { char _clean[128]; text_sub(_clean, sizeof(_clean), hev->summary);
          snprintf(it->title, sizeof(it->title), "%s", _clean); }
    }

    /* ── Local events from calendar_service ────────────────────────────── */
    char date_str[11];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", year, mon_1, day);
    cal_event_t local_evs[EV_LOCAL_MAX];
    int local_n = calendar_service_get_for_day(date_str, local_evs, EV_LOCAL_MAX);

    for (int i = 0; i < local_n && n < EV_ITEM_MAX; i++) {
        const cal_event_t *lev = &local_evs[i];
        ev_item_t *it = &items[n++];
        it->event_id = lev->id;
        it->all_day  = (lev->hour < 0);
        snprintf(it->uid,  sizeof(it->uid),  "%s", lev->uid);
        snprintf(it->date, sizeof(it->date), "%s", lev->date);
        it->hour   = lev->hour;
        it->minute = lev->minute;

        if (lev->hour < 0) {
            snprintf(it->time_str, sizeof(it->time_str), "All day");
            it->sort_min = -1;
            it->is_past  = is_past;
        } else {
            snprintf(it->time_str, sizeof(it->time_str), "%02d:%02d",
                     lev->hour, lev->minute);
            it->sort_min = lev->hour * 60 + lev->minute;
            it->is_past  = is_past || (is_today &&
                           ((lev->hour < now_tm.tm_hour) ||
                            (lev->hour == now_tm.tm_hour &&
                             lev->minute <= now_tm.tm_min)));
        }
        { char _clean[128]; text_sub(_clean, sizeof(_clean), lev->title);
          snprintf(it->title, sizeof(it->title), "%s", _clean); }
    }

    home_events_panel_show_day(year, mon_1, day, items, n);
#undef EV_ITEM_MAX
#undef EV_LOCAL_MAX
}

/* ── cal_fetch_window ─────────────────────────────────────────────────────── */

static void cal_fetch_window(void)
{
    int today_year, today_mon_1, today_day;
    home_calendar_get_today(&today_year, &today_mon_1, &today_day);

    struct tm tf = {0};
    tf.tm_year = today_year - 1900;
    tf.tm_mon  = today_mon_1 - 1 - HC_FETCH_BACK_MONTHS;
    tf.tm_mday = 1;
    time_t from = mktime(&tf);

    struct tm tt = {0};
    tt.tm_year = today_year - 1900;
    tt.tm_mon  = today_mon_1 - 1 + HC_FETCH_FORWARD_MONTHS + 1;
    tt.tm_mday = 1;
    time_t to = mktime(&tt);

    char start_iso[32], end_iso[32];
    struct tm *ptf = localtime(&from);
    strftime(start_iso, sizeof(start_iso), "%Y-%m-%dT00:00:00", ptf);
    struct tm *ptt = localtime(&to);
    strftime(end_iso, sizeof(end_iso), "%Y-%m-%dT00:00:00", ptt);

    LOG_D(TAG, "Fetching calendar window %s -> %s", start_iso, end_iso);
    if (gc_auth_state() == GC_AUTH_READY) {
        gc_get_events(dashboard_config_get_google_calendar_id(), from, to,
                      on_gc_calendar_events, NULL);
    } else {
        ha_get_calendar_events(HA_ENTITY_CALENDAR, start_iso, end_iso,
                               on_calendar_events, NULL);
    }
}

/* ── on_calendar_events: cache whole month, update grid dots, show selection ─ */

static void on_calendar_events(const ha_calendar_event_t *events, int count, void *ud)
{
    (void)ud;

    int n = count < HHB_MONTH_CACHE_MAX ? count : HHB_MONTH_CACHE_MAX;
    memcpy(s_month_cache, events, (size_t)n * sizeof(ha_calendar_event_t));
    s_month_cache_count = n;

    int sel_year, sel_mon_1, sel_day;
    home_calendar_get_selected(&sel_year, &sel_mon_1, &sel_day);
    LOG_I(TAG, "Calendar (HA): %d event(s) for %04d-%02d", n, sel_year, sel_mon_1);

    home_calendar_set_events(s_month_cache, s_month_cache_count);
    bridge_show_day(sel_year, sel_mon_1, sel_day);
}

static void on_gc_calendar_events(bool ok, const gc_event_t *events, int count, void *ctx)
{
    (void)ctx;
    if (!ok) {
        LOG_W(TAG, "GC get_events failed");
        return;
    }
    int n = count < HHB_MONTH_CACHE_MAX ? count : HHB_MONTH_CACHE_MAX;
    memset(s_month_cache, 0, (size_t)n * sizeof(ha_calendar_event_t));
    for (int i = 0; i < n; i++) {
        strncpy(s_month_cache[i].summary, events[i].summary, sizeof(s_month_cache[i].summary) - 1);
        strncpy(s_month_cache[i].start,   events[i].start_dt, sizeof(s_month_cache[i].start) - 1);
        strncpy(s_month_cache[i].end,     events[i].end_dt,   sizeof(s_month_cache[i].end) - 1);
        strncpy(s_month_cache[i].uid,     events[i].uid,      sizeof(s_month_cache[i].uid) - 1);
        s_month_cache[i].all_day = events[i].all_day ? 1 : 0;
    }
    s_month_cache_count = n;

    int sel_year, sel_mon_1, sel_day;
    home_calendar_get_selected(&sel_year, &sel_mon_1, &sel_day);
    LOG_I(TAG, "Calendar (GC): %d event(s) for %04d-%02d", n, sel_year, sel_mon_1);

    home_calendar_set_events(s_month_cache, s_month_cache_count);
    bridge_show_day(sel_year, sel_mon_1, sel_day);
}

/* ── Calendar live-update subscription ───────────────────────────────────── */

/* Fires from the FreeRTOS timer daemon task — NOT inside lv_timer_handler(),
 * so lv_lock() is NOT held.  cal_fetch_window() uses home_calendar getters
 * and network APIs only, so no lv_lock needed here.  This avoids the
 * deadlock where on_calendar_state called ha_get_calendar_events() →
 * esp_websocket_client_send_text() which can block on a stalled TCP send. */
#ifdef ESP_PLATFORM
static void _cal_debounce_cb(TimerHandle_t t)
{
    (void)t;
    cal_fetch_window();
}
#endif

/* HA fires state_changed for the calendar entity whenever the current/next
   event changes, or when the Google Calendar integration polls and picks up
   new or modified events. Defer the fetch via a one-shot timer so it runs
   outside lv_lock() — see _cal_debounce_cb above. */
static void on_calendar_state(const ha_state_t *s, void *ud)
{
    (void)s; (void)ud;
    LOG_D(TAG, "Calendar entity updated — scheduling refresh");
#ifdef ESP_PLATFORM
    if (s_cal_debounce_timer)
        xTimerReset(s_cal_debounce_timer, 0);
#else
    cal_fetch_window();
#endif
}

/* ── Calendar-service changed callback ───────────────────────────────────── */

static void _cal_svc_changed_cb(void *ud)
{
    (void)ud;
    int sel_year, sel_mon_1, sel_day;
    home_calendar_get_selected(&sel_year, &sel_mon_1, &sel_day);
    int today_year, today_mon_1, today_day;
    home_calendar_get_today(&today_year, &today_mon_1, &today_day);

    /* Push fresh event list into the calendar for dot updates */
    home_calendar_set_events(s_month_cache, s_month_cache_count);
    bridge_show_day(sel_year, sel_mon_1, sel_day);
    /* After a save, restart idle timer if viewing a non-today day */
    if (!(sel_year == today_year && sel_mon_1 == today_mon_1 && sel_day == today_day)) {
        home_calendar_idle_reset();
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void home_ha_bridge_init(void)
{
    /* Subscribe to environmental entities (delegated to home_summary). */
    home_summary_register_subscriptions();

    /* Subscribe to calendar entity — fires the debounce timer that triggers a fetch. */
    ha_subscribe(HA_ENTITY_CALENDAR, on_calendar_state, NULL);

#ifdef ESP_PLATFORM
    /* 500 ms debounce for calendar state changes (matches old behaviour). */
    s_cal_debounce_timer = xTimerCreate("cal_deb",
                                         pdMS_TO_TICKS(500),
                                         pdFALSE, NULL,
                                         _cal_debounce_cb);
#endif

    /* Local calendar service changes (locally-created events). */
    calendar_service_set_changed_cb(_cal_svc_changed_cb, NULL);

    /* Wire calendar selection → events panel via this bridge. */
    home_calendar_set_selection_changed_cb(_on_cal_selection_changed);

    /* Hook the clock day-rollover so we refresh on midnight. */
    clock_service_set_day_changed_cb(home_ha_bridge_refresh_calendar);

    /* Kick off the initial fetch — populates dots + today's event list. */
    cal_fetch_window();
}

void home_ha_bridge_refresh_calendar(void)
{
    /* Re-read today from OS clock and update calendar (handles day-rollover
       and SNTP-jump cases). */
    time_t now = time(NULL);
    struct tm t = *localtime(&now);
    home_calendar_set_today(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);

#ifdef ESP_PLATFORM
    if (s_cal_debounce_timer) xTimerReset(s_cal_debounce_timer, 0);
#else
    cal_fetch_window();
#endif
}
