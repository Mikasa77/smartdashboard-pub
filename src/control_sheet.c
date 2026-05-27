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

#include "control_sheet.h"
#include "config_sheet.h"
#include "dashboard_colours.h"
#include "dashboard_config.h"
#include "dashboard_log.h"
#include "sheet.h"
#include "lvgl.h"

#ifdef ESP_PLATFORM
#include "bsp/esp-bsp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#endif

static const char *TAG = "control_sheet";

/* ── Row geometry ─────────────────────────────────────────────────────────── */

#define ROW_H       56   /* standard row height — satisfies ≥ 48 px touch target */
#define PAD_H       24   /* horizontal padding                                    */
#define SLIDER_H    48   /* slider row height                                     */

/* ── Restart section state ────────────────────────────────────────────────── */

static lv_obj_t *s_confirm_row = NULL;   /* inline confirmation row (hidden by default) */

/* ── Helpers: style a bare flat row ──────────────────────────────────────── */

static void _style_row(lv_obj_t *row, bool top_border)
{
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, top_border ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
}

/* Label centred vertically in a fixed-height row (avoids chicken-and-egg
   alignment issue described in CONTEXT.md). */
static lv_obj_t *_add_label(lv_obj_t *parent, const char *text,
                             lv_color_t colour, const lv_font_t *font)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, colour, LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, font, LV_PART_MAIN);
    lv_obj_set_height(lbl, ROW_H);
    lv_obj_set_style_pad_top(lbl, (ROW_H - font->line_height) / 2, LV_PART_MAIN);
    lv_obj_set_pos(lbl, 0, 0);
    return lbl;
}

/* ── 1. Restart ───────────────────────────────────────────────────────────── */

static void _confirm_restart_cb(lv_event_t *e)
{
    (void)e;
    LOG_I(TAG, "Restart confirmed");
#ifdef ESP_PLATFORM
    esp_restart();
#endif
}

