/**
 * @file zhl_dl.h
 * @brief Internal platform abstraction for dynamic library loading.
 */

#ifndef ZHL_DL_H
#define ZHL_DL_H

#include <zhl/zhl.h>

typedef void *zhl_dl_handle_t;

zhl_status_t zhl_dl_open(const char *path, zhl_dl_handle_t *out);
zhl_status_t zhl_dl_sym(zhl_dl_handle_t handle, const char *name, void **out);
zhl_status_t zhl_dl_close(zhl_dl_handle_t handle);

#endif
