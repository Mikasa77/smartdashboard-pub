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

/* Single-instance album art fetcher.
 * Both now_playing and tab_entertainment call media_art_request() — only
 * one HTTP fetch + stbi decode happens regardless of how many callers ask
 * for the same entity_picture URL. */

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb_image.h"

#include "media_art.h"
#include "ha_credentials.h"
#include "dashboard_log.h"
#include <string.h>
#include <stdlib.h>

#ifdef SIMULATOR
#include <curl/curl.h>
#include <windows.h>
#else
#include "esp_http_client.h"
#endif

static const char *TAG = "media_art";

#define ART_MAX_BYTES (2 * 1024 * 1024)

/* ── State (LVGL thread only) ─────────────────────────────────────────────── */

typedef struct {
    media_art_cb_t fn;
    void          *ud;
} sub_t;

static char           s_url[256]                = "";
static bool           s_pending                 = false;
static uint8_t       *s_pixels                  = NULL;
static lv_image_dsc_t s_dsc                     = {0};
static sub_t          s_subs[MEDIA_ART_MAX_CBS];
static int            s_sub_cnt                 = 0;

/* ── Raw HTTP fetch (binary-safe) ─────────────────────────────────────────── */

typedef struct {
    uint8_t *data;
    size_t   len;
} raw_t;

#ifdef SIMULATOR

typedef struct { uint8_t *buf; size_t len; } sim_buf_t;

static size_t _write_cb(void *ptr, size_t sz, size_t nmemb, void *ud)
{
    sim_buf_t *b   = (sim_buf_t *)ud;
    size_t     got = sz * nmemb;
    if (b->len + got > ART_MAX_BYTES) {
        LOG_W(TAG, "response exceeds %u bytes, aborting", (unsigned)ART_MAX_BYTES);
        free(b->buf); b->buf = NULL; b->len = 0;
        return 0; /* signals curl to abort */
    }
    uint8_t   *tmp = realloc(b->buf, b->len + got);
    if (!tmp) return 0;
    b->buf = tmp;
    memcpy(b->buf + b->len, ptr, got);
    b->len += got;
    return got;
}

static bool raw_get(const char *url, raw_t *out)
{
    out->data = NULL; out->len = 0;
    CURL *c = curl_easy_init();
    if (!c) return false;
    sim_buf_t wb = {0};
    curl_easy_setopt(c, CURLOPT_URL,           url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, _write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &wb);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode rc = curl_easy_perform(c);
    long code = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc == CURLE_OK && code == 200 && wb.buf) { out->data = wb.buf; out->len = wb.len; return true; }
    free(wb.buf);
    return false;
}

#else /* Firmware */

typedef struct { uint8_t *buf; int len; } fw_buf_t;

static esp_err_t _evt(esp_http_client_event_t *e)
{
    fw_buf_t *wb = (fw_buf_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && wb) {
        if ((size_t)(wb->len + e->data_len) > ART_MAX_BYTES) {
            LOG_W(TAG, "response exceeds %u bytes, aborting", (unsigned)ART_MAX_BYTES);
            free(wb->buf); wb->buf = NULL; wb->len = 0;
            return ESP_FAIL;
        }
        uint8_t *tmp = realloc(wb->buf, (size_t)(wb->len + e->data_len));
        if (!tmp) return ESP_FAIL;
        wb->buf = tmp;
        memcpy(wb->buf + wb->len, e->data, e->data_len);
        wb->len += e->data_len;
    }
    return ESP_OK;
}

static bool raw_get(const char *url, raw_t *out)
{
    out->data = NULL; out->len = 0;
    fw_buf_t wb = {0};
    esp_http_client_config_t cfg = {
        .url           = url,
        .event_handler = _evt,
        .user_data     = &wb,
        .timeout_ms    = 15000,
    };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    if (!h) return false;
    esp_err_t err = esp_http_client_perform(h);
    int status    = esp_http_client_get_status_code(h);
    esp_http_client_cleanup(h);
    if (err == ESP_OK && status == 200 && wb.buf) { out->data = wb.buf; out->len = (size_t)wb.len; return true; }
    free(wb.buf);
    return false;
}

#endif

/* ── Nearest-neighbour downscale (background thread only) ────────────────── */

/* stb_image outputs R,G,B; LVGL LV_COLOR_FORMAT_RGB888 is stored B,G,R in
 * memory (lv_color_t struct: blue=byte[0], green=byte[1], red=byte[2]).
 * Swap R↔B while copying so colours render correctly. */
static uint8_t *_resize_nn(const uint8_t *src, int sw, int sh, int dw, int dh)
{
    uint8_t *dst = malloc((size_t)(dw * dh * 3));
    if (!dst) return NULL;
    for (int y = 0; y < dh; y++) {
        int sy = y * sh / dh;
        for (int x = 0; x < dw; x++) {
            int sx = x * sw / dw;
            const uint8_t *s = src + (sy * sw + sx) * 3;
            uint8_t       *d = dst + (y  * dw + x)  * 3;
            d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; /* RGB→BGR */
        }
    }
    return dst;
}

