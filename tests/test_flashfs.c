#include "build.h"

static const char *data_dir;

static const char passwd[] = "root:x:0:0::/root:/bin/sh\n";

static unsigned char big_byte(size_t i)
{
    return (unsigned char)((i * 7 + i / 300) & 0xff);
}

static int open_image(buf_t *b, const char *name, mtdf_fs_t **out)
{
    if (buf_load(b, data_dir, name) != 0)
        return -1;
    return mtdf_open_mem(b->data, b->len, NULL, out);
}

static void test_strings(void)
{
    int status;

    CHECK(strcmp(mtdf_format_name(MTDF_FORMAT_JFFS2), "jffs2") == 0);
    CHECK(strcmp(mtdf_format_name(MTDF_FORMAT_CRAMFS), "cramfs") == 0);
    CHECK(strcmp(mtdf_format_name(MTDF_FORMAT_UBI), "ubi") == 0);
    CHECK(mtdf_format_name(99) != NULL);
    for (status = MTDF_OK; status <= MTDF_ERR_RANGE; status++)
        CHECK(mtdf_strerror(status) != NULL && mtdf_strerror(status)[0] != '\0');
    CHECK(mtdf_strerror(-1) != NULL);
    CHECK((mtdf_features() & ~(unsigned)(MTDF_FEATURE_ZLIB | MTDF_FEATURE_LZMA | MTDF_FEATURE_LZO)) == 0);
}

static void test_open_arguments(void)
{
    static const unsigned char garbage[4096] = {1, 2, 3};
    mtdf_options_t opts;
    mtdf_fs_t *fs = (mtdf_fs_t *)&opts;
    buf_t b = {0};

    CHECK_EQ(mtdf_open_mem(garbage, sizeof(garbage), NULL, NULL), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_open_mem(NULL, 10, NULL, &fs), MTDF_ERR_ARG);
    CHECK(fs == NULL);
    CHECK_EQ(mtdf_open_mem(garbage, sizeof(garbage), NULL, &fs), MTDF_ERR_FORMAT);
    CHECK_EQ(mtdf_open_mem(garbage, 0, NULL, &fs), MTDF_ERR_FORMAT);
    memset(&opts, 0, sizeof(opts));
    opts.reserved[2] = 1;
    CHECK_EQ(mtdf_open_mem(garbage, sizeof(garbage), &opts, &fs), MTDF_ERR_ARG);
    memset(&opts, 0, sizeof(opts));
    opts.reserved_ptr = &opts;
    CHECK_EQ(mtdf_open_mem(garbage, sizeof(garbage), &opts, &fs), MTDF_ERR_ARG);
    memset(&opts, 0, sizeof(opts));
    opts.alloc_fn = counter_alloc;
    CHECK_EQ(mtdf_open_mem(garbage, sizeof(garbage), &opts, &fs), MTDF_ERR_ARG);
    memset(&opts, 0, sizeof(opts));
    opts.format = 4;
    CHECK_EQ(mtdf_open_mem(garbage, sizeof(garbage), &opts, &fs), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_open_cb(NULL, NULL, 10, NULL, &fs), MTDF_ERR_ARG);

    if (buf_load(&b, data_dir, "jffs2-le-zlib.img") == 0) {
        memset(&opts, 0, sizeof(opts));
        opts.format = MTDF_FORMAT_CRAMFS;
        CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_ERR_FORMAT);
        opts.format = MTDF_FORMAT_UBI;
        CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_ERR_FORMAT);
        opts.format = MTDF_FORMAT_JFFS2;
        CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_OK);
        mtdf_close(fs);
    }
    mtdf_close(NULL);
    buf_free(&b);
}

