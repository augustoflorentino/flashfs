# Changelog

Each release lists what changed, what updating means for existing code, and every security fix.

## Unreleased

## 0.2.0 - 2026-06-15

- Reads UBIFS (`MTDF_FORMAT_UBIFS`): the superblock, the newer master node, the committed index, and
  the journal replayed over it (buds named by the log, newest node per key, deletions, renames and
  truncations). Data nodes uncompressed, lzo, zlib or zstd; files of any of them can be read.
  Authenticated images are refused and encrypted files read as `MTDF_ERR_UNSUPPORTED`; extended
  attributes are skipped and counted.
- A UBIFS in a UBI volume opens through `mtdf_open_cb` over the volume as flashfs shows it; the
  README has the callback.
- Optional zstd decoder (`FLASHFS_WITH_ZSTD`, `MTDF_HAVE_ZSTD` in the single-file form,
  `MTDF_FEATURE_ZSTD`), through the library's allocator.
- Detection tries UBI, then UBIFS, cramfs and JFFS2.

Updating: no function changed. `MTDF_FORMAT_UBIFS` is a new accepted value of
`mtdf_options_t.format`, and `mtdf_features` can return the new bit `MTDF_FEATURE_ZSTD`: code that
rejected unknown values of either should accept them.

## 0.1.0 - 2026-05-19

First release.

- Reads JFFS2 (both byte orders; none, zlib, rtime, lzma and lzo nodes; the log replayed by version,
  with deletions, obsolete nodes and holes), cramfs (both byte orders, holes, the `mkcramfs -p` layout,
  extended block pointers stored uncompressed) and UBI (EC and VID headers, the volume table, copies by
  sequence number, static and dynamic volumes, each volume as a file).
- Opens from memory (`mtdf_open_mem`, no copy) or through a read callback (`mtdf_open_cb`, a 128 KiB
  window), with the format detected or given.
- Path lookup with symbolic links, `readdir`, `stat`, `read` at any offset, `readlink`.
- Every size, offset and count from the image is checked before use. Limits on depth, total decoded
  bytes, file size and index records stop decompression bombs and huge tables (`MTDF_ERR_BUDGET`).
- Damage does not stop the reading: what is still readable forms the tree, and `mtdf_report` counts
  what was left out (bad CRCs, broken structure, orphans, loops, duplicate names).
- zlib, liblzma and liblzo2 are optional. Without one, files that need it fail with
  `MTDF_ERR_UNSUPPORTED`; `mtdf_features` says which ones the build has.
- Custom allocator through `mtdf_options_t`.
- Single-file form (`scripts/amalgamate.sh`, `MTDF_IMPLEMENTATION`, `MTDF_STATIC`).
- Examples: `mtdf-ls` lists an image, `mtdf-extract` extracts it into a new directory without leaving it.

Where it differs from the Python reader it was extracted from (fwtriage), and why: `divergencias.txt`.
