/**
 * @file zhl_dl_posix.c
 * @brief POSIX implementation of dynamic library loading (dlopen/dlsym/dlclose).
 */

#include "zhl_dl.h"
#include <dlfcn.h>

zhl_status_t zhl_dl_open(const char *path, zhl_dl_handle_t *out)
{
    if (!path || !out) return ZHL_ERR_NULL_PARAM;

    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) return ZHL_ERR_DL_OPEN;

    *out = handle;
    return ZHL_OK;
}

zhl_status_t zhl_dl_sym(zhl_dl_handle_t handle, const char *name, void **out)
{
    if (!handle || !name || !out) return ZHL_ERR_NULL_PARAM;

    dlerror();
    void *sym = dlsym(handle, name);
    const char *err = dlerror();
    if (err) return ZHL_ERR_DL_SYM;

    *out = sym;
    return ZHL_OK;
}

zhl_status_t zhl_dl_close(zhl_dl_handle_t handle)
{
    if (!handle) return ZHL_ERR_NULL_PARAM;
    if (dlclose(handle) != 0) return ZHL_ERR_DL_OPEN;
    return ZHL_OK;
}
