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

#include "home_events_panel.h"
#include "home_calendar.h"
#include "text_sub.h"
#include "calendar_service.h"
#include "event_edit.h"
#include "sheet.h"
#include "google_calendar.h"
#include "home_ha_bridge.h"
#include "dashboard_colours.h"
#include "dashboard_icons.h"
#include "dashboard_log.h"
#include "dashboard_lv_utils.h"
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "home_events_panel";

#define EV_MAX  32   /* max event rows per day */

/* ── Per-row swipe state ──────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t  *title;          /* draggable title layer                       */
    lv_obj_t  *edit_ind;       /* edit indicator (revealed on right-swipe)    */
    lv_obj_t  *del_area;       /* delete area (revealed on left-swipe)        */
    lv_obj_t  *del_lbl;       /* "Delete" label — set to "..." while delete pending */
    lv_obj_t  *row_cont;      /* row container — used to unblock taps after delete fails */
    int32_t    drag_start_x;
    int32_t    drag_start_y;
    int32_t    base_dx;        /* start dx when drag begins                   */
    int32_t    cur_dx;         /* current translation of title from HEP_TIME_W */
    int        row_idx;        /* index in s_row_st[]                         */
    int        event_id;       /* >0 = local cal_service id; -1 = HA event    */
    char       uid[128];       /* HA event UID; empty for local events        */
    char       ev_title[128];  /* copy of title for HA event editing          */
    char       date[11];       /* "YYYY-MM-DD"                                */
    int        hour;           /* -1 = all-day                                */
    int        minute;
    bool       is_recurring;   /* uid pattern suggests recurring GCal event   */
    bool       revealed;       /* delete currently revealed                   */
    bool       revealed_edit;  /* edit currently revealed                     */
    bool       drag_active;
    bool       dir_locked;
    bool       dir_is_x;
} ev_row_state_t;

/* ── Delete repeat context ────────────────────────────────────────────────── */

typedef struct { char uid[128]; char title[128]; char date[11]; } del_repeat_ctx_t;

/* ── Module state ─────────────────────────────────────────────────────────── */

static lv_obj_t      *s_events_list    = NULL;
static lv_obj_t      *s_ev_heading_lbl = NULL;
static ev_row_state_t s_row_st[EV_MAX];
static int            s_row_count = 0;

static del_repeat_ctx_t *s_del_ctx = NULL;

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void ev_close_all_except(int idx);
static void ev_open_edit(ev_row_state_t *rs);
static void _update_ev_heading(int year, int month_1, int day);
static void _on_gc_delete_done(bool ok, void *ctx);

/* ── Animation helper ─────────────────────────────────────────────────────── */

static void ev_title_anim_exec(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, HEP_TIME_W + v);
}

static void _row_height_exec(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, v);
}

static void _row_opa_exec(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void _row_delete_anim_done(lv_anim_t *a)
{
    ev_row_state_t *rs = (ev_row_state_t *)lv_anim_get_user_data(a);
    if (!rs) return;
    lv_obj_add_flag(rs->row_cont, LV_OBJ_FLAG_HIDDEN);
    if (rs->event_id > 0) {
        calendar_service_remove(rs->event_id);
        home_ha_bridge_refresh_calendar();
    } else if (rs->uid[0]) {
        lv_obj_remove_flag(rs->row_cont, LV_OBJ_FLAG_CLICKABLE);
        gc_delete_event(rs->uid, _on_gc_delete_done, rs);
    }
}

static void ev_update_indicators(ev_row_state_t *rs)
{
    int32_t dx = rs->cur_dx;
    lv_opa_t edit_opa = dx > 8  ? (lv_opa_t)LV_MIN(255, (dx - 8)  * 255 / 40) : LV_OPA_TRANSP;
    lv_opa_t del_opa  = dx < -8 ? (lv_opa_t)LV_MIN(255, (-dx - 8) * 255 / 40) : LV_OPA_TRANSP;
    lv_obj_set_style_opa(rs->edit_ind, edit_opa, 0);
    lv_obj_set_style_opa(rs->del_area, del_opa,  0);
}

static void ev_spring_to(ev_row_state_t *rs, int32_t to_dx)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rs->title);
    lv_anim_set_values(&a, rs->cur_dx, to_dx);
    lv_anim_set_exec_cb(&a, ev_title_anim_exec);
    lv_anim_set_duration(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    rs->cur_dx = to_dx;
    ev_update_indicators(rs);
}

