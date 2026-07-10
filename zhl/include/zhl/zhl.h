/**
 * @file zhl.h
 * @brief zhotload — hot-reload system for shared libraries.
 *
 * @defgroup zhl_public Public API
 * @{
 *
 * ## Shared Library Manifest Convention
 *
 * Every hotloadable shared library **must** export a symbol named
 * `zhl_export_table` of type `zhl_export_table_t`.  The table lists each
 * hotloadable function's name, its per-function semantic version, and
 * its function pointer.
 *
 * Example (inside the shared library source):
 * @code
 *   #include <zhl/zhl.h>
 *
 *   int my_add(int a, int b) { return a + b; }
 *
 *   static const zhl_export_entry_t entries[] = {
 *       { "my_add", "1.0.0", (void *)my_add },
 *   };
 *
 *   const zhl_export_table_t zhl_export_table = {
 *       ZHL_EXPORT_MAGIC,
 *       sizeof(entries) / sizeof(entries[0]),
 *       entries,
 *   };
 * @endcode
 *
 * ## Application-Side Usage
 *
 * 1. Create a context with zhl_ctx_create().
 * 2. Configure it (server URL, app name, current version, library path).
 * 3. Register function-pointer bindings with zhl_register_bindings().
 * 4. Call zhl_check_for_update(), zhl_download_update(), zhl_hotload_apply().
 *
 * ## Memory Ownership
 *
 * - All strings accepted by the API are **copied** internally unless
 *   explicitly stated otherwise.  The caller retains ownership of the
 *   original and may free it after the call returns.
 * - Output structs (zhl_update_info_t) are filled by value; the caller
 *   owns the output and need not free it.
 * - zhl_ctx_destroy() frees all memory owned by the context.
 *
 * @}
 */

#ifndef ZHL_ZHL_H
#define ZHL_ZHL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Magic constant for export-table validation                         */
/* ------------------------------------------------------------------ */

/** Magic number that must appear in every valid zhl_export_table_t. */
#define ZHL_EXPORT_MAGIC ((uint32_t)0x5A484C45u)

/* ------------------------------------------------------------------ */
/*  Status / error codes                                               */
/* ------------------------------------------------------------------ */

/** Return type for every fallible API function. */
typedef enum zhl_status {
    ZHL_OK                  =  0,
    ZHL_UPDATE_AVAILABLE    =  1,
    ZHL_NO_UPDATE           =  2,

    ZHL_ERR_NULL_PARAM      = -1,
    ZHL_ERR_EMPTY_STRING    = -2,
    ZHL_ERR_INVALID_URL     = -3,
    ZHL_ERR_INVALID_VERSION = -4,
    ZHL_ERR_ALLOC           = -5,
    ZHL_ERR_NETWORK         = -6,
    ZHL_ERR_HTTP_ERROR      = -7,
    ZHL_ERR_MALFORMED_RESPONSE = -8,
    ZHL_ERR_DISK_WRITE      = -9,
    ZHL_ERR_CHECKSUM_MISMATCH  = -10,
    ZHL_ERR_DL_OPEN         = -11,
    ZHL_ERR_DL_SYM          = -12,
    ZHL_ERR_INVALID_MANIFEST = -13,
    ZHL_ERR_NOT_CONFIGURED  = -14,
    ZHL_ERR_NOT_FOUND       = -15,
    ZHL_ERR_IO              = -16,
    ZHL_ERR_SIGNATURE_INVALID = -17, /**< Ed25519 verification failed.        */
    ZHL_ERR_UNSIGNED        = -18,   /**< Trusted key set but no signature.   */
    ZHL_ERR_INVALID_KEY     = -19    /**< Malformed trusted-key hex string.   */
} zhl_status_t;

/* ------------------------------------------------------------------ */
/*  Version types                                                      */
/* ------------------------------------------------------------------ */

/** Parsed semantic version (major.minor.patch[.tweak]). */
typedef struct zhl_version {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
    uint32_t tweak;   /**< Optional 4th plane; 0 when absent from the string. */
} zhl_version_t;

/** Result of comparing two versions. */
typedef enum zhl_cmp {
    ZHL_CMP_LESS    = -1,
    ZHL_CMP_EQUAL   =  0,
    ZHL_CMP_GREATER =  1
} zhl_cmp_t;

/* ------------------------------------------------------------------ */
/*  Update information                                                 */
/* ------------------------------------------------------------------ */

/** Maximum length of a version string (including NUL). */
#define ZHL_MAX_VERSION_LEN  64

/** Maximum length of a URL (including NUL). */
#define ZHL_MAX_URL_LEN      1024

