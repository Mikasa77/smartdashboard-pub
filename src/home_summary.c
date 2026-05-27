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

#include "home_summary.h"
#include "ha_entities.h"
#include "clock_service.h"
#include "dashboard_colours.h"
#include "dashboard_icons.h"
#include "ha_ws_client.h"
#include "dashboard_log.h"
#include "dashboard_lv_utils.h"
#include "lvgl.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

LV_FONT_DECLARE(inter_60_600);

static const char *TAG = "home_summary";

/* ── Condition chip handle ────────────────────────────────────────────────── */

typedef struct {
    lv_obj_t *img_icon;
    lv_obj_t *lbl_value;
    lv_obj_t *lbl_label;
} condition_chip_t;

static condition_chip_t s_chip_weather;
static condition_chip_t s_chip_pollen;
static condition_chip_t s_chip_uv;

static lv_obj_t *s_lbl_heat_temp;
static lv_obj_t *s_lbl_kit_temp;
static lv_obj_t *s_lbl_kit_humidity;

/* ── Weather state helpers ────────────────────────────────────────────────── */

/* Pick the best available icon for a HA weather condition string. */
static const lv_image_dsc_t *weather_icon(const char *state)
{
    if (strcmp(state, "sunny") == 0 || strcmp(state, "clear-night") == 0)
        return &ICON_SUN_UV;
    if (strcmp(state, "lightning") == 0 || strcmp(state, "lightning-rainy") == 0)
        return &ICON_ZAP_POWER;
    if (strcmp(state, "rainy") == 0 || strcmp(state, "pouring") == 0)
        return &ICON_CLOUD_RAIN_RAIN;
    if (strcmp(state, "snowy") == 0 || strcmp(state, "snowy-rainy") == 0)
        return &ICON_SNOWFLAKE_SNOW;
    if (strcmp(state, "fog") == 0)
        return &ICON_CLOUD_FOG_FOG;
    if (strcmp(state, "windy") == 0 || strcmp(state, "windy-variant") == 0)
        return &ICON_WIND_WINDY;
    if (strcmp(state, "hail") == 0)
        return &ICON_CLOUD_HAIL_HAIL;
    if (strcmp(state, "partlycloudy") == 0)
        return &ICON_CLOUD_SUN_PARTLYCLOUDY;
    /* cloudy, exceptional, and any unknown state fall back to cloud */
    return &ICON_CLOUD_WEATHER;
}

/* Map HA weather condition strings to display-friendly labels. */
static const char *weather_label(const char *state)
{
    if (strcmp(state, "sunny") == 0)            return "Sunny";
    if (strcmp(state, "clear-night") == 0)      return "Clear";
    if (strcmp(state, "partlycloudy") == 0)     return "Part cloudy";
    if (strcmp(state, "cloudy") == 0)           return "Cloudy";
    if (strcmp(state, "rainy") == 0)            return "Rainy";
    if (strcmp(state, "pouring") == 0)          return "Pouring";
    if (strcmp(state, "snowy") == 0)            return "Snowy";
    if (strcmp(state, "snowy-rainy") == 0)      return "Sleet";
    if (strcmp(state, "fog") == 0)              return "Foggy";
    if (strcmp(state, "hail") == 0)             return "Hail";
    if (strcmp(state, "lightning") == 0)        return "Thunder";
    if (strcmp(state, "lightning-rainy") == 0)  return "Storms";
    if (strcmp(state, "windy") == 0)            return "Windy";
    if (strcmp(state, "windy-variant") == 0)    return "Windy";
    if (strcmp(state, "exceptional") == 0)      return "Unusual";
    return state; /* fallback: raw HA state string */
}

/* Capitalise the first letter of a pollen/risk level string. */
static const char *pollen_label(const char *state)
{
    if (strcmp(state, "none") == 0)      return "None";
    if (strcmp(state, "low") == 0)       return "Low";
    if (strcmp(state, "moderate") == 0)  return "Moderate";
    if (strcmp(state, "high") == 0)      return "High";
    if (strcmp(state, "very_high") == 0) return "Very high";
    return state;
}

/* ── HA callbacks (fire on LVGL thread) ───────────────────────────────────── */

