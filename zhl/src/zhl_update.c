/**
 * @file zhl_update.c
 * @brief Update checking and downloading.
 */

#include "zhl_internal.h"
#include "zhl_http.h"
#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

zhl_status_t zhl_check_for_update(zhl_ctx_t ctx, zhl_update_info_t *out)
{
    if (!ctx || !out) return ZHL_ERR_NULL_PARAM;
    if (!ctx->server_url || !ctx->app_name || !ctx->current_version) {
        return ZHL_ERR_NOT_CONFIGURED;
    }

    memset(out, 0, sizeof(*out));

    char url[ZHL_MAX_URL_LEN];
    int n = snprintf(url, sizeof(url), "%s/apps/%s/latest",
                     ctx->server_url, ctx->app_name);
    if (n < 0 || (size_t)n >= sizeof(url)) return ZHL_ERR_INVALID_URL;

    zhl_http_response_t resp = {0};
    zhl_status_t st = zhl_http_get(url, &resp);
    if (st != ZHL_OK) {
        zhl_http_response_free(&resp);
        return st;
    }

    cJSON *root = cJSON_Parse(resp.body);
    zhl_http_response_free(&resp);

    if (!root) return ZHL_ERR_MALFORMED_RESPONSE;

    const cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *j_url = cJSON_GetObjectItemCaseSensitive(root, "download_url");
    const cJSON *j_cksum = cJSON_GetObjectItemCaseSensitive(root, "checksum");

    if (!cJSON_IsString(j_ver) || !j_ver->valuestring) {
        cJSON_Delete(root);
        return ZHL_ERR_MALFORMED_RESPONSE;
    }
    if (!cJSON_IsString(j_url) || !j_url->valuestring) {
        cJSON_Delete(root);
        return ZHL_ERR_MALFORMED_RESPONSE;
    }

    strncpy(out->version, j_ver->valuestring, ZHL_MAX_VERSION_LEN - 1);
    out->version[ZHL_MAX_VERSION_LEN - 1] = '\0';

    if (j_url->valuestring[0] == '/') {
        snprintf(out->download_url, ZHL_MAX_URL_LEN, "%s%s",
                 ctx->server_url, j_url->valuestring);
    } else {
        strncpy(out->download_url, j_url->valuestring, ZHL_MAX_URL_LEN - 1);
        out->download_url[ZHL_MAX_URL_LEN - 1] = '\0';
    }

    if (cJSON_IsString(j_cksum) && j_cksum->valuestring) {
        strncpy(out->checksum, j_cksum->valuestring, ZHL_MAX_CHECKSUM_LEN - 1);
        out->checksum[ZHL_MAX_CHECKSUM_LEN - 1] = '\0';
    }

    cJSON_Delete(root);

    zhl_version_t cur, remote;
    if (zhl_version_parse(ctx->current_version, &cur) != ZHL_OK) {
        return ZHL_ERR_INVALID_VERSION;
    }
    if (zhl_version_parse(out->version, &remote) != ZHL_OK) {
        return ZHL_ERR_MALFORMED_RESPONSE;
    }

    if (zhl_version_compare(&remote, &cur) == ZHL_CMP_GREATER) {
        return ZHL_UPDATE_AVAILABLE;
    }
    return ZHL_NO_UPDATE;
}

zhl_status_t zhl_download_update(zhl_ctx_t ctx, const zhl_update_info_t *info)
{
    if (!ctx || !info) return ZHL_ERR_NULL_PARAM;
    if (!ctx->staging_dir) return ZHL_ERR_NOT_CONFIGURED;

    char dest[ZHL_MAX_PATH_LEN];
    int n = snprintf(dest, sizeof(dest), "%s/lib_%s.so",
                     ctx->staging_dir, info->version);
    if (n < 0 || (size_t)n >= sizeof(dest)) return ZHL_ERR_DISK_WRITE;

    zhl_status_t st = zhl_http_download(info->download_url, dest);
    if (st != ZHL_OK) return st;

    strncpy(ctx->downloaded_lib_path, dest, ZHL_MAX_PATH_LEN - 1);
    ctx->downloaded_lib_path[ZHL_MAX_PATH_LEN - 1] = '\0';

    return ZHL_OK;
}
