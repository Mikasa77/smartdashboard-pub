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

#include "ha_credentials.h"
#include "ha_ws_client.h"
#include "ha_config.h"
#include "dashboard_log.h"
#include <string.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "nvs_flash.h"
#include "nvs.h"
#endif

static const char *TAG = "ha_cred";

#define NVS_NS     "dashboard"
#define HOST_MAX   128
#define TOKEN_MAX  512

static char     s_host[HOST_MAX];
static uint16_t s_port;
static char     s_token[TOKEN_MAX];

/* ── Init ──────────────────────────────────────────────────────────────────── */

void ha_credentials_init(void)
{
    /* Seed from ha_config.h compile-time defaults */
    strncpy(s_host,  HA_HOST_DEFAULT, HOST_MAX  - 1);
    s_host[HOST_MAX - 1] = '\0';
    s_port = (uint16_t)HA_PORT_DEFAULT;
    strncpy(s_token, HA_TOKEN,        TOKEN_MAX - 1);
    s_token[TOKEN_MAX - 1] = '\0';

#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        LOG_I(TAG, "NVS empty — using ha_config.h defaults (host=%s port=%u)",
              s_host, (unsigned)s_port);
        return;
    }

    char tmp_host[HOST_MAX];
    size_t len = HOST_MAX;
    if (nvs_get_str(h, HA_NVS_KEY_HOST, tmp_host, &len) == ESP_OK &&
        tmp_host[0] != '\0') {
        strncpy(s_host, tmp_host, HOST_MAX - 1);
        s_host[HOST_MAX - 1] = '\0';
    }

    uint16_t port = 0;
    if (nvs_get_u16(h, HA_NVS_KEY_PORT, &port) == ESP_OK && port != 0) {
        s_port = port;
    }

    char tmp_token[TOKEN_MAX];
    len = TOKEN_MAX;
    if (nvs_get_str(h, HA_NVS_KEY_TOKEN, tmp_token, &len) == ESP_OK &&
        tmp_token[0] != '\0') {
        strncpy(s_token, tmp_token, TOKEN_MAX - 1);
        s_token[TOKEN_MAX - 1] = '\0';
    }

    nvs_close(h);
    LOG_I(TAG, "Loaded from NVS: host=%s port=%u", s_host, (unsigned)s_port);
#else
    LOG_I(TAG, "Simulator: using ha_config.h defaults: host=%s port=%u",
          s_host, (unsigned)s_port);
#endif
}

/* ── Getters ───────────────────────────────────────────────────────────────── */

const char *ha_credentials_get_host(void)  { return s_host;  }
uint16_t    ha_credentials_get_port(void)  { return s_port;  }
const char *ha_credentials_get_token(void) { return s_token; }

/* ── Setter ────────────────────────────────────────────────────────────────── */

void ha_credentials_set(const char *host, uint16_t port, const char *token)
{
    strncpy(s_host,  host,  HOST_MAX  - 1); s_host[HOST_MAX - 1]   = '\0';
    strncpy(s_token, token, TOKEN_MAX - 1); s_token[TOKEN_MAX - 1] = '\0';
    s_port = port;

#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        LOG_E(TAG, "NVS open failed — credentials not persisted");
    } else {
        nvs_set_str(h, HA_NVS_KEY_HOST,  s_host);
        nvs_set_u16(h, HA_NVS_KEY_PORT,  s_port);
        nvs_set_str(h, HA_NVS_KEY_TOKEN, s_token);
        nvs_commit(h);
        nvs_close(h);
        LOG_I(TAG, "Credentials written to NVS: host=%s port=%u", s_host, (unsigned)s_port);
    }
#endif

    ha_ws_client_reconnect();
}
