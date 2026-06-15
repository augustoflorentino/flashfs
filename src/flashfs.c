#include "flashfs_int.h"

#include <stdio.h>
#include <stdlib.h>

typedef char mtdf_options_size_locked[sizeof(mtdf_options_t) == 64 + 4 * sizeof(void *) ? 1 : -1];
typedef char mtdf_stat_size_locked[sizeof(mtdf_stat_t) == 80 ? 1 : -1];
typedef char mtdf_dirent_size_locked[sizeof(mtdf_dirent_t) == (sizeof(void *) == 8 ? 32 : 24) ? 1 : -1];
typedef char mtdf_info_size_locked[sizeof(mtdf_info_t) == 64 ? 1 : -1];
typedef char mtdf_report_size_locked[sizeof(mtdf_report_t) == 176 ? 1 : -1];

static void *mtdf_default_alloc(void *ctx, size_t size)
{
    (void)ctx;
    return malloc(size);
}

static void mtdf_default_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

void *mtdf_alloc(mtdf_fs_t *fs, size_t size)
{
    return fs->alloc_fn(fs->alloc_ctx, size ? size : 1);
}

void mtdf_free(mtdf_fs_t *fs, void *ptr)
{
    if (ptr != NULL)
        fs->free_fn(fs->alloc_ctx, ptr);
}

int mtdf_fail(mtdf_fs_t *fs, int status, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(fs->detail, sizeof(fs->detail), fmt, args);
    va_end(args);
    return status;
}

int mtdf_charge(mtdf_fs_t *fs, uint64_t bytes)
{
    if (bytes > fs->max_total - fs->spent)
        return mtdf_fail(fs, MTDF_ERR_BUDGET, "total byte budget of %llu reached",
                         (unsigned long long)fs->max_total);
    fs->spent += bytes;
    return MTDF_OK;
}

int mtdf_count(mtdf_fs_t *fs, uint64_t records)
{
    if (records > fs->max_entries - fs->records)
        return mtdf_fail(fs, MTDF_ERR_BUDGET, "index budget of %llu records reached",
                         (unsigned long long)fs->max_entries);
    fs->records += records;
    return MTDF_OK;
}

int mtdf_vec_grow(mtdf_fs_t *fs, mtdf_vec_t *vec, size_t elem, size_t extra)
{
    size_t cap;
    void *data;

    if (extra <= vec->cap - vec->len)
        return MTDF_OK;
    if (extra > (SIZE_MAX / elem) - vec->len)
        return MTDF_ERR_NOMEM;
    cap = vec->cap ? vec->cap : 16;
    while (cap < vec->len + extra)
        cap = cap > (SIZE_MAX / elem) / 2 ? vec->len + extra : cap * 2;
    data = mtdf_alloc(fs, cap * elem);
    if (data == NULL)
        return MTDF_ERR_NOMEM;
    if (vec->len > 0)
        memcpy(data, vec->data, vec->len * elem);
    mtdf_free(fs, vec->data);
    vec->data = data;
    vec->cap = cap;
    return MTDF_OK;
}

void mtdf_vec_free(mtdf_fs_t *fs, mtdf_vec_t *vec)
{
    mtdf_free(fs, vec->data);
    vec->data = NULL;
    vec->len = 0;
    vec->cap = 0;
}

int mtdf_unit_buffer(mtdf_fs_t *fs)
{
    if (fs->unit == NULL)
        fs->unit = mtdf_alloc(fs, 2 * MTDF_UNIT_MAX);
    return fs->unit == NULL ? MTDF_ERR_NOMEM : MTDF_OK;
}

static int mtdf_check_options(const mtdf_options_t *opts)
{
    size_t i;

    if (opts == NULL)
        return MTDF_OK;
    if ((opts->alloc_fn == NULL) != (opts->free_fn == NULL) || opts->format > MTDF_FORMAT_UBIFS ||
        opts->reserved_ptr != NULL)
        return MTDF_ERR_ARG;
    for (i = 0; i < sizeof(opts->reserved) / sizeof(opts->reserved[0]); i++) {
        if (opts->reserved[i] != 0)
            return MTDF_ERR_ARG;
    }
    return MTDF_OK;
}

