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

#include "config_sheet.h"
#include "sheet.h"
#include "ha_credentials.h"
#include "ha_entities.h"
#include "ha_ws_client.h"
#include "dashboard_colours.h"
#include "dashboard_config.h"
#include "dashboard_log.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_netif.h"
#endif

static const char *TAG = "config_sheet";

/* ── Entity ID table ──────────────────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *entity_id;
} cfg_entity_row_t;

static const cfg_entity_row_t s_entities[] = {
    { "Calendar",         HA_ENTITY_CALENDAR          },
    { "Weather",          HA_ENTITY_WEATHER            },
    { "Pollen",           HA_ENTITY_POLLEN             },
    { "UV",               HA_ENTITY_UV                 },
    { "Heating",          HA_ENTITY_HEATING            },
    { "Kitchen Temp",     HA_ENTITY_KITCHEN_TEMP       },
    { "Kitchen Humidity", HA_ENTITY_KITCHEN_HUMIDITY   },
    { "Presence",         HA_ENTITY_PRESENCE           },
    { "Condition Icon",   HA_ENTITY_CONDITION_ICON     },
    { "Sonos",            HA_ENTITY_MEDIA_SONOS        },
    { "Pete Spotify",     HA_ENTITY_MEDIA_PETE_SPOTIFY },
    { "Jude Spotify",     HA_ENTITY_MEDIA_JUDE_SPOTIFY },
};

#define ENTITY_COUNT  ((int)(sizeof(s_entities) / sizeof(s_entities[0])))

/* ── Layout helpers ───────────────────────────────────────────────────────── */

/* Horizontal padding inside the sheet */
#define CFG_PAD_H  24

static void _make_section_header(lv_obj_t *parent, const char *title)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), CFG_SHEET_SECTION_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_align(lbl, LV_ALIGN_LEFT_MID);
}

static void _make_kv_row(lv_obj_t *parent, const char *key, const char *value,
                         bool top_border)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), CFG_SHEET_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, top_border ? 1 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    /* Key label — left-aligned */
    lv_obj_t *key_lbl = lv_label_create(row);
    lv_label_set_text(key_lbl, key);
    lv_obj_set_style_text_color(key_lbl, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(key_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_align(key_lbl, LV_ALIGN_LEFT_MID);

    /* Value label — right-aligned */
    lv_obj_t *val_lbl = lv_label_create(row);
    lv_label_set_text(val_lbl, value);
    lv_label_set_long_mode(val_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(val_lbl, LV_PCT(60));
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_align(val_lbl, LV_ALIGN_RIGHT_MID);
}

static void _make_error_row(lv_obj_t *parent, const char *msg)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), CFG_SHEET_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, CFG_PAD_H + 12, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, msg);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_align(lbl, LV_ALIGN_LEFT_MID);
}

/* ── Use HTTPS / Verify cert toggles ─────────────────────────────────────── */

static void _use_tls_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw    = lv_event_get_target(e);
    bool      state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashboard_config_set_ha_use_tls(state);
    dashboard_config_save();
    ha_ws_client_reconnect();
}

static void _verify_cert_toggle_cb(lv_event_t *e)
{
    lv_obj_t *sw    = lv_event_get_target(e);
    bool      state = lv_obj_has_state(sw, LV_STATE_CHECKED);
    dashboard_config_set_ha_verify_cert(state);
    dashboard_config_save();
    ha_ws_client_reconnect();
}

