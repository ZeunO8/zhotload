/**
 * @file test_update_check.c
 * @brief Tests for zhl_check_for_update (mocked via direct status checks).
 */

#include "test_harness.h"
#include <zhl/zhl.h>

static void test_check_null_ctx(void)
{
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(NULL, &info), ZHL_ERR_NULL_PARAM);
}

static void test_check_null_out(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_check_not_configured(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_ERR_NOT_CONFIGURED);
    zhl_ctx_destroy(&ctx);
}

static void test_check_partial_config(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_server_url(ctx, "http://localhost:9999");
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_ERR_NOT_CONFIGURED);
    zhl_ctx_destroy(&ctx);
}

static void test_check_network_failure(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_server_url(ctx, "http://127.0.0.1:1");
    zhl_ctx_set_app_name(ctx, "testapp");
    zhl_ctx_set_current_version(ctx, "1.0.0");

    zhl_update_info_t info;
    zhl_status_t st = zhl_check_for_update(ctx, &info);
    TEST_ASSERT(st == ZHL_ERR_NETWORK || st == ZHL_ERR_HTTP_ERROR);

    zhl_ctx_destroy(&ctx);
}

TEST_MAIN_BEGIN()
    RUN_TEST(test_check_null_ctx);
    RUN_TEST(test_check_null_out);
    RUN_TEST(test_check_not_configured);
    RUN_TEST(test_check_partial_config);
    RUN_TEST(test_check_network_failure);
TEST_MAIN_END()
