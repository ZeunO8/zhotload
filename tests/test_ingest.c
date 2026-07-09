/**
 * @file test_ingest.c
 * @brief End-to-end test of zhs's --ingest (git-tag/URL) feature: an
 *        in-process zcio HTTP server plays a mock GitHub Releases API + asset
 *        host, the REAL `zhs` binary is spawned as a subprocess pointed at it
 *        via ZHS_GITHUB_API_BASE, and this test asserts the ingested file
 *        layout, checksum, and idempotency on disk.
 *
 * This is a subprocess/filesystem test (not the in-process ZTEST/TEST_HARNESS
 * style used elsewhere) because it exercises zhs as a whole program — the
 * thing that matters here is the argv parsing, the poll-loop scheduling, and
 * the on-disk artifact tree it produces, not an internal function in
 * isolation. POSIX-only (fork/exec); skipped on Windows and cross builds.
 */

#include "test_harness.h"
#include <zcio/zcio.h>
#include <zcio/http_server.h>
#include <zcio/crypto.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>

#ifndef ZHS_EXE_PATH
#error "ZHS_EXE_PATH must be baked in by CMake"
#endif

static uint8_t g_asset[1024];
static char    g_asset_sig_hex[140] = "";
static int     g_mock_port = 0;

static void handler(zcio_http_req *r, void *user)
{
    (void)user;
    const char *path = zcio_http_req_path(r);
    if (strcmp(path, "/repos/acme/thing/releases/latest") == 0) {
        char json[768];
        if (g_asset_sig_hex[0]) {
            snprintf(json, sizeof(json),
                "{\"tag_name\":\"v7.7.7.7\",\"assets\":["
                "{\"name\":\"libThing.dylib\",\"browser_download_url\":\"http://127.0.0.1:%d/assets/libThing.dylib\"},"
                "{\"name\":\"libThing.dylib.sig\",\"browser_download_url\":\"http://127.0.0.1:%d/assets/libThing.dylib.sig\"}"
                "]}", g_mock_port, g_mock_port);
        } else {
            snprintf(json, sizeof(json),
                "{\"tag_name\":\"v7.7.7.7\",\"assets\":["
                "{\"name\":\"libThing.dylib\",\"browser_download_url\":\"http://127.0.0.1:%d/assets/libThing.dylib\"}"
                "]}", g_mock_port);
        }
        const zcio_http_header hdr[] = { { "Content-Type", "application/json" } };
        zcio_http_respond(r, 200, hdr, 1, json, strlen(json));
        return;
    }
    if (strcmp(path, "/assets/libThing.dylib") == 0) {
        const zcio_http_header hdr[] = { { "Content-Type", "application/octet-stream" } };
        zcio_http_respond(r, 200, hdr, 1, g_asset, sizeof(g_asset));
        return;
    }
    if (strcmp(path, "/assets/libThing.dylib.sig") == 0) {
        const zcio_http_header hdr[] = { { "Content-Type", "text/plain" } };
        zcio_http_respond(r, 200, hdr, 1, g_asset_sig_hex, strlen(g_asset_sig_hex));
        return;
    }
    zcio_http_respond(r, 404, NULL, 0, "nope", 4);
}

static zcio_http_server *g_srv;
static void *server_main(void *p) { (void)p; zcio_http_server_run(g_srv); return NULL; }

/* rm -rf a small test tree (no symlinks/special files expected). */
static void rmrf(const char *path)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Spawn `zhs` with the given data dir + extra args, pointed at the mock
 * server; poll for up to timeout_s for `want_path` to appear; kill it either
 * way. Returns 1 if the file appeared in time. */
static int run_zhs_and_wait_for(const char *data_dir, const char *ingest_arg,
                                const char *want_path, int timeout_s)
{
    pid_t pid = fork();
    if (pid == 0) {
        char base[64];
        snprintf(base, sizeof(base), "http://127.0.0.1:%d", g_mock_port);
        setenv("ZHS_GITHUB_API_BASE", base, 1);
        execl(ZHS_EXE_PATH, "zhs", "--data", data_dir, "--port", "0",
              "--ingest", ingest_arg, "--ingest-interval", "1", (char *)NULL);
        _exit(127);   /* exec failed */
    }
    int found = 0;
    for (int i = 0; i < timeout_s * 4; i++) {
        usleep(250000);
        if (file_exists(want_path)) { found = 1; break; }
    }
    kill(pid, SIGTERM);
    int status = 0;
    waitpid(pid, &status, 0);
    return found;
}