static void on_weather(const ha_state_t *s, void *ud)
{
    (void)ud;

    /* Condition label + icon */
    lv_label_set_text(s_chip_weather.lbl_label, weather_label(s->state));
    lv_image_set_src(s_chip_weather.img_icon, weather_icon(s->state));

    /* Parse attributes for temperature and UV index */
    if (s->attributes_json && s->attributes_json[0] != '\0') {
        cJSON *attrs = cJSON_Parse(s->attributes_json);
        if (attrs) {
            cJSON *temp = cJSON_GetObjectItem(attrs, "temperature");
            if (cJSON_IsNumber(temp)) {
                char buf[12];
                snprintf(buf, sizeof(buf), "%.0f\xc2\xb0\x43", temp->valuedouble); /* °C */
                lv_label_set_text(s_chip_weather.lbl_value, buf);
                LOG_D(TAG, "Weather: %s %.0f°C", s->state, temp->valuedouble);
            }
            cJSON *uv = cJSON_GetObjectItem(attrs, "uv_index");
            if (cJSON_IsNumber(uv)) {
                int idx = (int)uv->valuedouble;
                const char *uv_label = idx <= 2  ? "Low"
                                     : idx <= 5  ? "Moderate"
                                     : idx <= 7  ? "High"
                                     : idx <= 10 ? "Very High"
                                     :             "Extreme";
                lv_label_set_text(s_chip_uv.lbl_value, uv_label);
                LOG_D(TAG, "UV index: %d (%s)", idx, uv_label);
            }
            cJSON_Delete(attrs);
        }
    }
}

static void on_pollen(const ha_state_t *s, void *ud)
{
    (void)ud;
    lv_label_set_text(s_chip_pollen.lbl_value, pollen_label(s->state));
    LOG_D(TAG, "Pollen: %s", s->state);
}

static void on_heating(const ha_state_t *s, void *ud)
{
    (void)ud;
    if (!s->attributes_json || s->attributes_json[0] == '\0') return;
    cJSON *attrs = cJSON_Parse(s->attributes_json);
    if (!attrs) return;
    cJSON *target = cJSON_GetObjectItem(attrs, "temperature");
    if (cJSON_IsNumber(target)) {
        char buf[10];
        snprintf(buf, sizeof(buf), "%.0f\xc2\xb0\x43", target->valuedouble);
        lv_label_set_text(s_lbl_heat_temp, buf);
        LOG_D(TAG, "Heating target: %.0f°C", target->valuedouble);
    }
    cJSON_Delete(attrs);
}

static void on_kitchen_temp(const ha_state_t *s, void *ud)
{
    (void)ud;
    char buf[10];
    snprintf(buf, sizeof(buf), "%.1f\xc2\xb0\x43", atof(s->state));
    lv_label_set_text(s_lbl_kit_temp, buf);
    LOG_D(TAG, "Kitchen temp: %s", s->state);
}

static void on_kitchen_humidity(const ha_state_t *s, void *ud)
{
    (void)ud;
    char buf[16];
    snprintf(buf, sizeof(buf), "Humidity %d%%", (int)(atof(s->state) + 0.5));
    lv_label_set_text(s_lbl_kit_humidity, buf);
    LOG_D(TAG, "Kitchen humidity: %s", s->state);
}

static void on_condition_icon(const ha_state_t *s, void *ud)
{
    (void)ud;
    if (!s_chip_weather.img_icon) return;

    const char *token = s->state;
    LOG_D(TAG, "Condition icon: %s", token);

    const lv_image_dsc_t *src = &ICON_CLOUD_WEATHER;
    if (strcmp(token, "frost") == 0) {
        src = &ICON_SNOWFLAKE_SNOW;
    } else if (strcmp(token, "rain") == 0) {
        src = &ICON_UMBRELLA_RAIN;
    } else if (strcmp(token, "cold_drop") == 0) {
        src = &ICON_THERMOMETER_COLD_DROP;
    } else if (strcmp(token, "high_pollen") == 0) {
        src = &ICON_FLOWER_HIGH_POLLEN;
    }

    lv_image_set_src(s_chip_weather.img_icon, src);
}

/* ── Condition chip builder ───────────────────────────────────────────────── */

static condition_chip_t make_chip(lv_obj_t *parent, const lv_image_dsc_t *icon,
                                   const char *init_label)
{
    condition_chip_t chip = {0};

    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(card, 1);
    obj_card(card);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 14, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(card, 6, 0);

    /* Icon — colour is baked into the SVG; do not apply recolor */
    lv_obj_t *img = lv_image_create(card);
    lv_image_set_src(img, icon);
    chip.img_icon = img;

    /* Value — large, starts as "--" */
    lv_obj_t *lbl_val = lv_label_create(card);
    lv_label_set_text(lbl_val, "--");
    lv_obj_set_style_text_font(lbl_val, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(COL_TEXT), 0);
    chip.lbl_value = lbl_val;

    /* Label — dim, describes what the value is */
    lv_obj_t *lbl_label = lv_label_create(card);
    lv_label_set_text(lbl_label, init_label);
    lv_obj_set_style_text_font(lbl_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_label, lv_color_hex(COL_TEXT_DIM), 0);
    chip.lbl_label = lbl_label;

    return chip;
}

/* ── Top row: clock column + 3 condition chips ────────────────────────────── */

