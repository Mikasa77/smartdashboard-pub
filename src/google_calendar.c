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

#include "google_calendar.h"
#include "google_credentials.h"
#include "dashboard_config.h"
#include "dashboard_colours.h"
#include "dashboard_log.h"
#include "platform/http_client.h"
#include "cJSON.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#ifdef SIMULATOR
#  include <windows.h>
#else
#  include "freertos/FreeRTOS.h"
#  include "freertos/semphr.h"
#  include "freertos/task.h"
#  include "nvs_flash.h"
#  include "nvs.h"
#endif

static const char *TAG = "google_cal";

/* ── Token storage ──────────────────────────────────────────────────────────── */

#define GC_CONF_FILE  "gcal.conf"
#define GC_NVS_NS     "gcal"
#define GC_TOKEN_MAX  512

static char   s_refresh_token[GC_TOKEN_MAX] = {0};
static char   s_access_token[GC_TOKEN_MAX]  = {0};
static time_t s_token_expiry                = 0;

/* ── Auth state ─────────────────────────────────────────────────────────────── */

static volatile gc_auth_state_t s_auth_state  = GC_AUTH_UNLINKED;
static volatile int             s_auth_cancel = 0;

/* ── Threading ──────────────────────────────────────────────────────────────── */

#define GC_WORK_QUEUE_SIZE 8

typedef enum {
    GC_WORK_AUTH_START,
    GC_WORK_CREATE,
    GC_WORK_UPDATE,
    GC_WORK_DELETE,
    GC_WORK_DELETE_FUTURE,
    GC_WORK_GET_EVENTS,
} gc_work_type_t;

typedef struct {
    gc_work_type_t  type;
    gc_event_t      ev;          /* create / update */
    char            uid[128];    /* delete / delete_future / get_events cal_id */
    char            date_str[16]; /* delete_future: "YYYY-MM-DD" cutoff */
    time_t          from;        /* get_events */
    time_t          to;          /* get_events */
    gc_done_cb_t    cb;
    gc_code_cb_t    code_cb;
    gc_events_cb_t  events_cb;   /* get_events */
    void           *ctx;
} gc_work_t;

#ifdef SIMULATOR
static HANDLE           s_thread;
static CRITICAL_SECTION s_lock;
static HANDLE           s_work_event;
#else
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;
static QueueHandle_t     s_work_queue;
#endif

#ifdef SIMULATOR
static gc_work_t *s_queue[GC_WORK_QUEUE_SIZE];
static int        s_queue_head = 0;
static int        s_queue_tail = 0;
#endif
static int        s_running    = 0;

/* ── Token persistence ──────────────────────────────────────────────────────── */

#ifdef SIMULATOR

static void gc_save_tokens(void)
{
    FILE *f = fopen(GC_CONF_FILE, "w");
    if (!f) { LOG_W(TAG, "Cannot write %s", GC_CONF_FILE); return; }
    fprintf(f, "refresh_token=%s\n", s_refresh_token);
    fprintf(f, "access_token=%s\n",  s_access_token);
    fprintf(f, "token_expiry=%lld\n", (long long)s_token_expiry);
    fclose(f);
}

static void gc_load_tokens(void)
{
    FILE *f = fopen(GC_CONF_FILE, "r");
    if (!f) return;
    char line[GC_TOKEN_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';
        if      (strcmp(line, "refresh_token") == 0)
            strncpy(s_refresh_token, val, GC_TOKEN_MAX - 1);
        else if (strcmp(line, "access_token") == 0)
            strncpy(s_access_token, val, GC_TOKEN_MAX - 1);
        else if (strcmp(line, "token_expiry") == 0)
            s_token_expiry = (time_t)atoll(val);
    }
    fclose(f);
}

#else /* Firmware NVS */

static void gc_save_tokens(void)
{
    nvs_handle_t h;
    if (nvs_open(GC_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "refresh_token", s_refresh_token);
    nvs_set_str(h, "access_token",  s_access_token);
    char exp_str[24];
    snprintf(exp_str, sizeof(exp_str), "%lld", (long long)s_token_expiry);
    nvs_set_str(h, "token_expiry", exp_str);
    nvs_commit(h);
    nvs_close(h);
}

static void gc_load_tokens(void)
{
    nvs_handle_t h;
    if (nvs_open(GC_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len;
    len = GC_TOKEN_MAX; nvs_get_str(h, "refresh_token", s_refresh_token, &len);
    len = GC_TOKEN_MAX; nvs_get_str(h, "access_token",  s_access_token,  &len);
    char exp_str[24] = {0};
    len = sizeof(exp_str); nvs_get_str(h, "token_expiry", exp_str, &len);
    if (exp_str[0]) s_token_expiry = (time_t)atoll(exp_str);
    nvs_close(h);
}

#endif /* SIMULATOR */

/* ── OAuth HTTP helpers (called from background thread only) ─────────────────── */

static void _urlencode(const char *src, char *buf, size_t buf_len)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < buf_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            buf[j++] = (char)c;
        } else {
            j += snprintf(buf + j, buf_len - j, "%%%02X", c);
        }
    }
    buf[j] = '\0';
}

