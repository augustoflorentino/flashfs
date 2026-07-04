#if defined(__unix__) || defined(__APPLE__)
#define _DEFAULT_SOURCE
#define FLASHFS_HAVE_MMAP 1
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "build.h"

#include <time.h>

static const char *data_dir;

static const char *const images[] = {"small-jffs2-le-zlib.img", "small-jffs2-be-lzma.img", "small-cramfs-le.img",
                                     "small-cramfs-be.img",     "ubi.img",                 "small-ubifs-zstd.img",
                                     "ubifs-journal.img",       "ubifs-xattr.img"};

static double now_ms(void)
{
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
}

static uint32_t rng_state = 20251124u;

static uint32_t rng(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 8;
}

#define M5_FILES 2000u
#define M5_DAMAGED 1000u

typedef enum { DAMAGE_HEADER, DAMAGE_NODE, DAMAGE_DATA } damage_t;

static size_t crc_field(damage_t kind, int is_inode)
{
    if (kind == DAMAGE_HEADER)
        return 8;
    if (kind == DAMAGE_NODE)
        return is_inode ? 64 : 32;
    return is_inode ? 60 : 36;
}

static void m5_check_tree(mtdf_fs_t *fs, const unsigned char *hit)
{
    char name[32];
    char want[64];
    char got_data[64];
    mtdf_stat_t st;
    mtdf_ino_t ino;
    size_t got;
    unsigned i;
    unsigned readable = 0;

    for (i = 0; i < M5_FILES; i++) {
        sprintf(name, "/f%04u", i);
        sprintf(want, "content of file %u\n", i);
        if (hit[2 * i]) {
            CHECK_EQ(mtdf_lookup(fs, name, 0, &ino), MTDF_ERR_NOENT);
            continue;
        }
        CHECK_EQ(mtdf_lookup(fs, name, 0, &ino), MTDF_OK);
        CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
        if (hit[2 * i + 1]) {
            CHECK_EQ(st.size, 0);
            continue;
        }
        CHECK_EQ(mtdf_read(fs, ino, 0, got_data, sizeof(got_data), &got), MTDF_OK);
        CHECK(got == strlen(want) && memcmp(got_data, want, got) == 0);
        readable++;
    }
    CHECK(readable > 0);
}

static void m5(damage_t kind)
{
    static unsigned char hit[2 * M5_FILES];
    static size_t offsets[2 * M5_FILES];
    char name[32];
    char content[64];
    mtdf_report_t report;
    mtdf_fs_t *fs;
    buf_t b = {0};
    unsigned i;
    unsigned done = 0;
    uint64_t counted;

    jb_cleanmarker(&b, 0);
    for (i = 0; i < M5_FILES; i++) {
        sprintf(name, "f%04u", i);
        sprintf(content, "content of file %u\n", i);
        offsets[2 * i] = jb_dirent(&b, 0, 1, i + 2, 1, name, JB_DT_REG);
        offsets[2 * i + 1] = jb_file(&b, 0, i + 2, 0100644, content, strlen(content));
    }
    memset(hit, 0, sizeof(hit));
    while (done < M5_DAMAGED) {
        i = rng() % (2 * M5_FILES);
        if (hit[i])
            continue;
        hit[i] = 1;
        b.data[offsets[i] + crc_field(kind, i % 2)] ^= 0x01;
        done++;
    }
    CHECK_EQ(mtdf_open_mem(b.data, b.len, NULL, &fs), MTDF_OK);
    if (fs == NULL) {
        buf_free(&b);
        return;
    }
    CHECK_EQ(mtdf_report(fs, &report), MTDF_OK);
    counted = kind == DAMAGE_HEADER ? report.bad_header
                                    : kind == DAMAGE_NODE ? report.bad_node_crc : report.bad_data_crc;
    CHECK_EQ(counted, M5_DAMAGED);
    m5_check_tree(fs, hit);
    printf("M5 %s: %u damaged nodes, %llu reported\n",
           kind == DAMAGE_HEADER ? "header CRC" : kind == DAMAGE_NODE ? "node CRC" : "data CRC", M5_DAMAGED,
           (unsigned long long)counted);
    mtdf_close(fs);
    buf_free(&b);
}

