cmake_minimum_required(VERSION 3.14)
project(zhotload VERSION 0.3.1.2 LANGUAGES C)

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
    ${CMAKE_BINARY_DIR}/inc/zhl/version.h
    @ONLY
)

include(FetchContent)

FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG        v1.7.18
)
FetchContent_MakeAvailable(cjson)

enable_testing()

add_subdirectory(zhl)
add_subdirectory(zhs)
add_subdirectory(tests)