static bool gc_http_device_code(char *out_user_code, size_t uc_len,
                                char *out_url,       size_t url_len,
                                char *out_dev_code,  size_t dc_len,
                                int  *out_interval,  int   *out_expires_in)
{
    char scope_enc[128];
    _urlencode("https://www.googleapis.com/auth/calendar", scope_enc, sizeof(scope_enc));

    char body[512];
    snprintf(body, sizeof(body), "client_id=%s&scope=%s",
             GOOGLE_CLIENT_ID, scope_enc);

    const char *headers[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
    http_req_t  req  = { .url = "https://oauth2.googleapis.com/device/code",
                         .method = "POST", .body = body, .headers = headers };
    http_resp_t resp = {0};
    if (!http_request(&req, &resp)) return false;

    bool ok = false;
    cJSON *j = cJSON_Parse(resp.body);
    if (j) {
        const char *uc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "user_code"));
        const char *vu = cJSON_GetStringValue(cJSON_GetObjectItem(j, "verification_url"));
        const char *dc = cJSON_GetStringValue(cJSON_GetObjectItem(j, "device_code"));
        cJSON *iv = cJSON_GetObjectItem(j, "interval");
        cJSON *ex = cJSON_GetObjectItem(j, "expires_in");
        if (uc && vu && dc) {
            strncpy(out_user_code, uc, uc_len - 1); out_user_code[uc_len - 1] = '\0';
            strncpy(out_url,       vu, url_len - 1); out_url[url_len - 1] = '\0';
            strncpy(out_dev_code,  dc, dc_len  - 1); out_dev_code[dc_len - 1] = '\0';
            *out_interval   = cJSON_IsNumber(iv) ? iv->valueint : 5;
            *out_expires_in = cJSON_IsNumber(ex) ? ex->valueint : 1800;
            ok = true;
        }
        cJSON_Delete(j);
    }
    http_resp_free(&resp);
    LOG_I(TAG, "device/code: ok=%d user_code=%s", ok, ok ? out_user_code : "(fail)");
    return ok;
}

static bool gc_http_poll_token(const char *device_code)
{
    char body[700];
    snprintf(body, sizeof(body),
             "client_id=%s&client_secret=%s&device_code=%s"
             "&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
             GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, device_code);

    const char *headers[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
    http_req_t  req  = { .url = "https://oauth2.googleapis.com/token",
                         .method = "POST", .body = body, .headers = headers };
    http_resp_t resp = {0};
    if (!http_request(&req, &resp)) return false;

    bool approved = false;
    cJSON *j = cJSON_Parse(resp.body);
    if (j) {
        cJSON *err_item = cJSON_GetObjectItem(j, "error");
        if (!err_item) {
            const char *rt  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "refresh_token"));
            const char *at  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "access_token"));
            cJSON      *exp = cJSON_GetObjectItem(j, "expires_in");
            if (rt && at) {
                strncpy(s_refresh_token, rt, GC_TOKEN_MAX - 1);
                strncpy(s_access_token,  at, GC_TOKEN_MAX - 1);
                s_token_expiry = time(NULL) + (cJSON_IsNumber(exp) ? exp->valueint : 3600) - 60;
                gc_save_tokens();
                s_auth_state = GC_AUTH_READY;
                approved     = true;
                LOG_I(TAG, "Device flow authorized");
            }
        } else {
            const char *err_str = cJSON_GetStringValue(err_item);
            if (err_str && strcmp(err_str, "access_denied") == 0) {
                s_auth_state = GC_AUTH_ERROR;
                LOG_W(TAG, "Device flow: access denied");
            }
            /* "authorization_pending" and "slow_down" → keep polling */
        }
        cJSON_Delete(j);
    }
    http_resp_free(&resp);
    return approved;
}

