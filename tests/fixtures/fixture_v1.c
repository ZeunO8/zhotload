/**
 * @file fixture_v1.c
 * @brief Test fixture: version 1 of a hotloadable library.
 *
 * Exports: add (1.0.0), multiply (1.0.0), subtract (1.0.0).
 */

#include <zhl/zhl.h>

int fixture_add(int a, int b) { return a + b; }
int fixture_multiply(int a, int b) { return a * b; }
int fixture_subtract(int a, int b) { return a - b; }

static const zhl_export_entry_t entries[] = {
    { "add",      "1.0.0", (void *)fixture_add },
    { "multiply", "1.0.0", (void *)fixture_multiply },
    { "subtract", "1.0.0", (void *)fixture_subtract },
};

const zhl_export_table_t zhl_export_table = {
    ZHL_EXPORT_MAGIC,
    3,
    entries,
};
