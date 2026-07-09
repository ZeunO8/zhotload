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

add_executable(zhs src/main.c)
zhl_compile_options(zhs)

# ${PROJECT_BINARY_DIR}/inc: generated zhl/version.h — zhs reports (and
# self-update compares against) the project version.
target_include_directories(zhs PRIVATE ${PROJECT_BINARY_DIR}/inc)

target_link_libraries(zhs PRIVATE zcio)
