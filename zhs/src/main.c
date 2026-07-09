/**
 * @file main.c
 * @brief zhs — hotload update server.
 *
 * Serves versioned shared-library artifacts over HTTP, with Ed25519 artifact
 * signing so zhl clients can verify what they load (zcio/crypto.h — no
 * OpenSSL dependency).
 *
 * Endpoints:
 *   GET /apps/{app_name}/latest
 *       -> JSON: { "version", "download_url", "checksum", "signature" }
 *          checksum  = SHA-256 hex of the artifact bytes
 *          signature = Ed25519 hex from "{artifact}.sig" ("" when unsigned)
 *
 *   GET /apps/{app_name}/download/{version}
 *       -> Raw binary artifact.
 *
 * Data layout:
 *   {data_root}/{app_name}/{version}/artifact.{so|dylib|dll}[.sig]
 *   The /latest scan is per-request, so dropping a new version directory into
 *   the tree publishes it immediately (watch-dir ingestion).
 *
 * Publisher tooling (subcommands):
 *   zhs keygen <keyfile>            Generate an Ed25519 keypair: <keyfile>
 *                                   holds the 32-byte seed (hex, 0600) and
 *                                   <keyfile>.pub the public key (hex).
 *   zhs sign <keyfile> <file>...    Write "{file}.sig" (Ed25519 hex over the
 *                                   raw bytes) for each artifact.
 *   zhs install-service [opts]      Write a launchd plist (macOS) / systemd
 *                                   user unit (Linux) that keeps zhs running
 *                                   and restarts it after self-update.
 *
 * Self-update:
 *   --watch-self APP --trust-key HEX|@FILE
 *       Between poll iterations zhs scans its own data root for app APP; when
 *       a version newer than this build appears, carrying a file named "zhs"
 *       whose "zhs.sig" verifies against the trusted key, the running binary
 *       is atomically replaced (write sibling + rename) and the process exits
 *       0 — the service manager (KeepAlive / Restart=always) restarts it on
 *       the new version. Unsigned or badly signed candidates are ignored.
 *
 * Git-tag (remote) ingestion:
 *   --ingest APP=OWNER/REPO[:ASSET_SUBSTR]
 *       Between poll iterations zhs polls the GitHub Releases API for
 *       OWNER/REPO's latest release. When its tag is a newer semver than what
 *       APP already has locally, the first release asset matching
 *       ASSET_SUBSTR (or, if omitted, the first non-".sig" asset) is
 *       downloaded into {data}/{APP}/{tag}/, along with a same-named
 *       ".sig" sibling asset when the release publishes one. This is the
 *       "URL" counterpart to watch-dir ingestion (dropping a version
 *       directory in by hand) — CI publishes a GitHub Release with signed
 *       artifacts as assets, and zhs picks it up on its own. Repeatable, one
 *       entry per app. Test override: env ZHS_GITHUB_API_BASE replaces
 *       "https://api.github.com" (points the poller at a mock server).
 *
 * HTTP server: zcio's hardened HTTP/1.1 server (zcio/http_server.h). The
 * handler runs synchronously inside the poll loop; both endpoints are
 * filesystem reads, well within "keep it short".
 */

#include <zcio/http.h>
#include <zcio/http_server.h>
#include <zcio/crypto.h>
#include <zcio/types.h>
#include <zhl/version.h>
#include <cJSON.h>

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif
#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

#define DEFAULT_DATA_ROOT "data"
#define DEFAULT_PORT      8080
#define DEFAULT_SELF_INTERVAL_S 30

static char g_data_root[512] = DEFAULT_DATA_ROOT;

/* ------------------------------------------------------------------ */
/*  Version comparison for finding the latest version                  */
/* ------------------------------------------------------------------ */

/* Parse 3- or 4-plane versions (major.minor.patch[.tweak]); the tweak plane is
 * optional and defaults to 0. Mirrors zhl_version_parse on the client so the
 * server and clients agree on ordering (including tweak-only bumps). */
static int parse_semver(const char *s, unsigned int *major, unsigned int *minor,
                        unsigned int *patch, unsigned int *tweak)
{
    *tweak = 0;
    int n = sscanf(s, "%u.%u.%u.%u", major, minor, patch, tweak);
    return n >= 3;
}

