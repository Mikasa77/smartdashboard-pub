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

/*
 * tab_entertainment.c — Entertainment tab
 *
 * Layout (top → bottom):
 *   1. Now Playing card  — large album art (128×128), track title, artist·album,
 *                          source name, progress bar + timestamp
 *   2. Transport controls — prev / play-pause (amber circle) / next
 *   3. Volume slider      — full-width, 0–100 %; value in amber to the right
 *   4. Source selector    — three static cards (Kitchen, Pete, Jude)
 *
 * Album art uses the same raw_http_get() + stb_image pattern from now_playing.c.
 * Transport and volume send media_player.* service calls via ha_call_service().
 * Source selector switches s_active_entity and calls now_playing_set_active_entity().
 *
 * Since all three HA_ENTITY_MEDIA_* constants resolve to media_player.portable,
 * switching sources only changes the UI selection; the subscription does not
 * actually change. This is correct for v0.2.0.
 */

#include "tab_entertainment.h"
#include "tab_entertainment_layout.h"
#include "media_art.h"
#include "now_playing.h"
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

static const char *TAG = "tab_entertainment";

/* ── Source definitions ──────────────────────────────────────────────────── */

#define ENT_SOURCE_COUNT 3

typedef struct {
    const char *entity_id;
    const char *label;
} source_def_t;

static const source_def_t k_sources[ENT_SOURCE_COUNT] = {
    { HA_ENTITY_MEDIA_SONOS,        "Kitchen" },
    { HA_ENTITY_MEDIA_PETE_SPOTIFY, "Pete"    },
    { HA_ENTITY_MEDIA_JUDE_SPOTIFY, "Jude"    },
};

/* ── Module state ────────────────────────────────────────────────────────── */

static char   s_active_entity[64]  = HA_ENTITY_MEDIA_SONOS;
static int    s_active_source_idx  = 0;   /* index into k_sources            */

static char   s_cur_art_path[256]  = {0};

static float  s_media_position     = 0.0f;
static float  s_media_duration     = 0.0f;

/* ── Widget handles ──────────────────────────────────────────────────────── */

static lv_obj_t *s_art_img      = NULL;
static lv_obj_t *s_lbl_title    = NULL;
static lv_obj_t *s_lbl_artist   = NULL;
static lv_obj_t *s_lbl_source   = NULL;
static lv_obj_t *s_lbl_time     = NULL;
static lv_obj_t *s_bar_elapsed  = NULL;
static lv_obj_t *s_bar_remain   = NULL;
static lv_obj_t *s_play_icon    = NULL;
static lv_obj_t *s_vol_slider   = NULL;
static lv_obj_t *s_vol_lbl      = NULL;
static lv_obj_t *s_source_cards[ENT_SOURCE_COUNT] = {NULL, NULL, NULL};
static lv_obj_t *s_source_lbls [ENT_SOURCE_COUNT] = {NULL, NULL, NULL};

static lv_timer_t *s_progress_timer = NULL;

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
        lv_obj_set_size(s_art_img, ENT_ART_SIZE_LARGE, ENT_ART_SIZE_LARGE);
    } else {
        lv_image_set_src(s_art_img, &ICON_MUSIC_2_ENTERTAINMENT);
    }
}

/* ── Source selector helpers ─────────────────────────────────────────────── */

static void _refresh_source_cards(void)
{
    for (int i = 0; i < ENT_SOURCE_COUNT; i++) {
        if (!s_source_cards[i]) continue;
        bool active = (i == s_active_source_idx);

        lv_obj_set_style_border_color(s_source_cards[i],
            active ? lv_color_hex(COL_ACCENT) : lv_color_hex(COL_BORDER), 0);
        lv_obj_set_style_bg_color(s_source_cards[i],
            lv_color_hex(COL_SURFACE), 0);

        if (s_source_lbls[i]) {
            lv_obj_set_style_text_color(s_source_lbls[i],
                active ? lv_color_hex(COL_ACCENT) : lv_color_hex(COL_TEXT_MUTE),
                0);
        }
    }
}

/* ── Transport callbacks ─────────────────────────────────────────────────── */

static void _btn_prev_cb(lv_event_t *e)
{
    (void)e;
    char data[96];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", s_active_entity);
    ha_call_service("media_player", "media_previous_track", data);
}

