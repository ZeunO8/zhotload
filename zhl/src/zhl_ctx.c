/**
 * @file zhl_ctx.c
 * @brief Context creation, destruction, configuration, and registration.
 */

#include "zhl_internal.h"
#include <zcio/crypto.h>
#include <stdlib.h>
#include <string.h>

char *zhl_strdup(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = (char *)malloc(len + 1);
    if (d) memcpy(d, s, len + 1);
    return d;
}

zhl_status_t zhl_set_str(char **dst, const char *src)
{
    if (!dst) return ZHL_ERR_NULL_PARAM;
    if (!src) return ZHL_ERR_NULL_PARAM;
    if (src[0] == '\0') return ZHL_ERR_EMPTY_STRING;

    char *copy = zhl_strdup(src);
    if (!copy) return ZHL_ERR_ALLOC;

    free(*dst);
    *dst = copy;
    return ZHL_OK;
}

int zhl_is_valid_url(const char *url)
{
    if (!url || url[0] == '\0') return 0;
    if (strncmp(url, "http://", 7) == 0) return 1;
    if (strncmp(url, "https://", 8) == 0) return 1;
    return 0;
}

zhl_loaded_lib_t *zhl_ctx_find_lib(zhl_ctx_t ctx, const char *path)
{
    for (uint32_t i = 0; i < ctx->loaded_lib_count; i++) {
        if (strcmp(ctx->loaded_libs[i].path, path) == 0) {
            return &ctx->loaded_libs[i];
        }
    }
    return NULL;
}

zhl_loaded_lib_t *zhl_ctx_add_lib(zhl_ctx_t ctx, const char *path, zhl_dl_handle_t handle)
{
    zhl_loaded_lib_t *existing = zhl_ctx_find_lib(ctx, path);
    if (existing) {
        existing->ref_count++;
        return existing;
    }

    uint32_t n = ctx->loaded_lib_count + 1;
    zhl_loaded_lib_t *tmp = (zhl_loaded_lib_t *)realloc(
        ctx->loaded_libs, n * sizeof(zhl_loaded_lib_t));
    if (!tmp) return NULL;

    ctx->loaded_libs = tmp;
    zhl_loaded_lib_t *lib = &ctx->loaded_libs[ctx->loaded_lib_count];
    lib->handle = handle;
    strncpy(lib->path, path, ZHL_MAX_PATH_LEN - 1);
    lib->path[ZHL_MAX_PATH_LEN - 1] = '\0';
    lib->ref_count = 1;
    ctx->loaded_lib_count = n;
    return lib;
}

void zhl_ctx_release_lib(zhl_ctx_t ctx, zhl_dl_handle_t handle)
{
    for (uint32_t i = 0; i < ctx->loaded_lib_count; i++) {
        if (ctx->loaded_libs[i].handle == handle) {
            ctx->loaded_libs[i].ref_count--;
            if (ctx->loaded_libs[i].ref_count == 0) {
                zhl_dl_close(ctx->loaded_libs[i].handle);
                uint32_t last = ctx->loaded_lib_count - 1;
                if (i != last) {
                    ctx->loaded_libs[i] = ctx->loaded_libs[last];
                }
                ctx->loaded_lib_count--;
            }
            return;
        }
    }
}

zhl_status_t zhl_ctx_create(zhl_ctx_t *out)
{
    if (!out) return ZHL_ERR_NULL_PARAM;

    struct zhl_ctx_impl *ctx = (struct zhl_ctx_impl *)calloc(1, sizeof(*ctx));
    if (!ctx) return ZHL_ERR_ALLOC;

    *out = ctx;
    return ZHL_OK;
}

void zhl_ctx_destroy(zhl_ctx_t *ctx)
{
    if (!ctx || !*ctx) return;

    struct zhl_ctx_impl *c = *ctx;

    for (uint32_t i = 0; i < c->loaded_lib_count; i++) {
        zhl_dl_close(c->loaded_libs[i].handle);
    }

    free(c->loaded_libs);
    free(c->bindings);
    free(c->slot_origins);
    free(c->server_url);
    free(c->app_name);
    free(c->current_version);
    free(c->current_lib_path);
    free(c->staging_dir);
    free(c->platform);
    free(c);
    *ctx = NULL;
}

zhl_status_t zhl_ctx_set_server_url(zhl_ctx_t ctx, const char *url)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!url) return ZHL_ERR_NULL_PARAM;
    if (url[0] == '\0') return ZHL_ERR_EMPTY_STRING;
    if (!zhl_is_valid_url(url)) return ZHL_ERR_INVALID_URL;

    size_t len = strlen(url);
    char cleaned[ZHL_MAX_URL_LEN];
    strncpy(cleaned, url, ZHL_MAX_URL_LEN - 1);
    cleaned[ZHL_MAX_URL_LEN - 1] = '\0';
    if (len > 0 && cleaned[len - 1] == '/') {
        cleaned[len - 1] = '\0';
    }

    return zhl_set_str(&ctx->server_url, cleaned);
}

zhl_status_t zhl_ctx_set_app_name(zhl_ctx_t ctx, const char *name)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!name) return ZHL_ERR_NULL_PARAM;
    if (name[0] == '\0') return ZHL_ERR_EMPTY_STRING;
    return zhl_set_str(&ctx->app_name, name);
}

