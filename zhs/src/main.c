/**
 * @file main.c
 * @brief zhs — hotload update server.
 *
 * Serves versioned shared-library artifacts over HTTP.
 *
 * Endpoints:
 *   GET /apps/{app_name}/latest
 *       -> JSON: { "version", "download_url", "checksum" }
 *
 *   GET /apps/{app_name}/download/{version}
 *       -> Raw binary artifact.
 *
 * Data layout:
 *   {data_root}/{app_name}/{version}/artifact.{so|dylib|dll}
 *
 * HTTP server: zcio's hardened HTTP/1.1 server (zcio/http_server.h) — the same
 * zcio dependency the zhl client's HTTP backend uses, replacing mongoose. The
 * handler runs synchronously inside the poll loop; both endpoints are
 * filesystem reads, well within "keep it short".
 */

#include <zcio/http_server.h>
#include <zcio/types.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#define DEFAULT_DATA_ROOT "data"
#define DEFAULT_PORT      8080

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
    DIR *d = opendir(app_dir);
    if (!d) return -1;

    char latest[64] = {0};
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL) {
        if (!is_version_dir(ent->d_name)) continue;
        if (latest[0] == '\0' || semver_cmp(ent->d_name, latest) > 0) {
            strncpy(latest, ent->d_name, sizeof(latest) - 1);
        }
    }
    closedir(d);

    if (latest[0] == '\0') return -1;
    strncpy(out, latest, out_len - 1);
    out[out_len - 1] = '\0';
    return 0;
}

static int find_artifact(const char *version_dir, char *out, size_t out_len)
{
    DIR *d = opendir(version_dir);
    if (!d) return -1;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len > 3 && strcmp(name + len - 3, ".so") == 0) goto found;
        if (len > 6 && strcmp(name + len - 6, ".dylib") == 0) goto found;
        if (len > 4 && strcmp(name + len - 4, ".dll") == 0) goto found;
    }
    closedir(d);
    return -1;

found:
    snprintf(out, out_len, "%s/%s", version_dir, ent->d_name);
    closedir(d);
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

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"download_url\":\"/apps/%s/download/%s\",\"checksum\":\"\"}",
             latest, app_name, latest);

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

    FILE *fp = fopen(artifact, "rb");
    if (!fp) {
        send_error(r, 500, "Cannot open artifact");
        return;
    }

    /* Responses go out as one complete zcio_http_respond, so the artifact is
     * read whole into memory. Artifacts are shared libraries — MBs, not GBs. */
    long file_size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) file_size = ftell(fp);
    if (file_size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        send_error(r, 500, "Cannot stat artifact");
        return;
    }

    char *data = (char *)malloc(file_size ? (size_t)file_size : 1);
    if (!data) {
        fclose(fp);
        send_error(r, 500, "Out of memory");
        return;
    }

    size_t got = fread(data, 1, (size_t)file_size, fp);
    fclose(fp);
    if (got != (size_t)file_size) {
        free(data);
        send_error(r, 500, "Cannot read artifact");
        return;
    }

    const zcio_http_header hdr[] = {
        { "Content-Type", "application/octet-stream" },
    };
    zcio_http_respond(r, 200, hdr, 1, data, (size_t)file_size);
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

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            strncpy(g_data_root, argv[++i], sizeof(g_data_root) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: zhs [--data DIR] [--port PORT]\n");
            printf("  --data DIR   Root directory for app artifacts (default: %s)\n", DEFAULT_DATA_ROOT);
            printf("  --port PORT  HTTP listen port (default: %d)\n", DEFAULT_PORT);
            return 0;
        }
    }

    zcio_http_server_config cfg = {0};
    cfg.port = port;
    /* Whole artifacts are queued as a single response; raise the per-connection
     * output cap (default 4 MiB) so large shared libraries fit comfortably. */
    cfg.max_out_bytes = (size_t)256 * 1024 * 1024;
    /* Handlers are sub-millisecond filesystem reads, so the graceful-stop
     * drain (default 5 s) only delays Ctrl-C; a second is ample. */
    cfg.drain_timeout_ms = 1000;

    zcio_http_server *s = zcio_http_server_start(&cfg, ev_handler, NULL);
    if (!s) {
        fprintf(stderr, "Failed to listen on :%d (%s)\n", port, zcio_last_error());
        return 1;
    }
    g_server = s;

    signal(SIGINT, on_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_signal);
#endif

    printf("zhs listening on http://0.0.0.0:%d, serving from %s\n",
           zcio_http_server_port(s), g_data_root);

    int rc = zcio_http_server_run(s);
    g_server = NULL;
    zcio_http_server_free(s);

    if (rc != ZCIO_OK) {
        fprintf(stderr, "zhs: server loop failed (%s)\n", zcio_result_str((zcio_result)rc));
        return 1;
    }
    return 0;
}
