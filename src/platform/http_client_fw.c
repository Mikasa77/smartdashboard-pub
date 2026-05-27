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

#ifndef SIMULATOR

#include "http_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "http_client_fw";

typedef struct { char *buf; int len; } fw_buf_t;

static esp_err_t _event_handler(esp_http_client_event_t *evt)
{
    fw_buf_t *wb = (fw_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && wb) {
        int new_cap = wb->len + evt->data_len + 1;
        char *tmp = realloc(wb->buf, new_cap);
        if (!tmp) return ESP_FAIL;
        wb->buf = tmp;
        memcpy(wb->buf + wb->len, evt->data, evt->data_len);
        wb->len += evt->data_len;
        wb->buf[wb->len] = '\0';
    }
    return ESP_OK;
}

bool http_request(const http_req_t *req, http_resp_t *resp)
{
    resp->status = 0;
    resp->body   = NULL;

    fw_buf_t wb = {0};

    esp_http_client_config_t cfg = {
        .url            = req->url,
        .event_handler  = _event_handler,
        .user_data      = &wb,
        .timeout_ms     = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    if (req->method) {
        if (strcmp(req->method, "POST") == 0)
            esp_http_client_set_method(client, HTTP_METHOD_POST);
        else if (strcmp(req->method, "PUT") == 0)
            esp_http_client_set_method(client, HTTP_METHOD_PUT);
        else if (strcmp(req->method, "DELETE") == 0)
            esp_http_client_set_method(client, HTTP_METHOD_DELETE);
        else if (strcmp(req->method, "PATCH") == 0)
            esp_http_client_set_method(client, HTTP_METHOD_PATCH);
    }

    if (req->headers) {
        for (int i = 0; req->headers[i]; i++) {
            const char *h = req->headers[i];
            const char *colon = strchr(h, ':');
            if (!colon) continue;
            char key[128] = {0};
            size_t key_len = (size_t)(colon - h);
            if (key_len >= sizeof(key)) key_len = sizeof(key) - 1;
            strncpy(key, h, key_len);
            key[key_len] = '\0';
            esp_http_client_set_header(client, key, colon + 2);
        }
    }

    if (req->body)
        esp_http_client_set_post_field(client, req->body, (int)strlen(req->body));

    esp_err_t err = esp_http_client_perform(client);
    bool ok = (err == ESP_OK);
    if (ok) {
        resp->status = esp_http_client_get_status_code(client);
        resp->body   = wb.buf;
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(wb.buf);
    }

    esp_http_client_cleanup(client);
    return ok;
}

void http_resp_free(http_resp_t *resp)
{
    free(resp->body);
    resp->body = NULL;
}

#endif /* !SIMULATOR */
