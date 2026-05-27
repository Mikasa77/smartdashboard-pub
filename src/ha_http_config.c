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

#ifdef ESP_PLATFORM

#include "ha_http_config.h"
#include "ha_credentials.h"
#include "dashboard_config.h"
#include "dashboard_log.h"
#include "esp_http_server.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "ha_http_cfg";

/* ── URL decoding helper ───────────────────────────────────────────────────── */

static void url_decode(char *dst, const char *src, size_t max)
{
    size_t i = 0;
    while (*src && i < max - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
        i++;
    }
    *dst = '\0';
}

/* ── Form-body parameter lookup ────────────────────────────────────────────── */

/* Returns pointer to the value portion of key=value in an
 * application/x-www-form-urlencoded body, or NULL if key not found. */
static char *find_param(char *body, const char *key)
{
    size_t klen = strlen(key);
    char  *p    = body;
    while ((p = strstr(p, key)) != NULL) {
        if (p == body || *(p - 1) == '&') {
            if (p[klen] == '=') return p + klen + 1;
        }
        p++;
    }
    return NULL;
}

/* ── GET /config — HTML form ────────────────────────────────────────────────── */

static esp_err_t get_config_handler(httpd_req_t *req)
{
    bool use_tls = dashboard_config_get_ha_use_tls();
    bool verify  = dashboard_config_get_ha_verify_cert();
    char buf[2048];
    snprintf(buf, sizeof(buf),
        "<!DOCTYPE html><html><head><title>Dashboard Config</title>"
        "<style>"
        "body{font-family:sans-serif;max-width:480px;margin:40px auto;padding:0 16px;"
        "background:#1a1a1a;color:#e5e5e5}"
        "h2{color:#f59e0b}"
        "label{display:block;margin-top:16px;font-weight:bold;color:#aaa}"
        "input[type=text],input[type=number],input[type=password]{"
        "width:100%%;box-sizing:border-box;padding:8px;margin-top:4px;"
        "font-size:14px;background:#2a2a2a;color:#e5e5e5;border:1px solid #444;"
        "border-radius:4px}"
        "input[type=checkbox]{margin-right:8px}"
        ".cb-row{display:flex;align-items:center;margin-top:16px}"
        ".cb-row label{margin:0 0 0 4px;font-weight:bold;color:#aaa}"
        "button{margin-top:24px;padding:12px 24px;background:#f59e0b;color:#000;"
        "border:none;font-size:16px;font-weight:bold;cursor:pointer;border-radius:4px}"
        "button:hover{background:#d97706}"
        ".note{font-size:12px;color:#777;margin-top:4px}"
        "</style></head>"
        "<body><h2>Dashboard HA Config</h2>"
        "<form method='POST' action='/config'>"
        "<label>HA Host"
        "<input type='text' name='host' value='%s'></label>"
        "<label>HA Port"
        "<input type='number' name='port' value='%u' min='1' max='65535'></label>"
        "<label>HA Token"
        "<input type='password' name='token' placeholder='Leave blank to keep current'></label>"
        "<p class='note'>Token is not displayed for security. Leave blank to keep the current token.</p>"
        "<div class='cb-row'>"
        "<input type='checkbox' name='use_tls' id='ut' value='1' %s>"
        "<label for='ut'>Use TLS (HTTPS/WSS)</label></div>"
        "<div class='cb-row'>"
        "<input type='checkbox' name='verify_cert' id='vc' value='1' %s>"
        "<label for='vc'>Verify TLS Certificate</label></div>"
        "<p class='note'>Uncheck Verify for self-signed certs.</p>"
        "<button type='submit'>Save &amp; Reconnect</button>"
        "</form></body></html>",
        ha_credentials_get_host(),
        (unsigned)ha_credentials_get_port(),
        use_tls ? "checked" : "",
        verify  ? "checked" : "");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── POST /config — parse, validate, write NVS ──────────────────────────────── */

static esp_err_t post_config_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    int  received   = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_FAIL;
    }
    body[received] = '\0';

    /* Start with current values as defaults */
    char     host[128];
    char     token[512];
    uint16_t port    = ha_credentials_get_port();

    strncpy(host,  ha_credentials_get_host(),  sizeof(host)  - 1); host[sizeof(host)  - 1] = '\0';
    strncpy(token, ha_credentials_get_token(), sizeof(token) - 1); token[sizeof(token) - 1] = '\0';

    /* Parse host */
    char *p = find_param(body, "host");
    if (p) {
        char raw[128] = {0};
        char *end = strchr(p, '&');
        int   len = end ? (int)(end - p) : (int)strlen(p);
        if (len > 0 && len < (int)sizeof(raw)) {
            memcpy(raw, p, (size_t)len);
            raw[len] = '\0';
            url_decode(host, raw, sizeof(host));
        }
    }

    /* Parse port */
    p = find_param(body, "port");
    if (p) {
        char tmp[16] = {0};
        char *end = strchr(p, '&');
        int   len = end ? (int)(end - p) : (int)strlen(p);
        if (len > 0 && len < (int)sizeof(tmp)) {
            memcpy(tmp, p, (size_t)len);
            tmp[len] = '\0';
            int v = atoi(tmp);
            if (v > 0 && v <= 65535) port = (uint16_t)v;
        }
    }

    /* Parse token — only replace if non-empty */
    p = find_param(body, "token");
    if (p && *p != '&' && *p != '\0') {
        char raw[512] = {0};
        char *end = strchr(p, '&');
        int   len = end ? (int)(end - p) : (int)strlen(p);
        if (len > 0 && len < (int)sizeof(raw)) {
            memcpy(raw, p, (size_t)len);
            raw[len] = '\0';
            char decoded[512] = {0};
            url_decode(decoded, raw, sizeof(decoded));
            if (decoded[0] != '\0') {
                strncpy(token, decoded, sizeof(token) - 1);
                token[sizeof(token) - 1] = '\0';
            }
        }
    }

    /* Parse use_tls — checkbox: present=1, absent=0 */
    bool use_tls = find_param(body, "use_tls") != NULL;

    /* Parse verify_cert — checkbox: present=1, absent=0 */
    bool verify_cert = find_param(body, "verify_cert") != NULL;

    /* Validate — host must be non-empty */
    if (host[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Host must not be empty");
        return ESP_FAIL;
    }

    LOG_I(TAG, "Saving: host=%s port=%u tls=%d verify=%d", host, (unsigned)port, use_tls, verify_cert);
    ha_credentials_set(host, port, token);
    dashboard_config_set_ha_use_tls(use_tls);
    dashboard_config_set_ha_verify_cert(verify_cert);
    dashboard_config_save();

    const char *resp =
        "<!DOCTYPE html><html><head><title>Saved</title>"
        "<style>body{font-family:sans-serif;max-width:480px;margin:40px auto;"
        "padding:0 16px;background:#1a1a1a;color:#e5e5e5}"
        "h2{color:#f59e0b}a{color:#f59e0b}</style></head>"
        "<body><h2>Saved!</h2>"
        "<p>New credentials written to NVS. Reconnecting to Home Assistant...</p>"
        "<p><a href='/config'>Back to config</a></p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* ── Start ──────────────────────────────────────────────────────────────────── */

void ha_http_config_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = 80;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        LOG_E(TAG, "Failed to start HTTP config server on port 80");
        return;
    }

    static const httpd_uri_t get_uri = {
        .uri     = "/config",
        .method  = HTTP_GET,
        .handler = get_config_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t post_uri = {
        .uri     = "/config",
        .method  = HTTP_POST,
        .handler = post_config_handler,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(server, &get_uri);
    httpd_register_uri_handler(server, &post_uri);
    LOG_I(TAG, "Config HTTP server started on port 80 — GET/POST /config");
}

#endif /* ESP_PLATFORM */
