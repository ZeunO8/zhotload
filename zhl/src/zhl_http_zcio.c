/**
 * @file zhl_http_zcio.c
 * @brief HTTP client implementation using zcio.
 *
 * Dependency choice: zcio (https://github.com/ZeunO8/zcio) is the streaming
 * I/O library the top-level build already fetches, and its synchronous
 * HTTP/1.1 client covers everything this backend needs (redirect following,
 * https via the OpenSSL TLS backend when available), so desktop platforms no
 * longer need libcurl. iOS and Android keep their platform backends
 * (NSURLSession / HttpURLConnection): zcio's TLS is compiled out there, and
 * the platform stacks verify against the system trust store.
 *
 * Contract notes against zcio v1.1.0.0:
 *   - zcio's client reads the response until EOF rather than stopping at
 *     Content-Length, so every request sends `Connection: close`; a compliant
 *     server then closes after responding and the read terminates promptly.
 *   - Responses are buffered in memory (zcio caps them at 64 MiB), so
 *     zhl_http_download holds the whole artifact in RAM before writing it
 *     out, unlike the old libcurl backend which streamed to disk.
 *   - Chunked transfer encoding is not decoded; download URLs must be served
 *     with Content-Length (static-file servers and zhs itself always are).
 */

#include "zhl_http.h"

#include <zcio/http.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static zcio_http_response zhl_zcio_get(const char *url)
{
    static const zcio_http_header headers[] = {
        { "Connection", "close" },
        { "User-Agent", "zhl" },
    };
    return zcio_http_request("GET", url, headers,
                             sizeof(headers) / sizeof(headers[0]), NULL, 0);
}

zhl_status_t zhl_http_get(const char *url, zhl_http_response_t *out)
{
    if (!url || !out) return ZHL_ERR_NULL_PARAM;

    memset(out, 0, sizeof(*out));

    zcio_http_response resp = zhl_zcio_get(url);
    if (resp.status == 0) {
        /* Transport-level failure: zcio returns a zeroed response. */
        zcio_http_response_free(&resp);
        return ZHL_ERR_NETWORK;
    }

    /* Steal the body instead of copying: zcio allocates every response field
     * with plain malloc and zhl_http_response_free releases with free(). */
    out->status_code = resp.status;
    out->body        = (char *)resp.body;
    out->body_len    = resp.body_size;
    resp.body        = NULL;
    resp.body_size   = 0;
    zcio_http_response_free(&resp);

    if (out->status_code < 200 || out->status_code >= 300) return ZHL_ERR_HTTP_ERROR;

    return ZHL_OK;
}

zhl_status_t zhl_http_download(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return ZHL_ERR_NULL_PARAM;

    zcio_http_response resp = zhl_zcio_get(url);
    if (resp.status == 0) {
        zcio_http_response_free(&resp);
        return ZHL_ERR_NETWORK;
    }
    if (resp.status < 200 || resp.status >= 300) {
        zcio_http_response_free(&resp);
        return ZHL_ERR_HTTP_ERROR;
    }

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        zcio_http_response_free(&resp);
        return ZHL_ERR_DISK_WRITE;
    }

    size_t written = resp.body_size ? fwrite(resp.body, 1, resp.body_size, fp) : 0;
    int close_ok = (fclose(fp) == 0);
    int write_ok = (written == resp.body_size) && close_ok;
    zcio_http_response_free(&resp);

    if (!write_ok) {
        remove(dest_path);
        return ZHL_ERR_DISK_WRITE;
    }

    return ZHL_OK;
}

void zhl_http_response_free(zhl_http_response_t *resp)
{
    if (!resp) return;
    free(resp->body);
    resp->body = NULL;
    resp->body_len = 0;
    resp->status_code = 0;
}