static bool gc_http_refresh_token(void)
{
    char body[700];
    snprintf(body, sizeof(body),
             "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, s_refresh_token);

    const char *headers[] = {"Content-Type: application/x-www-form-urlencoded", NULL};
    http_req_t  req  = { .url = "https://oauth2.googleapis.com/token",
                         .method = "POST", .body = body, .headers = headers };
    http_resp_t resp = {0};
    if (!http_request(&req, &resp)) return false;

    bool ok = false;
    cJSON *j = cJSON_Parse(resp.body);
    if (j) {
        if (resp.status == 200) {
            const char *at  = cJSON_GetStringValue(cJSON_GetObjectItem(j, "access_token"));
            cJSON      *exp = cJSON_GetObjectItem(j, "expires_in");
            if (at) {
                strncpy(s_access_token, at, GC_TOKEN_MAX - 1);
                s_token_expiry = time(NULL) + (cJSON_IsNumber(exp) ? exp->valueint : 3600) - 60;
                gc_save_tokens();
                ok = true;
                LOG_I(TAG, "Token refreshed");
            }
        } else {
            s_refresh_token[0] = '\0';
            s_access_token[0]  = '\0';
            s_token_expiry     = 0;
            s_auth_state       = GC_AUTH_ERROR;
            gc_save_tokens();
            LOG_W(TAG, "Token refresh failed (%d) — user must re-link", resp.status);
        }
        cJSON_Delete(j);
    }
    http_resp_free(&resp);
    return ok;
}

static bool gc_ensure_token(void)
{
    if (!s_refresh_token[0]) return false;
    if (s_access_token[0] && time(NULL) < s_token_expiry) return true;
    return gc_http_refresh_token();
}

/* ── Datetime utilities ─────────────────────────────────────────────────────── */

static int gc_utc_offset_min(void)
{
    time_t t = time(NULL);
    struct tm lt = *localtime(&t);
    struct tm ut = *gmtime(&t);
    int diff = (lt.tm_hour - ut.tm_hour) * 60 + (lt.tm_min - ut.tm_min);
    if      (lt.tm_mday > ut.tm_mday) diff += 1440;
    else if (lt.tm_mday < ut.tm_mday) diff -= 1440;
    return diff;
}

void gc_fmt_datetime(char *out, size_t n,
                     int year, int mon, int mday, int hour, int min)
{
    int  off  = gc_utc_offset_min();
    char sign = (off >= 0) ? '+' : '-';
    int  aoff = abs(off);
    snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:00%c%02d:%02d",
             year, mon, mday, hour, min, sign, aoff / 60, aoff % 60);
}

/* ── JSON body builder ──────────────────────────────────────────────────────── */

static char *gc_build_event_json(const gc_event_t *ev)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "summary", ev->summary);
    if (ev->uid[0])
        cJSON_AddStringToObject(j, "id", ev->uid);

    if (ev->all_day) {
        cJSON *start = cJSON_AddObjectToObject(j, "start");
        cJSON_AddStringToObject(start, "date", ev->start_dt);
        cJSON *end = cJSON_AddObjectToObject(j, "end");
        cJSON_AddStringToObject(end, "date", ev->end_dt);
    } else {
        cJSON *start = cJSON_AddObjectToObject(j, "start");
        cJSON_AddStringToObject(start, "dateTime", ev->start_dt);
        cJSON *end = cJSON_AddObjectToObject(j, "end");
        cJSON_AddStringToObject(end, "dateTime", ev->end_dt);
    }

    char *str = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return str;
}

/* ── Result dispatch (posts callback to LVGL thread) ────────────────────────── */

typedef struct { gc_done_cb_t cb; bool ok; void *ctx; } gc_result_t;
typedef struct { gc_code_cb_t cb; char user_code[64]; char url[128]; void *ctx; } gc_code_result_t;

static void _result_async(void *arg)
{
    gc_result_t *r = (gc_result_t *)arg;
    r->cb(r->ok, r->ctx);
    free(r);
}

static void _code_result_async(void *arg)
{
    gc_code_result_t *r = (gc_code_result_t *)arg;
    r->cb(r->user_code, r->url, r->ctx);
    free(r);
}

static void gc_post_result(gc_done_cb_t cb, bool ok, void *ctx)
{
    if (!cb) return;
    gc_result_t *r = malloc(sizeof(*r));
    if (!r) return;
    r->cb = cb; r->ok = ok; r->ctx = ctx;
    lv_async_call(_result_async, r);
}

static void gc_post_code(gc_code_cb_t cb, const char *user_code, const char *url, void *ctx)
{
    if (!cb) return;
    gc_code_result_t *r = malloc(sizeof(*r));
    if (!r) return;
    r->cb = cb; r->ctx = ctx;
    strncpy(r->user_code, user_code, sizeof(r->user_code) - 1);
    strncpy(r->url,       url,       sizeof(r->url) - 1);
    lv_async_call(_code_result_async, r);
}

typedef struct {
    gc_events_cb_t cb;
    bool           ok;
    gc_event_t    *events;
    int            count;
    void          *ctx;
} gc_events_result_t;

static void _events_result_async(void *arg)
{
    gc_events_result_t *r = (gc_events_result_t *)arg;
    r->cb(r->ok, r->events, r->count, r->ctx);
    free(r->events);
    free(r);
}

static void gc_post_events_result(gc_events_cb_t cb, bool ok,
                                   gc_event_t *events, int count, void *ctx)
{
    if (!cb) { free(events); return; }
    gc_events_result_t *r = malloc(sizeof(*r));
    if (!r) { free(events); return; }
    r->cb = cb; r->ok = ok; r->events = events; r->count = count; r->ctx = ctx;
    lv_async_call(_events_result_async, r);
}

/* ── Work queue ─────────────────────────────────────────────────────────────── */

static void gc_post_work(gc_work_t *w)
{
#ifdef SIMULATOR
    EnterCriticalSection(&s_lock);
    int next = (s_queue_tail + 1) % GC_WORK_QUEUE_SIZE;
    if (next != s_queue_head) {
        s_queue[s_queue_tail] = w;
        s_queue_tail = next;
    } else {
        LOG_W(TAG, "Work queue full — dropping request");
        free(w);
    }
    LeaveCriticalSection(&s_lock);
    SetEvent(s_work_event);
#else
    xQueueSend(s_work_queue, &w, 0);
#endif
}