static int semver_cmp(const char *a, const char *b)
{
    unsigned int a_maj, a_min, a_pat, a_twk, b_maj, b_min, b_pat, b_twk;
    if (!parse_semver(a, &a_maj, &a_min, &a_pat, &a_twk)) return 0;
    if (!parse_semver(b, &b_maj, &b_min, &b_pat, &b_twk)) return 0;

    if (a_maj != b_maj) return (a_maj > b_maj) ? 1 : -1;
    if (a_min != b_min) return (a_min > b_min) ? 1 : -1;
    if (a_pat != b_pat) return (a_pat > b_pat) ? 1 : -1;
    if (a_twk != b_twk) return (a_twk > b_twk) ? 1 : -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Directory scanning helpers                                         */
/* ------------------------------------------------------------------ */

static int is_version_dir(const char *name)
{
    unsigned int maj, min, pat, twk;
    return parse_semver(name, &maj, &min, &pat, &twk) && name[0] != '.';
}

static int find_latest_version(const char *app_dir, char *out, size_t out_len)
{
    char latest[64] = {0};
#if defined(_WIN32)
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s\\*", app_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!is_version_dir(fd.cFileName)) continue;
        if (latest[0] == '\0' || semver_cmp(fd.cFileName, latest) > 0)
            strncpy(latest, fd.cFileName, sizeof(latest) - 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(app_dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!is_version_dir(ent->d_name)) continue;
        if (latest[0] == '\0' || semver_cmp(ent->d_name, latest) > 0) {
            strncpy(latest, ent->d_name, sizeof(latest) - 1);
        }
    }
    closedir(d);
#endif
    if (latest[0] == '\0') return -1;
    strncpy(out, latest, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

/* Find a file in version_dir whose name ends with one of `exts` (NULL-
 * terminated list). Returns 0 and the full path in `out`, or -1. */
static int find_file_with_ext(const char *version_dir, const char **exts,
                              char *out, size_t out_len)
{
#if defined(_WIN32)
    char pattern[900];
    snprintf(pattern, sizeof(pattern), "%s\\*", version_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        const char *name = fd.cFileName;
        size_t len = strlen(name);
        for (const char **e = exts; *e; e++) {
            size_t el = strlen(*e);
            if (len > el && strcmp(name + len - el, *e) == 0) {
                snprintf(out, out_len, "%s/%s", version_dir, name);
                FindClose(h);
                return 0;
            }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return -1;
#else
    DIR *d = opendir(version_dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        for (const char **e = exts; *e; e++) {
            size_t el = strlen(*e);
            if (len > el && strcmp(name + len - el, *e) == 0) {
                snprintf(out, out_len, "%s/%s", version_dir, name);
                closedir(d);
                return 0;
            }
        }
    }
    closedir(d);
    return -1;
#endif
}

static int find_artifact(const char *version_dir, char *out, size_t out_len)
{
    static const char *exts[] = { ".so", ".dylib", ".dll", NULL };
    return find_file_with_ext(version_dir, exts, out, out_len);
}

/* ------------------------------------------------------------------ */
/*  File helpers                                                       */
/* ------------------------------------------------------------------ */

/* Read a whole file. Returns malloc'd buffer (caller frees) or NULL. */
static char *read_file(const char *path, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *data = (char *)malloc(sz ? (size_t)sz + 1 : 1);
    if (!data) { fclose(fp); return NULL; }
    size_t got = fread(data, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(data); return NULL; }
    data[sz] = '\0';
    if (len_out) *len_out = (size_t)sz;
    return data;
}

/* Streaming SHA-256 of a file into `hex` (65 bytes). 0 on success. */
static int sha256_file_hex(const char *path, char *hex)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    zcio_sha256_ctx c;
    zcio_sha256_init(&c);
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        zcio_sha256_update(&c, buf, n);
    int err = ferror(fp);
    fclose(fp);
    if (err) return -1;
    uint8_t digest[ZCIO_SHA256_LEN];
    zcio_sha256_final(&c, digest);
    zcio_hex_encode(digest, sizeof(digest), hex);
    return 0;
}

/* Read "{artifact}.sig" (hex text) into sig_hex (cap incl. NUL), trimming
 * trailing whitespace. Returns 0 on success, -1 when absent/invalid. */
static int read_sig_hex(const char *artifact, char *sig_hex, size_t cap)
{
    char path[900];
    snprintf(path, sizeof(path), "%s.sig", artifact);
    size_t len = 0;
    char *data = read_file(path, &len);
    if (!data) return -1;
    while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r' ||
                       data[len-1] == ' '  || data[len-1] == '\t'))
        len--;
    if (len == 0 || len >= cap) { free(data); return -1; }
    memcpy(sig_hex, data, len);
    sig_hex[len] = '\0';
    free(data);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  HTTP handlers                                                      */
/* ------------------------------------------------------------------ */

static void send_json(zcio_http_req *r, int status, const char *json)
{
    const zcio_http_header hdr[] = {
        { "Content-Type", "application/json" },
    };
    zcio_http_respond(r, status, hdr, 1, json, strlen(json));
}

static void send_error(zcio_http_req *r, int status, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    send_json(r, status, buf);
}

static void handle_latest(zcio_http_req *r, const char *app_name)
{
    char app_dir[600];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", g_data_root, app_name);

    char latest[64];
    if (find_latest_version(app_dir, latest, sizeof(latest)) != 0) {
        send_error(r, 404, "No versions found");
        return;
    }

    char version_dir[700];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", app_dir, latest);
    char artifact[800];
    if (find_artifact(version_dir, artifact, sizeof(artifact)) != 0) {
        send_error(r, 404, "Artifact not found");
        return;
    }

    char checksum[ZCIO_SHA256_LEN * 2 + 1] = "";
    if (sha256_file_hex(artifact, checksum) != 0) {
        send_error(r, 500, "Cannot hash artifact");
        return;
    }

    char signature[ZCIO_ED25519_SIG_LEN * 2 + 8] = "";
    (void)read_sig_hex(artifact, signature, sizeof(signature));

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"download_url\":\"/apps/%s/download/%s\","
             "\"checksum\":\"%s\",\"signature\":\"%s\"}",
             latest, app_name, latest, checksum, signature);

    send_json(r, 200, json);
}

static void handle_download(zcio_http_req *r,
                            const char *app_name,
                            const char *version)
{
    char version_dir[700];
    snprintf(version_dir, sizeof(version_dir), "%s/%s/%s",
             g_data_root, app_name, version);

    char artifact[800];
    if (find_artifact(version_dir, artifact, sizeof(artifact)) != 0) {
        send_error(r, 404, "Artifact not found");
        return;
    }

    /* Responses go out as one complete zcio_http_respond, so the artifact is
     * read whole into memory. Artifacts are shared libraries — MBs, not GBs. */
    size_t file_size = 0;
    char *data = read_file(artifact, &file_size);
    if (!data) {
        send_error(r, 500, "Cannot read artifact");
        return;
    }

    const zcio_http_header hdr[] = {
        { "Content-Type", "application/octet-stream" },
    };
    zcio_http_respond(r, 200, hdr, 1, data, file_size);
    free(data);
}

static void ev_handler(zcio_http_req *r, void *user)
{
    (void)user;

    const char *method = zcio_http_req_method(r);
    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(r, 405, "Method not allowed");
        return;
    }

    /* zcio hands the handler a percent-decoded, dot-segment-normalized path,
     * so ".."-style traversal never reaches these routes. */
    const char *uri = zcio_http_req_path(r);

    char app_name[256] = {0};
    char version[64] = {0};
    char tail = 0;

    /* Check the more specific /download/{version} route first. sscanf reports
     * the count of assigned fields *before* a trailing literal mismatch, so a
     * "/download/…" URI would otherwise satisfy the "…/latest" pattern's single
     * %[^/] conversion (count == 1) and misroute to handle_latest. The `%c`
     * tail guard on /latest rejects any trailing characters after "/latest". */
    if (sscanf(uri, "/apps/%255[^/]/download/%63s", app_name, version) == 2) {
        handle_download(r, app_name, version);
    } else if (sscanf(uri, "/apps/%255[^/]/latest%c", app_name, &tail) == 1) {
        handle_latest(r, app_name);
    } else {
        send_error(r, 404, "Not found");
    }
}

/* ------------------------------------------------------------------ */
/*  Publisher tooling: keygen / sign                                   */
/* ------------------------------------------------------------------ */

static int cmd_keygen(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "Usage: zhs keygen <keyfile>\n");
        return 2;
    }
    const char *keyfile = argv[0];

    uint8_t pub[ZCIO_ED25519_PUBKEY_LEN], priv[ZCIO_ED25519_PRIVKEY_LEN];
    uint8_t seed[ZCIO_ED25519_SEED_LEN];
    if (zcio_ed25519_keypair(pub, priv, seed) != ZCIO_OK) {
        fprintf(stderr, "zhs keygen: %s\n", zcio_last_error());
        return 1;
    }

    char seed_hex[ZCIO_ED25519_SEED_LEN * 2 + 1];
    char pub_hex[ZCIO_ED25519_PUBKEY_LEN * 2 + 1];
    zcio_hex_encode(seed, sizeof(seed), seed_hex);
    zcio_hex_encode(pub, sizeof(pub), pub_hex);

    FILE *fp = fopen(keyfile, "wb");
    if (!fp) { fprintf(stderr, "zhs keygen: cannot write %s\n", keyfile); return 1; }
    fprintf(fp, "%s\n", seed_hex);
    fclose(fp);
