/**
 * @file test_signed_update.c
 * @brief End-to-end artifact signing: an in-process zcio HTTP server plays a
 *        zhs-style manifest (checksum + Ed25519 signature) and zhl must
 *        accept only artifacts that verify against the pinned trusted key.
 */

#include "test_harness.h"
#include <zhl/zhl.h>
#include <zcio/zcio.h>
#include <zcio/crypto.h>
#include <zcio/http_server.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <direct.h>
#else
#  include <pthread.h>
#  include <sys/stat.h>
#endif

/* ------------------------------------------------------------------ */
/*  Mock zhs                                                           */
/* ------------------------------------------------------------------ */

static uint8_t g_payload[1536];       /* the "artifact" bytes             */
static char    g_checksum_hex[65];    /* manifest checksum                */
static char    g_signature_hex[130];  /* manifest signature ("" = none)   */
static int     g_tamper_download;     /* serve corrupted bytes            */

static void handler(zcio_http_req *r, void *user)
{
    (void)user;
    const char *path = zcio_http_req_path(r);
    if (strcmp(path, "/apps/demo/latest") == 0) {
        char json[512];
        snprintf(json, sizeof(json),
                 "{\"version\":\"9.9.9\","
                 "\"download_url\":\"/apps/demo/download/9.9.9\","
                 "\"checksum\":\"%s\",\"signature\":\"%s\"}",
                 g_checksum_hex, g_signature_hex);
        const zcio_http_header hdr[] = { { "Content-Type", "application/json" } };
        zcio_http_respond(r, 200, hdr, 1, json, strlen(json));
        return;
    }
    if (strcmp(path, "/apps/demo/download/9.9.9") == 0) {
        uint8_t buf[sizeof(g_payload)];
        memcpy(buf, g_payload, sizeof(g_payload));
        if (g_tamper_download) buf[7] ^= 0x55;
        const zcio_http_header hdr[] = { { "Content-Type", "application/octet-stream" } };
        zcio_http_respond(r, 200, hdr, 1, buf, sizeof(buf));
        return;
    }
    zcio_http_respond(r, 404, NULL, 0, "nope", 4);
}

static zcio_http_server *g_srv;

#if defined(_WIN32)
static DWORD WINAPI server_main(LPVOID p)
{
    (void)p;
    zcio_http_server_run(g_srv);
    return 0;
}
#else
static void *server_main(void *p)
{
    (void)p;
    zcio_http_server_run(g_srv);
    return NULL;
}
#endif

/* ------------------------------------------------------------------ */
/*  Shared state                                                       */
/* ------------------------------------------------------------------ */

static uint8_t g_pub[32], g_priv[64];
static char    g_pub_hex[65];
static char    g_server_url[64];

static zhl_ctx_t make_ctx(const char *trusted_key_hex)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    zhl_ctx_set_server_url(ctx, g_server_url);
    zhl_ctx_set_app_name(ctx, "demo");
    zhl_ctx_set_current_version(ctx, "1.0.0");
    zhl_ctx_set_staging_dir(ctx, "signed_staging");
    if (trusted_key_hex) zhl_ctx_set_trusted_key(ctx, trusted_key_hex);
    return ctx;
}

/* Manifest with a correct checksum and a signature over the real payload. */
static void arm_good_manifest(void)
{
    uint8_t digest[ZCIO_SHA256_LEN], sig[ZCIO_ED25519_SIG_LEN];
    zcio_sha256(g_payload, sizeof(g_payload), digest);
    zcio_hex_encode(digest, sizeof(digest), g_checksum_hex);
    zcio_ed25519_sign(sig, g_payload, sizeof(g_payload), g_pub, g_priv);
    zcio_hex_encode(sig, sizeof(sig), g_signature_hex);
    g_tamper_download = 0;
}

/* ------------------------------------------------------------------ */
/*  Tests                                                              */
/* ------------------------------------------------------------------ */

static void test_trusted_key_validation(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);
    TEST_ASSERT_EQ(zhl_ctx_set_trusted_key(ctx, "zz"), ZHL_ERR_INVALID_KEY);
    TEST_ASSERT_EQ(zhl_ctx_set_trusted_key(ctx, "abcd"), ZHL_ERR_INVALID_KEY);
    TEST_ASSERT_EQ(zhl_ctx_set_trusted_key(ctx, g_pub_hex), ZHL_OK);
    TEST_ASSERT_EQ(zhl_ctx_set_trusted_key(ctx, NULL), ZHL_OK); /* clear */
    zhl_ctx_destroy(&ctx);
}

static void test_manifest_carries_signature(void)
{
    arm_good_manifest();
    zhl_ctx_t ctx = make_ctx(NULL);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_UPDATE_AVAILABLE);
    TEST_ASSERT(strcmp(info.checksum, g_checksum_hex) == 0);
    TEST_ASSERT(strcmp(info.signature, g_signature_hex) == 0);
    zhl_ctx_destroy(&ctx);
}

