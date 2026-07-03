/**
 * @file zhl_http_curl.c
 * @brief HTTP client implementation using libcurl.
 *
 * Dependency choice: libcurl is the de-facto standard C HTTP library,
 * available on all major platforms, mature, and widely installed.
 * It is linked via find_package(CURL) in CMake.
 */

#include "zhl_http.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct mem_buf {
    char  *data;
    size_t len;
    size_t cap;
};

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    size_t total = size * nmemb;
    struct mem_buf *buf = (struct mem_buf *)userp;

    if (buf->len + total + 1 > buf->cap) {
        size_t new_cap = (buf->cap == 0) ? 4096 : buf->cap * 2;
        while (new_cap < buf->len + total + 1) new_cap *= 2;
        char *tmp = (char *)realloc(buf->data, new_cap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = new_cap;
    }

    memcpy(buf->data + buf->len, contents, total);
    buf->len += total;
    buf->data[buf->len] = '\0';
    return total;
}

static size_t file_write_cb(void *contents, size_t size, size_t nmemb, void *userp)
{
    return fwrite(contents, size, nmemb, (FILE *)userp);
}

zhl_status_t zhl_http_get(const char *url, zhl_http_response_t *out)
{
    if (!url || !out) return ZHL_ERR_NULL_PARAM;

    memset(out, 0, sizeof(*out));

    CURL *curl = curl_easy_init();
    if (!curl) return ZHL_ERR_NETWORK;

    struct mem_buf buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        free(buf.data);
        curl_easy_cleanup(curl);
        return ZHL_ERR_NETWORK;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    out->status_code = http_code;
    out->body = buf.data;
    out->body_len = buf.len;

    if (http_code < 200 || http_code >= 300) return ZHL_ERR_HTTP_ERROR;

    return ZHL_OK;
}

zhl_status_t zhl_http_download(const char *url, const char *dest_path)
{
    if (!url || !dest_path) return ZHL_ERR_NULL_PARAM;

    CURL *curl = curl_easy_init();
    if (!curl) return ZHL_ERR_NETWORK;

    FILE *fp = fopen(dest_path, "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return ZHL_ERR_DISK_WRITE;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L);

    CURLcode res = curl_easy_perform(curl);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        remove(dest_path);
        return ZHL_ERR_NETWORK;
    }

    if (http_code < 200 || http_code >= 300) {
        remove(dest_path);
        return ZHL_ERR_HTTP_ERROR;
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
