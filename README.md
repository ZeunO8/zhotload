# zhotload

A hot-reload system for shared libraries. A running application can download and swap in updated function implementations from a remote update server without restarting or losing state in unaffected functions.

## Project Layout

```
zhotload/
├── CMakeLists.txt          Root build configuration
├── version.h.in            Template for generated version header
├── zhl/                    Client-side hotload library
│   ├── include/zhl/zhl.h   Public API header
│   └── src/                Implementation + platform abstraction
├── zhs/                    Update server (standalone executable)
│   └── src/main.c          HTTP server serving versioned artifacts
└── tests/                  Test suite + fixture shared libraries
    ├── fixtures/            v1, v2, and bad-manifest test libraries
    └── test_*.c             One test file per logical area
```

## Prerequisites

- CMake >= 3.21
- C11 compiler (GCC, Clang, MSVC); the fetched zcio dependency builds as C23
- OpenSSL development headers (optional — only needed for `https://` URLs with
  the desktop zcio HTTP backend; without it zcio builds TLS-free and plain
  `http://` still works)
- Android NDK (for Android builds; installed via Android Studio's SDK Manager)

## Build

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

### Running the test suite on iOS / Android

Mobile builds exclude the tests by default, but the suite runs **on-target**
through zcio's mobile test launchers (`adb` push-and-run for Android, `simctl
spawn` for the iOS simulator) when opted in with `-DZHL_BUILD_TESTS=ON`. No
device needs to be running: the launcher boots the best available
emulator/simulator, creating one via `sdkmanager`/`avdmanager` or `simctl
create` if none exists (set `ZCIO_NO_AUTOBOOT=1` to fail fast instead).

```sh
# Android
cmake -B build-android-tests \
      -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake \
      -DZHL_BUILD_TESTS=ON -DZHL_HTTP_BACKEND=zcio
cmake --build build-android-tests
ctest --test-dir build-android-tests --output-on-failure

# iOS simulator
cmake -B build-sim-tests \
      -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
      -DZHL_IOS_PLATFORM=SIMULATORARM64 -DZHL_BUILD_TESTS=ON
cmake --build build-sim-tests
ctest --test-dir build-sim-tests --output-on-failure
```

On Android the tests use the zcio HTTP backend: the shipped JNI backend
dispatches through the app's `JavaVM`, which a bare adb-shell executable does
not have. Everything else under test — context, versioning, `dlopen`
hotloading against on-device fixture libraries, update/download flows — is
the same code that ships. The iOS run uses the shipped NSURLSession backend
as-is.

## iOS

`zhl` builds as a static library for iOS and is meant to be linked into other
iOS CMake projects. zcio's TLS backend (OpenSSL) is not part of the iOS SDK, so
on iOS/tvOS/watchOS the HTTP client transparently switches to a
Foundation/NSURLSession backend (`ZHL_HTTP_BACKEND=apple`), which speaks https
against the system trust store — no third-party linkage required. `dlopen` is
used for hotloading, exactly as on other POSIX platforms.

Configure with the bundled toolchain (Xcode generator recommended):

```sh
# Device (arm64)
cmake -B build-ios -G Xcode \
      -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake
cmake --build build-ios --config Release

# Simulator (Apple silicon host — use SIMULATOR64 for Intel)
cmake -B build-sim -G Xcode \
      -DCMAKE_TOOLCHAIN_FILE=cmake/ios.toolchain.cmake \
      -DZHL_IOS_PLATFORM=SIMULATORARM64
cmake --build build-sim --config Release
```

`ZHL_IOS_PLATFORM` accepts `OS64` (device, default), `SIMULATOR64` (x86_64), or
`SIMULATORARM64`; `DEPLOYMENT_TARGET` sets the minimum iOS version (default
`13.0`). The update server and test suite are automatically excluded from
iOS-family builds — only the `zhl` client library is produced.

The library links `Foundation`; embedders that consume it must do the same. When
vendoring via `FetchContent`, just link the `zhl` target and its usage
requirements propagate:

```cmake
FetchContent_Declare(zhotload GIT_REPOSITORY <url> GIT_TAG <tag>)
FetchContent_MakeAvailable(zhotload)
target_link_libraries(my_ios_app PRIVATE zhl)
```

## Android

`zhl` builds as a static library for Android via the NDK and is meant to be
linked into an app's JNI shared library. zcio's TLS backend (OpenSSL) is not
part of the NDK, so on Android the HTTP client switches to a JNI backend built
on `java.net.HttpURLConnection` (`ZHL_HTTP_BACKEND=android`) — always present
on every device, https via the platform trust store, no third-party linkage.
`dlopen` is used for hotloading, exactly as on other POSIX platforms.

Configure with the bundled toolchain, which wraps the NDK's own toolchain and
auto-detects the NDK from `ANDROID_NDK_HOME` or the Android Studio SDK location:

```sh
# arm64 device (default ABI + API 24)
cmake -B build-android \
      -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake
cmake --build build-android

# other ABIs / API level
cmake -B build-android-x86 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/android.toolchain.cmake \
      -DANDROID_ABI=x86_64 -DANDROID_PLATFORM=android-26
cmake --build build-android-x86
```

`ANDROID_ABI` accepts `arm64-v8a` (default), `armeabi-v7a`, `x86_64`, or `x86`;
`ANDROID_PLATFORM` sets the minimum API level (default `android-24`). If the NDK
is not on a standard path, pass `-DANDROID_NDK=/path/to/ndk`. The update server
and test suite are automatically excluded from Android builds — only the `zhl`
client library is produced.

Because a static library cannot own `JNI_OnLoad`, the embedding app must hand
zhl the process `JavaVM` once at startup, typically straight from its own
`JNI_OnLoad`:

```c
#include <zhl/zhl.h>

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    zhl_android_init(vm);          /* required before any update/download call */
    return JNI_VERSION_1_6;
}
```

The app also needs the `INTERNET` permission in its manifest, and downloaded
`.so` files must be staged in the app's own writable data directory and opened
by absolute path (`dlopen` of app-private files works from API 24 onward). Note
that Google Play policy forbids downloading executable code for Play-distributed
apps; this backend is intended for sideloaded, enterprise, or internal builds.

When vendoring via `FetchContent`, link the `zhl` target and its usage
requirements propagate:

```cmake
FetchContent_Declare(zhotload GIT_REPOSITORY <url> GIT_TAG <tag>)
FetchContent_MakeAvailable(zhotload)
target_link_libraries(my_android_lib PRIVATE zhl)
```

## Quick-Start Example

```c
#include <zhl/zhl.h>
#include <stdio.h>

