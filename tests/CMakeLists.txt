# Test fixtures — shared libraries used to exercise the hotload mechanism.

# On-target mobile runs: cross-compiled test binaries can't execute on the
# build host, so zcio's CTest launchers run them on the Android emulator /
# device (adb push-and-run) or inside the booted iOS simulator (simctl
# spawn). Must precede every add_executable below so the test targets inherit
# CROSSCOMPILING_EMULATOR. Opt in with -DZHL_BUILD_TESTS=ON on a cross build;
# see "On-target mobile testing" in zcio's README.
if(CMAKE_CROSSCOMPILING)
    include(${zcio_SOURCE_DIR}/cmake/ZcioMobileTest.cmake)
    zcio_enable_mobile_testing()
endif()

add_library(fixture_v1 SHARED fixtures/fixture_v1.c)
zhl_compile_options(fixture_v1)
target_link_libraries(fixture_v1 PRIVATE zhl)
set_target_properties(fixture_v1 PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

add_library(fixture_v2 SHARED fixtures/fixture_v2.c)
zhl_compile_options(fixture_v2)
target_link_libraries(fixture_v2 PRIVATE zhl)
set_target_properties(fixture_v2 PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

add_library(fixture_bad SHARED fixtures/fixture_bad.c)
zhl_compile_options(fixture_bad)
target_link_libraries(fixture_bad PRIVATE zhl)
set_target_properties(fixture_bad PROPERTIES
    PREFIX "lib"
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
)

# Determine the fixture file extension for the current platform.
if(APPLE)
    set(FIXTURE_EXT "dylib")
elseif(WIN32)
    set(FIXTURE_EXT "dll")
else()
    set(FIXTURE_EXT "so")
endif()

set(FIXTURE_DIR ${CMAKE_BINARY_DIR}/tests)

# Where the fixtures live at RUN time. Android tests execute on-device: the
# adb launcher stages each test's ZCIO_PUSH_FILES beside the binary in zcio's
# device dir, so the baked dlopen paths must point there. Everywhere else —
# host runs, and iOS-simulator runs (the simulator shares the host
# filesystem) — the build-tree paths are the runtime paths.
if(ANDROID AND ZCIO_ANDROID_DEVICE_DIR)
    set(FIXTURE_RUNTIME_DIR ${ZCIO_ANDROID_DEVICE_DIR})
else()
    set(FIXTURE_RUNTIME_DIR ${FIXTURE_DIR})
endif()

# Helper to add a test executable with the fixture paths baked in.
function(zhl_add_test name source)
    add_executable(${name} ${source})
    zhl_compile_options(${name})
    target_link_libraries(${name} PRIVATE zhl cjson)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../zhl/src
    )
    target_compile_definitions(${name} PRIVATE
        FIXTURE_V1_PATH="${FIXTURE_RUNTIME_DIR}/libfixture_v1.${FIXTURE_EXT}"
        FIXTURE_V2_PATH="${FIXTURE_RUNTIME_DIR}/libfixture_v2.${FIXTURE_EXT}"
        FIXTURE_BAD_PATH="${FIXTURE_RUNTIME_DIR}/libfixture_bad.${FIXTURE_EXT}"
    )
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${FIXTURE_DIR})
    if(ANDROID AND CMAKE_CROSSCOMPILING_EMULATOR)
        set_tests_properties(${name} PROPERTIES ENVIRONMENT
            "ZCIO_PUSH_FILES=${FIXTURE_DIR}/libfixture_v1.${FIXTURE_EXT}:${FIXTURE_DIR}/libfixture_v2.${FIXTURE_EXT}:${FIXTURE_DIR}/libfixture_bad.${FIXTURE_EXT}")
    endif()
endfunction()

zhl_add_test(test_version        test_version.c)
zhl_add_test(test_ctx            test_ctx.c)
zhl_add_test(test_update_check   test_update_check.c)
zhl_add_test(test_download       test_download.c)
zhl_add_test(test_dl_abstraction test_dl_abstraction.c)
zhl_add_test(test_hotload_apply  test_hotload_apply.c)
zhl_add_test(test_signed_update  test_signed_update.c)
# The signed-update test hosts an in-process zcio HTTP server (mock zhs) and
# signs artifacts with zcio/crypto.h directly.
target_link_libraries(test_signed_update PRIVATE zcio)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_signed_update PRIVATE Threads::Threads)
endif()

# The --ingest (git-tag/URL) test spawns the REAL zhs executable via
# fork/exec and drives it against a mock GitHub-shaped server, so it needs
# zhs to be built and is POSIX-only (fork/exec; not attempted cross-compiled).
if(TARGET zhs AND NOT WIN32 AND NOT CMAKE_CROSSCOMPILING)
    add_executable(test_ingest test_ingest.c)
    zhl_compile_options(test_ingest)
    target_link_libraries(test_ingest PRIVATE zcio)
    find_package(Threads REQUIRED)
    target_link_libraries(test_ingest PRIVATE Threads::Threads)
    target_compile_definitions(test_ingest PRIVATE ZHS_EXE_PATH="$<TARGET_FILE:zhs>")
    add_test(NAME test_ingest COMMAND test_ingest WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(test_ingest PROPERTIES TIMEOUT 30)
    add_dependencies(test_ingest zhs)

    # --watch-local-git-config: real git repos + a real (tiny) cmake build,
    # spawning zhs the same way test_ingest does. Also needs `git` and
    # `cmake` on PATH -- both are already hard build-time requirements of
    # this whole project, so no separate find_program guard.
    add_executable(test_local_watch test_local_watch.c)
    zhl_compile_options(test_local_watch)
    target_link_libraries(test_local_watch PRIVATE zcio)
    target_compile_definitions(test_local_watch PRIVATE ZHS_EXE_PATH="$<TARGET_FILE:zhs>")
    add_test(NAME test_local_watch COMMAND test_local_watch WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    # Two real cmake configure+build cycles (signed + unsigned watches) plus
    # a third on the incremental-rebuild/second-tag pass -- generous on a
    # loaded CI box.
    set_tests_properties(test_local_watch PROPERTIES TIMEOUT 180)
    add_dependencies(test_local_watch zhs)

    # ?platform= qualified serving + flat-layout fallback: talks real HTTP
    # to a zhs subprocess serving a hand-populated data tree (no --ingest or
    # --watch-local-git-config machinery involved -- this is purely C4's
    # request-routing logic).
    add_executable(test_platform_serving test_platform_serving.c)
    zhl_compile_options(test_platform_serving)
    target_link_libraries(test_platform_serving PRIVATE zcio cjson)
    target_include_directories(test_platform_serving PRIVATE ${cjson_SOURCE_DIR})
    target_compile_definitions(test_platform_serving PRIVATE ZHS_EXE_PATH="$<TARGET_FILE:zhs>")
    add_test(NAME test_platform_serving COMMAND test_platform_serving WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(test_platform_serving PROPERTIES TIMEOUT 30)
    add_dependencies(test_platform_serving zhs)
endif()
