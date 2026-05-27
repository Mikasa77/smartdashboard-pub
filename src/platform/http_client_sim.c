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

#ifdef SIMULATOR

#include "http_client.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *buf; size_t len; } write_buf_t;

static size_t _write_cb(void *data, size_t size, size_t nmemb, void *userp)
{
    write_buf_t *wb    = (write_buf_t *)userp;
    size_t       total = size * nmemb;
    char        *tmp   = realloc(wb->buf, wb->len + total + 1);
    if (!tmp) return 0;
    wb->buf = tmp;
    memcpy(wb->buf + wb->len, data, total);
    wb->len += total;
    wb->buf[wb->len] = '\0';
    return total;
}

bool http_request(const http_req_t *req, http_resp_t *resp)
{
    resp->status = 0;
    resp->body   = NULL;

    CURL *curl = curl_easy_init();
    if (!curl) return false;

    write_buf_t wb = {0};
    curl_easy_setopt(curl, CURLOPT_URL,           req->url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &wb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);

    struct curl_slist *hdrs = NULL;
    if (req->headers) {
        for (int i = 0; req->headers[i]; i++)
            hdrs = curl_slist_append(hdrs, req->headers[i]);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    }

    if (req->method) {
        if (strcmp(req->method, "POST") == 0) {
            curl_easy_setopt(curl, CURLOPT_POST,          1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req->body ? req->body : "");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(req->body ? strlen(req->body) : 0));
        } else if (strcmp(req->method, "PUT") == 0) {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req->body ? req->body : "");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(req->body ? strlen(req->body) : 0));
        } else if (strcmp(req->method, "DELETE") == 0) {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
        } else if (strcmp(req->method, "PATCH") == 0) {
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    req->body ? req->body : "");
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)(req->body ? strlen(req->body) : 0));
        }
    }

    CURLcode rc = curl_easy_perform(curl);
    if (hdrs) curl_slist_free_all(hdrs);

    bool ok = (rc == CURLE_OK);
    if (ok) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp->status = (int)code;
        resp->body   = wb.buf;
    } else {
        free(wb.buf);
    }

    curl_easy_cleanup(curl);
    return ok;
}

void http_resp_free(http_resp_t *resp)
{
    free(resp->body);
    resp->body = NULL;
}

#endif /* SIMULATOR */
