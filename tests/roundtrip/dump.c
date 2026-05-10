#include "../parity/sha256.h"

#include <flashfs/flashfs.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *data = NULL;
    long size;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0)
        return NULL;
    data = malloc((size_t)size + 1);
    if (data == NULL || fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *len = (size_t)size;
    return data;
}

static void put_hex(const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t i;

    if (len == 0)
        fputs("-", stdout);
    for (i = 0; i < len; i++)
        printf("%02x", p[i]);
}

static int put_digest(mtdf_fs_t *fs, mtdf_ino_t ino, uint64_t size)
{
    unsigned char digest[32];
    unsigned char *data = malloc(size > 0 ? (size_t)size : 1);
    uint64_t off;
    size_t got = 1;
    int status = MTDF_OK;

    if (data == NULL)
        return MTDF_ERR_NOMEM;
    for (off = 0; status == MTDF_OK && off < size && got > 0; off += got)
        status = mtdf_read(fs, ino, off, data + off, (size_t)(size - off), &got);
    if (status == MTDF_OK) {
        sha256(data, (size_t)size, digest);
        put_hex(digest, 32);
    }
    free(data);
    return status;
}

static int entry(mtdf_fs_t *fs, const char *path, size_t path_len, mtdf_ino_t ino)
{
    char target[MTDF_PATH_MAX + 1];
    mtdf_stat_t st;
    size_t len = 0;
    int status = mtdf_stat(fs, ino, &st);

    if (status != MTDF_OK)
        return status;
    put_hex(path, path_len);
    printf(" %o %u %u %u,%u %llu ", st.mode, st.uid, st.gid, st.dev_major, st.dev_minor,
           (unsigned long long)st.size);
    if (st.type == MTDF_TYPE_FILE)
        status = put_digest(fs, ino, st.size);
    else
        fputs("-", stdout);
    fputs(" ", stdout);
    if (st.type == MTDF_TYPE_LINK)
        status = mtdf_readlink(fs, ino, target, sizeof(target), &len);
    put_hex(target, status == MTDF_OK ? len : 0);
    fputs("\n", stdout);
    return status;
}

static int memory_read(void *ctx, uint64_t offset, void *buf, size_t len)
{
    const unsigned char *image = ctx;

    memcpy(buf, image + offset, len);
    return 0;
}

static int walk(mtdf_fs_t *fs, mtdf_ino_t dir, char *path, size_t len)
{
    mtdf_dirent_t e;
    uint64_t cookie = 0;
    int status;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
        path[len] = '/';
        memcpy(path + len + 1, e.name, e.name_len + 1);
        if ((status = entry(fs, path, len + 1 + e.name_len, e.ino)) != MTDF_OK)
            return status;
        if (e.type == MTDF_TYPE_DIR && (status = walk(fs, e.ino, path, len + 1 + e.name_len)) != MTDF_OK)
            return status;
    }
    return status == MTDF_END ? MTDF_OK : status;
}

int main(int argc, char **argv)
{
    static char path[64 * 1024];
    mtdf_options_t opts;
    mtdf_report_t r;
    mtdf_fs_t *fs;
    unsigned char *image;
    const char *name = argv[argc - 1];
    int callback = argc == 3 && strcmp(argv[1], "-c") == 0;
    size_t len;
    int status;

    if ((argc != 2 && !callback) || (image = slurp(name, &len)) == NULL) {
        fprintf(stderr, "usage: dump [-c] <image>\n");
        return 2;
    }
    memset(&opts, 0, sizeof(opts));
    opts.max_total_bytes = UINT64_MAX;
    status = callback ? mtdf_open_cb(memory_read, image, len, &opts, &fs) : mtdf_open_mem(image, len, &opts, &fs);
    if (status == MTDF_OK) {
        mtdf_report(fs, &r);
        if (r.bad_header + r.bad_node_crc + r.bad_data_crc + r.bad_image_crc + r.bad_structure + r.orphans +
                r.loops + r.duplicates + r.unknown !=
            0)
            fprintf(stderr, "dump: %s reports damage\n", name);
        status = walk(fs, MTDF_ROOT, path, 0);
        if (status != MTDF_OK)
            fprintf(stderr, "dump: %s: %s %s\n", name, mtdf_strerror(status), mtdf_detail(fs));
        mtdf_close(fs);
    } else {
        fprintf(stderr, "dump: %s: %s\n", name, mtdf_strerror(status));
    }
    free(image);
    return status == MTDF_OK ? 0 : 1;
}