/* ── CRUD work handlers ─────────────────────────────────────────────────────── */

static void gc_do_create(gc_work_t *w)
{
    if (!gc_ensure_token()) { gc_post_result(w->cb, false, w->ctx); return; }

    char url[384];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events",
             dashboard_config_get_google_calendar_id());

    char *body_str = gc_build_event_json(&w->ev);
    char  auth_hdr[GC_TOKEN_MAX + 24];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", s_access_token);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };

    http_req_t  req  = { .url = url, .method = "POST",
                         .body = body_str, .headers = headers };
    http_resp_t resp = {0};
    bool ok = http_request(&req, &resp);
    ok = ok && (resp.status == 200 || resp.status == 201);
    LOG_I(TAG, "create_event: status=%d ok=%d", resp.status, ok);
    http_resp_free(&resp);
    free(body_str);
    gc_post_result(w->cb, ok, w->ctx);
}

static void gc_do_update(gc_work_t *w)
{
    if (!gc_ensure_token()) { gc_post_result(w->cb, false, w->ctx); return; }

    char url[512];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events/%s",
             dashboard_config_get_google_calendar_id(), w->ev.uid);

    char *body_str = gc_build_event_json(&w->ev);
    char  auth_hdr[GC_TOKEN_MAX + 24];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", s_access_token);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };

    http_req_t  req  = { .url = url, .method = "PUT",
                         .body = body_str, .headers = headers };
    http_resp_t resp = {0};
    bool ok = http_request(&req, &resp);
    ok = ok && (resp.status == 200);
    LOG_I(TAG, "update_event: status=%d ok=%d", resp.status, ok);
    http_resp_free(&resp);
    free(body_str);
    gc_post_result(w->cb, ok, w->ctx);
}

static void gc_do_delete(gc_work_t *w)
{
    if (!gc_ensure_token()) { gc_post_result(w->cb, false, w->ctx); return; }

    char url[512];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events/%s",
             dashboard_config_get_google_calendar_id(), w->uid);

    char auth_hdr[GC_TOKEN_MAX + 24];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", s_access_token);
    const char *headers[] = { auth_hdr, NULL };

    http_req_t  req  = { .url = url, .method = "DELETE",
                         .body = NULL, .headers = headers };
    http_resp_t resp = {0};
    bool ok = http_request(&req, &resp);
    ok = ok && (resp.status == 204);
    LOG_I(TAG, "delete_event: status=%d ok=%d", resp.status, ok);
    http_resp_free(&resp);
    gc_post_result(w->cb, ok, w->ctx);
}

