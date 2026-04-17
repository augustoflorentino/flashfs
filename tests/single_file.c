#define MTDF_IMPLEMENTATION
#include "flashfs/flashfs.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned char image[1 << 20];

static int walk(mtdf_fs_t *fs, mtdf_ino_t dir, unsigned long *entries)
{
    static unsigned char scratch[4096];
    mtdf_dirent_t e;
    mtdf_stat_t st;
    uint64_t cookie = 0;
    uint64_t off;
    size_t got;
    int status;

    while ((status = mtdf_readdir(fs, dir, &cookie, &e)) == MTDF_OK) {
        ++*entries;
        if ((status = mtdf_stat(fs, e.ino, &st)) != MTDF_OK)
            return status;
        if (st.type == MTDF_TYPE_DIR && (status = walk(fs, e.ino, entries)) != MTDF_OK)
            return status;
        for (off = 0; st.type == MTDF_TYPE_FILE && off < st.size; off += got)
            if ((status = mtdf_read(fs, e.ino, off, scratch, sizeof(scratch), &got)) != MTDF_OK || got == 0)
                return status != MTDF_OK ? status : MTDF_ERR_FORMAT;
    }
    return status == MTDF_END ? MTDF_OK : status;
}

int main(int argc, char **argv)
{
    unsigned long entries = 0;
    mtdf_fs_t *fs;
    size_t len;
    FILE *f;
    int status;

    if (argc != 2 || (f = fopen(argv[1], "rb")) == NULL) {
        fprintf(stderr, "usage: single_file <image>\n");
        return 2;
    }
    len = fread(image, 1, sizeof(image), f);
    fclose(f);
    status = mtdf_open_mem(image, len, NULL, &fs);
    if (status == MTDF_OK) {
        status = walk(fs, MTDF_ROOT, &entries);
        mtdf_close(fs);
    }
    printf("%s: %s, %lu entries\n", argv[1], mtdf_strerror(status), entries);
    return status == MTDF_OK && entries > 0 ? 0 : 1;
}