static mtdf_fs_t *mtdf_create(const mtdf_options_t *opts)
{
    mtdf_alloc_fn alloc_fn = mtdf_default_alloc;
    mtdf_free_fn free_fn = mtdf_default_free;
    void *alloc_ctx = NULL;
    mtdf_fs_t *fs;

    if (opts != NULL && opts->alloc_fn != NULL) {
        alloc_fn = opts->alloc_fn;
        free_fn = opts->free_fn;
        alloc_ctx = opts->alloc_ctx;
    }
    fs = alloc_fn(alloc_ctx, sizeof(*fs));
    if (fs == NULL)
        return NULL;
    memset(fs, 0, sizeof(*fs));
    fs->alloc_fn = alloc_fn;
    fs->free_fn = free_fn;
    fs->alloc_ctx = alloc_ctx;
    fs->max_depth = opts && opts->max_depth ? opts->max_depth : MTDF_DEFAULT_DEPTH;
    fs->max_total = opts && opts->max_total_bytes ? opts->max_total_bytes : MTDF_DEFAULT_TOTAL;
    fs->max_file = opts && opts->max_file_bytes ? opts->max_file_bytes : MTDF_DEFAULT_FILE;
    fs->max_entries = opts && opts->max_entries ? opts->max_entries : MTDF_DEFAULT_ENTRIES;
    mtdf_crc_init(fs);
    return fs;
}

typedef int (*mtdf_open_fn)(mtdf_fs_t *fs, int forced);

static int mtdf_detect(mtdf_fs_t *fs, uint32_t format)
{
    static const mtdf_open_fn opens[] = {mtdf_ubi_open, mtdf_ubifs_open, mtdf_cramfs_open, mtdf_jffs2_open};
    static const uint32_t formats[] = {MTDF_FORMAT_UBI, MTDF_FORMAT_UBIFS, MTDF_FORMAT_CRAMFS, MTDF_FORMAT_JFFS2};
    size_t i;
    int status;

    for (i = 0; i < sizeof(opens) / sizeof(opens[0]); i++) {
        if (format != MTDF_FORMAT_AUTO && format != formats[i])
            continue;
        status = opens[i](fs, format != MTDF_FORMAT_AUTO);
        if (fs->io_failed)
            return MTDF_ERR_IO;
        if (status != MTDF_ERR_FORMAT)
            return status;
    }
    return MTDF_ERR_FORMAT;
}

static int mtdf_open(mtdf_fs_t *fs, const mtdf_options_t *opts, mtdf_fs_t **out_fs)
{
    int status = MTDF_OK;

    if (fs->read_fn != NULL) {
        fs->window = mtdf_alloc(fs, MTDF_WINDOW);
        if (fs->window == NULL)
            status = MTDF_ERR_NOMEM;
    }
    if (status == MTDF_OK)
        status = mtdf_tree_init(fs);
    if (status == MTDF_OK)
        status = mtdf_detect(fs, opts ? opts->format : MTDF_FORMAT_AUTO);
    if (status == MTDF_OK)
        status = mtdf_tree_finish(fs);
    if (status != MTDF_OK) {
        mtdf_close(fs);
        return status;
    }
    fs->info.entries = mtdf_entry_count(fs);
    fs->spent = 0;
    fs->detail[0] = '\0';
    *out_fs = fs;
    return MTDF_OK;
}

