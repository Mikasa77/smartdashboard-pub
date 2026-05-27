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

#include "ha_ws_client.h"
#include "ha_credentials.h"
#include "ha_config.h"
#include "dashboard_config.h"
#include "dashboard_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include "cJSON.h"
#include "lvgl.h"

static const char *TAG = "ha_ws";

/* ── Forward declarations ─────────────────────────────────────────────────── */

static void ha_lock(void);
static void ha_unlock(void);
static void ha_send_text(const char *json); /* requires lock held by caller */
static void dispatch_state_change(const char *entity_id, const char *state_str,
                                   const char *attrs_json);

/* ── Shared state ────────────────────────────────────────────────────────── */

#define MAX_SUBS    32
#define MAX_PENDING 16

typedef struct {
    char          entity_id[128];
    ha_state_cb_t cb;
    void         *user_data;
} ha_sub_t;

typedef struct {
    int              msg_id;
    char             entity_id[128];
    ha_calendar_cb_t cb;
    void            *user_data;
} ha_pending_t;

/* Requests that arrived before authentication — flushed on every auth_ok so
   calendar data also refreshes automatically on reconnect. */
typedef struct {
    char             entity_id[128];
    char             start_iso[64];
    char             end_iso[64];
    ha_calendar_cb_t cb;
    void            *user_data;
} ha_deferred_cal_t;

static ha_sub_t        s_subs[MAX_SUBS];
static int             s_sub_count      = 0;
static int             s_sub_trigger_ids[MAX_SUBS] = {
    [0 ... (MAX_SUBS-1)] = -1
}; /* msg ID for each subscribe_trigger */

static ha_pending_t    s_pending[MAX_PENDING];
static int             s_pending_count  = 0;

static ha_deferred_cal_t s_deferred_cal[MAX_PENDING];
static int               s_deferred_cal_count = 0;

static volatile int s_authenticated  = 0;
static volatile int s_running        = 0;
static int          s_next_id        = 1;
static int          s_get_states_id  = -1;

/* Last connection error string — written by WS task, read by LVGL thread.
 * Cleared on connect and auth_ok; set on transport error or auth_invalid. */
static char s_conn_error[80] = "";

/*
 * Deferred async dispatch — prevents lock inversion deadlock on ESP32.
 *
 * Problem: the HA WebSocket task calls lv_async_call() while holding
 * s_esp_lock (inside ha_on_message / dispatch_state_change). lv_async_call
 * needs the LVGL internal lock.  Simultaneously, the LVGL task can hold its
 * own lock and call ha_get_calendar_events() → ha_lock(), acquiring s_esp_lock.
 * Both threads wait for each other → deadlock.
 *
 * Fix: replace direct lv_async_call() calls inside the lock-held section with
 * HA_ASYNC_CALL(), which on ESP32 only queues the pointer.  After ha_unlock()
 * the caller calls ha_flush_async(), which dispatches to LVGL with no lock held.
 * On the Simulator the macro falls through to lv_async_call() directly
 * (Windows critical sections have a different threading model).
 */
#ifdef ESP_PLATFORM
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#define HA_DEFERRED_MAX (MAX_SUBS + MAX_PENDING)
typedef struct { lv_async_cb_t cb; void *data; } ha_async_entry_t;
static ha_async_entry_t s_async_queue[HA_DEFERRED_MAX];
static int              s_async_queue_n = 0;

static void ha_post_async(lv_async_cb_t cb, void *data)
{
    if (s_async_queue_n < HA_DEFERRED_MAX) {
        s_async_queue[s_async_queue_n].cb   = cb;
        s_async_queue[s_async_queue_n].data = data;
        s_async_queue_n++;
    } else {
        LOG_E(TAG, "deferred async queue full — dropping");
        free(data);
    }
}

static void ha_flush_async(void)
{
    int n = s_async_queue_n;
    s_async_queue_n = 0;
    if (n == 0) return;
    /* lv_async_call → lv_timer_create modifies the LVGL timer list without
     * holding lv_lock(). Calling it concurrently with lv_timer_handler()
     * (which holds lv_lock() and iterates the list) is a data race that
     * corrupts the linked list.  Hold lv_lock() here to serialise. */
    lv_lock();
    for (int i = 0; i < n; i++)
        lv_async_call(s_async_queue[i].cb, s_async_queue[i].data);
    lv_unlock();
}
#define HA_ASYNC_CALL(cb, data) ha_post_async((cb), (data))
#else
#define HA_ASYNC_CALL(cb, data) lv_async_call((cb), (data))
#endif

/* ── Async callback structs (heap-allocated, freed inside the async fn) ──── */

typedef struct {
    ha_state_cb_t cb;
    void         *user_data;
    ha_state_t    state;  /* pointers into buf[] below */
    char          buf[];  /* entity_id\0 state\0 attributes_json\0 */
} ha_state_async_t;