#ifdef FLASHFS_HAVE_MMAP
static void m7_one(const buf_t *image, size_t len, int guard_before, unsigned char *scratch)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t span = (len + page - 1) / page * page + page;
    unsigned char *map = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned char *at;
    mtdf_fs_t *fs;
    uint64_t bytes = 0;

    if (map == MAP_FAILED)
        abort();
    if (guard_before) {
        mprotect(map, page, PROT_NONE);
        at = map + page;
    } else {
        mprotect(map + span - page, page, PROT_NONE);
        at = map + span - page - len;
    }
    memcpy(at, image->data, len);
    mprotect(guard_before ? map + page : map, span - page, PROT_READ);
    if (mtdf_open_mem(at, len, NULL, &fs) == MTDF_OK) {
        (void)walk_all(fs, MTDF_ROOT, scratch, 65536, UINT64_MAX, &bytes);
        mtdf_close(fs);
    }
    munmap(map, span);
}

static void m7(void)
{
    static unsigned char scratch[65536];
    buf_t b = {0};
    size_t len;
    size_t i;
    size_t tried;

    for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        if (buf_load(&b, data_dir, images[i]) != 0)
            continue;
        tried = 0;
        for (len = 0; len <= b.len; len += len < 65536 ? 1 : 4096, tried++) {
            m7_one(&b, len, 0, scratch);
            m7_one(&b, len, 1, scratch);
        }
        printf("M7 %s: %zu lengths, both guard sides\n", images[i], tried);
    }
    buf_free(&b);
}
#else
static void m7(void)
{
    printf("M7: no mmap on this platform, skipped\n");
}
#endif

#define M9_IMAGE (1u << 20)
#define M9_FILES 25u
#define M9_NODE_DATA (128u * 1024u)
#define M9_BUDGET (256ull << 20)

static size_t m9_zeros_zlib(unsigned char *out)
{
    static const unsigned char head[] = {0x78, 0xda, 0xed, 0xc1, 0x31, 0x01, 0x00, 0x00,
                                         0x00, 0xc2, 0xa0, 0xf5, 0x4f, 0xed, 0x61, 0x0d, 0xa0};
    static const unsigned char tail[] = {0x6e, 0x00, 0x1e, 0x00, 0x01};

    memcpy(out, head, sizeof(head));
    memset(out + sizeof(head), 0, 127);
    memcpy(out + sizeof(head) + 127, tail, sizeof(tail));
    return sizeof(head) + 127 + sizeof(tail);
}

static void m9_build(buf_t *b, int zlib)
{
    unsigned char payload[256];
    size_t payload_len = zlib ? m9_zeros_zlib(payload) : 0;
    char name[16];
    jb_inode_t in;
    uint32_t f;
    uint32_t n;

    jb_cleanmarker(b, 0);
    for (f = 0; f < M9_FILES; f++) {
        sprintf(name, "bomb%02u", f);
        jb_dirent(b, 0, 1, f + 2, 1, name, JB_DT_REG);
    }
    memset(&in, 0, sizeof(in));
    in.mode = 0100644;
    in.isize = 0xffffffffu;
    in.dsize = M9_NODE_DATA;
    in.compr = zlib ? 6 : 1;
    in.payload = payload;
    in.payload_len = payload_len;
    for (n = 0; b->len + 68 + payload_len + 4 <= M9_IMAGE; n++) {
        in.ino = n % M9_FILES + 2;
        in.version = n + 1;
        in.offset = (n / M9_FILES) * M9_NODE_DATA;
        jb_inode(b, 0, &in);
    }
}

static void m9(int zlib)
{
    static unsigned char chunk[1 << 20];
    mtdf_options_t opts;
    counter_t c;
    mtdf_fs_t *fs;
    mtdf_dirent_t e;
    uint64_t cookie = 0;
    uint64_t off;
    uint64_t delivered = 0;
    size_t got;
    buf_t b = {0};
    int status = MTDF_OK;
    double start = now_ms();

    if (zlib && !(mtdf_features() & MTDF_FEATURE_ZLIB))
        return;
    m9_build(&b, zlib);
    memset(&opts, 0, sizeof(opts));
    counter_options(&opts, &c, -1);
    opts.max_total_bytes = M9_BUDGET;
    opts.max_file_bytes = 8ull << 30;
    CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_OK);
    if (fs == NULL) {
        buf_free(&b);
        return;
    }
    while (status == MTDF_OK && mtdf_readdir(fs, MTDF_ROOT, &cookie, &e) == MTDF_OK) {
        for (off = 0; status == MTDF_OK && off < 0xffffffffu; off += got) {
            status = mtdf_read(fs, e.ino, off, chunk, sizeof(chunk), &got);
            delivered += got;
        }
    }
    CHECK_EQ(status, MTDF_ERR_BUDGET);
    CHECK(c.peak <= M9_BUDGET + (1u << 20));
    printf("M9 %s: %zu-byte image declares %llu GiB; stopped with %s after %llu MiB delivered, "
           "peak heap %zu KiB, %.0f ms\n",
           zlib ? "zlib" : "zero", b.len, (unsigned long long)M9_FILES * 4, mtdf_strerror(status),
           (unsigned long long)(delivered >> 20), c.peak >> 10, now_ms() - start);
    mtdf_close(fs);
    CHECK_EQ(c.live, 0);
    buf_free(&b);
}