int mtdf_open_mem(const void *data, size_t data_len, const mtdf_options_t *opts, mtdf_fs_t **out_fs)
{
    mtdf_fs_t *fs;

    if (out_fs == NULL)
        return MTDF_ERR_ARG;
    *out_fs = NULL;
    if ((data == NULL && data_len > 0) || mtdf_check_options(opts) != MTDF_OK)
        return MTDF_ERR_ARG;
    fs = mtdf_create(opts);
    if (fs == NULL)
        return MTDF_ERR_NOMEM;
    fs->mem = data;
    fs->len = data_len;
    return mtdf_open(fs, opts, out_fs);
}

int mtdf_open_cb(mtdf_read_fn read_fn, void *read_ctx, uint64_t image_len, const mtdf_options_t *opts,
                 mtdf_fs_t **out_fs)
{
    mtdf_fs_t *fs;

    if (out_fs == NULL)
        return MTDF_ERR_ARG;
    *out_fs = NULL;
    if (read_fn == NULL || mtdf_check_options(opts) != MTDF_OK)
        return MTDF_ERR_ARG;
    fs = mtdf_create(opts);
    if (fs == NULL)
        return MTDF_ERR_NOMEM;
    fs->read_fn = read_fn;
    fs->read_ctx = read_ctx;
    fs->len = image_len;
    return mtdf_open(fs, opts, out_fs);
}

void mtdf_close(mtdf_fs_t *fs)
{
    if (fs == NULL)
        return;
    if (fs->driver != NULL)
        fs->driver->destroy(fs);
    mtdf_decoders_free(fs);
    mtdf_vec_free(fs, &fs->entries);
    mtdf_vec_free(fs, &fs->names);
    mtdf_free(fs, fs->children);
    mtdf_free(fs, fs->unit);
    mtdf_free(fs, fs->window);
    fs->free_fn(fs->alloc_ctx, fs);
}

static int mtdf_get_entry(mtdf_fs_t *fs, mtdf_ino_t ino, mtdf_entry_t **out)
{
    if (fs == NULL || ino >= mtdf_entry_count(fs))
        return MTDF_ERR_ARG;
    *out = mtdf_entry(fs, ino);
    fs->detail[0] = '\0';
    return MTDF_OK;
}

int mtdf_stat(mtdf_fs_t *fs, mtdf_ino_t ino, mtdf_stat_t *out_stat)
{
    mtdf_entry_t *entry;

    if (out_stat == NULL || mtdf_get_entry(fs, ino, &entry) != MTDF_OK)
        return MTDF_ERR_ARG;
    memset(out_stat, 0, sizeof(*out_stat));
    out_stat->size = entry->size;
    out_stat->mtime = entry->mtime;
    out_stat->ino = ino;
    out_stat->type = mtdf_type_of(entry->mode);
    out_stat->mode = entry->mode;
    out_stat->uid = entry->uid;
    out_stat->gid = entry->gid;
    out_stat->nlink = entry->nlink;
    out_stat->dev_major = entry->dev_major;
    out_stat->dev_minor = entry->dev_minor;
    return MTDF_OK;
}

static int mtdf_read_entry(mtdf_fs_t *fs, const mtdf_entry_t *entry, uint64_t offset, uint8_t *buf, size_t len)
{
    int status;

    if (entry->size > fs->max_file)
        return mtdf_fail(fs, MTDF_ERR_BUDGET, "file of %llu bytes exceeds the per-file budget of %llu",
                         (unsigned long long)entry->size, (unsigned long long)fs->max_file);
    status = mtdf_charge(fs, len);
    if (status != MTDF_OK || len == 0)
        return status;
    fs->io_failed = 0;
    status = fs->driver->read(fs, entry, offset, buf, len);
    return fs->io_failed ? MTDF_ERR_IO : status;
}