typedef struct {
    ha_calendar_cb_t    cb;
    void               *user_data;
    int                 count;
    ha_calendar_event_t events[];
} ha_calendar_async_t;

static void state_async_fn(void *data)
{
    ha_state_async_t *a = (ha_state_async_t *)data;
    a->cb(&a->state, a->user_data);
    free(a);
}

static void calendar_async_fn(void *data)
{
    ha_calendar_async_t *a = (ha_calendar_async_t *)data;
    a->cb(a->events, a->count, a->user_data);
    free(a);
}

/* ── Protocol helpers ────────────────────────────────────────────────────── */

static void ha_send_auth(void)
{
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"type\":\"auth\",\"access_token\":\"%s\"}",
             ha_credentials_get_token());
    ha_send_text(buf);
    LOG_D(TAG, "TX: auth");
}

/* Send subscribe_trigger for s_subs[idx]; stores msg ID in s_sub_trigger_ids[idx]. */
static void ha_send_trigger_subscribe(int idx)
{
    char buf[256];
    int id = s_next_id++;
    s_sub_trigger_ids[idx] = id;
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"subscribe_trigger\","
             "\"trigger\":{\"platform\":\"state\",\"entity_id\":\"%s\"}}",
             id, s_subs[idx].entity_id);
    ha_send_text(buf);
    LOG_D(TAG, "TX: subscribe_trigger %s id=%d", s_subs[idx].entity_id, id);
}

static void ha_fetch_initial_states(void)
{
    char buf[64];
    s_get_states_id = s_next_id++;
    snprintf(buf, sizeof(buf), "{\"id\":%d,\"type\":\"get_states\"}", s_get_states_id);
    ha_send_text(buf);
    LOG_D(TAG, "TX: get_states id=%d", s_get_states_id);
}

/* Send all deferred calendar requests now that we're authenticated.
   Called under lock from auth_ok. Keeps the deferred list so reconnects
   automatically re-fetch calendar data. */
static void ha_flush_deferred_calendar(void)
{
    for (int i = 0; i < s_deferred_cal_count; i++) {
        if (s_pending_count >= MAX_PENDING) break;

        ha_deferred_cal_t *d = &s_deferred_cal[i];
        int          id = s_next_id++;
        ha_pending_t *p = &s_pending[s_pending_count++];
        p->msg_id    = id;
        p->cb        = d->cb;
        p->user_data = d->user_data;
        snprintf(p->entity_id, sizeof(p->entity_id), "%s", d->entity_id);

        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"id\":%d,\"type\":\"call_service\","
                 "\"domain\":\"calendar\",\"service\":\"get_events\","
                 "\"target\":{\"entity_id\":\"%s\"},"
                 "\"service_data\":{\"start_date_time\":\"%s\","
                 "\"end_date_time\":\"%s\"},"
                 "\"return_response\":true}",
                 id, d->entity_id, d->start_iso, d->end_iso);
        ha_send_text(buf);
        LOG_I(TAG, "Flushed calendar request: %s (id=%d)", d->entity_id, id);
    }
}

/* Seed subscribed entities from the get_states snapshot (called under lock) */
static void dispatch_initial_states(cJSON *states_array)
{
    int dispatched = 0;
    int total = cJSON_GetArraySize(states_array);
    for (int i = 0; i < total; i++) {
        cJSON *obj   = cJSON_GetArrayItem(states_array, i);
        cJSON *eid   = cJSON_GetObjectItem(obj, "entity_id");
        cJSON *st    = cJSON_GetObjectItem(obj, "state");
        cJSON *attrs = cJSON_GetObjectItem(obj, "attributes");
        if (!cJSON_IsString(eid) || !cJSON_IsString(st)) continue;

        /* Only dispatch if there is a registered subscriber */
        int has_sub = 0;
        for (int j = 0; j < s_sub_count; j++) {
            if (strcmp(s_subs[j].entity_id, eid->valuestring) == 0) {
                has_sub = 1; break;
            }
        }
        if (!has_sub) continue;

        char *attrs_str = attrs ? cJSON_PrintUnformatted(attrs) : NULL;
        dispatch_state_change(eid->valuestring, st->valuestring, attrs_str);
        free(attrs_str);
        dispatched++;
    }
    LOG_I(TAG, "Initial state snapshot: %d subscribed entity/ies seeded", dispatched);
    s_get_states_id = -1;
}

/* ── State dispatch ──────────────────────────────────────────────────────── */

