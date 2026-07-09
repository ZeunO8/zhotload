# ── HTTP backend selection ────────────────────────────────────────────────────
# zcio is the default HTTP client on desktop platforms (macOS, Linux, Windows):
# the top-level build fetches it via FetchContent (zhs runs on its HTTP server
# too), so the client needs no external HTTP library — this replaced the old
# libcurl find_package dependency. zcio's TLS rides its OpenSSL backend, which the
# iOS/tvOS/watchOS SDKs and the Android NDK don't ship; on those platforms the
# client instead uses the OS HTTP stack (Foundation/NSURLSession on Apple,
# JNI/HttpURLConnection on Android), which speaks https against the system
# trust store and needs no third-party linkage. Callers can override the
# auto-detected default with -DZHL_HTTP_BACKEND=zcio|apple|android.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS"
        OR CMAKE_SYSTEM_NAME STREQUAL "tvOS"
        OR CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    set(_zhl_default_backend apple)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(_zhl_default_backend android)
else()
    set(_zhl_default_backend zcio)
endif()

set(ZHL_HTTP_BACKEND "${_zhl_default_backend}"
    CACHE STRING "HTTP client backend: 'zcio' (zcio HTTP/1.1 client), 'apple' (NSURLSession), or 'android' (HttpURLConnection)")
set_property(CACHE ZHL_HTTP_BACKEND PROPERTY STRINGS zcio apple android)

set(ZHL_SOURCES
    src/zhl_ctx.c
    src/zhl_version.c
    src/zhl_update.c
    src/zhl_hotload.c
)

if(ZHL_HTTP_BACKEND STREQUAL "apple")
    if(NOT APPLE)
        message(FATAL_ERROR "ZHL_HTTP_BACKEND=apple requires an Apple platform")
    endif()
    enable_language(OBJC)
    list(APPEND ZHL_SOURCES src/zhl_http_apple.m)
elseif(ZHL_HTTP_BACKEND STREQUAL "android")
    if(NOT ANDROID)
        message(FATAL_ERROR "ZHL_HTTP_BACKEND=android requires the Android NDK toolchain")
    endif()
    list(APPEND ZHL_SOURCES src/zhl_http_android.c)
elseif(ZHL_HTTP_BACKEND STREQUAL "zcio")
    list(APPEND ZHL_SOURCES src/zhl_http_zcio.c)
else()
    message(FATAL_ERROR "Unknown ZHL_HTTP_BACKEND '${ZHL_HTTP_BACKEND}' (expected: zcio, apple, android)")
endif()

# dlopen/dlsym is available in the iOS SDK and the Android NDK too, so the POSIX
# loader covers every non-Windows platform including Apple and Android devices.
if(WIN32)
    list(APPEND ZHL_SOURCES src/zhl_dl_win32.c)
else()
    list(APPEND ZHL_SOURCES src/zhl_dl_posix.c)
endif()

add_library(zhl STATIC ${ZHL_SOURCES})
zhl_compile_options(zhl)

target_include_directories(zhl
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_BINARY_DIR}/inc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(zhl PRIVATE cjson)

if(ZHL_HTTP_BACKEND STREQUAL "apple")
    # ARC keeps the NSURLSession backend free of manual retain/release.
    set_source_files_properties(src/zhl_http_apple.m
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(zhl PRIVATE "-framework Foundation")
elseif(ZHL_HTTP_BACKEND STREQUAL "android")
    # No link step: JNI calls dispatch through the JavaVM/JNIEnv function tables,
    # and <jni.h> ships in the NDK sysroot, so the backend needs no import library.
else()
    target_link_libraries(zhl PRIVATE zcio)
endif()

target_include_directories(zhl
    PRIVATE
        ${cjson_SOURCE_DIR}
)

if(UNIX AND NOT APPLE)
    target_link_libraries(zhl PRIVATE dl)
elseif(APPLE)
    target_link_libraries(zhl PRIVATE ${CMAKE_DL_LIBS})
endif()
