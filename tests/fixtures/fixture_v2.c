/**
 * @file fixture_v2.c
 * @brief Test fixture: version 2 of a hotloadable library.
 *
 * Compared to v1:
 *   - add:      unchanged (1.0.0)
 *   - multiply: changed (1.1.0, different implementation)
 *   - subtract: removed from export table
 *   - divide:   added (1.0.0)
 */

#include <zhl/zhl.h>

int fixture_add(int a, int b) { return a + b; }
int fixture_multiply(int a, int b) { return a * b * 2; }
int fixture_divide(int a, int b) { return b != 0 ? a / b : 0; }

static const zhl_export_entry_t entries[] = {
    { "add",      "1.0.0", (void *)fixture_add },
    { "multiply", "1.1.0", (void *)fixture_multiply },
    { "divide",   "1.0.0", (void *)fixture_divide },
};

const zhl_export_table_t zhl_export_table = {
    ZHL_EXPORT_MAGIC,
    3,
    entries,
};