#if !defined(_WIN32)
    chmod(keyfile, 0600);
#endif

    char pubfile[600];
    snprintf(pubfile, sizeof(pubfile), "%s.pub", keyfile);
    fp = fopen(pubfile, "wb");
    if (!fp) { fprintf(stderr, "zhs keygen: cannot write %s\n", pubfile); return 1; }
    fprintf(fp, "%s\n", pub_hex);
    fclose(fp);

    printf("private seed: %s (0600)\npublic key:   %s (also %s)\n",
           keyfile, pub_hex, pubfile);
    return 0;
}

/* Load a 32-byte Ed25519 seed from a keygen-written file and expand it. */
static int load_signing_key(const char *keyfile,
                            uint8_t pub[ZCIO_ED25519_PUBKEY_LEN],
                            uint8_t priv[ZCIO_ED25519_PRIVKEY_LEN])
{
    size_t len = 0;
    char *data = read_file(keyfile, &len);
    if (!data) return -1;
    while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r')) len--;
    data[len] = '\0';
    uint8_t seed[ZCIO_ED25519_SEED_LEN];
    int ok = zcio_hex_decode(data, seed, sizeof(seed)) == ZCIO_ED25519_SEED_LEN;
    free(data);
    if (!ok) return -1;
    zcio_ed25519_keypair_from_seed(pub, priv, seed);
    memset(seed, 0, sizeof(seed));
    return 0;
}

static int cmd_sign(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: zhs sign <keyfile> <file>...\n");
        return 2;
    }
    uint8_t pub[ZCIO_ED25519_PUBKEY_LEN], priv[ZCIO_ED25519_PRIVKEY_LEN];
    if (load_signing_key(argv[0], pub, priv) != 0) {
        fprintf(stderr, "zhs sign: cannot load key from %s\n", argv[0]);
        return 1;
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        size_t len = 0;
        char *data = read_file(argv[i], &len);
        if (!data) {
            fprintf(stderr, "zhs sign: cannot read %s\n", argv[i]);
            rc = 1;
            continue;
        }
        uint8_t sig[ZCIO_ED25519_SIG_LEN];
        zcio_ed25519_sign(sig, data, len, pub, priv);
        free(data);

        char sig_hex[ZCIO_ED25519_SIG_LEN * 2 + 1];
        zcio_hex_encode(sig, sizeof(sig), sig_hex);

        char sigfile[900];
        snprintf(sigfile, sizeof(sigfile), "%s.sig", argv[i]);
        FILE *fp = fopen(sigfile, "wb");
        if (!fp) {
            fprintf(stderr, "zhs sign: cannot write %s\n", sigfile);
            rc = 1;
            continue;
        }
        fprintf(fp, "%s\n", sig_hex);
        fclose(fp);
        printf("%s -> %s\n", argv[i], sigfile);
    }
    return rc;
}