int mtdf_read(mtdf_fs_t *fs, mtdf_ino_t ino, uint64_t offset, void *buf, size_t buf_len, size_t *out_read)
{
    mtdf_entry_t *entry;
    uint64_t left;
    size_t len;
    int status;

    if (out_read == NULL || (buf == NULL && buf_len > 0) || mtdf_get_entry(fs, ino, &entry) != MTDF_OK)
        return MTDF_ERR_ARG;
    *out_read = 0;
    if ((entry->mode & MTDF_S_IFMT) != MTDF_S_IFREG)
        return MTDF_ERR_TYPE;
    if (offset >= entry->size)
        return mtdf_read_entry(fs, entry, 0, NULL, 0);
    left = entry->size - offset;
    len = left < buf_len ? (size_t)left : buf_len;
    status = mtdf_read_entry(fs, entry, offset, buf, len);
    if (status == MTDF_OK)
        *out_read = len;
    return status;
}

int mtdf_readlink(mtdf_fs_t *fs, mtdf_ino_t ino, char *buf, size_t buf_len, size_t *out_len)
{
    mtdf_entry_t *entry;
    int status;

    if (out_len == NULL || (buf == NULL && buf_len > 0) || mtdf_get_entry(fs, ino, &entry) != MTDF_OK)
        return MTDF_ERR_ARG;
    *out_len = 0;
    if ((entry->mode & MTDF_S_IFMT) != MTDF_S_IFLNK)
        return MTDF_ERR_TYPE;
    if (entry->size > MTDF_PATH_MAX)
        return mtdf_fail(fs, MTDF_ERR_NAMETOOLONG, "link target of %llu bytes", (unsigned long long)entry->size);
    *out_len = (size_t)entry->size;
    if (buf_len <= *out_len)
        return MTDF_ERR_RANGE;
    status = mtdf_read_entry(fs, entry, 0, (uint8_t *)buf, *out_len);
    if (status != MTDF_OK) {
        *out_len = 0;
        buf[0] = '\0';
        return status;
    }
    *out_len = mtdf_strnlen(buf, *out_len);
    buf[*out_len] = '\0';
    return MTDF_OK;
}

int mtdf_info(const mtdf_fs_t *fs, mtdf_info_t *out_info)
{
    if (fs == NULL || out_info == NULL)
        return MTDF_ERR_ARG;
    *out_info = fs->info;
    return MTDF_OK;
}

int mtdf_report(const mtdf_fs_t *fs, mtdf_report_t *out_report)
{
    if (fs == NULL || out_report == NULL)
        return MTDF_ERR_ARG;
    *out_report = fs->report;
    return MTDF_OK;
}

const char *mtdf_detail(const mtdf_fs_t *fs)
{
    return fs == NULL ? "" : fs->detail;
}

const char *mtdf_format_name(uint32_t format)
{
    switch (format) {
    case MTDF_FORMAT_JFFS2:
        return "jffs2";
    case MTDF_FORMAT_CRAMFS:
        return "cramfs";
    case MTDF_FORMAT_UBI:
        return "ubi";
    case MTDF_FORMAT_UBIFS:
        return "ubifs";
    default:
        return "unknown";
    }
}

unsigned mtdf_features(void)
{
    unsigned features = 0;

#ifdef MTDF_HAVE_ZLIB
    features |= MTDF_FEATURE_ZLIB;
#endif
#ifdef MTDF_HAVE_LZMA
    features |= MTDF_FEATURE_LZMA;
#endif
#ifdef MTDF_HAVE_LZO
    features |= MTDF_FEATURE_LZO;
#endif
#ifdef MTDF_HAVE_ZSTD
    features |= MTDF_FEATURE_ZSTD;
#endif
    return features;
}

const char *mtdf_strerror(int status)
{
    static const char *const messages[] = {
        "ok",
        "end of directory",
        "invalid argument",
        "out of memory",
        "read callback failed",
        "not an image of the requested format",
        "image damaged",
        "not supported by this build",
        "budget exceeded",
        "no such file or directory",
        "not a directory",
        "too many symbolic links",
        "path too long",
        "wrong file type",
        "buffer too small",
    };

    if (status < 0 || (size_t)status >= sizeof(messages) / sizeof(messages[0]))
        return "unknown status";
    return messages[status];
}
