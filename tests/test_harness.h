/**
 * @file test_harness.h
 * @brief Minimal test harness — custom assert macros, no external dependency.
 *
 * Choice rationale: keeps the dependency graph minimal.  The macros below
 * provide file/line reporting on failure and a simple RUN_TEST driver.
 */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_tests_run    = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "  FAIL %s:%d: %s\n",                     \
                    __FILE__, __LINE__, #cond);                        \
            g_tests_failed++;                                          \
            return;                                                    \
        }                                                              \
    } while (0)

#define TEST_ASSERT_EQ(a, b)                                           \
    do {                                                               \
        if ((a) != (b)) {                                              \
            fprintf(stderr, "  FAIL %s:%d: %s != %s\n",               \
                    __FILE__, __LINE__, #a, #b);                       \
            g_tests_failed++;                                          \
            return;                                                    \
        }                                                              \
    } while (0)

#define TEST_ASSERT_STR_EQ(a, b)                                       \
    do {                                                               \
        if (strcmp((a), (b)) != 0) {                                   \
            fprintf(stderr, "  FAIL %s:%d: \"%s\" != \"%s\"\n",       \
                    __FILE__, __LINE__, (a), (b));                     \
            g_tests_failed++;                                          \
            return;                                                    \
        }                                                              \
    } while (0)

#define RUN_TEST(fn)                                                   \
    do {                                                               \
        g_tests_run++;                                                 \
        printf("  %-50s", #fn);                                        \
        fn();                                                          \
        if (g_tests_failed == prev_failed) {                           \
            printf(" PASS\n");                                         \
        } else {                                                       \
            printf("\n");                                              \
        }                                                              \
        prev_failed = g_tests_failed;                                  \
    } while (0)

#define TEST_REPORT()                                                  \
    do {                                                               \
        printf("\n%d tests run, %d passed, %d failed\n",              \
               g_tests_run,                                            \
               g_tests_run - g_tests_failed,                           \
               g_tests_failed);                                        \
        return g_tests_failed > 0 ? 1 : 0;                            \
    } while (0)

#define TEST_MAIN_BEGIN()                                              \
    int main(void) {                                                   \
        int prev_failed = 0;                                           \
        (void)prev_failed;

#define TEST_MAIN_END()                                                \
        TEST_REPORT();                                                 \
    }

#endif