static void _bounce_leg2_cb(lv_anim_t *a)
{
    ev_row_state_t *rs = (ev_row_state_t *)lv_anim_get_user_data(a);
    lv_anim_t a2;
    lv_anim_init(&a2);
    lv_anim_set_var(&a2, rs->title);
    lv_anim_set_values(&a2, rs->cur_dx + HEP_BOUNCE_PX, rs->cur_dx);
    lv_anim_set_exec_cb(&a2, ev_title_anim_exec);
    lv_anim_set_duration(&a2, HEP_BOUNCE_MS / 2);
    lv_anim_set_path_cb(&a2, lv_anim_path_ease_in);
    lv_anim_start(&a2);
}

static void ev_bounce(ev_row_state_t *rs)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, rs->title);
    lv_anim_set_values(&a, rs->cur_dx, rs->cur_dx + HEP_BOUNCE_PX);
    lv_anim_set_exec_cb(&a, ev_title_anim_exec);
    lv_anim_set_duration(&a, HEP_BOUNCE_MS / 2);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, rs);
    lv_anim_set_ready_cb(&a, _bounce_leg2_cb);
    lv_anim_start(&a);
}

/* ── Swipe event handler ──────────────────────────────────────────────────── */

static void ev_row_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    ev_row_state_t *rs   = (ev_row_state_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        if (home_calendar_idle_active()) home_calendar_idle_reset();
        ev_close_all_except(rs->row_idx);
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        rs->drag_start_x = p.x;
        rs->drag_start_y = p.y;
        rs->base_dx      = rs->revealed      ? -HEP_DEL_REVEAL :
                           rs->revealed_edit ?  HEP_EDIT_REVEAL : 0;
        rs->drag_active  = true;
        rs->dir_locked   = false;
        rs->dir_is_x     = false;
        lv_anim_delete(rs->title, ev_title_anim_exec);
    }
    else if (code == LV_EVENT_PRESSING) {
        if (!rs->drag_active) return;
        lv_indev_t *indev = lv_indev_active();
        if (!indev) return;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t ddx = p.x - rs->drag_start_x;
        int32_t ddy = p.y - rs->drag_start_y;

        if (!rs->dir_locked && (LV_ABS(ddx) > 6 || LV_ABS(ddy) > 6)) {
            rs->dir_locked = true;
            rs->dir_is_x   = LV_ABS(ddx) >= LV_ABS(ddy);
        }
        if (!rs->dir_is_x) return;

        int32_t next = rs->base_dx + ddx;
        /* Elastic resistance beyond the reveal bounds */
        if (next > HEP_EDIT_REVEAL)  next = HEP_EDIT_REVEAL + (next - HEP_EDIT_REVEAL) * 3 / 10;
        if (next < -HEP_DEL_REVEAL)  next = -HEP_DEL_REVEAL + (next - (-HEP_DEL_REVEAL)) * 3 / 10;
        rs->cur_dx = next;
        lv_obj_set_x(rs->title, HEP_TIME_W + next);
        ev_update_indicators(rs);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!rs->drag_active) return;
        rs->drag_active = false;

        if (!rs->dir_locked || !rs->dir_is_x) {
            /* Tap on row body */
            if (rs->revealed || rs->revealed_edit) {
                rs->revealed = rs->revealed_edit = false;
                ev_spring_to(rs, 0);
            } else {
                ev_bounce(rs);
            }
            return;
        }

        /* drag_delta = how far the user moved from the resting position they
           started from.  Using this (not absolute cur_dx) means a right-swipe
           from a delete-revealed state reads as positive — the "hook" releases
           when you pull back past the same threshold used to open it. */
        int32_t drag_delta = rs->cur_dx - rs->base_dx;

        if (rs->revealed_edit) {
            /* Edit panel is showing — left-swipe past threshold to close it */
            if (drag_delta < -HEP_RIGHT_THR) {
                rs->revealed_edit = false;
                ev_spring_to(rs, 0);
            } else {
                ev_spring_to(rs, HEP_EDIT_REVEAL);
            }
        } else if (rs->revealed) {
            /* Delete panel open: right-swipe past threshold unhooks and closes */
            if (drag_delta > HEP_LEFT_THR) {
                rs->revealed = false;
                ev_spring_to(rs, 0);
            } else {
                ev_spring_to(rs, -HEP_DEL_REVEAL);
            }
        } else {
            /* At rest — right-swipe latches edit indicator; left-swipe reveals delete */
            if (drag_delta > HEP_RIGHT_THR) {
                rs->revealed_edit = true;
                ev_spring_to(rs, HEP_EDIT_REVEAL);
            } else if (drag_delta < -HEP_LEFT_THR) {
                rs->revealed = true;
                ev_spring_to(rs, -HEP_DEL_REVEAL);
            } else {
                ev_spring_to(rs, 0);
            }
        }
    }
}

