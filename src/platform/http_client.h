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
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char  *url;
    const char  *method;    /* "GET", "POST", "PUT", "DELETE" */
    const char  *body;      /* may be NULL */
    const char **headers;   /* NULL-terminated array of "Key: Value" strings */
} http_req_t;

typedef struct {
    int   status;           /* HTTP status code; 0 on transport error */
    char *body;             /* heap-allocated NUL-terminated response body */
} http_resp_t;

/* Synchronous request. Returns true on transport success (check resp->status
   for HTTP-level success). On false, resp->body is NULL and resp->status is 0. */
bool http_request(const http_req_t *req, http_resp_t *resp);

/* Free resp->body. Safe to call with body == NULL. */
void http_resp_free(http_resp_t *resp);
