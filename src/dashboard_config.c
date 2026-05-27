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

#include "dashboard_config.h"
#include "ha_config.h"
#include "dashboard_log.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#ifndef SIMULATOR
#include "nvs_flash.h"
#include "nvs.h"
#endif

static const char *TAG = "dashboard_config";

#define CONF_FILE            "dashboard.conf"
#define NVS_NAMESPACE        "dashboard"
#define NTP_MAX              64
#define TZ_MAX               64
#define GCAL_ID_MAX          128
#define HA_HOST_MAX          128
#define PANEL_HOSTNAME_MAX   64

static struct {
    char            ntp_server[NTP_MAX];
    char            timezone[TZ_MAX];
    display_theme_t display_theme;
    char            google_calendar_id[GCAL_ID_MAX];
    char            ha_host[HA_HOST_MAX];
    uint16_t        ha_port;
    bool            ha_use_tls;
    bool            ha_verify_cert;
    bool            sleep_enabled;
    bool            dnd_enabled;
    char            panel_hostname[PANEL_HOSTNAME_MAX];
} s_cfg;

static bool s_first_boot = true;

/* ── Timezone application ─────────────────────────────────────────────────── */

static void apply_timezone(const char *tz)
{
    /* putenv holds a reference on some platforms — static buffer is required. */
    static char env_buf[TZ_MAX + 3];
    snprintf(env_buf, sizeof(env_buf), "TZ=%s", tz);
    putenv(env_buf);
    tzset();
    LOG_I(TAG, "TZ applied: %s", tz);
}

/* ── Defaults ─────────────────────────────────────────────────────────────── */

static void seed_defaults(void)
{
    strncpy(s_cfg.ntp_server, CONF_DEFAULT_NTP_SERVER, NTP_MAX - 1);
    s_cfg.ntp_server[NTP_MAX - 1] = '\0';
    strncpy(s_cfg.timezone, CONF_DEFAULT_TIMEZONE, TZ_MAX - 1);
    s_cfg.timezone[TZ_MAX - 1] = '\0';
    s_cfg.display_theme = CONF_DEFAULT_DISPLAY_THEME;
    strncpy(s_cfg.google_calendar_id, CONF_DEFAULT_GOOGLE_CALENDAR_ID, GCAL_ID_MAX - 1);
    s_cfg.google_calendar_id[GCAL_ID_MAX - 1] = '\0';
    strncpy(s_cfg.ha_host, CONF_DEFAULT_HA_HOST, HA_HOST_MAX - 1);
    s_cfg.ha_host[HA_HOST_MAX - 1] = '\0';
    s_cfg.ha_port         = CONF_DEFAULT_HA_PORT;
    s_cfg.ha_use_tls      = CONF_DEFAULT_HA_USE_TLS;
    s_cfg.ha_verify_cert  = CONF_DEFAULT_HA_VERIFY_CERT;
    s_cfg.sleep_enabled   = false;
    s_cfg.dnd_enabled     = false;
    strncpy(s_cfg.panel_hostname, CONF_DEFAULT_PANEL_HOSTNAME, PANEL_HOSTNAME_MAX - 1);
    s_cfg.panel_hostname[PANEL_HOSTNAME_MAX - 1] = '\0';
}

/* ── Simulator: key=value text file ───────────────────────────────────────── */

#ifdef SIMULATOR

static bool load_from_file(void)
{
    FILE *f = fopen(CONF_FILE, "r");
    if (!f) return false;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        val[strcspn(val, "\r\n")] = '\0';

        if (strcmp(key, "ntp_server") == 0) {
            strncpy(s_cfg.ntp_server, val, NTP_MAX - 1);
            s_cfg.ntp_server[NTP_MAX - 1] = '\0';
        } else if (strcmp(key, "timezone") == 0) {
            strncpy(s_cfg.timezone, val, TZ_MAX - 1);
            s_cfg.timezone[TZ_MAX - 1] = '\0';
        } else if (strcmp(key, "display_theme") == 0) {
            s_cfg.display_theme = (display_theme_t)atoi(val);
        } else if (strcmp(key, "google_calendar_id") == 0) {
            strncpy(s_cfg.google_calendar_id, val, GCAL_ID_MAX - 1);
            s_cfg.google_calendar_id[GCAL_ID_MAX - 1] = '\0';
        } else if (strcmp(key, HA_NVS_KEY_HOST) == 0) {
            strncpy(s_cfg.ha_host, val, HA_HOST_MAX - 1);
            s_cfg.ha_host[HA_HOST_MAX - 1] = '\0';
        } else if (strcmp(key, HA_NVS_KEY_PORT) == 0) {
            s_cfg.ha_port = (uint16_t)atoi(val);
        } else if (strcmp(key, HA_NVS_KEY_USE_TLS) == 0) {
            s_cfg.ha_use_tls = (bool)atoi(val);
        } else if (strcmp(key, "ha_verify_cert") == 0) {
            s_cfg.ha_verify_cert = (bool)atoi(val);
        } else if (strcmp(key, "sleep_enabled") == 0) {
            s_cfg.sleep_enabled = (bool)atoi(val);
        } else if (strcmp(key, "dnd_enabled") == 0) {
            s_cfg.dnd_enabled = (bool)atoi(val);
        } else if (strcmp(key, "panel_hostname") == 0) {
            strncpy(s_cfg.panel_hostname, val, PANEL_HOSTNAME_MAX - 1);
            s_cfg.panel_hostname[PANEL_HOSTNAME_MAX - 1] = '\0';
        }
    }
    fclose(f);
    return true;
}

