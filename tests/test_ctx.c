/**
 * @file test_ctx.c
 * @brief Tests for context creation, configuration, and registration.
 */

#include "test_harness.h"
#include <zhl/zhl.h>

#ifndef FIXTURE_V1_PATH
#define FIXTURE_V1_PATH "libfixture_v1.so"
#endif

static void test_create_null(void)
{
    TEST_ASSERT_EQ(zhl_ctx_create(NULL), ZHL_ERR_NULL_PARAM);
}

static void test_create_destroy(void)
{
    zhl_ctx_t ctx = NULL;
    TEST_ASSERT_EQ(zhl_ctx_create(&ctx), ZHL_OK);
    TEST_ASSERT(ctx != NULL);
    zhl_ctx_destroy(&ctx);
    TEST_ASSERT(ctx == NULL);
}

static void test_destroy_null(void)
{
    zhl_ctx_destroy(NULL);
}

static void test_destroy_null_handle(void)
{
    zhl_ctx_t ctx = NULL;
    zhl_ctx_destroy(&ctx);
}

static void test_set_server_url_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(NULL, "http://x"), ZHL_ERR_NULL_PARAM);
}

static void test_set_server_url_null_url(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_set_server_url_empty(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(ctx, ""), ZHL_ERR_EMPTY_STRING);
    zhl_ctx_destroy(&ctx);
}

static void test_set_server_url_invalid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(ctx, "ftp://bad"), ZHL_ERR_INVALID_URL);
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(ctx, "not-a-url"), ZHL_ERR_INVALID_URL);
    zhl_ctx_destroy(&ctx);
}

static void test_set_server_url_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(ctx, "http://localhost:8080"), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_set_server_url_strips_trailing_slash(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_server_url(ctx, "http://localhost:8080/"), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_set_app_name_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_ctx_set_app_name(NULL, "app"), ZHL_ERR_NULL_PARAM);
}

static void test_set_app_name_null_name(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_app_name(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_set_app_name_empty(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_app_name(ctx, ""), ZHL_ERR_EMPTY_STRING);
    zhl_ctx_destroy(&ctx);
}

static void test_set_app_name_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_app_name(ctx, "myapp"), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_set_current_version_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_ctx_set_current_version(NULL, "1.0.0"), ZHL_ERR_NULL_PARAM);
}

static void test_set_current_version_null_ver(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_version(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_set_current_version_empty(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_version(ctx, ""), ZHL_ERR_EMPTY_STRING);
    zhl_ctx_destroy(&ctx);
}

static void test_set_current_version_invalid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_version(ctx, "bad"), ZHL_ERR_INVALID_VERSION);
    zhl_ctx_destroy(&ctx);
}

static void test_set_current_version_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_version(ctx, "1.0.0"), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_set_poll_interval_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_ctx_set_poll_interval(NULL, 10), ZHL_ERR_NULL_PARAM);
}

static void test_set_poll_interval_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_poll_interval(ctx, 30), ZHL_OK);
    TEST_ASSERT_EQ(zhl_ctx_set_poll_interval(ctx, 0), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_set_lib_path_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_ctx_set_current_lib_path(NULL, "/a.so"), ZHL_ERR_NULL_PARAM);
}

static void test_set_lib_path_null_path(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_lib_path(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_set_lib_path_empty(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_lib_path(ctx, ""), ZHL_ERR_EMPTY_STRING);
    zhl_ctx_destroy(&ctx);
}

static void test_set_lib_path_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_current_lib_path(ctx, "/tmp/lib.so"), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_set_staging_dir_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_ctx_set_staging_dir(NULL, "/tmp"), ZHL_ERR_NULL_PARAM);
}

static void test_set_staging_dir_empty(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_staging_dir(ctx, ""), ZHL_ERR_EMPTY_STRING);
    zhl_ctx_destroy(&ctx);
}

static void test_set_staging_dir_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_staging_dir(ctx, "/tmp/staging"), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_register_bindings_null_ctx(void)
{
    zhl_func_binding_t b = { "x", NULL };
    TEST_ASSERT_EQ(zhl_register_bindings(NULL, &b, 1), ZHL_ERR_NULL_PARAM);
}

