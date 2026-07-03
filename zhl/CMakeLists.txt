find_package(CURL REQUIRED)

set(ZHL_SOURCES
    src/zhl_ctx.c
    src/zhl_version.c
    src/zhl_update.c
    src/zhl_hotload.c
    src/zhl_http_curl.c
)

if(WIN32)
    list(APPEND ZHL_SOURCES src/zhl_dl_win32.c)
else()
    list(APPEND ZHL_SOURCES src/zhl_dl_posix.c)
endif()

add_library(zhl STATIC ${ZHL_SOURCES})

target_include_directories(zhl
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/inc
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(zhl
    PRIVATE
        CURL::libcurl
        cjson
)

target_include_directories(zhl
    PRIVATE
        ${cjson_SOURCE_DIR}
)

if(UNIX AND NOT APPLE)
    target_link_libraries(zhl PRIVATE dl)
elseif(APPLE)
    target_link_libraries(zhl PRIVATE ${CMAKE_DL_LIBS})
endif()