static void _cancel_restart_cb(lv_event_t *e)
{
    (void)e;
    if (s_confirm_row) {
        lv_obj_add_flag(s_confirm_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _restart_tap_cb(lv_event_t *e)
{
    (void)e;
    LOG_I(TAG, "Restart tapped — showing confirmation");
    if (s_confirm_row) {
        lv_obj_remove_flag(s_confirm_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void _build_restart_section(lv_obj_t *parent)
{
    /* ── Primary row: "Restart" ── */
    lv_obj_t *row = lv_obj_create(parent);
    _style_row(row, false);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_BORDER),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, _restart_tap_cb, LV_EVENT_CLICKED, NULL);

    _add_label(row, "Restart",
               lv_color_hex(COL_TEXT), &lv_font_montserrat_16);

    /* ── Inline confirmation row (hidden until Restart is tapped) ── */
    lv_obj_t *conf = lv_obj_create(parent);
    lv_obj_set_size(conf, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(conf, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(conf, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(conf, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(conf, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(conf, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(conf, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(conf, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_flag(conf, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(conf, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(conf, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
    lv_obj_set_style_flex_main_place(conf, LV_FLEX_ALIGN_SPACE_AROUND, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(conf, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(conf, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(conf, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(conf, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(conf, 0, LV_PART_MAIN);
    lv_obj_add_flag(conf, LV_OBJ_FLAG_HIDDEN);
    s_confirm_row = conf;

    /* "Confirm restart" button — amber */
    lv_obj_t *btn_confirm = lv_obj_create(conf);
    lv_obj_set_size(btn_confirm, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_confirm, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_confirm, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_confirm, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_confirm, 8, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn_confirm, 0,
                                   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_flag(btn_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn_confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_confirm, _confirm_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_confirm = lv_label_create(btn_confirm);
    lv_label_set_text(lbl_confirm, "Confirm restart");
    lv_obj_set_style_text_color(lbl_confirm, lv_color_hex(COL_ACCENT),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_confirm, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(lbl_confirm);

    /* "Cancel" button — dim text */
    lv_obj_t *btn_cancel = lv_obj_create(conf);
    lv_obj_set_size(btn_cancel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_cancel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_cancel, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_cancel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn_cancel, 8, LV_PART_MAIN);
    lv_obj_set_style_outline_width(btn_cancel, 0,
                                   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_flag(btn_cancel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(btn_cancel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn_cancel, _cancel_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(COL_TEXT_DIM),
                                LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_cancel, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(lbl_cancel);
}

/* ── 2. Brightness ────────────────────────────────────────────────────────── */

static void _brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int32_t   val    = lv_slider_get_value(slider);
    LOG_I(TAG, "Brightness → %d%%", (int)val);
#ifdef ESP_PLATFORM
    bsp_display_brightness_set((int)val);
#endif
}

static void _build_brightness_section(lv_obj_t *parent)
{
    /* Label row */
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_START, LV_PART_MAIN);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(row, 6, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Brightness");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);

    /* Slider */
    lv_obj_t *slider = lv_slider_create(row);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, SLIDER_H / 3);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 80, LV_ANIM_OFF);

    /* Slider track: COL_BORDER background, COL_ACCENT indicator */
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ACCENT),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_TEXT), LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);

    lv_obj_add_event_cb(slider, _brightness_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ── 3. WiFi status ───────────────────────────────────────────────────────── */

static void _wifi_tap_cb(lv_event_t *e)
{
    (void)e;
#ifdef ESP_PLATFORM
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        LOG_I(TAG, "WiFi: connected to '%s' (RSSI %d)", (char *)ap.ssid, ap.rssi);
    } else {
        LOG_I(TAG, "WiFi: not connected");
    }
#else
    LOG_I(TAG, "WiFi: Simulator — no real connection");
#endif
}

static void _build_wifi_section(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    _style_row(row, true);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_BORDER),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 12, LV_PART_MAIN);
    lv_obj_add_event_cb(row, _wifi_tap_cb, LV_EVENT_CLICKED, NULL);

    /* WiFi icon (using built-in LVGL symbol) */
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(icon, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_16, LV_PART_MAIN);

    /* SSID label */
    lv_obj_t *lbl = lv_label_create(row);
#ifdef ESP_PLATFORM
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lv_label_set_text(lbl, (char *)ap.ssid);
    } else {
        lv_label_set_text(lbl, "Not connected");
    }
#else
    lv_label_set_text(lbl, "Simulator");
#endif
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
}

/* ── 4. Sleep / Display off ───────────────────────────────────────────────── */

static void _sleep_now_cb(lv_event_t *e)
{
    (void)e;
    LOG_I(TAG, "Sleep now");
#ifdef ESP_PLATFORM
    bsp_display_backlight_off();
#endif
}

static void _sleep_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw    = lv_event_get_target(e);
    bool      state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashboard_config_set_sleep_enabled(state);
    dashboard_config_save();
    LOG_I(TAG, "Sleep toggle → %s", state ? "on" : "off");
}

static void _build_sleep_section(lv_obj_t *parent)
{
    /* Toggle row */
    lv_obj_t *row = lv_obj_create(parent);
    _style_row(row, true);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                     LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Display Sleep");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_ACCENT),
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    if (dashboard_config_get_sleep_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, _sleep_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* "Sleep now" secondary action row */
    lv_obj_t *row2 = lv_obj_create(parent);
    lv_obj_set_size(row2, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_color(row2, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row2, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row2, lv_color_hex(COL_BORDER),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_radius(row2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row2, PAD_H + 16, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row2, PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row2, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row2, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row2, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row2, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row2, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row2, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_flag(row2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(row2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row2, _sleep_now_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl2 = lv_label_create(row2);
    lv_label_set_text(lbl2, "Sleep now");
    lv_obj_set_style_text_color(lbl2, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl2, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_height(lbl2, ROW_H);
    lv_obj_set_style_pad_top(lbl2,
                             (ROW_H - lv_font_montserrat_14.line_height) / 2,
                             LV_PART_MAIN);
    lv_obj_set_pos(lbl2, 0, 0);
}

/* ── 5. Do Not Disturb ────────────────────────────────────────────────────── */

static void _dnd_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw    = lv_event_get_target(e);
    bool      state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashboard_config_set_dnd_enabled(state);
    dashboard_config_save();
    LOG_I(TAG, "DND → %s", state ? "on" : "off");
}

static void _build_dnd_section(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    _style_row(row, true);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                                     LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, "Do Not Disturb");
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_ACCENT),
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    if (dashboard_config_get_dnd_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, _dnd_toggle_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ── 6. Config button ─────────────────────────────────────────────────────── */

static void _open_config_timer_cb(lv_timer_t *t)
{
    lv_timer_delete(t);
    config_sheet_open();
}

static void _open_config_deferred(void *user_data)
{
    (void)user_data;
    lv_timer_create(_open_config_timer_cb, 320, NULL);
}

static void _config_cb(lv_event_t *e)
{
    (void)e;
    LOG_I(TAG, "Config tapped — opening Config Sheet");
    sheet_set_close_cb(_open_config_deferred, NULL);
    sheet_close();
}

static void _build_config_section(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    _style_row(row, true);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_BORDER),
                              LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_add_event_cb(row, _config_cb, LV_EVENT_CLICKED, NULL);

    _add_label(row, "Config",
               lv_color_hex(COL_TEXT), &lv_font_montserrat_16);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void control_sheet_init(lv_obj_t *parent)
{
    /* Reset confirm-row pointer — it belongs to this sheet instance */
    s_confirm_row = NULL;

    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(parent, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 0, LV_PART_MAIN);

    _build_restart_section(parent);
    _build_brightness_section(parent);
    _build_wifi_section(parent);
    _build_sleep_section(parent);
    _build_dnd_section(parent);
    _build_config_section(parent);
}

void control_sheet_create_hint_pill(lv_obj_t *screen, int32_t nav_bottom_y)
{
    lv_obj_t *pill = lv_obj_create(screen);
    lv_obj_set_size(pill, CTRL_HINT_PILL_W, CTRL_HINT_PILL_H);
    lv_obj_set_pos(pill, (800 - CTRL_HINT_PILL_W) / 2, nav_bottom_y);
    lv_obj_set_style_bg_color(pill, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(pill, CTRL_HINT_PILL_H / 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(pill, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(pill, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    LOG_I(TAG, "Hint pill created at y=%d", (int)nav_bottom_y);
}