static void m10_report(const char *what, double ms, int status, uint64_t loops)
{
    printf("M10 %s: %s, loops reported %llu, %.3f ms\n", what, mtdf_strerror(status), (unsigned long long)loops,
           ms);
    CHECK(ms < 10.0);
}

static void m10_jffs2(void)
{
    mtdf_report_t report;
    mtdf_ino_t ino;
    mtdf_fs_t *fs;
    buf_t b = {0};
    double start;
    int status;

    jb_cleanmarker(&b, 0);
    jb_dirent(&b, 0, 1, 2, 1, "d", JB_DT_DIR);
    jb_file(&b, 0, 2, 0040755, NULL, 0);
    jb_dirent(&b, 0, 2, 2, 1, "self", JB_DT_DIR);
    jb_dirent(&b, 0, 4, 3, 1, "x", JB_DT_DIR);
    jb_file(&b, 0, 3, 0040755, NULL, 0);
    jb_dirent(&b, 0, 3, 4, 1, "y", JB_DT_DIR);
    jb_file(&b, 0, 4, 0040755, NULL, 0);
    jb_dirent(&b, 0, 1, 5, 1, "twin1", JB_DT_DIR);
    jb_dirent(&b, 0, 1, 5, 1, "twin2", JB_DT_DIR);
    jb_file(&b, 0, 5, 0040755, NULL, 0);
    jb_dirent(&b, 0, 1, 6, 1, "me", JB_DT_LNK);
    jb_file(&b, 0, 6, 0120777, "me", 2);
    jb_dirent(&b, 0, 1, 7, 1, "ping", JB_DT_LNK);
    jb_file(&b, 0, 7, 0120777, "pong/x", 6);
    jb_dirent(&b, 0, 1, 8, 1, "pong", JB_DT_LNK);
    jb_file(&b, 0, 8, 0120777, "ping/x", 6);

    start = now_ms();
    status = mtdf_open_mem(b.data, b.len, NULL, &fs);
    CHECK_EQ(status, MTDF_OK);
    if (status != MTDF_OK) {
        buf_free(&b);
        return;
    }
    mtdf_report(fs, &report);
    m10_report("jffs2 directories (self, cycle, twin)", now_ms() - start, status, report.loops);
    CHECK(report.loops >= 3);

    start = now_ms();
    status = mtdf_lookup(fs, "/me", MTDF_LOOKUP_FOLLOW, &ino);
    m10_report("link to itself", now_ms() - start, status, 0);
    CHECK_EQ(status, MTDF_ERR_LOOP);
    start = now_ms();
    status = mtdf_lookup(fs, "/ping", MTDF_LOOKUP_FOLLOW, &ino);
    m10_report("link pair growing the path", now_ms() - start, status, 0);
    CHECK(status == MTDF_ERR_LOOP || status == MTDF_ERR_NAMETOOLONG);
    mtdf_close(fs);
    buf_free(&b);
}

static void m10_cramfs(void)
{
    mtdf_report_t report;
    mtdf_fs_t *fs;
    buf_t b = {0};
    unsigned char *root;
    unsigned char *a;
    double start;
    uint64_t bytes = 0;
    unsigned char scratch[4096];
    int status;

    if (buf_load(&b, data_dir, "small-cramfs-le.img") != 0)
        return;
    root = b.data + 64;
    a = b.data + ((root[8] | root[9] << 8 | root[10] << 16 | (uint32_t)root[11] << 24) >> 6) * 4;
    memcpy(a + 4, root + 4, 3);
    a[8] = (unsigned char)((a[8] & 0x3f) | (root[8] & 0xc0));
    memcpy(a + 9, root + 9, 3);
    start = now_ms();
    status = mtdf_open_mem(b.data, b.len, NULL, &fs);
    CHECK_EQ(status, MTDF_OK);
    if (status == MTDF_OK) {
        CHECK_EQ(walk_all(fs, MTDF_ROOT, scratch, sizeof(scratch), UINT64_MAX, &bytes), MTDF_OK);
        mtdf_report(fs, &report);
        m10_report("cramfs directory pointing at the root", now_ms() - start, status, report.loops);
        CHECK(report.loops >= 1);
        mtdf_close(fs);
    }
    buf_free(&b);
}

