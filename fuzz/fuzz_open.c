#include <flashfs/flashfs.h>

#include <stdint.h>
#include <string.h>

#ifndef FUZZ_FORMAT
#error "build with -DFUZZ_FORMAT=MTDF_FORMAT_JFFS2, _CRAMFS or _UBI"
#endif

static unsigned char scratch[1 << 16];

static void walk(mtdf_fs_t *fs, mtdf_ino_t dir, unsigned depth)
{
    char target[MTDF_PATH_MAX + 1];
    mtdf_dirent_t e;
    mtdf_stat_t st;
    mtdf_ino_t ino;
    uint64_t cookie = 0;
    uint64_t off;
    size_t got;

    while (mtdf_readdir(fs, dir, &cookie, &e) == MTDF_OK) {
        if (mtdf_stat(fs, e.ino, &st) != MTDF_OK)
            return;
        if (st.type == MTDF_TYPE_DIR && depth < 64)
            walk(fs, e.ino, depth + 1);
        if (st.type == MTDF_TYPE_LINK && mtdf_readlink(fs, e.ino, target, sizeof(target), &got) == MTDF_OK)
            (void)mtdf_lookup(fs, target, MTDF_LOOKUP_FOLLOW, &ino);
        for (off = 0; st.type == MTDF_TYPE_FILE && off < st.size; off += got)
            if (mtdf_read(fs, e.ino, off, scratch, sizeof(scratch), &got) != MTDF_OK || got == 0)
                break;
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    mtdf_options_t opts;
    mtdf_fs_t *fs;

    memset(&opts, 0, sizeof(opts));
    opts.format = FUZZ_FORMAT;
    opts.max_total_bytes = 64u << 20;
    if (mtdf_open_mem(data, size, &opts, &fs) != MTDF_OK)
        return 0;
    walk(fs, MTDF_ROOT, 0);
    mtdf_close(fs);
    return 0;
}
