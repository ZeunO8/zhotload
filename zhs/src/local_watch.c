/**
 * @file local_watch.c
 * @brief Local-git-tag-watch: see local_watch.h.
 *
 * Trust model: zhs_local_watch_config_t comes from a JSON file the operator
 * of this zhs instance controls (same trust level as the CLI flags/service
 * files main.c already generates/reads) — repo paths, toolchain files and
 * extra cmake args are placed straight into shell command lines, same as
 * main.c's own `system(mk)` mkdir -p call in cmd_install_service's Linux
 * path. This is not a boundary that untrusted/remote input crosses.
 */
#include "local_watch.h"
#include "common.h"

#include <zcio/crypto.h>
#include <cJSON.h>

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
/* No background-thread implementation on Windows yet -- this feature's job
 * (driving a host cmake/toolchain build) targets developer/CI build hosts,
 * which in this project are macOS/Linux; see zhs_local_watch_start(). */
#else
#  include <pthread.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif

/* ------------------------------------------------------------------ */
/*  Config loading                                                     */
/* ------------------------------------------------------------------ */

static void get_str(const cJSON *obj, const char *key, char *out, size_t cap,
                    const char *dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring) {
        snprintf(out, cap, "%s", v->valuestring);
    } else if (dflt) {
        snprintf(out, cap, "%s", dflt);
    } else {
        out[0] = '\0';
    }
}

static int parse_target(const cJSON *j_target, zhs_local_watch_target_t *out)
{
    memset(out, 0, sizeof(*out));
    get_str(j_target, "platform", out->platform, sizeof(out->platform), NULL);
    if (!out->platform[0]) {
        fprintf(stderr, "zhs: local-watch target missing required \"platform\"\n");
        return -1;
    }
    get_str(j_target, "artifact_name", out->artifact_name, sizeof(out->artifact_name), NULL);
    if (!out->artifact_name[0]) {
        fprintf(stderr, "zhs: local-watch target '%s' missing required \"artifact_name\"\n",
                out->platform);
        return -1;
    }
    char default_build_dir[64];
    snprintf(default_build_dir, sizeof(default_build_dir), "build-zhs-%s", out->platform);
    get_str(j_target, "build_dir", out->build_dir, sizeof(out->build_dir), default_build_dir);
    get_str(j_target, "toolchain_file", out->toolchain_file, sizeof(out->toolchain_file), "");
    get_str(j_target, "extra_cmake_args", out->extra_cmake_args, sizeof(out->extra_cmake_args), "");
    get_str(j_target, "build_target", out->build_target, sizeof(out->build_target), "");
    return 0;
}

static int parse_entry(const cJSON *j_entry, zhs_local_watch_entry_t *out)
{
    memset(out, 0, sizeof(*out));
    get_str(j_entry, "app", out->app, sizeof(out->app), NULL);
    get_str(j_entry, "repo_path", out->repo_path, sizeof(out->repo_path), NULL);
    if (!out->app[0] || !out->repo_path[0]) {
        fprintf(stderr, "zhs: local-watch entry missing required \"app\"/\"repo_path\"\n");
        return -1;
    }
    get_str(j_entry, "tag_prefix", out->tag_prefix, sizeof(out->tag_prefix), "v");
    get_str(j_entry, "sign_key", out->sign_keyfile, sizeof(out->sign_keyfile), "");

    const cJSON *j_targets = cJSON_GetObjectItemCaseSensitive(j_entry, "targets");
    if (!cJSON_IsArray(j_targets) || cJSON_GetArraySize(j_targets) == 0) {
        fprintf(stderr, "zhs: local-watch entry '%s' has no \"targets\"\n", out->app);
        return -1;
    }
    const cJSON *j_target;
    cJSON_ArrayForEach(j_target, j_targets) {
        if (out->target_count >= ZHS_LOCAL_WATCH_MAX_TARGETS) {
            fprintf(stderr, "zhs: local-watch entry '%s': too many targets (max %d) — ignoring rest\n",
                    out->app, ZHS_LOCAL_WATCH_MAX_TARGETS);
            break;
        }
        if (parse_target(j_target, &out->targets[out->target_count]) == 0)
            out->target_count++;
    }
    return out->target_count > 0 ? 0 : -1;
}