static void gc_do_delete_future(gc_work_t *w)
{
    if (!gc_ensure_token()) { gc_post_result(w->cb, false, w->ctx); return; }

    /* Extract masterId from instance UID: pattern "{masterId}_{datetime}@..." */
    char master_id[128] = {0};
    const char *under = strchr(w->uid, '_');
    if (!under) {
        /* Not a recurring instance — fall back to single delete */
        LOG_W(TAG, "delete_future: uid has no '_' — falling back to delete: %s", w->uid);
        gc_do_delete(w);
        return;
    }
    size_t id_len = (size_t)(under - w->uid);
    if (id_len >= sizeof(master_id)) id_len = sizeof(master_id) - 1;
    memcpy(master_id, w->uid, id_len);

    /* Compute UNTIL: one day before date_str so the selected occurrence is excluded.
       RFC 5545: UNTIL is inclusive, so UNTIL=day-1T235959Z cuts off at day-1. */
    int iy, im, id_day;
    if (sscanf(w->date_str, "%d-%d-%d", &iy, &im, &id_day) != 3) {
        LOG_E(TAG, "delete_future: bad date_str '%s'", w->date_str);
        gc_post_result(w->cb, false, w->ctx);
        return;
    }
    struct tm cut = {0};
    cut.tm_year = iy - 1900;
    cut.tm_mon  = im - 1;
    cut.tm_mday = id_day - 1;  /* previous day; mktime normalises month boundary */
    cut.tm_hour = 23;
    cut.tm_min  = 59;
    cut.tm_sec  = 59;
    mktime(&cut);
    char until_str[20];
    snprintf(until_str, sizeof(until_str), "%04d%02d%02dT235959Z",
             cut.tm_year + 1900, cut.tm_mon + 1, cut.tm_mday);

    LOG_I(TAG, "delete_future: master=%s until=%s", master_id, until_str);

    char auth_hdr[GC_TOKEN_MAX + 24];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", s_access_token);

    /* Step 1: GET master event to read current recurrence rules */
    char url[512];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events/%s",
             dashboard_config_get_google_calendar_id(), master_id);

    const char *get_headers[] = { auth_hdr, NULL };
    http_req_t  get_req  = { .url = url, .method = "GET",
                              .body = NULL, .headers = get_headers };
    http_resp_t get_resp = {0};
    bool ok = http_request(&get_req, &get_resp);
    if (!ok || get_resp.status != 200) {
        LOG_W(TAG, "delete_future GET master: status=%d", get_resp.status);
        http_resp_free(&get_resp);
        gc_post_result(w->cb, false, w->ctx);
        return;
    }

    cJSON *root = cJSON_Parse(get_resp.body);
    http_resp_free(&get_resp);
    if (!root) {
        gc_post_result(w->cb, false, w->ctx);
        return;
    }

    /* Step 2: Rebuild the recurrence array with the updated RRULE */
    cJSON *orig_recur = cJSON_GetObjectItem(root, "recurrence");
    cJSON *new_recur  = cJSON_CreateArray();
    if (!cJSON_IsArray(orig_recur) || !new_recur) {
        cJSON_Delete(root);
        cJSON_Delete(new_recur);
        /* No recurrence — unexpected; delete single instance */
        gc_do_delete(w);
        return;
    }

    int n = cJSON_GetArraySize(orig_recur);
    for (int i = 0; i < n; i++) {
        cJSON *entry = cJSON_GetArrayItem(orig_recur, i);
        if (!cJSON_IsString(entry)) continue;
        const char *val = entry->valuestring;

        if (strncmp(val, "RRULE:", 6) == 0) {
            /* Strip existing UNTIL and COUNT; append new UNTIL */
            const char *rule_body = val + 6;
            char stripped[512]    = {0};
            const char *p = rule_body;
            bool first = true;
            while (*p) {
                const char *semi     = strchr(p, ';');
                size_t      part_len = semi ? (size_t)(semi - p) : strlen(p);
                if (strncmp(p, "UNTIL=", 6) != 0 && strncmp(p, "COUNT=", 6) != 0) {
                    if (!first) {
                        size_t l = strlen(stripped);
                        stripped[l]     = ';';
                        stripped[l + 1] = '\0';
                    }
                    strncat(stripped, p, part_len);
                    first = false;
                }
                if (!semi) break;
                p = semi + 1;
            }
            char new_rule[600];
            snprintf(new_rule, sizeof(new_rule), "RRULE:%s;UNTIL=%s",
                     stripped, until_str);
            cJSON_AddItemToArray(new_recur, cJSON_CreateString(new_rule));
        } else {
            cJSON_AddItemToArray(new_recur, cJSON_CreateString(val));
        }
    }

    /* Step 3: PATCH master event with new recurrence */
    cJSON *patch = cJSON_CreateObject();
    cJSON_AddItemToObject(patch, "recurrence", new_recur);
    char *patch_str = cJSON_PrintUnformatted(patch);
    cJSON_Delete(patch);
    cJSON_Delete(root);

    if (!patch_str) {
        gc_post_result(w->cb, false, w->ctx);
        return;
    }

    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events/%s",
             dashboard_config_get_google_calendar_id(), master_id);

    const char *patch_headers[] = { "Content-Type: application/json", auth_hdr, NULL };
    http_req_t  patch_req  = { .url = url, .method = "PATCH",
                                .body = patch_str, .headers = patch_headers };
    http_resp_t patch_resp = {0};
    ok = http_request(&patch_req, &patch_resp);
    ok = ok && (patch_resp.status == 200);
    LOG_I(TAG, "delete_future PATCH: status=%d ok=%d", patch_resp.status, ok);
    http_resp_free(&patch_resp);
    free(patch_str);
    gc_post_result(w->cb, ok, w->ctx);
}

