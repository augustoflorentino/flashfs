#ifndef FLASHFS_H
#define FLASHFS_H

#include <stddef.h>
#include <stdint.h>

#if defined(MTDF_STATIC) && defined(__GNUC__)
#define MTDF_API static __attribute__((unused))
#elif defined(MTDF_STATIC)
#define MTDF_API static
#elif defined(_WIN32) && defined(MTDF_BUILD_SHARED)
#define MTDF_API __declspec(dllexport)
#elif defined(_WIN32) && defined(MTDF_SHARED)
#define MTDF_API __declspec(dllimport)
#elif defined(__GNUC__) && defined(MTDF_BUILD_SHARED)
#define MTDF_API __attribute__((visibility("default")))
#else
#define MTDF_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MTDF_OK 0
#define MTDF_END 1
#define MTDF_ERR_ARG 2
#define MTDF_ERR_NOMEM 3
#define MTDF_ERR_IO 4
#define MTDF_ERR_FORMAT 5
#define MTDF_ERR_CORRUPT 6
#define MTDF_ERR_UNSUPPORTED 7
#define MTDF_ERR_BUDGET 8
#define MTDF_ERR_NOENT 9
#define MTDF_ERR_NOTDIR 10
#define MTDF_ERR_LOOP 11
#define MTDF_ERR_NAMETOOLONG 12
#define MTDF_ERR_TYPE 13
#define MTDF_ERR_RANGE 14

#define MTDF_FORMAT_AUTO 0
#define MTDF_FORMAT_JFFS2 1
#define MTDF_FORMAT_CRAMFS 2
#define MTDF_FORMAT_UBI 3
#define MTDF_FORMAT_UBIFS 4

#define MTDF_TYPE_OTHER 0
#define MTDF_TYPE_FILE 1
#define MTDF_TYPE_DIR 2
#define MTDF_TYPE_LINK 3
#define MTDF_TYPE_CHR 4
#define MTDF_TYPE_BLK 5
#define MTDF_TYPE_FIFO 6
#define MTDF_TYPE_SOCK 7

#define MTDF_FEATURE_ZLIB 0x1u
#define MTDF_FEATURE_LZMA 0x2u
#define MTDF_FEATURE_LZO 0x4u
#define MTDF_FEATURE_ZSTD 0x8u

#define MTDF_LOOKUP_FOLLOW 0x1u

#define MTDF_ROOT 0u
#define MTDF_PATH_MAX 4096
#define MTDF_SYMLINK_MAX 40

typedef uint32_t mtdf_ino_t;
typedef struct mtdf_fs mtdf_fs_t;

typedef void *(*mtdf_alloc_fn)(void *ctx, size_t size);
typedef void (*mtdf_free_fn)(void *ctx, void *ptr);
typedef int (*mtdf_read_fn)(void *ctx, uint64_t offset, void *buf, size_t buf_len);

typedef struct mtdf_options {
    uint32_t format;
    uint32_t max_depth;
    uint64_t max_total_bytes;
    uint64_t max_file_bytes;
    uint64_t max_entries;
    uint64_t reserved[4];
    mtdf_alloc_fn alloc_fn;
    mtdf_free_fn free_fn;
    void *alloc_ctx;
    void *reserved_ptr;
} mtdf_options_t;

typedef struct mtdf_stat {
    uint64_t size;
    int64_t mtime;
    mtdf_ino_t ino;
    uint32_t type;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint32_t reserved[8];
} mtdf_stat_t;

typedef struct mtdf_dirent {
    const char *name;
    size_t name_len;
    mtdf_ino_t ino;
    uint32_t type;
    uint32_t reserved[2];
} mtdf_dirent_t;

typedef struct mtdf_info {
    uint32_t format;
    uint32_t big_endian;
    uint64_t image_size;
    uint32_t block_size;
    uint32_t unit_size;
    uint64_t entries;
    uint64_t reserved[4];
} mtdf_info_t;

typedef struct mtdf_report {
    uint64_t nodes;
    uint64_t bad_header;
    uint64_t bad_node_crc;
    uint64_t bad_data_crc;
    uint64_t bad_image_crc;
    uint64_t bad_name;
    uint64_t bad_structure;
    uint64_t obsolete;
    uint64_t superseded;
    uint64_t erased_blocks;
    uint64_t orphans;
    uint64_t loops;
    uint64_t duplicates;
    uint64_t unknown;
    uint64_t reserved[8];
} mtdf_report_t;

MTDF_API int mtdf_open_mem(const void *data, size_t data_len, const mtdf_options_t *opts, mtdf_fs_t **out_fs);

MTDF_API int mtdf_open_cb(mtdf_read_fn read_fn, void *read_ctx, uint64_t image_len, const mtdf_options_t *opts,
                          mtdf_fs_t **out_fs);

MTDF_API void mtdf_close(mtdf_fs_t *fs);

MTDF_API int mtdf_lookup(mtdf_fs_t *fs, const char *path, uint32_t flags, mtdf_ino_t *out_ino);

MTDF_API int mtdf_stat(mtdf_fs_t *fs, mtdf_ino_t ino, mtdf_stat_t *out_stat);

MTDF_API int mtdf_readdir(mtdf_fs_t *fs, mtdf_ino_t dir, uint64_t *cookie, mtdf_dirent_t *out_entry);

MTDF_API int mtdf_read(mtdf_fs_t *fs, mtdf_ino_t ino, uint64_t offset, void *buf, size_t buf_len, size_t *out_read);

MTDF_API int mtdf_readlink(mtdf_fs_t *fs, mtdf_ino_t ino, char *buf, size_t buf_len, size_t *out_len);

MTDF_API int mtdf_info(const mtdf_fs_t *fs, mtdf_info_t *out_info);
MTDF_API int mtdf_report(const mtdf_fs_t *fs, mtdf_report_t *out_report);

MTDF_API const char *mtdf_detail(const mtdf_fs_t *fs);

MTDF_API const char *mtdf_format_name(uint32_t format);
MTDF_API unsigned mtdf_features(void);
MTDF_API const char *mtdf_strerror(int status);

#ifdef __cplusplus
}
#endif

#endif