/* ── Fetch task ───────────────────────────────────────────────────────────── */

typedef struct {
    char     url[256];
    uint8_t *pixels;
    int      w, h;
} job_t;

static void _done(void *arg)
{
    job_t *j = (job_t *)arg;

    /* Superseded by a newer request — discard silently */
    if (strcmp(j->url, s_url) != 0) {
        if (j->pixels) stbi_image_free(j->pixels);
        free(j);
        return;
    }

    s_pending = false;

    if (j->pixels) {
        if (s_pixels) stbi_image_free(s_pixels);
        s_pixels            = j->pixels;
        s_dsc.header.cf     = LV_COLOR_FORMAT_RGB888;
        s_dsc.header.w      = (uint32_t)j->w;
        s_dsc.header.h      = (uint32_t)j->h;
        s_dsc.header.stride = (uint32_t)(j->w * 3);
        s_dsc.data          = s_pixels;
        s_dsc.data_size     = (uint32_t)(j->w * j->h * 3);
        for (int i = 0; i < s_sub_cnt; i++) s_subs[i].fn(&s_dsc, s_subs[i].ud);
    } else {
        for (int i = 0; i < s_sub_cnt; i++) s_subs[i].fn(NULL, s_subs[i].ud);
    }

    s_sub_cnt = 0;
    free(j);
}

#ifdef SIMULATOR
static DWORD WINAPI _fetch_thread(LPVOID arg)
#else
static void _fetch_task(void *arg)
#endif
{
    job_t *j = (job_t *)arg;
    raw_t  r = {0};
    if (raw_get(j->url, &r) && r.data && r.len) {
        int w, h, ch;
        j->pixels = stbi_load_from_memory(r.data, (int)r.len, &w, &h, &ch, 3);
        free(r.data);
        j->w = w; j->h = h;
        if (!j->pixels) {
            LOG_W(TAG, "stbi decode failed: %s", j->url);
        } else {
            /* Always convert RGB→BGR via _resize_nn (LVGL RGB888 = BGR in memory).
             * Cap both dimensions at MEDIA_ART_MAX_DIM; smaller images are merely
             * color-swapped with no nearest-neighbour scaling. */
            int dw = (w > MEDIA_ART_MAX_DIM) ? MEDIA_ART_MAX_DIM : w;
            int dh = (h > MEDIA_ART_MAX_DIM) ? MEDIA_ART_MAX_DIM : h;
            uint8_t *bgr = _resize_nn(j->pixels, w, h, dw, dh);
            stbi_image_free(j->pixels);
            j->pixels = bgr;
            j->w = dw; j->h = dh;
            if (!bgr) LOG_W(TAG, "resize alloc failed: %s", j->url);
        }
    } else {
        free(r.data);
        LOG_W(TAG, "fetch failed: %s", j->url);
    }
    lv_async_call(_done, j);
#ifdef SIMULATOR
    return 0;
#else
    vTaskDelete(NULL);
#endif
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void media_art_request(const char *path, media_art_cb_t cb, void *ud)
{
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%u%s",
             ha_credentials_get_host(),
             (unsigned)ha_credentials_get_port(),
             path);

    /* Same URL already cached (fetch done, no pending task) */
    if (!s_pending && s_pixels && strcmp(url, s_url) == 0) {
        cb(&s_dsc, ud);
        return;
    }

    /* Same URL already in-flight — just queue the callback */
    if (s_pending && strcmp(url, s_url) == 0) {
        if (s_sub_cnt < MEDIA_ART_MAX_CBS) {
            s_subs[s_sub_cnt].fn = cb;
            s_subs[s_sub_cnt].ud = ud;
            s_sub_cnt++;
        }
        return;
    }

    /* New URL — notify any pending subs for the old URL that they're cancelled */
    for (int i = 0; i < s_sub_cnt; i++) s_subs[i].fn(NULL, s_subs[i].ud);
    s_sub_cnt = 0;

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
    s_pending = true;

    s_subs[0].fn = cb;
    s_subs[0].ud = ud;
    s_sub_cnt    = 1;

    job_t *j = calloc(1, sizeof(job_t));
    if (!j) { s_pending = false; s_sub_cnt = 0; cb(NULL, ud); return; }
    strncpy(j->url, url, sizeof(j->url) - 1);

    LOG_D(TAG, "fetching: %s", url);

#ifdef SIMULATOR
    CreateThread(NULL, 0, _fetch_thread, j, 0, NULL);
#else
    xTaskCreate(_fetch_task, "art", 8192, j, 4, NULL);
#endif
}