int zhs_local_watch_load_config(const char *path, zhs_local_watch_config_t *out)
{
    memset(out, 0, sizeof(*out));
    out->poll_interval_s = 300;

    size_t len = 0;
    char *data = zhs_read_file(path, &len);
    if (!data) {
        fprintf(stderr, "zhs: cannot read local-watch config %s\n", path);
        return -1;
    }
    cJSON *root = cJSON_Parse(data);
    free(data);
    if (!root) {
        fprintf(stderr, "zhs: local-watch config %s is not valid JSON\n", path);
        return -1;
    }

    const cJSON *j_interval = cJSON_GetObjectItemCaseSensitive(root, "poll_interval_sec");
    if (cJSON_IsNumber(j_interval) && j_interval->valueint > 0)
        out->poll_interval_s = j_interval->valueint;

    const cJSON *j_watches = cJSON_GetObjectItemCaseSensitive(root, "watches");
    if (!cJSON_IsArray(j_watches)) {
        fprintf(stderr, "zhs: local-watch config %s has no \"watches\" array\n", path);
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *j_entry;
    cJSON_ArrayForEach(j_entry, j_watches) {
        if (out->count >= ZHS_LOCAL_WATCH_MAX_ENTRIES) {
            fprintf(stderr, "zhs: local-watch config %s: too many watches (max %d) — ignoring rest\n",
                    path, ZHS_LOCAL_WATCH_MAX_ENTRIES);
            break;
        }
        if (parse_entry(j_entry, &out->entries[out->count]) == 0)
            out->count++;
    }
    cJSON_Delete(root);
    return out->count > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Git plumbing (shells out; see file header re: trust model)         */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)

static int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Run `cmd` and capture its first line of stdout into out (cap incl. NUL).
 * Returns 0 on success (line captured, possibly empty), -1 if the command
 * itself failed to run/exit cleanly. */
static int run_capture_line(const char *cmd, char *out, size_t cap)
{
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (fgets(out, (int)cap, fp) != NULL) {
        size_t n = strlen(out);
        while (n > 0 && (out[n-1] == '\n' || out[n-1] == '\r')) out[--n] = '\0';
    }
    int rc = pclose(fp);
    return (rc == 0) ? 0 : -1;
}

/* Run `cmd`, discarding output, returning its exit status (0 = success). */
static int run_quiet(const char *cmd)
{
    int rc = system(cmd);
    return rc;
}

/* Latest tag matching "{prefix}*" in repo_path, newest-first by version sort,
 * with the prefix stripped (so it matches the bare-semver directory-naming
 * convention zhs_find_latest_version expects). 0 on success (tag captured
 * into `out`), -1 if the repo has no matching tags or git failed. */
static int latest_local_tag(const char *repo_path, const char *tag_prefix,
                            char *out, size_t cap)
{
    /* Best-effort: pulls in tags from the configured remote(s), if any. A
     * repo with no remote (or currently offline) still has whatever tags
     * already exist locally, which is enough for the common "I run zhs on
     * the same box I develop/tag on" case. */
    char fetch_cmd[700];
    snprintf(fetch_cmd, sizeof(fetch_cmd),
             "git -C '%s' fetch --tags --quiet >/dev/null 2>&1", repo_path);
    (void)run_quiet(fetch_cmd);

    char list_cmd[768];
    snprintf(list_cmd, sizeof(list_cmd),
             "git -C '%s' tag --list '%s*' --sort=-v:refname 2>/dev/null",
             repo_path, tag_prefix);
    char tag[128];
    if (run_capture_line(list_cmd, tag, sizeof(tag)) != 0 || !tag[0])
        return -1;

    size_t plen = strlen(tag_prefix);
    const char *bare = (strncmp(tag, tag_prefix, plen) == 0) ? tag + plen : tag;
    snprintf(out, cap, "%s", bare);
    return 0;
}

/* Ensure a persistent, isolated worktree for `repo_path` exists at
 * worktree_path and has `full_tag` checked out (detached HEAD — a tag isn't
 * a branch, and detached avoids "branch already checked out elsewhere"
 * conflicts on repeat runs). The worktree is intentionally never removed
 * between builds: keeping it in place lets CMake's own build-dir caching
 * give incremental (not full-clean) rebuilds on every subsequent tag. */
static int ensure_worktree_at_tag(const char *repo_path, const char *worktree_path,
                                  const char *full_tag)
{
    char prune_cmd[600];
    snprintf(prune_cmd, sizeof(prune_cmd), "git -C '%s' worktree prune 2>&1", repo_path);
    (void)run_quiet(prune_cmd);

    char cmd[1200];
    if (!dir_exists(worktree_path)) {
        if (zhs_mkdir_p(worktree_path) != 0) return -1;
        /* mkdir_p pre-creates the directory so `git worktree add` sees an
         * existing-but-empty target; git accepts that (it only refuses a
         * non-empty one). */
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' worktree add --detach '%s' '%s' 2>&1",
                 repo_path, worktree_path, full_tag);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "git -C '%s' checkout --force --detach '%s' 2>&1",
                 worktree_path, full_tag);
    }
    return run_quiet(cmd) == 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Host CMake build orchestration                                     */
