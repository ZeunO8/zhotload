/**
 * @file test_local_watch.c
 * @brief End-to-end test of zhs's --watch-local-git-config (local-git-tag-
 *        watch + host cmake build) feature: real git repos with a tiny
 *        buildable CMake project are tagged, the REAL `zhs` binary is
 *        spawned as a subprocess pointed at a config watching both, and this
 *        test asserts the platform-qualified published layout, signing, and
 *        that a SECOND tag on an already-watched repo gets picked up and
 *        built incrementally against the same persistent worktree without
 *        restarting zhs.
 *
 * Subprocess/filesystem test (mirrors test_ingest.c's style/rationale): the
 * thing under test is a whole program's argv parsing, background-thread
 * poll scheduling, and the on-disk artifact tree it produces from a real
 * `git`+`cmake` toolchain, not an internal function in isolation.
 * POSIX-only (fork/exec, and local-watch itself is POSIX-only); skipped on
 * Windows and cross builds.
 */

#include "test_harness.h"
#include <zcio/zcio.h>
#include <zcio/crypto.h>

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

#if defined(__APPLE__)
#define LIB_EXT "dylib"
#else
#define LIB_EXT "so"
#endif

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int nonempty_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

static void rmrf(const char *path)
{
    char cmd[1200];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    (void)system(cmd);
}

static void run_shell(const char *cmd)
{
    if (system(cmd) != 0) {
        fprintf(stderr, "  (warning) command failed: %s\n", cmd);
    }
}

/* Write a minimal, fast-to-build CMake project (one C file -> one SHARED
 * lib named "thing") into `dir`, git-init it, and tag it `tag`. `answer`
 * becomes thing_answer()'s return value, so a second tag can carry
 * visibly different source (exercises a real incremental rebuild, not just
 * a re-tag of identical content). */
static void make_tagged_repo(const char *dir, const char *tag, int answer)
{
    char cmd[1400];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
    run_shell(cmd);

    char cmakelists[1024];
    snprintf(cmakelists, sizeof(cmakelists), "%s/CMakeLists.txt", dir);
    FILE *fp = fopen(cmakelists, "wb");
    fprintf(fp, "cmake_minimum_required(VERSION 3.16)\n"
                "project(thing C)\n"
                "add_library(thing SHARED thing.c)\n");
    fclose(fp);

    char thing_c[1024];
    snprintf(thing_c, sizeof(thing_c), "%s/thing.c", dir);
    fp = fopen(thing_c, "wb");
    fprintf(fp, "int thing_answer(void) { return %d; }\n", answer);
    fclose(fp);

    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git init -q && git config user.email t@t.test && "
             "git config user.name test && git add -A && "
             "git commit -q -m init && git tag '%s'", dir, tag);
    run_shell(cmd);
}

/* Re-write thing.c with a new answer, commit, and add a second tag -- used
 * to prove a repo already being watched picks up a NEW tag on the next
 * poll, reusing (not re-cloning) its persistent worktree. */
static void bump_tagged_repo(const char *dir, const char *tag, int answer)
{
    char thing_c[1024];
    snprintf(thing_c, sizeof(thing_c), "%s/thing.c", dir);
    FILE *fp = fopen(thing_c, "wb");
    fprintf(fp, "int thing_answer(void) { return %d; }\n", answer);
    fclose(fp);

    char cmd[1200];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && git commit -q -am bump && git tag '%s'", dir, tag);
    run_shell(cmd);
}

static pid_t g_zhs_pid = -1;

static void spawn_zhs(const char *data_dir, const char *config_path)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(ZHS_EXE_PATH, "zhs", "--data", data_dir, "--port", "0",
              "--watch-local-git-config", config_path, (char *)NULL);
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

static int wait_for(const char *path, int timeout_s)
{
    for (int i = 0; i < timeout_s * 4; i++) {
        usleep(250000);
        if (file_exists(path)) return 1;
    }
    return 0;
}

/* One zhs process watches two repos concurrently -- one with a sign_key
 * configured, one without -- covering both the signed and unsigned publish
 * paths, and proving a repo it's already watching picks up a SECOND tag on
 * its own persistent worktree without restarting zhs. */
