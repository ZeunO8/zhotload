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