typedef int (*binary_op)(int, int);

static binary_op op_add;
static binary_op op_multiply;

int main(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);

    zhl_ctx_set_server_url(ctx, "http://localhost:8080");
    zhl_ctx_set_app_name(ctx, "myapp");
    zhl_ctx_set_current_version(ctx, "1.0.0");
    zhl_ctx_set_current_lib_path(ctx, "./libmyapp.so");
    zhl_ctx_set_staging_dir(ctx, "/tmp/zhl_staging");

    zhl_func_binding_t bindings[] = {
        { "add",      (void **)&op_add },
        { "multiply", (void **)&op_multiply },
    };
    zhl_register_bindings(ctx, bindings, 2);

    printf("add(2,3) = %d\n", op_add(2, 3));

    zhl_update_info_t info;
    if (zhl_check_for_update(ctx, &info) == ZHL_UPDATE_AVAILABLE) {
        zhl_download_update(ctx, &info);
        zhl_hotload_apply(ctx);
        printf("After hotload: multiply(2,3) = %d\n", op_multiply(2, 3));
    }

    zhl_ctx_destroy(&ctx);
    return 0;
}
```

## Shared Library Manifest

Every hotloadable shared library must export a `zhl_export_table` symbol:

```c
#include <zhl/zhl.h>

int my_add(int a, int b) { return a + b; }

static const zhl_export_entry_t entries[] = {
    { "my_add", "1.0.0", (void *)my_add },
};

const zhl_export_table_t zhl_export_table = {
    ZHL_EXPORT_MAGIC,
    1,
    entries,
};
```

## Running the Update Server

```sh
./build/zhs/zhs --data ./data --port 8080 [--host localhost]
```

The `/latest` scan is per-request, so dropping `{data}/{app}/{version}/artifact.so`
into the tree publishes it immediately (watch-dir ingestion). `--host` scopes
the listener to one interface (default: all).

See `zhs/README.md` for the data directory layout and how to register applications.

## Signed Artifacts

Artifacts can (and should) be Ed25519-signed; verification is built into both
ends with no OpenSSL dependency (zcio/crypto.h), so mobile clients verify too:

```sh
zhs keygen update.key                      # writes update.key (seed, 0600) + update.key.pub
zhs sign update.key data/myapp/1.2.0/libmyapp.so   # writes libmyapp.so.sig
```

`GET /apps/{app}/latest` then reports `checksum` (SHA-256) and `signature`
(Ed25519 over the artifact bytes). On the client, pin the public key:

```c
zhl_ctx_set_trusted_key(ctx, "f03afe6d...");   /* 64 hex chars */
```

Once pinned, `zhl_download_update()` REQUIRES a valid signature — a missing
one fails with `ZHL_ERR_UNSIGNED`, a bad one with `ZHL_ERR_SIGNATURE_INVALID`,
and the staged file is deleted. Without a pinned key the legacy behavior
applies (checksum enforced when present).

## Running zhs as a Self-Updating Service

```sh
zhs install-service --data /srv/zhs-data --port 8080 \
    --watch-self zhs --trust-key @update.key.pub
```

Writes a launchd agent (macOS, `KeepAlive`) or systemd user unit (Linux,
`Restart=always`) and prints the load command. With `--watch-self APP
--trust-key K`, zhs checks `{data}/{APP}` between poll iterations: when a
newer version than the running build carries a file named `zhs` whose
`zhs.sig` verifies against the trusted key, the running binary is atomically
replaced (write sibling + rename) and the process exits 0 — the service
manager restarts it on the new version. Self-update is never unsigned; the
trust key is inlined into the service file so no key file is needed at
runtime.

## Dependencies

| Library      | Used by  | Fetch method   | Purpose                              |
|--------------|----------|----------------|--------------------------------------|
| zcio         | zhl, zhs | `FetchContent` | HTTP client (desktop backend) + HTTP server |
| Foundation   | zhl      | system framework | HTTP client (iOS/tvOS/watchOS backend) |
| HttpURLConnection | zhl | Android framework (JNI) | HTTP client (Android backend)    |
| cJSON        | zhl      | `FetchContent` | JSON parsing                         |
| OpenSSL      | zcio     | `find_package` (optional) | TLS for `https://` on the desktop backend |