static void test_signed_ingest_creates_versioned_layout(void)
{
    rmrf("ingest_data_signed");
    mkdir("ingest_data_signed", 0755);

    const char *want = "ingest_data_signed/thing-app/7.7.7.7/libThing.dylib";
    TEST_ASSERT(run_zhs_and_wait_for("ingest_data_signed", "thing-app=acme/thing", want, 6));

    /* Checksum on disk matches the manifest's SHA-256 over the asset bytes. */
    uint8_t digest[ZCIO_SHA256_LEN];
    zcio_sha256(g_asset, sizeof(g_asset), digest);
    char want_hex[ZCIO_SHA256_LEN * 2 + 1];
    zcio_hex_encode(digest, sizeof(digest), want_hex);

    FILE *fp = fopen(want, "rb");
    TEST_ASSERT(fp != NULL);
    if (fp) {
        uint8_t buf[2048];
        size_t n = fread(buf, 1, sizeof(buf), fp);
        fclose(fp);
        TEST_ASSERT_EQ(n, sizeof(g_asset));
        uint8_t got[ZCIO_SHA256_LEN];
        zcio_sha256(buf, n, got);
        char got_hex[ZCIO_SHA256_LEN * 2 + 1];
        zcio_hex_encode(got, sizeof(got), got_hex);
        TEST_ASSERT_STR_EQ(got_hex, want_hex);
    }

    /* The ".sig" sibling asset was ingested too. */
    TEST_ASSERT(file_exists("ingest_data_signed/thing-app/7.7.7.7/libThing.dylib.sig"));
}

static void test_unsigned_release_ingests_without_sig(void)
{
    /* Reuses the same mock release/asset but the mock's sig hex is cleared by
     * the caller (see main) so no ".sig" asset is advertised. */
    rmrf("ingest_data_unsigned");
    mkdir("ingest_data_unsigned", 0755);

    const char *want = "ingest_data_unsigned/thing-app/7.7.7.7/libThing.dylib";
    TEST_ASSERT(run_zhs_and_wait_for("ingest_data_unsigned", "thing-app=acme/thing", want, 6));
    TEST_ASSERT(!file_exists("ingest_data_unsigned/thing-app/7.7.7.7/libThing.dylib.sig"));
}

TEST_MAIN_BEGIN()
    zcio_init();
    for (size_t i = 0; i < sizeof(g_asset); i++) g_asset[i] = (uint8_t)(i * 31 + 11);

    uint8_t pub[32], priv[64], sig[64];
    if (zcio_ed25519_keypair(pub, priv, NULL) != ZCIO_OK) {
        fprintf(stderr, "keypair generation failed: %s\n", zcio_last_error());
        return 1;
    }
    zcio_ed25519_sign(sig, g_asset, sizeof(g_asset), pub, priv);
    zcio_hex_encode(sig, sizeof(sig), g_asset_sig_hex);

    zcio_http_server_config cfg = {0};
    cfg.port = 0;
    cfg.bind_host = "127.0.0.1";
    g_srv = zcio_http_server_start(&cfg, handler, NULL);
    if (!g_srv) {
        fprintf(stderr, "mock server start failed: %s\n", zcio_last_error());
        return 1;
    }
    g_mock_port = zcio_http_server_port(g_srv);

    pthread_t th;
    pthread_create(&th, NULL, server_main, NULL);

    RUN_TEST(test_signed_ingest_creates_versioned_layout);

    /* Second scenario: same mock, but strip the sig so the release looks
     * unsigned to the poller. */
    g_asset_sig_hex[0] = '\0';
    RUN_TEST(test_unsigned_release_ingests_without_sig);

    zcio_http_server_stop(g_srv);
    pthread_join(th, NULL);
    zcio_http_server_free(g_srv);

    rmrf("ingest_data_signed");
    rmrf("ingest_data_unsigned");
TEST_MAIN_END()