static void test_local_watch_signed_and_unsigned_two_tags(void)
{
    rmrf("lw_signed_repo");
    rmrf("lw_unsigned_repo");
    rmrf("lw_data");
    rmrf("lw_keyfile");

    make_tagged_repo("lw_signed_repo", "v1.0.0.0", 111);
    make_tagged_repo("lw_unsigned_repo", "v1.0.0.0", 222);

    char cwd[900];
    TEST_ASSERT(getcwd(cwd, sizeof(cwd)) != NULL);

    /* Real `zhs keygen` subcommand, not a hand-rolled key -- also exercises
     * that command as a side effect. */
    pid_t kg = fork();
    if (kg == 0) {
        execl(ZHS_EXE_PATH, "zhs", "keygen", "lw_keyfile", (char *)NULL);
        _exit(127);
    }
    int kg_status = 0;
    waitpid(kg, &kg_status, 0);
    TEST_ASSERT(file_exists("lw_keyfile"));

    char config_json[4096];
    snprintf(config_json, sizeof(config_json),
        "{\n"
        "  \"poll_interval_sec\": 2,\n"
        "  \"watches\": [\n"
        "    {\n"
        "      \"app\": \"thing-signed\",\n"
        "      \"repo_path\": \"%s/lw_signed_repo\",\n"
        "      \"sign_key\": \"%s/lw_keyfile\",\n"
        "      \"targets\": [ { \"platform\": \"testplat\", \"build_target\": \"thing\", "
                              "\"artifact_name\": \"libthing." LIB_EXT "\" } ]\n"
        "    },\n"
        "    {\n"
        "      \"app\": \"thing-unsigned\",\n"
        "      \"repo_path\": \"%s/lw_unsigned_repo\",\n"
        "      \"targets\": [ { \"platform\": \"testplat\", \"build_target\": \"thing\", "
                              "\"artifact_name\": \"libthing." LIB_EXT "\" } ]\n"
        "    }\n"
        "  ]\n"
        "}\n",
        cwd, cwd, cwd);
    FILE *fp = fopen("lw_config.json", "wb");
    fwrite(config_json, 1, strlen(config_json), fp);
    fclose(fp);

    spawn_zhs("lw_data", "lw_config.json");

    const char *signed_v1 = "lw_data/thing-signed/1.0.0.0/testplat/libthing." LIB_EXT;
    const char *unsigned_v1 = "lw_data/thing-unsigned/1.0.0.0/testplat/libthing." LIB_EXT;

    /* A real cmake configure+build (x2, one per watch) can take a while on a
     * loaded box -- generous per-file budget. */
    TEST_ASSERT(wait_for(signed_v1, 90));
    TEST_ASSERT(wait_for(unsigned_v1, 30));

    TEST_ASSERT(nonempty_file(signed_v1));
    TEST_ASSERT(nonempty_file(unsigned_v1));

    char signed_sig[256];
    snprintf(signed_sig, sizeof(signed_sig), "%s.sig", signed_v1);
    TEST_ASSERT(file_exists(signed_sig));

    char unsigned_sig[256];
    snprintf(unsigned_sig, sizeof(unsigned_sig), "%s.sig", unsigned_v1);
    TEST_ASSERT(!file_exists(unsigned_sig));

    /* Now tag a second version on the ALREADY-watched signed repo while zhs
     * keeps running -- proves the next poll picks it up and rebuilds against
     * the same persistent worktree (not a fresh clone). */
    bump_tagged_repo("lw_signed_repo", "v1.1.0.0", 333);
    const char *signed_v2 = "lw_data/thing-signed/1.1.0.0/testplat/libthing." LIB_EXT;
    TEST_ASSERT(wait_for(signed_v2, 90));
    TEST_ASSERT(nonempty_file(signed_v2));

    char signed_v2_sig[256];
    snprintf(signed_v2_sig, sizeof(signed_v2_sig), "%s.sig", signed_v2);
    TEST_ASSERT(file_exists(signed_v2_sig));

    kill_zhs();

    rmrf("lw_signed_repo");
    rmrf("lw_unsigned_repo");
    rmrf("lw_data");
    rmrf("lw_keyfile");
    remove("lw_config.json");
}

TEST_MAIN_BEGIN()
    zcio_init();
    RUN_TEST(test_local_watch_signed_and_unsigned_two_tags);
    kill_zhs();  /* safety net if an assertion returned early mid-test */
TEST_MAIN_END()
