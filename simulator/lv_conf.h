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

/**
 * lv_conf.h — LVGL 9.x configuration for the Smart Dashboard PC simulator.
 *
 * This file is simulator-specific. The firmware target will have its own
 * lv_conf.h configured for ESP32-P4 / RGB565.
 */
#if 1  /* Set to 0 to disable (use default config) */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Mark this build as the simulator target. Also passed as a CMake
   compile definition so non-LVGL files get it without including LVGL. */
#define SIMULATOR 1

/*==============================================================
  COLOR
 *==============================================================*/
/* 32-bit ARGB8888 for SDL2. Firmware uses 16-bit RGB565. */
#define LV_COLOR_DEPTH 32

/*==============================================================
  MEMORY
 *==============================================================*/
#define LV_MEM_SIZE (4U * 1024U * 1024U)   /* 4 MB */

#define LV_USE_STDLIB_MALLOC    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_STRING    LV_STDLIB_BUILTIN
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_BUILTIN

/*==============================================================
  OPERATING SYSTEM
 *==============================================================*/
/* Single-threaded — lv_timer_handler() driven from the main loop. */
#define LV_USE_OS   LV_OS_NONE

/*==============================================================
  FONTS
 *==============================================================*/
/* Montserrat as a stand-in until Inter bitmap fonts are generated. */
#define LV_FONT_MONTSERRAT_14   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_18   1
#define LV_FONT_MONTSERRAT_22   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_32   1
#define LV_FONT_MONTSERRAT_40   1
#define LV_FONT_MONTSERRAT_48   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_16

/*==============================================================
  LOGGING (LVGL internal — not to be confused with dashboard_log.h)
 *==============================================================*/
#define LV_USE_LOG      1
#define LV_LOG_LEVEL    LV_LOG_LEVEL_WARN   /* suppress LVGL trace noise */
#define LV_LOG_PRINTF   1

/*==============================================================
  SDL2 DISPLAY & INPUT DRIVER (built into LVGL 9.x)
 *==============================================================*/
#define LV_USE_SDL              1
#define LV_SDL_INCLUDE_PATH     <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE      LV_DISPLAY_RENDER_MODE_PARTIAL
#define LV_SDL_BUF_COUNT        2
#define LV_SDL_FULLSCREEN       0
#define LV_SDL_DIRECT_EXIT      1

/*==============================================================
  SVG (standalone — no ThorVG dependency)
 *==============================================================*/
#define LV_USE_FLOAT            1
#define LV_USE_MATRIX           1
#define LV_USE_THORVG_INTERNAL  1
#define LV_USE_VECTOR_GRAPHIC   1
#define LV_USE_SVG              1

/*==============================================================
  PERFORMANCE MONITOR OVERLAY
 *==============================================================*/
#define LV_USE_SYSMON       1
#define LV_USE_PERF_MONITOR 1
#define LV_USE_MEM_MONITOR  0

#endif /* LV_CONF_H */
#endif /* Content enable */