static void dispatch_state_change(const char *entity_id, const char *state_str,
                                   const char *attrs_json)
{
    for (int i = 0; i < s_sub_count; i++) {
        if (strcmp(s_subs[i].entity_id, entity_id) != 0) continue;

        size_t eid_len   = strlen(entity_id)  + 1;
        size_t state_len = strlen(state_str)   + 1;
        size_t attrs_len = attrs_json ? strlen(attrs_json) + 1 : 1;

        ha_state_async_t *a = malloc(sizeof(*a) + eid_len + state_len + attrs_len);
        if (!a) continue;

        char *p = a->buf;
        memcpy(p, entity_id, eid_len);    a->state.entity_id      = p; p += eid_len;
        memcpy(p, state_str, state_len);  a->state.state          = p; p += state_len;
        if (attrs_json) memcpy(p, attrs_json, attrs_len);
        else            *p = '\0';
        a->state.attributes_json = p;

        a->cb        = s_subs[i].cb;
        a->user_data = s_subs[i].user_data;

        HA_ASYNC_CALL(state_async_fn, a);
        LOG_D(TAG, "State dispatch: %s → %s", entity_id, state_str);
    }
}

/* ── Calendar result dispatch ────────────────────────────────────────────── */

static void dispatch_calendar_result(int msg_id, cJSON *result)
{
    for (int i = 0; i < s_pending_count; i++) {
        if (s_pending[i].msg_id != msg_id) continue;

        ha_calendar_cb_t cb        = s_pending[i].cb;
        void            *user_data = s_pending[i].user_data;
        const char      *entity_id = s_pending[i].entity_id;

        s_pending[i] = s_pending[--s_pending_count]; /* remove */

        /* call_service return_response shape:
           result = { "context": {…}, "response": { entity_id: { "events": […] } } }
           Each event: { "summary": "…", "start": "2026-05-18T10:00:00", "end": "…" }
           All-day events use date-only strings with no 'T'. */
        cJSON *response = cJSON_GetObjectItem(result, "response");
        cJSON *cal_obj  = response ? cJSON_GetObjectItem(response, entity_id) : NULL;
        cJSON *arr      = cal_obj  ? cJSON_GetObjectItem(cal_obj,  "events")  : NULL;

        if (!cJSON_IsArray(arr)) {
            ha_calendar_async_t *a = malloc(sizeof(*a));
            if (!a) return;
            a->cb = cb; a->user_data = user_data; a->count = 0;
            HA_ASYNC_CALL(calendar_async_fn, a);
            return;
        }

        int count = cJSON_GetArraySize(arr);
        ha_calendar_async_t *a =
            malloc(sizeof(*a) + (size_t)count * sizeof(ha_calendar_event_t));
        if (!a) return;
        a->cb = cb; a->user_data = user_data; a->count = count;

        for (int j = 0; j < count; j++) {
            cJSON               *ev = cJSON_GetArrayItem(arr, j);
            ha_calendar_event_t *e  = &a->events[j];
            memset(e, 0, sizeof(*e));

            /* Log raw JSON of first event so we can see all available fields */
            if (j == 0) {
                char *raw = cJSON_PrintUnformatted(ev);
                if (raw) { LOG_D(TAG, "First calendar event JSON: %s", raw); free(raw); }
            }

            cJSON *summary = cJSON_GetObjectItem(ev, "summary");
            if (cJSON_IsString(summary))
                snprintf(e->summary, sizeof(e->summary), "%s", summary->valuestring);

            cJSON *start = cJSON_GetObjectItem(ev, "start");
            if (cJSON_IsString(start)) {
                snprintf(e->start, sizeof(e->start), "%s", start->valuestring);
                /* All-day: date-only string has no 'T' and no ' ' time separator */
                e->all_day = (strchr(start->valuestring, 'T') == NULL &&
                              strchr(start->valuestring, ' ') == NULL) ? 1 : 0;
            }

            cJSON *end = cJSON_GetObjectItem(ev, "end");
            if (cJSON_IsString(end))
                snprintf(e->end, sizeof(e->end), "%s", end->valuestring);

            cJSON *uid = cJSON_GetObjectItem(ev, "uid");
            if (cJSON_IsString(uid))
                snprintf(e->uid, sizeof(e->uid), "%s", uid->valuestring);
        }

        HA_ASYNC_CALL(calendar_async_fn, a);
        LOG_I(TAG, "Calendar result id=%d: %d event(s)", msg_id, count);
        return;
    }
    LOG_W(TAG, "No pending calendar request for result id=%d", msg_id);
}

/* ── Top-level message handler (called under lock, on background thread) ─── */

