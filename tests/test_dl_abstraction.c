/**
 * @file test_dl_abstraction.c
 * @brief Tests for the platform dynamic-loading abstraction layer.
 */

#include "test_harness.h"
#include <zhl/zhl.h>

#ifndef FIXTURE_V1_PATH
#define FIXTURE_V1_PATH "libfixture_v1.so"
#endif

static void test_dl_open_null_path(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, "/nonexistent/path.so");
    void *fp = NULL;
    zhl_func_binding_t b = { "x", &fp };
    TEST_ASSERT_EQ(zhl_register_bindings(ctx, &b, 1), ZHL_ERR_DL_OPEN);
    zhl_ctx_destroy(&ctx);
}

static void test_dl_open_missing_library(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, "/this/does/not/exist.so");
    void *fp = NULL;
    zhl_func_binding_t b = { "x", &fp };
    zhl_status_t st = zhl_register_bindings(ctx, &b, 1);
    TEST_ASSERT_EQ(st, ZHL_ERR_DL_OPEN);
    zhl_ctx_destroy(&ctx);
}

static void test_dl_sym_missing_symbol(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_V1_PATH);

    void *fp = NULL;
    zhl_func_binding_t b = { "nonexistent_function_xyz", &fp };
    zhl_status_t st = zhl_register_bindings(ctx, &b, 1);
    TEST_ASSERT_EQ(st, ZHL_OK);
    TEST_ASSERT(fp == NULL);

    zhl_ctx_destroy(&ctx);
}

static void test_dl_open_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_V1_PATH);

    void *fp = NULL;
    zhl_func_binding_t b = { "add", &fp };
    TEST_ASSERT_EQ(zhl_register_bindings(ctx, &b, 1), ZHL_OK);
    TEST_ASSERT(fp != NULL);

    zhl_ctx_destroy(&ctx);
}

TEST_MAIN_BEGIN()
    RUN_TEST(test_dl_open_null_path);
    RUN_TEST(test_dl_open_missing_library);
    RUN_TEST(test_dl_sym_missing_symbol);
    RUN_TEST(test_dl_open_valid);
TEST_MAIN_END()