static void _make_toggle_row(lv_obj_t *parent, const char *label,
                              bool checked, lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), CFG_SHEET_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_set_style_outline_width(row, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
    lv_obj_set_style_flex_main_place(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, label);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_flex_grow(lbl, 1);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(COL_ACCENT),
                              LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(sw, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    if (checked)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ── Reconfigure button callback ──────────────────────────────────────────── */

static void _reconfigure_cb(lv_event_t *e)
{
    (void)e;
    LOG_I(TAG, "Reconfigure tapped");
    /* TODO (Firmware): fetch panel IP, render QR code for http://<ip>/config */
    /* Simulator: URL already shown as a label below the button */
}

/* ── Content init (passed to sheet_open) ─────────────────────────────────── */

static void _content_init(lv_obj_t *parent)
{
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(parent, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN);
    lv_obj_set_style_pad_all(parent, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 0, LV_PART_MAIN);

    /* ── 1. HA Connection section ─────────────────────────────────────────── */

    _make_section_header(parent, "HA Connection");

    const char *host = ha_credentials_get_host();
    _make_kv_row(parent, "Host", host ? host : "(not set)", false);

    const char *conn_err = ha_ws_get_conn_error();
    if (conn_err && conn_err[0] != '\0')
        _make_error_row(parent, conn_err);

    _make_toggle_row(parent, "Use HTTPS",
                     dashboard_config_get_ha_use_tls(),
                     _use_tls_toggle_cb);

    _make_toggle_row(parent, "Verify Certificate",
                     dashboard_config_get_ha_verify_cert(),
                     _verify_cert_toggle_cb);

    /* Reconfigure row */
    lv_obj_t *reconfig_row = lv_obj_create(parent);
    lv_obj_set_size(reconfig_row, LV_PCT(100), CFG_SHEET_ROW_H);
    lv_obj_set_style_bg_color(reconfig_row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(reconfig_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(reconfig_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(reconfig_row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(reconfig_row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(reconfig_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(reconfig_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(reconfig_row, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_width(reconfig_row, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(reconfig_row, LV_BORDER_SIDE_TOP, LV_PART_MAIN);
    lv_obj_remove_flag(reconfig_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *reconfig_lbl = lv_label_create(reconfig_row);
    lv_label_set_text(reconfig_lbl, "Reconfigure");
    lv_obj_set_style_text_color(reconfig_lbl, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(reconfig_lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_align(reconfig_lbl, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(reconfig_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(reconfig_row, lv_color_hex(COL_BORDER),
                               LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_outline_width(reconfig_row, 0,
                                   LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_add_event_cb(reconfig_row, _reconfigure_cb, LV_EVENT_CLICKED, NULL);

#ifdef SIMULATOR
    /* On Simulator show the URL as text (no QR library available) */
    lv_obj_t *url_row = lv_obj_create(parent);
    lv_obj_set_size(url_row, LV_PCT(100), CFG_SHEET_ROW_H);
    lv_obj_set_style_bg_color(url_row, lv_color_hex(COL_SHEET), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(url_row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(url_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(url_row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(url_row, CFG_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(url_row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(url_row, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(url_row, 0, LV_PART_MAIN);
    lv_obj_remove_flag(url_row, LV_OBJ_FLAG_SCROLLABLE);

    char url_buf[128];
    snprintf(url_buf, sizeof(url_buf), "http://<panel-ip>/config");
    lv_obj_t *url_lbl = lv_label_create(url_row);
    lv_label_set_text(url_lbl, url_buf);
    lv_label_set_long_mode(url_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(url_lbl, LV_PCT(100));
    lv_obj_set_style_text_color(url_lbl, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(url_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_align(url_lbl, LV_ALIGN_LEFT_MID);
#endif /* SIMULATOR */

    /* ── 2. Device section ───────────────────────────────────────────────── */

    _make_section_header(parent, "Device");

    char mdns_str[80];
    snprintf(mdns_str, sizeof(mdns_str), "%s.local",
             dashboard_config_get_panel_hostname());

    char ip_str[20];
#ifdef ESP_PLATFORM
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK)
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    else
        snprintf(ip_str, sizeof(ip_str), "Not connected");
#else
    snprintf(ip_str, sizeof(ip_str), "Simulator");
#endif

    _make_kv_row(parent, "Hostname", mdns_str, false);
    _make_kv_row(parent, "IP", ip_str, true);

    /* ── 3. Entity IDs section ────────────────────────────────────────────── */

    _make_section_header(parent, "Entity IDs");

    for (int i = 0; i < ENTITY_COUNT; i++) {
        _make_kv_row(parent, s_entities[i].name, s_entities[i].entity_id,
                     i > 0);
    }
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void config_sheet_open(void)
{
    LOG_I(TAG, "Opening config sheet");
    sheet_open(_content_init);
}

void config_sheet_close(void)
{
    LOG_I(TAG, "Closing config sheet");
    sheet_close();
}
