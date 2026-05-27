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

#include "event_edit.h"
#include "sheet.h"
#include "keyboard.h"
#include "date_picker.h"
#include "time_picker.h"
#include "calendar_service.h"
#include "google_calendar.h"
#include "home_ha_bridge.h"
#include "dashboard_colours.h"
#include "dashboard_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdbool.h>

static const char *TAG = "event_edit";

/* ── Repeat label table ───────────────────────────────────────────────────── */

static const char *const REPEAT_LABELS[] = {
    "None", "Daily", "Weekly", "Monthly", "Yearly"
};
#define REPEAT_COUNT 5

/* ── Module state ─────────────────────────────────────────────────────────── */

#define PEND_NONE     0
#define PEND_KEYBOARD 1
#define PEND_DATE     2
#define PEND_TIME     3

static struct {
    bool          is_edit;
    bool          is_ha_event;    /* true when editing an HA-origin event   */
    int           edit_id;
    char          ha_uid[128];    /* HA event UID; empty if API didn't provide */
    char          ha_title[128];  /* pre-fill title for HA-origin edits     */

    lv_obj_t     *ta_title;
    lv_obj_t     *lbl_date_field;
    lv_obj_t     *lbl_time_field;
    lv_obj_t     *btn_save;
    lv_obj_t     *repeat_btns[REPEAT_COUNT];

    int           sel_year;
    int           sel_month;     /* 1-12 */
    int           sel_day;
    int           sel_hour;      /* -1 = all-day / no time */
    int           sel_minute;
    cal_repeat_t  sel_repeat;

    /* Sheet lift tracking — managed here because we pass NULL to pickers
       to avoid animation conflicts when swapping between sub-panels.        */
    int32_t       cur_lift;
    bool          auto_keyboard;

    /* Sequential sub-panel transition: close current → sheet lowers → open new */
    int           pend_type;
    lv_timer_t   *pend_timer;
    bool          transition;  /* true while animated panel switch is in flight */
} s_ee;

/* ── Lift management ──────────────────────────────────────────────────────── */

static void _set_lift(int32_t desired)
{
    int32_t delta = desired - s_ee.cur_lift;
    if (delta > 0)       sheet_lift(delta);
    else if (delta < 0)  sheet_lower(-delta);
    s_ee.cur_lift = desired;
}

/* ── Display string helpers ───────────────────────────────────────────────── */

static void _fmt_date(char *buf, int sz)
{
    struct tm t = {0};
    t.tm_year = s_ee.sel_year - 1900;
    t.tm_mon  = s_ee.sel_month - 1;
    t.tm_mday = s_ee.sel_day;
    mktime(&t);
    /* e.g. "21 May 2026" */
    char mon[16];
    strftime(mon, sizeof(mon), "%B", &t);
    snprintf(buf, (size_t)sz, "%d %s %d", t.tm_mday, mon, s_ee.sel_year);
}

static void _fmt_time(char *buf, int sz)
{
    if (s_ee.sel_hour < 0)
        snprintf(buf, (size_t)sz, "No time");
    else
        snprintf(buf, (size_t)sz, "%02d:%02d", s_ee.sel_hour, s_ee.sel_minute);
}

/* ── Save button enable/disable ───────────────────────────────────────────── */

static void _update_save_btn(void)
{
    if (!s_ee.btn_save) return;
    const char *text = s_ee.ta_title ? lv_textarea_get_text(s_ee.ta_title) : "";
    bool enabled = text && text[0] != '\0';
    lv_obj_set_style_bg_color(s_ee.btn_save,
        lv_color_hex(enabled ? COL_ACCENT : COL_BORDER), 0);
    lv_obj_t *lbl = lv_obj_get_child(s_ee.btn_save, 0);
    if (lbl)
        lv_obj_set_style_text_color(lbl,
            lv_color_hex(enabled ? COL_BG : COL_TEXT_MUTE), 0);
    if (enabled)
        lv_obj_add_flag(s_ee.btn_save, LV_OBJ_FLAG_CLICKABLE);
    else
        lv_obj_remove_flag(s_ee.btn_save, LV_OBJ_FLAG_CLICKABLE);
}

