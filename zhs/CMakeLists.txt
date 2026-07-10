# zhs — Hotload Update Server
#
# HTTP server library choice: zcio (https://github.com/ZeunO8/zcio)
#
# Rationale:
#   - Already fetched by the top-level build — the zhl client's default HTTP
#     backend is zcio's HTTP/1.1 client, so the server side reuses the same
#     dependency instead of pulling in a second HTTP stack (formerly mongoose).
#   - zcio_http_server is a hardened single-threaded event loop: bounded
#     parsers, slowloris/flood guards, and request-smuggling defenses out of
#     the box.
#   - Works on macOS, Linux, and Windows; zcio links ws2_32/bcrypt itself
#     where needed.

add_executable(zhs src/main.c src/common.c src/local_watch.c)
zhl_compile_options(zhs)

# ${PROJECT_BINARY_DIR}/inc: generated zhl/version.h — zhs reports (and
# self-update compares against) the project version.
target_include_directories(zhs PRIVATE ${PROJECT_BINARY_DIR}/inc)

# cJSON: parses GitHub Releases API responses for --ingest (git-tag/URL
# ingestion) and --watch-local-git-config's config file. Already fetched at
# the top level for zhl's own manifest parsing; the cjson target doesn't
# publish its header dir, so add it explicitly (mirrors zhl/CMakeLists.txt's
# own cJSON.h include path).
target_link_libraries(zhs PRIVATE zcio cjson)
target_include_directories(zhs PRIVATE ${cjson_SOURCE_DIR})

# local_watch.c's background build-poll thread (POSIX only -- see its header
# comment). Threads::Threads resolves to nothing on platforms without a
# pthread implementation instead of failing configure.
find_package(Threads QUIET)
if(Threads_FOUND)
  target_link_libraries(zhs PRIVATE Threads::Threads)
endif()
