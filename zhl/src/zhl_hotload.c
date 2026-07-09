/**
 * @file zhl_hotload.c
 * @brief Core hotload-apply logic: diff manifests and swap function pointers.
 */

#include "zhl_internal.h"
#include <stdlib.h>
#include <string.h>

static const zhl_export_entry_t *find_entry(const zhl_export_table_t *table,
                                            const char *name)
{
    for (uint32_t i = 0; i < table->count; i++) {
        if (strcmp(table->entries[i].name, name) == 0) {
            return &table->entries[i];
        }
    }
    return NULL;
}

static zhl_status_t read_export_table(zhl_dl_handle_t handle,
                                      const zhl_export_table_t **out)
{
    void *sym = NULL;
    zhl_status_t st = zhl_dl_sym(handle, "zhl_export_table", &sym);
    if (st != ZHL_OK) return st;

    const zhl_export_table_t *table = (const zhl_export_table_t *)sym;
    if (table->magic != ZHL_EXPORT_MAGIC) return ZHL_ERR_INVALID_MANIFEST;

    *out = table;
    return ZHL_OK;
}

/* Bounded set of distinct handles pending release once the binding loop
 * below is done reading from them. 64 covers every realistic binding count
 * (an app with more than 64 DISTINCT origin libraries feeding one context
 * would be extraordinary); the fallback if it's ever exceeded is simply to
 * release immediately, same as the old (buggy) behavior — a regression only
 * in that specific edge case, never a crash. */
#define ZHL_MAX_PENDING_RELEASES 64

zhl_status_t zhl_hotload_apply(zhl_ctx_t ctx)
{
    if (!ctx) return ZHL_ERR_NULL_PARAM;
    if (ctx->downloaded_lib_path[0] == '\0') return ZHL_ERR_NOT_CONFIGURED;
    if (!ctx->bindings || ctx->binding_count == 0) return ZHL_ERR_NOT_CONFIGURED;

    zhl_dl_handle_t new_handle = NULL;
    zhl_status_t st = zhl_dl_open(ctx->downloaded_lib_path, &new_handle);
    if (st != ZHL_OK) return st;

    const zhl_export_table_t *new_table = NULL;
    st = read_export_table(new_handle, &new_table);
    if (st != ZHL_OK) {
        zhl_dl_close(new_handle);
        return st;
    }

    zhl_dl_handle_t old_handle = ctx->slot_origins[0].lib_handle;
    const zhl_export_table_t *old_table = NULL;
    if (old_handle) {
        st = read_export_table(old_handle, &old_table);
        if (st != ZHL_OK) {
            zhl_dl_close(new_handle);
            return st;
        }
    }

    zhl_loaded_lib_t *new_lib = zhl_ctx_add_lib(ctx, ctx->downloaded_lib_path, new_handle);
    if (!new_lib) {
        zhl_dl_close(new_handle);
        return ZHL_ERR_ALLOC;
    }

    /* Handles released inline, mid-loop, would risk dlclose()-ing the very
     * library old_table (or a later binding's own old export table) points
     * into — safe with exactly one binding (the original qtx_hot usage:
     * one whole-core entry point), but a real crash the moment more than
     * one binding shares an origin library and an EARLIER binding's release
     * drops that library's refcount to zero before a LATER binding has
     * finished reading its table. Collect handles here; release them only
     * after every binding has been fully processed. */
    zhl_dl_handle_t pending_release[ZHL_MAX_PENDING_RELEASES];
    uint32_t pending_count = 0;

    for (uint32_t i = 0; i < ctx->binding_count; i++) {
        const zhl_export_entry_t *new_entry = find_entry(new_table, ctx->bindings[i].name);
        if (!new_entry) continue;

        const zhl_export_entry_t *old_entry = NULL;
        if (old_table) {
            old_entry = find_entry(old_table, ctx->bindings[i].name);
        }

        int needs_update = 0;
        if (!old_entry) {
            needs_update = 1;
        } else if (strcmp(old_entry->version, new_entry->version) != 0) {
            needs_update = 1;
        }

        if (needs_update) {
            void *old_ptr = *ctx->bindings[i].func_ptr;
            void *new_ptr = new_entry->func_ptr;

            zhl_dl_handle_t prev_handle = ctx->slot_origins[i].lib_handle;

            *ctx->bindings[i].func_ptr = new_ptr;
            ctx->slot_origins[i].lib_handle = new_handle;
            strncpy(ctx->slot_origins[i].lib_path,
                    ctx->downloaded_lib_path,
                    ZHL_MAX_PATH_LEN - 1);
            ctx->slot_origins[i].lib_path[ZHL_MAX_PATH_LEN - 1] = '\0';

            if (ctx->swap_cb) {
                ctx->swap_cb(ctx->bindings[i].name, old_ptr, new_ptr, ctx->swap_cb_data);
            }

            if (prev_handle && prev_handle != new_handle) {
                int already_pending = 0;
                for (uint32_t k = 0; k < pending_count; k++) {
                    if (pending_release[k] == prev_handle) { already_pending = 1; break; }
                }
                if (!already_pending) {
                    if (pending_count < ZHL_MAX_PENDING_RELEASES) {
                        pending_release[pending_count++] = prev_handle;
                    } else {
                        /* Overflow fallback: release now (matches the old
                         * behavior's risk profile, not a new one). */
                        zhl_ctx_release_lib(ctx, prev_handle);
                    }
                }
            }
        }
    }

    for (uint32_t k = 0; k < pending_count; k++) {
        zhl_ctx_release_lib(ctx, pending_release[k]);
    }

    ctx->downloaded_lib_path[0] = '\0';
    return ZHL_OK;
}
