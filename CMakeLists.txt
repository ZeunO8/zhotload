cmake_minimum_required(VERSION 3.14)
project(zhotload VERSION 0.3.3.0 LANGUAGES C)

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

option(ZHL_BUILD_SERVER "Build the zhs update-server executable (fetches mongoose)" ${ZHL_IS_TOP_LEVEL})
option(ZHL_BUILD_TESTS  "Build zhl test fixtures and register ctest"                ${ZHL_IS_TOP_LEVEL})

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
FetchContent_MakeAvailable(cjson)

add_subdirectory(zhl)

if(ZHL_BUILD_SERVER)
    add_subdirectory(zhs)
endif()

if(ZHL_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