static int m11_open(const buf_t *b, int callback, mtdf_options_t *opts, mtdf_fs_t **fs);

typedef struct m11_source {
    const buf_t *image;
} m11_source_t;

static int m11_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const m11_source_t *s = ctx;

    memcpy(buf, s->image->data + offset, len);
    return 0;
}

static m11_source_t m11_src;

static int m11_open(const buf_t *b, int callback, mtdf_options_t *opts, mtdf_fs_t **fs)
{
    m11_src.image = b;
    if (callback)
        return mtdf_open_cb(m11_read, &m11_src, b->len, opts, fs);
    return mtdf_open_mem(b->data, b->len, opts, fs);
}

static void m11_image(const buf_t *b, const char *name, int callback)
{
    static unsigned char scratch[65536];
    mtdf_options_t opts;
    counter_t c;
    mtdf_fs_t *fs;
    uint64_t bytes;
    long n;
    long opened = -1;
    long reading = 0;
    long after;
    int status;

    for (n = 0; opened < 0; n++) {
        memset(&opts, 0, sizeof(opts));
        counter_options(&opts, &c, n);
        status = m11_open(b, callback, &opts, &fs);
        if (status == MTDF_OK) {
            opened = n;
            mtdf_close(fs);
        } else {
            CHECK_EQ(status, MTDF_ERR_NOMEM);
        }
        CHECK_EQ(c.live, 0);
    }
    for (n = 0;; n++) {
        memset(&opts, 0, sizeof(opts));
        counter_options(&opts, &c, -1);
        if (m11_open(b, callback, &opts, &fs) != MTDF_OK) {
            test_failures++;
            return;
        }
        after = c.calls;
        c.fail_at = after + n;
        bytes = 0;
        status = walk_all(fs, MTDF_ROOT, scratch, sizeof(scratch), UINT64_MAX, &bytes);
        mtdf_close(fs);
        CHECK_EQ(c.live, 0);
        if (c.calls <= after + n) {
            CHECK_EQ(status, MTDF_OK);
            break;
        }
        CHECK_EQ(status, MTDF_ERR_NOMEM);
        reading++;
    }
    printf("M11 %s (%s): %ld allocations failed in open, %ld while reading; no leak\n", name,
           callback ? "callback" : "memory", opened, reading);
}

static void m11(void)
{
    buf_t b = {0};
    size_t i;

    for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        if (buf_load(&b, data_dir, images[i]) != 0)
            continue;
        m11_image(&b, images[i], 0);
        m11_image(&b, images[i], 1);
    }
    buf_free(&b);
}

typedef struct failing {
    const buf_t *image;
    long calls;
    long fail_at;
} failing_t;

static int failing_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    failing_t *f = ctx;

    if (f->calls++ == f->fail_at)
        return -1;
    memcpy(buf, f->image->data + offset, len);
    return 0;
}

static void io_failures(void)
{
    static unsigned char scratch[65536];
    failing_t src;
    mtdf_fs_t *fs;
    buf_t b = {0};
    uint64_t bytes;
    long n;
    long failed;
    size_t i;
    int status;

    for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        if (buf_load(&b, data_dir, images[i]) != 0)
            continue;
        for (n = 0, failed = 0;; n++) {
            src.image = &b;
            src.calls = 0;
            src.fail_at = n;
            status = mtdf_open_cb(failing_read, &src, b.len, NULL, &fs);
            if (status == MTDF_OK) {
                bytes = 0;
                status = walk_all(fs, MTDF_ROOT, scratch, sizeof(scratch), UINT64_MAX, &bytes);
                mtdf_close(fs);
            }
            if (src.calls <= n) {
                CHECK_EQ(status, MTDF_OK);
                break;
            }
            CHECK_EQ(status, MTDF_ERR_IO);
            failed++;
        }
        printf("I/O %s: each of %ld reads failed in turn, each reported as an I/O error\n", images[i], failed);
    }
    buf_free(&b);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_robust <tests/data>\n");
        return 2;
    }
    data_dir = argv[1];
    m5(DAMAGE_HEADER);
    m5(DAMAGE_NODE);
    m5(DAMAGE_DATA);
    m7();
    m9(0);
    m9(1);
    m10_jffs2();
    m10_cramfs();
    m11();
    io_failures();
    if (test_failures != 0) {
        printf("test_robust: %d checks failed\n", test_failures);
        return 1;
    }
    printf("test_robust: ok\n");
    return 0;
}