static void ha_on_message(const char *text)
{
    cJSON *root = cJSON_Parse(text);
    if (!root) { LOG_W(TAG, "JSON parse failed"); return; }

    cJSON *type_j = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type_j)) { cJSON_Delete(root); return; }
    const char *type = type_j->valuestring;

    if (strcmp(type, "auth_required") == 0) {
        LOG_I(TAG, "Auth required — sending token");
        ha_send_auth();

    } else if (strcmp(type, "auth_ok") == 0) {
        LOG_I(TAG, "Authenticated successfully");
        s_conn_error[0] = '\0';
        s_authenticated = 1;
        for (int i = 0; i < s_sub_count; i++)
            ha_send_trigger_subscribe(i);
        ha_fetch_initial_states();
        ha_flush_deferred_calendar();

    } else if (strcmp(type, "auth_invalid") == 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        LOG_E(TAG, "Auth invalid: %s",
              cJSON_IsString(msg) ? msg->valuestring : "unknown");
        snprintf(s_conn_error, sizeof(s_conn_error), "Authentication failed");

    } else if (strcmp(type, "result") == 0) {
        cJSON *id_j    = cJSON_GetObjectItem(root, "id");
        cJSON *ok_j    = cJSON_GetObjectItem(root, "success");
        int    msg_id  = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : -1;
        int    success = cJSON_IsTrue(ok_j);

        int trigger_idx = -1;
        for (int i = 0; i < s_sub_count; i++) {
            if (s_sub_trigger_ids[i] == msg_id) { trigger_idx = i; break; }
        }
        if (trigger_idx >= 0) {
            if (success) LOG_I(TAG, "subscribe_trigger confirmed: %s (id=%d)",
                               s_subs[trigger_idx].entity_id, msg_id);
            else         LOG_E(TAG, "subscribe_trigger failed: %s (id=%d)",
                               s_subs[trigger_idx].entity_id, msg_id);
        } else if (success) {
            cJSON *result = cJSON_GetObjectItem(root, "result");
            if (msg_id == s_get_states_id && cJSON_IsArray(result))
                dispatch_initial_states(result);
            else if (cJSON_IsObject(result))
                dispatch_calendar_result(msg_id, result);
            else
                LOG_D(TAG, "Result id=%d OK", msg_id);
        } else {
            cJSON *err  = cJSON_GetObjectItem(root, "error");
            cJSON *code = cJSON_GetObjectItem(err, "code");
            cJSON *emsg = cJSON_GetObjectItem(err, "message");
            LOG_W(TAG, "Result id=%d failed: %s — %s", msg_id,
                  cJSON_IsString(code) ? code->valuestring : "?",
                  cJSON_IsString(emsg) ? emsg->valuestring : "?");
        }

    } else if (strcmp(type, "event") == 0) {
        cJSON *id_j   = cJSON_GetObjectItem(root, "id");
        int    sub_id = cJSON_IsNumber(id_j) ? (int)id_j->valuedouble : -1;

        int is_trigger = 0;
        for (int i = 0; i < s_sub_count; i++) {
            if (s_sub_trigger_ids[i] == sub_id) { is_trigger = 1; break; }
        }
        if (is_trigger) {
            /* subscribe_trigger shape: event.variables.trigger.to_state */
            cJSON *event    = cJSON_GetObjectItem(root,    "event");
            cJSON *vars     = cJSON_GetObjectItem(event,   "variables");
            cJSON *trig     = cJSON_GetObjectItem(vars,    "trigger");
            cJSON *to_state = cJSON_GetObjectItem(trig,    "to_state");
            cJSON *eid      = cJSON_GetObjectItem(to_state,"entity_id");
            cJSON *st       = cJSON_GetObjectItem(to_state,"state");
            cJSON *attrs    = cJSON_GetObjectItem(to_state,"attributes");

            if (cJSON_IsString(eid) && cJSON_IsString(st)) {
                char *attrs_str = attrs ? cJSON_PrintUnformatted(attrs) : NULL;
                dispatch_state_change(eid->valuestring, st->valuestring, attrs_str);
                free(attrs_str);
            }
        }
    }

    cJSON_Delete(root);
}

