/**
 * @file zhl_http.h
 * @brief Internal HTTP client abstraction.
 */

#ifndef ZHL_HTTP_H
#define ZHL_HTTP_H

#include <zhl/zhl.h>
#include <stddef.h>

typedef struct zhl_http_response {
    long   status_code;
    char  *body;
    size_t body_len;
} zhl_http_response_t;

zhl_status_t zhl_http_get(const char *url, zhl_http_response_t *out);
zhl_status_t zhl_http_download(const char *url, const char *dest_path);
void zhl_http_response_free(zhl_http_response_t *resp);

#endif