/* ------------------------------------------------------------------ */
/*  Self-update                                                        */
/* ------------------------------------------------------------------ */

static char    g_self_app[128]  = "";
static uint8_t g_trust_key[ZCIO_ED25519_PUBKEY_LEN];
static int     g_trust_key_set  = 0;
static int     g_self_interval  = DEFAULT_SELF_INTERVAL_S;

/* Absolute path of the running executable. 0 on success. */
static int self_exe_path(char *out, size_t cap)
{
#if defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(out, &size) == 0 ? 0 : -1;
#elif defined(_WIN32)
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    return (n > 0 && n < cap) ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0) return -1;
    out[n] = '\0';
    return 0;
#endif
}

/* Check the data root for a newer signed zhs build; when found, atomically
 * replace the running binary and return 1 (caller exits so the service
 * manager restarts on the new version). Returns 0 when there is nothing to
 * do; failures are logged and treated as "nothing to do". */
static int self_update_check(void)
{
    char app_dir[700];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", g_data_root, g_self_app);

    char latest[64];
    if (find_latest_version(app_dir, latest, sizeof(latest)) != 0) return 0;
    if (semver_cmp(latest, ZHL_VERSION_STRING) <= 0) return 0;

    char version_dir[800];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", app_dir, latest);
    char candidate[900];
    snprintf(candidate, sizeof(candidate), "%s/zhs", version_dir);

    size_t len = 0;
    char *data = read_file(candidate, &len);
    if (!data) return 0;   /* no executable artifact in this version dir */

    char sig_hex[ZCIO_ED25519_SIG_LEN * 2 + 8];
    uint8_t sig[ZCIO_ED25519_SIG_LEN];
    if (read_sig_hex(candidate, sig_hex, sizeof(sig_hex)) != 0 ||
        zcio_hex_decode(sig_hex, sig, sizeof(sig)) != ZCIO_ED25519_SIG_LEN ||
        zcio_ed25519_verify(sig, data, len, g_trust_key) != 1) {
        fprintf(stderr, "zhs: self-update %s found but signature invalid/missing — ignored\n",
                latest);
        free(data);
        return 0;
    }

    char exe[1024];
    if (self_exe_path(exe, sizeof(exe)) != 0) { free(data); return 0; }

    /* Stage next to the running binary (same filesystem), then rename over
     * it — atomic on POSIX; the running process keeps its mapped image. */
    char staged[1100];
    snprintf(staged, sizeof(staged), "%s.new", exe);
    FILE *fp = fopen(staged, "wb");
    if (!fp) { free(data); return 0; }
    size_t wrote = fwrite(data, 1, len, fp);
    fclose(fp);
    free(data);
    if (wrote != len) { remove(staged); return 0; }
#if !defined(_WIN32)
    chmod(staged, 0755);
#endif
    if (rename(staged, exe) != 0) {
        remove(staged);
        fprintf(stderr, "zhs: self-update rename failed\n");
        return 0;
    }

    printf("zhs: self-updated %s -> %s; exiting for service restart\n",
           ZHL_VERSION_STRING, latest);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Git-tag (remote) ingestion                                          */
/* ------------------------------------------------------------------ */

#define ZHS_MAX_INGESTS 16
#define ZHS_DEFAULT_INGEST_INTERVAL_S 300

typedef struct {
    char app[128];
    char owner_repo[256];
    char asset_substr[128];   /* "" = first non-".sig" asset */
} ingest_source_t;

static ingest_source_t g_ingests[ZHS_MAX_INGESTS];
static int g_ingest_count = 0;
static int g_ingest_interval = ZHS_DEFAULT_INGEST_INTERVAL_S;

/* mkdir -p. Best-effort: a pre-existing directory is success. */
static int mkdir_p(const char *path)
{
    char tmp[1024];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
#if defined(_WIN32)
            if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
            *p = '/';
        }
    }
#if defined(_WIN32)
    if (_mkdir(tmp) != 0 && errno != EEXIST) return -1;
#else
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
#endif
    return 0;
}

/* Parse "APP=OWNER/REPO[:ASSET_SUBSTR]" into *out. Returns 0 on success. */
static int parse_ingest_arg(const char *arg, ingest_source_t *out)
{
    memset(out, 0, sizeof(*out));
    const char *eq = strchr(arg, '=');
    if (!eq || eq == arg) return -1;
    size_t app_len = (size_t)(eq - arg);
    if (app_len >= sizeof(out->app)) return -1;
    memcpy(out->app, arg, app_len);
    out->app[app_len] = '\0';

    const char *rest = eq + 1;
    const char *colon = strchr(rest, ':');
    size_t repo_len = colon ? (size_t)(colon - rest) : strlen(rest);
    if (repo_len == 0 || repo_len >= sizeof(out->owner_repo)) return -1;
    memcpy(out->owner_repo, rest, repo_len);
    out->owner_repo[repo_len] = '\0';
    if (!strchr(out->owner_repo, '/')) return -1;   /* must be owner/repo */

    if (colon && colon[1]) {
        snprintf(out->asset_substr, sizeof(out->asset_substr), "%s", colon + 1);
    }
    return 0;
}

