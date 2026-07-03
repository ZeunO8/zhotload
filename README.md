# zhotload

A hot-reload system for shared libraries. A running application can download and swap in updated function implementations from a remote update server without restarting or losing state in unaffected functions.

## Project Layout

```
zhotload/
├── CMakeLists.txt          Root build configuration
├── version.h.in            Template for generated version header
├── zhl/                    Client-side hotload library
│   ├── include/zhl/zhl.h   Public API header
│   └── src/                Implementation + platform abstraction
├── zhs/                    Update server (standalone executable)
│   └── src/main.c          HTTP server serving versioned artifacts
└── tests/                  Test suite + fixture shared libraries
    ├── fixtures/            v1, v2, and bad-manifest test libraries
    └── test_*.c             One test file per logical area
```

## Prerequisites

- CMake >= 3.14
- C11 compiler (GCC, Clang, MSVC)
- libcurl (for `zhl` HTTP client)

## Build

```sh
cmake -B build
cmake --build build
ctest --test-dir build
```

## Quick-Start Example

```c
#include <zhl/zhl.h>
#include <stdio.h>

typedef int (*binary_op)(int, int);

static binary_op op_add;
static binary_op op_multiply;

int main(void)
{
    zhl_ctx_t ctx;
    zhl_ctx_create(&ctx);

    zhl_ctx_set_server_url(ctx, "http://localhost:8080");
    zhl_ctx_set_app_name(ctx, "myapp");
    zhl_ctx_set_current_version(ctx, "1.0.0");
    zhl_ctx_set_current_lib_path(ctx, "./libmyapp.so");
    zhl_ctx_set_staging_dir(ctx, "/tmp/zhl_staging");

    zhl_func_binding_t bindings[] = {
        { "add",      (void **)&op_add },
        { "multiply", (void **)&op_multiply },
    };
    zhl_register_bindings(ctx, bindings, 2);

    printf("add(2,3) = %d\n", op_add(2, 3));

    zhl_update_info_t info;
    if (zhl_check_for_update(ctx, &info) == ZHL_UPDATE_AVAILABLE) {
        zhl_download_update(ctx, &info);
        zhl_hotload_apply(ctx);
        printf("After hotload: multiply(2,3) = %d\n", op_multiply(2, 3));
    }

    zhl_ctx_destroy(&ctx);
    return 0;
}
```

## Shared Library Manifest

Every hotloadable shared library must export a `zhl_export_table` symbol:

```c
#include <zhl/zhl.h>

int my_add(int a, int b) { return a + b; }

static const zhl_export_entry_t entries[] = {
    { "my_add", "1.0.0", (void *)my_add },
};

const zhl_export_table_t zhl_export_table = {
    ZHL_EXPORT_MAGIC,
    1,
    entries,
};
```

## Running the Update Server

```sh
./build/zhs/zhs --data ./data --port 8080
```

See `zhs/README.md` for the data directory layout and how to register applications.

## Dependencies

| Library  | Used by | Fetch method  | Purpose                        |
|----------|---------|---------------|--------------------------------|
| libcurl  | zhl     | `find_package`| HTTP client for update checks  |
| cJSON    | zhl,zhs| `FetchContent`| JSON parsing/generation        |
| mongoose | zhs     | `FetchContent`| Embeddable HTTP server         |
