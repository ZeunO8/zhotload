# zhs — Hotload Update Server
#
# HTTP server library choice: mongoose (https://github.com/cesanta/mongoose)
#
# Rationale:
#   - Single-file C library (mongoose.c + mongoose.h) — trivially embeddable.
#   - Works on macOS, Linux, and Windows with no additional dependencies.
#   - MIT licensed.
#   - Well-maintained by Cesanta with active CI across all three platforms.
#
# Platform-specific notes:
#   - No special flags needed on macOS or Linux.
#   - On Windows, mongoose uses Winsock; link ws2_32.

FetchContent_Declare(
    mongoose
    GIT_REPOSITORY https://github.com/cesanta/mongoose.git
    GIT_TAG        7.14
)
FetchContent_MakeAvailable(mongoose)

add_library(mongoose_lib STATIC
    ${mongoose_SOURCE_DIR}/mongoose.c
)
target_include_directories(mongoose_lib PUBLIC ${mongoose_SOURCE_DIR})

if(WIN32)
    target_link_libraries(mongoose_lib PUBLIC ws2_32)
endif()

add_executable(zhs src/main.c)

target_link_libraries(zhs PRIVATE mongoose_lib cjson)

target_include_directories(zhs PRIVATE
    ${CMAKE_SOURCE_DIR}/zhl/include
    ${CMAKE_BINARY_DIR}/inc
    ${cjson_SOURCE_DIR}
)
