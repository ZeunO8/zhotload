# ── HTTP backend selection ────────────────────────────────────────────────────
# libcurl is the default HTTP client on desktop platforms (macOS, Linux,
# Windows). It is not part of the iOS/tvOS/watchOS SDKs, however, so on those
# platforms the client uses a Foundation/NSURLSession backend instead — no
# third-party linkage, ships with every Apple device. Callers can override the
# auto-detected default with -DZHL_HTTP_BACKEND=curl|apple.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS"
        OR CMAKE_SYSTEM_NAME STREQUAL "tvOS"
        OR CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    set(_zhl_default_backend apple)
else()
    set(_zhl_default_backend curl)
endif()

set(ZHL_HTTP_BACKEND "${_zhl_default_backend}"
    CACHE STRING "HTTP client backend: 'curl' (libcurl) or 'apple' (NSURLSession)")
set_property(CACHE ZHL_HTTP_BACKEND PROPERTY STRINGS curl apple)

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
elseif(ZHL_HTTP_BACKEND STREQUAL "curl")
    list(APPEND ZHL_SOURCES src/zhl_http_curl.c)
else()
    message(FATAL_ERROR "Unknown ZHL_HTTP_BACKEND '${ZHL_HTTP_BACKEND}' (expected: curl, apple)")
endif()

# dlopen/dlsym is available in the iOS SDK too, so the POSIX loader covers every
# non-Windows platform including Apple embedded devices.
if(WIN32)
    list(APPEND ZHL_SOURCES src/zhl_dl_win32.c)
else()
    list(APPEND ZHL_SOURCES src/zhl_dl_posix.c)
endif()

add_library(zhl STATIC ${ZHL_SOURCES})

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
else()
    find_package(CURL REQUIRED)
    target_link_libraries(zhl PRIVATE CURL::libcurl)
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
