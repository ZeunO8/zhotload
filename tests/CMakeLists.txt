# Test fixtures — shared libraries used to exercise the hotload mechanism.

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

# Helper to add a test executable with the fixture paths baked in.
function(zhl_add_test name source)
    add_executable(${name} ${source})
    zhl_compile_options(${name})
    target_link_libraries(${name} PRIVATE zhl cjson)
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../zhl/src
    )
    target_compile_definitions(${name} PRIVATE
        FIXTURE_V1_PATH="${FIXTURE_DIR}/libfixture_v1.${FIXTURE_EXT}"
        FIXTURE_V2_PATH="${FIXTURE_DIR}/libfixture_v2.${FIXTURE_EXT}"
        FIXTURE_BAD_PATH="${FIXTURE_DIR}/libfixture_bad.${FIXTURE_EXT}"
    )
    add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY ${FIXTURE_DIR})
endfunction()

zhl_add_test(test_version        test_version.c)
zhl_add_test(test_ctx            test_ctx.c)
zhl_add_test(test_update_check   test_update_check.c)
zhl_add_test(test_download       test_download.c)
zhl_add_test(test_dl_abstraction test_dl_abstraction.c)
zhl_add_test(test_hotload_apply  test_hotload_apply.c)
