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

/* Callback fired on the LVGL thread when art is ready (dsc != NULL) or
 * the fetch failed / was superseded (dsc == NULL).
 * The dsc pointer is owned by media_art — do not free it. */
typedef void (*media_art_cb_t)(lv_image_dsc_t *dsc, void *ud);

/* Request album art for the given HA entity_picture path
 * (e.g. "/api/media_player_proxy/media_player.portable?token=...").
 *
 * Multiple callers requesting the same path share a single HTTP fetch:
 * the second call just registers its callback alongside the first.
 * If the path differs from the in-flight fetch, the new path supersedes
 * it — pending callbacks from the old fetch receive dsc=NULL.
 * Up to MEDIA_ART_MAX_CBS callbacks may be queued at once.
 *
 * Must be called from the LVGL thread (i.e. inside an HA state callback
 * or lv_async_call). */
/* Decoded art is resized to this square in the background thread before
 * reaching LVGL — keeps the software transform negligible (max display
 * size is ENT_ART_SIZE_LARGE = 128 px). */
#define MEDIA_ART_MAX_DIM 128

#define MEDIA_ART_MAX_CBS 4
void media_art_request(const char *entity_picture_path,
                       media_art_cb_t cb, void *ud);
