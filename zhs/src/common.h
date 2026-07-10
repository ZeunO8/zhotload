/**
 * @file common.h
 * @brief Shared helpers used by both main.c (HTTP serving, self-update,
 * remote --ingest) and local_watch.c (local-git-tag-watch + host builds).
 *
 * Pulled out of main.c verbatim (no behavior change) so both modules agree
 * on one semver/versioned-directory scheme instead of risking drift between
 * two independently-maintained copies.
 */
#ifndef ZHS_COMMON_H
#define ZHS_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <zcio/crypto.h>

/* ------------------------------------------------------------------ */
/*  Version comparison for finding the latest version                  */
/* ------------------------------------------------------------------ */

/* Parse 3- or 4-plane versions (major.minor.patch[.tweak]); the tweak plane
 * is optional and defaults to 0. Mirrors zhl_version_parse on the client so
 * the server and clients agree on ordering (including tweak-only bumps). */
int zhs_parse_semver(const char *s, unsigned int *major, unsigned int *minor,
                     unsigned int *patch, unsigned int *tweak);

int zhs_semver_cmp(const char *a, const char *b);

int zhs_is_version_dir(const char *name);

/* Scans app_dir for the highest-semver subdirectory name. 0 on success. */
int zhs_find_latest_version(const char *app_dir, char *out, size_t out_len);

/* Find a file in version_dir whose name ends with one of `exts` (NULL-
 * terminated list). Returns 0 and the full path in `out`, or -1. */
int zhs_find_file_with_ext(const char *version_dir, const char **exts,
                           char *out, size_t out_len);

/* Find a shared-library artifact (.so/.dylib/.dll) directly inside
 * version_dir. Returns 0 and the full path in `out`, or -1. */
int zhs_find_artifact(const char *version_dir, char *out, size_t out_len);

/* ------------------------------------------------------------------ */
/*  File helpers                                                       */
/* ------------------------------------------------------------------ */

/* Read a whole file. Returns malloc'd buffer (caller frees) or NULL. */
char *zhs_read_file(const char *path, size_t *len_out);

/* Streaming SHA-256 of a file into `hex` (65 bytes). 0 on success. */
int zhs_sha256_file_hex(const char *path, char *hex);

/* Read "{artifact}.sig" (hex text) into sig_hex (cap incl. NUL), trimming
 * trailing whitespace. Returns 0 on success, -1 when absent/invalid. */
int zhs_read_sig_hex(const char *artifact, char *sig_hex, size_t cap);

/* mkdir -p. Best-effort: a pre-existing directory is success. */
int zhs_mkdir_p(const char *path);

/* Load a 32-byte Ed25519 seed from a keygen-written file and expand it. */
int zhs_load_signing_key(const char *keyfile,
                         uint8_t pub[ZCIO_ED25519_PUBKEY_LEN],
                         uint8_t priv[ZCIO_ED25519_PRIVKEY_LEN]);

/* Sign `path` with (pub, priv) and write "{path}.sig" (Ed25519 hex over the
 * raw bytes) next to it. Returns 0 on success. */
int zhs_sign_file(const char *path, const uint8_t pub[ZCIO_ED25519_PUBKEY_LEN],
                  const uint8_t priv[ZCIO_ED25519_PRIVKEY_LEN]);

#endif
