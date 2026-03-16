#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#endif

#include <flashfs/flashfs.h>

#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define fseek64(f, off, whence) _fseeki64(f, off, whence)
#define ftell64(f) _ftelli64(f)
#define make_dir(path, mode) _mkdir(path)
#define create(path) fopen(path, "wbx")
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define fseek64(f, off, whence) fseeko(f, (off_t)(off), whence)
#define ftell64(f) ftello(f)
#define make_dir(path, mode) mkdir(path, mode)
#define create(path) fdopen(open(path, O_WRONLY | O_CREAT | O_EXCL, 0600), "wb")
#endif

static int read_file(void *ctx, uint64_t offset, void *buf, size_t len)
{
    FILE *f = ctx;

    return fseek64(f, (long long)offset, SEEK_SET) == 0 && fread(buf, 1, len, f) == len ? 0 : -1;
}

static int write_file(mtdf_fs_t *fs, mtdf_ino_t ino, const char *path)
{
    static unsigned char chunk[1 << 16];
    FILE *out = create(path);
    uint64_t off = 0;
    size_t got = 1;
    int status = MTDF_OK;

    if (out == NULL)
        return MTDF_ERR_IO;
    while (status == MTDF_OK && got > 0) {
        status = mtdf_read(fs, ino, off, chunk, sizeof(chunk), &got);
        if (status == MTDF_OK && fwrite(chunk, 1, got, out) != got)
            status = MTDF_ERR_IO;
        off += got;
    }
    return fclose(out) == 0 ? status : MTDF_ERR_IO;
}

static int make_link(mtdf_fs_t *fs, mtdf_ino_t ino, const char *path)
{
    char target[MTDF_PATH_MAX + 1];
    size_t len;
    int status = mtdf_readlink(fs, ino, target, sizeof(target), &len);

#if defined(_WIN32)
    if (status == MTDF_OK)
        printf("skipped link %s -> %s\n", path, target);
#else
    if (status == MTDF_OK && symlink(target, path) != 0)
        status = MTDF_ERR_IO;
#endif
    return status;
}

static int extract(mtdf_fs_t *fs, mtdf_ino_t dir, char *path, size_t len)
{
    mtdf_dirent_t e;
    mtdf_stat_t st;
    uint64_t cookie = 0;
    int status;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
#if defined(_WIN32)
        if (strpbrk(e.name, "\\:") != NULL) {
            printf("skipped %s/%s: not a valid Windows name\n", path, e.name);
            continue;
        }
#endif
        if (len + 1 + e.name_len > MTDF_PATH_MAX || mtdf_stat(fs, e.ino, &st) != MTDF_OK)
            return MTDF_ERR_NAMETOOLONG;
        path[len] = '/';
        memcpy(path + len + 1, e.name, e.name_len + 1);
        if (st.type == MTDF_TYPE_DIR)
            status = make_dir(path, 0700) == 0 ? extract(fs, e.ino, path, len + 1 + e.name_len) : MTDF_ERR_IO;
        else if (st.type == MTDF_TYPE_FILE)
            status = write_file(fs, e.ino, path);
        else if (st.type == MTDF_TYPE_LINK)
            status = make_link(fs, e.ino, path);
        else
            printf("skipped %s: mode %06o\n", path, st.mode);
#if !defined(_WIN32)
        if (status == MTDF_OK && st.type != MTDF_TYPE_LINK)
            chmod(path, (mode_t)(st.mode & 01777));
#endif
        if (status != MTDF_OK) {
            fprintf(stderr, "mtdf-extract: %s: %s %s\n", path, mtdf_strerror(status), mtdf_detail(fs));
            return status;
        }
        path[len] = '\0';
    }
    return status == MTDF_END ? MTDF_OK : status;
}

int main(int argc, char **argv)
{
    static char path[2 * MTDF_PATH_MAX + 2];
    mtdf_fs_t *fs;
    long long size;
    FILE *f;
    int status;

    if (argc != 3 || strlen(argv[2]) > MTDF_PATH_MAX) {
        fprintf(stderr, "usage: mtdf-extract <image> <new directory>\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (f == NULL || fseek64(f, 0, SEEK_END) != 0 || (size = (long long)ftell64(f)) < 0) {
        fprintf(stderr, "mtdf-extract: cannot open %s\n", argv[1]);
        return 1;
    }
    status = mtdf_open_cb(read_file, f, (uint64_t)size, NULL, &fs);
    if (status == MTDF_OK && make_dir(argv[2], 0755) != 0) {
        fprintf(stderr, "mtdf-extract: %s exists or cannot be made\n", argv[2]);
        mtdf_close(fs);
        fclose(f);
        return 1;
    }
    if (status == MTDF_OK) {
        strcpy(path, argv[2]);
        status = extract(fs, MTDF_ROOT, path, strlen(path));
        mtdf_close(fs);
    } else
        fprintf(stderr, "mtdf-extract: %s: %s\n", argv[1], mtdf_strerror(status));
    fclose(f);
    return status == MTDF_OK ? 0 : 1;
}