/** Maximum length of a hex-encoded checksum (including NUL). */
#define ZHL_MAX_CHECKSUM_LEN 128

/** Maximum length of a hex-encoded Ed25519 signature (128 hex + NUL). */
#define ZHL_MAX_SIGNATURE_LEN 132

/** Information about an available update, filled by zhl_check_for_update(). */
typedef struct zhl_update_info {
    char version[ZHL_MAX_VERSION_LEN];
    char download_url[ZHL_MAX_URL_LEN];
    char checksum[ZHL_MAX_CHECKSUM_LEN];    /**< SHA-256 hex, "" when absent  */
    char signature[ZHL_MAX_SIGNATURE_LEN];  /**< Ed25519 hex, "" when absent  */
} zhl_update_info_t;

/* ------------------------------------------------------------------ */
/*  Export-table types (shared-library manifest)                       */
/* ------------------------------------------------------------------ */

/** A single entry in the export table of a hotloadable shared library. */
typedef struct zhl_export_entry {
    const char *name;
    const char *version;
    void       *func_ptr;
} zhl_export_entry_t;

/**
 * Export table that every hotloadable shared library must expose under
 * the symbol name `zhl_export_table`.
 */
typedef struct zhl_export_table {
    uint32_t                 magic;
    uint32_t                 count;
    const zhl_export_entry_t *entries;
} zhl_export_table_t;

/* ------------------------------------------------------------------ */
/*  Function-binding types (application side)                          */
/* ------------------------------------------------------------------ */

/**
 * Binds a named function slot to a pointer-to-function-pointer.
 *
 * The application creates an array of these, one per hotloadable
 * function, and passes it to zhl_register_bindings().
 *
 * `func_ptr` must point to a `void *` (or a function-pointer variable
 * cast to `void **`) that zhl will read and write.
 */
typedef struct zhl_func_binding {
    const char *name;
    void      **func_ptr;
} zhl_func_binding_t;

/**
 * Optional callback invoked when a function pointer is swapped.
 *
 * @param func_name  Name of the swapped function.
 * @param old_ptr    Previous function pointer value.
 * @param new_ptr    New function pointer value.
 * @param user_data  Opaque pointer supplied at registration time.
 */
typedef void (*zhl_swap_callback_t)(const char *func_name,
                                    void       *old_ptr,
                                    void       *new_ptr,
                                    void       *user_data);

/* ------------------------------------------------------------------ */
/*  Opaque context handle                                              */
/* ------------------------------------------------------------------ */

/** Opaque hotload context.  Created via zhl_ctx_create(). */
typedef struct zhl_ctx_impl *zhl_ctx_t;

/* ------------------------------------------------------------------ */
/*  Context management                                                 */
/* ------------------------------------------------------------------ */

/**
 * Create a new hotload context.
 *
 * @param[out] out  Receives the new context handle.
 * @return ZHL_OK on success, ZHL_ERR_NULL_PARAM if @p out is NULL,
 *         ZHL_ERR_ALLOC on allocation failure.
 *
 * The caller must eventually destroy the context with zhl_ctx_destroy().
 */
zhl_status_t zhl_ctx_create(zhl_ctx_t *out);

/**
 * Destroy a hotload context and free all associated resources.
 *
 * Any loaded shared libraries still referenced by the context are
 * unloaded.  After this call, @p *ctx is set to NULL.
 *
 * @param[in,out] ctx  Pointer to the context handle.
 */
void zhl_ctx_destroy(zhl_ctx_t *ctx);

/* ------------------------------------------------------------------ */
/*  Configuration setters                                              */
/* ------------------------------------------------------------------ */

/**
 * Set the remote update-server base URL (e.g. "http://localhost:8080").
 *
 * The string is copied internally.  A trailing slash is stripped if present.
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, ZHL_ERR_EMPTY_STRING, or
 *         ZHL_ERR_INVALID_URL.
 */
zhl_status_t zhl_ctx_set_server_url(zhl_ctx_t ctx, const char *url);

/**
 * Set the application identifier used when querying the server.
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, or ZHL_ERR_EMPTY_STRING.
 */
zhl_status_t zhl_ctx_set_app_name(zhl_ctx_t ctx, const char *name);

/**
 * Set the semantic version string of the currently running build.
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, ZHL_ERR_EMPTY_STRING, or
 *         ZHL_ERR_INVALID_VERSION.
 */
zhl_status_t zhl_ctx_set_current_version(zhl_ctx_t ctx, const char *version);

/**
 * Set the polling interval in seconds (0 disables polling).
 *
 * @return ZHL_OK on success.
 */
