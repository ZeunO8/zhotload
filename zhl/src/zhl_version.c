/**
 * @file zhl_version.c
 * @brief Version parsing, comparison, and zhl's own version reporting.
 */

#include <zhl/zhl.h>
#include <zhl/version.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

const char *zhl_version_string(void)
{
    return ZHL_VERSION_STRING;
}

uint32_t zhl_version_major(void)
{
    return ZHL_VERSION_MAJOR;
}

uint32_t zhl_version_minor(void)
{
    return ZHL_VERSION_MINOR;
}

uint32_t zhl_version_patch(void)
{
    return ZHL_VERSION_PATCH;
}

uint32_t zhl_version_tweak(void)
{
    return ZHL_VERSION_TWEAK;
}

zhl_status_t zhl_version_parse(const char *str, zhl_version_t *out)
{
    if (!str || !out) return ZHL_ERR_NULL_PARAM;
    if (str[0] == '\0') return ZHL_ERR_EMPTY_STRING;

    unsigned int major = 0, minor = 0, patch = 0;
    char trailing = '\0';

    int n = sscanf(str, "%u.%u.%u%c", &major, &minor, &patch, &trailing);
    if (n != 3) return ZHL_ERR_INVALID_VERSION;

    const char *p = str;
    while (*p) {
        if (*p != '.' && !isdigit((unsigned char)*p)) {
            return ZHL_ERR_INVALID_VERSION;
        }
        p++;
    }

    out->major = major;
    out->minor = minor;
    out->patch = patch;
    return ZHL_OK;
}

zhl_cmp_t zhl_version_compare(const zhl_version_t *a, const zhl_version_t *b)
{
    if (!a || !b) return ZHL_CMP_EQUAL;

    if (a->major < b->major) return ZHL_CMP_LESS;
    if (a->major > b->major) return ZHL_CMP_GREATER;
    if (a->minor < b->minor) return ZHL_CMP_LESS;
    if (a->minor > b->minor) return ZHL_CMP_GREATER;
    if (a->patch < b->patch) return ZHL_CMP_LESS;
    if (a->patch > b->patch) return ZHL_CMP_GREATER;
    return ZHL_CMP_EQUAL;
}