/* Cancel all pending calendar requests on disconnect (empty result to caller) */
static void ha_on_disconnect(void)
{
    s_authenticated = 0;
    s_get_states_id = -1;
    for (int i = 0; i < s_sub_count; i++) s_sub_trigger_ids[i] = -1;

    for (int i = 0; i < s_pending_count; i++) {
        ha_calendar_async_t *a = malloc(sizeof(*a));
        if (!a) continue;
        a->cb        = s_pending[i].cb;
        a->user_data = s_pending[i].user_data;
        a->count     = 0;
        HA_ASYNC_CALL(calendar_async_fn, a);
    }
    s_pending_count = 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Platform split                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

#ifdef ESP_PLATFORM

/* ── ESP32 / FreeRTOS (esp_websocket_client) ─────────────────────────────── */

#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static esp_websocket_client_handle_t s_client;
static SemaphoreHandle_t             s_esp_lock;

/* Reassembly buffer for fragmented WebSocket frames (e.g. large get_states response). */
static char   *s_rx_buf = NULL;
static size_t  s_rx_cap = 0;

static void ha_lock(void)   { xSemaphoreTake(s_esp_lock, portMAX_DELAY); }
static void ha_unlock(void) { xSemaphoreGive(s_esp_lock); }

static void ha_send_text(const char *json)
{
    /* Caller holds s_esp_lock; esp_websocket_client uses its own internal lock.
     * Use a bounded timeout: portMAX_DELAY can hang any caller that holds lv_lock()
     * if the TCP send buffer stalls (e.g. flaky WiFi at boot). 5 s is generous for
     * a healthy connection; a longer block implies the link is dead and will reconnect. */
    int ret = esp_websocket_client_send_text(s_client, json, (int)strlen(json),
                                             pdMS_TO_TICKS(5000));
    if (ret < 0)
        LOG_W(TAG, "ha_send_text: send failed (ret=%d) — dropped: %.60s", ret, json);
    else
        LOG_D(TAG, "TX: %s", json);
}

static void esp_ws_event_handler(void *arg, esp_event_base_t base,
                                  int32_t event_id, void *event_data)
{
    (void)arg; (void)base;
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        LOG_I(TAG, "Connected to HA");
        s_conn_error[0] = '\0';
        ha_lock();
        ha_on_disconnect(); /* reset state machine */
        ha_unlock();
        ha_flush_async();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x01 && d->data_len > 0) {
            /* esp_websocket_client splits large messages across multiple events.
               payload_offset/payload_len track position in the full message. */
            if (d->payload_offset == 0) {
                if (s_rx_cap < (size_t)d->payload_len + 1) {
                    free(s_rx_buf);
                    s_rx_buf = malloc((size_t)d->payload_len + 1);
                    s_rx_cap = s_rx_buf ? (size_t)d->payload_len + 1 : 0;
                }
            }
            if (!s_rx_buf) break;
            size_t end = (size_t)d->payload_offset + (size_t)d->data_len;
            if (end > s_rx_cap - 1) break;
            memcpy(s_rx_buf + d->payload_offset, d->data_ptr, (size_t)d->data_len);
            if (end < (size_t)d->payload_len) break; /* more chunks coming */
            s_rx_buf[end] = '\0';
            LOG_D(TAG, "RX (%d B): %.200s%s", d->payload_len, s_rx_buf,
                  d->payload_len > 200 ? "…" : "");
            ha_lock();
            ha_on_message(s_rx_buf);
            ha_unlock();
            ha_flush_async();
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        LOG_W(TAG, "Disconnected from HA — reconnecting");
        ha_lock();
        ha_on_disconnect();
        ha_unlock();
        ha_flush_async();
        break;
    case WEBSOCKET_EVENT_ERROR: {
        LOG_E(TAG, "WebSocket error");
        esp_err_t e = d->error_handle.esp_tls_last_esp_err;
        if (e == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME)
            snprintf(s_conn_error, sizeof(s_conn_error), "Cannot resolve hostname");
        else if (e == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT)
            snprintf(s_conn_error, sizeof(s_conn_error), "Connection timeout");
        else if (e == ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST)
            snprintf(s_conn_error, sizeof(s_conn_error), "Connection refused");
        else if (e == ESP_ERR_MBEDTLS_SSL_SETUP_FAILED)
            snprintf(s_conn_error, sizeof(s_conn_error), "TLS setup failed");
        else if (e != ESP_OK)
            snprintf(s_conn_error, sizeof(s_conn_error), "TLS error %s",
                     esp_err_to_name(e));
        else
            snprintf(s_conn_error, sizeof(s_conn_error), "Connection failed");
        break;
    }
    default:
        break;
    }
}

/* Build URI and attempt connection. Returns 1 on success, 0 on failure.
 * scheme must be "wss" or "ws". */
static int esp_try_connect(const char *scheme)
{
    const char *host = ha_credentials_get_host();
    uint16_t    port = ha_credentials_get_port();
    char        uri[256];
    snprintf(uri, sizeof(uri), "%s://%s:%u/api/websocket", scheme, host, (unsigned)port);

    bool verify = dashboard_config_get_ha_verify_cert();
    esp_websocket_client_config_t cfg = {
        .uri                         = uri,
        .reconnect_timeout_ms        = 10000,
        .network_timeout_ms          = 10000,
        .skip_cert_common_name_check = !verify,
        .crt_bundle_attach           = verify ? esp_crt_bundle_attach : NULL,
    };
    if (!verify)
        LOG_W(TAG, "TLS cert verification disabled — self-signed certs accepted");
    s_client = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY,
                                   esp_ws_event_handler, NULL);
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        LOG_W(TAG, "esp_websocket_client_start failed for %s (err=%d)", uri, err);
        return 0;
    }
    LOG_I(TAG, "HA WebSocket client started → %s", uri);
    return 1;
}

void ha_ws_client_start(void)
{
    s_esp_lock = xSemaphoreCreateMutex();
    bool use_tls = dashboard_config_get_ha_use_tls();
    if (use_tls) {
        if (!esp_try_connect("wss")) {
            LOG_W(TAG, "TLS unavailable, falling back to WS");
            esp_try_connect("ws");
        }
    } else {
        esp_try_connect("ws");
    }
}