/* ── Repeat selector helpers ──────────────────────────────────────────────── */

static void _highlight_repeat(cal_repeat_t r)
{
    for (int i = 0; i < REPEAT_COUNT; i++) {
        if (!s_ee.repeat_btns[i]) continue;
        bool sel = ((int)r == i);
        lv_obj_set_style_bg_color(s_ee.repeat_btns[i],
            lv_color_hex(sel ? COL_ACCENT : COL_BORDER), 0);
        lv_obj_t *lbl = lv_obj_get_child(s_ee.repeat_btns[i], 0);
        if (lbl)
            lv_obj_set_style_text_color(lbl,
                lv_color_hex(sel ? COL_BG : COL_TEXT_DIM), 0);
    }
}

/* ── Sub-panel open/close helpers ─────────────────────────────────────────── */

static void _kb_closed_cb(void *ud);
static void _on_date_selected(struct tm date, void *ud);
static void _on_time_selected(int hour, int minute, void *ud);

/* Executes the pending sub-panel open after the sheet has lowered. */
static void _deferred_open_impl(void)
{
    int type = s_ee.pend_type;
    s_ee.pend_type  = PEND_NONE;
    s_ee.pend_timer = NULL;

    if (!s_ee.ta_title) return;   /* sheet was closed while waiting */

    switch (type) {
    case PEND_KEYBOARD:
        _set_lift(KB_SHEET_H);
        keyboard_open(NULL, s_ee.ta_title, _kb_closed_cb, NULL);
        LOG_D(TAG, "Keyboard opened");
        break;
    case PEND_DATE: {
        struct tm initial = {0};
        initial.tm_year = s_ee.sel_year - 1900;
        initial.tm_mon  = s_ee.sel_month - 1;
        initial.tm_mday = s_ee.sel_day;
        mktime(&initial);
        _set_lift(DP_PICKER_H);
        date_picker_open(NULL, initial, _on_date_selected, NULL);
        break;
    }
    case PEND_TIME: {
        int h = s_ee.sel_hour < 0 ? 9 : s_ee.sel_hour;
        _set_lift(TP_PICKER_H);
        time_picker_open(NULL, h, s_ee.sel_minute, _on_time_selected, NULL);
        break;
    }
    default: break;
    }
}

static void _deferred_open_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    _deferred_open_impl();
}

/* Close any open sub-panel and schedule opening of the requested panel.
   When transitioning between panels the sheet lowers first, then the new
   panel rises — giving the sequential "drop then push up" animation the
   user wants.  When no panel is currently open, the new panel opens at once. */
static void _switch_to(int pend_type)
{
    bool was_open = keyboard_is_open() || date_picker_is_open() || time_picker_is_open();

    /* Cancel any in-flight deferred open */
    if (s_ee.pend_timer) {
        lv_timer_delete(s_ee.pend_timer);
        s_ee.pend_timer = NULL;
    }

    if (keyboard_is_open()) {
        s_ee.transition = true;   /* suppress _kb_closed_cb's _set_lift(0) */
        keyboard_close();         /* animated — fires _kb_closed_cb immediately */
    }
    if (date_picker_is_open())  date_picker_close();   /* animated, no lift callback */
    if (time_picker_is_open())  time_picker_close();   /* animated, no lift callback */

    s_ee.pend_type = pend_type;

    if (was_open) {
        /* Lower sheet simultaneously with the outgoing panel so they move together. */
        _set_lift(0);
        s_ee.pend_timer = lv_timer_create(_deferred_open_cb, 320, NULL);
        lv_timer_set_repeat_count(s_ee.pend_timer, 1);
    } else {
        _deferred_open_impl();
    }
}

static void _open_keyboard(void)
{
    if (keyboard_is_open()) return;
    _switch_to(PEND_KEYBOARD);
}

