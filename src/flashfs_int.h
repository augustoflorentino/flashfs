#ifndef FLASHFS_INT_H
#define FLASHFS_INT_H

#include "flashfs/flashfs.h"

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#if defined(MTDF_STATIC) && defined(__GNUC__)
#define MTDF_INTERN static __attribute__((unused))
#elif defined(MTDF_STATIC)
#define MTDF_INTERN static
#else
#define MTDF_INTERN
#endif

#define MTDF_DEFAULT_DEPTH 64u
#define MTDF_DEFAULT_TOTAL (2ull << 30)
#define MTDF_DEFAULT_FILE (256ull << 20)
#define MTDF_DEFAULT_ENTRIES 1000000ull

#define MTDF_WINDOW (128u * 1024u)
#define MTDF_SMALL (64u * 1024u)

#define MTDF_UNIT_MAX (128u * 1024u)

#define MTDF_S_IFMT 0170000u
#define MTDF_S_IFSOCK 0140000u
#define MTDF_S_IFLNK 0120000u
#define MTDF_S_IFREG 0100000u
#define MTDF_S_IFBLK 0060000u
#define MTDF_S_IFDIR 0040000u
#define MTDF_S_IFCHR 0020000u
#define MTDF_S_IFIFO 0010000u

#define MTDF_NONE 0xffffffffu

typedef struct mtdf_entry {
    uint64_t size;
    uint64_t ref;
    int64_t mtime;
    uint32_t parent;
    uint32_t name_off;
    uint32_t name_len;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint32_t dev_major;
    uint32_t dev_minor;
    uint32_t first_child;
    uint32_t child_count;
    uint32_t linked;
} mtdf_entry_t;

typedef struct mtdf_vec {
    void *data;
    size_t len;
    size_t cap;
} mtdf_vec_t;

typedef struct mtdf_driver {
    int (*read)(mtdf_fs_t *fs, const mtdf_entry_t *entry, uint64_t offset, uint8_t *buf, size_t len);
    void (*destroy)(mtdf_fs_t *fs);
} mtdf_driver_t;

struct mtdf_fs {
    mtdf_alloc_fn alloc_fn;
    mtdf_free_fn free_fn;
    void *alloc_ctx;

    const uint8_t *mem;
    mtdf_read_fn read_fn;
    void *read_ctx;
    uint64_t len;
    uint8_t *window;
    uint64_t window_off;
    size_t window_len;
    int io_failed;

    uint64_t max_total;
    uint64_t max_file;
    uint64_t max_entries;
    uint32_t max_depth;
    uint64_t spent;
    uint64_t records;

    mtdf_vec_t entries;
    mtdf_vec_t names;
    uint32_t *children;

    const mtdf_driver_t *driver;
    void *state;

    uint8_t *unit;
    uint64_t unit_key;
    uint64_t unit_key2;
    size_t unit_len;
    int unit_valid;

    void *zlib;
    void *lzma;
    void *zstd;

    mtdf_info_t info;
    mtdf_report_t report;
    uint32_t crc_table[8][256];
    char path[2][MTDF_PATH_MAX + 1];
    char detail[200];
};

MTDF_INTERN void *mtdf_alloc(mtdf_fs_t *fs, size_t size);
MTDF_INTERN void mtdf_free(mtdf_fs_t *fs, void *ptr);
MTDF_INTERN int mtdf_fail(mtdf_fs_t *fs, int status, const char *fmt, ...);
MTDF_INTERN int mtdf_charge(mtdf_fs_t *fs, uint64_t bytes);
MTDF_INTERN int mtdf_count(mtdf_fs_t *fs, uint64_t records);
MTDF_INTERN int mtdf_vec_grow(mtdf_fs_t *fs, mtdf_vec_t *vec, size_t elem, size_t extra);
MTDF_INTERN void mtdf_vec_free(mtdf_fs_t *fs, mtdf_vec_t *vec);
MTDF_INTERN int mtdf_unit_buffer(mtdf_fs_t *fs);

MTDF_INTERN int mtdf_src_get(mtdf_fs_t *fs, uint64_t offset, size_t len, uint8_t *scratch, const uint8_t **out);
MTDF_INTERN int mtdf_src_crc(mtdf_fs_t *fs, uint64_t offset, uint64_t len, uint32_t crc, uint32_t *out_crc);
MTDF_INTERN void mtdf_crc_init(mtdf_fs_t *fs);
MTDF_INTERN uint32_t mtdf_crc(const mtdf_fs_t *fs, uint32_t crc, const uint8_t *data, size_t len);
MTDF_INTERN int mtdf_all_ff(mtdf_fs_t *fs, uint64_t offset, uint64_t len, int *out_erased);

static inline uint32_t mtdf_le16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static inline uint32_t mtdf_be16(const uint8_t *p)
{
    return (uint32_t)p[0] << 8 | (uint32_t)p[1];
}

static inline uint32_t mtdf_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t mtdf_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

static inline uint64_t mtdf_le64(const uint8_t *p)
{
    return (uint64_t)mtdf_le32(p + 4) << 32 | mtdf_le32(p);
}

static inline uint64_t mtdf_be64(const uint8_t *p)
{
    return (uint64_t)mtdf_be32(p) << 32 | mtdf_be32(p + 4);
}

static inline uint32_t mtdf_get16(const uint8_t *p, int big)
{
    return big ? mtdf_be16(p) : mtdf_le16(p);
}

static inline uint32_t mtdf_get32(const uint8_t *p, int big)
{
    return big ? mtdf_be32(p) : mtdf_le32(p);
}

static inline size_t mtdf_strnlen(const char *s, size_t max)
{
    const char *nul = memchr(s, 0, max);

    return nul == NULL ? max : (size_t)(nul - s);
}

typedef int (*mtdf_cmp_fn)(const void *ctx, const void *a, const void *b);
MTDF_INTERN void mtdf_sort(void *items, void *tmp, size_t count, size_t size, mtdf_cmp_fn cmp, const void *ctx);
MTDF_INTERN int mtdf_tree_init(mtdf_fs_t *fs);
MTDF_INTERN int mtdf_tree_add(mtdf_fs_t *fs, const char *name, size_t name_len, uint32_t *out_index);
MTDF_INTERN int mtdf_tree_finish(mtdf_fs_t *fs);
MTDF_INTERN int mtdf_name_ok(const char *name, size_t name_len);
MTDF_INTERN uint32_t mtdf_type_of(uint32_t mode);

static inline mtdf_entry_t *mtdf_entry(mtdf_fs_t *fs, uint32_t index)
{
    return (mtdf_entry_t *)fs->entries.data + index;
}

static inline uint32_t mtdf_entry_count(const mtdf_fs_t *fs)
{
    return (uint32_t)fs->entries.len;
}

MTDF_INTERN int mtdf_inflate(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len, int raw);
MTDF_INTERN int mtdf_unlzma_jffs2(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
MTDF_INTERN int mtdf_unlzo(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
MTDF_INTERN int mtdf_unzstd(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
MTDF_INTERN int mtdf_unrtime(mtdf_fs_t *fs, const uint8_t *in, size_t in_len, uint8_t *out, size_t out_len);
MTDF_INTERN void mtdf_decoders_free(mtdf_fs_t *fs);

MTDF_INTERN int mtdf_jffs2_open(mtdf_fs_t *fs, int forced);
MTDF_INTERN int mtdf_cramfs_open(mtdf_fs_t *fs, int forced);
MTDF_INTERN int mtdf_ubi_open(mtdf_fs_t *fs, int forced);
MTDF_INTERN int mtdf_ubifs_open(mtdf_fs_t *fs, int forced);

#endif