void ha_ws_client_reconnect(void)
{
    LOG_I(TAG, "Reconnect requested — restarting esp_websocket_client");
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
    }
    bool use_tls = dashboard_config_get_ha_use_tls();
    if (use_tls) {
        if (!esp_try_connect("wss")) {
            LOG_W(TAG, "TLS unavailable, falling back to WS");
            esp_try_connect("ws");
        }
    } else {
        esp_try_connect("ws");
    }
}

#else /* ════════════════════════════════════════════════════════════════════ */
      /* Simulator — Win32 + WinSock2 + RFC 6455                             */
      /* ════════════════════════════════════════════════════════════════════ */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

static SOCKET           s_sock  = INVALID_SOCKET;
static CRITICAL_SECTION s_lock;
static HANDLE           s_thread;

/* Bytes from the HTTP upgrade response that belong to the WebSocket stream.
 * HA sometimes sends auth_required in the same TCP segment as the 101 header;
 * ws_connect() saves any such leftovers here so recv_exactly() can drain them
 * before calling recv(). */
static unsigned char s_pre[512];
static int           s_pre_len = 0;
static int           s_pre_pos = 0;

static void ha_lock(void)   { EnterCriticalSection(&s_lock); }
static void ha_unlock(void) { LeaveCriticalSection(&s_lock); }

/* ── Base64 encoder (RFC 4648 §4) ────────────────────────────────────────── */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_encode(const unsigned char *src, int len, char *dst)
{
    int i = 0, j = 0;
    while (len > 0) {
        unsigned a = src[i];
        unsigned b = len > 1 ? src[i + 1] : 0;
        unsigned c = len > 2 ? src[i + 2] : 0;
        dst[j++] = B64[a >> 2];
        dst[j++] = B64[((a & 3) << 4) | (b >> 4)];
        dst[j++] = len > 1 ? B64[((b & 15) << 2) | (c >> 6)] : '=';
        dst[j++] = len > 2 ? B64[c & 63] : '=';
        i   += 3;
        len -= 3;
    }
    dst[j] = '\0';
}

/* ── RFC 6455 framing ────────────────────────────────────────────────────── */

