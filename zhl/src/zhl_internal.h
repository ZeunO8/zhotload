/**
 * @file zhl_internal.h
 * @brief Internal context structure and helpers — not part of the public API.
 */

#ifndef ZHL_INTERNAL_H
#define ZHL_INTERNAL_H

#include <zhl/zhl.h>
#include "zhl_dl.h"

#define ZHL_MAX_PATH_LEN 1024

typedef struct zhl_slot_origin {
    zhl_dl_handle_t lib_handle;
    char            lib_path[ZHL_MAX_PATH_LEN];
} zhl_slot_origin_t;

typedef struct zhl_loaded_lib {
    zhl_dl_handle_t handle;
    char            path[ZHL_MAX_PATH_LEN];
    uint32_t        ref_count;
} zhl_loaded_lib_t;

struct zhl_ctx_impl {
    char    *server_url;
    char    *app_name;
    char    *current_version;
    char    *current_lib_path;
    char    *staging_dir;
    char    *platform;   /* optional; "" -> update checks omit ?platform= */
    uint32_t poll_interval;

    zhl_func_binding_t *bindings;
    uint32_t            binding_count;
    zhl_slot_origin_t  *slot_origins;

    zhl_swap_callback_t swap_cb;
    void               *swap_cb_data;

    zhl_loaded_lib_t *loaded_libs;
    uint32_t          loaded_lib_count;

    char downloaded_lib_path[ZHL_MAX_PATH_LEN];

    uint8_t trusted_key[32];   /* Ed25519 public key                          */
    int     trusted_key_set;   /* 0 = signatures not required (legacy mode)   */
};

char *zhl_strdup(const char *s);
zhl_status_t zhl_set_str(char **dst, const char *src);
int zhl_is_valid_url(const char *url);

zhl_loaded_lib_t *zhl_ctx_find_lib(zhl_ctx_t ctx, const char *path);
zhl_loaded_lib_t *zhl_ctx_add_lib(zhl_ctx_t ctx, const char *path, zhl_dl_handle_t handle);
void zhl_ctx_release_lib(zhl_ctx_t ctx, zhl_dl_handle_t handle);

#endif