static void gc_do_get_events(gc_work_t *w)
{
    if (!gc_ensure_token()) {
        gc_post_events_result(w->events_cb, false, NULL, 0, w->ctx);
        return;
    }

    char time_min[32], time_max[32];
    struct tm tm_min = *gmtime(&w->from);
    struct tm tm_max = *gmtime(&w->to);
    strftime(time_min, sizeof(time_min), "%Y-%m-%dT%H:%M:%SZ", &tm_min);
    strftime(time_max, sizeof(time_max), "%Y-%m-%dT%H:%M:%SZ", &tm_max);

    char cal_id_enc[256];
    _urlencode(w->uid, cal_id_enc, sizeof(cal_id_enc));

    char url[768];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/calendar/v3/calendars/%s/events"
             "?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime&maxResults=250",
             cal_id_enc, time_min, time_max);

    char auth_hdr[GC_TOKEN_MAX + 24];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", s_access_token);
    const char *headers[] = { auth_hdr, NULL };

    http_req_t  req  = { .url = url, .method = "GET", .body = NULL, .headers = headers };
    http_resp_t resp = {0};
    bool ok = http_request(&req, &resp);
    if (!ok || resp.status != 200) {
        LOG_W(TAG, "get_events: status=%d", resp.status);
        http_resp_free(&resp);
        gc_post_events_result(w->events_cb, false, NULL, 0, w->ctx);
        return;
    }

    cJSON *root = cJSON_Parse(resp.body);
    http_resp_free(&resp);
    if (!root) {
        gc_post_events_result(w->events_cb, false, NULL, 0, w->ctx);
        return;
    }

    cJSON *items = cJSON_GetObjectItem(root, "items");
    int total = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    gc_event_t *events = total > 0 ? calloc(total, sizeof(gc_event_t)) : NULL;
    int n = 0;

    for (int i = 0; i < total; i++) {
        cJSON      *item    = cJSON_GetArrayItem(items, i);
        const char *status  = cJSON_GetStringValue(cJSON_GetObjectItem(item, "status"));
        if (status && strcmp(status, "cancelled") == 0) continue;

        gc_event_t *ev      = &events[n];
        const char *id      = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
        const char *summary = cJSON_GetStringValue(cJSON_GetObjectItem(item, "summary"));
        if (id)      strncpy(ev->uid,     id,      sizeof(ev->uid)     - 1);
        if (summary) strncpy(ev->summary, summary, sizeof(ev->summary) - 1);

        cJSON      *s_obj = cJSON_GetObjectItem(item, "start");
        cJSON      *e_obj = cJSON_GetObjectItem(item, "end");
        const char *sdt   = cJSON_GetStringValue(cJSON_GetObjectItem(s_obj, "dateTime"));
        const char *sd    = cJSON_GetStringValue(cJSON_GetObjectItem(s_obj, "date"));
        const char *edt   = cJSON_GetStringValue(cJSON_GetObjectItem(e_obj, "dateTime"));
        const char *ed    = cJSON_GetStringValue(cJSON_GetObjectItem(e_obj, "date"));

        if (sdt) {
            strncpy(ev->start_dt, sdt, sizeof(ev->start_dt) - 1);
            ev->all_day = false;
        } else if (sd) {
            strncpy(ev->start_dt, sd, sizeof(ev->start_dt) - 1);
            ev->all_day = true;
        }
        if      (edt) strncpy(ev->end_dt, edt, sizeof(ev->end_dt) - 1);
        else if (ed)  strncpy(ev->end_dt, ed,  sizeof(ev->end_dt) - 1);
        n++;
    }
    cJSON_Delete(root);
    LOG_I(TAG, "get_events: %d event(s)", n);
    gc_post_events_result(w->events_cb, true, events, n, w->ctx);
}

/* ── Auth flow handler ──────────────────────────────────────────────────────── */

static void gc_do_auth_start(gc_work_t *w)
{
    char user_code[64] = {0}, url[128] = {0}, device_code[256] = {0};
    int  interval = 5, expires_in = 1800;

    if (!gc_http_device_code(user_code, sizeof(user_code),
                             url, sizeof(url),
                             device_code, sizeof(device_code),
                             &interval, &expires_in)) {
        s_auth_state = GC_AUTH_ERROR;
        gc_post_result(w->cb, false, w->ctx);
        return;
    }

    s_auth_state = GC_AUTH_PENDING;
    gc_post_code(w->code_cb, user_code, url, w->ctx);

    time_t start = time(NULL);
    while (s_running && !s_auth_cancel) {
#ifdef SIMULATOR
        Sleep((DWORD)(interval * 1000));
#else
        vTaskDelay(pdMS_TO_TICKS(interval * 1000));
#endif
        if (time(NULL) - start >= expires_in) {
            s_auth_state = GC_AUTH_ERROR;
            gc_post_result(w->cb, false, w->ctx);
            return;
        }
        if (gc_http_poll_token(device_code)) {
            gc_post_result(w->cb, true, w->ctx);
            return;
        }
        if (s_auth_state == GC_AUTH_ERROR) {
            gc_post_result(w->cb, false, w->ctx);
            return;
        }
    }
    /* Cancelled */
    s_auth_state = GC_AUTH_UNLINKED;
    gc_post_result(w->cb, false, w->ctx);
}

/* ── Background thread main loop ────────────────────────────────────────────── */

#ifdef SIMULATOR

static DWORD WINAPI gc_thread_fn(LPVOID arg)
{
    (void)arg;
    while (s_running) {
        WaitForSingleObject(s_work_event, INFINITE);
        while (1) {
            EnterCriticalSection(&s_lock);
            gc_work_t *w = NULL;
            if (s_queue_head != s_queue_tail) {
                w = s_queue[s_queue_head];
                s_queue_head = (s_queue_head + 1) % GC_WORK_QUEUE_SIZE;
            }
            LeaveCriticalSection(&s_lock);
            if (!w) break;

            switch (w->type) {
                case GC_WORK_AUTH_START:    gc_do_auth_start(w);     break;
                case GC_WORK_CREATE:        gc_do_create(w);         break;
                case GC_WORK_UPDATE:        gc_do_update(w);         break;
                case GC_WORK_DELETE:        gc_do_delete(w);         break;
                case GC_WORK_DELETE_FUTURE: gc_do_delete_future(w);  break;
                case GC_WORK_GET_EVENTS:    gc_do_get_events(w);     break;
            }
            free(w);
        }
        ResetEvent(s_work_event);
    }
    return 0;
}

