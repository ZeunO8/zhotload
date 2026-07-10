/**
 * @file test_platform_serving.c
 * @brief End-to-end test of zhs's ?platform= qualified serving: a REAL zhs
 *        subprocess is pointed at a hand-populated data tree carrying both a
 *        flat (legacy) artifact and a platform-qualified one, and this test
 *        asserts /latest and /download resolve the right one -- an exact
 *        platform match wins, and platform absent/unmatched falls back to
 *        the flat layout untouched, so pre-platform-aware clients and
 *        existing publishers (hand-dropped, remote --ingest) keep working.
 *
 * Subprocess test (mirrors test_ingest.c / test_local_watch.c): this
 * exercises the real HTTP routing zhs serves, not an internal function
 * (handle_latest/handle_download are file-local statics). POSIX-only
 * (fork/exec); skipped on Windows and cross builds.
 */

#include "test_harness.h"
#include <zcio/zcio.h>
#include <zcio/http.h>
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

#ifndef ZHS_EXE_PATH
#error "ZHS_EXE_PATH must be baked in by CMake"
#endif

/* Fixed test port: this test talks real HTTP to the subprocess, unlike
 * test_ingest.c/test_local_watch.c which only ever inspect zhs's on-disk
 * output, so (unlike them) it cannot pass "--port 0" -- there'd be no way
 * for this process to learn which port a *different* process bound. */
#define TEST_PORT 28173
#define TEST_PORT_STR_(x) #x
#define TEST_PORT_STR(x) TEST_PORT_STR_(x)

static const char *FLAT_BYTES     = "flat-legacy-bytes";
static const char *QUALIFIED_BYTES = "android-qualified-bytes";

static void rmrf(const char *path)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void write_file(const char *path, const char *contents)
{
    FILE *fp = fopen(path, "wb");
    fwrite(contents, 1, strlen(contents), fp);
    fclose(fp);
}

static pid_t g_zhs_pid = -1;

static void spawn_zhs(const char *data_dir)
{
    pid_t pid = fork();
    if (pid == 0) {
        char port_str[16];
        snprintf(port_str, sizeof(port_str), "%d", TEST_PORT);
        execl(ZHS_EXE_PATH, "zhs", "--data", data_dir, "--port", port_str,
              "--host", "127.0.0.1", (char *)NULL);
        _exit(127);
    }
    g_zhs_pid = pid;
}

static void kill_zhs(void)
{
    if (g_zhs_pid <= 0) return;
    kill(g_zhs_pid, SIGTERM);
    int status = 0;
    waitpid(g_zhs_pid, &status, 0);
    g_zhs_pid = -1;
}

/* Poll /apps/{app}/latest with no query until it answers 200 (server up). */
static int wait_for_server_up(int timeout_s)
{
    for (int i = 0; i < timeout_s * 10; i++) {
        usleep(100000);
        zcio_http_opts opts = { .timeout_ms = 500 };
        zcio_http_response r = zcio_http_request_opts(
            "GET", "http://127.0.0.1:" TEST_PORT_STR(TEST_PORT) "/apps/probe/latest",
            NULL, 0, NULL, 0, &opts);
        int up = (r.status > 0);
        zcio_http_response_free(&r);
        if (up) return 1;
    }
    return 0;
}

static int get_json(const char *path, cJSON **out_json, char *body_out, size_t body_cap)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", TEST_PORT, path);
    zcio_http_opts opts = { .timeout_ms = 3000 };
    zcio_http_response r = zcio_http_request_opts("GET", url, NULL, 0, NULL, 0, &opts);
    int status = r.status;
    if (r.body && body_out) {
        size_t n = r.body_size < body_cap - 1 ? r.body_size : body_cap - 1;
        memcpy(body_out, r.body, n);
        body_out[n] = '\0';
    }
    *out_json = r.body ? cJSON_ParseWithLength(r.body, r.body_size) : NULL;
    zcio_http_response_free(&r);
    return status;
}

