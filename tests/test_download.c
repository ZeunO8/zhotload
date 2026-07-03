/**
 * @file test_download.c
 * @brief Tests for zhl_download_update.
 */

#include "test_harness.h"
#include <zhl/zhl.h>

static void test_download_null_ctx(void)
{
    zhl_update_info_t info = {0};
    TEST_ASSERT_EQ(zhl_download_update(NULL, &info), ZHL_ERR_NULL_PARAM);
}

static void test_download_null_info(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_download_update(ctx, NULL), ZHL_ERR_NULL_PARAM);
    zhl_ctx_destroy(&ctx);
}

static void test_download_not_configured(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_update_info_t info = {0};
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_ERR_NOT_CONFIGURED);
    zhl_ctx_destroy(&ctx);
}

static void test_download_network_failure(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_staging_dir(ctx, "/tmp");

    zhl_update_info_t info;
    memset(&info, 0, sizeof(info));
    strncpy(info.version, "2.0.0", ZHL_MAX_VERSION_LEN - 1);
    strncpy(info.download_url, "http://127.0.0.1:1/bad", ZHL_MAX_URL_LEN - 1);

    zhl_status_t st = zhl_download_update(ctx, &info);
    TEST_ASSERT(st == ZHL_ERR_NETWORK || st == ZHL_ERR_HTTP_ERROR);

    zhl_ctx_destroy(&ctx);
}

TEST_MAIN_BEGIN()
    RUN_TEST(test_download_null_ctx);
    RUN_TEST(test_download_null_info);
    RUN_TEST(test_download_not_configured);
    RUN_TEST(test_download_network_failure);
TEST_MAIN_END()
