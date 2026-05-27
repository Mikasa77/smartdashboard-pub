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

#include "lvgl.h"
#include "dashboard_log.h"
#include "dashboard_colours.h"
#include "dashboard_config.h"
#include "tab_registry.h"
#include "tab_home.h"
#include "tab_entertainment.h"
#include "shell.h"
#include "ha_ws_client.h"
#include "ha_credentials.h"
#include "display_sleep.h"
#include "google_calendar.h"
#include <SDL2/SDL.h>
#include <curl/curl.h>
#include <time.h>

#define SIM_W  800
#define SIM_H  1280

static const char *TAG = "main";

/* ── Placeholder tab init functions ──────────────────────────────────────── */

static void _placeholder(lv_obj_t *parent, const char *name)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_MUTE), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_center(lbl);
}

static void tab_heating_init(lv_obj_t *p)  { _placeholder(p, "Heating"); }
static void tab_lighting_init(lv_obj_t *p)       { _placeholder(p, "Lighting"); }
static void tab_power_init(lv_obj_t *p)          { _placeholder(p, "Power"); }

/* ── Entry point ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    lv_init();

    lv_display_t *disp = lv_sdl_window_create(SIM_W, SIM_H);
    if (!disp) {
        fprintf(stderr, "ERROR: failed to create SDL2 display\n");
        return 1;
    }

    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_t *kb    = lv_sdl_keyboard_create();
    (void)mouse;
    (void)kb;

    LOG_I(TAG, "Smart Dashboard simulator %dx%d", SIM_W, SIM_H);

    curl_global_init(CURL_GLOBAL_DEFAULT);

    dashboard_config_init();
    ha_credentials_init();
    gc_init();
    ha_ws_client_start();
    display_sleep_init();

    /* Register tabs — order defines Nav Bar order */
    tab_register("home",          "Home",          tab_home_init);
    tab_register("heating",       "Heating",       tab_heating_init);
    tab_register("lighting",      "Lighting",      tab_lighting_init);
    tab_register("entertainment", "Entertainment", tab_entertainment_init);
    tab_register("power",         "Power",         tab_power_init);

    shell_init();

    LOG_I(TAG, "Ready");

    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms > 10) delay_ms = 10;
        SDL_Delay(delay_ms);
    }

    return 0;
}