#else /* Firmware FreeRTOS */

static void gc_task_fn(void *arg)
{
    (void)arg;
    while (1) {
        gc_work_t *w = NULL;
        if (xQueueReceive(s_work_queue, &w, portMAX_DELAY) == pdTRUE && w) {
            switch (w->type) {
                case GC_WORK_AUTH_START:    gc_do_auth_start(w);     break;
                case GC_WORK_CREATE:        gc_do_create(w);         break;
                case GC_WORK_UPDATE:        gc_do_update(w);         break;
                case GC_WORK_DELETE:        gc_do_delete(w);         break;
                case GC_WORK_DELETE_FUTURE: gc_do_delete_future(w);  break;
                case GC_WORK_GET_EVENTS:    gc_do_get_events(w);     break;
            }
            free(w);
        }
    }
}

#endif /* SIMULATOR */

/* ── Public API ─────────────────────────────────────────────────────────────── */

void gc_init(void)
{
    gc_load_tokens();

#if defined(GOOGLE_REFRESH_TOKEN) && !defined(SIMULATOR)
    /* Seed from compile-time header when NVS is blank (fresh flash / erased
     * partition).  Writes to NVS immediately so subsequent boots load normally. */
    if (!s_refresh_token[0] && GOOGLE_REFRESH_TOKEN[0]) {
        strncpy(s_refresh_token, GOOGLE_REFRESH_TOKEN, GC_TOKEN_MAX - 1);
        s_refresh_token[GC_TOKEN_MAX - 1] = '\0';
        gc_save_tokens();
        LOG_I(TAG, "gc_init: seeded refresh token from header");
    }
#endif

    if (s_refresh_token[0])
        s_auth_state = GC_AUTH_READY;
    else
        s_auth_state = GC_AUTH_UNLINKED;

#ifdef SIMULATOR
    InitializeCriticalSection(&s_lock);
    s_work_event = CreateEvent(NULL, TRUE, FALSE, NULL);
    s_running = 1;
    s_thread  = CreateThread(NULL, 0, gc_thread_fn, NULL, 0, NULL);
#else
    s_lock       = xSemaphoreCreateMutex();
    s_work_queue = xQueueCreate(GC_WORK_QUEUE_SIZE, sizeof(gc_work_t *));
    s_running    = 1;
    xTaskCreate(gc_task_fn, "gc_task", 8192, NULL, 5, &s_task);
#endif

    LOG_I(TAG, "gc_init: auth_state=%d", (int)s_auth_state);
}

gc_auth_state_t gc_auth_state(void) { return s_auth_state; }

void gc_auth_start(gc_code_cb_t on_code, gc_done_cb_t on_done, void *ctx)
{
    s_auth_cancel = 0;
    gc_work_t *w = calloc(1, sizeof(*w));
    if (!w) return;
    w->type    = GC_WORK_AUTH_START;
    w->code_cb = on_code;
    w->cb      = on_done;
    w->ctx     = ctx;
    gc_post_work(w);
}

void gc_auth_cancel(void)  { s_auth_cancel = 1; }

void gc_auth_revoke(void)
{
    s_refresh_token[0] = '\0';
    s_access_token[0]  = '\0';
    s_token_expiry     = 0;
    s_auth_state       = GC_AUTH_UNLINKED;
    gc_save_tokens();
    LOG_I(TAG, "Tokens revoked");
}

void gc_create_event(const gc_event_t *ev, gc_done_cb_t cb, void *ctx)
{
    gc_work_t *w = calloc(1, sizeof(*w));
    if (!w) { if (cb) cb(false, ctx); return; }
    w->type = GC_WORK_CREATE;
    w->ev   = *ev;
    w->cb   = cb;
    w->ctx  = ctx;
    gc_post_work(w);
}

void gc_update_event(const gc_event_t *ev, gc_done_cb_t cb, void *ctx)
{
    gc_work_t *w = calloc(1, sizeof(*w));
    if (!w) { if (cb) cb(false, ctx); return; }
    w->type = GC_WORK_UPDATE;
    w->ev   = *ev;
    w->cb   = cb;
    w->ctx  = ctx;
    gc_post_work(w);
}

void gc_delete_event(const char *uid, gc_done_cb_t cb, void *ctx)
{
    gc_work_t *w = calloc(1, sizeof(*w));
    if (!w) { if (cb) cb(false, ctx); return; }
    w->type = GC_WORK_DELETE;
    strncpy(w->uid, uid, sizeof(w->uid) - 1);
    w->cb   = cb;
    w->ctx  = ctx;
    gc_post_work(w);
}

void gc_delete_future_events(const char *uid, const char *date_str,
                             gc_done_cb_t cb, void *ctx)
{
    gc_work_t *w = calloc(1, sizeof(*w));
    if (!w) { if (cb) cb(false, ctx); return; }
    w->type = GC_WORK_DELETE_FUTURE;
    strncpy(w->uid,      uid,      sizeof(w->uid)      - 1);
    strncpy(w->date_str, date_str, sizeof(w->date_str) - 1);
    w->cb   = cb;
    w->ctx  = ctx;
    gc_post_work(w);
}