static void _btn_play_cb(lv_event_t *e)
{
    (void)e;
    char data[96];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", s_active_entity);
    ha_call_service("media_player", "media_play_pause", data);
}

static void _btn_next_cb(lv_event_t *e)
{
    (void)e;
    char data[96];
    snprintf(data, sizeof(data), "{\"entity_id\":\"%s\"}", s_active_entity);
    ha_call_service("media_player", "media_next_track", data);
}

/* ── Volume slider callback ──────────────────────────────────────────────── */

static void _vol_slider_released_cb(lv_event_t *e)
{
    (void)e;
    if (!s_vol_slider) return;
    int32_t val = lv_slider_get_value(s_vol_slider);
    float vol   = (float)val / 100.0f;

    char data[128];
    snprintf(data, sizeof(data),
             "{\"entity_id\":\"%s\",\"volume_level\":%.2f}",
             s_active_entity, (double)vol);
    ha_call_service("media_player", "volume_set", data);
    LOG_D(TAG, "Volume set to %d%%", (int)val);
}

static void _vol_slider_changed_cb(lv_event_t *e)
{
    (void)e;
    if (!s_vol_slider || !s_vol_lbl) return;
    int32_t val = lv_slider_get_value(s_vol_slider);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", (int)val);
    lv_label_set_text(s_vol_lbl, buf);
}

/* ── Source card tap callback ────────────────────────────────────────────── */

typedef struct {
    int idx;
} source_cb_data_t;

static source_cb_data_t s_source_cb_data[ENT_SOURCE_COUNT];

static void _source_tap_cb(lv_event_t *e)
{
    source_cb_data_t *d = (source_cb_data_t *)lv_event_get_user_data(e);
    if (!d) return;

    int idx = d->idx;
    if (idx < 0 || idx >= ENT_SOURCE_COUNT) return;
    if (idx == s_active_source_idx) return;

    s_active_source_idx = idx;
    strncpy(s_active_entity, k_sources[idx].entity_id,
            sizeof(s_active_entity) - 1);
    s_active_entity[sizeof(s_active_entity) - 1] = '\0';

    /* Update Home tab now-playing widget */
    now_playing_set_active_entity(s_active_entity);

    _refresh_source_cards();

    LOG_I(TAG, "Source -> %s (%s)", k_sources[idx].label, s_active_entity);
}

/* ── HA state callback ───────────────────────────────────────────────────── */