/* Returns true when uid looks like a Google Calendar recurring instance:
   pattern is <masterEventId>_<digit>... e.g. "abc123_20261105T100000Z@..." */
static bool gc_uid_is_recurring(const char *uid)
{
    /* Google Calendar recurring instance UIDs end in _YYYYMMDD[Thhmmssz].
     * Require at least 8 consecutive digits after '_' to avoid false positives
     * from iCal subscription event UIDs (e.g. "formulaone_round7@ical"). */
    if (!uid || !uid[0]) return false;
    const char *p = strchr(uid, '_');
    while (p) {
        int n = 0;
        while (p[1 + n] >= '0' && p[1 + n] <= '9') n++;
        if (n >= 8) return true;
        p = strchr(p + 1, '_');
    }
    return false;
}

static void _on_gc_delete_done(bool ok, void *ctx)
{
    if (ok) {
        home_ha_bridge_refresh_calendar();
    } else {
        LOG_W(TAG, "Delete failed");
        ev_row_state_t *rs = (ev_row_state_t *)ctx;
        if (rs) {
            /* Restore the row that was collapsed by the delete animation */
            lv_obj_remove_flag(rs->row_cont, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_height(rs->row_cont, HEP_ROW_H);
            lv_obj_set_style_opa(rs->row_cont, LV_OPA_COVER, 0);
            lv_obj_add_flag(rs->row_cont, LV_OBJ_FLAG_CLICKABLE);
            ev_spring_to(rs, 0);
            rs->revealed = false;
        }
    }
}

/* ── Delete Repeat Sheet ──────────────────────────────────────────────────── */

static void _del_confirmed_cb(lv_event_t *e)
{
    del_repeat_ctx_t *ctx = (del_repeat_ctx_t *)lv_event_get_user_data(e);
    LOG_I(TAG, "Delete: \"%s\" %s", ctx->title, ctx->date);
    gc_delete_event(ctx->uid, _on_gc_delete_done, NULL);
    free(ctx);
    s_del_ctx = NULL; /* prevent _del_sheet_close_cb double-free */
    sheet_close();
}

static void _del_future_cb(lv_event_t *e)
{
    del_repeat_ctx_t *ctx = (del_repeat_ctx_t *)lv_event_get_user_data(e);
    LOG_I(TAG, "Delete future: \"%s\" from %s", ctx->title, ctx->date);
    gc_delete_future_events(ctx->uid, ctx->date, _on_gc_delete_done, NULL);
    free(ctx);
    s_del_ctx = NULL; /* prevent _del_sheet_close_cb double-free */
    sheet_close();
}

static void _del_repeat_sheet_init(lv_obj_t *parent)
{
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    /* Title */
    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, "Remove recurring event");
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_ver(hdr, 16, 0);
    lv_obj_set_style_pad_left(hdr, 40, 0);
    lv_obj_set_width(hdr, LV_PCT(100));

    /* "Remove this date only" */
    lv_obj_t *btn1 = lv_obj_create(parent);
    lv_obj_set_size(btn1, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(btn1, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(btn1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn1, 0, 0);
    lv_obj_set_style_border_color(btn1, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(btn1, 1, 0);
    lv_obj_set_style_border_side(btn1, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(btn1, 0, 0);
    lv_obj_remove_flag(btn1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn1, _del_confirmed_cb, LV_EVENT_CLICKED, s_del_ctx);
    lv_obj_t *lbl1 = lv_label_create(btn1);
    lv_label_set_text(lbl1, "Remove this date only");
    lv_obj_set_style_text_color(lbl1, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(lbl1, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl1);

    /* "Remove all future occurrences" */
    lv_obj_t *btn2 = lv_obj_create(parent);
    lv_obj_set_size(btn2, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(btn2, lv_color_hex(COL_SHEET), 0);
    lv_obj_set_style_bg_opa(btn2, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn2, 0, 0);
    lv_obj_set_style_border_color(btn2, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(btn2, 1, 0);
    lv_obj_set_style_border_side(btn2, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_pad_all(btn2, 0, 0);
    lv_obj_remove_flag(btn2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn2, _del_future_cb, LV_EVENT_CLICKED, s_del_ctx);
    lv_obj_t *lbl2 = lv_label_create(btn2);
    lv_label_set_text(lbl2, "Remove all future occurrences");
    lv_obj_set_style_text_color(lbl2, lv_color_hex(COL_DELETE), 0);
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl2);
}

static void _del_sheet_close_cb(void *ud)
{
    (void)ud;
    /* If user dragged to dismiss without tapping, free the context */
    if (s_del_ctx) { free(s_del_ctx); s_del_ctx = NULL; }
}

static void ev_del_event_cb(lv_event_t *e)
{
    ev_row_state_t *rs = (ev_row_state_t *)lv_event_get_user_data(e);
    if (!rs || !rs->revealed) return;

    if (rs->event_id > 0) {
        /* Local-only event — animate collapse, then remove from calendar_service */
        LOG_I(TAG, "Delete: \"%s\" %s scope=single (local)", rs->ev_title, rs->date);
        ev_spring_to(rs, 0);
        lv_obj_remove_flag(rs->row_cont, LV_OBJ_FLAG_CLICKABLE);

        lv_anim_t ah;
        lv_anim_init(&ah);
        lv_anim_set_var(&ah, rs->row_cont);
        lv_anim_set_values(&ah, HEP_ROW_H, 0);
        lv_anim_set_exec_cb(&ah, _row_height_exec);
        lv_anim_set_duration(&ah, 250);
        lv_anim_set_path_cb(&ah, lv_anim_path_ease_in);
        lv_anim_set_user_data(&ah, rs);
        lv_anim_set_ready_cb(&ah, _row_delete_anim_done);
        lv_anim_start(&ah);

        lv_anim_t ao;
        lv_anim_init(&ao);
        lv_anim_set_var(&ao, rs->row_cont);
        lv_anim_set_values(&ao, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_exec_cb(&ao, _row_opa_exec);
        lv_anim_set_duration(&ao, 250);
        lv_anim_set_path_cb(&ao, lv_anim_path_ease_in);
        lv_anim_start(&ao);
        return;
    }

    if (!rs->uid[0]) return;

    if (rs->is_recurring) {
        /* Repeating event — open choice sheet */
        s_del_ctx = malloc(sizeof(del_repeat_ctx_t));
        if (!s_del_ctx) return;
        strncpy(s_del_ctx->uid,   rs->uid,      sizeof(s_del_ctx->uid)   - 1);
        strncpy(s_del_ctx->title, rs->ev_title, sizeof(s_del_ctx->title) - 1);
        strncpy(s_del_ctx->date,  rs->date,     sizeof(s_del_ctx->date)  - 1);
        sheet_open(_del_repeat_sheet_init);
        sheet_set_close_cb(_del_sheet_close_cb, NULL);
    } else {
        /* Non-repeating remote event — animate collapse, then fire API */
        LOG_I(TAG, "Delete: \"%s\" %s scope=single", rs->ev_title, rs->date);
        ev_spring_to(rs, 0);
        lv_obj_remove_flag(rs->row_cont, LV_OBJ_FLAG_CLICKABLE);

        lv_anim_t ah;
        lv_anim_init(&ah);
        lv_anim_set_var(&ah, rs->row_cont);
        lv_anim_set_values(&ah, HEP_ROW_H, 0);
        lv_anim_set_exec_cb(&ah, _row_height_exec);
        lv_anim_set_duration(&ah, 250);
        lv_anim_set_path_cb(&ah, lv_anim_path_ease_in);
        lv_anim_set_user_data(&ah, rs);
        lv_anim_set_ready_cb(&ah, _row_delete_anim_done);
        lv_anim_start(&ah);

        lv_anim_t ao;
        lv_anim_init(&ao);
        lv_anim_set_var(&ao, rs->row_cont);
        lv_anim_set_values(&ao, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_exec_cb(&ao, _row_opa_exec);
        lv_anim_set_duration(&ao, 250);
        lv_anim_set_path_cb(&ao, lv_anim_path_ease_in);
        lv_anim_start(&ao);
    }
}

static void _ev_edit_ind_cb(lv_event_t *e)
{
    ev_row_state_t *rs = (ev_row_state_t *)lv_event_get_user_data(e);
    rs->revealed_edit = false;
    ev_spring_to(rs, 0);
    ev_open_edit(rs);
}

static void ev_open_edit(ev_row_state_t *rs)
{
    /* Pause idle timer while the edit sheet is open */
    home_calendar_idle_pause();

    if (rs->event_id > 0) {
        cal_event_t ev;
        if (calendar_service_get_by_id(rs->event_id, &ev))
            event_edit_open_edit(&ev);
        else
            LOG_W(TAG, "Edit: local event id=%d not found", rs->event_id);
    } else {
        /* HA-origin event — build transient cal_event_t from row state */
        cal_event_t ev = {0};
        snprintf(ev.uid,   sizeof(ev.uid),   "%s", rs->uid);
        snprintf(ev.title, sizeof(ev.title), "%s", rs->ev_title);
        snprintf(ev.date,  sizeof(ev.date),  "%s", rs->date);
        ev.hour   = rs->hour;
        ev.minute = rs->minute;
        ev.repeat = CAL_REPEAT_NONE;
        event_edit_open_edit(&ev);
    }
}

static void ev_close_all_except(int idx)
{
    for (int i = 0; i < s_row_count; i++) {
        if (i == idx) continue;
        ev_row_state_t *rs = &s_row_st[i];
        if (rs->revealed || rs->revealed_edit || rs->cur_dx != 0) {
            rs->revealed      = false;
            rs->revealed_edit = false;
            ev_spring_to(rs, 0);
        }
    }
}

static void ev_list_bg_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (home_calendar_idle_active()) home_calendar_idle_reset();
    ev_close_all_except(-1);
}

/* ── Build one row ────────────────────────────────────────────────────────── */

static void build_ev_row(lv_obj_t *parent, const char *time_str,
                         const char *title_str, bool is_past, int idx, int event_id)
{
    ev_row_state_t *rs = &s_row_st[idx];
    memset(rs, 0, sizeof(*rs));
    rs->row_idx  = idx;
    rs->event_id = event_id;

    uint32_t bg_col   = is_past ? COL_PAST_BG : COL_SURFACE;
    uint32_t time_col = is_past ? COL_PAST    : COL_ACCENT;
    uint32_t text_col = is_past ? COL_PAST    : COL_TEXT;

    /* Row container — children clipped to bounds (default LVGL behaviour) */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), HEP_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(bg_col), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, HEP_ROW_RADIUS, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

    /* ① Edit indicator — z=low (created first), hidden until right-swipe */
    lv_obj_t *edit_ind = lv_obj_create(row);
    lv_obj_set_pos(edit_ind, HEP_TIME_W + 12, 0);
    lv_obj_set_size(edit_ind, HEP_EDIT_REVEAL - 12, HEP_ROW_H);
    lv_obj_set_style_bg_opa(edit_ind, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(edit_ind, 0, 0);
    lv_obj_set_style_pad_all(edit_ind, 0, 0);
    lv_obj_set_style_opa(edit_ind, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(edit_ind, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edit_ind, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(edit_ind, _ev_edit_ind_cb, LV_EVENT_CLICKED, rs);

    lv_obj_t *edit_icon = lv_image_create(edit_ind);
    lv_image_set_src(edit_icon, &ICON_PENCIL_EDIT);
    lv_obj_set_style_align(edit_icon, LV_ALIGN_LEFT_MID, 0);
    lv_obj_set_pos(edit_icon, HEP_EDIT_ICON_X, HEP_EDIT_ICON_Y);

    lv_obj_t *edit_lbl = lv_label_create(edit_ind);
    lv_label_set_text(edit_lbl, "Edit");
    lv_obj_remove_flag(edit_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(edit_lbl, lv_color_hex(COL_EDIT), 0);
    lv_obj_set_style_text_font(edit_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_align(edit_lbl, LV_ALIGN_LEFT_MID, 0);
    lv_obj_set_pos(edit_lbl, HEP_EDIT_LBL_X, HEP_EDIT_LBL_Y);
    rs->edit_ind = edit_ind;
    lv_obj_set_style_bg_color(edit_ind, lv_color_hex(COL_EDIT),   LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa  (edit_ind, HEP_BTN_PRESSED_OPA,      LV_STATE_PRESSED);
    lv_obj_set_style_radius  (edit_ind, HEP_ROW_RADIUS,           LV_STATE_PRESSED);

    /* ② Delete area — z=low, right-aligned, hidden until left-swipe */
    lv_obj_t *del_area = lv_obj_create(row);
    lv_obj_set_size(del_area, HEP_DEL_W, HEP_ROW_H);
    lv_obj_set_align(del_area, LV_ALIGN_RIGHT_MID);
    lv_obj_set_style_bg_opa(del_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(del_area, 0, 0);
    lv_obj_set_style_pad_all(del_area, 0, 0);
    lv_obj_set_style_opa(del_area, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(del_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(del_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(del_area, ev_del_event_cb, LV_EVENT_CLICKED, rs);

    lv_obj_t *del_lbl = lv_label_create(del_area);
    lv_label_set_text(del_lbl, "Delete");
    lv_obj_remove_flag(del_lbl, LV_OBJ_FLAG_CLICKABLE);
    rs->del_lbl   = del_lbl;
    rs->row_cont  = row;
    lv_obj_set_style_text_color(del_lbl, lv_color_hex(COL_DELETE), 0);
    lv_obj_set_style_text_font(del_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(del_lbl, LV_ALIGN_RIGHT_MID, -HEP_DEL_LBL_PAD_R, 0);

    lv_obj_t *del_icon = lv_image_create(del_area);
    lv_image_set_src(del_icon, &ICON_TRASH_2_DELETE);
    lv_obj_set_style_align(del_icon, LV_ALIGN_LEFT_MID, 0);
    lv_obj_set_pos(del_icon, HEP_DEL_ICON_X, HEP_DEL_ICON_Y);
    rs->del_area = del_area;
    lv_obj_set_style_bg_color(del_area, lv_color_hex(COL_DELETE), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa  (del_area, HEP_BTN_PRESSED_OPA,      LV_STATE_PRESSED);
    lv_obj_set_style_radius  (del_area, HEP_ROW_RADIUS,           LV_STATE_PRESSED);

    /* ③ Time label — z=low (created first), opaque, covers the time column  */
    lv_obj_t *time_cont = lv_obj_create(row);
    lv_obj_set_pos(time_cont, 0, 0);
    lv_obj_set_size(time_cont, HEP_TIME_W, HEP_ROW_H);
    lv_obj_set_style_bg_color(time_cont, lv_color_hex(bg_col), 0);
    lv_obj_set_style_bg_opa(time_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(time_cont, 0, 0);
    lv_obj_set_style_pad_all(time_cont, 0, 0);
    lv_obj_remove_flag(time_cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *time_lbl = lv_label_create(time_cont);
    lv_label_set_text(time_lbl, time_str);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(time_col), 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_align(time_lbl, LV_ALIGN_LEFT_MID, 0);
    lv_obj_set_pos(time_lbl, HEP_TIME_PAD_X, HEP_TIME_PAD_Y);
    lv_obj_add_flag(time_lbl,  LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(time_cont, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* ④ Title layer — z=top (created last) so its drop shadow renders over
       time_cont; sized to sit between time_cont and del_area */
    lv_obj_t *title = lv_obj_create(row);
    lv_obj_set_pos(title, HEP_TIME_W, 0);
    lv_obj_set_size(title, HEP_CONTENT_W - HEP_SCROLLBAR_PAD - HEP_TIME_W, HEP_ROW_H);
    lv_obj_set_style_bg_color(title, lv_color_hex(bg_col), 0);
    lv_obj_set_style_bg_opa(title, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(title, 0, 0);
    lv_obj_set_style_pad_all(title, 0, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(title);
    lv_label_set_text(title_lbl, title_str);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(text_col), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_18, 0);
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(title_lbl, LV_PCT(100), HEP_ROW_H);
    lv_obj_set_style_pad_top(title_lbl,
        (HEP_ROW_H - (int32_t)lv_font_get_line_height(&lv_font_montserrat_18)) / 2, 0);
    lv_obj_set_pos(title_lbl, HEP_TITLE_PAD_X, 0);
    lv_obj_add_flag(title_lbl, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(title,     LV_OBJ_FLAG_EVENT_BUBBLE);
    rs->title = title;
    lv_obj_set_style_shadow_width   (title, HEP_SHADOW_WIDTH,    0);
    lv_obj_set_style_shadow_opa     (title, HEP_SHADOW_OPA,      0);
    lv_obj_set_style_shadow_offset_x(title, HEP_SHADOW_OFFSET_X, 0);
    lv_obj_set_style_shadow_offset_y(title, 0,                   0);
    lv_obj_set_style_shadow_spread  (title, 0,                   0);

    /* Swipe events on the row — all children bubble touches up to here */
    lv_obj_add_event_cb(row, ev_row_event_cb, LV_EVENT_PRESSED,    rs);
    lv_obj_add_event_cb(row, ev_row_event_cb, LV_EVENT_PRESSING,   rs);
    lv_obj_add_event_cb(row, ev_row_event_cb, LV_EVENT_RELEASED,   rs);
    lv_obj_add_event_cb(row, ev_row_event_cb, LV_EVENT_PRESS_LOST, rs);
}

/* ── Populate panel ───────────────────────────────────────────────────────── */

static int ev_item_cmp(const void *a, const void *b)
{
    const ev_item_t *ia = (const ev_item_t *)a;
    const ev_item_t *ib = (const ev_item_t *)b;
    if (ia->all_day != ib->all_day)
        return ia->all_day ? -1 : 1;   /* all-day first */
    return ia->sort_min - ib->sort_min;
}

static void ev_populate_panel(ev_item_t *items, int count)
{
    if (!s_events_list) return;

    lv_obj_clean(s_events_list);
    s_row_count = 0;

    if (count == 0) {
        lv_obj_t *lbl = lv_label_create(s_events_list);
        lv_label_set_text(lbl, "Nothing scheduled");
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MUTE), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_pad_top(lbl, 4, 0);
        return;
    }

    int n = count < EV_MAX ? count : EV_MAX;
    qsort(items, (size_t)n, sizeof(ev_item_t), ev_item_cmp);

    int last_past_idx = -1;
    for (int i = 0; i < n; i++) {
        if (items[i].is_past) last_past_idx = i;
        build_ev_row(s_events_list, items[i].time_str, items[i].title,
                     items[i].is_past, i, items[i].event_id);
        /* build_ev_row does memset — copy extra fields after */
        snprintf(s_row_st[i].uid,      sizeof(s_row_st[i].uid),      "%s", items[i].uid);
        snprintf(s_row_st[i].ev_title, sizeof(s_row_st[i].ev_title), "%s", items[i].title);
        snprintf(s_row_st[i].date,     sizeof(s_row_st[i].date),     "%s", items[i].date);
        s_row_st[i].hour         = items[i].hour;
        s_row_st[i].minute       = items[i].minute;
        s_row_st[i].is_recurring = gc_uid_is_recurring(items[i].uid);
        s_row_count++;
    }

    if (last_past_idx >= 2) {
        lv_obj_update_layout(s_events_list);
        lv_obj_scroll_to_y(s_events_list,
                           (int32_t)last_past_idx * (HEP_ROW_H + HEP_ROW_GAP),
                           LV_ANIM_OFF);
    }
}

/* ── Heading ──────────────────────────────────────────────────────────────── */

static void _update_ev_heading(int year, int month_1, int day)
{
    if (!s_ev_heading_lbl) return;
    struct tm t = {0};
    t.tm_year = year - 1900;
    t.tm_mon  = month_1 - 1;
    t.tm_mday = day;
    mktime(&t);
    char buf[48], wd[16], mo[16];
    strftime(wd, sizeof(wd), "%A", &t);
    strftime(mo, sizeof(mo), "%B",  &t);
    snprintf(buf, sizeof(buf), "%s %d %s", wd, t.tm_mday, mo);
    lv_label_set_text(s_ev_heading_lbl, buf);
}

/* ── Events section scaffold ─────────────────────────────────────────────── */

static void build_events_section(lv_obj_t *parent)
{
    lv_obj_t *section = lv_obj_create(parent);
    lv_obj_set_size(section, LV_PCT(100), 0);
    lv_obj_set_flex_grow(section, 1);
    obj_clear(section);
    lv_obj_set_layout(section, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(section, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(section, HEP_HDR_GAP, 0);

    /* Date heading — text set by _update_ev_heading() at init and on day/SNTP change */
    lv_obj_t *lbl_heading = lv_label_create(section);
    lv_obj_set_style_text_font(lbl_heading, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl_heading, lv_color_hex(COL_TEXT), 0);
    s_ev_heading_lbl = lbl_heading;

    /* Scrollable event list — fixed height, flex column, gapped rows */
    lv_obj_t *list = lv_obj_create(section);
    lv_obj_set_size(list, LV_PCT(100), 0);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_right(list, HEP_SCROLLBAR_PAD, 0);
    lv_obj_set_style_pad_row(list, HEP_ROW_GAP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(list, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(list, HEP_SCROLLBAR_W, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(list, HEP_SCROLLBAR_RADIUS, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(list, lv_color_hex(COL_SCROLLBAR), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(list, ev_list_bg_clicked_cb, LV_EVENT_CLICKED, NULL);
    s_events_list = list;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void home_events_panel_init(lv_obj_t *parent)
{
    build_events_section(parent);
}

void home_events_panel_show_day(int year, int mon_1, int day,
                                 ev_item_t *items, int count)
{
    _update_ev_heading(year, mon_1, day);
    /* ev_populate_panel sorts items[] in-place then populates s_row_st[] */
    ev_populate_panel(items, count);
}

void home_events_panel_set_heading(int year, int mon_1, int day)
{
    _update_ev_heading(year, mon_1, day);
}

void home_events_panel_close_all_reveals(void)
{
    ev_close_all_except(-1);   /* -1 = no row exempted; closes all */
}
