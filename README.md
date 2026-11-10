# flashfs

Reads the filesystems routers and other small devices keep on raw flash: **JFFS2, cramfs, UBI and
UBIFS**, from C99, with no mount, no root and no kernel module. Give it the bytes of an image, or
a callback that reads them, and list, stat, read and extract its files.

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/images/flash-dark.svg">
  <img src="docs/images/flash-light.svg" alt="Three public OpenWrt images: two JFFS2 root filesystems holding twice their size in files, and a UBI image whose three volumes flashfs opens as files" width="1000">
</picture>

The images in the picture are the ones `scripts/realworld.sh` downloads and checks on every
release, pinned by SHA-256.

## Hostile input

Firmware is built by someone else and often damaged on the way out, so every size, offset and
count in an image is checked against the bytes present and against a budget before it is used:
decoded bytes, directory depth, file size and index records all have limits in `mtdf_options_t`.
A damaged image still gives what can be read, and `mtdf_report` counts what was left out.
`fuzz/` holds one target per format, and `tests/test_robust.c` covers truncation, decompression
bombs, directory loops and failing allocations.

## Reading files

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

A UBIFS usually sits inside a UBI volume. Each volume is a file of the UBI handle, and
`mtdf_open_file` opens the filesystem stored in it:

```c
mtdf_fs_t *ubi, *rootfs;
mtdf_ino_t vol;

if (mtdf_open_mem(image, image_len, NULL, &ubi) == MTDF_OK &&
    mtdf_lookup(ubi, "/rootfs", 0, &vol) == MTDF_OK &&
    mtdf_open_file(ubi, vol, NULL, &rootfs) == MTDF_OK) {
    list_tree(rootfs);
    mtdf_close(rootfs);
}
mtdf_close(ubi);
```

Close the inner handle before the outer one, which it reads from. Extended attributes of JFFS2
and UBIFS are listed like a directory and read by name:

```c
mtdf_xattr_t x;
uint64_t cookie = 0;
unsigned char value[256];
size_t len;

while (mtdf_listxattr(fs, ino, &cookie, &x) == MTDF_OK)
    if (mtdf_getxattr(fs, ino, x.name, value, sizeof(value), &len) == MTDF_OK)
        printf("%s: %zu bytes\n", x.name, len);
```

Two complete programs come with it: [`examples/mtdf-ls.c`](examples/mtdf-ls.c) lists an image
read through a callback, and [`examples/mtdf-extract.c`](examples/mtdf-extract.c) unpacks one
into a new directory and refuses any path that would land outside it.

- every function that can fail returns `MTDF_OK` or an error code; `mtdf_strerror` names it
- `mtdf_open_mem` does not copy the image: it must outlive the handle; `mtdf_open_cb` reads through
  your callback instead, a window at a time
- names and pointers the library returns belong to the handle and live until `mtdf_close`
- `mtdf_options_t`, zero-initialized, takes the limits (depth, decoded bytes, file size, index
  records) and an allocator; `NULL` means the defaults and `malloc`/`free`
- `mtdf_report` counts what opening found damaged and left out; nothing is hidden
- a handle is not thread-safe: one per thread, or a lock around every call

## Speed

| image | open + list | read everything | heap peak while listing | fwtriage (Python) |
|---|---|---|---|---|
| JFFS2, 16 MiB, zlib, 1770 entries | 7.4 ms | 79 ms (318 MB/s) | 5.7% of the image | 178 ms |
| cramfs, 14.5 MB, 1770 entries | 5.7 ms | 82 ms (305 MB/s) | 1.7% | 114 ms |
| UBI, 128 MiB, one volume | 0.18 ms | 5 ms | 0.12% | 260 ms |
| fwtriage's test corpus, 874 images | | 10.6 ms in all | | 187 ms in all |

`bench/run.sh` on one machine, release build, images in memory, median of 7 runs. Opening only
builds an index; reading a zlib image is bound by zlib, which alone takes 86 ms for the 6247
nodes of the JFFS2 image (`bench/zlib_floor.py`). The Python column is the fwtriage reader
flashfs was extracted from.

## Building

```sh
cmake -S . -B build -DBUILD_SHARED_LIBS=ON
cmake --build build
cmake --install build --prefix /usr/local
```

zlib, liblzma, liblzo2 and libzstd are optional (`FLASHFS_WITH_ZLIB`, `FLASHFS_WITH_LZMA`,
`FLASHFS_WITH_LZO`, `FLASHFS_WITH_ZSTD`); a file that needs a missing decoder reads as
`MTDF_ERR_UNSUPPORTED`, and `mtdf_features()` tells which ones the build has. CMake users get
`find_package(flashfs)` and `flashfs::flashfs`, everyone else `pkg-config flashfs` (`--static` for
the static library).

`scripts/amalgamate.sh` writes the single header `dist/flashfs/flashfs.h`. One `.c` file defines
`MTDF_IMPLEMENTATION` before including it; `MTDF_STATIC` keeps the functions private there, and
`MTDF_HAVE_ZLIB`, `MTDF_HAVE_LZMA`, `MTDF_HAVE_LZO`, `MTDF_HAVE_ZSTD` turn on each decoder linked.

## Status

0.x, under Semantic Versioning, until a 1.0 can be checked where it ships: CI on every platform,
an ABI baseline, distribution packages. Changes per release are in the [CHANGELOG](CHANGELOG.md),
security reports go through [SECURITY.md](SECURITY.md), and the license is MIT.
