#include <flashfs/flashfs.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifndef FUZZ_FORMAT
#error "build with -DFUZZ_FORMAT=MTDF_FORMAT_JFFS2, _CRAMFS or _UBI"
#endif

typedef struct input {
    const uint8_t *data;
    size_t size;
} input_t;

static void mix(uint64_t *h, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t i;

    for (i = 0; i < len; i++)
        *h = (*h ^ p[i]) * 0x100000001b3ull;
}

static void mix_status(uint64_t *h, int status)
{
    mix(h, &status, sizeof(status));
}

static void *filled_alloc(void *ctx, size_t size)
{
    void *p = malloc(size);

    if (p != NULL)
        memset(p, *(const int *)ctx, size);
    return p;
}

static void filled_free(void *ctx, void *ptr)
{
    (void)ctx;
    free(ptr);
}

static int read_input(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const input_t *in = ctx;

    memcpy(buf, in->data + offset, len);
    return 0;
}

static void read_file(mtdf_fs_t *fs, mtdf_ino_t ino, uint64_t size, uint64_t *h)
{
    static unsigned char scratch[1 << 16];
    uint64_t off;
    size_t got;
    int status;

    for (off = 0; off < size; off += got) {
        status = mtdf_read(fs, ino, off, scratch, sizeof(scratch), &got);
        mix_status(h, status);
        if (status != MTDF_OK || got == 0)
            return;
        mix(h, scratch, got);
    }
}

static void walk(mtdf_fs_t *fs, mtdf_ino_t dir, unsigned depth, uint64_t *h)
{
    char target[MTDF_PATH_MAX + 1];
    mtdf_dirent_t e;
    mtdf_stat_t st;
    mtdf_ino_t ino;
    uint64_t cookie = 0;
    size_t got;
    int status;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
        mix(h, e.name, e.name_len);
        status = mtdf_stat(fs, e.ino, &st);
        mix_status(h, status);
        if (status != MTDF_OK)
            return;
        mix(h, &st.size, sizeof(st.size));
        mix(h, &st.mode, sizeof(st.mode));
        if (st.type == MTDF_TYPE_DIR && depth < 64)
            walk(fs, e.ino, depth + 1, h);
        if (st.type == MTDF_TYPE_LINK && (status = mtdf_readlink(fs, e.ino, target, sizeof(target), &got)) == MTDF_OK) {
            mix(h, target, got);
            status = mtdf_lookup(fs, target, MTDF_LOOKUP_FOLLOW, &ino);
        }
        mix_status(h, status);
        if (st.type == MTDF_TYPE_FILE)
            read_file(fs, e.ino, st.size, h);
    }
    mix_status(h, status);
}

static uint64_t read_all(input_t *in, int callback)
{
    static const int fills[] = {0x00, 0xa5};
    mtdf_options_t opts;
    mtdf_report_t report;
    mtdf_fs_t *fs;
    uint64_t h = 0xcbf29ce484222325ull;
    int status;

    memset(&opts, 0, sizeof(opts));
    opts.format = FUZZ_FORMAT;
    opts.max_total_bytes = 64u << 20;
    opts.alloc_fn = filled_alloc;
    opts.free_fn = filled_free;
    opts.alloc_ctx = (void *)&fills[callback];
    status = callback ? mtdf_open_cb(read_input, in, in->size, &opts, &fs)
                      : mtdf_open_mem(in->data, in->size, &opts, &fs);
    mix_status(&h, status);
    if (status != MTDF_OK)
        return h;
    mtdf_report(fs, &report);
    mix(&h, &report, sizeof(report));
    walk(fs, MTDF_ROOT, 0, &h);
    mtdf_close(fs);
    return h;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    input_t in;

    in.data = data;
    in.size = size;
    if (read_all(&in, 0) != read_all(&in, 1))
        abort();
    return 0;
}