static void build_top_row(lv_obj_t *parent,
                           lv_obj_t **lbl_time_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    obj_clear(row);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(row, LV_FLEX_ALIGN_START, 0);
    lv_obj_set_style_pad_column(row, HOME_CHIP_GAP, 0);

    /* Clock column — grows to fill space left of the chips */
    lv_obj_t *clock_col = lv_obj_create(row);
    lv_obj_set_height(clock_col, LV_SIZE_CONTENT);
    obj_clear(clock_col);
    lv_obj_set_flex_grow(clock_col, 1);
    lv_obj_set_layout(clock_col, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(clock_col, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(clock_col, 10, 0);

    lv_obj_t *lbl_time = lv_label_create(clock_col);
    lv_label_set_text(lbl_time, "00:00");
    lv_obj_set_style_text_font(lbl_time, &inter_60_600, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_letter_space(lbl_time, -2, 0);

    *lbl_time_out = lbl_time;

    /* Condition chips — show "--" until first HA update */
    s_chip_weather = make_chip(row, &ICON_CLOUD_WEATHER, "Weather");
    s_chip_pollen  = make_chip(row, &ICON_LEAF_POLLEN,   "Pollen");
    s_chip_uv      = make_chip(row, &ICON_SUN_UV,        "UV index");

}

/* ── Indoor summary: heating target + kitchen current ─────────────────────── */

static lv_obj_t *make_indoor_card(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    obj_card(card);
    lv_obj_set_style_pad_hor(card, 22, 0);
    lv_obj_set_style_pad_ver(card, 20, 0);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    return card;
}

static void build_indoor_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    obj_clear(row);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_pad_column(row, 16, 0);

    /* Heating card */
    lv_obj_t *heat = make_indoor_card(row);

    lv_obj_t *heat_head = lv_label_create(heat);
    lv_label_set_text(heat_head, "Heating");
    lv_obj_set_style_text_font(heat_head, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(heat_head, lv_color_hex(COL_TEXT_DIM), 0);

    s_lbl_heat_temp = lv_label_create(heat);
    lv_label_set_text(s_lbl_heat_temp, "--");
    lv_obj_set_style_text_font(s_lbl_heat_temp, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_lbl_heat_temp, lv_color_hex(COL_ACCENT), 0);

    lv_obj_t *heat_sub = lv_label_create(heat);
    lv_label_set_text(heat_sub, "Target");
    lv_obj_set_style_text_font(heat_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(heat_sub, lv_color_hex(COL_TEXT_DIM), 0);

    /* Kitchen card */
    lv_obj_t *kit = make_indoor_card(row);

    /* Kitchen header row: title left, humidity right */
    lv_obj_t *kit_hdr = lv_obj_create(kit);
    lv_obj_set_size(kit_hdr, LV_PCT(100), LV_SIZE_CONTENT);
    obj_clear(kit_hdr);
    lv_obj_set_layout(kit_hdr, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(kit_hdr, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_main_place(kit_hdr, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_flex_cross_place(kit_hdr, LV_FLEX_ALIGN_START, 0);

    lv_obj_t *kit_head = lv_label_create(kit_hdr);
    lv_label_set_text(kit_head, "Kitchen");
    lv_obj_set_style_text_font(kit_head, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(kit_head, lv_color_hex(COL_TEXT_DIM), 0);

    s_lbl_kit_humidity = lv_label_create(kit_hdr);
    lv_label_set_text(s_lbl_kit_humidity, "--");
    lv_obj_set_style_text_font(s_lbl_kit_humidity, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lbl_kit_humidity, lv_color_hex(COL_TEXT_DIM), 0);

    s_lbl_kit_temp = lv_label_create(kit);
    lv_label_set_text(s_lbl_kit_temp, "--");
    lv_obj_set_style_text_font(s_lbl_kit_temp, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_lbl_kit_temp, lv_color_hex(COL_TEXT), 0);

    lv_obj_t *kit_sub = lv_label_create(kit);
    lv_label_set_text(kit_sub, "Current");
    lv_obj_set_style_text_font(kit_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(kit_sub, lv_color_hex(COL_TEXT_DIM), 0);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void home_summary_init(lv_obj_t *parent, lv_obj_t **out_lbl_time)
{
    /* Caller has already constructed the top group with FLEX column + row gap.
       We just append our content. */
    build_top_row(parent, out_lbl_time);
    build_indoor_row(parent);
}

void home_summary_register_subscriptions(void)
{
    ha_subscribe(HA_ENTITY_WEATHER,          on_weather,          NULL);
    ha_subscribe(HA_ENTITY_POLLEN,           on_pollen,           NULL);
    ha_subscribe(HA_ENTITY_HEATING,          on_heating,          NULL);
    ha_subscribe(HA_ENTITY_KITCHEN_TEMP,     on_kitchen_temp,     NULL);
    ha_subscribe(HA_ENTITY_KITCHEN_HUMIDITY, on_kitchen_humidity, NULL);
    ha_subscribe(HA_ENTITY_CONDITION_ICON,   on_condition_icon,   NULL);
}
