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

#pragma once
#include <stdbool.h>
#include <stdint.h>

/* ── Display theme ────────────────────────────────────────────────────────── */

typedef enum {
    DISPLAY_THEME_DARK  = 0,
    DISPLAY_THEME_LIGHT = 1,
    DISPLAY_THEME_AUTO  = 2,
} display_theme_t;

/* ── Default constants — single source of truth for all config defaults ───── */

#define CONF_DEFAULT_NTP_SERVER         "pool.ntp.org"
#define CONF_DEFAULT_TIMEZONE           "GMT0BST,M3.5.0/1,M10.5.0"
#define CONF_DEFAULT_DISPLAY_THEME      DISPLAY_THEME_DARK
#define CONF_DEFAULT_GOOGLE_CALENDAR_ID "primary"
#define CONF_DEFAULT_HA_HOST            "homeassist.ad.nodesolutions.co.uk"
#define CONF_DEFAULT_HA_PORT            8123
#define CONF_DEFAULT_HA_USE_TLS         false
#define CONF_DEFAULT_HA_VERIFY_CERT     true
#define CONF_DEFAULT_PANEL_HOSTNAME     "panel"

/* ── Init / persistence ───────────────────────────────────────────────────── */

/* Seed defaults then overlay any persisted values. Call before shell_init(). */
void dashboard_config_init(void);

/* True when no persisted config existed on this boot (first run / factory reset). */
bool dashboard_config_is_first_boot(void);

/* Write the current runtime config to persistent storage. */
void dashboard_config_save(void);

/* ── Getters / setters ────────────────────────────────────────────────────── */

const char      *dashboard_config_get_ntp_server(void);
void             dashboard_config_set_ntp_server(const char *server);

const char      *dashboard_config_get_google_calendar_id(void);
void             dashboard_config_set_google_calendar_id(const char *cal_id);

/* POSIX TZ string — e.g. "GMT0BST,M3.5.0/1,M10.5.0". Applied immediately
 * via putenv/tzset so localtime() reflects the new timezone. */
const char      *dashboard_config_get_timezone(void);
void             dashboard_config_set_timezone(const char *tz);

display_theme_t  dashboard_config_get_display_theme(void);
void             dashboard_config_set_display_theme(display_theme_t theme);

/* ── HA connection ────────────────────────────────────────────────────────── */

const char      *dashboard_config_get_ha_host(void);
void             dashboard_config_set_ha_host(const char *host);

uint16_t         dashboard_config_get_ha_port(void);
void             dashboard_config_set_ha_port(uint16_t port);

bool             dashboard_config_get_ha_use_tls(void);
void             dashboard_config_set_ha_use_tls(bool use_tls);

bool             dashboard_config_get_ha_verify_cert(void);
void             dashboard_config_set_ha_verify_cert(bool verify);

/* ── Device identity (mDNS hostname) ─────────────────────────────────────── */

const char      *dashboard_config_get_panel_hostname(void);
void             dashboard_config_set_panel_hostname(const char *name);

/* ── Display sleep and Do Not Disturb ────────────────────────────────────── */

bool             dashboard_config_get_sleep_enabled(void);
void             dashboard_config_set_sleep_enabled(bool enabled);

bool             dashboard_config_get_dnd_enabled(void);
void             dashboard_config_set_dnd_enabled(bool enabled);