static int get_raw(const char *path, char *body_out, size_t body_cap)
{
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", TEST_PORT, path);
    zcio_http_opts opts = { .timeout_ms = 3000 };
    zcio_http_response r = zcio_http_request_opts("GET", url, NULL, 0, NULL, 0, &opts);
    int status = r.status;
    if (r.body) {
        size_t n = r.body_size < body_cap - 1 ? r.body_size : body_cap - 1;
        memcpy(body_out, r.body, n);
        body_out[n] = '\0';
    } else if (body_cap) {
        body_out[0] = '\0';
    }
    zcio_http_response_free(&r);
    return status;
}

static void test_no_platform_param_serves_flat(void)
{
    cJSON *json = NULL;
    char body[1024];
    int status = get_json("/apps/qualapp/latest", &json, body, sizeof(body));
    TEST_ASSERT_EQ(status, 200);
    TEST_ASSERT(json != NULL);
    if (json) {
        cJSON *dl = cJSON_GetObjectItemCaseSensitive(json, "download_url");
        TEST_ASSERT(cJSON_IsString(dl));
        if (cJSON_IsString(dl)) TEST_ASSERT(strstr(dl->valuestring, "platform=") == NULL);
        cJSON_Delete(json);
    }

    char downloaded[256] = "";
    status = get_raw("/apps/qualapp/download/1.0.0.0", downloaded, sizeof(downloaded));
    TEST_ASSERT_EQ(status, 200);
    TEST_ASSERT_STR_EQ(downloaded, FLAT_BYTES);
}

static void test_matching_platform_param_serves_qualified(void)
{
    cJSON *json = NULL;
    char body[1024];
    int status = get_json("/apps/qualapp/latest?platform=android", &json, body, sizeof(body));
    TEST_ASSERT_EQ(status, 200);
    TEST_ASSERT(json != NULL);
    if (json) {
        cJSON *dl = cJSON_GetObjectItemCaseSensitive(json, "download_url");
        TEST_ASSERT(cJSON_IsString(dl));
        if (cJSON_IsString(dl)) {
            TEST_ASSERT(strstr(dl->valuestring, "platform=android") != NULL);
            char path[256];
            snprintf(path, sizeof(path), "%s", dl->valuestring);
            char downloaded[256] = "";
            int dstatus = get_raw(path, downloaded, sizeof(downloaded));
            TEST_ASSERT_EQ(dstatus, 200);
            TEST_ASSERT_STR_EQ(downloaded, QUALIFIED_BYTES);
        }
        cJSON_Delete(json);
    }
}

static void test_unmatched_platform_param_falls_back_to_flat(void)
{
    char downloaded[256] = "";
    int status = get_raw("/apps/qualapp/download/1.0.0.0?platform=ios", downloaded, sizeof(downloaded));
    TEST_ASSERT_EQ(status, 200);
    TEST_ASSERT_STR_EQ(downloaded, FLAT_BYTES);
}

TEST_MAIN_BEGIN()
    zcio_init();

    rmrf("ps_data");
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p ps_data/qualapp/1.0.0.0/android");
    (void)system(cmd);
    write_file("ps_data/qualapp/1.0.0.0/lib.dylib", FLAT_BYTES);
    write_file("ps_data/qualapp/1.0.0.0/android/lib.dylib", QUALIFIED_BYTES);

    spawn_zhs("ps_data");
    if (!wait_for_server_up(10)) {
        fprintf(stderr, "zhs did not come up on port %d\n", TEST_PORT);
        kill_zhs();
        rmrf("ps_data");
        return 1;
    }

    RUN_TEST(test_no_platform_param_serves_flat);
    RUN_TEST(test_matching_platform_param_serves_qualified);
    RUN_TEST(test_unmatched_platform_param_falls_back_to_flat);

    kill_zhs();
    rmrf("ps_data");
TEST_MAIN_END()
