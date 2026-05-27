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

/*
 * dashboard_log.h — structured logging abstraction.
 *
 * Usage: define TAG at the top of each .c file, then call LOG_* macros.
 *   static const char *TAG = "my_module";
 *   LOG_I(TAG, "Started with value %d", x);
 *
 * On Simulator: prints "[ms][LEVEL][tag] message" to stdout.
 * On Firmware:  expands to ESP_LOG* — no code change at pivot.
 */

#ifdef ESP_PLATFORM

#include "esp_log.h"
#define LOG_D(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)

#else

#include <stdio.h>
#include <time.h>

static inline unsigned long _log_elapsed_ms(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    static long s0 = -1;
    static long ns0 = 0;
    if (s0 < 0) { s0 = ts.tv_sec; ns0 = ts.tv_nsec; }
    return (unsigned long)((ts.tv_sec - s0) * 1000UL +
                           (unsigned long)(ts.tv_nsec - ns0) / 1000000UL);
}

#define LOG_D(tag, fmt, ...) printf("[%6lums][D][%s] " fmt "\n", _log_elapsed_ms(), tag, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) printf("[%6lums][I][%s] " fmt "\n", _log_elapsed_ms(), tag, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) printf("[%6lums][W][%s] " fmt "\n", _log_elapsed_ms(), tag, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) printf("[%6lums][E][%s] " fmt "\n", _log_elapsed_ms(), tag, ##__VA_ARGS__)

#endif /* ESP_PLATFORM */
