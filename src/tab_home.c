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

#include "tab_home.h"
#include "home_ha_bridge.h"
#include "home_calendar.h"
#include "home_summary.h"
#include "home_events_panel.h"
#include "now_playing.h"
#include "dashboard_lv_utils.h"
#include "clock_service.h"
#include "dashboard_colours.h"
#include "calendar_service.h"
#include "dashboard_log.h"
#include "lvgl.h"

static const char *TAG = "tab_home";

/* ── Private layout constants ─────────────────────────────────────────────── */
#define HOME_PAD          40   /* content inset on all four edges (px)          */
#define HOME_TOP_BOT_GAP  22   /* gap between top cards and events/calendar (px) */
#define HOME_BOT_PAD_ROW  16   /* gap between events section and calendar (px)  */

/* ── Entry point ──────────────────────────────────────────────────────────── */

void tab_home_init(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_size(parent, LV_PCT(100), LV_PCT(100));
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_all(parent, HOME_PAD, 0);
    lv_obj_set_style_pad_row(parent, HOME_TOP_BOT_GAP, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    /* Top section: clock + condition chips + indoor summary */
    lv_obj_t *top_grp = lv_obj_create(parent);
    lv_obj_set_size(top_grp, LV_PCT(100), LV_SIZE_CONTENT);
    obj_clear(top_grp);
    lv_obj_set_layout(top_grp, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(top_grp, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(top_grp, 16, 0);

    lv_obj_t *lbl_time;
    home_summary_init(top_grp, &lbl_time);
    clock_service_init(lbl_time);
    now_playing_init(top_grp);

    lv_obj_t *bot_grp = lv_obj_create(parent);
    lv_obj_set_size(bot_grp, LV_PCT(100), 0);
    lv_obj_set_flex_grow(bot_grp, 1);
    obj_clear(bot_grp);
    lv_obj_set_layout(bot_grp, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(bot_grp, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(bot_grp, HOME_BOT_PAD_ROW, 0);

    home_events_panel_init(bot_grp);

    /* home_calendar_init seeds today from OS clock and builds the calendar card */
    home_calendar_init(bot_grp);

    /* Set the events-panel heading to today */
    {
        int today_year, today_mon_1, today_day;
        home_calendar_get_today(&today_year, &today_mon_1, &today_day);
        home_events_panel_set_heading(today_year, today_mon_1, today_day);
    }

    /* Local event store — must init before bridge wires the changed callback */
    calendar_service_init();

    /* Wire all HA subscriptions, calendar fetch, and cross-widget orchestration */
    home_ha_bridge_init();

    LOG_I(TAG, "Home tab ready");
}
