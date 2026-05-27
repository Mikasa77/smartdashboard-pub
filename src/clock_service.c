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

#include "clock_service.h"
#include "dashboard_log.h"
#include <time.h>
#include <stdio.h>

static const char *TAG = "clock_service";

static lv_obj_t              *s_lbl_time        = NULL;
static lv_timer_t            *s_timer           = NULL;
static clock_day_changed_cb_t s_day_changed_cb  = NULL;
static int                    s_last_year      = -1;
static int                    s_last_mon       = -1;
static int                    s_last_mday      = -1;

static void update_labels(void)
{
    time_t now = time(NULL);
    struct tm t = *localtime(&now);

    char buf_time[8];
    snprintf(buf_time, sizeof(buf_time), "%02d:%02d", t.tm_hour, t.tm_min);
    lv_label_set_text(s_lbl_time, buf_time);

    if (s_day_changed_cb &&
        (t.tm_year != s_last_year ||
         t.tm_mon  != s_last_mon  ||
         t.tm_mday != s_last_mday)) {
        s_last_year = t.tm_year;
        s_last_mon  = t.tm_mon;
        s_last_mday = t.tm_mday;
        LOG_I(TAG, "date changed → %04d-%02d-%02d: calling cb",
              t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
        s_day_changed_cb();
        LOG_I(TAG, "date-change cb returned");
    }
}

static void timer_cb(lv_timer_t *tmr)
{
    (void)tmr;
    update_labels();
}

void clock_service_set_day_changed_cb(clock_day_changed_cb_t cb)
{
    s_day_changed_cb = cb;
}

void clock_service_init(lv_obj_t *lbl_time)
{
    s_lbl_time = lbl_time;
    update_labels();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    LOG_I(TAG, "Clock service ready");
}