zhl_status_t zhl_ctx_set_poll_interval(zhl_ctx_t ctx, uint32_t seconds);

/**
 * Set the filesystem path of the currently loaded shared library.
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, or ZHL_ERR_EMPTY_STRING.
 */
zhl_status_t zhl_ctx_set_current_lib_path(zhl_ctx_t ctx, const char *path);

/**
 * Set the local directory where downloaded artifacts are staged.
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, or ZHL_ERR_EMPTY_STRING.
 */
zhl_status_t zhl_ctx_set_staging_dir(zhl_ctx_t ctx, const char *path);

/**
 * Set the platform identifier sent as ?platform= on update checks (e.g.
 * "android-arm64", "ios", "macos"). Servers that publish per-platform
 * builds (zhs's --watch-local-git-config) use this to pick the matching
 * artifact; servers that don't recognize it, or don't have a build for it,
 * fall back to their default/flat artifact — safe to always set this to
 * whatever the running platform actually is.
 *
 * @param platform  Platform identifier, or NULL/"" to stop sending one
 *                  (the default — matches pre-platform-aware servers).
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM.
 */
zhl_status_t zhl_ctx_set_platform(zhl_ctx_t ctx, const char *platform);

/**
 * Pin the Ed25519 public key that artifacts must be signed with.
 *
 * @param pubkey_hex  64 hex characters (32-byte key), or NULL to clear.
 *
 * Once a key is set, zhl_download_update() REQUIRES every artifact to carry
 * a valid signature: the manifest's `signature` (Ed25519 over the raw
 * artifact bytes) is verified after download and before the artifact is
 * accepted, and the SHA-256 `checksum` is enforced when present. A missing
 * signature fails with ZHL_ERR_UNSIGNED, a bad one with
 * ZHL_ERR_SIGNATURE_INVALID; the staged file is deleted in both cases.
 *
 * With no key set the previous behavior applies (checksum verified when the
 * manifest provides one, signature ignored).
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, or ZHL_ERR_INVALID_KEY.
 */
zhl_status_t zhl_ctx_set_trusted_key(zhl_ctx_t ctx, const char *pubkey_hex);

/* ------------------------------------------------------------------ */
/*  Registration                                                       */
/* ------------------------------------------------------------------ */

/**
 * Register the application's function-pointer bindings.
 *
 * @param ctx       Hotload context.
 * @param bindings  Array of binding descriptors.  The array is copied.
 * @param count     Number of elements in @p bindings.
 *
 * @return ZHL_OK, ZHL_ERR_NULL_PARAM, ZHL_ERR_NOT_CONFIGURED (if the
 *         current library path has not been set), ZHL_ERR_DL_OPEN, or
 *         ZHL_ERR_DL_SYM.
 *
 * On success every binding's `*func_ptr` is resolved from the currently
 * loaded shared library.
 */
zhl_status_t zhl_register_bindings(zhl_ctx_t            ctx,
                                   zhl_func_binding_t   *bindings,
                                   uint32_t              count);

/**
 * Set an optional callback invoked whenever a function pointer is swapped.
 *
 * Pass NULL to clear the callback.
 *
 * @return ZHL_OK on success or ZHL_ERR_NULL_PARAM if @p ctx is NULL.
 */
zhl_status_t zhl_set_swap_callback(zhl_ctx_t            ctx,
                                   zhl_swap_callback_t  cb,
                                   void                *user_data);

/* ------------------------------------------------------------------ */
/*  Update checking                                                    */
/* ------------------------------------------------------------------ */

/**
 * Query the remote server for the latest available version.
 *
 * @param ctx       Hotload context (must be fully configured).
 * @param[out] out  Filled with update information when an update is available.
 *
 * @return ZHL_UPDATE_AVAILABLE   A newer version exists; @p out is filled.
 * @return ZHL_NO_UPDATE          The server reports no newer version.
 * @return ZHL_ERR_NOT_CONFIGURED Required configuration is missing.
 * @return ZHL_ERR_NETWORK        Network-level failure.
 * @return ZHL_ERR_HTTP_ERROR     Non-2xx HTTP response.
 * @return ZHL_ERR_MALFORMED_RESPONSE  Could not parse the server response.
 */
zhl_status_t zhl_check_for_update(zhl_ctx_t          ctx,
                                  zhl_update_info_t *out);

/* ------------------------------------------------------------------ */
/*  Download                                                           */
/* ------------------------------------------------------------------ */