/* GET url (no auth — public release assets/API) and write the body to
 * dir/filename. 0 on success. */
static int download_to_file(const char *url, const char *dir, const char *filename)
{
    static const zcio_http_header hdr = { "User-Agent", "zhs-update-server" };
    zcio_http_opts opts = { .timeout_ms = 30000 };
    zcio_http_response r = zcio_http_request_opts("GET", url, &hdr, 1, NULL, 0, &opts);
    if (r.status != 200 || !r.body) {
        fprintf(stderr, "zhs: ingest: GET %s failed (status %d, %s)\n",
                url, r.status, zcio_last_error());
        zcio_http_response_free(&r);
        return -1;
    }
    char path[900];
    snprintf(path, sizeof(path), "%s/%s", dir, filename);
    FILE *fp = fopen(path, "wb");
    if (!fp) { zcio_http_response_free(&r); return -1; }
    size_t wrote = fwrite(r.body, 1, r.body_size, fp);
    fclose(fp);
    int ok = wrote == r.body_size;
    zcio_http_response_free(&r);
    if (!ok) remove(path);
    return ok ? 0 : -1;
}

/* Poll one GitHub repo's latest release; if its tag is newer than what `src`'s
 * app already has locally, download the matching asset (+ ".sig" sibling, if
 * published) into {data}/{app}/{tag}/. Best-effort throughout: any failure is
 * logged and treated as "nothing to do" so one bad source never wedges the
 * poll loop or takes down the others. Returns 1 on a successful ingest. */
static int ingest_check_one(const ingest_source_t *src)
{
    const char *api_base = getenv("ZHS_GITHUB_API_BASE");
    if (!api_base || !api_base[0]) api_base = "https://api.github.com";

    char url[600];
    snprintf(url, sizeof(url), "%s/repos/%s/releases/latest", api_base, src->owner_repo);

    static const zcio_http_header hdrs[] = {
        { "User-Agent", "zhs-update-server" },
        { "Accept", "application/vnd.github+json" },
    };
    zcio_http_opts opts = { .timeout_ms = 10000 };
    zcio_http_response r = zcio_http_request_opts("GET", url, hdrs, 2, NULL, 0, &opts);
    if (r.status != 200 || !r.body) {
        fprintf(stderr, "zhs: ingest %s: fetch failed (status %d, %s)\n",
                src->owner_repo, r.status, zcio_last_error());
        zcio_http_response_free(&r);
        return 0;
    }

    cJSON *root = cJSON_Parse(r.body);
    zcio_http_response_free(&r);
    if (!root) {
        fprintf(stderr, "zhs: ingest %s: malformed release JSON\n", src->owner_repo);
        return 0;
    }

    const cJSON *j_tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(j_tag) || !j_tag->valuestring) {
        cJSON_Delete(root);
        return 0;
    }
    /* Tags are conventionally "v1.2.3.4"; strip a leading v/V to match the
     * bare semver directory-naming convention find_latest_version expects.
     * Copied into a fixed buffer up front: j_tag->valuestring is owned by
     * `root`, which is deleted partway through this function, but `tag` is
     * still needed afterward (to build version_dir). */
    const char *raw_tag = j_tag->valuestring;
    const char *tag_src = (raw_tag[0] == 'v' || raw_tag[0] == 'V') ? raw_tag + 1 : raw_tag;
    char tag[64];
    snprintf(tag, sizeof(tag), "%s", tag_src);
    unsigned int maj, min, pat, twk;
    if (!parse_semver(tag, &maj, &min, &pat, &twk)) {
        fprintf(stderr, "zhs: ingest %s: tag '%s' is not a semver — skipped\n",
                src->owner_repo, raw_tag);
        cJSON_Delete(root);
        return 0;
    }

    char app_dir[700];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", g_data_root, src->app);
    char have[64];
    if (find_latest_version(app_dir, have, sizeof(have)) == 0 &&
        semver_cmp(tag, have) <= 0) {
        cJSON_Delete(root);
        return 0;   /* already current */
    }

    const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    if (!cJSON_IsArray(assets)) {
        fprintf(stderr, "zhs: ingest %s: release %s has no assets array\n",
                src->owner_repo, tag);
        cJSON_Delete(root);
        return 0;
    }

    /* First pass: the primary artifact — first non-".sig" asset matching
     * asset_substr (or the first non-".sig" asset when unset). */
    char primary_name[256] = "", primary_url[1024] = "";
    const cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *j_name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        const cJSON *j_url  = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        if (!cJSON_IsString(j_name) || !cJSON_IsString(j_url)) continue;
        const char *name = j_name->valuestring;
        size_t nlen = strlen(name);
        bool is_sig = nlen > 4 && strcmp(name + nlen - 4, ".sig") == 0;
        if (primary_name[0] || is_sig) continue;
        if (src->asset_substr[0] && !strstr(name, src->asset_substr)) continue;
        snprintf(primary_name, sizeof(primary_name), "%s", name);
        snprintf(primary_url, sizeof(primary_url), "%s", j_url->valuestring);
    }
    if (!primary_name[0]) {
        fprintf(stderr, "zhs: ingest %s: release %s has no matching asset%s%s\n",
                src->owner_repo, tag,
                src->asset_substr[0] ? " for pattern " : "", src->asset_substr);
        cJSON_Delete(root);
        return 0;
    }

    /* Second pass: an optional "{primary_name}.sig" sibling asset. */
    char sig_name[300], sig_url[1024] = "";
    snprintf(sig_name, sizeof(sig_name), "%s.sig", primary_name);
    cJSON_ArrayForEach(asset, assets) {
        const cJSON *j_name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        const cJSON *j_url  = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
        if (!cJSON_IsString(j_name) || !cJSON_IsString(j_url)) continue;
        if (strcmp(j_name->valuestring, sig_name) == 0) {
            snprintf(sig_url, sizeof(sig_url), "%s", j_url->valuestring);
            break;
        }
    }
    cJSON_Delete(root);

    char version_dir[800];
    snprintf(version_dir, sizeof(version_dir), "%s/%s", app_dir, tag);
    if (mkdir_p(version_dir) != 0) {
        fprintf(stderr, "zhs: ingest %s: cannot create %s\n", src->owner_repo, version_dir);
        return 0;
    }

    if (download_to_file(primary_url, version_dir, primary_name) != 0) {
        fprintf(stderr, "zhs: ingest %s: failed to download %s\n",
                src->owner_repo, primary_name);
        return 0;
    }
    if (sig_url[0]) {
        /* Best-effort: an unsigned ingest still publishes — pinned-key
         * clients simply reject it, same as a hand-dropped unsigned build. */
        (void)download_to_file(sig_url, version_dir, sig_name);
    }

    printf("zhs: ingested %s/%s %s -> %s (%s)\n",
           src->owner_repo, primary_name, tag, src->app,
           sig_url[0] ? "signed" : "unsigned");
    return 1;
}

