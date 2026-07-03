/**
 * @file test_hotload_apply.c
 * @brief Tests for zhl_hotload_apply — the core hotload mechanism.
 */

#include "test_harness.h"
#include <zhl/zhl.h>
#include "zhl_internal.h"
#include <string.h>

#ifndef FIXTURE_V1_PATH
#define FIXTURE_V1_PATH "libfixture_v1.so"
#endif

#ifndef FIXTURE_V2_PATH
#define FIXTURE_V2_PATH "libfixture_v2.so"
#endif

#ifndef FIXTURE_BAD_PATH
#define FIXTURE_BAD_PATH "libfixture_bad.so"
#endif

typedef int (*func_ii_t)(int, int);

static int g_swap_count = 0;
static char g_swap_name[64] = {0};

static void swap_cb(const char *name, void *old_ptr, void *new_ptr, void *ud)
{
    (void)old_ptr;
    (void)new_ptr;
    (void)ud;
    g_swap_count++;
    strncpy(g_swap_name, name, sizeof(g_swap_name) - 1);
}

static void test_hotload_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_hotload_apply(NULL), ZHL_ERR_NULL_PARAM);
}

static void test_hotload_not_configured(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_hotload_apply(ctx), ZHL_ERR_NOT_CONFIGURED);
    zhl_ctx_destroy(&ctx);
}

static void test_hotload_no_download(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_V1_PATH);

    void *fp = NULL;
    zhl_func_binding_t b = { "add", &fp };
    zhl_register_bindings(ctx, &b, 1);

    TEST_ASSERT_EQ(zhl_hotload_apply(ctx), ZHL_ERR_NOT_CONFIGURED);
    zhl_ctx_destroy(&ctx);
}

static void test_hotload_no_changes(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_V1_PATH);

    func_ii_t fp_add = NULL;
    func_ii_t fp_mul = NULL;
    func_ii_t fp_sub = NULL;
    zhl_func_binding_t bindings[] = {
        { "add",      (void **)&fp_add },
        { "multiply", (void **)&fp_mul },
        { "subtract", (void **)&fp_sub },
    };
    zhl_register_bindings(ctx, bindings, 3);

    func_ii_t old_add = fp_add;
    func_ii_t old_mul = fp_mul;
    func_ii_t old_sub = fp_sub;

    struct zhl_ctx_impl *impl = (struct zhl_ctx_impl *)ctx;
    strncpy(impl->downloaded_lib_path, FIXTURE_V1_PATH,
            sizeof(impl->downloaded_lib_path) - 1);

    TEST_ASSERT_EQ(zhl_hotload_apply(ctx), ZHL_OK);

    TEST_ASSERT(fp_add == old_add);
    TEST_ASSERT(fp_mul == old_mul);
    TEST_ASSERT(fp_sub == old_sub);

    zhl_ctx_destroy(&ctx);
}

static void test_hotload_some_changed(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_V1_PATH);

    func_ii_t fp_add = NULL;
    func_ii_t fp_mul = NULL;
    func_ii_t fp_sub = NULL;
    func_ii_t fp_div = NULL;
    zhl_func_binding_t bindings[] = {
        { "add",      (void **)&fp_add },
        { "multiply", (void **)&fp_mul },
        { "subtract", (void **)&fp_sub },
        { "divide",   (void **)&fp_div },
    };
    zhl_register_bindings(ctx, bindings, 4);

    func_ii_t old_add = fp_add;
    func_ii_t old_mul = fp_mul;
    func_ii_t old_sub = fp_sub;

    g_swap_count = 0;
    zhl_set_swap_callback(ctx, swap_cb, NULL);

    struct zhl_ctx_impl *impl = (struct zhl_ctx_impl *)ctx;
    strncpy(impl->downloaded_lib_path, FIXTURE_V2_PATH,
            sizeof(impl->downloaded_lib_path) - 1);

    zhl_status_t st = zhl_hotload_apply(ctx);
    TEST_ASSERT_EQ(st, ZHL_OK);

    TEST_ASSERT(fp_add == old_add);
    TEST_ASSERT(fp_mul != old_mul);
    TEST_ASSERT(fp_sub == old_sub);
    TEST_ASSERT(fp_div != NULL);
    TEST_ASSERT(g_swap_count >= 2);

    if (fp_mul) {
        TEST_ASSERT_EQ(fp_mul(3, 4), 24);
    }
    if (fp_div) {
        TEST_ASSERT_EQ(fp_div(10, 2), 5);
    }

    zhl_ctx_destroy(&ctx);
}

static void test_hotload_invalid_manifest(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_BAD_PATH);

    void *fp = NULL;
    zhl_func_binding_t b = { "noop", &fp };
    zhl_status_t st = zhl_register_bindings(ctx, &b, 1);
    TEST_ASSERT_EQ(st, ZHL_ERR_INVALID_MANIFEST);

    zhl_ctx_destroy(&ctx);
}

TEST_MAIN_BEGIN()
    RUN_TEST(test_hotload_null_ctx);
    RUN_TEST(test_hotload_not_configured);
    RUN_TEST(test_hotload_no_download);
    RUN_TEST(test_hotload_no_changes);
    RUN_TEST(test_hotload_some_changed);
    RUN_TEST(test_hotload_invalid_manifest);
TEST_MAIN_END()
