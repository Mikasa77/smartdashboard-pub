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
#include "text_sub.h"
#include <stdint.h>

char *text_sub(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return dst;
    size_t wi = 0;

#define PUT(lit) do { \
    const char *_p = (lit); \
    while (*_p && wi + 1 < dst_sz) dst[wi++] = *_p++; \
} while (0)

    const unsigned char *p = (const unsigned char *)src;
    while (*p) {
        uint8_t b = *p;
        if (b < 0x80) {
            if (b >= 0x20 || b == 0x09 || b == 0x0A)
                if (wi + 1 < dst_sz) dst[wi++] = (char)b;
            p++;
        } else if ((b & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) { p++; continue; }
            uint32_t cp = ((uint32_t)(b & 0x1F) << 6) | (p[1] & 0x3F);
            p += 2;
            switch (cp) {
                case 0x00A0: PUT(" ");  break;
                case 0x00B7: PUT("."); break;
                default:               break;
            }
        } else if ((b & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) { p++; continue; }
            uint32_t cp = ((uint32_t)(b & 0x0F) << 12)
                        | ((uint32_t)(p[1] & 0x3F) << 6)
                        | (p[2] & 0x3F);
            p += 3;
            switch (cp) {
                case 0x2013: case 0x2014: PUT(" - ");  break;
                case 0x2018: case 0x2019: PUT("'");    break;
                case 0x201C: case 0x201D: PUT("\"");   break;
                case 0x2026:              PUT("...");  break;
                case 0x2022:              PUT("-");    break;
                default:                               break;
            }
        } else if ((b & 0xF8) == 0xF0) {
            /* 4-byte sequences (emoji, supplementary planes): skip */
            int len = 4;
            for (int i = 1; i < len; i++)
                if ((p[i] & 0xC0) != 0x80) { len = i; break; }
            p += len;
        } else {
            p++;
        }
    }
    dst[wi] = '\0';
#undef PUT
    return dst;
}