/**
 * Download the shared-library binary described by @p info to the staging
 * directory.
 *
 * @return ZHL_OK                  Download succeeded; path stored in context.
 * @return ZHL_ERR_NULL_PARAM      @p ctx or @p info is NULL.
 * @return ZHL_ERR_NOT_CONFIGURED  Staging directory not set.
 * @return ZHL_ERR_NETWORK         Network-level failure.
 * @return ZHL_ERR_DISK_WRITE      Could not write to disk.
 * @return ZHL_ERR_CHECKSUM_MISMATCH  Checksum verification failed.
 */
zhl_status_t zhl_download_update(zhl_ctx_t                ctx,
                                 const zhl_update_info_t *info);

/* ------------------------------------------------------------------ */
/*  Hotload apply                                                      */
/* ------------------------------------------------------------------ */

/**
 * Apply the most recently downloaded update.
 *
 * 1. Opens the currently active library and the newly downloaded library.
 * 2. Reads the `zhl_export_table` from both.
 * 3. Diffs the manifests by function name and per-function version.
 * 4. Updates **only** changed / new function pointers in the application's
 *    binding table.
 * 5. Tracks library provenance; unloads any library that no longer has
 *    any binding referencing it.
 *
 * @return ZHL_OK  At least one function was updated (or no changes needed).
 * @return ZHL_ERR_NULL_PARAM, ZHL_ERR_NOT_CONFIGURED, ZHL_ERR_DL_OPEN,
 *         ZHL_ERR_DL_SYM, ZHL_ERR_INVALID_MANIFEST.
 */
zhl_status_t zhl_hotload_apply(zhl_ctx_t ctx);

/* ------------------------------------------------------------------ */
/*  Versioning API                                                     */
/* ------------------------------------------------------------------ */

/** Return zhl's own version string (e.g. "0.1.0"). */
const char *zhl_version_string(void);

/** Return zhl's own major version number. */
uint32_t zhl_version_major(void);

/** Return zhl's own minor version number. */
uint32_t zhl_version_minor(void);

/** Return zhl's own patch version number. */
uint32_t zhl_version_patch(void);

/** Return zhl's own tweak version number. */
uint32_t zhl_version_tweak(void);

/**
 * Parse a "major.minor.patch" or "major.minor.patch.tweak" string into a
 * zhl_version_t. The 4th (tweak) plane is optional and defaults to 0 when
 * absent; projects that version in four planes (e.g. "1.10.0.0") are accepted.
 *
 * @return ZHL_OK on success, ZHL_ERR_NULL_PARAM, ZHL_ERR_EMPTY_STRING,
 *         or ZHL_ERR_INVALID_VERSION.
 */
zhl_status_t zhl_version_parse(const char    *str,
                               zhl_version_t *out);

/**
 * Compare two parsed versions.
 *
 * @return ZHL_CMP_LESS, ZHL_CMP_EQUAL, or ZHL_CMP_GREATER.
 */
zhl_cmp_t zhl_version_compare(const zhl_version_t *a,
                              const zhl_version_t *b);

/**
 * Retrieve the application version currently tracked by @p ctx.
 *
 * @param ctx  Hotload context.
 * @param[out] out  On success, receives a pointer to the internal
 *                  version string.  The pointer is valid until the
 *                  context is destroyed or the version is changed.
 *                  The caller must **not** free it.
 *
 * @return ZHL_OK or ZHL_ERR_NULL_PARAM.
 */
zhl_status_t zhl_ctx_get_active_version(const zhl_ctx_t ctx,
                                        const char     **out);

/* ------------------------------------------------------------------ */
/*  Android integration                                                */
/* ------------------------------------------------------------------ */

#ifdef __ANDROID__
#include <jni.h>

/**
 * Hand zhl the process JavaVM (Android only).
 *
 * On Android the HTTP backend speaks through java.net.HttpURLConnection over
 * JNI (libcurl is not part of the NDK), which requires a JavaVM. Because zhl is
 * a static library it cannot own JNI_OnLoad, so the embedding app must forward
 * the VM once at startup — typically straight from its own JNI_OnLoad:
 *
 * @code
 *   JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
 *       zhl_android_init(vm);
 *       return JNI_VERSION_1_6;
 *   }
 * @endcode
 *
 * Must be called before zhl_check_for_update() or zhl_download_update();
 * otherwise those return ZHL_ERR_NOT_CONFIGURED. The VM pointer is process-wide
 * and stable, so a single call is sufficient.
 *
 * @return ZHL_OK, or ZHL_ERR_NULL_PARAM if @p vm is NULL.
 */
zhl_status_t zhl_android_init(JavaVM *vm);
#endif /* __ANDROID__ */

#ifdef __cplusplus
}
#endif

#endif /* ZHL_ZHL_H */