static void _open_date_picker(void)
{
    if (date_picker_is_open()) return;
    _switch_to(PEND_DATE);
}

static void _open_time_picker(void)
{
    if (time_picker_is_open()) return;
    _switch_to(PEND_TIME);
}

static void _close_all_sub_panels(void)
{
    if (s_ee.pend_timer) {
        lv_timer_delete(s_ee.pend_timer);
        s_ee.pend_timer = NULL;
    }
    s_ee.pend_type = PEND_NONE;
    if (keyboard_is_open())     keyboard_close_silent();
    if (date_picker_is_open())  date_picker_close_silent();
    if (time_picker_is_open())  time_picker_close_silent();
    _set_lift(0);
}

/* ── Callbacks ────────────────────────────────────────────────────────────── */

static void _kb_closed_cb(void *ud)
{
    (void)ud;
    if (s_ee.transition) {
        /* Closed programmatically as part of a panel switch; _switch_to handles lift. */
        s_ee.transition = false;
        return;
    }
    /* Keyboard dismissed by the user (drag or OK) — lower the sheet.
       Parent is NULL so keyboard.c didn't auto-lower; we do it here. */
    _set_lift(0);
    LOG_D(TAG, "Keyboard dismissed");
}

static void _on_date_selected(struct tm date, void *ud)
{
    (void)ud;
    s_ee.sel_year  = date.tm_year + 1900;
    s_ee.sel_month = date.tm_mon  + 1;
    s_ee.sel_day   = date.tm_mday;

    char buf[32];
    _fmt_date(buf, sizeof(buf));
    if (s_ee.lbl_date_field)
        lv_label_set_text(s_ee.lbl_date_field, buf);

    _update_save_btn();
    _set_lift(0);
    LOG_D(TAG, "Date selected: %04d-%02d-%02d", s_ee.sel_year, s_ee.sel_month, s_ee.sel_day);
}

static void _on_time_selected(int hour, int minute, void *ud)
{
    (void)ud;
    s_ee.sel_hour   = hour;
    s_ee.sel_minute = minute;

    char buf[16];
    _fmt_time(buf, sizeof(buf));
    if (s_ee.lbl_time_field)
        lv_label_set_text(s_ee.lbl_time_field, buf);

    _update_save_btn();
    _set_lift(0);
    LOG_D(TAG, "Time selected: %02d:%02d", hour, minute);
}

static void _on_title_tapped(lv_event_t *e)
{
    (void)e;
    _open_keyboard();
}

static void _on_date_field_tapped(lv_event_t *e)
{
    (void)e;
    _open_date_picker();
}

static void _on_time_field_tapped(lv_event_t *e)
{
    (void)e;
    _open_time_picker();
}

static void _on_ta_changed(lv_event_t *e)
{
    (void)e;
    _update_save_btn();
}

static void _on_repeat_btn(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_ee.sel_repeat = (cal_repeat_t)idx;
    _highlight_repeat(s_ee.sel_repeat);
    _update_save_btn();
    LOG_D(TAG, "Repeat set to %d (%s)", idx, REPEAT_LABELS[idx]);
}

static void _on_gc_done(bool ok, void *ctx)
{
    (void)ctx;
    if (ok) home_ha_bridge_refresh_calendar();
    else    LOG_W(TAG, "Google Calendar write failed");
}

