# flashfs

Read-only reader for embedded flash filesystems: JFFS2, cramfs and UBI

## Build and install

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build
cmake --install build --prefix /usr/local
```

Use it from CMake:

```cmake
find_package(flashfs REQUIRED)
target_link_libraries(app PRIVATE flashfs::flashfs)
```

Or with pkg-config:

```sh
cc app.c $(pkg-config --cflags --libs flashfs)
```

## Single-file form

`scripts/amalgamate.sh` writes `dist/flashfs/flashfs.h`. Copy it into your project and, in exactly
one `.c` file:

```c
#define MTDF_IMPLEMENTATION
#include "flashfs/flashfs.h"
```

Define `MTDF_STATIC` as well to keep every function private to that file.

## Usage

See [`examples/basic.c`](examples/basic.c). The header `include/flashfs/flashfs.h` is the
API reference.

Conventions:

- every function that can fail returns `MTDF_OK` or an error code; `mtdf_strerror` describes it
- what `mtdf_x_create` returns is released only by `mtdf_x_destroy`, which accepts `NULL`
- functions never take ownership of their arguments; returned pointers belong to the library
- pass your own allocator to `mtdf_buf_create`, or `NULL` for `malloc`/`free`

## Versioning

Semantic Versioning. Compatible releases bump the minor version; incompatible ones bump the
major version and the soname. See [CHANGELOG.md](CHANGELOG.md).

## License

MIT, see [LICENSE](LICENSE).