static void save_to_file(void)
{
    FILE *f = fopen(CONF_FILE, "w");
    if (!f) {
        LOG_W(TAG, "Cannot write %s", CONF_FILE);
        return;
    }
    fprintf(f, "ntp_server=%s\n", s_cfg.ntp_server);
    fprintf(f, "timezone=%s\n",   s_cfg.timezone);
    fprintf(f, "display_theme=%d\n", (int)s_cfg.display_theme);
    fprintf(f, "google_calendar_id=%s\n", s_cfg.google_calendar_id);
    fprintf(f, "%s=%s\n",  HA_NVS_KEY_HOST,    s_cfg.ha_host);
    fprintf(f, "%s=%u\n",  HA_NVS_KEY_PORT,    (unsigned)s_cfg.ha_port);
    fprintf(f, "%s=%d\n",  HA_NVS_KEY_USE_TLS, (int)s_cfg.ha_use_tls);
    fprintf(f, "ha_verify_cert=%d\n",            (int)s_cfg.ha_verify_cert);
    fprintf(f, "sleep_enabled=%d\n",             (int)s_cfg.sleep_enabled);
    fprintf(f, "dnd_enabled=%d\n",       (int)s_cfg.dnd_enabled);
    fprintf(f, "panel_hostname=%s\n",    s_cfg.panel_hostname);
    fclose(f);
    LOG_I(TAG, "Saved to %s", CONF_FILE);
}

#else
/* ── Firmware: NVS namespace "dashboard" ──────────────────────────────────── */
/* Caller must have run nvs_flash_init() before dashboard_config_init(). */

static bool load_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len;

    len = NTP_MAX;
    nvs_get_str(h, "ntp_server", s_cfg.ntp_server, &len);

    len = TZ_MAX;
    nvs_get_str(h, "timezone", s_cfg.timezone, &len);

    len = GCAL_ID_MAX;
    nvs_get_str(h, "gcal_id", s_cfg.google_calendar_id, &len);

    uint8_t theme = 0;
    if (nvs_get_u8(h, "display_theme", &theme) == ESP_OK)
        s_cfg.display_theme = (display_theme_t)theme;

    len = HA_HOST_MAX;
    nvs_get_str(h, HA_NVS_KEY_HOST, s_cfg.ha_host, &len);

    uint16_t port = 0;
    if (nvs_get_u16(h, HA_NVS_KEY_PORT, &port) == ESP_OK)
        s_cfg.ha_port = port;

    uint8_t use_tls = 0;
    if (nvs_get_u8(h, HA_NVS_KEY_USE_TLS, &use_tls) == ESP_OK)
        s_cfg.ha_use_tls = (bool)use_tls;

    uint8_t verify_cert = (uint8_t)CONF_DEFAULT_HA_VERIFY_CERT;
    if (nvs_get_u8(h, "ha_verify_cert", &verify_cert) == ESP_OK)
        s_cfg.ha_verify_cert = (bool)verify_cert;

    uint8_t sleep_en = 0;
    if (nvs_get_u8(h, "sleep_enabled", &sleep_en) == ESP_OK)
        s_cfg.sleep_enabled = (bool)sleep_en;

    uint8_t dnd_en = 0;
    if (nvs_get_u8(h, "dnd_enabled", &dnd_en) == ESP_OK)
        s_cfg.dnd_enabled = (bool)dnd_en;

    len = PANEL_HOSTNAME_MAX;
    nvs_get_str(h, "panel_host", s_cfg.panel_hostname, &len);

    nvs_close(h);
    return true;
}