/* Client MUST mask all frames (RFC 6455 §5.3). */
static void ws_send_frame(uint8_t opcode, const char *payload, size_t plen)
{
    uint8_t hdr[14];
    int     hlen = 0;
    uint8_t mkey[4] = {0x9F, 0x3A, 0x67, 0x1B};

    hdr[hlen++] = (uint8_t)(0x80 | (opcode & 0x0F)); /* FIN + opcode */

    if (plen <= 125) {
        hdr[hlen++] = (uint8_t)(0x80 | plen);
    } else if (plen <= 0xFFFF) {
        hdr[hlen++] = 0x80 | 126;
        hdr[hlen++] = (uint8_t)(plen >> 8);
        hdr[hlen++] = (uint8_t)(plen & 0xFF);
    } else {
        hdr[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            hdr[hlen++] = (uint8_t)(plen >> (8 * i));
    }
    memcpy(hdr + hlen, mkey, 4);
    hlen += 4;

    send(s_sock, (const char *)hdr, hlen, 0);

    if (plen > 0) {
        char *masked = malloc(plen);
        if (!masked) return;
        for (size_t i = 0; i < plen; i++)
            masked[i] = ((const unsigned char *)payload)[i] ^ mkey[i & 3];
        send(s_sock, masked, (int)plen, 0);
        free(masked);
    }
}

static void ha_send_text(const char *json)
{
    if (s_sock == INVALID_SOCKET) return;
    ws_send_frame(0x01, json, strlen(json));
    LOG_D(TAG, "TX: %.200s%s", json, strlen(json) > 200 ? "…" : "");
}

/* ── Receive helpers ─────────────────────────────────────────────────────── */

static int recv_exactly(unsigned char *buf, int n)
{
    int got = 0;
    while (got < n && s_pre_pos < s_pre_len)
        buf[got++] = s_pre[s_pre_pos++];
    while (got < n) {
        int r = recv(s_sock, (char *)buf + got, n - got, 0);
        if (r <= 0) {
            if (r == SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT)
                LOG_W(TAG, "Receive timeout");
            return 0;
        }
        got += r;
    }
    return 1;
}

/* Read and process one WebSocket frame. Returns 1 to continue, 0 on close/error. */
static int ws_recv_frame(void)
{
    unsigned char hdr2[2];
    if (!recv_exactly(hdr2, 2)) return 0;

    int      opcode = hdr2[0] & 0x0F;
    int      masked = (hdr2[1] >> 7) & 1;
    uint64_t plen   = hdr2[1] & 0x7F;

    if (plen == 126) {
        unsigned char ext[2];
        if (!recv_exactly(ext, 2)) return 0;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        unsigned char ext[8];
        if (!recv_exactly(ext, 8)) return 0;
        plen = 0;
        for (int i = 0; i < 8; i++) plen = (plen << 8) | ext[i];
    }

    unsigned char mkey[4] = {0};
    if (masked && !recv_exactly(mkey, 4)) return 0;

    if (plen > 1024 * 1024) { /* 1 MB sanity cap */
        LOG_E(TAG, "Frame too large (%llu B) — disconnecting",
              (unsigned long long)plen);
        return 0;
    }

    char *payload = malloc((size_t)plen + 1);
    if (!payload) return 0;

    if (plen > 0 && !recv_exactly((unsigned char *)payload, (int)plen)) {
        free(payload); return 0;
    }
    if (masked) {
        for (uint64_t i = 0; i < plen; i++) payload[i] ^= mkey[i & 3];
    }
    payload[plen] = '\0';

    if (opcode == 0x01 || opcode == 0x00) { /* text / continuation */
        LOG_D(TAG, "RX: %.200s%s", payload, plen > 200 ? "…" : "");
        EnterCriticalSection(&s_lock);
        ha_on_message(payload);
        LeaveCriticalSection(&s_lock);

    } else if (opcode == 0x09) { /* ping → pong */
        LOG_D(TAG, "Ping — sending pong");
        EnterCriticalSection(&s_lock);
        ws_send_frame(0x0A, payload, (size_t)plen);
        LeaveCriticalSection(&s_lock);

    } else if (opcode == 0x08) { /* close */
        LOG_I(TAG, "Server sent close frame");
        free(payload);
        return 0;
    }

    free(payload);
    return 1;
}

/* ── HTTP/1.1 WebSocket upgrade ──────────────────────────────────────────── */

static int ws_connect(const char *scheme)
{
    s_pre_len = 0;
    s_pre_pos = 0;

    const char *host = ha_credentials_get_host();
    uint16_t    port = ha_credentials_get_port();
    char        port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0) {
        LOG_W(TAG, "DNS lookup failed for %s", host);
        return 0;
    }

    s_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_sock == INVALID_SOCKET) {
        freeaddrinfo(res);
        LOG_W(TAG, "socket() failed: %d", WSAGetLastError());
        return 0;
    }

    if (connect(s_sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        closesocket(s_sock); s_sock = INVALID_SOCKET;
        LOG_W(TAG, "connect() to %s:%s failed: %d", host, port_str, WSAGetLastError());
        return 0;
    }
    freeaddrinfo(res);

    /* 60-second receive timeout so a silent drop doesn't stall forever */
    DWORD rcvtimeo = 60000;
    setsockopt(s_sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&rcvtimeo, sizeof(rcvtimeo));

    /* Random WebSocket handshake key (16 bytes → 24-char base64) */
    unsigned char key_raw[16];
    for (int i = 0; i < 16; i++) key_raw[i] = (unsigned char)(rand() & 0xFF);
    char key_b64[25];
    b64_encode(key_raw, 16, key_b64);

    char req[512];
    int  reqlen = snprintf(req, sizeof(req),
        "GET /api/websocket HTTP/1.1\r\n"
        "Host: %s:%s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        host, port_str, key_b64);
    send(s_sock, req, reqlen, 0);

    /* Read HTTP response headers until \r\n\r\n */
    char resp[2048];
    int  rlen = 0, upgraded = 0;
    while (rlen < (int)sizeof(resp) - 1) {
        int r = recv(s_sock, resp + rlen, (int)sizeof(resp) - 1 - rlen, 0);
        if (r <= 0) break;
        rlen += r;
        resp[rlen] = '\0';
        if (strstr(resp, "\r\n\r\n")) { upgraded = 1; break; }
    }

    if (!upgraded || !strstr(resp, " 101 ")) {
        LOG_E(TAG, "WebSocket upgrade failed");
        closesocket(s_sock); s_sock = INVALID_SOCKET;
        return 0;
    }

    /* Save any WebSocket frame bytes already read past the HTTP headers. */
    char *body = strstr(resp, "\r\n\r\n");
    if (body) {
        body += 4;
        int leftover = rlen - (int)(body - resp);
        if (leftover > 0 && leftover <= (int)sizeof(s_pre)) {
            memcpy(s_pre, body, (size_t)leftover);
            s_pre_len = leftover;
            s_pre_pos = 0;
            LOG_D(TAG, "Saved %d leftover byte(s) from HTTP upgrade", leftover);
        }
    }

    LOG_I(TAG, "Connected to %s://%s:%s/api/websocket", scheme, host, port_str);
    return 1;
}

/* ── Background thread ───────────────────────────────────────────────────── */

static DWORD WINAPI ws_thread_fn(LPVOID arg)
{
    (void)arg;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    srand((unsigned)GetTickCount());

    while (s_running) {
        /* Use plain ws:// on the Simulator — no TLS stack available.
         * The scheme label from dashboard_config is ignored here; the
         * Simulator always connects as plain WebSocket. */
        int connected = ws_connect("ws");

        if (!connected) {
            LOG_W(TAG, "Reconnecting in 5 s…");
            Sleep(5000);
            continue;
        }

        while (s_running) {
            if (!ws_recv_frame()) break;
        }

        LOG_W(TAG, "Disconnected from HA");
        EnterCriticalSection(&s_lock);
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
        ha_on_disconnect();
        LeaveCriticalSection(&s_lock);

        if (s_running) {
            LOG_I(TAG, "Reconnecting in 5 s…");
            Sleep(5000);
        }
    }

    WSACleanup();
    return 0;
}

void ha_ws_client_start(void)
{
    InitializeCriticalSection(&s_lock);
    s_running = 1;
    s_thread  = CreateThread(NULL, 0, ws_thread_fn, NULL, 0, NULL);
    LOG_I(TAG, "HA WebSocket client started");
}

void ha_ws_client_reconnect(void)
{
    LOG_I(TAG, "Reconnect requested — closing socket");
    EnterCriticalSection(&s_lock);
    if (s_sock != INVALID_SOCKET) {
        closesocket(s_sock);
        s_sock = INVALID_SOCKET;
    }
    LeaveCriticalSection(&s_lock);
}

#endif /* ESP_PLATFORM */

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API — all platforms                                                 */
/* ═══════════════════════════════════════════════════════════════════════════ */

const char *ha_ws_get_conn_error(void) { return s_conn_error; }

void ha_subscribe(const char *entity_id, ha_state_cb_t cb, void *user_data)
{
    if (s_sub_count >= MAX_SUBS) {
        LOG_E(TAG, "ha_subscribe: table full (MAX_SUBS=%d)", MAX_SUBS);
        return;
    }

    ha_lock();
    int idx = s_sub_count++;
    ha_sub_t *s = &s_subs[idx];
    snprintf(s->entity_id, sizeof(s->entity_id), "%s", entity_id);
    s->cb                  = cb;
    s->user_data           = user_data;
    s_sub_trigger_ids[idx] = -1;
    if (s_authenticated)
        ha_send_trigger_subscribe(idx);
    ha_unlock();

    LOG_I(TAG, "Subscribed to %s", entity_id);
}

void ha_call_service(const char *domain, const char *service,
                     const char *data_json)
{
    ha_lock();
    int id = s_next_id++;
    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"call_service\","
             "\"domain\":\"%s\",\"service\":\"%s\","
             "\"service_data\":%s}",
             id, domain, service, data_json ? data_json : "{}");
    ha_send_text(buf);
    ha_unlock();

    LOG_I(TAG, "Service call: %s.%s (id=%d)", domain, service, id);
}

