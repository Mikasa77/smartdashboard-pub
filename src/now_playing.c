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

#include "now_playing.h"
#include "now_playing_layout.h"
#include "shell.h"
#include "media_art.h"
#include "ha_ws_client.h"
#include "ha_credentials.h"
#include "ha_entities.h"
#include "dashboard_colours.h"
#include "dashboard_icons.h"
#include "dashboard_log.h"
#include "cJSON.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "now_playing";

/* ── State ───────────────────────────────────────────────────────────────── */

static char   s_entity_id[64]        = HA_ENTITY_MEDIA_SONOS;
static char   s_cur_art_path[256]    = {0};

static float  s_media_position       = 0.0f;
static float  s_media_duration       = 0.0f;

/* ── Widget handles ──────────────────────────────────────────────────────── */

static lv_obj_t *s_card        = NULL;
static lv_obj_t *s_art_img     = NULL;
static lv_obj_t *s_lbl_title   = NULL;
static lv_obj_t *s_lbl_artist  = NULL;
static lv_obj_t *s_lbl_source  = NULL;
static lv_obj_t *s_lbl_time    = NULL;
static lv_obj_t *s_bar_elapsed = NULL;
static lv_obj_t *s_bar_remain  = NULL;
static lv_obj_t *s_btn_play    = NULL;
static lv_obj_t *s_play_icon   = NULL;

static lv_timer_t *s_progress_timer = NULL;

/* ── Transport callbacks ─────────────────────────────────────────────────── */

static void _btn_prev_cb(lv_event_t *e)
{
    (void)e;
    char data[96];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", s_entity_id);
    ha_call_service("media_player", "media_previous_track", data);
}

static void _btn_play_cb(lv_event_t *e)
{
    (void)e;
    char data[96];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", s_entity_id);
    ha_call_service("media_player", "media_play_pause", data);
}

static void _btn_next_cb(lv_event_t *e)
{
    (void)e;
    char data[96];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", s_entity_id);
    ha_call_service("media_player", "media_next_track", data);
}

/* ── Progress helpers ────────────────────────────────────────────────────── */

static void _fmt_time(char *buf, size_t sz, float secs)
{
    int total = (int)secs;
    snprintf(buf, sz, "%d:%02d", total / 60, total % 60);
}

static void _update_progress_bar(void)
{
    if (!s_bar_elapsed || !s_bar_remain) return;

    float pos = s_media_position;
    float dur = s_media_duration;
    if (dur < 0.01f) {
        lv_obj_set_width(s_bar_elapsed, 0);
        return;
    }

    float frac = pos / dur;
    if (frac > 1.0f) frac = 1.0f;

    int32_t parent_w  = lv_obj_get_width(lv_obj_get_parent(s_bar_elapsed));
    int32_t elapsed_w = (int32_t)(parent_w * frac);

    lv_obj_set_width(s_bar_elapsed, elapsed_w);

    char pos_buf[10], dur_buf[10], combined[24];
    _fmt_time(pos_buf, sizeof(pos_buf), pos);
    _fmt_time(dur_buf, sizeof(dur_buf), dur);
    snprintf(combined, sizeof(combined), "%s / %s", pos_buf, dur_buf);
    if (s_lbl_time) lv_label_set_text(s_lbl_time, combined);
}

static void _progress_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_media_duration > 0.01f && s_media_position < s_media_duration) {
        s_media_position += 1.0f;
        _update_progress_bar();
    }
}

/* ── Album art callback ──────────────────────────────────────────────────── */

static void _on_art_ready(lv_image_dsc_t *dsc, void *ud)
{
    (void)ud;
    if (!s_art_img) return;
    if (dsc) {
        lv_image_set_src(s_art_img, dsc);
        lv_obj_set_size(s_art_img, NP_ART_SIZE, NP_ART_SIZE);
    } else {
        lv_image_set_src(s_art_img, &ICON_MUSIC_2_ENTERTAINMENT);
    }
}

/* ── HA state callback ───────────────────────────────────────────────────── */