/* Poll every configured ingest source once. Returns the number ingested. */
static int ingest_check_all(void)
{
    int n = 0;
    for (int i = 0; i < g_ingest_count; i++)
        n += ingest_check_one(&g_ingests[i]) ? 1 : 0;
    return n;
}

/* ------------------------------------------------------------------ */
/*  Service install                                                    */
/* ------------------------------------------------------------------ */

/* Format one ingest source back into its "APP=OWNER/REPO[:ASSET_SUBSTR]" CLI
 * form, for baking into a generated service file. */
static void format_ingest_arg(const ingest_source_t *src, char *out, size_t cap)
{
    if (src->asset_substr[0])
        snprintf(out, cap, "%s=%s:%s", src->app, src->owner_repo, src->asset_substr);
    else
        snprintf(out, cap, "%s=%s", src->app, src->owner_repo);
}

static int cmd_install_service(const char *data_root, int port,
                               const char *host,
                               const char *self_app, const char *trust_key_arg,
                               const ingest_source_t *ingests, int ingest_count,
                               int ingest_interval)
{
    char exe[1024];
    if (self_exe_path(exe, sizeof(exe)) != 0) {
        fprintf(stderr, "zhs: cannot resolve own executable path\n");
        return 1;
    }
    char data_abs[1024];
#if defined(_WIN32)
    if (_fullpath(data_abs, data_root, sizeof(data_abs)) == NULL)
        snprintf(data_abs, sizeof(data_abs), "%s", data_root);
#else
    if (realpath(data_root, data_abs) == NULL) {
        fprintf(stderr, "zhs: data root %s must exist before install-service\n",
                data_root);
        return 1;
    }
#endif
    const char *home = getenv("HOME");

#if defined(__APPLE__)
    if (!home) { fprintf(stderr, "zhs: HOME not set\n"); return 1; }
    char dir[1024], plist[1200];
    snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home);
    (void)mkdir(dir, 0755);
    snprintf(plist, sizeof(plist), "%s/com.zeun.zhs.plist", dir);

    FILE *fp = fopen(plist, "wb");
    if (!fp) { fprintf(stderr, "zhs: cannot write %s\n", plist); return 1; }
    fprintf(fp,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "  <key>Label</key><string>com.zeun.zhs</string>\n"
        "  <key>ProgramArguments</key>\n  <array>\n"
        "    <string>%s</string>\n"
        "    <string>--data</string><string>%s</string>\n"
        "    <string>--port</string><string>%d</string>\n", exe, data_abs, port);
    if (host && host[0])
        fprintf(fp, "    <string>--host</string><string>%s</string>\n", host);
    if (self_app && self_app[0] && trust_key_arg && trust_key_arg[0])
        fprintf(fp, "    <string>--watch-self</string><string>%s</string>\n"
                    "    <string>--trust-key</string><string>%s</string>\n",
                self_app, trust_key_arg);
    for (int i = 0; i < ingest_count; i++) {
        char arg[600];
        format_ingest_arg(&ingests[i], arg, sizeof(arg));
        fprintf(fp, "    <string>--ingest</string><string>%s</string>\n", arg);
    }
    if (ingest_count > 0)
        fprintf(fp, "    <string>--ingest-interval</string><string>%d</string>\n",
                ingest_interval);
    fprintf(fp,
        "  </array>\n"
        "  <key>RunAtLoad</key><true/>\n"
        "  <key>KeepAlive</key><true/>\n"
        "  <key>StandardOutPath</key><string>%s/Library/Logs/zhs.log</string>\n"
        "  <key>StandardErrorPath</key><string>%s/Library/Logs/zhs.log</string>\n"
        "</dict>\n</plist>\n", home, home);
    fclose(fp);
    printf("Wrote %s\nLoad with:\n  launchctl load -w %s\n", plist, plist);
    return 0;
