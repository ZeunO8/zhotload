/**
 * @file fixture_bad.c
 * @brief Test fixture: library with an invalid export table (wrong magic).
 */

#include <zhl/zhl.h>

static const zhl_export_entry_t entries[] = {
    { "noop", "1.0.0", (void *)0 },
};

const zhl_export_table_t zhl_export_table = {
    0xDEADBEEF,
    1,
    entries,
};
