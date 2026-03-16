#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include <flashfs/flashfs.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define fseek64(f, off, whence) _fseeki64(f, off, whence)
#define ftell64(f) _ftelli64(f)
#else
#define fseek64(f, off, whence) fseeko(f, (off_t)(off), whence)
#define ftell64(f) ftello(f)
#endif

static int read_file(void *ctx, uint64_t offset, void *buf, size_t len)
{
    FILE *f = ctx;

    if (fseek64(f, (long long)offset, SEEK_SET) != 0)
        return -1;
    return fread(buf, 1, len, f) == len ? 0 : -1;
}

static void show(mtdf_fs_t *fs, mtdf_ino_t ino, const char *path)
{
    char target[MTDF_PATH_MAX + 1];
    mtdf_stat_t st;
    size_t len;

    if (mtdf_stat(fs, ino, &st) != MTDF_OK)
        return;
    printf("%06o %u %u %llu %s", st.mode, st.uid, st.gid, (unsigned long long)st.size, path[0] ? path : "/");
    if (st.type == MTDF_TYPE_LINK && mtdf_readlink(fs, ino, target, sizeof(target), &len) == MTDF_OK)
        printf(" -> %s", target);
    if (st.type == MTDF_TYPE_CHR || st.type == MTDF_TYPE_BLK)
        printf(" %u,%u", st.dev_major, st.dev_minor);
    putchar('\n');
}

static int list(mtdf_fs_t *fs, mtdf_ino_t dir, char *path, size_t len)
{
    mtdf_dirent_t e;
    uint64_t cookie = 0;
    int status;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
        if (len + 1 + e.name_len > MTDF_PATH_MAX)
            continue;
        path[len] = '/';
        memcpy(path + len + 1, e.name, e.name_len + 1);
        show(fs, e.ino, path);
        if (e.type == MTDF_TYPE_DIR && (status = list(fs, e.ino, path, len + 1 + e.name_len)) != MTDF_OK)
            return status;
    }
    return status == MTDF_END ? MTDF_OK : status;
}

static void summary(const mtdf_fs_t *fs)
{
    mtdf_info_t info;
    mtdf_report_t r;

    mtdf_info(fs, &info);
    mtdf_report(fs, &r);
    fprintf(stderr, "%s, %s-endian, %llu entries, %llu bytes", mtdf_format_name(info.format),
            info.big_endian ? "big" : "little", (unsigned long long)info.entries,
            (unsigned long long)info.image_size);
    if (r.bad_header || r.bad_node_crc || r.bad_data_crc || r.bad_image_crc || r.bad_structure || r.orphans)
        fprintf(stderr, "; damaged: %llu headers, %llu node CRCs, %llu data CRCs, %llu image CRC, "
                        "%llu structure, %llu orphans",
                (unsigned long long)r.bad_header, (unsigned long long)r.bad_node_crc,
                (unsigned long long)r.bad_data_crc, (unsigned long long)r.bad_image_crc,
                (unsigned long long)r.bad_structure, (unsigned long long)r.orphans);
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    static char path[2 * MTDF_PATH_MAX + 2];
    mtdf_fs_t *fs;
    mtdf_ino_t ino = MTDF_ROOT;
    mtdf_stat_t st;
    long long size;
    FILE *f;
    int status;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: mtdf-ls <image> [path]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL || fseek64(f, 0, SEEK_END) != 0 || (size = (long long)ftell64(f)) < 0) {
        fprintf(stderr, "mtdf-ls: cannot open %s\n", argv[1]);
        return 1;
    }
    status = mtdf_open_cb(read_file, f, (uint64_t)size, NULL, &fs);
    if (status != MTDF_OK) {
        fprintf(stderr, "mtdf-ls: %s: %s\n", argv[1], mtdf_strerror(status));
        fclose(f);
        return 1;
    }
    summary(fs);
    if (argc == 3) {
        status = mtdf_lookup(fs, argv[2], MTDF_LOOKUP_FOLLOW, &ino);
        strncpy(path, argv[2], MTDF_PATH_MAX);
        if (status == MTDF_OK)
            show(fs, ino, path);
    }
    if (status == MTDF_OK && mtdf_stat(fs, ino, &st) == MTDF_OK && st.type == MTDF_TYPE_DIR)
        status = list(fs, ino, path, strlen(path));
    if (status != MTDF_OK)
        fprintf(stderr, "mtdf-ls: %s %s\n", mtdf_strerror(status), mtdf_detail(fs));
    mtdf_close(fs);
    fclose(f);
    return status == MTDF_OK ? 0 : 1;
}