#elif defined(__linux__)
    if (!home) { fprintf(stderr, "zhs: HOME not set\n"); return 1; }
    char dir[1024], unit[1200];
    snprintf(dir, sizeof(dir), "%s/.config/systemd/user", home);
    char mk[1100];
    snprintf(mk, sizeof(mk), "mkdir -p '%s'", dir);
    if (system(mk) != 0) { fprintf(stderr, "zhs: cannot create %s\n", dir); return 1; }
    snprintf(unit, sizeof(unit), "%s/zhs.service", dir);

    FILE *fp = fopen(unit, "wb");
    if (!fp) { fprintf(stderr, "zhs: cannot write %s\n", unit); return 1; }
    fprintf(fp,
        "[Unit]\nDescription=zhotload update server\nAfter=network.target\n\n"
        "[Service]\nExecStart=%s --data %s --port %d", exe, data_abs, port);
    if (host && host[0]) fprintf(fp, " --host %s", host);
    if (self_app && self_app[0] && trust_key_arg && trust_key_arg[0])
        fprintf(fp, " --watch-self %s --trust-key %s", self_app, trust_key_arg);
    for (int i = 0; i < ingest_count; i++) {
        char arg[600];
        format_ingest_arg(&ingests[i], arg, sizeof(arg));
        fprintf(fp, " --ingest %s", arg);
    }
    if (ingest_count > 0) fprintf(fp, " --ingest-interval %d", ingest_interval);
    fprintf(fp,
        "\nRestart=always\nRestartSec=1\n\n"
        "[Install]\nWantedBy=default.target\n");
    fclose(fp);
    printf("Wrote %s\nEnable with:\n"
           "  systemctl --user daemon-reload && systemctl --user enable --now zhs\n",
           unit);
    return 0;
#else
    (void)port; (void)host; (void)self_app; (void)trust_key_arg;
    (void)ingests; (void)ingest_count; (void)ingest_interval;
    fprintf(stderr, "zhs: install-service is not supported on this platform\n");
    return 1;
#endif
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

static zcio_http_server *g_server = NULL;

/* zcio_http_server_stop is async-signal-safe: it sets a flag and pokes a wake
 * pipe, then run() drains in-flight exchanges and returns. */
static void on_signal(int sig)
{
    (void)sig;
    if (g_server) zcio_http_server_stop(g_server);
}

/* --trust-key accepts 64 hex chars inline or @file (e.g. @update.key.pub). */
static int parse_trust_key(const char *arg)
{
    char hexbuf[80];
    const char *hex = arg;
    if (arg[0] == '@') {
        size_t len = 0;
        char *data = read_file(arg + 1, &len);
        if (!data) { fprintf(stderr, "zhs: cannot read key file %s\n", arg + 1); return -1; }
        while (len > 0 && (data[len-1] == '\n' || data[len-1] == '\r')) len--;
        if (len >= sizeof(hexbuf)) { free(data); return -1; }
        memcpy(hexbuf, data, len);
        hexbuf[len] = '\0';
        free(data);
        hex = hexbuf;
    }
    if (zcio_hex_decode(hex, g_trust_key, sizeof(g_trust_key))
            != ZCIO_ED25519_PUBKEY_LEN) {
        fprintf(stderr, "zhs: --trust-key must be 64 hex chars (or @file)\n");
        return -1;
    }
    g_trust_key_set = 1;
    return 0;
}

