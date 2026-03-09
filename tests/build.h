#ifndef FLASHFS_TEST_BUILD_H
#define FLASHFS_TEST_BUILD_H

#include <flashfs/flashfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__GNUC__)
#define TEST_UNUSED __attribute__((unused))
#else
#define TEST_UNUSED
#endif

static int test_failures;

#define CHECK(cond)                                                                                              \
    do {                                                                                                         \
        if (!(cond)) {                                                                                           \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                                      \
            test_failures++;                                                                                     \
        }                                                                                                        \
    } while (0)

#define CHECK_EQ(got, want)                                                                                      \
    do {                                                                                                         \
        long long got_ = (long long)(got);                                                                       \
        long long want_ = (long long)(want);                                                                     \
        if (got_ != want_) {                                                                                     \
            printf("%s:%d: %s is %lld, expected %lld\n", __FILE__, __LINE__, #got, got_, want_);                 \
            test_failures++;                                                                                     \
        }                                                                                                        \
    } while (0)

typedef struct buf {
    unsigned char *data;
    size_t len;
    size_t cap;
} buf_t;

TEST_UNUSED static void buf_reserve(buf_t *b, size_t extra)
{
    size_t cap = b->cap ? b->cap : 4096;

    while (cap - b->len < extra)
        cap *= 2;
    if (cap != b->cap) {
        b->data = realloc(b->data, cap);
        if (b->data == NULL)
            abort();
        b->cap = cap;
    }
}

TEST_UNUSED static void buf_put(buf_t *b, const void *data, size_t len)
{
    buf_reserve(b, len);
    memcpy(b->data + b->len, data, len);
    b->len += len;
}

TEST_UNUSED static void buf_fill(buf_t *b, int byte, size_t len)
{
    buf_reserve(b, len);
    memset(b->data + b->len, byte, len);
    b->len += len;
}

TEST_UNUSED static void buf_free(buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

TEST_UNUSED static int buf_load(buf_t *b, const char *dir, const char *name)
{
    char path[4096];
    unsigned char chunk[65536];
    size_t got;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("cannot open %s\n", path);
        test_failures++;
        return -1;
    }
    b->len = 0;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
        buf_put(b, chunk, got);
    fclose(f);
    return 0;
}

#define JB_DIRENT 0xe001u
#define JB_INODE 0xe002u
#define JB_CLEANMARKER 0x2003u
#define JB_DT_DIR 4u
#define JB_DT_REG 8u
#define JB_DT_LNK 10u

TEST_UNUSED static uint32_t jb_crc(const void *data, size_t len)
{
    const unsigned char *p = data;
    uint32_t crc = 0;
    int k;

    while (len-- > 0) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = crc & 1 ? (crc >> 1) ^ 0xedb88320u : crc >> 1;
    }
    return crc;
}

TEST_UNUSED static void jb_put16(unsigned char *p, uint32_t v, int big)
{
    p[big ? 0 : 1] = (unsigned char)(v >> 8);
    p[big ? 1 : 0] = (unsigned char)v;
}

TEST_UNUSED static void jb_put32(unsigned char *p, uint32_t v, int big)
{
    int i;

    for (i = 0; i < 4; i++)
        p[big ? 3 - i : i] = (unsigned char)(v >> (8 * i));
}

TEST_UNUSED static void jb_header(unsigned char *p, uint32_t nodetype, uint32_t totlen, int big)
{
    jb_put16(p, 0x1985, big);
    jb_put16(p + 2, nodetype, big);
    jb_put32(p + 4, totlen, big);
    jb_put32(p + 8, jb_crc(p, 8), big);
}

TEST_UNUSED static void jb_pad(buf_t *b)
{
    while (b->len % 4 != 0)
        buf_fill(b, 0xff, 1);
}

TEST_UNUSED static void jb_cleanmarker(buf_t *b, int big)
{
    unsigned char node[12];

    jb_header(node, JB_CLEANMARKER, 12, big);
    buf_put(b, node, 12);
}

TEST_UNUSED static size_t jb_dirent(buf_t *b, int big, uint32_t pino, uint32_t ino, uint32_t version, const char *name,
                        unsigned dtype)
{
    unsigned char node[40];
    size_t len = strlen(name);
    size_t at = b->len;

    memset(node, 0, sizeof(node));
    jb_header(node, JB_DIRENT, (uint32_t)(40 + len), big);
    jb_put32(node + 12, pino, big);
    jb_put32(node + 16, version, big);
    jb_put32(node + 20, ino, big);
    node[28] = (unsigned char)len;
    node[29] = (unsigned char)dtype;
    jb_put32(node + 32, jb_crc(node, 32), big);
    jb_put32(node + 36, jb_crc(name, len), big);
    buf_put(b, node, 40);
    buf_put(b, name, len);
    jb_pad(b);
    return at;
}