static void check_tree(mtdf_fs_t *fs, size_t big_len)
{
    unsigned char *data = malloc(big_len + 16);
    char target[64];
    mtdf_ino_t ino;
    mtdf_stat_t st;
    size_t got;
    size_t i;
    int same = 1;

    if (data == NULL)
        abort();
    CHECK_EQ(mtdf_lookup(fs, "/etc/passwd", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.type, MTDF_TYPE_FILE);
    CHECK_EQ(st.mode, 0100600);
    CHECK_EQ(st.size, sizeof(passwd) - 1);
    CHECK_EQ(mtdf_read(fs, ino, 0, data, big_len, &got), MTDF_OK);
    CHECK(got == sizeof(passwd) - 1 && memcmp(data, passwd, got) == 0);

    CHECK_EQ(mtdf_lookup(fs, "/bin/sh", MTDF_LOOKUP_FOLLOW, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.size, big_len);
    CHECK_EQ(mtdf_read(fs, ino, 0, data, big_len + 16, &got), MTDF_OK);
    CHECK_EQ(got, big_len);
    for (i = 0; i < big_len; i++)
        same &= data[i] == big_byte(i);
    CHECK(same);

    CHECK_EQ(mtdf_lookup(fs, "/bin/sh", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_readlink(fs, ino, target, sizeof(target), &got), MTDF_OK);
    CHECK(got == 7 && strcmp(target, "busybox") == 0);

    CHECK_EQ(mtdf_lookup(fs, "/a/b/c/d.txt", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.size, 5000);
    CHECK_EQ(mtdf_lookup(fs, "/tmp", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.mode, 041777);
    CHECK_EQ(st.type, MTDF_TYPE_DIR);
    free(data);
}

static void test_fixed_images(void)
{
    static const struct {
        const char *name;
        uint32_t format;
        uint32_t big;
        size_t big_len;
    } images[] = {
        {"jffs2-le-zlib.img", MTDF_FORMAT_JFFS2, 0, 70000},  {"jffs2-be-lzma.img", MTDF_FORMAT_JFFS2, 1, 70000},
        {"jffs2-le-rtime.img", MTDF_FORMAT_JFFS2, 0, 70000}, {"jffs2-be-none.img", MTDF_FORMAT_JFFS2, 1, 70000},
        {"cramfs-le.img", MTDF_FORMAT_CRAMFS, 0, 70000},     {"cramfs-be.img", MTDF_FORMAT_CRAMFS, 1, 70000},
        {"small-cramfs-le.img", MTDF_FORMAT_CRAMFS, 0, 5000},
    };
    mtdf_info_t info;
    mtdf_report_t report;
    mtdf_fs_t *fs;
    buf_t b = {0};
    size_t i;

    for (i = 0; i < sizeof(images) / sizeof(images[0]); i++) {
        if (open_image(&b, images[i].name, &fs) != MTDF_OK) {
            printf("%s does not open\n", images[i].name);
            test_failures++;
            continue;
        }
        CHECK_EQ(mtdf_info(fs, &info), MTDF_OK);
        CHECK_EQ(info.format, images[i].format);
        CHECK_EQ(info.big_endian, images[i].big);
        CHECK_EQ(info.entries, 16);
        CHECK(info.image_size > 0 && info.image_size <= b.len);
        CHECK_EQ(mtdf_report(fs, &report), MTDF_OK);
        CHECK_EQ(report.bad_header + report.bad_node_crc + report.bad_data_crc + report.bad_image_crc, 0);
        CHECK_EQ(report.bad_structure + report.orphans + report.loops + report.duplicates, 0);
        check_tree(fs, images[i].big_len);
        mtdf_close(fs);
    }
    CHECK_EQ(mtdf_info(NULL, &info), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_report(NULL, &report), MTDF_ERR_ARG);
    buf_free(&b);
}

static void test_ubi(void)
{
    unsigned char chunk[4096];
    mtdf_info_t info;
    mtdf_report_t report;
    mtdf_fs_t *fs;
    mtdf_ino_t ino;
    mtdf_stat_t st;
    buf_t b = {0};
    size_t got;
    size_t i;
    int same = 1;

    if (open_image(&b, "ubi.img", &fs) != MTDF_OK) {
        printf("ubi.img does not open\n");
        test_failures++;
        buf_free(&b);
        return;
    }
    CHECK_EQ(mtdf_info(fs, &info), MTDF_OK);
    CHECK_EQ(info.format, MTDF_FORMAT_UBI);
    CHECK_EQ(info.block_size, 65536);
    CHECK_EQ(info.unit_size, 65536 - 4096);
    CHECK_EQ(mtdf_report(fs, &report), MTDF_OK);
    CHECK_EQ(report.superseded, 1);
    CHECK_EQ(mtdf_lookup(fs, "/rootfs", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.size, 8 * (65536 - 4096));
    for (i = 0; i < 70000; i++) {
        if (i % sizeof(chunk) == 0)
            CHECK_EQ(mtdf_read(fs, ino, i, chunk, sizeof(chunk), &got), MTDF_OK);
        same &= chunk[i % sizeof(chunk)] == big_byte(i);
    }
    CHECK(same);
    CHECK_EQ(mtdf_read(fs, ino, 70000, chunk, 1, &got), MTDF_OK);
    CHECK_EQ(chunk[0], 0xff);
    CHECK_EQ(mtdf_lookup(fs, "/kernel", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_read(fs, ino, 0, chunk, 5, &got), MTDF_OK);
    CHECK(got == 5 && memcmp(chunk, "KKKKK", 5) == 0);
    mtdf_close(fs);
    buf_free(&b);
}

typedef struct source {
    const buf_t *image;
    long calls;
    long fail_at;
} source_t;

static int source_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    source_t *s = ctx;

    if (s->calls++ == s->fail_at || offset > s->image->len || len > s->image->len - offset)
        return -1;
    memcpy(buf, s->image->data + offset, len);
    return 0;
}

static void test_open_cb(void)
{
    static const char *names[] = {"jffs2-le-zlib.img", "cramfs-be.img", "ubi.img"};
    unsigned char scratch[100000];
    mtdf_fs_t *mem;
    mtdf_fs_t *cb;
    source_t src;
    buf_t b = {0};
    uint64_t bytes_mem;
    uint64_t bytes_cb;
    size_t i;

    for (i = 0; i < 3; i++) {
        if (open_image(&b, names[i], &mem) != MTDF_OK) {
            test_failures++;
            continue;
        }
        memset(&src, 0, sizeof(src));
        src.image = &b;
        src.fail_at = -1;
        CHECK_EQ(mtdf_open_cb(source_read, &src, b.len, NULL, &cb), MTDF_OK);
        bytes_mem = bytes_cb = 0;
        CHECK_EQ(walk_all(mem, MTDF_ROOT, scratch, sizeof(scratch), UINT64_MAX, &bytes_mem), MTDF_OK);
        CHECK_EQ(walk_all(cb, MTDF_ROOT, scratch, sizeof(scratch), UINT64_MAX, &bytes_cb), MTDF_OK);
        CHECK(bytes_mem == bytes_cb && bytes_mem > 0);
        mtdf_close(mem);
        mtdf_close(cb);

        src.calls = 0;
        src.fail_at = 0;
        CHECK_EQ(mtdf_open_cb(source_read, &src, b.len, NULL, &cb), MTDF_ERR_IO);
    }
    buf_free(&b);
}

static void test_lookup(void)
{
    char long_path[MTDF_PATH_MAX + 2];
    mtdf_ino_t ino;
    mtdf_ino_t passwd_ino;
    mtdf_fs_t *fs;
    mtdf_stat_t st;
    buf_t b = {0};

    if (open_image(&b, "jffs2-be-lzma.img", &fs) != MTDF_OK) {
        test_failures++;
        buf_free(&b);
        return;
    }
    CHECK_EQ(mtdf_lookup(fs, "/etc/passwd", 0, &passwd_ino), MTDF_OK);
    CHECK_EQ(mtdf_lookup(fs, "", 0, &ino), MTDF_OK);
    CHECK_EQ(ino, MTDF_ROOT);
    CHECK_EQ(mtdf_lookup(fs, "/../../..", 0, &ino), MTDF_OK);
    CHECK_EQ(ino, MTDF_ROOT);
    CHECK_EQ(mtdf_lookup(fs, "etc//./passwd", 0, &ino), MTDF_OK);
    CHECK_EQ(ino, passwd_ino);
    CHECK_EQ(mtdf_lookup(fs, "/a/b/../../etc/passwd", 0, &ino), MTDF_OK);
    CHECK_EQ(ino, passwd_ino);
    CHECK_EQ(mtdf_lookup(fs, "/lib/up/passwd", 0, &ino), MTDF_OK);
    CHECK_EQ(ino, passwd_ino);
    CHECK_EQ(mtdf_lookup(fs, "/lib/up", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.type, MTDF_TYPE_LINK);
    CHECK_EQ(mtdf_lookup(fs, "/lib/up", MTDF_LOOKUP_FOLLOW, &ino), MTDF_OK);
    CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
    CHECK_EQ(st.type, MTDF_TYPE_DIR);
    CHECK_EQ(mtdf_lookup(fs, "/loop1", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_lookup(fs, "/loop1", MTDF_LOOKUP_FOLLOW, &ino), MTDF_ERR_LOOP);
    CHECK_EQ(mtdf_lookup(fs, "/loop1/x", 0, &ino), MTDF_ERR_LOOP);
    CHECK_EQ(mtdf_lookup(fs, "/etc/passwd/x", 0, &ino), MTDF_ERR_NOTDIR);
    CHECK_EQ(mtdf_lookup(fs, "/etc/nope", 0, &ino), MTDF_ERR_NOENT);
    CHECK_EQ(mtdf_lookup(fs, "/etc", 2, &ino), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_lookup(fs, NULL, 0, &ino), MTDF_ERR_ARG);
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    CHECK_EQ(mtdf_lookup(fs, long_path, 0, &ino), MTDF_ERR_NAMETOOLONG);
    mtdf_close(fs);
    buf_free(&b);
}

static void test_readdir_and_stat(void)
{
    static const char *root[] = {"a", "bin", "empty", "etc", "lib", "loop1", "loop2", "tmp"};
    mtdf_dirent_t e;
    mtdf_ino_t ino;
    mtdf_stat_t st;
    mtdf_fs_t *fs;
    uint64_t cookie = 0;
    buf_t b = {0};
    size_t n = 0;

    if (open_image(&b, "cramfs-le.img", &fs) != MTDF_OK) {
        test_failures++;
        buf_free(&b);
        return;
    }
    while (mtdf_readdir(fs, MTDF_ROOT, &cookie, &e) == MTDF_OK) {
        CHECK(n < 8 && strcmp(e.name, root[n]) == 0 && e.name_len == strlen(root[n]));
        CHECK_EQ(mtdf_stat(fs, e.ino, &st), MTDF_OK);
        CHECK_EQ(st.type, e.type);
        CHECK_EQ(st.ino, e.ino);
        n++;
    }
    CHECK_EQ(n, 8);
    CHECK_EQ(mtdf_readdir(fs, MTDF_ROOT, &cookie, &e), MTDF_END);
    CHECK_EQ(mtdf_stat(fs, MTDF_ROOT, &st), MTDF_OK);
    CHECK_EQ(st.type, MTDF_TYPE_DIR);
    CHECK_EQ(st.nlink, 2 + 5);
    CHECK_EQ(mtdf_lookup(fs, "/etc/passwd", 0, &ino), MTDF_OK);
    cookie = 0;
    CHECK_EQ(mtdf_readdir(fs, ino, &cookie, &e), MTDF_ERR_NOTDIR);
    CHECK_EQ(mtdf_readdir(fs, 100000, &cookie, &e), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_readdir(fs, MTDF_ROOT, NULL, &e), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_stat(fs, 100000, &st), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_stat(fs, MTDF_ROOT, NULL), MTDF_ERR_ARG);
    mtdf_close(fs);
    buf_free(&b);
}

static void test_read_and_readlink(void)
{
    unsigned char chunk[10000];
    char target[8];
    mtdf_ino_t ino;
    mtdf_fs_t *fs;
    buf_t b = {0};
    size_t got;
    size_t i;
    int same = 1;

    if (open_image(&b, "jffs2-le-zlib.img", &fs) != MTDF_OK) {
        test_failures++;
        buf_free(&b);
        return;
    }
    CHECK_EQ(mtdf_lookup(fs, "/bin/busybox", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_read(fs, ino, 4000, chunk, 200, &got), MTDF_OK);
    CHECK_EQ(got, 200);
    for (i = 0; i < 200; i++)
        same &= chunk[i] == big_byte(4000 + i);
    CHECK_EQ(mtdf_read(fs, ino, 65000, chunk, sizeof(chunk), &got), MTDF_OK);
    CHECK_EQ(got, 5000);
    for (i = 0; i < 5000; i++)
        same &= chunk[i] == big_byte(65000 + i);
    CHECK(same);
    CHECK_EQ(mtdf_read(fs, ino, 70000, chunk, sizeof(chunk), &got), MTDF_OK);
    CHECK_EQ(got, 0);
    CHECK_EQ(mtdf_read(fs, ino, UINT64_MAX, chunk, sizeof(chunk), &got), MTDF_OK);
    CHECK_EQ(got, 0);
    CHECK_EQ(mtdf_read(fs, ino, 0, NULL, 0, &got), MTDF_OK);
    CHECK_EQ(mtdf_read(fs, ino, 0, NULL, 1, &got), MTDF_ERR_ARG);
    CHECK_EQ(mtdf_read(fs, MTDF_ROOT, 0, chunk, 1, &got), MTDF_ERR_TYPE);
    CHECK_EQ(mtdf_readlink(fs, ino, target, sizeof(target), &got), MTDF_ERR_TYPE);

    CHECK_EQ(mtdf_lookup(fs, "/bin/sh", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_readlink(fs, ino, target, 7, &got), MTDF_ERR_RANGE);
    CHECK_EQ(got, 7);
    CHECK_EQ(mtdf_readlink(fs, ino, target, 8, &got), MTDF_OK);
    CHECK(strcmp(target, "busybox") == 0);
    CHECK_EQ(mtdf_read(fs, ino, 0, chunk, 1, &got), MTDF_ERR_TYPE);
    CHECK_EQ(mtdf_readlink(fs, ino, NULL, 8, &got), MTDF_ERR_ARG);
    mtdf_close(fs);
    buf_free(&b);
}

static void test_budget(void)
{
    unsigned char chunk[4096];
    mtdf_options_t opts;
    mtdf_ino_t ino;
    mtdf_fs_t *fs;
    buf_t b = {0};
    size_t got;
    uint64_t off;
    int status = MTDF_OK;

    if (buf_load(&b, data_dir, "jffs2-le-zlib.img") != 0)
        return;
    memset(&opts, 0, sizeof(opts));
    opts.max_file_bytes = 69999;
    CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_OK);
    CHECK_EQ(mtdf_lookup(fs, "/bin/busybox", 0, &ino), MTDF_OK);
    CHECK_EQ(mtdf_read(fs, ino, 0, chunk, 1, &got), MTDF_ERR_BUDGET);
    CHECK(mtdf_detail(fs)[0] != '\0');
    CHECK_EQ(mtdf_lookup(fs, "/etc/passwd", 0, &ino), MTDF_OK);
    CHECK(mtdf_detail(fs)[0] == '\0');
    CHECK_EQ(mtdf_read(fs, ino, 0, chunk, 1, &got), MTDF_OK);
    mtdf_close(fs);

    memset(&opts, 0, sizeof(opts));
    opts.max_total_bytes = 100000;
    CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_OK);
    CHECK_EQ(mtdf_lookup(fs, "/bin/busybox", 0, &ino), MTDF_OK);
    for (off = 0; status == MTDF_OK && off < 70000; off += got)
        status = mtdf_read(fs, ino, off, chunk, sizeof(chunk), &got);
    CHECK_EQ(status, MTDF_ERR_BUDGET);
    mtdf_close(fs);

    memset(&opts, 0, sizeof(opts));
    opts.max_entries = 10;
    CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_ERR_BUDGET);
    memset(&opts, 0, sizeof(opts));
    opts.max_depth = 3;
    CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_ERR_BUDGET);
    opts.max_depth = 4;
    CHECK_EQ(mtdf_open_mem(b.data, b.len, &opts, &fs), MTDF_OK);
    mtdf_close(fs);
    CHECK(mtdf_detail(NULL) != NULL);
    buf_free(&b);
}

static void test_jffs2_log(void)
{
    static const char old_data[] = "old content";
    static const char new_data[] = "NEW";
    unsigned char dev[4];
    unsigned char chunk[64];
    mtdf_report_t report;
    mtdf_stat_t st;
    mtdf_ino_t ino;
    mtdf_fs_t *fs;
    jb_inode_t in;
    buf_t b = {0};
    size_t got;
    size_t at;
    int big;

    for (big = 0; big < 2; big++) {
        b.len = 0;
        jb_cleanmarker(&b, big);
        jb_dirent(&b, big, 1, 2, 1, "file", JB_DT_REG);
        jb_file(&b, big, 2, 0100644, old_data, sizeof(old_data) - 1);
        memset(&in, 0, sizeof(in));
        in.ino = 2;
        in.version = 2;
        in.mode = 0100640;
        in.isize = sizeof(old_data) - 1;
        in.dsize = 3;
        in.mtime = 1700000000;
        in.payload = new_data;
        in.payload_len = 3;
        jb_inode(&b, big, &in);
        jb_dirent(&b, big, 1, 3, 1, "gone", JB_DT_REG);
        jb_file(&b, big, 3, 0100644, "x", 1);
        jb_dirent(&b, big, 1, 0, 2, "gone", JB_DT_REG);
        jb_put32(dev, (65u & 0xffu) | (4u << 8) | ((65u & ~0xffu) << 12), big);
        jb_dirent(&b, big, 1, 4, 1, "tty", 2);
        jb_file(&b, big, 4, 0020620, dev, 4);
        at = jb_dirent(&b, big, 1, 5, 1, "obsolete", JB_DT_REG);
        b.data[at + (big ? 2 : 3)] &= (unsigned char)~0x20u;

        CHECK_EQ(mtdf_open_mem(b.data, b.len, NULL, &fs), MTDF_OK);
        CHECK_EQ(mtdf_lookup(fs, "/file", 0, &ino), MTDF_OK);
        CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
        CHECK_EQ(st.mode, 0100640);
        CHECK_EQ(st.mtime, 1700000000);
        CHECK_EQ(mtdf_read(fs, ino, 0, chunk, sizeof(chunk), &got), MTDF_OK);
        CHECK(got == sizeof(old_data) - 1 && memcmp(chunk, "NEW content", got) == 0);
        CHECK_EQ(mtdf_lookup(fs, "/gone", 0, &ino), MTDF_ERR_NOENT);
        CHECK_EQ(mtdf_lookup(fs, "/tty", 0, &ino), MTDF_OK);
        CHECK_EQ(mtdf_stat(fs, ino, &st), MTDF_OK);
        CHECK_EQ(st.type, MTDF_TYPE_CHR);
        CHECK(st.dev_major == 4 && st.dev_minor == 65);
        CHECK_EQ(mtdf_report(fs, &report), MTDF_OK);
        CHECK_EQ(report.obsolete, 1);
        CHECK_EQ(report.superseded, 1);
        CHECK_EQ(report.orphans, 1);
        mtdf_close(fs);
    }
    buf_free(&b);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_flashfs <tests/data>\n");
        return 2;
    }
    data_dir = argv[1];
    test_strings();
    test_open_arguments();
    test_fixed_images();
    test_ubi();
    test_open_cb();
    test_lookup();
    test_readdir_and_stat();
    test_read_and_readlink();
    test_budget();
    test_jffs2_log();
    if (test_failures != 0) {
        printf("test_flashfs: %d checks failed\n", test_failures);
        return 1;
    }
    printf("test_flashfs: ok\n");
    return 0;
}