zhl_status_t zhl_ctx_set_current_version(zhl_ctx_t ctx, const char *version)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!version) return ZHL_ERR_NULL_PARAM;
    if (version[0] == '\0') return ZHL_ERR_EMPTY_STRING;

    zhl_version_t v;
    zhl_status_t st = zhl_version_parse(version, &v);
    if (st != ZHL_OK) return st;

    return zhl_set_str(&ctx->current_version, version);
}

zhl_status_t zhl_ctx_set_poll_interval(zhl_ctx_t ctx, uint32_t seconds)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    ctx->poll_interval = seconds;
    return ZHL_OK;
}

zhl_status_t zhl_ctx_set_current_lib_path(zhl_ctx_t ctx, const char *path)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!path) return ZHL_ERR_NULL_PARAM;
    if (path[0] == '\0') return ZHL_ERR_EMPTY_STRING;
    return zhl_set_str(&ctx->current_lib_path, path);
}

zhl_status_t zhl_ctx_set_staging_dir(zhl_ctx_t ctx, const char *path)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!path) return ZHL_ERR_NULL_PARAM;
    if (path[0] == '\0') return ZHL_ERR_EMPTY_STRING;
    return zhl_set_str(&ctx->staging_dir, path);
}

zhl_status_t zhl_ctx_set_platform(zhl_ctx_t ctx, const char *platform)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!platform || platform[0] == '\0') {
        free(ctx->platform);
        ctx->platform = NULL;
        return ZHL_OK;
    }
    return zhl_set_str(&ctx->platform, platform);
}

zhl_status_t zhl_ctx_set_trusted_key(zhl_ctx_t ctx, const char *pubkey_hex)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (!pubkey_hex) {
        memset(ctx->trusted_key, 0, sizeof(ctx->trusted_key));
        ctx->trusted_key_set = 0;
        return ZHL_OK;
    }
    uint8_t key[32];
    if (strlen(pubkey_hex) != 64 ||
        zcio_hex_decode(pubkey_hex, key, sizeof(key)) != 32) {
        return ZHL_ERR_INVALID_KEY;
    }
    memcpy(ctx->trusted_key, key, sizeof(key));
    ctx->trusted_key_set = 1;
    return ZHL_OK;
}

zhl_status_t zhl_register_bindings(zhl_ctx_t          ctx,
                                   zhl_func_binding_t *bindings,
                                   uint32_t            count)
{
    if (!ctx || !bindings) return ZHL_ERR_NULL_PARAM;
    if (!ctx->current_lib_path) return ZHL_ERR_NOT_CONFIGURED;

    zhl_dl_handle_t handle = NULL;
    zhl_status_t st = zhl_dl_open(ctx->current_lib_path, &handle);
    if (st != ZHL_OK) return st;

    zhl_loaded_lib_t *lib = zhl_ctx_add_lib(ctx, ctx->current_lib_path, handle);
    if (!lib) {
        zhl_dl_close(handle);
        return ZHL_ERR_ALLOC;
    }

    void *sym = NULL;
    st = zhl_dl_sym(handle, "zhl_export_table", &sym);
    if (st != ZHL_OK) {
        zhl_ctx_release_lib(ctx, handle);
        return st;
    }

    const zhl_export_table_t *table = (const zhl_export_table_t *)sym;
    if (table->magic != ZHL_EXPORT_MAGIC) {
        zhl_ctx_release_lib(ctx, handle);
        return ZHL_ERR_INVALID_MANIFEST;
    }

    free(ctx->bindings);
    free(ctx->slot_origins);

    ctx->bindings = (zhl_func_binding_t *)malloc(count * sizeof(zhl_func_binding_t));
    ctx->slot_origins = (zhl_slot_origin_t *)calloc(count, sizeof(zhl_slot_origin_t));
    if (!ctx->bindings || !ctx->slot_origins) {
        free(ctx->bindings);
        free(ctx->slot_origins);
        ctx->bindings = NULL;
        ctx->slot_origins = NULL;
        ctx->binding_count = 0;
        zhl_ctx_release_lib(ctx, handle);
        return ZHL_ERR_ALLOC;
    }

    memcpy(ctx->bindings, bindings, count * sizeof(zhl_func_binding_t));
    ctx->binding_count = count;

    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j < table->count; j++) {
            if (strcmp(ctx->bindings[i].name, table->entries[j].name) == 0) {
                *ctx->bindings[i].func_ptr = table->entries[j].func_ptr;
                ctx->slot_origins[i].lib_handle = handle;
                strncpy(ctx->slot_origins[i].lib_path,
                        ctx->current_lib_path,
                        ZHL_MAX_PATH_LEN - 1);
                ctx->slot_origins[i].lib_path[ZHL_MAX_PATH_LEN - 1] = '\0';
                break;
            }
        }
    }

    return ZHL_OK;
}

zhl_status_t zhl_set_swap_callback(zhl_ctx_t ctx, zhl_swap_callback_t cb, void *user_data)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    ctx->swap_cb = cb;
    ctx->swap_cb_data = user_data;
    return ZHL_OK;
}

zhl_status_t zhl_ctx_get_active_version(const zhl_ctx_t ctx, const char **out)
{
    if (!ctx || !out) return ZHL_ERR_NULL_PARAM;
    *out = ctx->current_version;
    return ZHL_OK;
}
