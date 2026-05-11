# flashfs

Read-only reader for embedded flash filesystems: JFFS2, cramfs and UBI, in C99, without mounting
and without root.

Images come from firmware someone else built, so the library treats them as hostile: every size,
offset and count is checked against the bytes present and against a budget before it is used, and
a damaged image still yields what can be read, with a report of what could not.

## Build and install

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build
cmake --install build --prefix /usr/local
```

zlib, liblzma and liblzo2 are used when found (`FLASHFS_WITH_ZLIB`, `FLASHFS_WITH_LZMA`,
`FLASHFS_WITH_LZO`). Without one, the files that need it read as `MTDF_ERR_UNSUPPORTED`;
`mtdf_features()` says which decoders the build has.

Use it from CMake:

```cmake
find_package(flashfs REQUIRED)
target_link_libraries(app PRIVATE flashfs::flashfs)
```

Or with pkg-config (`--static` for the static library):

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

Define `MTDF_STATIC` as well to keep every function private to that file, and `MTDF_HAVE_ZLIB`,
`MTDF_HAVE_LZMA`, `MTDF_HAVE_LZO` for each decoder you link.

## Usage

```c
mtdf_fs_t *fs;
mtdf_ino_t ino;
mtdf_stat_t st;
char buf[4096];
size_t got;

if (mtdf_open_mem(image, image_len, NULL, &fs) != MTDF_OK)
    return;
if (mtdf_lookup(fs, "/etc/passwd", MTDF_LOOKUP_FOLLOW, &ino) == MTDF_OK &&
    mtdf_stat(fs, ino, &st) == MTDF_OK &&
    mtdf_read(fs, ino, 0, buf, sizeof(buf), &got) == MTDF_OK)
    fwrite(buf, 1, got, stdout);
mtdf_close(fs);
```

[`examples/mtdf-ls.c`](examples/mtdf-ls.c) lists an image read through a callback;
[`examples/mtdf-extract.c`](examples/mtdf-extract.c) extracts it into a new directory without
writing outside it. The header `include/flashfs/flashfs.h` is the API reference.

Conventions:

- every function that can fail returns `MTDF_OK` or an error code; `mtdf_strerror` names it
- `mtdf_open_mem` does not copy the image: it must outlive the handle; `mtdf_open_cb` reads through
  your callback instead, a window at a time
- names and pointers the library returns belong to the handle and live until `mtdf_close`
- `mtdf_options_t`, zero-initialized, takes the limits (depth, decoded bytes, file size, index
  records) and an allocator; `NULL` means the defaults and `malloc`/`free`
- `mtdf_report` counts what opening found damaged and left out; nothing is hidden
- a handle is not thread-safe: one per thread, or a lock around every call

## Performance

PERF

## Versioning

Semantic Versioning. Compatible releases bump the minor version; incompatible ones bump the
major version and the soname. See [CHANGELOG.md](CHANGELOG.md).

## License

MIT, see [LICENSE](LICENSE).