static void test_signed_download_ok(void)
{
    arm_good_manifest();
    zhl_ctx_t ctx = make_ctx(g_pub_hex);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_UPDATE_AVAILABLE);
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_OK);
    zhl_ctx_destroy(&ctx);
}

static void test_tampered_artifact_rejected(void)
{
    arm_good_manifest();
    g_tamper_download = 1;   /* served bytes != signed bytes */
    zhl_ctx_t ctx = make_ctx(g_pub_hex);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_UPDATE_AVAILABLE);
    /* Checksum catches it first (both cover the artifact bytes). */
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_ERR_CHECKSUM_MISMATCH);
    /* The staged file must be gone. */
    FILE *fp = fopen("signed_staging/lib_9.9.9.so", "rb");
    TEST_ASSERT(fp == NULL);
    if (fp) fclose(fp);
    zhl_ctx_destroy(&ctx);
}

static void test_bad_signature_rejected(void)
{
    arm_good_manifest();
    /* Signature over DIFFERENT bytes: checksum passes, signature must fail. */
    uint8_t other[16] = "not the artifact";
    uint8_t sig[ZCIO_ED25519_SIG_LEN];
    zcio_ed25519_sign(sig, other, sizeof(other), g_pub, g_priv);
    zcio_hex_encode(sig, sizeof(sig), g_signature_hex);

    zhl_ctx_t ctx = make_ctx(g_pub_hex);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_UPDATE_AVAILABLE);
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_ERR_SIGNATURE_INVALID);
    zhl_ctx_destroy(&ctx);
}

static void test_unsigned_rejected_when_key_pinned(void)
{
    arm_good_manifest();
    g_signature_hex[0] = '\0';
    zhl_ctx_t ctx = make_ctx(g_pub_hex);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_UPDATE_AVAILABLE);
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_ERR_UNSIGNED);
    zhl_ctx_destroy(&ctx);
}

static void test_legacy_no_key_still_checksums(void)
{
    arm_good_manifest();
    g_signature_hex[0] = '\0';   /* unsigned is fine without a pinned key */
    zhl_ctx_t ctx = make_ctx(NULL);
    zhl_update_info_t info;
    TEST_ASSERT_EQ(zhl_check_for_update(ctx, &info), ZHL_UPDATE_AVAILABLE);
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_OK);

    /* ...but a wrong checksum still fails. */
    g_tamper_download = 1;
    TEST_ASSERT_EQ(zhl_download_update(ctx, &info), ZHL_ERR_CHECKSUM_MISMATCH);
    zhl_ctx_destroy(&ctx);
}

/* ------------------------------------------------------------------ */

TEST_MAIN_BEGIN()
    zcio_init();

    /* Deterministic-ish payload + fresh signing key. */
    for (size_t i = 0; i < sizeof(g_payload); i++)
        g_payload[i] = (uint8_t)(i * 7 + 3);
    if (zcio_ed25519_keypair(g_pub, g_priv, NULL) != ZCIO_OK) {
        fprintf(stderr, "keypair generation failed: %s\n", zcio_last_error());
        return 1;
    }
    zcio_hex_encode(g_pub, sizeof(g_pub), g_pub_hex);

#if defined(_WIN32)
    _mkdir("signed_staging");
#else
    mkdir("signed_staging", 0755);
#endif

    zcio_http_server_config cfg = {0};
    cfg.port = 0;
    cfg.bind_host = "127.0.0.1";
    cfg.drain_timeout_ms = 500;
    g_srv = zcio_http_server_start(&cfg, handler, NULL);
    if (!g_srv) {
        fprintf(stderr, "mock server start failed: %s\n", zcio_last_error());
        return 1;
    }
    snprintf(g_server_url, sizeof(g_server_url), "http://127.0.0.1:%d",
             zcio_http_server_port(g_srv));

#if defined(_WIN32)
    HANDLE th = CreateThread(NULL, 0, server_main, NULL, 0, NULL);
#else
    pthread_t th;
    pthread_create(&th, NULL, server_main, NULL);
#endif

    RUN_TEST(test_trusted_key_validation);
    RUN_TEST(test_manifest_carries_signature);
    RUN_TEST(test_signed_download_ok);
    RUN_TEST(test_tampered_artifact_rejected);
    RUN_TEST(test_bad_signature_rejected);
    RUN_TEST(test_unsigned_rejected_when_key_pinned);
    RUN_TEST(test_legacy_no_key_still_checksums);

    zcio_http_server_stop(g_srv);
#if defined(_WIN32)
    WaitForSingleObject(th, 5000);
#else
    pthread_join(th, NULL);
#endif
    zcio_http_server_free(g_srv);
TEST_MAIN_END()