static void _on_save(lv_event_t *e)
{
    (void)e;
    const char *title = s_ee.ta_title ? lv_textarea_get_text(s_ee.ta_title) : "";
    if (!title || title[0] == '\0') return;

    /* Capture roller position if the time picker is still open. */
    if (time_picker_is_open())
        time_picker_get_current(&s_ee.sel_hour, &s_ee.sel_minute);

    char date_str[11];
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d",
             s_ee.sel_year, s_ee.sel_month, s_ee.sel_day);

    _close_all_sub_panels();
    sheet_close();

    if (s_ee.is_edit && !s_ee.is_ha_event) {
        /* ── Local-only event — update calendar_service only ───────────────── */
        cal_event_t ev = {0};
        ev.id     = s_ee.edit_id;
        ev.hour   = s_ee.sel_hour;
        ev.minute = s_ee.sel_minute;
        ev.repeat = s_ee.sel_repeat;   /* TODO: repeat not implemented — stored but never expanded */
        snprintf(ev.title, sizeof(ev.title), "%s", title);
        snprintf(ev.date,  sizeof(ev.date),  "%s", date_str);
        calendar_service_update(&ev);
        LOG_I(TAG, "Event updated locally (id=%d uid=local): \"%s\" %s",
              ev.id, ev.title, ev.date);
        return;
    }

    /* ── Google Calendar create/update ─────────────────────────────────────── */
    gc_event_t gcev = {0};
    /* TODO: repeat not implemented — sel_repeat is not converted to an RRULE */
    snprintf(gcev.summary, sizeof(gcev.summary), "%s", title);
    snprintf(gcev.uid,     sizeof(gcev.uid),     "%s", s_ee.ha_uid);

    if (s_ee.sel_hour < 0) {
        gcev.all_day = true;
        snprintf(gcev.start_dt, sizeof(gcev.start_dt), "%s", date_str);

        struct tm et = {0};
        et.tm_year = s_ee.sel_year - 1900;
        et.tm_mon  = s_ee.sel_month - 1;
        et.tm_mday = s_ee.sel_day + 1;
        mktime(&et);
        snprintf(gcev.end_dt, sizeof(gcev.end_dt), "%04d-%02d-%02d",
                 et.tm_year + 1900, et.tm_mon + 1, et.tm_mday);
    } else {
        gcev.all_day = false;
        gc_fmt_datetime(gcev.start_dt, sizeof(gcev.start_dt),
                        s_ee.sel_year, s_ee.sel_month, s_ee.sel_day,
                        s_ee.sel_hour, s_ee.sel_minute);

        struct tm et = {0};
        et.tm_year = s_ee.sel_year - 1900;
        et.tm_mon  = s_ee.sel_month - 1;
        et.tm_mday = s_ee.sel_day;
        et.tm_hour = s_ee.sel_hour + 1;
        et.tm_min  = s_ee.sel_minute;
        mktime(&et);
        gc_fmt_datetime(gcev.end_dt, sizeof(gcev.end_dt),
                        et.tm_year + 1900, et.tm_mon + 1, et.tm_mday,
                        et.tm_hour, et.tm_min);
    }

    if (s_ee.is_ha_event) {
        gc_update_event(&gcev, _on_gc_done, NULL);
        LOG_I(TAG, "gc_update_event uid=%s: \"%s\" %s",
              gcev.uid[0] ? gcev.uid : "(no uid)", title, date_str);
    } else {
        gc_create_event(&gcev, _on_gc_done, NULL);
        LOG_I(TAG, "gc_create_event: \"%s\" %s", title, date_str);
    }
}

/* ── Sheet close callback ─────────────────────────────────────────────────── */

static void _on_sheet_close(void *ud)
{
    (void)ud;
    /* Fires before close animation — clean up sub-panels and stale pointers. */
    _close_all_sub_panels();
    s_ee.ta_title       = NULL;
    s_ee.lbl_date_field = NULL;
    s_ee.lbl_time_field = NULL;
    s_ee.btn_save       = NULL;
    memset(s_ee.repeat_btns, 0, sizeof(s_ee.repeat_btns));
}

/* ── Auto-keyboard one-shot timer ─────────────────────────────────────────── */

static void _auto_kb_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    if (!s_ee.ta_title) return;
    _open_keyboard();
}

/* ── Shared widget builder helpers ───────────────────────────────────────── */

static lv_obj_t *_make_field_row(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(row, 6, 0);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label_text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DIM), 0);

    return row;
}