typedef struct jb_inode {
    uint32_t ino;
    uint32_t version;
    uint32_t mode;
    uint32_t isize;
    uint32_t offset;
    uint32_t dsize;
    uint32_t compr;
    uint32_t mtime;
    const void *payload;
    size_t payload_len;
} jb_inode_t;

TEST_UNUSED static size_t jb_inode(buf_t *b, int big, const jb_inode_t *in)
{
    unsigned char node[68];
    size_t at = b->len;

    memset(node, 0, sizeof(node));
    jb_header(node, JB_INODE, (uint32_t)(68 + in->payload_len), big);
    jb_put32(node + 12, in->ino, big);
    jb_put32(node + 16, in->version, big);
    jb_put32(node + 20, in->mode, big);
    jb_put32(node + 28, in->isize, big);
    jb_put32(node + 36, in->mtime, big);
    jb_put32(node + 44, in->offset, big);
    jb_put32(node + 48, (uint32_t)in->payload_len, big);
    jb_put32(node + 52, in->dsize, big);
    node[56] = (unsigned char)in->compr;
    jb_put32(node + 60, jb_crc(in->payload, in->payload_len), big);
    jb_put32(node + 64, jb_crc(node, 60), big);
    buf_put(b, node, 68);
    if (in->payload_len > 0)
        buf_put(b, in->payload, in->payload_len);
    jb_pad(b);
    return at;
}

TEST_UNUSED static size_t jb_file(buf_t *b, int big, uint32_t ino, uint32_t mode, const void *data, size_t len)
{
    jb_inode_t in;

    memset(&in, 0, sizeof(in));
    in.ino = ino;
    in.version = 1;
    in.mode = mode;
    in.isize = (uint32_t)len;
    in.dsize = (uint32_t)len;
    in.payload = data;
    in.payload_len = len;
    return jb_inode(b, big, &in);
}

typedef struct counter {
    long calls;
    long fail_at;
    long live;
    size_t bytes;
    size_t peak;
} counter_t;

typedef union counter_header {
    size_t size;
    long double align_ld;
    void *align_p;
} counter_header_t;

TEST_UNUSED static void *counter_alloc(void *ctx, size_t size)
{
    counter_t *c = ctx;
    counter_header_t *h;

    if (c->calls++ == c->fail_at)
        return NULL;
    h = malloc(sizeof(*h) + size);
    if (h == NULL)
        return NULL;
    h->size = size;
    c->live++;
    c->bytes += size;
    if (c->bytes > c->peak)
        c->peak = c->bytes;
    return h + 1;
}

TEST_UNUSED static void counter_free(void *ctx, void *ptr)
{
    counter_t *c = ctx;
    counter_header_t *h;

    if (ptr == NULL)
        return;
    h = (counter_header_t *)ptr - 1;
    c->live--;
    c->bytes -= h->size;
    free(h);
}

TEST_UNUSED static void counter_options(mtdf_options_t *opts, counter_t *c, long fail_at)
{
    memset(c, 0, sizeof(*c));
    c->fail_at = fail_at;
    opts->alloc_fn = counter_alloc;
    opts->free_fn = counter_free;
    opts->alloc_ctx = c;
}

TEST_UNUSED static int walk_all(mtdf_fs_t *fs, mtdf_ino_t dir, unsigned char *scratch, size_t scratch_len, uint64_t limit,
                    uint64_t *out_bytes)
{
    mtdf_dirent_t e;
    mtdf_stat_t st;
    uint64_t cookie = 0;
    uint64_t off;
    size_t got;
    char target[MTDF_PATH_MAX + 1];
    int status;
    int first = MTDF_OK;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
        if ((status = mtdf_stat(fs, e.ino, &st)) != MTDF_OK)
            return status;
        if (st.type == MTDF_TYPE_DIR)
            status = walk_all(fs, e.ino, scratch, scratch_len, limit, out_bytes);
        else if (st.type == MTDF_TYPE_LINK)
            status = mtdf_readlink(fs, e.ino, target, sizeof(target), &got);
        for (off = 0; status == MTDF_OK && st.type == MTDF_TYPE_FILE && off < st.size && off < limit; off += got) {
            status = mtdf_read(fs, e.ino, off, scratch, scratch_len, &got);
            *out_bytes += got;
        }
        if (status != MTDF_OK && first == MTDF_OK)
            first = status;
    }
    return status == MTDF_END ? first : status;
}

#endif