void gc_get_events(const char *cal_id, time_t from, time_t to,
                   gc_events_cb_t cb, void *ctx)
{
    gc_work_t *w = calloc(1, sizeof(*w));
    if (!w) { if (cb) cb(false, NULL, 0, ctx); return; }
    w->type      = GC_WORK_GET_EVENTS;
    strncpy(w->uid, cal_id, sizeof(w->uid) - 1);  /* uid field reused for cal_id */
    w->from      = from;
    w->to        = to;
    w->events_cb = cb;
    w->ctx       = ctx;
    gc_post_work(w);
}

/* ── Config UI ──────────────────────────────────────────────────────────────── */

static lv_obj_t *s_connect_btn    = NULL;
static lv_obj_t *s_code_label     = NULL;
static lv_obj_t *s_url_label      = NULL;
static lv_obj_t *s_status_label   = NULL;
static lv_obj_t *s_disconnect_btn = NULL;
static lv_obj_t *s_config_section = NULL;

#define _SHOW(obj, visible) \
    do { if (visible) lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); \
         else         lv_obj_add_flag(obj,    LV_OBJ_FLAG_HIDDEN); } while (0)

static void _gc_refresh_ui(void)
{
    if (!s_config_section) return;
    gc_auth_state_t st = s_auth_state;

    _SHOW(s_connect_btn,    st == GC_AUTH_UNLINKED || st == GC_AUTH_ERROR);
    _SHOW(s_code_label,     st == GC_AUTH_PENDING);
    _SHOW(s_url_label,      st == GC_AUTH_PENDING);
    _SHOW(s_status_label,   st == GC_AUTH_READY);
    _SHOW(s_disconnect_btn, st == GC_AUTH_READY);

    lv_obj_t *btn_lbl = lv_obj_get_child(s_connect_btn, 0);
    if (btn_lbl)
        lv_label_set_text(btn_lbl, st == GC_AUTH_ERROR ? "Reconnect" : "Connect");
}

static void _on_code_ready(const char *user_code, const char *url, void *ctx)
{
    (void)ctx;
    if (s_code_label) lv_label_set_text(s_code_label, user_code);
    if (s_url_label)  lv_label_set_text(s_url_label, url);
    _gc_refresh_ui();
}

static void _on_auth_done(bool ok, void *ctx)
{
    (void)ctx;
    LOG_I(TAG, "Auth done: ok=%d", ok);
    _gc_refresh_ui();
}

static void _on_connect_click(lv_event_t *e)
{
    (void)e;
    gc_auth_start(_on_code_ready, _on_auth_done, NULL);
    _gc_refresh_ui();
}

static void _on_disconnect_click(lv_event_t *e)
{
    (void)e;
    gc_auth_revoke();
    _gc_refresh_ui();
}

void gc_config_section_init(lv_obj_t *parent)
{
    s_config_section = parent;

    lv_obj_t *hdr = lv_label_create(parent);
    lv_label_set_text(hdr, "Google Calendar");
    lv_obj_set_style_text_color(hdr, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(hdr, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_top(hdr, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_left(hdr, 40, LV_PART_MAIN);
    lv_obj_set_width(hdr, LV_PCT(100));

    s_connect_btn = lv_btn_create(parent);
    lv_obj_set_size(s_connect_btn, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(s_connect_btn, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_radius(s_connect_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_connect_btn, _on_connect_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(s_connect_btn);
    lv_label_set_text(lbl, "Connect");
    lv_obj_center(lbl);

    s_code_label = lv_label_create(parent);
    lv_label_set_text(s_code_label, "");
    lv_obj_set_style_text_font(s_code_label, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_code_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_align(s_code_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_code_label, LV_PCT(100));

    s_url_label = lv_label_create(parent);
    lv_label_set_text(s_url_label, "Visit g.co/device on your phone");
    lv_obj_set_style_text_color(s_url_label, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_url_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(s_url_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(s_url_label, LV_PCT(100));

    s_status_label = lv_label_create(parent);
    lv_label_set_text(s_status_label, "Google Calendar connected");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_status_label, 40, LV_PART_MAIN);
    lv_obj_set_width(s_status_label, LV_PCT(100));

    s_disconnect_btn = lv_btn_create(parent);
    lv_obj_set_size(s_disconnect_btn, LV_PCT(100), 64);
    lv_obj_set_style_bg_color(s_disconnect_btn, lv_color_hex(COL_BORDER), LV_PART_MAIN);
    lv_obj_set_style_radius(s_disconnect_btn, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_disconnect_btn, _on_disconnect_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *dlbl = lv_label_create(s_disconnect_btn);
    lv_label_set_text(dlbl, "Disconnect");
    lv_obj_set_style_text_color(dlbl, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_center(dlbl);

    _gc_refresh_ui();
}