static lv_obj_t *_make_tappable_field(lv_obj_t *parent, const char *text)
{
    lv_obj_t *field = lv_obj_create(parent);
    lv_obj_set_size(field, LV_PCT(100), EE_FIELD_H);
    lv_obj_set_style_bg_color(field, lv_color_hex(COL_INPUT), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_radius(field, EE_FIELD_R, 0);
    lv_obj_set_style_pad_hor(field, EE_FIELD_PAD_H, 0);
    lv_obj_set_style_pad_ver(field, 0, 0);
    lv_obj_remove_flag(field, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(field, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(field);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_align(lbl, LV_ALIGN_LEFT_MID);

    return field;
}

/* ── Content init (called by sheet_open) ──────────────────────────────────── */

static void _edit_content_init(lv_obj_t *parent)
{
    lv_obj_set_style_pad_hor(parent, EE_PAD_H, 0);
    lv_obj_set_style_pad_top(parent, EE_PAD_T, 0);
    lv_obj_set_style_pad_bottom(parent, EE_PAD_B, 0);
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(parent, EE_ROW_GAP, 0);

    /* ── Title row ──────────────────────────────────────────────────────── */
    lv_obj_t *title_row = _make_field_row(parent, "Title");

    lv_obj_t *ta = lv_textarea_create(title_row);
    lv_textarea_set_one_line(ta, true);
    lv_obj_set_size(ta, LV_PCT(100), EE_FIELD_H);
    lv_textarea_set_placeholder_text(ta, "Event title");
    lv_obj_set_style_bg_color(ta, lv_color_hex(COL_INPUT), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, EE_FIELD_R, 0);
    lv_obj_set_style_pad_hor(ta, EE_FIELD_PAD_H, 0);
    lv_obj_set_style_pad_ver(ta,
        (EE_FIELD_H - (int32_t)lv_font_get_line_height(&lv_font_montserrat_18)) / 2, 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(COL_TEXT), 0);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLLABLE);
    /* Cursor: invisible when not focused; blinking left-border line when focused */
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(ta, lv_color_hex(COL_ACCENT), LV_PART_CURSOR | LV_STATE_FOCUSED);

    if (s_ee.is_edit) {
        if (s_ee.edit_id > 0) {
            /* Local calendar_service event */
            cal_event_t existing = {0};
            if (calendar_service_get_by_id(s_ee.edit_id, &existing))
                lv_textarea_set_text(ta, existing.title);
        } else if (s_ee.ha_title[0]) {
            /* HA-origin event — title passed via ev->title */
            lv_textarea_set_text(ta, s_ee.ha_title);
        }
    }

    lv_obj_add_event_cb(ta, _on_title_tapped, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, _on_ta_changed,   LV_EVENT_VALUE_CHANGED, NULL);
    s_ee.ta_title = ta;

    /* ── Date row ───────────────────────────────────────────────────────── */
    lv_obj_t *date_row = _make_field_row(parent, "Date");
    char date_buf[32];
    _fmt_date(date_buf, sizeof(date_buf));
    lv_obj_t *date_field = _make_tappable_field(date_row, date_buf);
    lv_obj_add_event_cb(date_field, _on_date_field_tapped, LV_EVENT_CLICKED, NULL);
    s_ee.lbl_date_field = lv_obj_get_child(date_field, 0);

    /* ── Time row ───────────────────────────────────────────────────────── */
    lv_obj_t *time_row = _make_field_row(parent, "Time");
    char time_buf[16];
    _fmt_time(time_buf, sizeof(time_buf));
    lv_obj_t *time_field = _make_tappable_field(time_row, time_buf);
    lv_obj_add_event_cb(time_field, _on_time_field_tapped, LV_EVENT_CLICKED, NULL);
    s_ee.lbl_time_field = lv_obj_get_child(time_field, 0);

    /* ── Repeat row ─────────────────────────────────────────────────────── */
    lv_obj_t *rep_row = _make_field_row(parent, "Repeat");

    lv_obj_t *seg = lv_obj_create(rep_row);
    lv_obj_set_size(seg, LV_PCT(100), EE_SEG_H);
    lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(seg, 0, 0);
    lv_obj_set_style_pad_all(seg, 0, 0);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(seg, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(seg, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_pad_column(seg, 6, 0);

    for (int i = 0; i < REPEAT_COUNT; i++) {
        lv_obj_t *btn = lv_obj_create(seg);
        lv_obj_set_height(btn, EE_SEG_H);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_radius(btn, EE_SEG_R, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, REPEAT_LABELS[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_align(lbl, LV_ALIGN_CENTER);

        lv_obj_add_event_cb(btn, _on_repeat_btn, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_ee.repeat_btns[i] = btn;
    }
    _highlight_repeat(s_ee.sel_repeat);

    /* ── Save button ────────────────────────────────────────────────────── */
    lv_obj_t *spacer = lv_obj_create(parent);
    lv_obj_set_size(spacer, LV_PCT(100), EE_SECTION_GAP);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_set_style_pad_all(spacer, 0, 0);
    lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *save = lv_obj_create(parent);
    lv_obj_set_size(save, LV_PCT(100), EE_SAVE_H);
    lv_obj_set_style_bg_color(save, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(save, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(save, 0, 0);
    lv_obj_set_style_radius(save, EE_SAVE_R, 0);
    lv_obj_set_style_pad_all(save, 0, 0);
    lv_obj_remove_flag(save, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(save, _on_save, LV_EVENT_CLICKED, NULL);
    s_ee.btn_save = save;

    lv_obj_t *save_lbl = lv_label_create(save);
    lv_label_set_text(save_lbl, "Save");
    lv_obj_set_style_text_font(save_lbl, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(save_lbl, lv_color_hex(COL_TEXT_MUTE), 0);
    lv_obj_set_align(save_lbl, LV_ALIGN_CENTER);

    /* Set initial save-button state (disabled until title typed) */
    _update_save_btn();

    /* Auto-open keyboard in create mode via a 1-shot timer so the sheet's
       is_open flag is set before keyboard_open() calls sheet_lift().       */
    if (s_ee.auto_keyboard) {
        lv_timer_t *t = lv_timer_create(_auto_kb_timer_cb, 1, NULL);
        lv_timer_set_repeat_count(t, 1);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void event_edit_open_create(int year, int month_1, int day)
{
    memset(&s_ee, 0, sizeof(s_ee));
    s_ee.is_edit      = false;
    s_ee.sel_year     = year;
    s_ee.sel_month    = month_1;
    s_ee.sel_day      = day;
    s_ee.sel_hour     = -1;   /* no time */
    s_ee.sel_repeat   = CAL_REPEAT_NONE;
    s_ee.auto_keyboard = true;

    sheet_open(_edit_content_init);
    sheet_set_close_cb(_on_sheet_close, NULL);
    LOG_D(TAG, "Edit sheet opened (create) for %04d-%02d-%02d", year, month_1, day);
}

void event_edit_open_edit(const cal_event_t *ev)
{
    memset(&s_ee, 0, sizeof(s_ee));
    s_ee.is_edit    = true;
    s_ee.is_ha_event = (ev->id == 0);   /* id==0 means HA-origin, not in local store */
    s_ee.edit_id   = ev->id;
    snprintf(s_ee.ha_uid,   sizeof(s_ee.ha_uid),   "%s", ev->uid);
    snprintf(s_ee.ha_title, sizeof(s_ee.ha_title), "%s", ev->title);
    s_ee.sel_hour  = ev->hour;
    s_ee.sel_minute = ev->minute;
    s_ee.sel_repeat = ev->repeat;
    s_ee.auto_keyboard = false;

    /* Parse date */
    sscanf(ev->date, "%d-%d-%d", &s_ee.sel_year, &s_ee.sel_month, &s_ee.sel_day);

    sheet_open(_edit_content_init);
    sheet_set_close_cb(_on_sheet_close, NULL);
    LOG_D(TAG, "Edit sheet opened (edit) id=%d uid=%s \"%s\"",
          ev->id, ev->uid[0] ? ev->uid : "local", ev->title);
}
