/**
 * @file common.c
 * @brief Shared helpers used by both main.c and local_watch.c. See common.h.
 */
#include "common.h"

#include <zcio/crypto.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#else
#  include <dirent.h>
#  include <unistd.h>
#  include <sys/stat.h>
#endif

int zhs_parse_semver(const char *s, unsigned int *major, unsigned int *minor,
                     unsigned int *patch, unsigned int *tweak)
{
    *tweak = 0;
    int n = sscanf(s, "%u.%u.%u.%u", major, minor, patch, tweak);
    return n >= 3;
}

int zhs_semver_cmp(const char *a, const char *b)
{
    unsigned int a_maj, a_min, a_pat, a_twk, b_maj, b_min, b_pat, b_twk;
    if (!zhs_parse_semver(a, &a_maj, &a_min, &a_pat, &a_twk)) return 0;
    if (!zhs_parse_semver(b, &b_maj, &b_min, &b_pat, &b_twk)) return 0;

    if (a_maj != b_maj) return (a_maj > b_maj) ? 1 : -1;
    if (a_min != b_min) return (a_min > b_min) ? 1 : -1;
    if (a_pat != b_pat) return (a_pat > b_pat) ? 1 : -1;
    if (a_twk != b_twk) return (a_twk > b_twk) ? 1 : -1;
    return 0;
}

int zhs_is_version_dir(const char *name)
{
    unsigned int maj, min, pat, twk;
    return zhs_parse_semver(name, &maj, &min, &pat, &twk) && name[0] != '.';
}

int zhs_find_latest_version(const char *app_dir, char *out, size_t out_len)
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
        if (!zhs_is_version_dir(fd.cFileName)) continue;
        if (latest[0] == '\0' || zhs_semver_cmp(fd.cFileName, latest) > 0)
            strncpy(latest, fd.cFileName, sizeof(latest) - 1);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(app_dir);
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!zhs_is_version_dir(ent->d_name)) continue;
        if (latest[0] == '\0' || zhs_semver_cmp(ent->d_name, latest) > 0) {
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

int zhs_find_file_with_ext(const char *version_dir, const char **exts,
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

int zhs_find_artifact(const char *version_dir, char *out, size_t out_len)
{
    static const char *exts[] = { ".so", ".dylib", ".dll", NULL };
    return zhs_find_file_with_ext(version_dir, exts, out, out_len);
}

char *zhs_read_file(const char *path, size_t *len_out)
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

int zhs_sha256_file_hex(const char *path, char *hex)
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

int zhs_read_sig_hex(const char *artifact, char *sig_hex, size_t cap)
{
    char path[900];
    snprintf(path, sizeof(path), "%s.sig", artifact);
    size_t len = 0;
    char *data = zhs_read_file(path, &len);
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

int zhs_mkdir_p(const char *path)
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

int zhs_load_signing_key(const char *keyfile,
                         uint8_t pub[ZCIO_ED25519_PUBKEY_LEN],
                         uint8_t priv[ZCIO_ED25519_PRIVKEY_LEN])
{
    size_t len = 0;
    char *data = zhs_read_file(keyfile, &len);
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

int zhs_sign_file(const char *path, const uint8_t pub[ZCIO_ED25519_PUBKEY_LEN],
                  const uint8_t priv[ZCIO_ED25519_PRIVKEY_LEN])
{
    size_t len = 0;
    char *data = zhs_read_file(path, &len);
    if (!data) return -1;

    uint8_t sig[ZCIO_ED25519_SIG_LEN];
    zcio_ed25519_sign(sig, data, len, pub, priv);
    free(data);

    char sig_hex[ZCIO_ED25519_SIG_LEN * 2 + 1];
    zcio_hex_encode(sig, sizeof(sig), sig_hex);

    char sigfile[900];
    snprintf(sigfile, sizeof(sigfile), "%s.sig", path);
    FILE *fp = fopen(sigfile, "wb");
    if (!fp) return -1;
    fprintf(fp, "%s\n", sig_hex);
    fclose(fp);
    return 0;
}
