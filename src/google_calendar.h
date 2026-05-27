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
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

typedef enum {
    GC_AUTH_UNLINKED = 0,  /* no refresh token stored */
    GC_AUTH_PENDING,       /* device flow in progress */
    GC_AUTH_READY,         /* access token valid or refreshable */
    GC_AUTH_ERROR,         /* refresh failed — user must re-link */
} gc_auth_state_t;

typedef struct {
    char uid[128];      /* Google Calendar event ID; empty string for new events */
    char summary[256];
    char start_dt[32];  /* "YYYY-MM-DDTHH:MM:00+HH:MM"  or  "YYYY-MM-DD" (all-day) */
    char end_dt[32];
    bool all_day;
} gc_event_t;

typedef void (*gc_done_cb_t)(bool ok, void *ctx);

/* Delivers a snapshot of fetched events. events/count valid only for the
   duration of the call — copy anything you need. */
typedef void (*gc_events_cb_t)(bool ok, const gc_event_t *events, int count,
                                void *ctx);

/* on_code fires on the LVGL thread when the device code is ready to display.
   user_code is the short code (e.g. "ABCD-EFGH"), url is the verification URL. */
typedef void (*gc_code_cb_t)(const char *user_code, const char *url, void *ctx);

/* ── Lifecycle ──────────────────────────────────────────────────────────────── */

/* Load stored tokens. Call once from the LVGL thread after lv_init(). */
void             gc_init(void);

/* Current auth state. Safe to call from LVGL thread at any time. */
gc_auth_state_t  gc_auth_state(void);

/* ── Device Authorization Flow ──────────────────────────────────────────────── */

/* Start the Device Authorization Flow.
   on_code fires when the user code is ready to display.
   on_done fires when auth succeeds (ok=true) or fails/times out (ok=false).
   All callbacks fire on the LVGL thread. */
void gc_auth_start(gc_code_cb_t on_code, gc_done_cb_t on_done, void *ctx);

/* Cancel an in-progress auth flow. No-op if not pending. */
void gc_auth_cancel(void);

/* Clear stored tokens and return to UNLINKED state. */
void gc_auth_revoke(void);

/* ── Calendar CRUD ──────────────────────────────────────────────────────────── */

/* All functions are non-blocking; cb fires on the LVGL thread when complete. */
void gc_create_event(const gc_event_t *ev, gc_done_cb_t cb,    void *ctx);
void gc_update_event(const gc_event_t *ev, gc_done_cb_t cb,    void *ctx);
void gc_delete_event(const char *uid,      gc_done_cb_t cb,    void *ctx);
/* Truncate a recurring series at date_str ("YYYY-MM-DD"): PATCHes the master
   event's RRULE UNTIL so the selected occurrence and all following are removed. */
void gc_delete_future_events(const char *uid, const char *date_str,
                             gc_done_cb_t cb, void *ctx);
void gc_get_events  (const char *cal_id,   time_t from, time_t to,
                     gc_events_cb_t cb,    void *ctx);

/* Format local time as "YYYY-MM-DDTHH:MM:00+HH:MM" using current UTC offset. */
void gc_fmt_datetime(char *out, size_t n,
                     int year, int mon, int mday, int hour, int min);

/* Called from control_sheet_init() to build the Google Calendar config section. */
void gc_config_section_init(lv_obj_t *parent);
