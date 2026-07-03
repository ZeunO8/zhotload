/**
 * @file test_version.c
 * @brief Tests for version parsing, comparison, and zhl's own version API.
 */

#include "test_harness.h"
#include <zhl/zhl.h>
#include <zhl/version.h>

static void test_zhl_version_string_not_null(void)
{
    TEST_ASSERT(zhl_version_string() != NULL);
}

static void test_zhl_version_string_value(void)
{
    TEST_ASSERT_STR_EQ(zhl_version_string(), ZHL_VERSION_STRING);
}

static void test_zhl_version_major(void)
{
    TEST_ASSERT_EQ(zhl_version_major(), ZHL_VERSION_MAJOR);
}

static void test_zhl_version_minor(void)
{
    TEST_ASSERT_EQ(zhl_version_minor(), ZHL_VERSION_MINOR);
}

static void test_zhl_version_patch(void)
{
    TEST_ASSERT_EQ(zhl_version_patch(), ZHL_VERSION_PATCH);
}

static void test_zhl_version_tweak(void)
{
    TEST_ASSERT_EQ(zhl_version_tweak(), ZHL_VERSION_TWEAK);
}

static void test_parse_valid(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("1.2.3", &v), ZHL_OK);
    TEST_ASSERT_EQ(v.major, 1u);
    TEST_ASSERT_EQ(v.minor, 2u);
    TEST_ASSERT_EQ(v.patch, 3u);
}

static void test_parse_zeros(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("0.0.0", &v), ZHL_OK);
    TEST_ASSERT_EQ(v.major, 0u);
    TEST_ASSERT_EQ(v.minor, 0u);
    TEST_ASSERT_EQ(v.patch, 0u);
}

static void test_parse_large_numbers(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("100.200.300", &v), ZHL_OK);
    TEST_ASSERT_EQ(v.major, 100u);
    TEST_ASSERT_EQ(v.minor, 200u);
    TEST_ASSERT_EQ(v.patch, 300u);
}

static void test_parse_null_str(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse(NULL, &v), ZHL_ERR_NULL_PARAM);
}

static void test_parse_null_out(void)
{
    TEST_ASSERT_EQ(zhl_version_parse("1.0.0", NULL), ZHL_ERR_NULL_PARAM);
}

static void test_parse_empty(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("", &v), ZHL_ERR_EMPTY_STRING);
}

static void test_parse_malformed_no_dots(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("123", &v), ZHL_ERR_INVALID_VERSION);
}

static void test_parse_malformed_one_dot(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("1.2", &v), ZHL_ERR_INVALID_VERSION);
}

static void test_parse_four_plane(void)
{
    /* 4-plane versions (major.minor.patch.tweak) are accepted; the tweak plane
     * is captured. Projects like GDZ/QUANTIX version this way (e.g. 1.10.0.0). */
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("1.2.3.4", &v), ZHL_OK);
    TEST_ASSERT_EQ(v.major, 1u);
    TEST_ASSERT_EQ(v.minor, 2u);
    TEST_ASSERT_EQ(v.patch, 3u);
    TEST_ASSERT_EQ(v.tweak, 4u);
}

static void test_parse_three_plane_zero_tweak(void)
{
    /* Absent tweak plane defaults to 0. */
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("1.2.3", &v), ZHL_OK);
    TEST_ASSERT_EQ(v.tweak, 0u);
}

static void test_parse_malformed_five_plane(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("1.2.3.4.5", &v), ZHL_ERR_INVALID_VERSION);
}

static void test_parse_malformed_alpha(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("a.b.c", &v), ZHL_ERR_INVALID_VERSION);
}

static void test_parse_malformed_spaces(void)
{
    zhl_version_t v;
    TEST_ASSERT_EQ(zhl_version_parse("1. 2.3", &v), ZHL_ERR_INVALID_VERSION);
}

static void test_compare_equal(void)
{
    zhl_version_t a = {1, 2, 3};
    zhl_version_t b = {1, 2, 3};
    TEST_ASSERT_EQ(zhl_version_compare(&a, &b), ZHL_CMP_EQUAL);
}

static void test_compare_major_less(void)
{
    zhl_version_t a = {0, 9, 9};
    zhl_version_t b = {1, 0, 0};
    TEST_ASSERT_EQ(zhl_version_compare(&a, &b), ZHL_CMP_LESS);
}

static void test_compare_major_greater(void)
{
    zhl_version_t a = {2, 0, 0};
    zhl_version_t b = {1, 9, 9};
    TEST_ASSERT_EQ(zhl_version_compare(&a, &b), ZHL_CMP_GREATER);
}

static void test_compare_minor_diff(void)
{
    zhl_version_t a = {1, 1, 0};
    zhl_version_t b = {1, 2, 0};
    TEST_ASSERT_EQ(zhl_version_compare(&a, &b), ZHL_CMP_LESS);
}

static void test_compare_patch_diff(void)
{
    zhl_version_t a = {1, 2, 4};
    zhl_version_t b = {1, 2, 3};
    TEST_ASSERT_EQ(zhl_version_compare(&a, &b), ZHL_CMP_GREATER);
}

static void test_compare_tweak_diff(void)
{
    /* A tweak-only bump (1.2.3.0 -> 1.2.3.1) must order as greater so a
     * tweak-only release is still seen as an available update. */
    zhl_version_t a = {1, 2, 3, 0};
    zhl_version_t b = {1, 2, 3, 1};
    TEST_ASSERT_EQ(zhl_version_compare(&a, &b), ZHL_CMP_LESS);
    TEST_ASSERT_EQ(zhl_version_compare(&b, &a), ZHL_CMP_GREATER);
}

TEST_MAIN_BEGIN()
    RUN_TEST(test_zhl_version_string_not_null);
    RUN_TEST(test_zhl_version_string_value);
    RUN_TEST(test_zhl_version_major);
    RUN_TEST(test_zhl_version_minor);
    RUN_TEST(test_zhl_version_patch);
    RUN_TEST(test_zhl_version_tweak);
    RUN_TEST(test_parse_valid);
    RUN_TEST(test_parse_zeros);
    RUN_TEST(test_parse_large_numbers);
    RUN_TEST(test_parse_null_str);
    RUN_TEST(test_parse_null_out);
    RUN_TEST(test_parse_empty);
    RUN_TEST(test_parse_malformed_no_dots);
    RUN_TEST(test_parse_malformed_one_dot);
    RUN_TEST(test_parse_four_plane);
    RUN_TEST(test_parse_three_plane_zero_tweak);
    RUN_TEST(test_parse_malformed_five_plane);
    RUN_TEST(test_parse_malformed_alpha);
    RUN_TEST(test_parse_malformed_spaces);
    RUN_TEST(test_compare_equal);
    RUN_TEST(test_compare_major_less);
    RUN_TEST(test_compare_major_greater);
    RUN_TEST(test_compare_minor_diff);
    RUN_TEST(test_compare_patch_diff);
    RUN_TEST(test_compare_tweak_diff);
TEST_MAIN_END()
