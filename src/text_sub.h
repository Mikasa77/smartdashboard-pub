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
#include <stddef.h>

/* Sanitise `src` into caller-supplied `dst` (size `dst_sz`).
 * Replaces multi-byte UTF-8 sequences outside the supported glyph range
 * with ASCII equivalents or removes them. Always NUL-terminates dst.
 * Returns dst. */
char *text_sub(char *dst, size_t dst_sz, const char *src);
