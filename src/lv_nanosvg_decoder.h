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
#include "lvgl.h"

/*
 * Register a pure-C NanoSVG-backed LVGL image decoder.
 * Must be called after lv_init() and before any SVG icons are drawn.
 *
 * Requires nanosvg.h + nanosvgrast.h in src/ — obtain from:
 *   https://github.com/memononen/nanosvg/tree/master/src
 */
void lv_nanosvg_decoder_init(void);