static void usage(void)
{
    printf("Usage: zhs [--data DIR] [--port PORT] [--host HOST]\n"
           "           [--watch-self APP --trust-key HEX|@FILE [--self-interval SEC]]\n"
           "           [--ingest APP=OWNER/REPO[:ASSET_SUBSTR] ...] [--ingest-interval SEC]\n"
           "       zhs keygen <keyfile>\n"
           "       zhs sign <keyfile> <file>...\n"
           "       zhs install-service [--data DIR] [--port PORT] [--host HOST]\n"
           "                           [--watch-self APP --trust-key HEX|@FILE]\n"
           "                           [--ingest APP=OWNER/REPO[:ASSET_SUBSTR] ...]\n"
           "  --data DIR        Root directory for app artifacts (default: %s)\n"
           "  --port PORT       HTTP listen port (default: %d)\n"
           "  --host HOST       Interface to bind (default: all; e.g. localhost)\n"
           "  --watch-self APP  Self-update from {data}/{APP}: when a newer\n"
           "                    version carries a signed 'zhs' binary, replace\n"
           "                    this executable atomically and exit 0 for the\n"
           "                    service manager to restart\n"
           "  --trust-key K     Ed25519 public key artifacts must be signed with\n"
           "  --self-interval S Self-update check cadence in seconds (default %d)\n"
           "  --ingest A=O/R[:S] Poll GitHub repo O/R's latest release; when its tag\n"
           "                    is newer than app A's local version, download the\n"
           "                    asset matching substring S (default: first non-.sig\n"
           "                    asset) into {data}/A/{tag}/, plus a \".sig\" sibling\n"
           "                    asset if the release publishes one. Repeatable.\n"
           "  --ingest-interval S  Ingestion poll cadence in seconds (default %d)\n",
           DEFAULT_DATA_ROOT, DEFAULT_PORT, DEFAULT_SELF_INTERVAL_S,
           ZHS_DEFAULT_INGEST_INTERVAL_S);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "keygen") == 0)
        return cmd_keygen(argc - 2, argv + 2);
    if (argc > 1 && strcmp(argv[1], "sign") == 0)
        return cmd_sign(argc - 2, argv + 2);

    int  port = DEFAULT_PORT;
    char host[256] = "";
    char trust_key_arg[600] = "";
    int  install = (argc > 1 && strcmp(argv[1], "install-service") == 0);

    for (int i = install ? 2 : 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            strncpy(g_data_root, argv[++i], sizeof(g_data_root) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            strncpy(host, argv[++i], sizeof(host) - 1);
        } else if (strcmp(argv[i], "--watch-self") == 0 && i + 1 < argc) {
            strncpy(g_self_app, argv[++i], sizeof(g_self_app) - 1);
        } else if (strcmp(argv[i], "--trust-key") == 0 && i + 1 < argc) {
            strncpy(trust_key_arg, argv[++i], sizeof(trust_key_arg) - 1);
        } else if (strcmp(argv[i], "--self-interval") == 0 && i + 1 < argc) {
            g_self_interval = atoi(argv[++i]);
            if (g_self_interval < 1) g_self_interval = 1;
        } else if (strcmp(argv[i], "--ingest") == 0 && i + 1 < argc) {
            const char *arg = argv[++i];
            if (g_ingest_count >= ZHS_MAX_INGESTS) {
                fprintf(stderr, "zhs: too many --ingest sources (max %d) — ignoring %s\n",
                        ZHS_MAX_INGESTS, arg);
            } else if (parse_ingest_arg(arg, &g_ingests[g_ingest_count]) != 0) {
                fprintf(stderr,
                        "zhs: malformed --ingest '%s' (want APP=OWNER/REPO[:ASSET_SUBSTR])\n",
                        arg);
                return 1;
            } else {
                g_ingest_count++;
            }
        } else if (strcmp(argv[i], "--ingest-interval") == 0 && i + 1 < argc) {
            g_ingest_interval = atoi(argv[++i]);
            if (g_ingest_interval < 1) g_ingest_interval = 1;
        } else if (strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        }
    }

    if (install) {
        /* Inline an @file trust key as hex: the service manager runs zhs with
         * a different cwd, so a relative key path would not resolve there. */
        if (trust_key_arg[0] == '@') {
            if (parse_trust_key(trust_key_arg) != 0) return 1;
            zcio_hex_encode(g_trust_key, sizeof(g_trust_key), trust_key_arg);
        }
        return cmd_install_service(g_data_root, port, host,
                                   g_self_app, trust_key_arg,
                                   g_ingests, g_ingest_count, g_ingest_interval);
    }

    if (g_self_app[0]) {
        if (!trust_key_arg[0] || parse_trust_key(trust_key_arg) != 0) {
            fprintf(stderr,
                    "zhs: --watch-self requires a valid --trust-key "
                    "(self-update is never unsigned)\n");
            return 1;
        }
    }

    zcio_http_server_config cfg = {0};
    cfg.port = port;
    if (host[0]) cfg.bind_host = host;
    /* Whole artifacts are queued as a single response; raise the per-connection
     * output cap (default 4 MiB) so large shared libraries fit comfortably. */
    cfg.max_out_bytes = (size_t)256 * 1024 * 1024;
    /* Handlers are sub-millisecond filesystem reads, so the graceful-stop
     * drain (default 5 s) only delays Ctrl-C; a second is ample. */
    cfg.drain_timeout_ms = 1000;
    /* zhs runs as a fixed-port service and restarts itself on the SAME port
     * after a signed self-update (exit 0 -> supervisor re-execs) — needs to
     * rebind immediately rather than fail on a lingering TIME_WAIT. */
    cfg.reuse_addr = true;

    zcio_http_server *s = zcio_http_server_start(&cfg, ev_handler, NULL);
    if (!s) {
        fprintf(stderr, "Failed to listen on %s:%d (%s)\n",
                host[0] ? host : "0.0.0.0", port, zcio_last_error());
        return 1;
    }
    g_server = s;

    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif

    printf("zhs %s listening on http://%s:%d, serving from %s%s%s\n",
           ZHL_VERSION_STRING, host[0] ? host : "0.0.0.0",
           zcio_http_server_port(s), g_data_root,
           g_self_app[0] ? " (self-update armed)" : "",
           g_ingest_count > 0 ? " (ingestion armed)" : "");

    /* Event loop with self-update + ingestion checks between iterations.
     * poll() returns ZCIO_ERR_EOF (negative) once a stop() has fully drained. */
    int rc = ZCIO_OK;
    time_t next_self = time(NULL) + g_self_interval;
    time_t next_ingest = time(NULL) + g_ingest_interval;
    int self_updated = 0;
    for (;;) {
        int p = zcio_http_server_poll(s, 1000);
        if (p < 0) {
            rc = (p == ZCIO_ERR_EOF) ? ZCIO_OK : p;
            break;
        }
        if (g_self_app[0] && !self_updated && time(NULL) >= next_self) {
            next_self = time(NULL) + g_self_interval;
            if (self_update_check()) {
                self_updated = 1;
                zcio_http_server_stop(s);   /* drain, then exit 0 below */
            }
        }
        if (g_ingest_count > 0 && time(NULL) >= next_ingest) {
            next_ingest = time(NULL) + g_ingest_interval;
            (void)ingest_check_all();
        }
    }
    g_server = NULL;
    zcio_http_server_free(s);

    if (rc != ZCIO_OK) {
        fprintf(stderr, "zhs: server loop failed (%s)\n", zcio_result_str((zcio_result)rc));
        return 1;
    }
    return 0;
}