/* ------------------------------------------------------------------ */

static int build_target(const char *worktree_path, const zhs_local_watch_target_t *t,
                        char *artifact_path_out, size_t cap)
{
    char build_dir[900];
    snprintf(build_dir, sizeof(build_dir), "%s/%s", worktree_path, t->build_dir);

    char configure_cmd[2048];
    int n = snprintf(configure_cmd, sizeof(configure_cmd),
                     "cmake -S '%s' -B '%s' -DCMAKE_BUILD_TYPE=Release",
                     worktree_path, build_dir);
    if (t->toolchain_file[0])
        n += snprintf(configure_cmd + n, sizeof(configure_cmd) - (size_t)n,
                      " -DCMAKE_TOOLCHAIN_FILE='%s'", t->toolchain_file);
    if (t->extra_cmake_args[0])
        snprintf(configure_cmd + n, sizeof(configure_cmd) - (size_t)n,
                 " %s", t->extra_cmake_args);
    snprintf(configure_cmd + strlen(configure_cmd),
             sizeof(configure_cmd) - strlen(configure_cmd), " 2>&1");

    printf("zhs: local-watch [%s]: configuring...\n", t->platform);
    if (run_quiet(configure_cmd) != 0) {
        fprintf(stderr, "zhs: local-watch [%s]: cmake configure failed\n", t->platform);
        return -1;
    }

    char build_cmd[1200];
    if (t->build_target[0]) {
        snprintf(build_cmd, sizeof(build_cmd),
                 "cmake --build '%s' --target '%s' -j 2>&1", build_dir, t->build_target);
    } else {
        snprintf(build_cmd, sizeof(build_cmd), "cmake --build '%s' -j 2>&1", build_dir);
    }

    printf("zhs: local-watch [%s]: building...\n", t->platform);
    if (run_quiet(build_cmd) != 0) {
        fprintf(stderr, "zhs: local-watch [%s]: cmake build failed\n", t->platform);
        return -1;
    }

    snprintf(artifact_path_out, cap, "%s/%s", build_dir, t->artifact_name);
    if (!dir_exists(build_dir)) return -1; /* build dir itself vanished -- treat as failure */
    FILE *fp = fopen(artifact_path_out, "rb");
    if (!fp) {
        fprintf(stderr, "zhs: local-watch [%s]: built, but artifact '%s' not found under %s\n",
                t->platform, t->artifact_name, build_dir);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* Copy `src` to a temp name inside `dest_dir` then rename() it into place as
 * "{dest_dir}/{basename(src)}" -- readers only ever see the fully-written
 * file, never a partial one (same "stage then atomic rename" discipline as
 * main.c's self_update_check). Returns 0 on success. */
static int publish_atomic(const char *src, const char *dest_dir, const char *filename)
{
    if (zhs_mkdir_p(dest_dir) != 0) return -1;

    size_t len = 0;
    char *data = zhs_read_file(src, &len);
    if (!data) return -1;

    char tmp_path[900], final_path[900];
    snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp-%s", dest_dir, filename);
    snprintf(final_path, sizeof(final_path), "%s/%s", dest_dir, filename);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) { free(data); return -1; }
    size_t wrote = fwrite(data, 1, len, fp);
    fclose(fp);
    free(data);
    if (wrote != len) { remove(tmp_path); return -1; }

    if (rename(tmp_path, final_path) != 0) { remove(tmp_path); return -1; }
    return 0;
}

/* Build+publish every target of one entry once `tag` has been confirmed
 * newer than what's currently published. Returns the number of targets
 * successfully published. */
static int build_and_publish_entry(const zhs_local_watch_entry_t *entry,
                                   const char *tag, const char *full_tag,
                                   const char *data_root, const char *state_root)
{
    char worktree_path[700];
    snprintf(worktree_path, sizeof(worktree_path), "%s/%s/src", state_root, entry->app);
    if (ensure_worktree_at_tag(entry->repo_path, worktree_path, full_tag) != 0) {
        fprintf(stderr, "zhs: local-watch '%s': failed to check out %s into worktree\n",
                entry->app, full_tag);
        return 0;
    }

    uint8_t pub[ZCIO_ED25519_PUBKEY_LEN], priv[ZCIO_ED25519_PRIVKEY_LEN];
    bool have_key = entry->sign_keyfile[0] &&
                    zhs_load_signing_key(entry->sign_keyfile, pub, priv) == 0;
    if (entry->sign_keyfile[0] && !have_key) {
        fprintf(stderr, "zhs: local-watch '%s': cannot load sign key %s -- publishing unsigned\n",
                entry->app, entry->sign_keyfile);
    }

    int published = 0;
    for (int i = 0; i < entry->target_count; i++) {
        const zhs_local_watch_target_t *t = &entry->targets[i];
        char artifact_path[1024];
        if (build_target(worktree_path, t, artifact_path, sizeof(artifact_path)) != 0)
            continue;

        char dest_dir[800];
        snprintf(dest_dir, sizeof(dest_dir), "%s/%s/%s/%s",
                 data_root, entry->app, tag, t->platform);
        if (publish_atomic(artifact_path, dest_dir, t->artifact_name) != 0) {
            fprintf(stderr, "zhs: local-watch '%s' [%s]: failed to publish artifact\n",
                    entry->app, t->platform);
            continue;
        }

        if (have_key) {
            char published_path[900];
            snprintf(published_path, sizeof(published_path), "%s/%s", dest_dir, t->artifact_name);
            (void)zhs_sign_file(published_path, pub, priv);
        }

        printf("zhs: local-watch: published %s %s/%s (%s)\n",
               entry->app, tag, t->platform, have_key ? "signed" : "unsigned");
        published++;
    }
    return published;
}

static int check_one_entry(const zhs_local_watch_entry_t *entry,
                           const char *data_root, const char *state_root)
{
    char tag[64];
    if (latest_local_tag(entry->repo_path, entry->tag_prefix, tag, sizeof(tag)) != 0)
        return 0; /* no tags yet, or repo unreachable -- nothing to do */

    unsigned int maj, min, pat, twk;
    if (!zhs_parse_semver(tag, &maj, &min, &pat, &twk)) {
        fprintf(stderr, "zhs: local-watch '%s': tag '%s%s' is not a semver -- skipped\n",
                entry->app, entry->tag_prefix, tag);
        return 0;
    }

    char app_dir[700];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", data_root, entry->app);
    char have[64];
    if (zhs_find_latest_version(app_dir, have, sizeof(have)) == 0 &&
        zhs_semver_cmp(tag, have) <= 0) {
        return 0; /* already current */
    }

    char full_tag[80];
    snprintf(full_tag, sizeof(full_tag), "%s%s", entry->tag_prefix, tag);
    return build_and_publish_entry(entry, tag, full_tag, data_root, state_root);
}

#endif /* !_WIN32 */

int zhs_local_watch_check_all(const zhs_local_watch_config_t *cfg,
                              const char *data_root, const char *state_root)
{
#if defined(_WIN32)
    (void)cfg; (void)data_root; (void)state_root;
    fprintf(stderr, "zhs: local-watch is not supported on this platform\n");
    return 0;
#else
    int n = 0;
    for (int i = 0; i < cfg->count; i++)
        n += check_one_entry(&cfg->entries[i], data_root, state_root);
    return n;
#endif
}

/* ------------------------------------------------------------------ */
/*  Background thread                                                  */
/* ------------------------------------------------------------------ */

#if !defined(_WIN32)

typedef struct {
    zhs_local_watch_config_t cfg;
    char data_root[512];
    char state_root[512];
} watch_thread_args_t;

static pthread_t      g_thread;
static int             g_thread_running = 0;
static volatile sig_atomic_t g_stop_flag = 0;
static watch_thread_args_t g_thread_args;

static void *watch_thread_main(void *arg)
{
    watch_thread_args_t *a = (watch_thread_args_t *)arg;
    for (;;) {
        (void)zhs_local_watch_check_all(&a->cfg, a->data_root, a->state_root);
        for (int waited = 0; waited < a->cfg.poll_interval_s; waited++) {
            if (g_stop_flag) return NULL;
            sleep(1);
        }
    }
    return NULL;
}

int zhs_local_watch_start(const zhs_local_watch_config_t *cfg,
                          const char *data_root, const char *state_root)
{
    if (g_thread_running) return -1;
    g_thread_args.cfg = *cfg;

    /* Resolve to absolute paths before storing: worktree/build paths built
     * from these get handed to `git -C repo_path ...`, which resolves a
     * relative path argument against repo_path's directory, not zhs's own
     * cwd -- a relative --data (the common "zhs --data data" case) would
     * silently check builds out under {repo_path}/data/... instead of
     * zhs's own data root, and cmake would then find nothing there. */
    if (zhs_mkdir_p(data_root) != 0) return -1;
    if (zhs_mkdir_p(state_root) != 0) return -1;
    char data_abs[512], state_abs[512];
    if (realpath(data_root, data_abs) == NULL) return -1;
    if (realpath(state_root, state_abs) == NULL) return -1;
    snprintf(g_thread_args.data_root, sizeof(g_thread_args.data_root), "%s", data_abs);
    snprintf(g_thread_args.state_root, sizeof(g_thread_args.state_root), "%s", state_abs);

    g_stop_flag = 0;
    if (pthread_create(&g_thread, NULL, watch_thread_main, &g_thread_args) != 0)
        return -1;
    g_thread_running = 1;
    return 0;
}

void zhs_local_watch_stop(void)
{
    if (!g_thread_running) return;
    g_stop_flag = 1;
    pthread_join(g_thread, NULL);
    g_thread_running = 0;
}

#else /* _WIN32 */

int zhs_local_watch_start(const zhs_local_watch_config_t *cfg,
                          const char *data_root, const char *state_root)
{
    (void)cfg; (void)data_root; (void)state_root;
    fprintf(stderr, "zhs: local-watch is not supported on this platform\n");
    return -1;
}

void zhs_local_watch_stop(void) {}

#endif
