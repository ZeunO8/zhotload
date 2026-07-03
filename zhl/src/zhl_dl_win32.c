/**
 * @file zhl_dl_win32.c
 * @brief Windows implementation of dynamic library loading.
 */

#include "zhl_dl.h"

#ifndef _WIN32
#error "This file should only be compiled on Windows."
#endif

#include <windows.h>

zhl_status_t zhl_dl_open(const char *path, zhl_dl_handle_t *out)
{
    if (!path || !out) return ZHL_ERR_NULL_PARAM;

    HMODULE h = LoadLibraryA(path);
    if (!h) return ZHL_ERR_DL_OPEN;

    *out = (zhl_dl_handle_t)h;
    return ZHL_OK;
}

zhl_status_t zhl_dl_sym(zhl_dl_handle_t handle, const char *name, void **out)
{
    if (!handle || !name || !out) return ZHL_ERR_NULL_PARAM;

    FARPROC proc = GetProcAddress((HMODULE)handle, name);
    if (!proc) return ZHL_ERR_DL_SYM;

    *out = (void *)proc;
    return ZHL_OK;
}

zhl_status_t zhl_dl_close(zhl_dl_handle_t handle)
{
    if (!handle) return ZHL_ERR_NULL_PARAM;

    if (!FreeLibrary((HMODULE)handle)) return ZHL_ERR_DL_OPEN;
    return ZHL_OK;
}