static void test_register_bindings_null_bindings(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_register_bindings(ctx, NULL, 1), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_register_bindings_not_configured(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_func_binding_t b = { "x", NULL };
    TEST_ASSERT_EQ(zhl_register_bindings(ctx, &b, 1), ZHL_ERR_NOT_CONFIGURED);
    zhl_ctx_destroy(&ctx);
}

static void test_register_bindings_bad_lib_path(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, "/nonexistent/lib.so");
    zhl_func_binding_t b = { "x", NULL };
    TEST_ASSERT_EQ(zhl_register_bindings(ctx, &b, 1), ZHL_ERR_DL_OPEN);
    zhl_ctx_destroy(&ctx);
}

static void test_register_bindings_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_lib_path(ctx, FIXTURE_V1_PATH);

    void *fp_add = NULL;
    void *fp_mul = NULL;
    void *fp_sub = NULL;
    zhl_func_binding_t bindings[] = {
        { "add",      &fp_add },
        { "multiply", &fp_mul },
        { "subtract", &fp_sub },
    };

    zhl_status_t st = zhl_register_bindings(ctx, bindings, 3);
    TEST_ASSERT_EQ(st, ZHL_OK);
    TEST_ASSERT(fp_add != NULL);
    TEST_ASSERT(fp_mul != NULL);
    TEST_ASSERT(fp_sub != NULL);

    zhl_ctx_destroy(&ctx);
}

static void test_get_active_version_null_ctx(void)
{
    const char *v = NULL;
    TEST_ASSERT_EQ(zhl_ctx_get_active_version(NULL, &v), ZHL_ERR_NULL_PARAM);
}

static void test_get_active_version_null_out(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_get_active_version(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_get_active_version_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_current_version(ctx, "2.0.0");

    const char *v = NULL;
    TEST_ASSERT_EQ(zhl_ctx_get_active_version(ctx, &v), ZHL_OK);
    TEST_ASSERT(v != NULL);
    TEST_ASSERT_STR_EQ(v, "2.0.0");

    zhl_ctx_destroy(&ctx);
}

static void test_set_swap_callback_null_ctx(void)
{
    TEST_ASSERT_EQ(zhl_set_swap_callback(NULL, NULL, NULL), ZHL_ERR_NULL_PARAM);
}

static void test_set_swap_callback_valid(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_set_swap_callback(ctx, NULL, NULL), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

TEST_MAIN_BEGIN()
    RUN_TEST(test_create_null);
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_destroy_null);
    RUN_TEST(test_destroy_null_handle);
    RUN_TEST(test_set_server_url_null_ctx);
    RUN_TEST(test_set_server_url_null_url);
    RUN_TEST(test_set_server_url_empty);
    RUN_TEST(test_set_server_url_invalid);
    RUN_TEST(test_set_server_url_valid);
    RUN_TEST(test_set_server_url_strips_trailing_slash);
    RUN_TEST(test_set_app_name_null_ctx);
    RUN_TEST(test_set_app_name_null_name);
    RUN_TEST(test_set_app_name_empty);
    RUN_TEST(test_set_app_name_valid);
    RUN_TEST(test_set_current_version_null_ctx);
    RUN_TEST(test_set_current_version_null_ver);
    RUN_TEST(test_set_current_version_empty);
    RUN_TEST(test_set_current_version_invalid);
    RUN_TEST(test_set_current_version_valid);
    RUN_TEST(test_set_poll_interval_null_ctx);
    RUN_TEST(test_set_poll_interval_valid);
    RUN_TEST(test_set_lib_path_null_ctx);
    RUN_TEST(test_set_lib_path_null_path);
    RUN_TEST(test_set_lib_path_empty);
    RUN_TEST(test_set_lib_path_valid);
    RUN_TEST(test_set_staging_dir_null_ctx);
    RUN_TEST(test_set_staging_dir_empty);
    RUN_TEST(test_set_staging_dir_valid);
    RUN_TEST(test_register_bindings_null_ctx);
    RUN_TEST(test_register_bindings_null_bindings);
    RUN_TEST(test_register_bindings_not_configured);
    RUN_TEST(test_register_bindings_bad_lib_path);
    RUN_TEST(test_register_bindings_valid);
    RUN_TEST(test_get_active_version_null_ctx);
    RUN_TEST(test_get_active_version_null_out);
    RUN_TEST(test_get_active_version_valid);
    RUN_TEST(test_set_swap_callback_null_ctx);
    RUN_TEST(test_set_swap_callback_valid);
TEST_MAIN_END()
