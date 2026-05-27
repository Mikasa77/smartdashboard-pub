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

/* Pure-C SVG decoder for LVGL 9.x using NanoSVG.
 * Handles LV_COLOR_FORMAT_RAW image descriptors whose data bytes begin with
 * "<svg" or "<?xm" (covers both bare SVG and XML-declared SVG).
 * Output colour format: LV_COLOR_FORMAT_ARGB8888 (BGRA in memory).
 *
 * ── Third-party attribution ───────────────────────────────────────────────
 * This file uses NanoSVG by Mikko Mononen <memon@inside.org>.
 * NanoSVG is licensed under the zlib licence:
 *   Copyright (c) 2013-14 Mikko Mononen memon@inside.org
 *   https://github.com/memononen/nanosvg
 * The zlib licence requires that the original copyright notice is not
 * removed from nanosvg.h and nanosvgrast.h.  Do NOT strip those headers.
 * ─────────────────────────────────────────────────────────────────────────
 *
 * To build: copy nanosvg.h + nanosvgrast.h (unmodified) from
 *   https://github.com/memononen/nanosvg/tree/master/src
 * into src/ alongside this file. */

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#include "draw/lv_image_decoder_private.h"
#include "lv_nanosvg_decoder.h"
#include "dashboard_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "nanosvg";

/* ── info ────────────────────────────────────────────────────────────────── */

static lv_result_t _info(lv_image_decoder_t *dec,
                          lv_image_decoder_dsc_t *dsc,
                          lv_image_header_t *header)
{
    (void)dec;
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return LV_RESULT_INVALID;

    const lv_image_dsc_t *img = (const lv_image_dsc_t *)dsc->src;
    if (img->header.cf != LV_COLOR_FORMAT_RAW) return LV_RESULT_INVALID;
    if (img->data_size < 5)                    return LV_RESULT_INVALID;
    if (memcmp(img->data, "<svg", 4) != 0 &&
        memcmp(img->data, "<?xm", 4) != 0)     return LV_RESULT_INVALID;

    header->w      = img->header.w;
    header->h      = img->header.h;
    header->cf     = LV_COLOR_FORMAT_ARGB8888;
    header->stride = (uint32_t)(img->header.w * 4);
    return LV_RESULT_OK;
}

/* ── open ────────────────────────────────────────────────────────────────── */

static lv_result_t _open(lv_image_decoder_t *dec,
                          lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    const lv_image_dsc_t *img = (const lv_image_dsc_t *)dsc->src;
    uint32_t w = dsc->header.w;
    uint32_t h = dsc->header.h;

    /* nsvgParse() modifies its input string — copy + null-terminate */
    char *svg = malloc(img->data_size + 1);
    if (!svg) return LV_RESULT_INVALID;
    memcpy(svg, img->data, img->data_size);
    svg[img->data_size] = '\0';

    NSVGimage *nsvg = nsvgParse(svg, "px", 96.0f);
    free(svg);
    if (!nsvg) {
        LOG_W(TAG, "parse failed");
        return LV_RESULT_INVALID;
    }

    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) {
        nsvgDelete(nsvg);
        return LV_RESULT_INVALID;
    }

    /* Scale uniformly to fit target dimensions */
    float sx    = (nsvg->width  > 0.0f) ? (float)w / nsvg->width  : 1.0f;
    float sy    = (nsvg->height > 0.0f) ? (float)h / nsvg->height : 1.0f;
    float scale = (sx < sy) ? sx : sy;

    /* Rasterize into a temporary RGBA scratch buffer */
    uint8_t *rgba = malloc((size_t)(w * h * 4));
    if (!rgba) {
        nsvgDeleteRasterizer(rast);
        nsvgDelete(nsvg);
        return LV_RESULT_INVALID;
    }
    nsvgRasterize(rast, nsvg, 0.0f, 0.0f, scale,
                  rgba, (int)w, (int)h, (int)(w * 4));
    nsvgDeleteRasterizer(rast);
    nsvgDelete(nsvg);

    /* Allocate LVGL draw buffer (ARGB8888 = BGRA byte order in memory) */
    lv_draw_buf_t *buf = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_ARGB8888, 0);
    if (!buf) {
        free(rgba);
        return LV_RESULT_INVALID;
    }

    /* Copy RGBA → BGRA (swap R↔B), respecting LVGL's row stride */
    uint32_t stride = buf->header.stride;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = rgba + y * w * 4;
        uint8_t       *dst = (uint8_t *)buf->data + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            dst[x*4 + 0] = src[x*4 + 2]; /* B */
            dst[x*4 + 1] = src[x*4 + 1]; /* G */
            dst[x*4 + 2] = src[x*4 + 0]; /* R */
            dst[x*4 + 3] = src[x*4 + 3]; /* A */
        }
    }
    free(rgba);

    dsc->decoded   = buf;
    dsc->user_data = buf; /* saved for _close */
    return LV_RESULT_OK;
}

/* ── close ───────────────────────────────────────────────────────────────── */

static void _close(lv_image_decoder_t *dec,
                    lv_image_decoder_dsc_t *dsc)
{
    (void)dec;
    if (dsc->user_data) {
        lv_draw_buf_destroy((lv_draw_buf_t *)dsc->user_data);
        dsc->user_data = NULL;
        dsc->decoded   = NULL;
    }
}

/* ── public API ──────────────────────────────────────────────────────────── */

void lv_nanosvg_decoder_init(void)
{
    lv_image_decoder_t *dec = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(dec, _info);
    lv_image_decoder_set_open_cb(dec, _open);
    lv_image_decoder_set_close_cb(dec, _close);
    LOG_I(TAG, "NanoSVG decoder registered");
}