static void _on_media_state(const ha_state_t *s, void *ud)
{
    (void)ud;

    bool playing = (strcmp(s->state, "playing") == 0);
    bool active  = playing || (strcmp(s->state, "paused") == 0);

    /* Play/pause icon */
    if (s_play_icon) {
        lv_label_set_text(s_play_icon,
            playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }

    /* Progress timer */
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
    if (cJSON_IsString(title) && s_lbl_title)
        lv_label_set_text(s_lbl_title, title->valuestring);

    /* Artist · Album */
    cJSON *artist = cJSON_GetObjectItem(attrs, "media_artist");
    cJSON *album  = cJSON_GetObjectItem(attrs, "media_album_name");
    if (s_lbl_artist) {
        char buf[128] = "";
        if (cJSON_IsString(artist) && cJSON_IsString(album))
            snprintf(buf, sizeof(buf), "%s - %s",
                     artist->valuestring, album->valuestring);
        else if (cJSON_IsString(artist))
            snprintf(buf, sizeof(buf), "%s", artist->valuestring);
        else if (cJSON_IsString(album))
            snprintf(buf, sizeof(buf), "%s", album->valuestring);
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
            upper[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
        }
        lv_label_set_text(s_lbl_source, upper);
    }

    /* Position and duration */
    cJSON *pos = cJSON_GetObjectItem(attrs, "media_position");
    cJSON *dur = cJSON_GetObjectItem(attrs, "media_duration");
    if (cJSON_IsNumber(pos)) s_media_position = (float)pos->valuedouble;
    if (cJSON_IsNumber(dur)) s_media_duration = (float)dur->valuedouble;
    if (active) _update_progress_bar();

    /* Volume */
    cJSON *vol = cJSON_GetObjectItem(attrs, "volume_level");
    if (cJSON_IsNumber(vol) && s_vol_slider && s_vol_lbl) {
        int32_t pct = (int32_t)(vol->valuedouble * 100.0 + 0.5);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        lv_slider_set_value(s_vol_slider, pct, LV_ANIM_OFF);
        char vbuf[8];
        snprintf(vbuf, sizeof(vbuf), "%d%%", (int)pct);
        lv_label_set_text(s_vol_lbl, vbuf);
    }

    /* Album art */
    cJSON *pic = cJSON_GetObjectItem(attrs, "entity_picture");
    if (cJSON_IsString(pic) && pic->valuestring[0]) {
        if (strcmp(pic->valuestring, s_cur_art_path) != 0) {
            strncpy(s_cur_art_path, pic->valuestring,
                    sizeof(s_cur_art_path) - 1);
            if (s_art_img)
                lv_image_set_src(s_art_img, &ICON_MUSIC_2_ENTERTAINMENT);
            media_art_request(s_cur_art_path, _on_art_ready, NULL);
        }
    }

    cJSON_Delete(attrs);
}

/* ── Widget builder helper ───────────────────────────────────────────────── */

static lv_obj_t *_make_transport_btn(lv_obj_t *parent, int32_t size,
                                      bool circle_filled, lv_event_cb_t cb)
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

/* ── Tab init ────────────────────────────────────────────────────────────── */

void tab_entertainment_init(lv_obj_t *parent)
{
    /* Reset per-init state (handles re-entry if tabs are ever destroyed) */
    s_media_position  = 0.0f;
    s_media_duration  = 0.0f;
    s_cur_art_path[0] = '\0';
    if (s_progress_timer) { lv_timer_delete(s_progress_timer); s_progress_timer = NULL; }

    /* ── Outer scroll container ───────────────────────────────────────────── */
    lv_obj_set_style_bg_color(parent, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(parent, ENT_TAB_PAD_HOR, 0);
    lv_obj_set_style_pad_top(parent, ENT_TAB_PAD_VER, 0);
    lv_obj_set_style_pad_bottom(parent, ENT_TAB_PAD_VER, 0);
    lv_obj_set_layout(parent, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(parent, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(parent, ENT_SECTION_GAP, 0);
    lv_obj_set_style_flex_cross_place(parent, LV_FLEX_ALIGN_CENTER, 0);

    /* ════════════════════════════════════════════════════════════════════════
     * 1. NOW PLAYING CARD
     * ════════════════════════════════════════════════════════════════════════ */
    lv_obj_t *np_card = lv_obj_create(parent);
    lv_obj_set_size(np_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(np_card, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(np_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(np_card, 0, 0);
    lv_obj_set_style_radius(np_card, ENT_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(np_card, ENT_CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(np_card, ENT_CARD_PAD_VER, 0);
    lv_obj_set_style_pad_row(np_card, 0, 0);
    lv_obj_set_style_pad_column(np_card, 0, 0);
    lv_obj_remove_flag(np_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(np_card, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(np_card, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(np_card, 12, 0);

    /* ── Art + info row ───────────────────────────────────────────────────── */
    lv_obj_t *art_row = lv_obj_create(np_card);
    lv_obj_set_size(art_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(art_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(art_row, 0, 0);
    lv_obj_set_style_pad_all(art_row, 0, 0);
    lv_obj_remove_flag(art_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(art_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(art_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(art_row, LV_FLEX_ALIGN_START, 0);
    lv_obj_set_style_pad_column(art_row, ENT_ART_RIGHT_GAP, 0);

    /* Album art */
    lv_obj_t *art = lv_image_create(art_row);
    lv_obj_set_size(art, ENT_ART_SIZE_LARGE, ENT_ART_SIZE_LARGE);
    lv_image_set_src(art, &ICON_MUSIC_2_ENTERTAINMENT);
    lv_obj_set_style_radius(art, 8, 0);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_SCROLLABLE);
    s_art_img = art;

    /* Info column */
    lv_obj_t *info_col = lv_obj_create(art_row);
    lv_obj_set_flex_grow(info_col, 1);
    lv_obj_set_height(info_col, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(info_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_col, 0, 0);
    lv_obj_set_style_pad_all(info_col, 0, 0);
    lv_obj_remove_flag(info_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(info_col, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(info_col, LV_FLEX_FLOW_COLUMN, 0);
    lv_obj_set_style_pad_row(info_col, 6, 0);

    /* Track title — large semibold */
    lv_obj_t *lbl_title = lv_label_create(info_col);
    lv_label_set_text(lbl_title, "");
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(COL_TEXT), 0);
    lv_label_set_long_mode(lbl_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_title, LV_PCT(100));
    s_lbl_title = lbl_title;

    /* Artist · Album — dim */
    lv_obj_t *lbl_artist = lv_label_create(info_col);
    lv_label_set_text(lbl_artist, "");
    lv_obj_set_style_text_font(lbl_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_artist, lv_color_hex(COL_TEXT_DIM), 0);
    lv_label_set_long_mode(lbl_artist, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(lbl_artist, LV_PCT(100));
    s_lbl_artist = lbl_artist;

    /* Source name */
    lv_obj_t *lbl_source = lv_label_create(info_col);
    lv_label_set_text(lbl_source, "");
    lv_obj_set_style_text_font(lbl_source, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_source, lv_color_hex(COL_TEXT_MUTE), 0);
    s_lbl_source = lbl_source;

    /* Progress bar row */
    lv_obj_t *bar_row = lv_obj_create(info_col);
    lv_obj_set_size(bar_row, LV_PCT(100), ENT_PROGRESS_H);
    lv_obj_set_style_bg_opa(bar_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar_row, 0, 0);
    lv_obj_set_style_pad_all(bar_row, 0, 0);
    lv_obj_remove_flag(bar_row, LV_OBJ_FLAG_SCROLLABLE);

    /* Grey background — created first so it sits behind the orange fill */
    lv_obj_t *bar_rem = lv_obj_create(bar_row);
    lv_obj_set_pos(bar_rem, 0, 0);
    lv_obj_set_size(bar_rem, LV_PCT(100), ENT_PROGRESS_H);
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
    lv_obj_set_size(bar_el, 0, ENT_PROGRESS_H);
    lv_obj_set_style_bg_color(bar_el, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(bar_el, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar_el, 0, 0);
    lv_obj_set_style_radius(bar_el, 0, 0);
    lv_obj_set_style_pad_all(bar_el, 0, 0);
    lv_obj_remove_flag(bar_el, LV_OBJ_FLAG_SCROLLABLE);
    s_bar_elapsed = bar_el;

    /* Timestamp */
    lv_obj_t *lbl_time = lv_label_create(info_col);
    lv_label_set_text(lbl_time, "0:00 / 0:00");
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(COL_TEXT_DIM), 0);
    s_lbl_time = lbl_time;

    /* ════════════════════════════════════════════════════════════════════════
     * 2. TRANSPORT CONTROLS
     * ════════════════════════════════════════════════════════════════════════ */
    lv_obj_t *transport_row = lv_obj_create(parent);
    lv_obj_set_size(transport_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(transport_row, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(transport_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(transport_row, 0, 0);
    lv_obj_set_style_radius(transport_row, ENT_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(transport_row, ENT_CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(transport_row, ENT_CARD_PAD_VER, 0);
    lv_obj_remove_flag(transport_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(transport_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(transport_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_main_place(transport_row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_flex_cross_place(transport_row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(transport_row, ENT_BTN_ROW_GAP, 0);

    lv_obj_t *btn_prev = _make_transport_btn(transport_row, ENT_BTN_SKIP_SIZE,
                                              false, _btn_prev_cb);
    lv_obj_t *prev_icon = lv_image_create(btn_prev);
    lv_image_set_src(prev_icon, &ICON_SKIP_BACK_TRANSPORT_PREV);
    lv_obj_center(prev_icon);

    lv_obj_t *btn_play = _make_transport_btn(transport_row, ENT_BTN_PLAY_SIZE,
                                              true, _btn_play_cb);
    lv_obj_t *play_lbl = lv_label_create(btn_play);
    lv_label_set_text(play_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(play_lbl, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_text_font(play_lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(play_lbl);
    s_play_icon = play_lbl;

    lv_obj_t *btn_next = _make_transport_btn(transport_row, ENT_BTN_SKIP_SIZE,
                                              false, _btn_next_cb);
    lv_obj_t *next_icon = lv_image_create(btn_next);
    lv_image_set_src(next_icon, &ICON_SKIP_FORWARD_TRANSPORT_NEXT);
    lv_obj_center(next_icon);

    (void)btn_prev; (void)btn_play; (void)btn_next;

    /* ════════════════════════════════════════════════════════════════════════
     * 3. VOLUME SLIDER
     * ════════════════════════════════════════════════════════════════════════ */
    lv_obj_t *vol_row = lv_obj_create(parent);
    lv_obj_set_size(vol_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(vol_row, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(vol_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vol_row, 0, 0);
    lv_obj_set_style_radius(vol_row, ENT_CARD_RADIUS, 0);
    lv_obj_set_style_pad_hor(vol_row, ENT_CARD_PAD_HOR, 0);
    lv_obj_set_style_pad_ver(vol_row, ENT_CARD_PAD_VER, 0);
    lv_obj_remove_flag(vol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(vol_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(vol_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_cross_place(vol_row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(vol_row, ENT_VOL_ROW_GAP, 0);

    lv_obj_t *slider = lv_slider_create(vol_row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, ENT_VOL_SLIDER_H / 4);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);

    /* Slider track colour */
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_BORDER),
                               LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ACCENT),
                               LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_hex(COL_ACCENT),
                               LV_PART_KNOB);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_KNOB);

    lv_obj_add_event_cb(slider, _vol_slider_changed_cb,  LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, _vol_slider_released_cb, LV_EVENT_RELEASED,      NULL);
    s_vol_slider = slider;

    lv_obj_t *vol_lbl = lv_label_create(vol_row);
    lv_obj_set_width(vol_lbl, ENT_VOL_VAL_W);
    lv_label_set_text(vol_lbl, "50%");
    lv_obj_set_style_text_font(vol_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(vol_lbl, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_text_align(vol_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    s_vol_lbl = vol_lbl;

    /* ════════════════════════════════════════════════════════════════════════
     * 4. SOURCE SELECTOR GRID
     * ════════════════════════════════════════════════════════════════════════ */
    lv_obj_t *source_row = lv_obj_create(parent);
    lv_obj_set_size(source_row, LV_PCT(100), ENT_SOURCE_CARD_H);
    lv_obj_set_style_bg_opa(source_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(source_row, 0, 0);
    lv_obj_set_style_pad_all(source_row, 0, 0);
    lv_obj_remove_flag(source_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(source_row, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(source_row, LV_FLEX_FLOW_ROW, 0);
    lv_obj_set_style_flex_main_place(source_row, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    lv_obj_set_style_flex_cross_place(source_row, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_column(source_row, ENT_SOURCE_GRID_GAP, 0);

    for (int i = 0; i < ENT_SOURCE_COUNT; i++) {
        s_source_cb_data[i].idx = i;

        lv_obj_t *card = lv_obj_create(source_row);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, ENT_SOURCE_CARD_H);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_SURFACE), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, ENT_SOURCE_BORDER_W, 0);
        lv_obj_set_style_radius(card, ENT_SOURCE_RADIUS, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, _source_tap_cb, LV_EVENT_CLICKED,
                            &s_source_cb_data[i]);
        s_source_cards[i] = card;

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, k_sources[i].label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl);
        s_source_lbls[i] = lbl;
    }

    /* Apply initial source highlight */
    _refresh_source_cards();

    /* ── Subscribe to the active entity ──────────────────────────────────── */
    ha_subscribe(s_active_entity, _on_media_state, NULL);

    /* Tell the Home widget to track the same entity */
    now_playing_set_active_entity(s_active_entity);

    LOG_I(TAG, "Entertainment tab ready, entity=%s", s_active_entity);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

const char *tab_entertainment_get_active_entity(void)
{
    return s_active_entity;
}

void tab_entertainment_set_active_entity(const char *entity_id)
{
    if (!entity_id || !entity_id[0]) return;
    if (strcmp(entity_id, s_active_entity) == 0) return;

    strncpy(s_active_entity, entity_id, sizeof(s_active_entity) - 1);
    s_active_entity[sizeof(s_active_entity) - 1] = '\0';
    s_cur_art_path[0] = '\0';

    /* Find matching source index */
    s_active_source_idx = 0;
    for (int i = 0; i < ENT_SOURCE_COUNT; i++) {
        if (strcmp(k_sources[i].entity_id, entity_id) == 0) {
            s_active_source_idx = i;
            break;
        }
    }

    ha_subscribe(s_active_entity, _on_media_state, NULL);
    now_playing_set_active_entity(s_active_entity);
    _refresh_source_cards();

    LOG_I(TAG, "Active entity -> %s", s_active_entity);
}
