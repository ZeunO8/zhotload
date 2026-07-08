cmake_minimum_required(VERSION 3.14)
project(zhotload VERSION 0.3.4.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
    ${PROJECT_BINARY_DIR}/inc/zhl/version.h
    @ONLY
)

# ── Embedding detection ───────────────────────────────────────────────────────
# zhl is meant to be vendored into other projects (QUANTIX, GDZ) via FetchContent
# as a *client* library only. When embedded we build just `zhl` — the standalone
# update server (zhs, which fetches mongoose) and the test suite (which builds
# fixture .so's and registers ctest) would otherwise leak into the embedder's
# build and ctest run. Mirror GDZ's top-level gate: default the server/tests ON
# only when zhotload is the top-level project.
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(ZHL_IS_TOP_LEVEL ON)
else()
    set(ZHL_IS_TOP_LEVEL OFF)
endif()

# iOS/tvOS/watchOS and Android are cross-compiled, embedded targets: the update
# server is a host-side executable and the test suite loads fixture .so's and
# runs ctest, neither of which applies on-device. Only `zhl` (the client
# library) is built there, regardless of the top-level gate above.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS"
        OR CMAKE_SYSTEM_NAME STREQUAL "tvOS"
        OR CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    set(ZHL_APPLE_EMBEDDED ON)
else()
    set(ZHL_APPLE_EMBEDDED OFF)
endif()

# CMake sets ANDROID when configured with the NDK toolchain.
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(ZHL_ANDROID_EMBEDDED ON)
else()
    set(ZHL_ANDROID_EMBEDDED OFF)
endif()

set(_zhl_tools_default OFF)
if(ZHL_IS_TOP_LEVEL AND NOT ZHL_APPLE_EMBEDDED AND NOT ZHL_ANDROID_EMBEDDED)
    set(_zhl_tools_default ON)
endif()

option(ZHL_BUILD_SERVER "Build the zhs update-server executable (fetches mongoose)" ${_zhl_tools_default})
option(ZHL_BUILD_TESTS  "Build zhl test fixtures and register ctest"                ${_zhl_tools_default})

include(FetchContent)

# cJSON is a dependency of the zhl client itself, so it is always needed. Declared
# unconditionally; FetchContent_MakeAvailable dedups by name if an embedder (or a
# sibling dependency) already declared `cjson`. Configure it lean: no shared lib,
# no CLI utils, and crucially no test suite — cJSON otherwise registers ~13 tests
# via add_test() that would leak into every embedder's ctest run.
set(ENABLE_CJSON_TEST      OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_UTILS     OFF CACHE BOOL "" FORCE)
set(CJSON_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
)
message(STATUS "Fetching cJSON from GitHub...")
FetchContent_MakeAvailable(cjson)

FetchContent_Declare(
    iostreams
    GIT_REPOSITORY https://github.com/ZeunO8/iostreams.git
    GIT_SHALLOW TRUE
)

add_subdirectory(zhl)

if(ZHL_BUILD_SERVER)
    add_subdirectory(zhs)
endif()

if(ZHL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