void ha_get_calendar_events(const char *entity_id,
                             const char *start_iso,
                             const char *end_iso,
                             ha_calendar_cb_t cb,
                             void *user_data)
{
    LOG_I(TAG, "ha_get_calendar_events: enter");
    ha_lock();
    LOG_I(TAG, "ha_get_calendar_events: lock acquired");

    if (!s_authenticated) {
        /* Not yet connected — queue for sending on auth_ok.
           Deferred list is kept across reconnects so calendar refreshes
           automatically if the connection is lost and re-established. */
        if (s_deferred_cal_count < MAX_PENDING) {
            ha_deferred_cal_t *d = &s_deferred_cal[s_deferred_cal_count++];
            snprintf(d->entity_id, sizeof(d->entity_id), "%s", entity_id);
            snprintf(d->start_iso, sizeof(d->start_iso), "%s", start_iso);
            snprintf(d->end_iso,   sizeof(d->end_iso),   "%s", end_iso);
            d->cb        = cb;
            d->user_data = user_data;
            LOG_I(TAG, "Calendar request deferred (not authenticated): %s", entity_id);
        } else {
            LOG_E(TAG, "ha_get_calendar_events: deferred table full");
        }
        ha_unlock();
        return;
    }

    if (s_pending_count >= MAX_PENDING) {
        LOG_E(TAG, "ha_get_calendar_events: pending table full");
        ha_unlock();
        return;
    }

    int          id = s_next_id++;
    ha_pending_t *p = &s_pending[s_pending_count++];
    p->msg_id    = id;
    p->cb        = cb;
    p->user_data = user_data;
    snprintf(p->entity_id, sizeof(p->entity_id), "%s", entity_id);

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"id\":%d,\"type\":\"call_service\","
             "\"domain\":\"calendar\",\"service\":\"get_events\","
             "\"target\":{\"entity_id\":\"%s\"},"
             "\"service_data\":{\"start_date_time\":\"%s\","
             "\"end_date_time\":\"%s\"},"
             "\"return_response\":true}",
             id, entity_id, start_iso, end_iso);
    LOG_I(TAG, "ha_get_calendar_events: sending (id=%d)", id);
    ha_send_text(buf);
    LOG_I(TAG, "ha_get_calendar_events: sent");
    ha_unlock();

    LOG_I(TAG, "Calendar request: %s [%s → %s] (id=%d)",
          entity_id, start_iso, end_iso, id);
}