static void _on_media_state(const ha_state_t *s, void *ud)
{
    (void)ud;
    if (!s_card) return;

    bool idle = (strcmp(s->state, "idle")        == 0 ||
                 strcmp(s->state, "unavailable") == 0 ||
                 strcmp(s->state, "off")         == 0);

    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    if (idle) {
        if (s_progress_timer) {
            lv_timer_delete(s_progress_timer);
            s_progress_timer = NULL;
        }
        if (s_lbl_title)   lv_label_set_text(s_lbl_title,  "Nothing playing");
        if (s_lbl_artist)  lv_label_set_text(s_lbl_artist, "");
        if (s_lbl_source)  lv_label_set_text(s_lbl_source, "");
        if (s_lbl_time)    lv_label_set_text(s_lbl_time,   "");
        if (s_art_img)     lv_image_set_src(s_art_img, &ICON_MUSIC_2_ENTERTAINMENT);
        if (s_play_icon)   lv_label_set_text(s_play_icon, LV_SYMBOL_PLAY);
        s_media_position = 0.0f;
        s_media_duration = 0.0f;
        s_cur_art_path[0] = '\0';
        _update_progress_bar();
        return;
    }

    bool playing = (strcmp(s->state, "playing") == 0);

    /* Update play/pause icon */
    if (s_play_icon) {
        lv_label_set_text(s_play_icon,
            playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    /* Progress timer: run only when playing */
    if (playing && !s_progress_timer) {
        s_progress_timer = lv_timer_create(_progress_timer_cb, 1000, NULL);
    } else if (!playing && s_progress_timer) {
        lv_timer_delete(s_progress_timer);
        s_progress_timer = NULL;
    }

    if (!s->attributes_json || !s->attributes_json[0]) return;

    cJSON *attrs = cJSON_Parse(s->attributes_json);
    if (!attrs) return;

    /* Track title */
    cJSON *title = cJSON_GetObjectItem(attrs, "media_title");
    if (cJSON_IsString(title) && s_lbl_title) {
        lv_label_set_text(s_lbl_title, title->valuestring);
    }

    /* Artist · Album */
    cJSON *artist = cJSON_GetObjectItem(attrs, "media_artist");
    cJSON *album  = cJSON_GetObjectItem(attrs, "media_album_name");
    if (s_lbl_artist) {
        char buf[128] = "";
        if (cJSON_IsString(artist) && cJSON_IsString(album)) {
            snprintf(buf, sizeof(buf), "%s - %s",
                     artist->valuestring, album->valuestring);
        } else if (cJSON_IsString(artist)) {
            snprintf(buf, sizeof(buf), "%s", artist->valuestring);
        } else if (cJSON_IsString(album)) {
            snprintf(buf, sizeof(buf), "%s", album->valuestring);
        }
        lv_label_set_text(s_lbl_artist, buf);
    }

    /* Source name — upper-case */
    cJSON *src = cJSON_GetObjectItem(attrs, "source");
    if (cJSON_IsString(src) && s_lbl_source) {
        char upper[64] = "";
        const char *p = src->valuestring;
        size_t i = 0;
        while (*p && i < sizeof(upper) - 1) {
            char c = *p++;
            upper[i++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
        lv_label_set_text(s_lbl_source, upper);
    }

    /* Position and duration — reset local counter */
    cJSON *pos = cJSON_GetObjectItem(attrs, "media_position");
    cJSON *dur = cJSON_GetObjectItem(attrs, "media_duration");
    if (cJSON_IsNumber(pos)) s_media_position = (float)pos->valuedouble;
    if (cJSON_IsNumber(dur)) s_media_duration = (float)dur->valuedouble;
    _update_progress_bar();

    /* Album art */
    cJSON *pic = cJSON_GetObjectItem(attrs, "entity_picture");
    if (cJSON_IsString(pic) && pic->valuestring[0]) {
        if (strcmp(pic->valuestring, s_cur_art_path) != 0) {
            strncpy(s_cur_art_path, pic->valuestring, sizeof(s_cur_art_path) - 1);
            if (s_art_img)
                lv_image_set_src(s_art_img, &ICON_MUSIC_2_ENTERTAINMENT);
            media_art_request(s_cur_art_path, _on_art_ready, NULL);
        }
    }

    cJSON_Delete(attrs);
}

/* ── Widget builder helpers ──────────────────────────────────────────────── */

static lv_obj_t *_make_transport_btn(lv_obj_t *parent, int32_t size,
                                      bool circle_filled,
                                      lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    if (circle_filled) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    } else {
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    }

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void _card_tap_cb(lv_event_t *e)
{
    (void)e;
    shell_activate_tab(3);  /* entertainment tab */
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void now_playing_init(lv_obj_t *parent)
{
    /* ── Card: single flex row — art | info | transport ─────────────────── */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_hor(card, NP_CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(card, NP_CARD_PAD_VER, 0);
    lv_obj_set_style_pad_column(card, NP_ART_RIGHT_GAP, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(card, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(card, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_80, LV_STATE_PRESSED);
    lv_obj_add_event_cb(card, _card_tap_cb, LV_EVENT_CLICKED, NULL);
    s_card = card;

    /* Album art — left column */
    lv_obj_t *art = lv_image_create(card);
    lv_obj_set_size(art, NP_ART_SIZE, NP_ART_SIZE);
    lv_image_set_src(art, &ICON_MUSIC_2_ENTERTAINMENT);
    lv_obj_set_style_radius(art, 6, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    s_art_img = art;

    /* Info column — middle, grows to fill */
    lv_obj_t *info_col = lv_obj_create(card);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_set_height(info_col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_col, 0, 0);
    lv_obj_set_style_pad_all(info_col, 0, 0);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(info_col, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(info_col, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(info_col, 4, 0);

    /* Header row: source (left) | time (right) */
    lv_obj_t *header_row = lv_obj_create(info_col);
    lv_obj_set_size(header_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header_row, 0, 0);
    lv_obj_set_style_pad_all(header_row, 0, 0);
    lv_obj_remove_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(header_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(header_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_main_place(header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_flex_cross_place(header_row, LV_FLEX_ALIGN_CENTER, 0);

    lv_obj_t *lbl_src = lv_label_create(header_row);
    lv_label_set_text(lbl_src, "");
    lv_obj_set_style_text_font(lbl_src, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_src, lv_color_hex(COL_TEXT_MUTE), 0);
    s_lbl_source = lbl_src;

    lv_obj_t *lbl_time = lv_label_create(header_row);
    lv_label_set_text(lbl_time, "0:00 / 0:00");
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(COL_TEXT_DIM), 0);
    s_lbl_time = lbl_time;

    lv_obj_t *lbl_title = lv_label_create(info_col);
    lv_label_set_text(lbl_title, "");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_title, LV_PCT(100));
    s_lbl_title = lbl_title;

    lv_obj_t *lbl_artist = lv_label_create(info_col);
    lv_label_set_text(lbl_artist, "");
    lv_obj_set_style_text_font(lbl_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_artist, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_long_mode(lbl_artist, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_artist, LV_PCT(100));
    s_lbl_artist = lbl_artist;

    /* Progress bar: remaining (border, full-width background) + elapsed (accent, on top) */
    lv_obj_t *bar_row = lv_obj_create(info_col);
    lv_obj_set_size(bar_row, LV_PCT(100), NP_PROGRESS_H);
    lv_obj_set_style_bg_opa(bar_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar_row, 0, 0);
    lv_obj_set_style_pad_all(bar_row, 0, 0);
    lv_obj_remove_flag(bar_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Grey background — created first so it sits behind the orange fill */
    lv_obj_t *bar_rem = lv_obj_create(bar_row);
    lv_obj_set_pos(bar_rem, 0, 0);
    lv_obj_set_size(bar_rem, LV_PCT(100), NP_PROGRESS_H);
    lv_obj_set_style_bg_color(bar_rem, lv_color_hex(COL_BORDER), 0);
    lv_obj_set_style_bg_opa(bar_rem, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_rem, 0, 0);
    lv_obj_set_style_radius(bar_rem, 0, 0);
    lv_obj_set_style_pad_all(bar_rem, 0, 0);
    lv_obj_remove_flag(bar_rem, LV_OBJ_FLAG_SCROLLABLE);
    s_bar_remain = bar_rem;

    /* Orange fill — created second so it renders on top of the grey background */
    lv_obj_t *bar_el = lv_obj_create(bar_row);
    lv_obj_set_pos(bar_el, 0, 0);
    lv_obj_set_size(bar_el, 0, NP_PROGRESS_H);
    lv_obj_set_style_bg_color(bar_el, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(bar_el, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_el, 0, 0);
    lv_obj_set_style_radius(bar_el, 0, 0);
    lv_obj_set_style_pad_all(bar_el, 0, 0);
    lv_obj_remove_flag(bar_el, LV_OBJ_FLAG_SCROLLABLE);
    s_bar_elapsed = bar_el;

    /* Transport buttons — right column */
    lv_obj_t *btn_col = lv_obj_create(card);
    lv_obj_set_size(btn_col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btn_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_col, 0, 0);
    lv_obj_set_style_pad_all(btn_col, 0, 0);
    lv_obj_remove_flag(btn_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(btn_col, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(btn_col, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(btn_col, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(btn_col, NP_BTN_ROW_GAP, 0);

    lv_obj_t *btn_prev = _make_transport_btn(btn_col, NP_BTN_SKIP_SIZE,
                                              false, _btn_prev_cb);
    lv_obj_t *prev_icon = lv_image_create(btn_prev);
    lv_image_set_src(prev_icon, &ICON_SKIP_BACK_TRANSPORT_PREV);
    lv_obj_center(prev_icon);

    lv_obj_t *btn_play = _make_transport_btn(btn_col, NP_BTN_PLAY_SIZE,
                                              true, _btn_play_cb);
    s_btn_play = btn_play;

    lv_obj_t *play_lbl = lv_label_create(btn_play);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(play_lbl, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(play_lbl);
    s_play_icon = play_lbl;

    lv_obj_t *btn_next = _make_transport_btn(btn_col, NP_BTN_SKIP_SIZE,
                                              false, _btn_next_cb);
    lv_obj_t *next_icon = lv_image_create(btn_next);
    lv_image_set_src(next_icon, &ICON_SKIP_FORWARD_TRANSPORT_NEXT);
    lv_obj_center(next_icon);

    /* Subscribe to the default media entity */
    ha_subscribe(s_entity_id, _on_media_state, NULL);

    LOG_I(TAG, "Now playing widget ready, entity=%s", s_entity_id);
}

void now_playing_set_active_entity(const char *entity_id)
{
    if (!entity_id || !entity_id[0]) return;
    if (strcmp(entity_id, s_entity_id) == 0) return;

    strncpy(s_entity_id, entity_id, sizeof(s_entity_id) - 1);
    s_entity_id[sizeof(s_entity_id) - 1] = '\0';
    s_cur_art_path[0] = '\0';

    ha_subscribe(s_entity_id, _on_media_state, NULL);
    LOG_I(TAG, "Active media entity -> %s", s_entity_id);
}
