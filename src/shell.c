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

#include "shell.h"
#include "tab_registry.h"
#include "dashboard_screen.h"
#include "dashboard_log.h"
#include "dashboard_colours.h"
#include "control_sheet.h"
#include "sheet.h"
#include "lvgl.h"

static const char *TAG = "shell";

/* Layout constants — must match lv_sdl_window_create() dimensions in main.c */
#define NAV_H        52     /* slim tab bar height             */
#define NAV_PAD_H    40     /* horizontal padding (= content padding) */
#define NAV_BTN_PAD_H 24   /* extra horizontal padding on each tab button */
#define NAV_FONT     (&lv_font_montserrat_14)

static struct {
    lv_obj_t *btn;
    lv_obj_t *lbl;
    lv_obj_t *content;
} s_tab[MAX_TABS];

static int s_count  = 0;
static int s_active = -1;

/* ── Swipe-down gesture state ─────────────────────────────────────────────── */

#define SWIPE_ZONE_H   28   /* transparent strip height at top of screen       */
#define SWIPE_MIN_PX   40   /* minimum downward drag to trigger the sheet      */

static struct {
    lv_point_t press_start;
    bool       armed;
} s_swipe;

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void style_bare(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void activate_tab(int idx)
{
    if (s_active >= 0) {
        lv_obj_remove_state(s_tab[s_active].btn, LV_STATE_CHECKED);
        lv_obj_set_style_text_color(s_tab[s_active].lbl,
                                    lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
        lv_obj_add_flag(s_tab[s_active].content, LV_OBJ_FLAG_HIDDEN);
    }

    s_active = idx;
    lv_obj_add_state(s_tab[s_active].btn, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_tab[s_active].lbl,
                                lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_remove_flag(s_tab[s_active].content, LV_OBJ_FLAG_HIDDEN);

    LOG_I(TAG, "Tab → %s", tab_get_all()[idx].label);
}

static void tab_btn_cb(lv_event_t *e)
{
    activate_tab((int)(intptr_t)lv_event_get_user_data(e));
}

/* ── Swipe-down gesture callbacks ────────────────────────────────────────── */

static void _swipe_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_get_point(lv_indev_get_act(), &s_swipe.press_start);
    s_swipe.armed = true;
}

static void _swipe_released(lv_event_t *e)
{
    (void)e;
    if (!s_swipe.armed) return;
    s_swipe.armed = false;

    lv_point_t cur;
    lv_indev_get_point(lv_indev_get_act(), &cur);

    if (cur.y - s_swipe.press_start.y >= SWIPE_MIN_PX) {
        LOG_I(TAG, "Control Sheet opened");
        sheet_open_top(control_sheet_init);
    }
}

/* ── shell_activate_tab ───────────────────────────────────────────────────── */

void shell_activate_tab(int idx)
{
    if (idx < 0 || idx >= s_count) return;
    activate_tab(idx);
}

/* ── shell_init ───────────────────────────────────────────────────────────── */

void shell_init(void)
{
    const tab_entry_t *tabs = tab_get_all();
    s_count = tab_get_count();

    /* Screen background */
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    /* ── Nav bar ──────────────────────────────────────────────────────────── */
    lv_obj_t *nav = lv_obj_create(screen);
    lv_obj_set_size(nav, SCREEN_W, NAV_H);
    lv_obj_align(nav, LV_ALIGN_TOP_LEFT, 0, 0);
    style_bare(nav);

    /* Bottom border — #333333 separator between nav and content */
    lv_obj_set_style_border_width(nav, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(nav, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_border_color(nav, lv_color_hex(COL_BORDER), LV_PART_MAIN);

    /* Flex row: space-between within horizontal padding */
    lv_obj_set_layout(nav, LV_LAYOUT_FLEX);
    lv_obj_set_style_flex_flow(nav, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
    lv_obj_set_style_flex_main_place(nav, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN);
    lv_obj_set_style_flex_cross_place(nav, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_pad_left(nav, NAV_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_right(nav, NAV_PAD_H, LV_PART_MAIN);
    lv_obj_set_style_pad_top(nav, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(nav, 0, LV_PART_MAIN);

    /* ── Content area ─────────────────────────────────────────────────────── */
    lv_obj_t *content_area = lv_obj_create(screen);
    lv_obj_set_size(content_area, SCREEN_W, SCREEN_H - NAV_H);
    lv_obj_align(content_area, LV_ALIGN_TOP_LEFT, 0, NAV_H);
    style_bare(content_area);

    /* ── Per-tab: button + content container ──────────────────────────────── */
    for (int i = 0; i < s_count; i++) {

        /* Tab button */
        lv_obj_t *btn = lv_obj_create(nav);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, NAV_H);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);

        /* Button base style: transparent, flat */
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_hor(btn, NAV_BTN_PAD_H, LV_PART_MAIN);
        lv_obj_set_style_pad_column(btn, 0, LV_PART_MAIN);

        /* Suppress pressed flash */
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_PRESSED);

        /* Active state: amber bottom border = underline rule */
        lv_obj_set_style_border_width(btn, 2, LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM,
                                     LV_PART_MAIN | LV_STATE_CHECKED);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_ACCENT),
                                      LV_PART_MAIN | LV_STATE_CHECKED);

        /* Suppress focus outline */
        lv_obj_set_style_outline_width(btn, 0, LV_PART_MAIN | LV_STATE_FOCUSED);

        lv_obj_add_event_cb(btn, tab_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        /* Label */
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, tabs[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
        lv_obj_set_style_text_font(lbl, NAV_FONT, LV_PART_MAIN);
        lv_obj_center(lbl);
        /* Label intercepts hits by default in LVGL 9 — make it click-through so
           the button's own event handler fires regardless of where on the button
           the user taps. */
        lv_obj_remove_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        s_tab[i].btn = btn;
        s_tab[i].lbl = lbl;

        /* Content container — full content area, starts hidden */
        lv_obj_t *cont = lv_obj_create(content_area);
        lv_obj_set_size(cont, LV_PCT(100), LV_PCT(100));
        lv_obj_align(cont, LV_ALIGN_TOP_LEFT, 0, 0);
        style_bare(cont);
        lv_obj_add_flag(cont, LV_OBJ_FLAG_HIDDEN);

        if (tabs[i].init_fn) {
            tabs[i].init_fn(cont);
        }

        s_tab[i].content = cont;
    }

    /* Show first tab */
    if (s_count > 0) {
        activate_tab(0);
    }

    /* ── Hint pill — persistent swipe cue flush below nav bar ───────────────── */
    control_sheet_create_hint_pill(screen, NAV_H);

    /* ── Swipe-down zone — transparent strip at top edge of lv_layer_top() ── */
    lv_obj_t *swipe_zone = lv_obj_create(lv_layer_top());
    lv_obj_set_align(swipe_zone, LV_ALIGN_DEFAULT);
    lv_obj_set_pos(swipe_zone, 0, 0);
    lv_obj_set_size(swipe_zone, SCREEN_W, SWIPE_ZONE_H);
    lv_obj_set_style_bg_opa(swipe_zone, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(swipe_zone, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(swipe_zone, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(swipe_zone, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(swipe_zone, 0, LV_PART_MAIN | LV_STATE_FOCUSED);
    lv_obj_remove_flag(swipe_zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(swipe_zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(swipe_zone, _swipe_pressed,  LV_EVENT_PRESSED,  NULL);
    lv_obj_add_event_cb(swipe_zone, _swipe_released, LV_EVENT_RELEASED, NULL);

    LOG_I(TAG, "Shell ready — %d tab(s)", s_count);
}
