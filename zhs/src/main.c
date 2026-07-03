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
 */

#include "mongoose.h"
#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#define DEFAULT_DATA_ROOT "data"
#define DEFAULT_PORT      "8080"

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

static void send_json(struct mg_connection *c, int code, const char *json)
{
    mg_printf(c,
              "HTTP/1.1 %d OK\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %u\r\n"
              "\r\n"
              "%s",
              code, (unsigned)strlen(json), json);
}

static void send_error(struct mg_connection *c, int code, const char *msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    mg_printf(c,
              "HTTP/1.1 %d %s\r\n"
              "Content-Type: application/json\r\n"
              "Content-Length: %u\r\n"
              "\r\n"
              "%s",
              code, msg, (unsigned)strlen(buf), buf);
}

static void handle_latest(struct mg_connection *c, const char *app_name)
{
    char app_dir[600];
    snprintf(app_dir, sizeof(app_dir), "%s/%s", g_data_root, app_name);

    char latest[64];
    if (find_latest_version(app_dir, latest, sizeof(latest)) != 0) {
        send_error(c, 404, "No versions found");
        return;
    }

    char json[1024];
    snprintf(json, sizeof(json),
             "{\"version\":\"%s\",\"download_url\":\"/apps/%s/download/%s\",\"checksum\":\"\"}",
             latest, app_name, latest);

    send_json(c, 200, json);
}

static void handle_download(struct mg_connection *c,
                            const char *app_name,
                            const char *version)
{
    char version_dir[700];
    snprintf(version_dir, sizeof(version_dir), "%s/%s/%s",
             g_data_root, app_name, version);

    char artifact[800];
    if (find_artifact(version_dir, artifact, sizeof(artifact)) != 0) {
        send_error(c, 404, "Artifact not found");
        return;
    }

    struct stat st;
    if (stat(artifact, &st) != 0) {
        send_error(c, 500, "Cannot stat artifact");
        return;
    }

    FILE *fp = fopen(artifact, "rb");
    if (!fp) {
        send_error(c, 500, "Cannot open artifact");
        return;
    }

    mg_printf(c,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: application/octet-stream\r\n"
              "Content-Length: %ld\r\n"
              "\r\n",
              (long)st.st_size);

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        mg_send(c, buf, n);
    }
    fclose(fp);
}

static void ev_handler(struct mg_connection *c, int ev, void *ev_data)
{
    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    char uri[512];
    snprintf(uri, sizeof(uri), "%.*s", (int)hm->uri.len, hm->uri.buf);

    char app_name[256] = {0};
    char version[64] = {0};
    char tail = 0;

    /* Check the more specific /download/{version} route first. sscanf reports
     * the count of assigned fields *before* a trailing literal mismatch, so a
     * "/download/…" URI would otherwise satisfy the "…/latest" pattern's single
     * %[^/] conversion (count == 1) and misroute to handle_latest. The `%c`
     * tail guard on /latest rejects any trailing characters after "/latest". */
    if (sscanf(uri, "/apps/%255[^/]/download/%63s", app_name, version) == 2) {
        handle_download(c, app_name, version);
    } else if (sscanf(uri, "/apps/%255[^/]/latest%c", app_name, &tail) == 1) {
        handle_latest(c, app_name);
    } else {
        send_error(c, 404, "Not found");
    }
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *port = DEFAULT_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--data") == 0 && i + 1 < argc) {
            strncpy(g_data_root, argv[++i], sizeof(g_data_root) - 1);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: zhs [--data DIR] [--port PORT]\n");
            printf("  --data DIR   Root directory for app artifacts (default: %s)\n", DEFAULT_DATA_ROOT);
            printf("  --port PORT  HTTP listen port (default: %s)\n", DEFAULT_PORT);
            return 0;
        }
    }

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char listen_addr[64];
    snprintf(listen_addr, sizeof(listen_addr), "http://0.0.0.0:%s", port);

    struct mg_connection *c = mg_http_listen(&mgr, listen_addr, ev_handler, NULL);
    if (!c) {
        fprintf(stderr, "Failed to listen on %s\n", listen_addr);
        return 1;
    }

    printf("zhs listening on %s, serving from %s\n", listen_addr, g_data_root);

    for (;;) {
        mg_mgr_poll(&mgr, 1000);
    }

    mg_mgr_free(&mgr);
    return 0;
}
