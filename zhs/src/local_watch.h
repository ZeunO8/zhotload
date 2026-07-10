/**
 * @file local_watch.h
 * @brief Local-git-tag-watch: build+publish client artifacts straight from a
 * local git checkout's tags, using the HOST's own CMake + toolchain, instead
 * of waiting on a CI pipeline to push a GitHub Release (see --ingest in
 * main.c, the URL/CI counterpart of this).
 *
 * Typical use: a macOS build host runs zhs pointed at its own QUANTIX
 * checkout. On a new tag it checks the tag out into an isolated, persistent
 * git worktree (never touches the caller's live working tree) and drives
 * `cmake --build` once per configured target (e.g. a plain host build and an
 * Android NDK cross-build via a toolchain file), publishing each artifact
 * under a platform-qualified path so zhs can serve the right one to the
 * right client (see the ?platform= handling in main.c).
 *
 * Config is a JSON file (--watch-local-git-config path.json) rather than
 * repeated CLI flags: each watched app can have several build targets, and
 * each target has enough knobs (toolchain file, extra -D args, build
 * target name) that flattening it into "APP=..." CLI syntax like --ingest
 * does would be unreadable.
 */
#ifndef ZHS_LOCAL_WATCH_H
#define ZHS_LOCAL_WATCH_H

#define ZHS_LOCAL_WATCH_MAX_TARGETS  8
#define ZHS_LOCAL_WATCH_MAX_ENTRIES  16

typedef struct {
    char platform[32];        /* served under {data}/{app}/{version}/{platform}/ */
    char build_dir[128];      /* build dir name inside the worktree; reused
                                * across builds for incremental compiles */
    char toolchain_file[512]; /* "" = host default toolchain */
    char extra_cmake_args[512]; /* verbatim extra "-D..." args, space-separated */
    char build_target[128];   /* `cmake --build ... --target NAME`; "" = default (all) */
    char artifact_name[128];  /* exact file name expected directly under build_dir,
                                * e.g. "libqtx_tick_hot.dylib" / "libqtx_tick_hot.so" */
} zhs_local_watch_target_t;

typedef struct {
    char app[128];             /* zhs app name published artifacts serve under */
    char repo_path[512];       /* local git repository to watch */
    char tag_prefix[16];       /* e.g. "v"; stripped before semver parsing (default "v") */
    char sign_keyfile[512];    /* "" = publish unsigned */
    zhs_local_watch_target_t targets[ZHS_LOCAL_WATCH_MAX_TARGETS];
    int  target_count;
} zhs_local_watch_entry_t;

typedef struct {
    zhs_local_watch_entry_t entries[ZHS_LOCAL_WATCH_MAX_ENTRIES];
    int count;
    int poll_interval_s;   /* default 300 */
} zhs_local_watch_config_t;

/* Parse a JSON config file into *out. Returns 0 on success; on failure logs
 * a diagnostic to stderr and returns -1 (config left zeroed/partial). */
int zhs_local_watch_load_config(const char *path, zhs_local_watch_config_t *out);

/* Runs the watch loop on a dedicated background thread (builds can take
 * minutes; the caller's HTTP poll loop must stay responsive) until
 * zhs_local_watch_stop() is called. `data_root` is zhs's existing artifact
 * tree; `state_root` holds the persistent per-app worktrees
 * ({state_root}/{app}/src, reused build-to-build for incremental compiles).
 * Returns 0 if the thread was started, -1 on failure (e.g. platform without
 * thread support). */
int zhs_local_watch_start(const zhs_local_watch_config_t *cfg,
                          const char *data_root, const char *state_root);

/* Signals the background thread to stop after its current poll iteration and
 * joins it. Safe to call even if start() was never called or already
 * stopped. */
void zhs_local_watch_stop(void);

/* Poll every configured entry once, synchronously on the calling thread:
 * for each, checks the repo for a tag newer than what's currently published
 * and, if found, builds+publishes every target. Returns the number of
 * (entry, target) pairs successfully published. Best-effort throughout — one
 * bad entry/target is logged and skipped, never wedging the others. Exposed
 * separately from start()/stop() so tests can drive one deterministic pass
 * without racing a background thread. */
int zhs_local_watch_check_all(const zhs_local_watch_config_t *cfg,
                              const char *data_root, const char *state_root);

#endif