static void save_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        LOG_W(TAG, "Cannot open NVS namespace");
        return;
    }
    nvs_set_str(h, "ntp_server", s_cfg.ntp_server);
    nvs_set_str(h, "timezone",   s_cfg.timezone);
    nvs_set_u8(h,  "display_theme", (uint8_t)s_cfg.display_theme);
    nvs_set_str(h, "gcal_id",    s_cfg.google_calendar_id);
    nvs_set_str(h, HA_NVS_KEY_HOST,    s_cfg.ha_host);
    nvs_set_u16(h, HA_NVS_KEY_PORT,    s_cfg.ha_port);
    nvs_set_u8(h,  HA_NVS_KEY_USE_TLS,  (uint8_t)s_cfg.ha_use_tls);
    nvs_set_u8(h,  "ha_verify_cert",   (uint8_t)s_cfg.ha_verify_cert);
    nvs_set_u8(h,  "sleep_enabled",    (uint8_t)s_cfg.sleep_enabled);
    nvs_set_u8(h,  "dnd_enabled",      (uint8_t)s_cfg.dnd_enabled);
    nvs_set_str(h, "panel_host",       s_cfg.panel_hostname);
    nvs_commit(h);
    nvs_close(h);
    LOG_I(TAG, "Saved to NVS");
}

#endif /* SIMULATOR */

/* ── Public API ───────────────────────────────────────────────────────────── */

void dashboard_config_init(void)
{
    seed_defaults();
#ifdef SIMULATOR
    s_first_boot = !load_from_file();
#else
    s_first_boot = !load_from_nvs();
#endif

    apply_timezone(s_cfg.timezone);

    if (s_first_boot) {
        LOG_I(TAG, "First boot — writing defaults (ntp=%s tz=%s theme=%d)",
              s_cfg.ntp_server, s_cfg.timezone, (int)s_cfg.display_theme);
        dashboard_config_save();
    } else {
        LOG_I(TAG, "Config loaded (ntp=%s tz=%s theme=%d)",
              s_cfg.ntp_server, s_cfg.timezone, (int)s_cfg.display_theme);
    }
}

bool dashboard_config_is_first_boot(void) { return s_first_boot; }

void dashboard_config_save(void)
{
#ifdef SIMULATOR
    save_to_file();
#else
    save_to_nvs();
#endif
}

const char     *dashboard_config_get_ntp_server(void)   { return s_cfg.ntp_server; }
display_theme_t dashboard_config_get_display_theme(void) { return s_cfg.display_theme; }
const char     *dashboard_config_get_timezone(void)      { return s_cfg.timezone; }

void dashboard_config_set_ntp_server(const char *server)
{
    strncpy(s_cfg.ntp_server, server, NTP_MAX - 1);
    s_cfg.ntp_server[NTP_MAX - 1] = '\0';
}

void dashboard_config_set_timezone(const char *tz)
{
    strncpy(s_cfg.timezone, tz, TZ_MAX - 1);
    s_cfg.timezone[TZ_MAX - 1] = '\0';
    apply_timezone(s_cfg.timezone);
}

void dashboard_config_set_display_theme(display_theme_t theme)
{
    s_cfg.display_theme = theme;
}

const char *dashboard_config_get_google_calendar_id(void)
{
    return s_cfg.google_calendar_id;
}

void dashboard_config_set_google_calendar_id(const char *cal_id)
{
    strncpy(s_cfg.google_calendar_id, cal_id, GCAL_ID_MAX - 1);
    s_cfg.google_calendar_id[GCAL_ID_MAX - 1] = '\0';
}

const char *dashboard_config_get_ha_host(void) { return s_cfg.ha_host; }

void dashboard_config_set_ha_host(const char *host)
{
    strncpy(s_cfg.ha_host, host, HA_HOST_MAX - 1);
    s_cfg.ha_host[HA_HOST_MAX - 1] = '\0';
}

uint16_t dashboard_config_get_ha_port(void) { return s_cfg.ha_port; }

void dashboard_config_set_ha_port(uint16_t port) { s_cfg.ha_port = port; }

bool dashboard_config_get_ha_use_tls(void) { return s_cfg.ha_use_tls; }

void dashboard_config_set_ha_use_tls(bool use_tls) { s_cfg.ha_use_tls = use_tls; }

bool dashboard_config_get_ha_verify_cert(void) { return s_cfg.ha_verify_cert; }

void dashboard_config_set_ha_verify_cert(bool verify) { s_cfg.ha_verify_cert = verify; }

bool dashboard_config_get_sleep_enabled(void) { return s_cfg.sleep_enabled; }

void dashboard_config_set_sleep_enabled(bool enabled) { s_cfg.sleep_enabled = enabled; }

bool dashboard_config_get_dnd_enabled(void) { return s_cfg.dnd_enabled; }

void dashboard_config_set_dnd_enabled(bool enabled) { s_cfg.dnd_enabled = enabled; }

const char *dashboard_config_get_panel_hostname(void) { return s_cfg.panel_hostname; }

void dashboard_config_set_panel_hostname(const char *name)
{
    strncpy(s_cfg.panel_hostname, name, PANEL_HOSTNAME_MAX - 1);
    s_cfg.panel_hostname[PANEL_HOSTNAME_MAX - 1] = '\0';
}
